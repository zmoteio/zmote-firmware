#include <esp8266.h>
#include "ir.h"
#include "jsmn.h"
#include "rest_utils.h"
#include "console.h"
#include "mutex.h"
#include "mqttclient.h"
#include "zmote_config.h"

#define TX_GPIO 2
#define RX_GPIO 3
#define LED_GPIO 0

#define DEBUG_IR_TX
enum {
	RPT_COUNT,
	RPT_START,
	RPT_END
};
typedef struct IrCode_ {
	struct IrCode_ *next;
	uint32 period; // Actually period/2 in Q16
	// These are indices into the seq[] table
	uint16 n; 		// seq count for first transmit 
	uint16 alloc; 	// number allocated for seq[]
	sint16 repeat[3]; 	// count, start and end; if count is negative, repeat forever or until stop
	// State variables
	uint16 isRepeat; 	// Set when entering repeat mode
	uint16 cur; 	// Current index
	uint16 stop; 	// Flag to cause abort 

	ETSTimer timer; // Timer for longer gaps. FIXME: worth making dynamic?

#ifdef DEBUG_IR_TX
	uint32 startTime;
	uint32 np;
#endif
	// Actual sequence
	uint16 seq[]; // flexible array member: https://gcc.gnu.org/onlinedocs/gcc/Zero-Length.html
} IrCode;

// Rx related globals: Interrupt is not re-entrant (I hope)
static uint32 rxTrigger[256];
static uint8 rxTriggerNdx = 0, rxReadNdx = 0;

static uint16 rxLastCode[256]; // in delta us; zero marks the end
static ETSTimer rxTimer; // Timeout on receive
static mutex_t rxMutex; // Protects access to last code
static bool irMonitor = false; // Enables reporting over mqtt

// Tx related globals
static mutex_t txMutex;
static int txNow = 0; // Index into txArray
static IrCode **txArray = NULL;  // txArray is NULL terminated
static ETSTimer txAbortTimer; // Needed when mutex is unavailable to abort

// Config variables read at startup (FIXME not done yet)
static uint32 maxBusyWait = 10000 /* us */, rxTimeout = 500 /* ms */;
static int maxRepeat = 20;

static int  ICACHE_FLASH_ATTR readSeq(uint16 *p, const char *json, jsmntok_t *t)
{
	int i;

	for (i = 0; i < t[0].size; i++) {
		if (t[i+1].type != JSMN_PRIMITIVE)
			goto err;
		p[i] = jsonNum(json, &t[i+1]);
	}
	return t[0].size;
err:
	ERROR("Error parsing sequence");
	return 0;
}

static IrCode *ICACHE_FLASH_ATTR parseSeq(const char *json, jsmntok_t *t, int *skip)
{
	int i, j, alloc = -1;
	IrCode *code = NULL;

	/* Find out the sequence length */
	/* Loop over all keys of the root object */
	INFO("sz=%d", t[0].size);
	for (i = 0, j = 1; i < t[0].size; i++, j += 2 + t[j+1].size) {
		//INFO("check %s", jsonStr(json, &t[i], temp));
		if (jsonEq(json, &t[j], "seq")) {
			alloc = t[j+1].size;
			break;
		}
	}
	if (alloc < 0) {
		ERROR("seq[] not found");
		goto err;
	}
	if (alloc&1)
		++alloc; // Need an even number
	INFO("Sequence has %d codes", alloc);
	if (!(code = os_zalloc(sizeof(*code) + alloc*sizeof(code->seq[0])))) {
		ERROR("out of mem");
		goto err;
	}
	code->alloc = alloc;

	for (i = 0, j = 1; i < t[0].size; i++, j += 2 + t[j+1].size) {
		if (jsonEq(json, &t[j], "period")) {
			code->period = jsonNum(json, &t[j+1]);
		} else if (jsonEq(json, &t[j], "frequency")) {
			//code->period = 32768000000.0 / jsonNum(json, &t[j+1]);
			code->period = ((1000000u*0x1000u)/jsonNum(json, &t[j+1]))<<3u;
		} else if (jsonEq(json, &t[j], "n")) {
			code->n = jsonNum(json, &t[j+1]);
		} else if (jsonEq(json, &t[j], "repeat")) {
			if (t[j+1].size != 3) {
				ERROR("Repeat array sz=%d", t[j+1].size);
				goto err;
			}
			readSeq((uint16*)code->repeat, json, &t[j+1]);
		} else if (jsonEq(json, &t[j], "seq")) {
			readSeq(code->seq, json, &t[j+1]);
		} else {
			ERROR("BAD token %d,%d: type=%d [%d,%d] sz=%d", i, j, t[j].type, t[j].start, t[j].end, t[j].size);
			goto err;
		}
	}
	if (!code->period || !code->n) {
		ERROR("Badly formatted command");
		goto err;
	}
	if (code->repeat[RPT_COUNT] > maxRepeat)
		code->repeat[RPT_COUNT] = maxRepeat;
	if (skip)
		*skip = j;
	return code;
err:
	if (code)
		os_free(code);
	return NULL;

}
static IrCode ** ICACHE_FLASH_ATTR parseCode(const char *json)
{
	int i, j, r, len = strlen(json), skip;
	jsmntok_t *t = NULL;
	IrCode **pCode = NULL;
	jsmn_parser p;

	// Determine # of tokesn and allocate
	jsmn_init(&p);
	r = jsmn_parse(&p, json, len, NULL, 0);
	if (r <= 0) {
		ERROR("Failed to parse JSON: %d\n", r);
		goto err;
	}
	if (!(t = os_malloc(r*sizeof(t[0])))) {
		ERROR("out of mem");
		goto err;
	}
	jsmn_init(&p);
	r = jsmn_parse(&p, json, len, t, r);

	/* Top-level element is object or array */
	if (r < 1 || (t[0].type != JSMN_OBJECT && t[0].type != JSMN_ARRAY)) {
		ERROR("Object or array expected");
		goto err;
	}
	if (t[0].type == JSMN_OBJECT) {
		if (!(pCode = os_zalloc(2*sizeof(IrCode *)))) {
			ERROR("memory err");
			goto err;
		}
		if (!(pCode[0] = parseSeq(json, &t[0], NULL))) {
			ERROR("error parsing code");
			goto err;
		}
	} else {
		if (!(pCode = os_zalloc((t[0].size + 1)*sizeof(IrCode *)))) {
			ERROR("memory err");
			goto err;
		}
		for (i = 0, j = 1; i < t[0].size; i++) {
			if (!(pCode[i] = parseSeq(json, &t[j], &skip))) {
				ERROR("Error parsing code#%d", i);
				goto err;
			}
			j += skip;
		}
	}
	os_free(t);
	return pCode;
err:
	if (t)
		os_free(t);
	if (pCode)
		os_free(pCode);
	return NULL;
}

static IrCode ** ICACHE_FLASH_ATTR parseIRSend(const char *cmd)
{
	int i, n = 0, alloc;
	const char *p;
	IrCode **pCode = NULL;

	// COunt the number of commas
	p = cmd;
	while (*p && (p = strchr(p, ','))) {
		++p;
		++n;
	}
	alloc = n - 2;
	if (alloc&1)
		++alloc;
	INFO("n=%d alloc=%d", n, alloc);
	// Allocate
	if (!(pCode = os_zalloc(2*sizeof(IrCode *)))) {
		ERROR("memory err");
		goto err;
	}
	if (!(pCode[0] = os_zalloc(sizeof(*pCode[0]) + alloc*sizeof(pCode[0]->seq[0])))) {
		ERROR("out of mem");
		goto err;
	}
	pCode[0]->alloc = alloc;

	// Format of the string is
	//	frequency,repeatN,repeatStart,ON,OFF,ON,OFF
	p = cmd;
	pCode[0]->period = atoi(p);
	INFO("freq=%d", pCode[0]->period);
	// we need to do 1e6/freq/2*65536
	// we try to preserve precision while sticking with integer arithmetic
	// we lose out on just bits
	pCode[0]->period = ((1000000u*0x1000u)/(pCode[0]->period))<<3u;
	INFO("period=%d", pCode[0]->period);
	pCode[0]->n = alloc;
	pCode[0]->alloc = alloc;
	p = strchr(p, ',') + 1; 
	pCode[0]->repeat[0] = atoi(p) - 1;
	p = strchr(p, ',') + 1; 
	pCode[0]->repeat[1] = atoi(p) - 1;
	pCode[0]->repeat[2] = alloc - pCode[0]->repeat[1];

	for (i = 0; i < n - 2; i++) {
		p = strchr(p, ',') + 1; 
		pCode[0]->seq[i] = atoi(p);
	}
	return pCode;
err:
	if (pCode && pCode[0])
		os_free(pCode[0]);
	if (pCode)
		os_free(pCode);
	return NULL;
}

// Cheap and dirty "accumulative" timer protects against drift errors during ON phase
static uint32 start;
static void accTimer(uint32 delay)
{
	//uint32 start = system_get_time();
	while (system_get_time() < start+delay)
		;
	start = system_get_time();
}
static void txOn(int pin, int pulse, int n) 
{
	int i, err = 0, del;
#ifdef INVERT_IR_TX
	int s = 0;
#else
	int s = 1;
#endif
	start = system_get_time();
	for (i = 0; i < n; i++) {
		//system_get_time(void)
		del = pulse + err;
		GPIO_OUTPUT_SET(GPIO_ID_PIN(pin), s);
		//os_delay_us(del>>16);
		accTimer(del>>16);
		err = (del&0xFFFFu);

		del = pulse + err;
		GPIO_OUTPUT_SET(GPIO_ID_PIN(pin), (1-s));
		//os_delay_us(del>>16);
		accTimer(del>>16);
		err = (del&0xFFFFu);

	}
}

static int ICACHE_FLASH_ATTR checkFinished(IrCode *code)
{
	int n = (code->isRepeat?code->repeat[RPT_END]:code->n);
	int isEnd = (n <= code->cur);
	//INFO("checkFinish cur=%d n=%d end=%d", code->cur, n, isEnd);
#ifdef DEBUG_IR_TX
	if (!code->startTime)
		code->startTime = system_get_time();
#endif
	if (code->stop)
		goto finish;
	if (!isEnd)
		return 0; // Not finished
	if (/*!code->isRepeat && */code->repeat[RPT_COUNT])
		code->isRepeat = 1; // Enter repeat mode
	if (!code->isRepeat)
		goto finish; // Have reached end of non-repeat code
	if (code->repeat[RPT_COUNT] != 0) {
		if (code->repeat[RPT_COUNT] > 0)
			--code->repeat[RPT_COUNT];
		code->cur = code->repeat[RPT_START];
		return 0;  // Not finished; restart at repeat point
	}
	// Definitely finished or stopped by this point
finish:
	INFO("Code Finished");
#ifdef DEBUG_IR_TX
	uint32 end = system_get_time();
	end = end - code->startTime;
	code->np = 2*((code->np*code->period)>>16);
	INFO("Sending finished %u us instead of %u (err=%u)", end, code->np, end-code->np);
#endif
	os_free(code);
	txArray[txNow++] = NULL;
	return 1;
}
static int ICACHE_FLASH_ATTR txCode(IrCode *code) 
{
	uint32 gap;
	//DEBUG("Sending code %u\n", system_get_time());
	INFO("send %d %d", code->cur, code->n);
	if (!GetMutex(&txMutex)) {
		os_timer_disarm(&(code->timer));
		os_timer_setfn(&(code->timer), (os_timer_func_t *)txCode, code);
		os_timer_arm(&(code->timer), 1, 0);
		return 1;

	}
	while (!checkFinished(code))  {
		txOn(TX_GPIO, code->period, code->seq[code->cur]);
		//gap = (code->seq[code->cur+1]*code->period*2)>>16; // FIXME gap needs to be adjusted by acc error
		gap = 2*((code->seq[code->cur+1]*(code->period>>16)) + 
			((code->seq[code->cur+1]*(code->period&0xFFFF))>>16));
#ifdef DEBUG_IR_TX
		code->np += code->seq[code->cur] + code->seq[code->cur+1];
#endif
		code->cur += 2;
		if (gap > maxBusyWait) {
			os_timer_disarm(&(code->timer));
			os_timer_setfn(&(code->timer), (os_timer_func_t *)txCode, code);
			os_timer_arm_us(&(code->timer), gap, 0);
			ReleaseMutex(&txMutex);
			return 1;
		} else if (gap)
			os_delay_us(gap); // Busy wait for smaller times
	}
	ReleaseMutex(&txMutex);
	if (txArray[txNow])
		txCode(txArray[txNow]);
	else {
		if (txArray)
			os_free(txArray);
		txArray = NULL;
	}
	return 2;
}
static int ICACHE_FLASH_ATTR abortSend(void)
{
	if (!GetMutex(&txMutex)) {
		os_timer_disarm(&txAbortTimer);
		os_timer_setfn(&txAbortTimer, (os_timer_func_t *)abortSend, 0);
		os_timer_arm(&txAbortTimer, 10, 0); // Try after 10ms -- arbitrary
		return 2;
	}	
	if (txArray) {
		int i = 0;
		while (txArray[i])
			txArray[i++]->stop = 1;
		ReleaseMutex(&txMutex);
		return 1;
	} else {
		// No code was being sent
		ReleaseMutex(&txMutex);
		return 0;
	}
}
static void ICACHE_FLASH_ATTR printCode(IrCode **pCodes)
{
	#ifdef ENABLE_UART_DEBUG
	int i, j;
	uint32 t;
	IrCode *code;
	for (j = 0; pCodes[j]; ++j) {
		INFO("===============code[%d]", j);
		code = pCodes[j];
		INFO("code.period=%d", code->period);
		INFO("code.n=%d", code->n);
		INFO("code.repeat=[%d,%d,%d]", code->repeat[0], code->repeat[1], code->repeat[2]);
		for (i = 0; i < code->n; i++) {
			t = 2*((code->seq[i]*code->period)>>16);
			INFO("[%d]: %d pulses = %d us %s", i, code->seq[i], t, ((i&1) && (t > maxBusyWait))?"[*]":"");
		}
	}
	#endif
}


static void gpioInterrupt(void)  // Placed in IRAM (as opposed to ICACHE) FWIW
{
	uint32 status;
	uint32 now;
	now = system_get_time();
	status = GPIO_REG_READ(GPIO_STATUS_ADDRESS);
	if (status & (1u << RX_GPIO)) {
		rxTrigger[rxTriggerNdx++] = now;
	}
	//clear interrupt status
	GPIO_REG_WRITE(GPIO_STATUS_W1TC_ADDRESS, status);
}

static void  (*irLearnCb)(char *) = NULL;
static void ICACHE_FLASH_ATTR transmitLearnedCode(void)
{
	int i, n, big = 0;
	char sendir[512];
	for (i = 0; i < sizeof(rxLastCode)/sizeof(rxLastCode[0]) && big < 2 && rxLastCode[i]; i++)
		if (rxLastCode[i] > maxBusyWait)
			++big;
	DEBUG("Code=%d big=%d", i, big);
	if (big < 2)
		return;
	DEBUG("Probably got a full sequence");
	if (!irLearnCb && !irMonitor)
		return;

	os_sprintf(sendir, "sendir,1:1,0,38400,1,1");
	for (i = 0; i < sizeof(rxLastCode)/sizeof(rxLastCode[0]) && rxLastCode[i]; i++) {
		n = os_strlen(sendir);
		if (n > sizeof(sendir) - 8)
			break;
		// 38400/1e6 * 65536 == 2516.58
		os_sprintf(sendir+n, ",%d", (rxLastCode[i]*2517u + 0x7FFFu)>>16u);
	}
	n = os_strlen(sendir);
	if (i&1)
		os_strcpy(sendir+n, ",3692\r");
	else
		os_strcpy(sendir+n, "\r");
	INFO("Got learned code: %s", sendir);
	if (irLearnCb) {
		irLearnCb(sendir);
		rxLastCode[0] = 0;
		return;
	}
	os_sprintf(sendir, "{\"frequency\":38400,\"seq\":[");
	for (i = 0; i < sizeof(rxLastCode)/sizeof(rxLastCode[0]) && rxLastCode[i]; i++) {
		if (!rxLastCode[i])
			break;
		n = os_strlen(sendir);
		if (n > sizeof(sendir) - 8)
			break;
		// 38400/1e6 * 65536 == 2516.58
		os_sprintf(sendir+n, "%s%u", i?",":"", (rxLastCode[i]*2517u + 0x7FFFu)>>16u);
	}
	n = os_strlen(sendir);
	if (i&1) {
		os_strcpy(sendir+n, ",3692");
		i++;
	}
	n = os_strlen(sendir);
	os_sprintf(sendir+n, "],\"n\":%d,\"repeat\":[0,0,%d]}", i, i);
	mqttPub(sendir); 
	rxLastCode[0] = 0;
}
static void ICACHE_FLASH_ATTR rxMonitor(void)
{
	int ncodes, ndx = 0;
	uint32 lastTS, nextTS;

	//INFO("rxMonitor");
	if (!GetMutex(&rxMutex)) {
		os_timer_disarm(&rxTimer);
		os_timer_setfn(&rxTimer, (os_timer_func_t *)rxMonitor, 0);
		os_timer_arm(&rxTimer, 10, 0); // Try after 10ms -- arbitrary
	//INFO("Mutex Busy");
		return;
	}
	ncodes = (rxTriggerNdx - rxReadNdx)&0xFFu;
	if (ncodes < 2 ) { // No pulses accumulated
		ReleaseMutex(&rxMutex);
		os_timer_disarm(&rxTimer);
		os_timer_setfn(&rxTimer, (os_timer_func_t *)rxMonitor, 0);
		os_timer_arm(&rxTimer, rxTimeout, 0);
	//INFO("No codes %d", ncodes);
		return;
	}
	lastTS = rxTrigger[(rxTriggerNdx - 1u)&0xFFu];
	nextTS = system_get_time();
	if (nextTS - lastTS < rxTimeout*1000) { // Not timeout yet
		ReleaseMutex(&rxMutex);
		os_timer_disarm(&rxTimer);
		os_timer_setfn(&rxTimer, (os_timer_func_t *)rxMonitor, 0);
		os_timer_arm_us(&rxTimer, rxTimeout, (rxTimeout*1000 - (nextTS - lastTS)));
	//INFO("No timeout codes=%d", ncodes);
		return;
	}
	INFO("Got codes=%d [%d,%d]", ncodes, rxReadNdx, rxTriggerNdx);
	lastTS = rxTrigger[rxReadNdx];
	++rxReadNdx;
	while (rxReadNdx != rxTriggerNdx) {
		nextTS = rxTrigger[rxReadNdx++];
		rxLastCode[ndx++] =  nextTS - lastTS;
		//INFO("    code[%d] = %d", ndx - 1, rxLastCode[ndx-1]);
		lastTS = nextTS;
	}
	rxLastCode[ndx++] = 0; // xero marks the end
	INFO("Got code %d irLearnCb=%x irMonitor=%d", ndx, (uint32)irLearnCb, irMonitor);
	if (irLearnCb || irMonitor)
		transmitLearnedCode();
	// FIXME: Post to MQTT here
	ReleaseMutex(&rxMutex);
	os_timer_disarm(&rxTimer);
	os_timer_setfn(&rxTimer, (os_timer_func_t *)rxMonitor, 0);
	os_timer_arm(&rxTimer, rxTimeout, 0);
	//INFO("Finished [%d,%d]", rxReadNdx, rxTriggerNdx);
	return;
}
//Cgi that turns the LED on or off according to the 'led' param in the POST data
int ICACHE_FLASH_ATTR irOps(HttpdConnData *connData) 
{
	int i;

	if (connData->url==NULL) {
		//Connection aborted. Clean up.
		return HTTPD_CGI_DONE;
	}

	if (connData->requestType == HTTPD_METHOD_OPTIONS) {
		sendJSON(connData);
		HTTPD_SEND_STR("{\"status\":\"ok\"}\r\n\r\n");
		return HTTPD_CGI_DONE;
	}
	//DEBUG("url=\"%s\" request_type=%d\n", connData->url, connData->requestType);
	//DEBUG("postData: %s\n", connData->post->buff);
	if (URL_IS("/api/ir/trigger")) {
		// Clear old code and prepare to receive new one
		if (!GetMutex(&rxMutex)) {
			sendOK(connData, "busy");
		} else {
			rxLastCode[0] = 0;
			ReleaseMutex(&rxMutex);
			sendOK(connData, "ok");
		}
	} else if (URL_IS("/api/ir/read")) {
		if (!GetMutex(&rxMutex)) {
			sendOK(connData, "busy");
			return HTTPD_CGI_DONE;
		}
		sendJSON(connData);
		HTTPD_SEND_STR("{\"status\":\"ok\",\"trigger\":[");
		for (i = 0; i < sizeof(rxLastCode)/sizeof(rxLastCode[0]); i++) {
			if (!rxLastCode[i])
				break;
			HTTPD_PRINTF("%s%u", i?",":"", rxLastCode[i]);
		}
		HTTPD_SEND_STR("]}\r\n");
		ReleaseMutex(&rxMutex);
	} else if (URL_IS("/api/ir/write")) {
		if (txArray || !GetMutex(&txMutex)) {
			sendOK(connData, "busy"); // FIXME For now.  There's no Q
			return HTTPD_CGI_DONE;
		}
		if (!(txArray = parseCode(connData->post->buff))) {
			sendOK(connData, "badformat");
			ReleaseMutex(&txMutex);
			return HTTPD_CGI_DONE;
		}
		ReleaseMutex(&txMutex);
		//printCode(txArray);
		(void)printCode; // Shut up, compiler!
		txNow = 0;
		i = txCode(txArray[0]);
		if (!i)
			sendOK(connData, "busy");
		else if (i == 1)
			sendOK(connData, "ok"); // Sending
		else
			sendOK(connData, "ok"); // FInished
	} else if (URL_IS("/api/ir/stop")) {
		i = abortSend();
		if (!i) 
			sendOK(connData, "ok"); // Free
		else if (i == 1)
			sendOK(connData, "ok"); // Stopped
		else // if (i == 2)
			sendOK(connData, "busy");
		return HTTPD_CGI_DONE;
	} else
		sendOK(connData, "unknown command");
	return HTTPD_CGI_DONE;
}

int ICACHE_FLASH_ATTR irSend(char *cmd)
{
	if (txArray || !GetMutex(&txMutex)) {
		WARN("irSend busy");
		return -1;
	}
	if (!(txArray = parseIRSend(cmd))) {
		ERROR("bad irSend format");
		ReleaseMutex(&txMutex);
		return -1;
	}
	ReleaseMutex(&txMutex);
	//printCode(txArray);
	txNow = 0;
	return txCode(txArray[0]);
}
int ICACHE_FLASH_ATTR irSendStop(void)
{
	return abortSend();
}
int ICACHE_FLASH_ATTR irLearn(void (*cb)(char *))
{
	if (!GetMutex(&rxMutex))
		return -1;
	rxReadNdx = rxTriggerNdx;
	irLearnCb = cb;
	rxLastCode[0] = 0;
	ReleaseMutex(&rxMutex);
	return 1;
}
int ICACHE_FLASH_ATTR irLearnStop(void)
{
	irLearnCb = NULL;
	return 1;
}
void ICACHE_FLASH_ATTR irInit(void)
{
	char temp[64];
	DEBUG("Initializing GPIOs\n");

	if (cfgGet("ir_max_busy_wait", temp, sizeof(temp)))
		maxBusyWait = atoi(temp);
	if (cfgGet("ir_max_repeat", temp, sizeof(temp)))
		maxRepeat = atoi(temp);
	if (cfgGet("ir_rx_timeout", temp, sizeof(temp)))
		rxTimeout = atoi(temp);
	if (cfgGet("ir_monitor", temp, sizeof(temp)))
		irMonitor = atoi(temp)?true:false;

	CreateMutux(&txMutex);
	CreateMutux(&rxMutex);

	PIN_FUNC_SELECT(PERIPHS_IO_MUX_U0RXD_U, FUNC_GPIO3);
	PIN_FUNC_SELECT(PERIPHS_IO_MUX_GPIO2_U, FUNC_GPIO2);
	PIN_FUNC_SELECT(PERIPHS_IO_MUX_GPIO0_U, FUNC_GPIO0);
#ifdef INVERT_IR_TX
	GPIO_OUTPUT_SET(GPIO_ID_PIN(TX_GPIO), 1); // 0 turns on the IR LED
#else
	GPIO_OUTPUT_SET(GPIO_ID_PIN(TX_GPIO), 0);
#endif 
	GPIO_DIS_OUTPUT(RX_GPIO);
	ETS_GPIO_INTR_ATTACH(gpioInterrupt, NULL); 
	gpio_pin_intr_state_set(GPIO_ID_PIN(RX_GPIO), GPIO_PIN_INTR_ANYEDGE);
	ETS_GPIO_INTR_ENABLE();
	rxMonitor(); // Kick off the monitor
}