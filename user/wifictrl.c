/*
Some random cgi routines. Used in the LED example and the page that returns the entire
flash as a binary. Also handles the hit counter on the main page.
*/

/*
 * ----------------------------------------------------------------------------
 * "THE BEER-WARE LICENSE" (Revision 42):
 * Jeroen Domburg <jeroen@spritesmods.com> wrote this file. As long as you retain
 * this notice you can do whatever you want with this stuff. If we meet some day,
 * and you think this stuff is worth it, you can buy me a beer in return.
 * ----------------------------------------------------------------------------
 */


#include <esp8266.h>
#include "wifictrl.h"
#include "mqttclient.h"
#include "itach.h"
#include "rest_utils.h"
#include "jsmn.h"
#include "console.h"
#include "stled.h"
#include "zmote_config.h"
#include "rboot-ota.h"
#include "zmote_config.h"


typedef struct wifiAccessPoint_ {
	char ssid[32];
	sint8 rssi;
	AUTH_MODE authmode;
	int apID; // Index into stored AP list or -1 is not stored
	int connected;
} wifiAccessPoint;
typedef enum {
	WIFI_AP, // Initial, no known APs found or unable to connect to the ones found
	WIFI_WPS, // AP+WPS (starts after three seconds)
	WIFI_CONNECTING, // ESP SDK is attempting to connect
	WIFI_CONNECTED, // Got IP
}  wifiStatus;

static const char *authModeStr[] = {
	"open",
	"wep",
	"wpa_psk",
	"wpa2_psk",
	"wpa_wpa2_psk"
};
static const char *wifiStatusStr[] = {
	"ap",
	"wps",
	"connecting",
	"connected",
};
static const char *wifiNetworkStatus[] = {
	"IDLE",
	"CONNECTING",
	"WRONG_PASSWORD",
	"NO_AP_FOUND",
	"CONNECT_FAIL",
	"GOT_IP"
};

static bool newConnect = false; // Flag is set before connecting to a new access point
								// Used to temporarily turn off softAP so that
								// phone will auto disconnect
static wifiStatus wifiState = WIFI_AP;

static wifiAccessPoint *scannedAP = NULL;

static ETSTimer scanTimer, gpTimer, apTimer;

static int nConnFailed = 0; // No of times connection to AP falied
static int nConnected = 0; // No of stations connected to softAP
static struct station_config stconf;

static void ICACHE_FLASH_ATTR wifiScanDone(void *arg, STATUS status);
static void ICACHE_FLASH_ATTR wifiStartScan(int force);

static char * ICACHE_FLASH_ATTR fmtMac(int which, char *buf)
{
	uint8 mac[6];
	wifi_get_macaddr(which, mac);
	os_sprintf(buf, "%02x-%02x-%02x-%02x-%02x-%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
	return buf;
}
int ICACHE_FLASH_ATTR wifiGetMac(HttpdConnData *connData)
{
	char sta[20], ap[20];
	sendJSON(connData);
	HTTPD_PRINTF("{\"ap_mac\": \"%s\", \"sta_mac\":\"%s\"}\r\n\r\n", fmtMac(SOFTAP_IF, ap), fmtMac(STATION_IF, sta));
	return HTTPD_CGI_DONE;
}
static void ICACHE_FLASH_ATTR wifiDoReset(void)
{
	system_restart();
}
int ICACHE_FLASH_ATTR wifiReset(HttpdConnData *connData)
{
	os_timer_disarm(&scanTimer);
	os_timer_setfn(&scanTimer, (os_timer_func_t *)wifiDoReset, 0);
	os_timer_arm(&scanTimer, 500, 0);
	sendOK(connData, "OK");
	return HTTPD_CGI_DONE;
}
static void ICACHE_FLASH_ATTR wifiConnect()
{
	wifi_station_connect();
}
static void ICACHE_FLASH_ATTR wifiConfigConnect()
{
	wifi_station_set_auto_connect(1);
	wifi_station_set_reconnect_policy(1);	// NG
	wifi_station_set_config(&stconf);
	system_restart();
}
#ifndef ENABLE_WPS
static void ICACHE_FLASH_ATTR wifiDisSoftAP(void)
{
	if (nConnected <= 0) {
		INFO("Disabling softAP");
		wifi_set_opmode_current(STATION_MODE);
		if (wifiState != WIFI_CONNECTED)
			stledSet(STLED_OFF);
	} else {
		// Check again after 3 minutes
		INFO("softAP can't be diabled due to connected client %d", nConnected);
		os_timer_disarm(&apTimer);
		os_timer_setfn(&apTimer, (os_timer_func_t *)wifiDisSoftAP, NULL);
		os_timer_arm(&apTimer, 3*60*1000, 0);
	}
}
#endif

int ICACHE_FLASH_ATTR wifiConfig(HttpdConnData *connData)
{
	struct softap_config ap_config;
	int i, r;
	jsmn_parser p;
	char *json = connData->post->buff;
	jsmntok_t t[16];

	if (connData->requestType == HTTPD_METHOD_OPTIONS) {
		sendJSON(connData);
		HTTPD_SEND_STR("{\"status\":\"ok\"}\r\n\r\n");
		return HTTPD_CGI_DONE;
	}

	if (!wifi_softap_get_config(&ap_config)) // That way we don't have to worry about defaults
		goto err;

	jsmn_init(&p);
	r = jsmn_parse(&p, json, strlen(json), t, sizeof(t)/sizeof(t[0]));
	if (r < 1 || t[0].type != JSMN_OBJECT) {
		ERROR("Failed to parse JSON: %d", r);
		goto err;
	}

	for (i = 1; i < r; ) {
		if (jsonEq(json, &t[i], "ssid") && i + 1 < r) {
			jsonStr(json, &t[i+1], (char *)ap_config.ssid);
			i += 2;
		} else {
			//os_printf("BAD token %d: %s [%d,%d] sz=%d\n", i, type[t[i].type], t[i].start, t[i].end, t[i].size);
			ERROR("BAD token %d: type=%d [%d,%d] sz=%d", i, t[i].type, t[i].start, t[i].end, t[i].size);
			goto err;
		}
	}
   	ap_config.ssid_len = 0;// or its actual length
   	ap_config.max_connection = 4; // how many stations can connect to ESP8266 softAP at most.
	if (!wifi_softap_set_config(&ap_config))
		goto err;
	cfgSet("ssidAP", (char *)ap_config.ssid);
	 sendOK(connData, "OK");
	 return HTTPD_CGI_DONE;
err:
	sendOK(connData, "error");
	return HTTPD_CGI_DONE;
}

int ICACHE_FLASH_ATTR wifiConnectAP(HttpdConnData *connData)
{
	int i, r, apID = -1;
	jsmn_parser p;
	char *json = connData->post->buff;
	jsmntok_t t[16]; /* We expect no more than 16 tokens */

	if (connData->requestType == HTTPD_METHOD_OPTIONS) {
		sendJSON(connData);
		HTTPD_SEND_STR("{\"status\":\"ok\"}\r\n\r\n");
		return HTTPD_CGI_DONE;
	}

	jsmn_init(&p);
	r = jsmn_parse(&p, json, strlen(json), t, sizeof(t)/sizeof(t[0]));
	if (r < 1 || t[0].type != JSMN_OBJECT) {
		ERROR("Failed to parse JSON: %d", r);
		sendOK(connData, "JSON error");
		return HTTPD_CGI_DONE;
	}

	for (i = 1; i < r; ) {
		if (jsonEq(json, &t[i], "ssid") && i + 1 < r) {
			jsonStr(json, &t[i+1], (char *)stconf.ssid);
			i += 2;
		} else if (jsonEq(json, &t[i], "password") && i + 1 < r) {
			jsonStr(json, &t[i+1], (char *)stconf.password);
			i += 2;
		} else if (jsonEq(json, &t[i], "apID") && i + 1 < r) {
			apID = jsonNum(json, &t[i+1]);
			break;
		} else {
			//os_printf("BAD token %d: %s [%d,%d] sz=%d\n", i, type[t[i].type], t[i].start, t[i].end, t[i].size);
			ERROR("BAD token %d: type=%d [%d,%d] sz=%d", i, t[i].type, t[i].start, t[i].end, t[i].size);
			sendOK(connData, "JSON error");
			return HTTPD_CGI_DONE;
		}
	}
	if (apID >= 0) {
		if (!wifi_station_ap_change(apID))
			sendOK(connData, "error");
		else
			sendOK(connData, "OK");
	} else if (strlen((char *)stconf.ssid) /*&& strlen((char *)stconf.password)*/) {
		cfgSet("ssidSTA", (char *)stconf.ssid);
		cfgSet("pswdSTA", (char *)stconf.password);
		INFO("connecting to %s:%s", stconf.ssid, stconf.password);
		sendOK(connData, "OK");
		stconf.bssid_set = 0;
		newConnect = true;
		os_timer_disarm(&gpTimer);
		os_timer_setfn(&gpTimer, (os_timer_func_t *)wifiConfigConnect, NULL);
		os_timer_arm(&gpTimer, 1000, 0);
	} else {
		ERROR("no apID or ssid and password");
		sendOK(connData, "JSON error");
		return HTTPD_CGI_DONE;
	}
	return HTTPD_CGI_DONE;
}
int ICACHE_FLASH_ATTR wifiScan(HttpdConnData *connData)
{
	int i;

	wifiStartScan(1); // Force a rescan
	sendJSON(connData);
	if (!scannedAP) {
		HTTPD_SEND_STR("[]\r\n\r\n");
		return HTTPD_CGI_DONE;
	}
	HTTPD_SEND_STR("[");
	for (i = 0;  strlen(scannedAP[i].ssid); i++) {
		HTTPD_PRINTF("%s{\"ssid\":\"%s\", \"rssi\":%d, \"authmode\":\"%s\",  \"apID\": %d, \"connected\": %s}",
			i?",":"", scannedAP[i].ssid, scannedAP[i].rssi, authModeStr[scannedAP[i].authmode],
			scannedAP[i].apID,
			scannedAP[i].connected?"true":"false");
	}
	HTTPD_SEND_STR("]\r\n\r\n");
	return HTTPD_CGI_DONE;
}
int ICACHE_FLASH_ATTR wifiConnectionStatus(HttpdConnData *connData)
{
	struct softap_config ap_config;
	char fs_version[32];

	wifi_softap_get_config(&ap_config);
	sendJSON(connData);
	struct ip_info ipconfig;
	wifi_get_ip_info(STATION_IF, &ipconfig);
	HTTPD_PRINTF("{\"sdk\":\"%s\",\"freq\":%d,\"chipID\":\"%x\",\"boot\":%d,"
		"\"version\":\"%s\",\"commit\":\"%16s\",\"build\":\"%s\","
		"\"ap_ssid\":\"%s\",\"free_heap\":%d,"
		"\"network_status\":\"%s\",\"ip\":\"%d.%d.%d.%d\","
		"\"wifi_status\":\"%s\",\"fs_version\":\"%s\"}",
		system_get_sdk_version(), system_get_cpu_freq(), system_get_chip_id(), rboot_get_current_rom(),
		ZMOTE_FIRMWARE_VERSION, ZMOTE_FIRMWARE_COMMIT, ZMOTE_FIRMWARE_BUILD "("__DATE__ ")",
		(char *)ap_config.ssid, system_get_free_heap_size(),
		wifiNetworkStatus[wifi_station_get_connect_status()], IP2STR(&ipconfig.ip),
		wifiStatusStr[wifiState], 
		cfgGet("fs_version", fs_version, sizeof(fs_version)));
	return HTTPD_CGI_DONE;
}

static void ICACHE_FLASH_ATTR gotIPCb(void)
{
	mqttInit();
	itachInit();

	if (newConnect) {
		newConnect = false;
		wifi_set_opmode_current(STATION_MODE);
	}
}
void ICACHE_FLASH_ATTR wifiEventCB(System_Event_t *evt)
{
	INFO("event %x", evt->event);
	switch (evt->event) {
	case EVENT_STAMODE_CONNECTED:
		nConnFailed = 0;
		wifiState = WIFI_CONNECTING;
		WARN("connect to ssid %s, channel %d",
		          evt->event_info.connected.ssid,
		          evt->event_info.connected.channel);
#ifndef ENABLE_WPS
		os_timer_disarm(&apTimer);
		wifiDisSoftAP();
#endif
		break;
	case EVENT_STAMODE_DISCONNECTED:
		nConnFailed++;
		wifiState = WIFI_AP;
		WARN("disconnect from ssid %s, reason %d",
		          evt->event_info.disconnected.ssid,
		          evt->event_info.disconnected.reason);
		// Restart if we could not connect to an AP for quite some time (only if SoftAP is disabled)
		if (nConnFailed >= 20 && wifi_get_opmode() != STATIONAP_MODE)
			system_restart();
		os_timer_disarm(&scanTimer);
		os_timer_setfn(&scanTimer, (os_timer_func_t *)wifiStartScan, 0);
		os_timer_arm(&scanTimer, 10, 0); // Re-scan right away
		stledSet(STLED_BLINK_SLOW);
		os_timer_disarm(&gpTimer);
		os_timer_setfn(&gpTimer, (os_timer_func_t *)wifiConnect, NULL);
		// NOTE: Once wifi_station_connect() is called, system re-tries connection every 1s until
		//       connection is established. This timer will be irrelevent then.
		os_timer_arm(&gpTimer, 10*1000, 0);
		break;
	case EVENT_STAMODE_AUTHMODE_CHANGE:
		WARN("mode: %d -> %d",
		          evt->event_info.auth_change.old_mode,
		          evt->event_info.auth_change.new_mode);
		break;
	case EVENT_STAMODE_GOT_IP:
		wifiState = WIFI_CONNECTED;
		WARN("ip:" IPSTR ",mask:" IPSTR ",gw:" IPSTR,
		          IP2STR(&evt->event_info.got_ip.ip),
		          IP2STR(&evt->event_info.got_ip.mask),
		          IP2STR(&evt->event_info.got_ip.gw));
		os_timer_disarm(&gpTimer);
		os_timer_setfn(&gpTimer, (os_timer_func_t *)gotIPCb, NULL);
		os_timer_arm(&gpTimer, 1, 0);
		stledSet(STLED_ON);
		break;
	case EVENT_SOFTAPMODE_STACONNECTED:
		WARN("station: " MACSTR "join, AID = %d",
		          MAC2STR(evt->event_info.sta_connected.mac),
		          evt->event_info.sta_connected.aid);
		++nConnected;
		break;
	case EVENT_SOFTAPMODE_STADISCONNECTED:
		WARN("station: " MACSTR "leave, AID = %d",
		          MAC2STR(evt->event_info.sta_disconnected.mac),
		          evt->event_info.sta_disconnected.aid);
		--nConnected;
		break;
	default:
		WARN("Unknown event");
		break;
	}
}

static void ICACHE_FLASH_ATTR wifiStartScan(int force)
{
	if (force || wifiState == WIFI_AP || wifiState == WIFI_WPS)
		wifi_station_scan(NULL, wifiScanDone);
}

static void ICACHE_FLASH_ATTR wifiScanDone(void *arg, STATUS status)
{
	int i, j, n = 0, nstored, current;
	struct station_config storedAP[5];
	if (status != OK) {
		ERROR("scan error %d", status);
		return;
	}
	INFO("scan finished");
	struct bss_info *bss_link = (struct bss_info *)arg;
	bss_link = bss_link->next.stqe_next;
	while (bss_link != NULL) {
		INFO("[%d] %s rssi=%d auth=%d channel=%d", n, bss_link->ssid, bss_link->rssi, bss_link->authmode, bss_link->channel);
		bss_link = bss_link->next.stqe_next;
		n++;
	}
	if (scannedAP)
		os_free(scannedAP);
	if (!(scannedAP = os_zalloc((n+6)*sizeof(scannedAP[0])))) {
		ERROR("out of mem n=%d sz=%d", n, (n+6)*sizeof(scannedAP[0]));
		return;
	}
	nstored = wifi_station_get_ap_info(storedAP);
	INFO("Stored networks=%d", nstored);
	current = wifi_station_get_current_ap_id();
	INFO("Current network=%d", current);
	#ifdef ENABLE_UART_DEBUG
	for (i = 0; i < nstored; i++)
		INFO("\tStored[%d]=%s", i, storedAP[i].ssid);
	#endif
	bss_link = ((struct bss_info *)arg)->next.stqe_next;
	for (i = 0; i < n; i++) {
		os_strcpy(scannedAP[i].ssid, (char *)bss_link->ssid);
		scannedAP[i].rssi = bss_link->rssi;
		scannedAP[i].authmode = bss_link->authmode;
		scannedAP[i].apID = -1;
		for (j = 0; j < nstored; j++) {
			if (!os_strcmp((char *)storedAP[j].ssid, (char *)scannedAP[i].ssid)) {
				scannedAP[i].apID = j;
				break;
			}
		}
		if (current >= 0 && current == scannedAP[i].apID)
			scannedAP[i].connected = 1;
		else
			scannedAP[i].connected = 0;
		bss_link = bss_link->next.stqe_next;
	}
	os_strcpy(scannedAP[i].ssid, "");
	INFO("scan done %d APs", n);
	os_timer_disarm(&scanTimer);
	os_timer_setfn(&scanTimer, (os_timer_func_t *)wifiStartScan, 0);
	os_timer_arm(&scanTimer, (wifiState == WIFI_AP)?60000:15*60000, 0);
}
#ifdef ENABLE_WPS

static ICACHE_FLASH_ATTR void wifiWPSStart(void);
static ICACHE_FLASH_ATTR void wifiWPSCb(int status)
{
	INFO("wps callback %d", status);
	if (status == WPS_CB_ST_SUCCESS) {
		INFO("wps success");
		wifiState = WIFI_AP;
		wifi_wps_disable();
		wifi_station_connect();
		stledSet(STLED_BLINK_SLOW);
	} else {
		// Retry (FIXME: need to give up after some number of attempts)
		INFO("wps fail.  status=%d", wifiState);
		if (wifiState == WIFI_WPS) {
			INFO("retry");
			wifi_wps_start();
		}
	}

}
static ICACHE_FLASH_ATTR void wifiWPSStart(void)
{
	if (wifiState == WIFI_CONNECTING) {
		os_timer_disarm(&apTimer);
		os_timer_setfn(&apTimer, (os_timer_func_t *)wifiWPSStart, NULL);
		os_timer_arm(&apTimer, 3000, 0);
		return;
	} else if (wifiState == WIFI_CONNECTED) {
		return;
	} else {
		wifiState = WIFI_WPS;
		wifi_wps_disable();
		wifi_wps_enable(WPS_TYPE_PBC);
		wifi_set_wps_cb(wifiWPSCb);
		wifi_wps_start();
		stledSet(STLED_BLINK_HB);
	}

}
#endif

void ICACHE_FLASH_ATTR wifiInit(void)
{
	if (wifi_get_opmode() != STATIONAP_MODE)
		wifi_set_opmode(STATIONAP_MODE); // Yes, always
	wifi_station_set_reconnect_policy(1);
	wifi_set_event_handler_cb(wifiEventCB);
	system_init_done_cb((init_done_cb_t) wifiStartScan); // This may look like a problem, but is not
	wifiState = WIFI_AP;
	#ifdef ENABLE_WPS
	// DIsable for now
	os_timer_disarm(&apTimer);
	os_timer_setfn(&apTimer, (os_timer_func_t *)wifiWPSStart, NULL);
	os_timer_arm(&apTimer, 10000, 0);
	#else
	// SoftAP is disabled 3 minutes after boot
	os_timer_disarm(&apTimer);
	os_timer_setfn(&apTimer, (os_timer_func_t *)wifiDisSoftAP, NULL);
	os_timer_arm(&apTimer, 3*60*1000, 0);
	#endif

	stledSet(STLED_BLINK_SLOW);
}