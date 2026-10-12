//////////////////////////////////////////////////////////////////////////////
// 
// bjornwennberg71@gmail.com
// 
// modbusmq_log.h
// 
#ifndef modbusmq_log_h_
#define modbusmq_log_h_

#define MODBUSMQ_LOG_ERROR 1
#define MODBUSMQ_LOG_INFO  2
#define MODBUSMQ_LOG_DEBUG 3

#ifdef __cplusplus
extern "C" {
#endif
    
extern void modbusmq_logf(int loglevel, const char *format, ...) __attribute__((format(printf, 2, 3)));

//
// Route the library's log lines somewhere else, e.g. into the application's
// own logger. The handler gets the level and the bare message: no timestamp,
// no level tag, no trailing newline. Messages longer than 1023 bytes are
// truncated. NULL restores the default printer (timestamped, errors to
// stderr, the rest to stdout). Process-wide, like the logger itself.
//
typedef void (*modbusmq_log_handler_t)(int level, const char *message, void *user);

extern void modbusmq_log_set_handler(modbusmq_log_handler_t handler, void *user);

#ifdef __cplusplus
}
#endif

#endif // modbusmq_log_h_
