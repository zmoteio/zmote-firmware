#include <esp8266.h>
#include "mqttclient.h"
#include "console.h"
#include "mutex.h"
#include "updatefs.h"
#include "rest_utils.h"
#include "zmote_config.h"

#define UPDATEFS_MAX_FILES (64)
#define UPDATEFS_TIMEOUT (20000)
#define UPDATEFS_MAX_RETRY (16)
#define UPDATEFS_RETRY_WAIT (5000)

typedef struct updatefs_state_ {
	char *fs_version;
	uint8 ip[6];
	int port;
	char *fstab[UPDATEFS_MAX_FILES];
	char **blobs;
	int retry;
	int cur_blob;
	int acc_len;
	int total_len;

	ETSTimer toTimer;
	struct espconn *conn;
	char request[256];
	updatefs_finished_cb cb;
	void *cb_arg;
} updatefs_state;
static mutex_t inprogress = 1; // DONTFIXME: To avoid having to call CreateMutux -- breaks opacity, but worth it
static updatefs_state *state = NULL;

static void ICACHE_FLASH_ATTR dumpUpdatefs(updatefs_state *s)
{
	int i;

	DEBUG("UPDATEFS:");
	DEBUG("\tfs_version=%s", s->fs_version);
	DEBUG("\tip=" IPSTR, IP2STR(s->ip));
	DEBUG("\tport=%d", s->port);
	for (i = 0; s->fstab[i]; i+= 2)
		DEBUG("\t\t%s:%s", s->fstab[i], s->fstab[i+1]);
	for (i = 0; s->blobs && s->blobs[i]; i += 2)
		DEBUG("\t\tBLOB[%d]={%s,%s}", i>>1, s->blobs[i], s->blobs[i+1]);
}

static void ICACHE_FLASH_ATTR parseIP(updatefs_state *s, char *ipStr)
{
	int i;
	char *p = ipStr;
	for (i = 0; i < 4; i++) {
		s->ip[i] = atoi(p);
		if (!(p = strchr(p, '.')))
			return;
		++p; // skip the '.'
	}
}
static void ICACHE_FLASH_ATTR parseBlobs(updatefs_state *s, char *json, jsmntok_t *t)
{
	int i, j;

	if (t[0].type != JSMN_ARRAY)
		return;
	DEBUG("Allocating for %d blobs", t[0].size);
	if (!(s->blobs = os_zalloc(2*(t[0].size+1)*sizeof(char *))))
		return;
	for (i = 0, j = 1; i < 2*t[0].size; i += 2, j += 3) {
		s->blobs[i] = jsonStr_p(json, t+j+1);
		s->blobs[i+1] = jsonStr_p(json, t+j+2);
	}
}
static void ICACHE_FLASH_ATTR deinitUpdate(updatefs_state *s) ;
static void ICACHE_FLASH_ATTR timeout_cb(void *) ;
static void ICACHE_FLASH_ATTR wget(updatefs_state *s);

static int ICACHE_FLASH_ATTR write_flash(updatefs_state *s, void *p, int len)
{
	uint32 base1, base2 = 0, cur, *q = NULL;
	int len1, olen1, len2 = 0;

	if (len > SPI_FLASH_SEC_SIZE) {
		ERROR("Can't handle more than 4K.  Where did the SDK find the memory?");
		return UPDATEFS_MEM_ERROR;
	}
	cur = atoi(s->blobs[s->cur_blob]) + s->acc_len;
	base1 = (cur>>12)<<12;
	olen1 = cur&0xfffu;
	len1 = SPI_FLASH_SEC_SIZE - olen1;
	if (len1 > len)
		len1 = len;
	else if (len > len1) {
		base2 = ((cur>>12)+1)<<12;
		len2 = len - len1;
	} else {
		len2 = 0;
	}
	INFO("len=%d cur=%x base1=%x,len1=%d base2=%x,len2=%d", len, cur, base1, len1, base2, len2);
	if (!(q = os_malloc(SPI_FLASH_SEC_SIZE)))
		goto err;
	if (cur == base1) {
		// Fresh sector: erase and write
		if (spi_flash_erase_sector(base1>>12))
			goto err;
		memcpy(q, p, len1);
		if (spi_flash_write(cur, q, (len1+3u)&(~3u)))
			goto err;
	} else {
		// Half finished sector.  Already erased before last write.  
		// But need to worry about alignment
		uint32  rem = (olen1&3);
		if (spi_flash_read(base1 + olen1 - rem, q, 4))
			goto err;
		memcpy(((uint8*)q)+rem, p, len1);
		if (spi_flash_write(cur-rem, q, (len1+rem+3u)&(~3u)))
			goto err;		
	}
	if (len2) {
		// Overflow into next sector. Must erase first
		if (spi_flash_erase_sector(base2>>12))
			goto err;
		memcpy(q, (uint8*)p+len1, len2);
		if (spi_flash_write(base2, q, (len2+3u)&(~3u)))
			goto err;

	}
	os_free(q);
	return UPDATEFS_OK;
err:
	ERROR("Error writing to or erasing flash");
	if (q)
		os_free(q);
	return UPDATEFS_MEM_ERROR;
}
static void ICACHE_FLASH_ATTR initConn(updatefs_state *s)
{
	void *tcp = s->conn->proto.tcp;

	os_memset(s->conn, 0, sizeof(struct espconn));
	os_memset(tcp, 0, sizeof(esp_tcp));
	s->conn->proto.tcp = tcp;

	s->conn->type = ESPCONN_TCP;
	s->conn->state = ESPCONN_NONE;
	s->conn->proto.tcp->local_port = espconn_port();
	s->conn->proto.tcp->remote_port = s->port;
	s->conn->proto.tcp->remote_ip[0] = s->ip[0];
	s->conn->proto.tcp->remote_ip[1] = s->ip[1];
	s->conn->proto.tcp->remote_ip[2] = s->ip[2];
	s->conn->proto.tcp->remote_ip[3] = s->ip[3];	
}
static int ICACHE_FLASH_ATTR triggerNext(updatefs_state *s) 
{
	DEBUG("triggerNext");
	if (s->conn) {
		if (espconn_disconnect(s->conn))
			ERROR("Disconnect error");
	} else {
		if (!(s->conn = os_malloc(sizeof(struct espconn))))
			goto err;
		if (!(s->conn->proto.tcp = os_malloc(sizeof(esp_tcp))))
			goto err;
	}	
	initConn(s);
	//s->cur_blob = 0;
	os_timer_disarm(&s->toTimer);
	os_timer_setfn(&s->toTimer, (os_timer_func_t *)wget, s);
	os_timer_arm(&s->toTimer, 10, 0);		
	return UPDATEFS_OK;
err:
	ERROR("Memory error");
	deinitUpdate(s);
	return UPDATEFS_MEM_ERROR;
}
static void ICACHE_FLASH_ATTR deinitUpdate(updatefs_state *s) 
{
	DEBUG("deinitUpdate");
	if (s)
		os_timer_disarm(&s->toTimer);
	if (s && s->conn && espconn_disconnect(s->conn))
		ERROR("disconnection error");
	if (s && s->conn && s->conn->proto.tcp)
		os_free(s->conn->proto.tcp);
	if (s && s->conn)
		os_free(s->conn);
	if (s && s->blobs)
		os_free(s->blobs);
	if (s)
		os_free(s);
	s = NULL;
	ReleaseMutex(&inprogress);
}

static void ICACHE_FLASH_ATTR recv_cb(void *arg, char *pusrdata, unsigned short length) 
{	
	updatefs_state *s = state; // FIXME: is there any way at all to get rid of this?
	char *ptrData, *ptrLen;
	
	DEBUG("recv_cb %d", length);
	// first reply?
	if (s->total_len == 0) {
		// valid http response?
		if ((ptrLen = strcasestr(pusrdata, "Content-Length: "))
			&& (ptrData = os_strstr(ptrLen, "\r\n\r\n"))
			&& !os_strncmp(pusrdata + 9, "200", 3)) {
			
			// work out total download size
			ptrLen += os_strlen("Content-Length: ");
			s->total_len = atoi(ptrLen);
			INFO("content-length = %d", s->total_len);
			// end of header/start of data
			ptrData += os_strlen("\r\n\r\n");
			// length of data after header in this chunk
			length -= (ptrData - pusrdata);
			//INFO("header-size=%d data-len=%d", ptrData - pusrdata, length);
			// process current chunk
			if (length && write_flash(s, ptrData, length))
				goto err;
			// running total of download length
			s->acc_len = length;
		} else {
			ERROR("fail, not a valid http header/non-200 response/etc. %s", pusrdata);
			timeout_cb(NULL);
			return;
		}
	} else {
		// not the first chunk, process it
		if (write_flash(s, pusrdata, length))
			goto err;
		s->acc_len += length;
	}
	
	// check if we are finished
	INFO("recvcb finished: 	total=%d acc=%d", s->total_len, s->acc_len);
	if (s->total_len == s->acc_len) {
		// Advance to next file
		s->cur_blob += 2;
		triggerNext(s);
	} else if (s->conn->state != ESPCONN_READ) {
		ERROR("Bad state %d", s->conn->state);
		goto err;
	}
	return;
err: 
	ERROR("fail, but how do we get here? premature end of stream? toomuch data received? flash write error?");
	timeout_cb(NULL);
}

static void ICACHE_FLASH_ATTR discon_cb(void *arg)
{
	updatefs_state *s = state; // FIXME: is there any way at all to get rid of this?
	ERROR("TCP disconnection retry_count=%d", s->retry);
	if (s->retry >= UPDATEFS_MAX_RETRY) {
		timeout_cb(s);
		return;
	}
	++(s->retry);
	os_timer_disarm(&s->toTimer);
	os_timer_setfn(&s->toTimer, (os_timer_func_t *)triggerNext, s);
	os_timer_arm(&s->toTimer, UPDATEFS_RETRY_WAIT, 0);
}
static void ICACHE_FLASH_ATTR connect_cb(void *arg) 
{
	updatefs_state *s = state; // FIXME: is there any way at all to get rid of this?
	int req_len;

	DEBUG("connect_cb");
	os_timer_disarm(&s->toTimer);

	espconn_regist_disconcb(s->conn, discon_cb);
	espconn_regist_recvcb(s->conn, recv_cb);

	req_len = os_sprintf(s->request,
		"GET %s HTTP/1.1\r\n"
		"Host: "IPSTR"\r\n" 
        "Connection: keep-alive\r\n"
        "Cache-Control: no-cache\r\n"
        "User-Agent: zMote/" ZMOTE_FIRMWARE_VERSION "\r\n"
        "Accept: */*\r\n\r\n",
        s->blobs[s->cur_blob + 1],
        IP2STR(s->ip));
	os_timer_setfn(&s->toTimer, (os_timer_func_t *)timeout_cb, 0);
	os_timer_arm(&s->toTimer, UPDATEFS_TIMEOUT, 0);
	espconn_sent(s->conn, (uint8*)s->request, req_len);
}

// connection attempt timed out
static void ICACHE_FLASH_ATTR timeout_cb(void *p) 
{
	updatefs_state *s = state; // FIXME: is there any way at all to get rid of this?
	ERROR("Connect timeout or lost or something. %p %p", p, state);
	if (!s)
		return;
	// not connected so don't call disconnect on the connection
	// but call our own disconnect callback to do the cleanup
	s->cb(s->cb_arg, UPDATEFS_TIMEOUT);
	deinitUpdate(s);
}

static void ICACHE_FLASH_ATTR updateFSTab(updatefs_state *s)
{
	int i;
	for (i = 0; s->fstab[i]; i += 2)
		cfgSet(s->fstab[i], s->fstab[i+1]);
	cfgSet("fs_version", s->fs_version);
	mqttHello();
}

static void ICACHE_FLASH_ATTR wget(updatefs_state *s)
{
	DEBUG("wget %d", s->cur_blob);
	if (!s->blobs[s->cur_blob]) {
		INFO("Downloads finished");
		updateFSTab(s);
		s->cb(s->cb_arg, UPDATEFS_OK);
		deinitUpdate(s);
		//system_restart();
		return;
	}

	s->acc_len = s->total_len = 0;

	espconn_regist_connectcb(s->conn, connect_cb);
	espconn_regist_reconcb(s->conn, (espconn_reconnect_callback)timeout_cb);

	// try to connect
	espconn_connect(s->conn);

	// set connection timeout timer
	os_timer_disarm(&s->toTimer);
	os_timer_setfn(&s->toTimer, (os_timer_func_t *)timeout_cb, s);
	os_timer_arm(&s->toTimer, UPDATEFS_TIMEOUT, 0);
}

int ICACHE_FLASH_ATTR updatefs(char *json, jsmntok_t *toks, int ntok, 
	updatefs_finished_cb cb, void *arg)
{
	int i, fno = 0;
	char *key;
	if (ntok < 4 || toks[0].type != JSMN_OBJECT)
		return UPDATEFS_MEM_ERROR;
	if (!GetMutex(&inprogress))
		return UPDATEFS_BUSY;
	if (!(state = os_zalloc(sizeof(updatefs_state))))
		return UPDATEFS_MEM_ERROR;
	state->cb = cb;
	state->cb_arg = arg;
	for (i = 1; i < ntok; i += 1 + jsonSkip(toks+i+1)) {
		key  = jsonStr_p(json, toks+i); 
		if (!os_strcmp(key, "ip"))
			 parseIP(state, jsonStr_p(json, toks+i+1));
		else if (!os_strcmp(key, "port"))
			state->port = atoi(jsonStr_p(json, toks+i+1));
		else if (!os_strcmp(key, "fs_version"))
			state->fs_version = jsonStr_p(json, toks+i+1);
		else if (!os_strcmp(key, "blobs"))
			parseBlobs(state, json, toks+i+1);
		else if (strstr(key, "file://")) {
			if (fno > UPDATEFS_MAX_FILES-2)
				goto err;
			state->fstab[fno] = key;
			state->fstab[fno+1] = jsonStr_p(json, toks+i+1);
			fno += 2;
		} else {
			WARN("Unknown field %s.  Skipping", key);
			//dumpUpdatefs(state);
			//goto err;
		}
	}
	dumpUpdatefs(state);
	if (!state->port || !state->fs_version || !state->blobs || fno == 0)
		goto err;
	state->cur_blob = 0;
	char p[128];
	if (cfgGet("fs_version", p, sizeof(p)) && !os_strcmp(p, state->fs_version)) {
		INFO("Already up to date (v%s)", p);
		goto err;
	}
	cfgSet("fs_version", "update");
	if (cfgGet("file://update.html", p, sizeof(p)))
		cfgSet("file://index.html", p);
	return triggerNext(state);
err:
	deinitUpdate(state);
	return UPDATEFS_MEM_ERROR;
}

