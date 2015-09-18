#ifndef ZMOTE_CONFIG_H_
#define ZMOTE_CONFIG_H_

#include "httpd.h"

void ICACHE_FLASH_ATTR cfgInit(void);
char *ICACHE_FLASH_ATTR cfgGet(const char *key, char *val, int n);
void ICACHE_FLASH_ATTR cfgSet(const char *key, const char *val);

int ICACHE_FLASH_ATTR cfgOps(HttpdConnData *connData);
int ICACHE_FLASH_ATTR cfgGetSet(HttpdConnData *connData);
int ICACHE_FLASH_ATTR cfgFile(HttpdConnData *connData);

#endif
