#include <esp8266.h>
#include "rest_utils.h"
#include "stled.h"
#include "jsmn.h"
#include "console.h"
#include "rps.h"

#ifndef ZMOTE_CFG_SECTOR
//#	define ZMOTE_CFG_SECTOR 0x7F
#		error "ZMOTE_CFG_SECTOR must be supplied"
#endif
#define ZMOTE_CFG_ADDR (ZMOTE_CFG_SECTOR<<12)
static int len = 0, ntok = 0;

static char *cfg = NULL;
static 	jsmntok_t *cfgToks = NULL; 

static int ICACHE_FLASH_ATTR cfg_len(uint32 *p)
{
	int i, r;
	for (i = 0; i < (RPS_MAX_DATA_LEN >> 2); i++) {
		r = p[i];
		if (r == 0xffffffffu)
			return (i<<2);
		if ((r&0xFFu) == 0)
			return (i<<2);
		if (((r>>8)&0xFFu) == 0)
			return (i<<2) + 1;
		if (((r>>16)&0xFFu) == 0)
			return (i<<2) + 2;
		if (((r>>24)&0xFFu) == 0)
			return (i<<2) + 3;
	}
	return RPS_MAX_DATA_LEN;
}
static int ICACHE_FLASH_ATTR flash_strnlen(const char *p, int n)
{
	int i, cnt = 0;
	for (i = 0; i < n; i++) {
		if (!p[i])
			break;
		if (((uint8*)p)[i] ==  0xff)
			++cnt;
		else
			cnt = 0;
		if (cnt == 4)
			return i-3;

	}
	return i;
}

void ICACHE_FLASH_ATTR cfgInit(void)
{
	jsmn_parser p;
	uint32 *fcfg = rps_read_ptr(ZMOTE_CFG_ADDR);

	INFO("cfgInit");
	if (cfgToks)
		os_free(cfgToks);
	if (cfg)
		os_free(cfg);
	cfgToks =  NULL;
	cfg = NULL;
	len = cfg_len(fcfg);
	ntok = 0;
	INFO("cfgInit len=%d", len);
	if (!(cfg = os_malloc(len+3)))
		goto err;
	if (rps_read(cfg, ZMOTE_CFG_ADDR, 0, len+1))
		goto err;
	if (len <= 0) {
		ERROR("Bad cfg");
		goto err;
	}
	jsmn_init(&p);
	ntok = jsmn_parse(&p, cfg, len, NULL, 0);
	if (ntok <= 0) {
		ERROR("cfg json error %d", ntok);
		goto err;
	}
	if (!(cfgToks = os_malloc(ntok*sizeof(cfgToks[0])))) {
		ERROR("out of mem");
		goto err;
	}
	jsmn_init(&p);
	int t;
	if ((t = jsmn_parse(&p, cfg, len, cfgToks, ntok)) != ntok) {
		ERROR("wtf? %d != %d", t, ntok);
		goto err;
	}
	if (cfgToks[0].type != JSMN_OBJECT)
		goto err;
	return;
err:
	ERROR("Parse error");
	if (cfg)
		os_free(cfg);
	if (cfgToks)
		os_free(cfgToks);
	len = -1;
	ntok = -1;
	return;
}
char *ICACHE_FLASH_ATTR cfgGet(const char *key, char *val, int n)
{
	int i;
	//char temp[32];
	//DEBUG("cfgGet(%s) ntok=%d", key, ntok);
	if (ntok < 0) 
		return NULL;
	for (i = 1; i < ntok; i += 1 + jsonSkip(cfgToks+i+1)) {
		//DEBUG("[%d/%d] cfgGet %s==%s?", i, ntok, key, jsonStr(cfg, &cfgToks[i], temp));
		if (jsonEq(cfg, &cfgToks[i], key) && i + 1 < ntok) {
			jsonStr(cfg, &cfgToks[i+1], val); // FIXME string length is ignored
			return val;
		}
	}
	return NULL;
}
void ICACHE_FLASH_ATTR cfgSet(const char *key, const char *val)
{
	//char buf[4*1024];
	char *buf = NULL;
	char *quot = (val[0] == '[' || val[0] == '{')?"":"\"";
	int i, len = 1, found = 0;

	if (ntok < 0)
		return;
	if (!(buf = os_malloc(SPI_FLASH_SEC_SIZE)))
		goto err;
	INFO("cfgSet: %s=%s", key, val);
	strcpy(buf, "{");
	for (i = 1; i < ntok && len < sizeof(buf) - 32; i += 1 + jsonSkip(cfgToks+i+1)) {
		if (jsonEq(cfg, &cfgToks[i], key)) {
			len += os_sprintf(buf + len, "%s\"%s\":%s%s%s", (i>1)?",":"", key, quot, val, quot);
			found = 1;
		} else if (cfgToks[i+1].type == JSMN_STRING || cfgToks[i+1].type == JSMN_PRIMITIVE) {
			len += os_sprintf(buf + len, "%s\"", (i>1)?",":"");
			strncpy(buf + len, cfg + cfgToks[i].start, cfgToks[i].end - cfgToks[i].start);
			len += cfgToks[i].end - cfgToks[i].start;
			strcpy(buf+len, "\":\"");
			len += 3;
			strncpy(buf + len, cfg + cfgToks[i+1].start, cfgToks[i+1].end - cfgToks[i+1].start);
			len += cfgToks[i+1].end - cfgToks[i+1].start;
			buf[len++] = '"';		
		} else /*if (cfgToks[i+1].type == JSMN_ARRAY)*/ {
			len += os_sprintf(buf + len, "%s\"", (i>1)?",":"");
			strncpy(buf + len, cfg + cfgToks[i].start, cfgToks[i].end - cfgToks[i].start);
			len += cfgToks[i].end - cfgToks[i].start;
			strcpy(buf+len, "\":");
			len += 2;
			strncpy(buf + len, cfg + cfgToks[i+1].start, cfgToks[i+1].end - cfgToks[i+1].start);
			len += cfgToks[i+1].end - cfgToks[i+1].start;
		}
	}
	if (!found) {
		len += os_sprintf(buf + len, "%s\"%s\":%s%s%s", 
			(i>1)?",":"", key, quot, val, quot);
	}
	buf[len++] = '}';
	buf[len++] = 0;
	DEBUG("%s", buf);
	//os_memset(buf+len, 0, sizeof(buf)-len);
	//if (!system_param_save_with_protect(ZMOTE_CFG_SECTOR, buf, sizeof(buf))) {
	if (rps_write(buf, ZMOTE_CFG_ADDR, len)) {
		ERROR("write failure.  unrecoverable.  send it back to the factory or whatever");
		goto err;
	}
	os_free(buf);
	cfgInit(); //  Re-initialize
	return;
err:
	ERROR("memory or other error");
	if (buf)
		os_free(buf);
}


#define MAX_SEND_CHUNK 1024
typedef struct cfgFileState_ {
	uint16 fileIdx;    // File entry (not really needed except for debug)
	uint16 tabIdx;     // Current entry into fsTab
	uint16 lastEntry;  // Last fsTab entry
	uint16 sec;        // current section number with the fsTab entry
	uint16 lastSec;	   // Last section in the current fsTab entry
	uint16 offset;     // Offset withing current section
	bool done;
	uint32 length;     // Total length
	uint32 accLen;     // Total sent so far
	uint32 data[];	   // Usually MAX_SEND_CHUNK is allocated
} cfgFileState;

int ICACHE_FLASH_ATTR cfgFile(HttpdConnData *connData)
{
	cfgFileState *fState = connData->cgiData;
	char file[64], hdrKey[64], hdrVal[64];
	int i, j;

	if (connData->url==NULL) {
		//Connection aborted. Clean up.
		return HTTPD_CGI_DONE;
	}
	if (!fState) { // FIrst call
		os_sprintf(file, "file:/%s", connData->url);
		INFO("Request: %s", file);
		if (ntok < 0) {
			// Send 404
			return HTTPD_CGI_NOTFOUND;
		}
		for (i = 1; i < ntok; i += 1 + jsonSkip(cfgToks+i+1)) {
			//DEBUG("[%d/%d] cfgGet %s==%s?", i, ntok, key, jsonStr(cfg, &cfgToks[i], temp));
			if (!jsonEq(cfg, &cfgToks[i], file))
				continue;
			// Found file!
			INFO("Found %s", file);
			if (!(fState = connData->cgiData = os_zalloc(sizeof(*fState) + MAX_SEND_CHUNK))) {
				ERROR("out of mem");
				return HTTPD_CGI_NOTFOUND; // FIXME need a better code
			}
			httpdStartResponse(connData, 200);
			for (j = 0; j < 3*cfgToks[i+1].size; j+=3) {
				if (cfgToks[i+j+3].type == JSMN_PRIMITIVE) { // end of headers
					INFO("Headers complete");
					break;
				}
				httpdHeader(connData, jsonStr(cfg, &cfgToks[i+j+3], hdrKey), 
					jsonStr(cfg, &cfgToks[i+j+4], hdrVal));
				if (!os_strcmp(hdrKey, "Content-Length"))
					fState->length = atoi(hdrVal);
			}
			httpdEndHeaders(connData);
			fState->fileIdx = i;
			fState->tabIdx = i + j + 2;
			fState->lastEntry = fState->tabIdx + 3*cfgToks[i+1].size;
			fState->sec = jsonNum(cfg, &cfgToks[fState->tabIdx + 1]);
			fState->lastSec = fState->sec + jsonNum(cfg, &cfgToks[fState->tabIdx + 2]);
			INFO("Streaming contents: fileIdx=%d len=%d tabIdx=%d, lastEntry=%d sec=%d lastSec=%d",
				fState->fileIdx, fState->length, fState->tabIdx, fState->lastEntry, fState->sec, fState->lastSec);
			return HTTPD_CGI_MORE;
		}
		return HTTPD_CGI_NOTFOUND;
	}
	if (fState->done)
		goto err; 
	int tLen = SPI_FLASH_SEC_SIZE - fState->offset;
	if (tLen > MAX_SEND_CHUNK)
		tLen = MAX_SEND_CHUNK;
	if (fState->accLen + tLen > fState->length)
		tLen = fState->length - fState->accLen;
	if (tLen&3)
		tLen += 4 - (tLen&3); // Read only a multiple of 4 bytes
	DEBUG("fs_flash_read (%x, %d)", fState->sec*SPI_FLASH_SEC_SIZE + fState->offset, tLen);
	if (spi_flash_read(fState->sec*SPI_FLASH_SEC_SIZE + fState->offset, fState->data, tLen) != SPI_FLASH_RESULT_OK) {
		ERROR("Flash read error");
		goto err;
	}
	espconn_sent(connData->conn, (void *)fState->data, tLen);
	fState->accLen += tLen;
	fState->offset += tLen;
	if (fState->offset >= SPI_FLASH_SEC_SIZE) {
		++fState->sec;
		fState->offset = 0;
	}
	if (fState->sec >= fState->lastSec) {
		fState->tabIdx += 3;
		if (fState->tabIdx < fState->lastEntry) {
			fState->sec = jsonNum(cfg, &cfgToks[fState->tabIdx + 1]);
			fState->lastSec = fState->sec + jsonNum(cfg, &cfgToks[fState->tabIdx + 2]);
		}
	}
	if (fState->accLen >= fState->length)
		fState->done = true;
	return HTTPD_CGI_MORE;
err:
	os_free(fState);
	connData->cgiData = NULL;
	return HTTPD_CGI_DONE;

}
typedef struct cfgSendState_ {
	uint32 offset;
	int done;
	uint32 buf[256];
} cfgSendState;
static int ICACHE_FLASH_ATTR sendCfg(HttpdConnData *connData, int page)
{
	uint32 nb = 0;
	cfgSendState *st = (cfgSendState *)(connData->cgiData);
	if (!st) {
		if (!(connData->cgiData = st = os_malloc(sizeof(cfgSendState)))) {
			ERROR("malloc error");
			return HTTPD_CGI_DONE;
		}
		st->offset = 0;
		st->done = 0;
		sendJSON(connData);
		return HTTPD_CGI_MORE;
	}
	if (st->done) {
		INFO("clean up sending page=%x", page);
		os_free(st);
		connData->cgiData = NULL;
		return HTTPD_CGI_DONE;
	}
	DEBUG("load %d from %x", st->offset, page);
	if (page == ZMOTE_CFG_SECTOR) {
		//system_param_load(page, st->offset, st->buf, CHUNK);
		rps_read(st->buf, ZMOTE_CFG_ADDR, st->offset, sizeof(st->buf));
		if ((nb = flash_strnlen((char *)st->buf, sizeof(st->buf))) < 0)
			goto err;
		if (nb == 0 && st->offset) {
			st->buf[0] = 0;
			nb = 1;
		}
	} else {
		if (spi_flash_read(page*SPI_FLASH_SEC_SIZE + st->offset, st->buf, sizeof(st->buf)) != SPI_FLASH_RESULT_OK)
			goto err;
		nb = sizeof(st->buf);
	}
	DEBUG("chunk=%d len=%d", st->offset, nb);
	//httpdSend(connData, (void *)buf, n);
	espconn_sent(connData->conn, (void *)st->buf, nb);
	st->offset += nb;
	if (nb < sizeof(st->buf) || st->offset >= SPI_FLASH_SEC_SIZE) {
		INFO("FInished sending page=%x", page);
		st->done = 1;
	}
	return HTTPD_CGI_MORE;
err:
	HTTPD_SEND_STR("{}");
	os_free(st);
	connData->cgiData = NULL;
	return HTTPD_CGI_DONE;
}
int ICACHE_FLASH_ATTR cfgGetSet(HttpdConnData *connData)
{
	char *json, *key, value[256], *delim;
	jsmn_parser p;
	jsmntok_t *tok = NULL;
	int i, ntok;

	if (connData->url==NULL) {
		//Connection aborted. Clean up.
		return HTTPD_CGI_DONE;
	}
	if (connData->requestType == HTTPD_METHOD_OPTIONS) {
		sendJSON(connData);
		HTTPD_SEND_STR("{\"status\":\"ok\"}\r\n\r\n");
		return HTTPD_CGI_DONE;
	}
	key = os_strstr(connData->url, "/config/") + os_strlen("/config/");
	if (connData->requestType == HTTPD_METHOD_GET) {
		if (!cfgGet(key, value, sizeof(value)))
			os_strcpy(value, "Not found");
	} else {
		// PUT or POST: key has to be sent as part of the payload
		//INFO("No implemented");
		//os_strcpy(value, "Not found");
		json = connData->post->buff;
		jsmn_init(&p);
		ntok = jsmn_parse(&p, json, connData->post->len, NULL, 0);
		if (ntok <= 0)
			goto err;
		if (!(tok = os_malloc(ntok*sizeof(jsmntok_t))))
			goto err;
		jsmn_init(&p);
		if (ntok != jsmn_parse(&p, json, connData->post->len, tok, ntok))
			goto err;
		if (tok[0].type != JSMN_OBJECT)
			goto err;
		for (i = 1; i < ntok; i += 1 + jsonSkip(tok+i+1)) {
			key = jsonStr_p(json, &tok[i]);
			jsonStr(json, &tok[i+1], value);
			cfgSet(key, value);
		}
		os_free(tok);
	}
	if (value[0] == '[')
		delim = "";
	else
		delim = "\"";
	HTTPD_PRINTF("{\"%s\":%s%s%s}\r\n", key, delim, value, delim);
	return HTTPD_CGI_DONE;
err:
	ERROR("Unknown error");
	if (tok)
		os_free(tok);
	HTTPD_PRINTF("{\"status\":\"error\"}\r\n");
	return HTTPD_CGI_DONE;

}

int ICACHE_FLASH_ATTR cfgOps(HttpdConnData *connData)
{
	char *r;
	int page = 0;
	if (connData->url==NULL) {
		//Connection aborted. Clean up.
		return HTTPD_CGI_DONE;
	}
	if (connData->requestType == HTTPD_METHOD_OPTIONS) {
		sendJSON(connData);
		HTTPD_SEND_STR("{\"status\":\"ok\"}\r\n\r\n");
		return HTTPD_CGI_DONE;
	}
	r = connData->url;
	while (strchr(r, '/'))
		r = strchr(r, '/') + 1; // strrchr not available
	DEBUG("r=%s", r);
	if (r)
		page = toHex(r);
	if (page == 0)
		page = ZMOTE_CFG_SECTOR;
	if (page > ZMOTE_CFG_SECTOR) {
		sendOK(connData, "Bad address");
		return HTTPD_CGI_DONE;
	}
	DEBUG("page=%d, %x", page, page);
	if (connData->requestType == HTTPD_METHOD_GET) {
		return sendCfg(connData, page);
	} else /* put or post */ {	
		DEBUG("received %d/%d bytes strlen=%d", connData->post->buffLen, connData->post->len, strlen(connData->post->buff));
		if (page == ZMOTE_CFG_SECTOR) {
			connData->post->buff[connData->post->len++] = '\0';
			DEBUG("writing \"%s\" %d btes", connData->post->buff, connData->post->len);
			if (!system_param_save_with_protect(page, connData->post->buff, connData->post->len)) {
				ERROR("write failure.  unrecoverable.  send it back to the factory or whatever");
			}
			DEBUG("writing %d bytes done", connData->post->len);
			cfgInit();
		} else {
			spi_flash_write(page*SPI_FLASH_SEC_SIZE, (uint32 *)connData->post->buff, connData->post->len); // FIXME error check
		}
		sendOK(connData, "OK");
		return HTTPD_CGI_DONE;
	}
	return HTTPD_CGI_DONE;
}
