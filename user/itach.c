#include <esp8266.h>

#include "rest_utils.h"

#include "console.h"
#include "ir.h"
#include "itach.h"

int ICACHE_FLASH_ATTR itachConfig(HttpdConnData *connData) 
{
	uint8 mac[6];
	if (connData->url==NULL) {
		//Connection aborted. Clean up.
		return HTTPD_CGI_DONE;
	}
	wifi_get_macaddr(STATION_IF, mac);
	httpdStartResponse(connData, 200);
	httpdHeader(connData, "Content-Type", "text/html; charset=utf-8");
	httpdEndHeaders(connData);
	HTTPD_PRINTF("<name=\"MAC\" size=\"18\" "
			"value=\"%02X:%02X:%02X:%02X:%02X:%02X\">\r\n",
			mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
	return HTTPD_CGI_DONE;
}

static ETSTimer beaconTimer;

static char beaconMsg[] = "AMXB<-UUID=GlobalCache_MMMMMMMMMMMM>"
	"<-SDKClass=Utility><-Make=zmote.io><-Model=zmote>"
	"<-Revision=" ZMOTE_FIRMWARE_VERSION "><-Pkg_Level=ZMPK001><-PCB_PN=zmotev1>"
	"<-Config-URL=http://IIIIIII         ><-Status=Ready>";
static char postBeacon[] = "><-Status=Ready>";

static struct espconn *udpConn = NULL;
static struct espconn *tcpConn = NULL;
static char reply[128];

#define MATCH_AT_START(x, str) (os_strncmp(x, str, os_strlen(str)) == 0)
static void ICACHE_FLASH_ATTR tcpRecvCb(void *arg, char *data, unsigned short len) 
{
	struct espconn *conn = arg;
	INFO("TCP receive: >>%s<<", data);
	if (MATCH_AT_START(data, "getdevices")) {
		os_strcpy(reply, "device,0,0 WIFI\rdevice,1,3 IR\rendlistdevices\r");
	} else if (MATCH_AT_START(data, "getversion,")) {
		os_strcpy(reply, data + 3);
		os_strcpy(reply + strlen(reply), "," ZMOTE_FIRMWARE_VERSION "\r");
	} else if (MATCH_AT_START(data, "getversion")) {
		os_strcpy(reply, ZMOTE_FIRMWARE_VERSION "\r");
	} else if (MATCH_AT_START(data, "get_IR,")) {
		os_strcpy(reply, data + 4);
		os_strcpy(reply + strlen(reply), ",IR_BLASTER\r");
	} else if (MATCH_AT_START(data, "sendir,")) {
		// The reply should hold everything between the first and third commas
		// Example input: "sendir,1:2,49,38028,1,1,172,171,..."
		// Output: "completeir,1:2,49\r"
		char *p, *q = NULL; 
		p = strchr(data, ',');
		if (p) q = strchr(p+1, ',');
		if (q) q = strchr(q+1, ',');
		if (!p || !q)
			goto err;
		INFO("p=\"%s\" q=\"%s\"", p, q);
		*q = 0;
		os_sprintf(reply, "completeir%s\r", p);
		if (irSend(q+1) <= 0)
			os_sprintf(reply, "busyIR%s\r", p);
	} else if (MATCH_AT_START(data, "stopir")) {
		irSendStop();
		os_sprintf(reply, "%s\r", data);
	} else if (MATCH_AT_START(data, "get_IRL")) {
		if (irLearn(conn) <= 0)
			os_sprintf(reply, "busyIR,1:1,0\r"); // FIXME
		else
			os_sprintf(reply, "IR Learner Enabled\r");
	} else if (MATCH_AT_START(data, "stop_IRL")) {
		irLearnStop(conn);
		os_sprintf(reply, "IR Learner Disabled\r");
	} else {
err:
		os_strcpy(reply, "unknowncommand,ERR_01\r");
	}
	INFO("Reply: >>\n%s\n<<", reply);
	espconn_send(conn, (uint8 *)reply, strlen(reply));
}
static void ICACHE_FLASH_ATTR tcpReconCb(void *arg, sint8 err) 
{
	ERROR("recon? %d", err);
}
static void ICACHE_FLASH_ATTR tcpDisconCb(void *arg) 
{
	INFO("Disconnected");
}
static void ICACHE_FLASH_ATTR tcpSentCb(void *arg) 
{
	INFO("Finished sending");
}
static void ICACHE_FLASH_ATTR tcpConnectCB(void *arg) 
{
        struct espconn *conn = arg;
        espconn_regist_recvcb(conn, tcpRecvCb);
        espconn_regist_reconcb(conn, tcpReconCb);
        espconn_regist_disconcb(conn, tcpDisconCb);
        espconn_regist_sentcb(conn, tcpSentCb);
}
static void ICACHE_FLASH_ATTR udpsentCB(void *arg)
{
	INFO("UDP send finished");
}
static void ICACHE_FLASH_ATTR udprecvCB(void *arg, char *data, unsigned short len)
{
	INFO("recived %d bytes over UDP.  Huh?", len);
}
static void ICACHE_FLASH_ATTR initConn(void)
{
	if (!udpConn) {
		if (!(udpConn = os_zalloc(sizeof(struct espconn))))
				goto err;
		if (!(udpConn->proto.udp = os_zalloc(sizeof(esp_udp))))
				goto err;
	} else {
		void *udp = udpConn->proto.udp;
		os_memset(udpConn, 0, sizeof(struct espconn));
		os_memset(udp, 0, sizeof(esp_udp));
		udpConn->proto.udp = udp;
	}

	udpConn->type = ESPCONN_UDP;
	udpConn->state = ESPCONN_NONE;
	udpConn->proto.udp->local_port = espconn_port();
	udpConn->proto.udp->remote_port = 9131;
	udpConn->proto.udp->remote_ip[0] = 239;
	udpConn->proto.udp->remote_ip[1] = 255;
	udpConn->proto.udp->remote_ip[2] = 250;
	udpConn->proto.udp->remote_ip[3] = 250;	
	if (espconn_regist_sentcb(udpConn, udpsentCB)) {
		ERROR("Can't register sent CB");
		goto err;
	}
	if (espconn_regist_recvcb(udpConn, udprecvCB)) {
		ERROR("Can't register sent CB");
		goto err;
	}
	if (espconn_create(udpConn)) {
		ERROR("Create error");
		goto err;
	}
	if (!tcpConn) {
		if (!(tcpConn = os_zalloc(sizeof(struct espconn))))
				goto err;
		if (!(tcpConn->proto.tcp = os_zalloc(sizeof(esp_tcp))))
				goto err;
	} else {
		void *tcp = tcpConn->proto.tcp;
		os_memset(tcpConn, 0, sizeof(struct espconn));
		os_memset(tcp, 0, sizeof(esp_tcp));
		tcpConn->proto.tcp = tcp;
	}
	tcpConn->type = ESPCONN_TCP;
	tcpConn->state = ESPCONN_NONE;
	tcpConn->proto.tcp->local_port = 4998;
	espconn_regist_connectcb(tcpConn, tcpConnectCB);
	espconn_accept(tcpConn);
	return;
err:
	ERROR("Memmory error");
	if (udpConn) {
		if (udpConn->proto.udp)
			os_free(udpConn->proto.udp);
		os_free(udpConn);
		udpConn	= NULL;
	}
	if (tcpConn) {
		if (tcpConn->proto.tcp)
			os_free(tcpConn->proto.tcp);
		os_free(tcpConn);
		tcpConn = NULL;
	}
}
static void beaconTimerCb()
{
	INFO("Beacon msg=%s", beaconMsg);
	if (!udpConn) {
		ERROR("udpConn not initialized");
		return;
	}
	if (espconn_send(udpConn, (uint8 *)beaconMsg, strlen(beaconMsg)))
		ERROR("Error sending beaconMessage");

}
void ICACHE_FLASH_ATTR itachInit(void)
{
	uint8 mac[6];
	char hostname[32], macstr[16], ipstr[16];
	struct ip_info ipconfig;

	wifi_get_ip_info(STATION_IF, &ipconfig);
	wifi_get_macaddr(STATION_IF, mac);

	os_sprintf(macstr, "%02X%02X%02X%02X%02X%02X", mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
	os_sprintf(hostname, "GlobalCache_%s", macstr);
	INFO("hostname = %s", hostname);
	if (!wifi_station_set_hostname(hostname)) 
		ERROR("Unable to set hostname");
	os_strncpy(strstr(beaconMsg, "MMMMMMMMMMMM"), macstr, 12);

	os_sprintf(ipstr, "%d.%d.%d.%d", IP2STR(&ipconfig.ip));
	os_strncpy(strstr(beaconMsg, "IIIIIII"), ipstr, strlen(ipstr)+1);
	os_strcpy(beaconMsg + strlen(beaconMsg), postBeacon);

	INFO("Beacon msg=%s", beaconMsg);
	initConn();
	os_timer_disarm(&beaconTimer);
	os_timer_setfn(&beaconTimer, beaconTimerCb, NULL);
	os_timer_arm(&beaconTimer, 10000, 1);

}
