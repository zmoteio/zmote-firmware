#include <esp8266.h>
#include "rest_utils.h"
#include "stled.h"
#include "jsmn.h"
#include "console.h"

#ifdef UART_TX_AS_STLED
#define STLED_GPIO 1
#else
#define STLED_GPIO 0
#endif

static stledOp opState = STLED_OFF;
static uint8 counter = 0;
static ETSTimer stledTimer;

static const char *opStr[] = {
	[STLED_OFF] "off",
	[STLED_ON] "on",
	[STLED_BLINK_SLOW] "blink_slow",
	[STLED_BLINK_FAST] "blink_fast",
	[STLED_BLINK_HB] "blink_hb"
};

int ICACHE_FLASH_ATTR stledOps(HttpdConnData *connData)
{
	int i, r;
	jsmn_parser p;
	jsmntok_t t[16]; /* We expect no more than 16 tokens */
	char *json = connData->post->buff;

	if (connData->requestType == HTTPD_METHOD_OPTIONS) {
		sendJSON(connData);
		HTTPD_SEND_STR("{\"status\":\"ok\"}\r\n\r\n");
		return HTTPD_CGI_DONE;
	} else if (connData->requestType == HTTPD_METHOD_GET) {
		sendJSON(connData);
		HTTPD_PRINTF("{\"status_led\": \"%s\"}\r\n\r\n", opStr[opState]);
		return HTTPD_CGI_DONE;
	} 

	jsmn_init(&p);
	r = jsmn_parse(&p, json, strlen(json), t, sizeof(t)/sizeof(t[0]));
	if (r < 0) {
		ERROR("[json] Failed to parse JSON: %d", r);
		goto err;
	} else
		DEBUG("[json]: OK. %d tokens\n", r);
	/* Assume the top-level element is an object */
	if (r < 1 || t[0].type != JSMN_OBJECT) {
		ERROR("Object expected\n");
		goto err;
	}

	/* Loop over all keys of the root object */
	for (i = 1; i < r; ) {
		if (jsonEq(json, &t[i], "status_led") && i + 1 < r) {
			if (jsonEq(json, &t[i+1], "on"))
				stledSet(STLED_ON);
			else if (jsonEq(json, &t[i+1], "off"))
				stledSet(STLED_OFF);
			else if (jsonEq(json, &t[i+1], "blink"))
				stledSet(STLED_BLINK_SLOW);
			else if (jsonEq(json, &t[i+1], "blink_fast"))
				stledSet(STLED_BLINK_FAST);
			else if (jsonEq(json, &t[i+1], "blink_slow"))
				stledSet(STLED_BLINK_SLOW);
			else if (jsonEq(json, &t[i+1], "blink_hb"))
				stledSet(STLED_BLINK_HB);
			i += 2;
		} else {
			//os_printf("BAD token %d: %s [%d,%d] sz=%d\n", i, type[t[i].type], t[i].start, t[i].end, t[i].size);
			ERROR("BAD token %d: type=%d [%d,%d] sz=%d", i, t[i].type, t[i].start, t[i].end, t[i].size);
			i++;
		}
	}
	sendOK(connData, "OK");
	return HTTPD_CGI_DONE;
err:
	sendOK(connData, "Error"); // FIXME
	return HTTPD_CGI_DONE;
}

static void ICACHE_FLASH_ATTR stledIsr(void)
{
	switch (opState) {
	case STLED_OFF:
		GPIO_OUTPUT_SET(GPIO_ID_PIN(STLED_GPIO), 1); 
		break;
	case STLED_ON:
		GPIO_OUTPUT_SET(GPIO_ID_PIN(STLED_GPIO), 0); 
		break;
	case STLED_BLINK_SLOW:
		GPIO_OUTPUT_SET(GPIO_ID_PIN(STLED_GPIO), ((counter>>2)&1u)); 
		break;
	case STLED_BLINK_FAST:
		GPIO_OUTPUT_SET(GPIO_ID_PIN(STLED_GPIO), (counter&1u)); 
		break;
	case STLED_BLINK_HB:
		if ((counter&7u) ==0 || (counter&7u) == 2 || (counter&7u) == 4)
			GPIO_OUTPUT_SET(GPIO_ID_PIN(STLED_GPIO), 0);
		else
			GPIO_OUTPUT_SET(GPIO_ID_PIN(STLED_GPIO), 1);
		break;
	}
	++counter;
}
void ICACHE_FLASH_ATTR stledInit(void)
{
	#ifdef UART_TX_AS_STLED
	PIN_FUNC_SELECT(PERIPHS_IO_MUX_U0TXD_U, FUNC_GPIO1);
	#endif
	
	GPIO_OUTPUT_SET(GPIO_ID_PIN(STLED_GPIO), 1);  // status  led pn is always inverted
	opState = STLED_OFF;
	os_timer_disarm(&stledTimer);
	os_timer_setfn(&stledTimer, (os_timer_func_t *)stledIsr, 0);
	os_timer_arm(&stledTimer, 250, 1);

}
stledOp ICACHE_FLASH_ATTR stledGet(void)
{
	return opState;
}
void ICACHE_FLASH_ATTR stledSet(stledOp op)
{
	opState = op;
}

