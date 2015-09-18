// gcc -m32 -DRPS_TEST -Wall -I. rps.c  && ./a.exe
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define sint16 short int
#define uint16 unsigned short
#define uint8 unsigned char
#define uint32 unsigned int
#define ICACHE_FLASH_ATTR
#define os_memset memset
#define os_malloc malloc
#define os_free free

#ifdef RPS_DEBUG
#define DEBUG(fmt, args...) printf("DEBUG[%s,%d]: " fmt "\r\n", __FILE__, __LINE__, ##args)
#define INFO(fmt, args...) printf("INFO[%s,%d]: " fmt "\r\n", __FILE__, __LINE__, ##args)
#define WARN(fmt, args...) printf("WARN[%s,%d]: " fmt "\r\n", __FILE__, __LINE__, ##args)
#define ERROR(fmt, args...) printf("ERROR[%s,%d]: " fmt "\r\n", __FILE__, __LINE__, ##args)
#else
#define DEBUG(fmt, args...) ((void)0)
#define INFO(fmt, args...) ((void)0)
#define WARN(fmt, args...) ((void)0)
#define ERROR(fmt, args...) printf("ERROR[%s,%d]: " fmt "\r\n", __FILE__, __LINE__, ##args)
#endif

enum{
SPI_FLASH_RESULT_OK,
SPI_FLASH_RESULT_ERR,
SPI_FLASH_RESULT_TIMEOUT
};

typedef enum {false, true} bool;

#define FLASH_MAP(c)  ((uint32 *)(c))

#include "rps.h"
void ICACHE_FLASH_ATTR cfgInit(void);

uint8 sec[2*RPS_SECTOR_SIZE], *head;
void dump2(void *p, void *q);

int main(void)
{
	uint8 t[RPS_SECTOR_SIZE] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
	uint8 c[16];
	uint32 flags, *ptr;
	int i;

	head = sec;
	for (i = 0; i < 4; i++) {
		rps_write(t, (uint32)sec, 16);
		rps_read(c, (uint32)sec, 0, 16);
		if (memcmp(c, t, 16)) {
			ERROR("mismatch");
			exit(2);
		}
		t[0]++;
		t[15]++;
	}
	rps_write(t, (uint32)sec, 16);
	for (i = 0; i < 4; i++) {
		rps_set_flags(1 << i, (uint32)sec);
		rps_get_flags(&flags, (uint32)sec);
		if (flags != (1<<i)) {
			ERROR("mismatch %d: %x != %x", i, flags, 1<<i);
			exit(2);
		}
		rps_read(c, (uint32)sec, 0, 16);
		if (memcmp(c, t, 16)) {
			ERROR("mismatch");
			exit(2);
		}
	}
	for (i = 0; i < 4; i++) {
		rps_write(t, (uint32)sec, 16);
		ptr = rps_read_ptr((uint32)sec);
		if (memcmp(ptr, t, 16)) {
			ERROR("mismatch");
			exit(2);
		}
		t[0]++;
		t[15]++;
	}

	void *p = malloc(2*4096), *q = malloc(4096);
	FILE *fp = fopen("../test/config.bin", "rb");
	fread(p, 4096, 2, fp);
	memcpy(q, p, 4096);
	fclose(fp);
	head = p;
	ptr = rps_read_ptr((uint32)p);
	if (memcmp(ptr, q, 16)) {
		ERROR("mismatch");
		exit(2);
	}
	memset(p, 0xff, 1); // CUrrupting one section will have no effect
	ptr = rps_read_ptr((uint32)p);
	if (memcmp(ptr, q, 16)) {
		ERROR("mismatch");
		exit(2);
	}
	memset((uint8*)p+4096, 0xff, 1); // Currupting both sections will
	ptr = rps_read_ptr((uint32)p);
	if (!memcmp(ptr, q, 16)) {
		ERROR("mismatch");
		exit(2);
	}
	printf("pass\n");

	return 0;
}
void dump2(void *p, void *q)
{
	int i;
	for (i = 0; i< 16; i++) {
		printf("%d:%d ", ((uint8*)p)[i], ((uint8*)q)[i]);
	}
	printf("\n");
}
void dump(void) 
{
#ifdef RPS_DEBUG
	int i, j;
	for (i = 0; i < 2; i++) {
		printf("%s:", i?"second":"first");
		for (j = 0; j < 16; j++) {
			printf("%2d ", sec[i*RPS_SECTOR_SIZE + j]);
		}
		printf("\n");
		rps_footer *f = &(((rps_sector *)(sec + i*RPS_SECTOR_SIZE))->footer);
		printf("    seq=%5d flags=%08x crc=%08x\n", f->seq, f->flags, f->crc);
	}
#endif
}
int spi_flash_erase_sector(uint16 sno)
{
	uint8 *p = head;
	DEBUG("Erasing %x %x", sno, (((uint32)sec)>>12));
	if (sno != (((uint32)p)>>12))
		p += RPS_SECTOR_SIZE;
	memset(p, 0xff, RPS_SECTOR_SIZE);
	dump();
	return SPI_FLASH_RESULT_OK;
}
int spi_flash_write(uint32 dst, uint32 *src, int n)
{
	DEBUG("Writing from %8p to %8x %d bytes", src, dst, n);
	memcpy((void *)dst, src, n);
	dump();
	return SPI_FLASH_RESULT_OK;
}
int spi_flash_read(uint32 src, uint32 *dst, int n)
{
	DEBUG("Reading from %8x to %8p %d bytes", src, dst, n);
	memcpy(dst, (void *)src, n);
	dump();
	return SPI_FLASH_RESULT_OK;
}
#if 0
// Read in chunks of 256 bytes
#define CHUNK 256 

static int len = 0, ntok = 0;

#define DYNAMIC_READ_SZ
#ifdef DYNAMIC_READ_SZ
static char *cfg = NULL;
static 	jsmntok_t *cfgToks = NULL; 
#else
static char cfg[1024];
static 	jsmntok_t cfgToks[32];
#endif
static int ICACHE_FLASH_ATTR skipOver(jsmntok_t *t)
{
	int i, j;
	if (t->type == JSMN_STRING || t->type == JSMN_PRIMITIVE)
		return 1;
	for (j = 0, i = 1; j < t->size; j++, i += skipOver(t+i))
		;
	return i;
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
#ifdef DYNAMIC_READ_SZ
static int ICACHE_FLASH_ATTR cfgReadChunk()
{
	int n;
	if (cfg)
		os_free(cfg);
	len += CHUNK;
	if (!(cfg = os_malloc(len))) {
		ERROR("Out of memory %d", len);
		return -1;
	}
	INFO("reading %d", len);
	//if (!system_param_load(ZMOTE_CFG_SECTOR, 0, cfg, len)) {
	if (rps_read(cfg, ZMOTE_CFG_SECTOR << 12, 0, len)) {
		ERROR("Read failure");
		return -1;
	}
	n = flash_strnlen(cfg + len - CHUNK, CHUNK);
	INFO("reading done %d", n);
	if (n < CHUNK || len == SPI_FLASH_SEC_SIZE) {
		len -= CHUNK - n;
		return 0;
	}
	return 1;
}
#else
static int ICACHE_FLASH_ATTR cfgReadChunk()
{
	//if (!system_param_load(ZMOTE_CFG_SECTOR, 0, cfg, sizeof(cfg))) {
	if (rps_read(cfg, ZMOTE_CFG_SECTOR << 12, 0, sizeof(cfg)) {
		ERROR("Read failure");
		return -1;
	}
	len = flash_strnlen(cfg, sizeof(cfg));
	return 0;
}
#endif
void ICACHE_FLASH_ATTR cfgInit(void)
{
	int ret;
	jsmn_parser p;
	len = ntok = 0;
#ifdef DYNAMIC_READ_SZ
	if (cfgToks)
		os_free(cfgToks);
#endif
	INFO("cfgInit");
	while ((ret = cfgReadChunk()) == 1)
		;
	INFO("cfgInit len=%d", len);
	if (ret < 0)
		goto err;
	if (len == 0) {
		strcpy(cfg, "{}");
		len = strlen(cfg);
		//if (!system_param_save_with_protect(ZMOTE_CFG_SECTOR, cfg, len)) {
		uint8 cfgFull[RPS_ALLOC];
		INFO("copy %d to full", len);
		os_memcpy(cfgFull, cfg, len);
		INFO("writing %d ", len);
		if (rps_write(cfgFull, ZMOTE_CFG_SECTOR<<12, len)) {
			ERROR("write failure.  unrecoverable.  send it back to the factory or whatever");
			return;
		}
		INFO("writing %d DONE", len);
	}
	jsmn_init(&p);
	ntok = jsmn_parse(&p, cfg, len, NULL, 0);
	if (ntok <= 0) {
		ERROR("cfg json error %d", ntok);
		goto err;
	}
	#ifdef DYNAMIC_READ_SZ
	if (!(cfgToks = os_malloc(ntok*sizeof(cfgToks[0])))) {
		ERROR("out of mem");
		goto err;
	}
	#else
	if (ntok > sizeof(cfgToks)/sizeof(cfgToks[0])) {
		ERROR("Need more tokens %d", ntok);
		goto err;
	}
	#endif
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
	#ifdef DYNAMIC_READ_SZ
	if (cfg)
		os_free(cfg);
	if (cfgToks)
		os_free(cfgToks);
	#endif
	len = -1;
	ntok = -1;
	return;
}
#endif