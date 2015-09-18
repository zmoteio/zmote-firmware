#ifndef CONSOLE_H_
#define CONSOLE_H_

#ifdef ENABLE_UART_DEBUG
	#define DEBUG(fmt, args...) os_printf("DEBUG[%s,%d]: " fmt "\r\n", __FILE__, __LINE__, ##args)
	#define INFO(fmt, args...) os_printf("INFO[%s,%d]: " fmt "\r\n", __FILE__, __LINE__, ##args)
	#define WARN(fmt, args...) os_printf("WARN[%s,%d]: " fmt "\r\n", __FILE__, __LINE__, ##args)
	#define ERROR(fmt, args...) os_printf("ERROR[%s,%d]: " fmt "\r\n", __FILE__, __LINE__, ##args)
#else
	#define DEBUG(fmt, args...) ((void)0)
	#define INFO DEBUG
	#define WARN DEBUG
	#define ERROR DEBUG
#endif

#endif
