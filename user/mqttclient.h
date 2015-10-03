#ifndef MQTTCLIENT_H_
#define MQTTCLIENT_H_

void ICACHE_FLASH_ATTR mqttInit(void);
void ICACHE_FLASH_ATTR mqttHello(void);
int ICACHE_FLASH_ATTR mqttPub(const char *msg);
int ICACHE_FLASH_ATTR mqttConnected(void);

#endif

