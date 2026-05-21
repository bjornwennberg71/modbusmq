//////////////////////////////////////////////////////////////////////////////
// 
// bjornwennberg71@gmail.com
// 
// modbus_time.h
// 
#ifndef modbus_time_h_
#define modbus_time_h_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <time.h>

typedef uint64_t millitime_t;

millitime_t    millitime();
millitime_t    mk_millitime(time_t nTime, int nMilliseconds);

#ifdef __cplusplus
}
#endif

#endif // modbus_time_h_
