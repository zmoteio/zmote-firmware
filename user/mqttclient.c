#include <esp8266.h>
#include "mqtt.h"
#include "mqttclient.h"
#include "zmote_config.h"
#include "jsmn.h"
#include "console.h"
#include "rboot-ota.h"
#include "rest_utils.h"
#include "routes.h"
#include "updatefs.h"

static MQTT_Client mqttClient;
static enum {
    INIT,
    DISCONNECTED,
    CONNECTED
} mqttState = INIT;
static char pubTopic[48], subTopic[48], idMessage[128];


#define OTA_HTTP_HEADER                                             \
        "Connection: keep-alive\r\n"                                \
        "Cache-Control: no-cache\r\n"                               \
        "User-Agent: zMote/" ZMOTE_FIRMWARE_VERSION "\r\n"          \
        "Accept: */*\r\n\r\n"

#define MSG_GOODBYE "{\"goodbye\":true}"
#define MSG_DISCONNECT "{\"disconnected\":true}"

static void ICACHE_FLASH_ATTR finishOTA(void *arg, bool result) 
{
    rboot_ota *ota = (rboot_ota*)arg;
    
    if(result == true) {
        // success, reboot
        INFO("Firmware updated, (rom %d)", ota->rom_slot);
        rboot_set_current_rom(ota->rom_slot);
        system_restart();
    } else {
        // fail, cleanup
        ERROR("Firmware update failed!");
        os_free(ota->request);
        os_free(ota);
        MQTT_Publish(&mqttClient, pubTopic, idMessage, strlen(idMessage), 1, 1);
    }
}
static void ICACHE_FLASH_ATTR startOTA(const char *json, jsmntok_t *toks, int ntoks) 
{    
    rboot_ota *ota  = NULL;
    char ipStr[16] = "", fwPath[64] = "", portStr[8] = "", *p;
    int i, j;

    // create the update structure
    if (!(ota = os_zalloc(sizeof(rboot_ota)))) {
        ERROR("Out of memory");
        return;
    }
    if (!(ota->request = os_zalloc(512))) {
        os_free(ota);
        ERROR("Out of memory");
        return;
    }
    ota->callback = (ota_callback)finishOTA;
    ota->rom_slot = 1 - rboot_get_current_rom();
    for (i = 1; i < ntoks; i += 1 + toks[i].size) {
        if (jsonEq(json, &toks[i], "ip")) {
            jsonStr(json, &toks[i+1], ipStr);
            p = ipStr;
            for (j = 0; j < 4; j++) {
                ota->ip[j] = atoi(p);
                p = strchr(p, '.');
                if (!p && j != 3) {
                    ERROR("Malformed IP address %s", ipStr);
                    goto err;
                }
                p++; // skip past the '.'
            }
            DEBUG("OTA IP: " IPSTR, IP2STR(ota->ip));
        } else if (jsonEq(json, &toks[i], "port")) {
            ota->port = atoi(jsonStr(json, &toks[i+1], portStr));
            DEBUG("OTA Port: %d", ota->port);
        } else if (jsonEq(json, &toks[i], ota->rom_slot?"rom1":"rom0")) {
            jsonStr(json, &toks[i+1], fwPath);
            DEBUG("FW Path: %s", fwPath);
        }
    }
    if (!os_strlen(ipStr) || !os_strlen(portStr) || !os_strlen(fwPath)) {
        ERROR("Badly formed update command");
        goto err;
    }
    os_sprintf((char*)ota->request,
        "GET %s HTTP/1.1\r\nHost: "IPSTR"\r\n" OTA_HTTP_HEADER,
        fwPath,
        IP2STR(ota->ip));
    INFO("HTTP request:\n%s", (char *)ota->request);
    // start the upgrade process    
    if (rboot_ota_start(ota)) {
        MQTT_Publish(&mqttClient, pubTopic, MSG_GOODBYE, strlen(MSG_GOODBYE), 1, 1);
        INFO("FIRMWARE Updating...");
        return;
    }
    ERROR("Error starting OTA");
err:
    os_free(ota->request);
    os_free(ota);
}

static void ICACHE_FLASH_ATTR cgiBridge(const char *json, jsmntok_t *toks, int ntoks)
{
    char url[32] = "", method[8] = "", id[32] = "";
    const char *postdata = NULL;
    char *response, *mqttPkt = NULL;
    int i, pdLen = 0;

    for (i = 1; i < ntoks; i += 1 + toks[i].size) {
        if (jsonEq(json, &toks[i], "command")) {
            jsonStr(json, &toks[i+1], method);
        } else if (jsonEq(json, &toks[i], "url")) {
            jsonStr(json, &toks[i+1], url);
        } else if (jsonEq(json, &toks[i], "id")) {
            jsonStr(json, &toks[i+1], id);
        } else if (jsonEq(json, &toks[i], "postdata")) {
            postdata = &json[toks[i+1].start];
            pdLen = toks[i+1].end - toks[i+1].start;
        } else {
            ERROR("malformed command");
            goto err;
        }
    }
    if (!os_strlen(url) || !os_strlen(method) || !os_strlen(id)) {
        ERROR("malformed command");
        goto err;
    }
    if (!(mqttPkt = os_malloc(1024))) { // FIXME arbitrary size
        ERROR("mem error");
        goto err;
    }
    INFO("bridge url=%s method=%s id=%s pdLen=%d", url, method, id, pdLen);
    os_sprintf(mqttPkt, "{\"id\":\"%s\",\"response\":", id);
    response = &mqttPkt[os_strlen(mqttPkt)];
    execRoute(url, method, postdata, pdLen, response, 1024);
    os_strcpy(&response[os_strlen(response)], "}");
    DEBUG("mqttResp=%s", mqttPkt);
    mqttPub(mqttPkt);
err:
    if (mqttPkt)
        os_free(mqttPkt);
}

static void ICACHE_FLASH_ATTR mqttConnectedCb(uint32_t *args)
{
    MQTT_Client* client = (MQTT_Client*)args;
    INFO("MQTT: Connected.");
    INFO("MQTT: Connected. Subscribe to: %s", subTopic);
    INFO("MQTT: Connected. Publish to: %s (%s)", pubTopic, idMessage);
    MQTT_Subscribe(client, subTopic, 1);
    MQTT_Publish(client, pubTopic, idMessage, strlen(idMessage), 1, 1);
    mqttState = CONNECTED;
}

static void ICACHE_FLASH_ATTR mqttDisconnectedCb(uint32_t *args)
{
    //MQTT_Client* client = (MQTT_Client*)args;
    INFO("MQTT: Disconnected");
    mqttState = DISCONNECTED;
}

static void ICACHE_FLASH_ATTR mqttPublishedCb(uint32_t *args)
{
    //MQTT_Client* client = (MQTT_Client*)args;
    INFO("MQTT: Published");
}


static void ICACHE_FLASH_ATTR updatefs_cb(void *arg, int status)
{
    os_free(arg);
    if (status)
        ERROR("Error doing fs update");
    else
        INFO("FS Update completed successfully");
}
static void ICACHE_FLASH_ATTR mqttDataCb(uint32_t *args, const char* topic, uint32_t topic_len, const char *data, uint32_t data_len)
{
    jsmn_parser p;
    jsmntok_t *toks;
    char *dcopy = NULL;
    int i, ntoks;

    INFO("Receive topic: %20s, data: %s", topic, data);
    jsmn_init(&p);
    if ((ntoks = jsmn_parse(&p, data, data_len, NULL, 0)) <= 0) {
        ERROR("json parse error %d", ntoks);
        return;
    }
    if (!(toks = os_malloc(ntoks*sizeof(toks[0])))) {
        ERROR("no memory for %d tokens", ntoks);
        return;
    }
    jsmn_init(&p);
    ntoks = jsmn_parse(&p, data, data_len, toks, ntoks);
    if (ntoks < 3 || toks[0].type != JSMN_OBJECT) {
        ERROR("Garbled message");
        goto err;
    }
    for (i = 1; i < ntoks; i += 1 + toks[i].size) {
        if (!jsonEq(data, &toks[i], "command"))
            continue;
        if (jsonEq(data, &toks[i+1], "update")) {
            startOTA(data, toks, ntoks);
            break;
        } else if (jsonEq(data, &toks[i+1], "updatefs")) {
            if (!(dcopy = os_malloc(data_len)))
                goto err;
            os_memcpy(dcopy, data, data_len);
            if (updatefs(dcopy, toks, ntoks, updatefs_cb, dcopy))
                goto err;
            break;
        } else if (jsonEq(data, &toks[i+1], "GET") || jsonEq(data, &toks[i+1], "PUT") || jsonEq(data, &toks[i+1], "POST")) {
            cgiBridge(data, toks, ntoks);
            break;
        } else {
            #ifdef ENABLE_UART_DEBUG
            char temp[32];
            #endif
            ERROR("Unknown mqtt command: %s", jsonStr(data, &toks[i+1], temp));
            goto err;
        }
    }
    os_free(toks);
    return;
err:
    if (dcopy)
        os_free(dcopy);
    if (toks)
        os_free(toks);
    return;
}

int ICACHE_FLASH_ATTR mqttPub(const char *msg)
{
    if (mqttState != CONNECTED)
        return 0;
    MQTT_Publish(&mqttClient, pubTopic, msg, strlen(msg), 1, 0);
    return 1;
}
int ICACHE_FLASH_ATTR mqttConnected(void)
{
    return (mqttState == CONNECTED);
}
#define BAD_CFG  do { \
    DEBUG("bad cfg"); \
    DEBUG("mqtt=%s:%d keepalive=%d", mqtt_server, mqtt_port, mqtt_keepalive); \
    DEBUG("auth=%s:%s", serial, secret);\
    goto err;\
} while (0)

static char serial[32], chipID[9], secret[65], mqtt_server[64];
static int ICACHE_FLASH_ATTR mqttMkHelloMsg(void)
{
    char temp[32];
    uint8 mac[6];
    struct ip_info ipconfig;

    INFO("mqtt helloMsg");
    wifi_get_ip_info(STATION_IF, &ipconfig);

    os_sprintf(chipID, "%08x", system_get_chip_id());
    wifi_get_macaddr(STATION_IF, mac);

    os_sprintf(subTopic, "zmote/towidget/%s", chipID);
    os_sprintf(pubTopic, "zmote/widget/%s", chipID);
    os_sprintf(idMessage, "{\"ts\":%d,\"version\":\"" ZMOTE_FIRMWARE_VERSION "\","
        "\"fs_version\":\"%s\",\"chipID\":\"%s\",\"ip\":\"" IPSTR "\"}", 
            system_get_time(), cfgGet("fs_version", temp, sizeof(temp)),
            chipID, IP2STR(&ipconfig.ip));
    return 1;

}
void ICACHE_FLASH_ATTR mqttHello(void)
{
    if (mqttState != CONNECTED)
        return;
    MQTT_Publish(&mqttClient, pubTopic, idMessage, strlen(idMessage), 1, 1);
}

void ICACHE_FLASH_ATTR mqttInit(void)
{
    int mqtt_port = 0, mqtt_keepalive = 0;
    char temp[32];

    // serial/secret to auth; send mac, chip id, local ip as second packet
    // mqtt_server mqtt_port
	if (mqttState == INIT) {
        if (mqttMkHelloMsg())
            goto err;
        INFO("mqtt init");

        if (!cfgGet("serial", serial, sizeof(serial)))
            BAD_CFG; // Not properly configured yet
        if (!cfgGet("secret", secret, sizeof(secret)))
            BAD_CFG; // Not properly configured yet
        if (!cfgGet("mqtt_server", mqtt_server, sizeof(mqtt_server)))
            BAD_CFG; // Not properly configured yet
        if (!cfgGet("mqtt_keepalive", temp, sizeof(temp)))
            BAD_CFG; // Not properly configured yet
        mqtt_keepalive = atoi(temp);
        if (!cfgGet("mqtt_port", temp, sizeof(temp)))
            BAD_CFG; // Not properly configured yet
        mqtt_port = atoi(temp);
        DEBUG("serial=\"%s\"", serial);
        DEBUG("secret=\"%s\"", secret);
        DEBUG("mqtt_server=\"%s\"", mqtt_server);
        DEBUG("mqtt_port=%d", mqtt_port);
        DEBUG("mqtt_keepalive=%d", mqtt_keepalive);
        //MQTT_InitConnection(&mqttClient, sysCfg.mqtt_host, sysCfg.mqtt_port, sysCfg.security);
        MQTT_InitConnection(&mqttClient, (uint8 *)mqtt_server, mqtt_port, 0);

	    //MQTT_InitClient(&mqttClient, sysCfg.device_id, sysCfg.mqtt_user, sysCfg.mqtt_pass, sysCfg.mqtt_keepalive, cleanSession);
        MQTT_InitClient(&mqttClient, (uint8 *)chipID, (uint8 *)serial, (uint8 *)secret, mqtt_keepalive, 1);

	    MQTT_InitLWT(&mqttClient, (uint8 *)pubTopic, (uint8 *)MSG_DISCONNECT, 1, 1);
	    MQTT_OnConnected(&mqttClient, mqttConnectedCb);
	    MQTT_OnDisconnected(&mqttClient, mqttDisconnectedCb);
	    MQTT_OnPublished(&mqttClient, mqttPublishedCb);
	    MQTT_OnData(&mqttClient, mqttDataCb);
	} else
		MQTT_Disconnect(&mqttClient);
	MQTT_Connect(&mqttClient);
	mqttState = DISCONNECTED;
    return;
err:
    MQTT_Disconnect(&mqttClient);
    mqttState = DISCONNECTED;    
    return;
}