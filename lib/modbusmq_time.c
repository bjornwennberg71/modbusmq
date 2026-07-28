//////////////////////////////////////////////////////////////////////////////
// 
// bjornwennberg71@gmail.com
// 
// modbusmq_time.c
// 

// INCLUDES //////////////////////////////////////////////////////////////////
#include "modbusmq_time.h"

#include <sys/time.h>

//////////////////////////////////////////////////////////////////////////////
//
// get time in milliseconds
//
millitime_t
millitime()
{
  struct timeval tv;
  gettimeofday(&tv,NULL);
  
  uint64_t mtime  = 1000 * (uint64_t)tv.tv_sec + (uint64_t)tv.tv_usec/1000;
  return mtime;
}

//////////////////////////////////////////////////////////////////////////////
// 
// make millitime_t from time() and milliseconds
//
millitime_t
mk_millitime(time_t nTime, int nMilliseconds)
{
    struct timeval tv = { nTime, nMilliseconds*1000};
    uint64_t mtime = 1000 * (uint64_t)tv.tv_sec + (uint64_t)tv.tv_usec/1000;
    return mtime;
}

