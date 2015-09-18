#ifndef STLED_H_
#define STLED_H_

typedef enum {
	STLED_OFF,
	STLED_ON,
	STLED_BLINK_SLOW,
	STLED_BLINK_FAST,
	STLED_BLINK_HB
} stledOp;

void ICACHE_FLASH_ATTR stledInit(void);
stledOp ICACHE_FLASH_ATTR stledGet(void);
void ICACHE_FLASH_ATTR stledSet(stledOp op);
int ICACHE_FLASH_ATTR stledOps(HttpdConnData *connData);

#endif
