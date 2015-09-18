
#ifndef REST_UTILS_H_
#define REST_UTILS_H_

#include "httpd.h"
#include "jsmn.h"

int ICACHE_FLASH_ATTR sendOK(HttpdConnData *connData, const char *t);
int ICACHE_FLASH_ATTR sendJSON(HttpdConnData *connData);
int ICACHE_FLASH_ATTR jsonEq(const char *json, jsmntok_t *tok, const char *s) ;
int ICACHE_FLASH_ATTR jsonNum(const char *json, jsmntok_t *tok);
char ICACHE_FLASH_ATTR *jsonStr(const char *json, jsmntok_t *tok, char *s);
char ICACHE_FLASH_ATTR *jsonStr_p(char *json, jsmntok_t *tok); // Changes *json buf
int ICACHE_FLASH_ATTR jsonSkip(jsmntok_t *t);
uint32 ICACHE_FLASH_ATTR toHex(const char *p);

#define HTTPD_SEND_STR(str)  do {				\
		if (connData->conn)						\
			httpdSend(connData, (str), strlen(str)); \
		else									\
			os_strcat((char *)connData->priv, str);		\
	} while (0)

#define HTTPD_PRINTF(fmt, args...)  do {			\
		char buf[1024];								\
		os_sprintf(buf, fmt, ##args);				\
		if (connData->conn)							\
			httpdSend(connData, buf, strlen(buf));	\
		 else 										\
			os_strcat((char *)connData->priv, buf);	\
	} while (0)

#define URL_IS(y) (os_strstr(connData->url, "/api/") \
			&& !os_strncmp(os_strstr(connData->url, "/api/"), (y), strlen(y)))

#endif
