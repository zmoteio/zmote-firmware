#ifndef ROUTES_H_
#define ROUTES_H_

void ICACHE_FLASH_ATTR routesInit(void);
void ICACHE_FLASH_ATTR	execRoute(const char *url, const char *method, const char *postdata, int pdLen, char *response, int repLen);

#endif
