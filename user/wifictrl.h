/*
Some random cgi routines. Used in the LED example and the page that returns the entire
flash as a binary. Also handles the hit counter on the main page.
*/

#ifndef WIFICTRL_H_
#define WIFICTRL_H_
#include "httpd.h"
int ICACHE_FLASH_ATTR wifiGetMac(HttpdConnData *connData);
int ICACHE_FLASH_ATTR wifiConnectAP(HttpdConnData *connData);
int ICACHE_FLASH_ATTR wifiScan(HttpdConnData *connData);
int ICACHE_FLASH_ATTR wifiConnectionStatus(HttpdConnData *connData);
int ICACHE_FLASH_ATTR wifiReset(HttpdConnData *connData);
int ICACHE_FLASH_ATTR wifiConfig(HttpdConnData *connData);
void ICACHE_FLASH_ATTR wifiInit(void);
#endif
