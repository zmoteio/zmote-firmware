#ifndef IR_H
#define IR_H

#include "httpd.h"

int irOps(HttpdConnData *connData);
int irSetLed(HttpdConnData *connData);

int ICACHE_FLASH_ATTR irSend(char *cmd);
int ICACHE_FLASH_ATTR irSendStop(void);
int ICACHE_FLASH_ATTR irLearn(void (*cb)(char *));
int ICACHE_FLASH_ATTR irLearnStop(void);

void irInit(void);

#endif