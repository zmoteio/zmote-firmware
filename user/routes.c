#include <esp8266.h>
#include "httpd.h"
#include "httpdespfs.h"
#include "ir.h"
#include "auth.h"
#include "wifictrl.h"
#include "mqttclient.h"
#include "stled.h"
#include "zmote_config.h"
#include "routes.h"
#include "rest_utils.h"
#include "console.h"
#include "itach.h"

#if 0
//Function that tells the authentication system what users/passwords live on the system.
//This is disabled in the default build; if you want to try it, enable the authBasic line in
//the builtInUrls below.
int myPassFn(HttpdConnData *connData, int no, char *user, int userLen, char *pass, int passLen) {
	if (no==0) {
		os_strcpy(user, "admin");
		os_strcpy(pass, "s3cr3t");
		return 1;
//Add more users this way. Check against incrementing no for each user added.
//	} else if (no==1) {
//		os_strcpy(user, "user1");
//		os_strcpy(pass, "something");
//		return 1;
	}
	return 0;
}
#endif

#define STA_MAC_ADDR_MARKER "/xx-yy-zz-pp-qq-rr"
static int ICACHE_FLASH_ATTR apRedirect(HttpdConnData *connData);
static HttpdBuiltInUrl builtInUrls[] = {
	{"/", apRedirect, "zmote.io"},
	{"/", cgiRedirect, "/index.html"},

	//Enable the line below to protect the WiFi configuration with an username/password combo.
	//	{"*", authBasic, myPassFn},
	{"/api/wifi/mac", wifiGetMac, NULL},
	{STA_MAC_ADDR_MARKER "/api/wifi/mac", wifiGetMac, NULL},
	{STA_MAC_ADDR_MARKER "/api/wifi/scan", wifiScan, NULL},
	{STA_MAC_ADDR_MARKER "/api/wifi/connect", wifiConnectAP, NULL},
	{STA_MAC_ADDR_MARKER "/api/wifi/status", wifiConnectionStatus, NULL},
	{STA_MAC_ADDR_MARKER "/api/wifi/reset", wifiReset, NULL},
	{STA_MAC_ADDR_MARKER "/api/wifi/config", wifiConfig, NULL},

	{STA_MAC_ADDR_MARKER "/api/ir/*", irOps, NULL},
	{STA_MAC_ADDR_MARKER "/api/stled", stledOps, NULL},

	{STA_MAC_ADDR_MARKER "/api/spi/*", cfgOps, NULL},
	{STA_MAC_ADDR_MARKER "/api/config/*", cfgGetSet, NULL},

	{"/config.htm", itachConfig, NULL },

	{"*", cfgFile, NULL}, //Catch-all cgi function for the filesystem
	{NULL, NULL, NULL}
};
static const char *httpNotFoundHeader="HTTP/1.0 404 Not Found\r\nServer: esp8266-httpd/"HTTPDVER"\r\nConnection: close\r\nContent-Type: text/plain\r\nContent-Length: 12\r\n\r\nNot Found.\r\n";
static int ICACHE_FLASH_ATTR apRedirect(HttpdConnData *connData) {
    uint32 *remadr;
    struct ip_info apip;
    int x=wifi_get_opmode();
    //Check if we have an softap interface; bail out if not
    if (x!=2 && x!=3) return HTTPD_CGI_NOTFOUND;
    if (os_strcmp(connData->hostName, "zmote.io") && os_strcmp(connData->hostName, "www.zmote.io")) {
        // Req not for zmote.io
        // Return 404 not found
        httpdSend(connData, httpNotFoundHeader, -1);
        return HTTPD_CGI_DONE;
    }
    remadr=(uint32 *)connData->conn->proto.tcp->remote_ip;
    wifi_get_ip_info(SOFTAP_IF, &apip);
    if ((*remadr & apip.netmask.addr) == (apip.ip.addr & apip.netmask.addr)) {
        return cgiRedirectToHostname(connData);
    } else {
        return HTTPD_CGI_NOTFOUND;
    }
}
static int ICACHE_FLASH_ATTR fixPostData(char *postBuf, int len)
{
	int i = 0, j = 0;
	while (j < len) {
		if (postBuf[j] == '\\' && postBuf[j+1] == '"')
			++j; // Skip over the back slash
		// Copy everything else
		postBuf[i++] = postBuf[j++];
	}
	postBuf[i] = 0;
	DEBUG("fixPostdata in=%d out=%d", j, i);

	return i;
}

void ICACHE_FLASH_ATTR	execRoute(const char *url, const char *method,
	const char *postBuf, int pbLen, char *response, int repLen)
{
	int i, n;
	char *match;
	if (!(url = os_strstr(url, "/api/"))) {
		INFO("Bad url %s", url);
		os_sprintf(response, "{\"status\":\"bad url\"}");
		return;
	}
	for (i = 0; builtInUrls[i].url; i++) {
		if (!(match = os_strstr(builtInUrls[i].url, "/api/")))
			continue;
		n = os_strlen(match);

		if (match[n-1] == '*')
		--n;
		if (os_strncmp(match, url, n))
			continue;
		// Match found
		break;
	}
	if (!builtInUrls[i].url) {
		INFO("Not found %s", url);
		os_sprintf(response, "{\"status\":\"not found\"}");
		return;
	}
	INFO("Found match %s == %s", url, builtInUrls[i].url);
	struct HttpdConnData *connData = NULL;
	struct HttpdPostData *postData = NULL;
	if (!(connData = os_zalloc(sizeof(struct HttpdConnData)))) {
		ERROR("Mem error");
		os_sprintf(response, "{\"status\":\"mem error\"}");
		goto err;
	}
	if (postBuf && !(postData = os_zalloc(sizeof(struct HttpdPostData)))) {
		ERROR("Mem error");
		os_sprintf(response, "{\"status\":\"mem error\"}");
		goto err;
	}
	pbLen = fixPostData((char *)postBuf, pbLen);
	if (!os_strcmp(method, "GET"))
		connData->requestType = HTTPD_METHOD_GET;
	else if (!os_strcmp(method, "PUT"))
		connData->requestType = HTTPD_METHOD_PUT;
	else if (!os_strcmp(method, "POST"))
		connData->requestType = HTTPD_METHOD_POST;
	else {
		ERROR("Info unknown http method %s", method);
				os_sprintf(response, "{\"status\":\"bad url\"}");
		os_sprintf(response, "{\"status\":\"bad method\"}");
		goto err;
	}
	os_strcpy(response, "");
	connData->url = (char *)url;
	connData->priv = (void *)response;
	connData->post = postData;
	if (pbLen) {
		postData->len =
				postData->buffSize =
				postData->buffLen =
				postData->received = pbLen;
		postData->buff = (char *)postBuf;
	}
	INFO("Calling handler for %d:%s", i, connData->url);
	while (builtInUrls[i].cgiCb(connData) != HTTPD_CGI_DONE)
		;
err:
    if (connData)
    	os_free(connData);
    if (postData)
    	os_free(postData);
}

static void ICACHE_FLASH_ATTR substMacAddr(const char *url, uint8 *mac)
{
	os_sprintf((char *)url, "/%02x-%02x-%02x-%02x-%02x-%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
	((char *)url)[strlen(STA_MAC_ADDR_MARKER)] = '/';
}

void ICACHE_FLASH_ATTR routesInit(void)
{
	int i;
	uint8 staMac[6];
	wifi_get_macaddr(STATION_IF, staMac);
	for (i = 0; builtInUrls[i].url; i++) {
		if (strstr(builtInUrls[i].url, STA_MAC_ADDR_MARKER) == builtInUrls[i].url)
			substMacAddr(builtInUrls[i].url, staMac);
	}
	httpdInit(builtInUrls, 80);

}