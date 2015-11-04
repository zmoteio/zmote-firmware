#ifndef ITACH_H_
#define ITACH_H_

int ICACHE_FLASH_ATTR itachConfig(HttpdConnData *connData);
void ICACHE_FLASH_ATTR itachInit(void);
void ICACHE_FLASH_ATTR itach_command(const char *data, char *reply, int len, void (*cb)(char *));

#endif

	