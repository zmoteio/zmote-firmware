#ifndef RPS_TEST
#include <esp8266.h>
#include "rps.h"
#else
#include "../test/rps_test.c"
#endif

#ifndef FLASH_MAP
#define FLASH_MAP(c) ((uint32 *)(0x40200000 + (c)))
#endif

#define MAP_FOOTER(c) &(((rps_sector *)(FLASH_MAP(c)))->footer)

static uint32 ICACHE_FLASH_ATTR
crc32(uint32 crc, const void *buf, size_t size);

#ifdef RBOOT
extern uint32 SPIRead(uint32 addr, void *outptr, uint32 len);
extern void ets_printf(char*, ...);
#endif

#ifdef RBOOT
static int ICACHE_FLASH_ATTR rps_init(uint32 dst)
{
	return SPI_FLASH_RESULT_ERR;
}
#else // RBOOT
static int ICACHE_FLASH_ATTR rps_init(uint32 dst)
{
	rps_sector *psec = NULL;
	int i;

	if (!(psec = os_malloc(sizeof(rps_sector))))
		goto err;
	os_memset(psec, 0xFFu, sizeof(rps_sector));
	for (i = 1; i < 3; i++, dst += RPS_SECTOR_SIZE) {
		psec->footer.seq = i;
		psec->footer.flags = 0;
		psec->footer.crc = crc32(0, psec, RPS_SECTOR_SIZE-4);
		if (spi_flash_erase_sector((dst >> 12)&0xFFFFU))
			goto err;
		if (spi_flash_write(dst + RPS_MAX_DATA_LEN, (void *)&(psec->footer), sizeof(rps_footer)))
			goto err;
	}
	os_free(psec);
	return SPI_FLASH_RESULT_OK;
err:
	if (psec)
		os_free(psec);
	return SPI_FLASH_RESULT_ERR;
}
#endif // RBOOT

static int ICACHE_FLASH_ATTR rps_examine(uint32 start, uint32 *cur, uint32 *next)
{
	uint32 crc1, crc2;
	rps_footer *f1, *f2;

#ifdef RBOOT
	rps_sector sector[2];

	if (SPIRead(start, sector, 2 * RPS_SECTOR_SIZE) != 0) {
		return SPI_FLASH_RESULT_ERR;
	}

	crc1 = crc32(0, &sector[0], RPS_SECTOR_SIZE-4);
	crc2 = crc32(0, &sector[1], RPS_SECTOR_SIZE-4);
	f1 = &sector[0].footer;
	f2 = &sector[1].footer;
#else // RBOOT
	crc1 = crc32(0, FLASH_MAP(start), RPS_SECTOR_SIZE-4);
	crc2 = crc32(0, FLASH_MAP(start+RPS_SECTOR_SIZE), RPS_SECTOR_SIZE-4);
	f1 = MAP_FOOTER(start);
	f2 = MAP_FOOTER(start + RPS_SECTOR_SIZE);
#endif // RBOOT

	if (f1->crc == crc1 && f2->crc == crc2) {
		if (f1->seq > f2->seq) {
			*cur = start;
			*next = start + RPS_SECTOR_SIZE;
		} else {
			*cur = start + RPS_SECTOR_SIZE;
			*next = start;
		}
	} else if (f1->crc == crc1) {
		*cur = start;
		*next = start + RPS_SECTOR_SIZE;
	} else  {
		if (f2->crc != crc2 && rps_init(start))
			return SPI_FLASH_RESULT_ERR;
		*cur = start + RPS_SECTOR_SIZE;
		*next = start;
	}
	return SPI_FLASH_RESULT_OK;
}

#ifndef RBOOT
static int ICACHE_FLASH_ATTR rps_write_(void *psrc, uint32 dst, bool new_flag, int len)
{
	uint32 cur, next, i;
	rps_sector *ptarget;
	rps_footer *cur_footer;

	if (len <= 0 || len > RPS_MAX_DATA_LEN)
		return SPI_FLASH_RESULT_ERR;
	if (rps_examine(dst, &cur, &next))
		return SPI_FLASH_RESULT_ERR;
	ptarget = psrc;
	// Unused bytes are filled with 0xff so the CRC calc comes out right
	// This allows us to write fewer bytes
	for (i = len; i < RPS_MAX_DATA_LEN; i++)
		ptarget->data[i] = 0xFFu;

	cur_footer = MAP_FOOTER(cur);
	ptarget->footer.seq = cur_footer->seq + 1;
	// new_flag indicates that it is an internal call
	// and the new flag value is already correctly
	// populated in the buffer
	if (!new_flag)
		ptarget->footer.flags = cur_footer->flags;
	ptarget->footer.crc = crc32(0, ptarget, RPS_SECTOR_SIZE-4);
	if (spi_flash_erase_sector((next >> 12)&0xFFFFU))
		return SPI_FLASH_RESULT_ERR;
	if (spi_flash_write(next, (void *)ptarget, len))
		return SPI_FLASH_RESULT_ERR;
	return spi_flash_write(next + RPS_MAX_DATA_LEN, (void *)&(ptarget->footer), sizeof(rps_footer));
}
int ICACHE_FLASH_ATTR rps_write(void *psrc, uint32 dst, int len)
{
	return rps_write_(psrc, dst, false, len);
}
int ICACHE_FLASH_ATTR rps_read(void *pdst, uint32 src, int offset, int len)
{
	uint32 cur, next;

	if (rps_examine(src, &cur, &next))
		return SPI_FLASH_RESULT_ERR;
	return spi_flash_read(cur + offset, (void *)pdst, len);
}

uint32 *ICACHE_FLASH_ATTR rps_read_ptr(uint32 src)
{
	uint32 cur, next;

	if (rps_examine(src, &cur, &next))
		return NULL;
	return FLASH_MAP(cur);
}
#endif // RBOOT

#ifdef RBOOT
int ICACHE_FLASH_ATTR rps_get_flags(uint32 *flags, uint32 src)
{
	uint32 cur = 0, next;
	rps_footer footer;
	if (rps_examine(src, &cur, &next))
		return SPI_FLASH_RESULT_ERR;
	if (SPIRead(cur + RPS_MAX_DATA_LEN, &footer, sizeof(footer)) != 0) {
		return SPI_FLASH_RESULT_ERR;
	}
	*flags = footer.flags;
	return SPI_FLASH_RESULT_OK;
}
#else // RBOOT
int ICACHE_FLASH_ATTR rps_set_flags(uint32 flags, uint32 dst)
{
	rps_sector target;
	if (rps_read(&target, dst, 0, RPS_MAX_DATA_LEN))
		return SPI_FLASH_RESULT_ERR;
	target.footer.flags = flags;
	return rps_write_(&target, dst, true, RPS_MAX_DATA_LEN);
}
int ICACHE_FLASH_ATTR rps_get_flags(uint32 *flags, uint32 src)
{
	rps_footer footer;
	if (rps_read(&footer, src, RPS_MAX_DATA_LEN, sizeof(footer)))
		return SPI_FLASH_RESULT_ERR;
	*flags = footer.flags;
	return SPI_FLASH_RESULT_OK;
}
#endif // RBOOT

/*-
 *  COPYRIGHT (C) 1986 Gary S. Brown.  You may use this program, or
 *  code or tables extracted from it, as desired without restriction.
 *
 *  First, the polynomial itself and its table of feedback terms.  The
 *  polynomial is
 *  X^32+X^26+X^23+X^22+X^16+X^12+X^11+X^10+X^8+X^7+X^5+X^4+X^2+X^1+X^0
 *
 *  Note that we take it "backwards" and put the highest-order term in
 *  the lowest-order bit.  The X^32 term is "implied"; the LSB is the
 *  X^31 term, etc.  The X^0 term (usually shown as "+1") results in
 *  the MSB being 1
 *
 *  Note that the usual hardware shift register implementation, which
 *  is what we're using (we're merely optimizing it by doing eight-bit
 *  chunks at a time) shifts bits into the lowest-order term.  In our
 *  implementation, that means shifting towards the right.  Why do we
 *  do it this way?  Because the calculated CRC must be transmitted in
 *  order from highest-order term to lowest-order term.  UARTs transmit
 *  characters in order from LSB to MSB.  By storing the CRC this way
 *  we hand it to the UART in the order low-byte to high-byte; the UART
 *  sends each low-bit to hight-bit; and the result is transmission bit
 *  by bit from highest- to lowest-order term without requiring any bit
 *  shuffling on our part.  Reception works similarly
 *
 *  The feedback terms table consists of 256, 32-bit entries.  Notes
 *
 *      The table can be generated at runtime if desired; code to do so
 *      is shown later.  It might not be obvious, but the feedback
 *      terms simply represent the results of eight shift/xor opera
 *      tions for all combinations of data and CRC register values
 *
 *      The values must be right-shifted by eight bits by the "updcrc
 *      logic; the shift must be unsigned (bring in zeroes).  On some
 *      hardware you could probably optimize the shift in assembler by
 *      using byte-swap instructions
 *      polynomial $edb88320
 *
 *
 * CRC32 code derived from work by Gary S. Brown.
 *
 * Modified for ESP8266 and to work with uint32 accesses by harik -- Sep 3, 2015
 *
 */


static const uint32 crc32_tab[] = {
	0x00000000, 0x77073096, 0xee0e612c, 0x990951ba, 0x076dc419, 0x706af48f,
	0xe963a535, 0x9e6495a3,	0x0edb8832, 0x79dcb8a4, 0xe0d5e91e, 0x97d2d988,
	0x09b64c2b, 0x7eb17cbd, 0xe7b82d07, 0x90bf1d91, 0x1db71064, 0x6ab020f2,
	0xf3b97148, 0x84be41de,	0x1adad47d, 0x6ddde4eb, 0xf4d4b551, 0x83d385c7,
	0x136c9856, 0x646ba8c0, 0xfd62f97a, 0x8a65c9ec,	0x14015c4f, 0x63066cd9,
	0xfa0f3d63, 0x8d080df5,	0x3b6e20c8, 0x4c69105e, 0xd56041e4, 0xa2677172,
	0x3c03e4d1, 0x4b04d447, 0xd20d85fd, 0xa50ab56b,	0x35b5a8fa, 0x42b2986c,
	0xdbbbc9d6, 0xacbcf940,	0x32d86ce3, 0x45df5c75, 0xdcd60dcf, 0xabd13d59,
	0x26d930ac, 0x51de003a, 0xc8d75180, 0xbfd06116, 0x21b4f4b5, 0x56b3c423,
	0xcfba9599, 0xb8bda50f, 0x2802b89e, 0x5f058808, 0xc60cd9b2, 0xb10be924,
	0x2f6f7c87, 0x58684c11, 0xc1611dab, 0xb6662d3d,	0x76dc4190, 0x01db7106,
	0x98d220bc, 0xefd5102a, 0x71b18589, 0x06b6b51f, 0x9fbfe4a5, 0xe8b8d433,
	0x7807c9a2, 0x0f00f934, 0x9609a88e, 0xe10e9818, 0x7f6a0dbb, 0x086d3d2d,
	0x91646c97, 0xe6635c01, 0x6b6b51f4, 0x1c6c6162, 0x856530d8, 0xf262004e,
	0x6c0695ed, 0x1b01a57b, 0x8208f4c1, 0xf50fc457, 0x65b0d9c6, 0x12b7e950,
	0x8bbeb8ea, 0xfcb9887c, 0x62dd1ddf, 0x15da2d49, 0x8cd37cf3, 0xfbd44c65,
	0x4db26158, 0x3ab551ce, 0xa3bc0074, 0xd4bb30e2, 0x4adfa541, 0x3dd895d7,
	0xa4d1c46d, 0xd3d6f4fb, 0x4369e96a, 0x346ed9fc, 0xad678846, 0xda60b8d0,
	0x44042d73, 0x33031de5, 0xaa0a4c5f, 0xdd0d7cc9, 0x5005713c, 0x270241aa,
	0xbe0b1010, 0xc90c2086, 0x5768b525, 0x206f85b3, 0xb966d409, 0xce61e49f,
	0x5edef90e, 0x29d9c998, 0xb0d09822, 0xc7d7a8b4, 0x59b33d17, 0x2eb40d81,
	0xb7bd5c3b, 0xc0ba6cad, 0xedb88320, 0x9abfb3b6, 0x03b6e20c, 0x74b1d29a,
	0xead54739, 0x9dd277af, 0x04db2615, 0x73dc1683, 0xe3630b12, 0x94643b84,
	0x0d6d6a3e, 0x7a6a5aa8, 0xe40ecf0b, 0x9309ff9d, 0x0a00ae27, 0x7d079eb1,
	0xf00f9344, 0x8708a3d2, 0x1e01f268, 0x6906c2fe, 0xf762575d, 0x806567cb,
	0x196c3671, 0x6e6b06e7, 0xfed41b76, 0x89d32be0, 0x10da7a5a, 0x67dd4acc,
	0xf9b9df6f, 0x8ebeeff9, 0x17b7be43, 0x60b08ed5, 0xd6d6a3e8, 0xa1d1937e,
	0x38d8c2c4, 0x4fdff252, 0xd1bb67f1, 0xa6bc5767, 0x3fb506dd, 0x48b2364b,
	0xd80d2bda, 0xaf0a1b4c, 0x36034af6, 0x41047a60, 0xdf60efc3, 0xa867df55,
	0x316e8eef, 0x4669be79, 0xcb61b38c, 0xbc66831a, 0x256fd2a0, 0x5268e236,
	0xcc0c7795, 0xbb0b4703, 0x220216b9, 0x5505262f, 0xc5ba3bbe, 0xb2bd0b28,
	0x2bb45a92, 0x5cb36a04, 0xc2d7ffa7, 0xb5d0cf31, 0x2cd99e8b, 0x5bdeae1d,
	0x9b64c2b0, 0xec63f226, 0x756aa39c, 0x026d930a, 0x9c0906a9, 0xeb0e363f,
	0x72076785, 0x05005713, 0x95bf4a82, 0xe2b87a14, 0x7bb12bae, 0x0cb61b38,
	0x92d28e9b, 0xe5d5be0d, 0x7cdcefb7, 0x0bdbdf21, 0x86d3d2d4, 0xf1d4e242,
	0x68ddb3f8, 0x1fda836e, 0x81be16cd, 0xf6b9265b, 0x6fb077e1, 0x18b74777,
	0x88085ae6, 0xff0f6a70, 0x66063bca, 0x11010b5c, 0x8f659eff, 0xf862ae69,
	0x616bffd3, 0x166ccf45, 0xa00ae278, 0xd70dd2ee, 0x4e048354, 0x3903b3c2,
	0xa7672661, 0xd06016f7, 0x4969474d, 0x3e6e77db, 0xaed16a4a, 0xd9d65adc,
	0x40df0b66, 0x37d83bf0, 0xa9bcae53, 0xdebb9ec5, 0x47b2cf7f, 0x30b5ffe9,
	0xbdbdf21c, 0xcabac28a, 0x53b39330, 0x24b4a3a6, 0xbad03605, 0xcdd70693,
	0x54de5729, 0x23d967bf, 0xb3667a2e, 0xc4614ab8, 0x5d681b02, 0x2a6f2b94,
	0xb40bbe37, 0xc30c8ea1, 0x5a05df1b, 0x2d02ef8d
};

static uint32 ICACHE_FLASH_ATTR
crc32(uint32 crc, const void *vbuf, size_t size)
{
	const uint32 *buf = vbuf;
	uint32 d;

	crc = crc ^ ~0U;
	size >>= 2;
	while (size--) {
		d = *buf++;
		crc = crc32_tab[(crc ^ ((d >>  0)&0xFFU)) & 0xFFU] ^ (crc >> 8);
		crc = crc32_tab[(crc ^ ((d >>  8)&0xFFU)) & 0xFFU] ^ (crc >> 8);
		crc = crc32_tab[(crc ^ ((d >> 16)&0xFFU)) & 0xFFU] ^ (crc >> 8);
		crc = crc32_tab[(crc ^ ((d >> 24)&0xFFU)) & 0xFFU] ^ (crc >> 8);
	}

	return crc ^ ~0U;
}
