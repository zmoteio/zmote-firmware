#ifndef RPS_H_
#define RPS_H_

/*
 * Redundant Protected Storage
 *
 * RPS uses 8 KB to store 4 KB of data in flash while making sure that a valid
 * copy of the data is always available.
 *
 * It does so by reserving 8 bytes at the end of the sector to store a sequence 
 * number and CRC.  While reading, should both sectors possess a valid CRC, later one 
 * (i.e., the one with a larger sequence number) is read.  Should only one of the 
 * sectors posses valid CRC, it read irrespective of its sequence number.  Should both
 * sectors not possess a valid CRC, RPS initializes both sectors with 0's and returns
 * a zero filled buffer.
 *
 * While writing, RPS always chooses the block other than the one that would be read.
 *
 * RPS storage may only be accessed in it's entirity. While reading and writing, 
 * you may ask or provide up to RPS_MAX_DATA_LEN bytes.  In both cases, the 
 * caller must allocate RPS_ALLOC bytes in the buffer to be read or written.
 *
 * RPS also offers a convinient flags storage for lightweight storage and 
 * retrieval of 4 bytes of data in a safe and redundant manner.  These 4 bytes 
 * may be read and written to without the caller having to allocate a buffer 
 * of data.  It is primarily intended for use from boot code.
 *
 */

#define RPS_SECTOR_SIZE (4096)
#define RPS_ALLOC (RPS_SECTOR_SIZE)
#define RPS_MAX_DATA_LEN (RPS_ALLOC - sizeof(rps_footer))

#ifdef RBOOT
#undef  ICACHE_FLASH_ATTR
#define ICACHE_FLASH_ATTR
#endif

typedef struct rps_footer_ {
	uint32 flags;
	uint32 seq;
	uint32 crc;
} rps_footer;

typedef struct rps_sector_ {
	uint8 data[RPS_MAX_DATA_LEN];
	rps_footer footer;
} rps_sector;

int ICACHE_FLASH_ATTR rps_write(void *psrc, uint32 dst, int len);
int ICACHE_FLASH_ATTR rps_read(void *pdst, uint32 src, int offset, int len);
int ICACHE_FLASH_ATTR rps_set_flags(uint32 flags, uint32 dst);
int ICACHE_FLASH_ATTR rps_get_flags(uint32 *flags, uint32 src);

uint32 *ICACHE_FLASH_ATTR rps_read_ptr(uint32 src);

#endif
