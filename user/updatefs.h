#ifndef UPDATEFS_H_
#define UPDATEFS_H_

#include "jsmn.h"

enum {
	UPDATEFS_OK,
	UPDATEFS_BUSY,
	UPDATEFS_MEM_ERROR,
	UPDATEFS_DISCONNECT,
	UPDATEFS_TIMEOUT
};

typedef void (*updatefs_finished_cb)(void *arg, int status);

int ICACHE_FLASH_ATTR updatefs(char *json, jsmntok_t *toks, int ntok, updatefs_finished_cb cb, void *arg);

#endif
