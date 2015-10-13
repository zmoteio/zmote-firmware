#ifndef IR_H
#define IR_H

#include "httpd.h"

int irOps(HttpdConnData *connData);
int irSetLed(HttpdConnData *connData);
int ICACHE_FLASH_ATTR irSend(char *cmd);
void irInit(void);

#endif