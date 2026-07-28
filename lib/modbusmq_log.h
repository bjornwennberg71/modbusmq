//////////////////////////////////////////////////////////////////////////////
// 
// bjornwennberg71@gmail.com
// 
// modbusmq_log.h
// 
#ifndef modbusmq_log_h_
#define modbusmq_log_h_

#define LOG_ERROR 1
#define LOG_INFO  2
#define LOG_DEBUG 3

#ifdef __cplusplus
extern "C" {
#endif
    
extern void modbusmq_logf(int loglevel, const char *format, ...);

#ifdef __cplusplus
}
#endif

#endif // modbusmq_log_h_
