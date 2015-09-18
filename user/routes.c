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
static HttpdBuiltInUrl builtInUrls[] = {
	{"/", cgiRedirectApClientToHostname, "zmote.io"},
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

	{"*", cfgFile, NULL}, //Catch-all cgi function for the filesystem
	{NULL, NULL, NULL}
};

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
	INFO("Calling handler for %s", connData->url);
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