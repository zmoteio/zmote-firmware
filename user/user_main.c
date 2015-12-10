/*
 * ----------------------------------------------------------------------------
 * "THE BEER-WARE LICENSE" (Revision 42):
 * Jeroen Domburg <jeroen@spritesmods.com> wrote this file. As long as you retain 
 * this notice you can do whatever you want with this stuff. If we meet some day, 
 * and you think this stuff is worth it, you can buy me a beer in return. 
 * ----------------------------------------------------------------------------
 */

/*
This is example code for the esphttpd library. It's a small-ish demo showing off 
the server, including WiFi connection management capabilities, some IO and
some pictures of cats.
*/

#include <esp8266.h>
#include "ir.h"
#include "wifictrl.h"
#include "mqttclient.h"
#include "stled.h"
#include "zmote_config.h"
#include "routes.h"
#include "stdout.h"
#include "captdns.h"
#include "console.h"
#include "rboot-ota.h"



#ifndef ENABLE_UART_DEBUG
	// No point to this when UART debug is disabled
#	undef SHOW_HEAP_USE 
#endif

#ifdef SHOW_HEAP_USE

static ETSTimer heapUseReportTimer;

static void ICACHE_FLASH_ATTR heapUseReportTimerCb() 
{
	char *flashMap[] = {
		[FLASH_SIZE_4M_MAP_256_256] "4M_MAP_256_256",
		[FLASH_SIZE_2M] "2M",
		[FLASH_SIZE_8M_MAP_512_512] "8M_MAP_512_512",
		[FLASH_SIZE_16M_MAP_512_512] "16M_MAP_512_512",
		[FLASH_SIZE_32M_MAP_512_512] "32M_MAP_512_512",
		[FLASH_SIZE_16M_MAP_1024_1024] "16M_MAP_1024_1024",
		[FLASH_SIZE_32M_MAP_1024_1024] "32M_MAP_1024_1024"
	};
	INFO("--------------------------------------------");
    INFO("SDK: v%s", system_get_sdk_version());
    INFO("Free Heap: %d", system_get_free_heap_size());
    INFO("CPU Frequency: %d MHz", system_get_cpu_freq());
    INFO("System Chip ID: 0x%x", system_get_chip_id());
    INFO("SPI Flash ID: 0x%x", spi_flash_get_id());
    INFO("SPI Flash Size: %d", (1 << ((spi_flash_get_id() >> 16) & 0xff)));
    INFO("rBoot Slot: %d", rboot_get_current_rom());
    INFO("Flash Size Map: %d=>%s", system_get_flash_size_map(), flashMap[system_get_flash_size_map()]);
	INFO("zMote version: " ZMOTE_FIRMWARE_VERSION);
	INFO("zMote commit: " ZMOTE_FIRMWARE_COMMIT);
	INFO("zMote build: " ZMOTE_FIRMWARE_BUILD "("__DATE__ ")");
	struct softap_config ap_config;	
	wifi_softap_get_config(&ap_config);
	INFO("AP SSID : %s", (char *)ap_config.ssid);
	char temp[32] = "<NOT SET>";
	cfgGet("ssidAP", temp, sizeof(temp));
	INFO("CFG AP SSID : %s", temp);

	struct ip_info ipconfig;
	wifi_get_ip_info(STATION_IF, &ipconfig);
	if (wifi_station_get_connect_status() == STATION_GOT_IP && ipconfig.ip.addr != 0) {
		INFO("ip: %d.%d.%d.%d, mask: %d.%d.%d.%d, gw: %d.%d.%d.%d",
			IP2STR(&ipconfig.ip), IP2STR(&ipconfig.netmask), IP2STR(&ipconfig.gw));
	} else {
		INFO("network status: %d", wifi_station_get_connect_status());
	}
	INFO("--------------------------------------------");

}

#endif

static void ICACHE_FLASH_ATTR writeConfigVars(void)
{
	char temp[64],temp2[64];
	uint8 mac[6];
	struct softap_config ap_config;
	bool doInit = false;

	// Protection to disable simple flash copy
	if (cfgGet("chipID", temp, sizeof(temp))) {
		os_sprintf(temp2, "%08x", system_get_chip_id());
		if (os_strcmp(temp, temp2)) {
			ERROR("Bad flash. Hang");
			while (1)
				;
		}
	}
	

	// Init happens on first flashing and after each OTA update
	// We use commit ID to check for difference in version
	if (!cfgGet("flashID", temp, sizeof(temp)))
		doInit = true;
	else if (!cfgGet("commit", temp, sizeof(temp)))
		doInit = true;
	else if (os_strcmp(temp, ZMOTE_FIRMWARE_COMMIT))
		doInit = true;
	if (doInit) {
		os_sprintf(temp, "%08x", system_get_chip_id());
		cfgSet("chipID", temp);
		cfgSet("version", ZMOTE_FIRMWARE_VERSION);
		cfgSet("commit", ZMOTE_FIRMWARE_COMMIT);
		cfgSet("build", ZMOTE_FIRMWARE_BUILD "("__DATE__ ")");
		os_sprintf(temp, "%08x", spi_flash_get_id());
		cfgSet("flashID", temp);
		wifi_get_macaddr(STATION_IF, mac);
		os_sprintf(temp, MACSTR, MAC2STR(mac));
		cfgSet("mac", temp);
		wifi_get_macaddr(SOFTAP_IF, mac);
		os_sprintf(temp, MACSTR, MAC2STR(mac));
		cfgSet("macAP", temp);

	}
	// This happens only once in the life of the product at the factory
	if (!cfgGet("ssidAP", temp, sizeof(temp))) {
		wifi_get_macaddr(STATION_IF, mac);
		os_sprintf(temp, "zmote_%02x%02x%02x", mac[3], mac[4], mac[5]);
		cfgSet("ssidAP", temp);
	}
	wifi_softap_get_config(&ap_config);
	cfgGet("ssidAP", temp, sizeof(temp));
	if (os_strcmp((char *)ap_config.ssid, temp)) {
		os_memset(ap_config.ssid, 0, 32);
	   	os_memset(ap_config.password, 0, 64);
   		os_strcpy((char *)ap_config.ssid, temp);
	   	os_memcpy(ap_config.password, "", 1);
	   	ap_config.authmode = AUTH_OPEN;
	   	ap_config.ssid_len = 0;// or its actual length
	   	ap_config.max_connection = 4; // how many stations can connect to ESP8266 softAP at most.
		wifi_softap_set_config(&ap_config);
	}
}

void user_init(void) 
{
	system_timer_reinit();
	stdoutInit();
	cfgInit();
	writeConfigVars();
	
	captdnsInit();
	irInit();
	wifiInit();
	stledInit();
	routesInit();
	wifiInit();

#ifdef SHOW_HEAP_USE
	os_timer_disarm(&heapUseReportTimer);
	os_timer_setfn(&heapUseReportTimer, heapUseReportTimerCb, NULL);
	os_timer_arm(&heapUseReportTimer, 60000, 1);
#endif
	INFO("\nReady\n");
}

void user_rf_pre_init() 
{
	//Not needed, but newer SDK versions want this defined.
}

