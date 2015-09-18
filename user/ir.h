#ifndef IR_H
#define IR_H

#include "httpd.h"

int irOps(HttpdConnData *connData);
int irSetLed(HttpdConnData *connData);
void irInit(void);

#endif