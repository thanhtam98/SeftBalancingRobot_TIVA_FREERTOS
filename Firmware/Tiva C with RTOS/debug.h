#ifndef	__DEBUG_H
#define	__DEBUG_H

#define	DEBUG

#ifdef DEBUG
typedef enum {FAIL=0,OK } xStatusTypeDef ;
typedef enum {LEFT=0,RIGHT} motor_t ;


//	#define DBG(fmt,...) UARTprintf("%s:  %s:  %d: "fmt, __FILE__, __FUNCTION__, __LINE__,##__VA_ARGS__)
#define DBG(fmt,...) UARTprintf(fmt,##__VA_ARGS__)

#else
  #define DBG(fmt, args...)
#endif

#endif
