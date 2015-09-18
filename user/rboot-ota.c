//////////////////////////////////////////////////
// API for OTA and rBoot config, for ESP8266.
// Copyright 2015 Richard A Burton
// richardaburton@gmail.com
// See license.txt for license terms.
// OTA code based on SDK sample from Espressif.
//////////////////////////////////////////////////

//#include <c_types.h>
//#include <user_interface.h>
//#include <espconn.h>
//#include <mem.h>
//#include <osapi.h>

#include <esp8266.h>
#include "rboot-ota.h"

// structure to hold our internal update state
typedef struct {
	uint32 start_addr;
	uint32 start_sector;
	uint32 max_sector_count;
	uint32 last_sector_erased;
	uint8 extra_count;
	uint8 extra_bytes[4];
	rboot_ota *ota;
	uint32 total_len;
	uint32 content_len;
	struct espconn *conn;
} upgrade_param;

static upgrade_param *upgrade;
static os_timer_t ota_timer;

// get current boot rom
uint8 ICACHE_FLASH_ATTR rboot_get_current_rom() {
	if ((uint32)rboot_get_current_rom & 0x80000)
		return 1;
	return 0;
}

// set current boot rom
bool ICACHE_FLASH_ATTR rboot_set_current_rom(uint8 rom) {
	uint32 flags = 0;
	if (SPI_FLASH_RESULT_OK != rps_get_flags(&flags, ZMOTE_CFG_ADDR)) {
		ERROR("Error reading RPS flags");
		return false;
	}
	flags = (flags & ~0x01) | rom;
	if (SPI_FLASH_RESULT_OK != rps_set_flags(flags, ZMOTE_CFG_ADDR)) {
		ERROR("Error writing RPS flags");
		return false;
	}
	return true;
}

// function to do the actual writing to flash
static bool ICACHE_FLASH_ATTR write_flash(uint8 *data, uint16 len) {
	
	bool ret = false;
	uint8 *buffer;
	
	if (data == NULL || len == 0) {
		return true;
	}
	
	// get a buffer
	buffer = (uint8 *)os_zalloc(len + upgrade->extra_count);

	// copy in any remaining bytes from last chunk
	os_memcpy(buffer, upgrade->extra_bytes, upgrade->extra_count);
	// copy in new data
	os_memcpy(buffer + upgrade->extra_count, data, len);

	// calculate length, must be multiple of 4
	// save any remaining bytes for next go
	len += upgrade->extra_count;
	upgrade->extra_count = len % 4;
	len -= upgrade->extra_count;
	os_memcpy(upgrade->extra_bytes, buffer + len, upgrade->extra_count);

	// check data will fit
	//if (upgrade->start_addr + len < (upgrade->start_sector + upgrade->max_sector_count) * SECTOR_SIZE) {

		if (len > SECTOR_SIZE) {
			// here we should erase current (if not already done), next
			// and possibly later sectors too, but doesn't look like we
			// actually ever get more than 4k at a time though
		} else {
			// check if sector the write finishes in has been erased yet,
			// this is fine as long as data len < sector size
			if (upgrade->last_sector_erased != (upgrade->start_addr + len) / SECTOR_SIZE) {
				upgrade->last_sector_erased = (upgrade->start_addr + len) / SECTOR_SIZE;
				spi_flash_erase_sector(upgrade->last_sector_erased);
			}
		}

		// write current chunk
		if (spi_flash_write(upgrade->start_addr, (uint32 *)buffer, len) == SPI_FLASH_RESULT_OK) {
			ret = true;
			upgrade->start_addr += len;
		}
	//}

	os_free(buffer);
	return ret;
}

// initialise the internal update state structure
static bool ICACHE_FLASH_ATTR rboot_ota_init(rboot_ota *ota) {

	upgrade = (upgrade_param*)os_zalloc(sizeof(upgrade_param));
	if (!upgrade) {
		ERROR("No ram!");
		return false;
	}
	
	// store user update options
	upgrade->ota = ota;
	
	// details of rom slot to update
	ota->rom_addr = (1 - rboot_get_current_rom()) * 0x80000 + 0x2000;
	upgrade->start_addr = ota->rom_addr;
	upgrade->start_sector = upgrade->start_addr / SECTOR_SIZE;
	
	// create connection
	upgrade->conn = (struct espconn *)os_zalloc(sizeof(struct espconn));
	if (!upgrade->conn) {
		ERROR("No ram!");
		os_free(upgrade);
		return false;
	}
	upgrade->conn->proto.tcp = (esp_tcp *)os_zalloc(sizeof(esp_tcp));
	if (!upgrade->conn->proto.tcp) {
		os_free(upgrade->conn);
		upgrade->conn = 0;
		ERROR("No ram!");
		os_free(upgrade);
		return false;
	}
	
	// set update flag
	system_upgrade_flag_set(UPGRADE_FLAG_START);
	
	return true;
}

// clean up at the end of the update
// will call the user call back to indicate completion
static void ICACHE_FLASH_ATTR rboot_ota_deinit() {
	
	bool result;
	rboot_ota *ota;
	struct espconn *conn;

	DEBUG("possible timeout or other error");
	os_timer_disarm(&ota_timer);
	
	// save only remaining bits of interest from upgrade struct
	// then we can clean it up early, so disconnect callback
	// can distinguish between us calling it after update finished
	// or being called earlier in the update process
	ota = upgrade->ota;
	conn = upgrade->conn;
	
	// clean up
	os_free(upgrade);
	upgrade = 0;
	
	// if connected, disconnect and clean up connection
	if (conn) espconn_disconnect(conn);
	
	// check for completion
	if (system_upgrade_flag_check() == UPGRADE_FLAG_FINISH) {
		result = true;
	} else {
		system_upgrade_flag_set(UPGRADE_FLAG_IDLE);
		result = false;
	}
	
	// call user call back
	if (ota->callback) {
		ota->callback(ota, result);
	}
	
}

// called when connection receives data (hopefully the rom)
static void ICACHE_FLASH_ATTR upgrade_recvcb(void *arg, char *pusrdata, unsigned short length) {
	
	char *ptrData, *ptrLen, *ptr;
	
	// first reply?
	if (upgrade->content_len == 0) {
		// valid http response?
		if ((ptrLen = (char*)strcasestr(pusrdata, "Content-Length: "))
			&& (ptrData = (char*)os_strstr(ptrLen, "\r\n\r\n"))
			&& (os_strncmp(pusrdata + 9, "200", 3) == 0)) {
			
			// end of header/start of data
			ptrData += 4;
			// length of data after header in this chunk
			length -= (ptrData - pusrdata);
			// running total of download length
			upgrade->total_len += length;
			// process current chunk
			write_flash((uint8*)ptrData, length);
			// work out total download size
			ptrLen += 16;
			ptr = (char *)os_strstr(ptrLen, "\r\n");
			*ptr = '\0'; // destructive
			upgrade->content_len = atoi(ptrLen);
		} else {
			ERROR("fail, not a valid http header/non-200 response/etc. %s", pusrdata);
			rboot_ota_deinit();
			return;
		}
	} else {
		// not the first chunk, process it
		upgrade->total_len += length;
		write_flash((uint8*)pusrdata, length);
	}
	
	// check if we are finished
	if (upgrade->total_len == upgrade->content_len) {
		system_upgrade_flag_set(UPGRADE_FLAG_FINISH);
		// clean up and call user callback
		rboot_ota_deinit();
	} else if (upgrade->conn->state != ESPCONN_READ) {
		ERROR("fail, but how do we get here? premature end of stream?");
		rboot_ota_deinit();
	}
}

// disconnect callback, clean up the connection
// we also call this ourselves
static void ICACHE_FLASH_ATTR upgrade_disconcb(void *arg) {
	// use passed ptr, as upgrade struct may have gone by now
	struct espconn *conn = (struct espconn*)arg;
	
	os_timer_disarm(&ota_timer);
	if (conn) {
		if (conn->proto.tcp) os_free(conn->proto.tcp);
		os_free(conn);
	}
	
	// is upgrade struct still around?
	// if so disconnect was from remote end, or we called
	// ourselves to cleanup a failed connection attempt
	// must ensure disconnect was for this upgrade attempt,
	// not a previous one! this call back is async so another
	// upgrade struct may have been created already
	if (upgrade && upgrade->conn == conn) {
		// mark connection as gone
		upgrade->conn = 0;
		// end the update process
		ERROR("discon callback");
		rboot_ota_deinit();
	}
}

// successfully connected to update server, send the request
static void ICACHE_FLASH_ATTR upgrade_connect_cb(void *arg) {
	
	// disable the timeout
	os_timer_disarm(&ota_timer);

	// register connection callbacks
	espconn_regist_disconcb(upgrade->conn, upgrade_disconcb);
	espconn_regist_recvcb(upgrade->conn, upgrade_recvcb);

	// send the http request, with timeout for reply and rest of update to complete
	os_timer_setfn(&ota_timer, (os_timer_func_t *)rboot_ota_deinit, 0);
	os_timer_arm(&ota_timer, OTA_DOWNLOAD_TIMEOUT, 0);
	espconn_sent(upgrade->conn, upgrade->ota->request, os_strlen((char*)upgrade->ota->request));
}

// connection attempt timed out
static void ICACHE_FLASH_ATTR connect_timeout_cb() {
	ERROR("Connect timeout.");
	// not connected so don't call disconnect on the connection
	// but call our own disconnect callback to do the cleanup
	upgrade_disconcb(upgrade->conn);
}

// call back for lost connection
static void ICACHE_FLASH_ATTR upgrade_recon_cb(void *arg, sint8 errType) {
	ERROR("Connection error.");
	// not connected so don't call disconnect on the connection
	// but call our own disconnect callback to do the cleanup
	upgrade_disconcb(upgrade->conn);
}

// start the ota process, with user supplied options
bool ICACHE_FLASH_ATTR rboot_ota_start(rboot_ota *ota) {
	
	// check not already updating
	if (system_upgrade_flag_check() == UPGRADE_FLAG_START) {
		return false;
	}
	
	// check parameters
	if (!ota || !ota->request) {
		ERROR("Invalid parameters.");
		return false;
	}
	
	// set up update structure
	if (!rboot_ota_init(ota)) {
		return false;
	}
	
	// set up connection
	upgrade->conn->type = ESPCONN_TCP;
	upgrade->conn->state = ESPCONN_NONE;
	upgrade->conn->proto.tcp->local_port = espconn_port();
	upgrade->conn->proto.tcp->remote_port = ota->port;
//	*(uint32*)upgrade->conn->proto.tcp->remote_ip = *(uint32*)ota->ip;
	upgrade->conn->proto.tcp->remote_ip[0] = ota->ip[0];
	upgrade->conn->proto.tcp->remote_ip[1] = ota->ip[1];
	upgrade->conn->proto.tcp->remote_ip[2] = ota->ip[2];
	upgrade->conn->proto.tcp->remote_ip[3] = ota->ip[3];
	// set connection call backs
	espconn_regist_connectcb(upgrade->conn, upgrade_connect_cb);
	espconn_regist_reconcb(upgrade->conn, upgrade_recon_cb);

	// try to connect
	espconn_connect(upgrade->conn);

	// set connection timeout timer
	os_timer_disarm(&ota_timer);
	os_timer_setfn(&ota_timer, (os_timer_func_t *)connect_timeout_cb, 0);
	os_timer_arm(&ota_timer, OTA_CONNECT_TIMEOUT, 0);

	return true;
}
