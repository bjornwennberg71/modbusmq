//////////////////////////////////////////////////////////////////////////////
// 
// bjornwennberg71@gmail.com
// 
// modbusmq_time.h
//
#ifndef modbusmq_time_h_
#define modbusmq_time_h_

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

#endif // modbusmq_time_h_
