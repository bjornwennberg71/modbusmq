//////////////////////////////////////////////////////////////////////////////
// 
// bjornwennberg71@gmail.com
// 
// modbusmq_log.c
// 

// INCLUDES //////////////////////////////////////////////////////////////////
#include "modbusmq_log.h"
#include "modbusmq_time.h"

#include <time.h>
#include <stdio.h>
#include <stdarg.h>


const char *
modbusmq_log_level(int level)
{
    if (level <= 1)
    {
        return "error";
    }
    else if (level == 2)
    {
        return "info";
    }

    return "debug";
}


void
modbusmq_logf(int level, const char *format, ...)
{
    va_list ap;
    if (!format)
    {
        return;
    }

    millitime_t milli_now = millitime();
    time_t      time_now = milli_now/1000;
    struct tm *tm_ptr = localtime(&time_now);

    char
        azBuf[100];
    
    millitime_t mseconds = milli_now - 1000*(milli_now/1000);
    strftime(azBuf, 100, "%Y%m%d %H:%M:%S", tm_ptr);
    char
        azBuf2[10];
    sprintf(azBuf2, ":%03d", (int)mseconds);
    
    fprintf(stdout, "%s%s:%s: ", azBuf, azBuf2, modbusmq_log_level(level));

    va_start(ap, format);
    vfprintf(stdout, format, ap);
    va_end(ap);

    //
    // stdout is fully buffered whenever it is not a terminal, which is the
    // normal case in production: redirected to a file or piped to a log
    // collector. A SIGTERM then drops up to a full buffer of unwritten lines —
    // exactly the lines explaining why the process was being stopped. Flush
    // here so a log that was written is a log that survives.
    //
    fflush(stdout);
}
