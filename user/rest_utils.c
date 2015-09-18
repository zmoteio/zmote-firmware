#include <esp8266.h>
#include "rest_utils.h"
#include "jsmn.h"

static void ICACHE_FLASH_ATTR sendHeaders(HttpdConnData *connData, int status, const char *ctype)
{
	if (!connData->conn)
		return; // MQTT response using faked connData object
	
	httpdStartResponse(connData, status);
	httpdHeader(connData, "Access-Control-Allow-Origin",  connData->origin?connData->origin:"*");
	httpdHeader(connData, "Access-Control-Allow-Credentials",  "true");
	httpdHeader(connData, "Access-Control-Allow-Methods", "PUT, POST, GET, OPTIONS");
	httpdHeader(connData, "Access-Control-Allow-Headers", "Origin, X-Requested-With, Content-Type, Accept");
	httpdHeader(connData, "Content-Type", ctype);
	httpdEndHeaders(connData);
}
int ICACHE_FLASH_ATTR sendOK(HttpdConnData *connData, const char *t)
{
	//if (!os_strcmp(t, "OK")) {
		sendHeaders(connData, 200, "application/json");
	//} else {
	//	sendHeaders(connData, 400, "application/json");
	//}
	HTTPD_PRINTF("{\"status\":\"%s\"}", t);
	return 1;

}
int ICACHE_FLASH_ATTR sendJSON(HttpdConnData *connData)
{
	sendHeaders(connData, 200, "application/json");
	return 1;
}
int ICACHE_FLASH_ATTR jsonEq(const char *json, jsmntok_t *tok, const char *s) 
{
	if (tok->type == JSMN_STRING && (int) strlen(s) == tok->end - tok->start &&
			strncmp(json + tok->start, s, tok->end - tok->start) == 0) {
		return 1;
	}
	return 0;
}
int ICACHE_FLASH_ATTR jsonNum(const char *json, jsmntok_t *tok)
{
	return atoi(json + tok->start);
}
char  *ICACHE_FLASH_ATTR jsonStr(const char *json, jsmntok_t *tok, char *s)
{
	strncpy(s, json + tok->start, tok->end - tok->start);
	s[tok->end - tok->start] = '\0';
	return s;
}
char ICACHE_FLASH_ATTR *jsonStr_p(char *json, jsmntok_t *tok)
{
	json[tok->end] = '\0';
	return &json[tok->start];
}
int ICACHE_FLASH_ATTR jsonSkip(jsmntok_t *t)
{
	int i, j;
	if (t->type == JSMN_STRING || t->type == JSMN_PRIMITIVE)
		return 1;
	for (j = 0, i = 1; j < t->size; j++, i += jsonSkip(t+i))
		;
	return i;
}
uint32 ICACHE_FLASH_ATTR toHex(const char *p)
{
	int ret = 0;
	if (!os_strncmp(p, "0x", 2) || !os_strncmp(p, "0X", 2))
		p += 2;
	while (*p) {
		if (*p >= '0' && *p <= '9')
			ret = (ret << 4) + (*p - '0');
		else if  (*p >= 'a' && *p <= 'f')
			ret = (ret << 4) + (*p - 'a') + 10;
		else if  (*p >= 'A' && *p <= 'F')
			ret = (ret << 4) + (*p - 'A') + 10;
		else
			return ret;
		++p;
	}
	return ret;
}
