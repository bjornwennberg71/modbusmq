//////////////////////////////////////////////////////////////////////////////
// 
// bjornwennberg71@gmail.com
// 
// modbusmq.h
// 
#ifndef modbusmq_h_
#define modbusmq_h_

// INCLUDES //////////////////////////////////////////////////////////////////
#include "modbusmq_time.h"

#include <stdint.h>

// DEFINES ///////////////////////////////////////////////////////////////////

#define MODBUSMQ_VERSION_MAJOR 1
#define MODBUSMQ_VERSION_MINOR 1
#define MODBUSMQ_VERSION_BUILD 0

#define MODBUSMQ_STRINGIFY_(x) #x
#define MODBUSMQ_STRINGIFY(x)  MODBUSMQ_STRINGIFY_(x)
#define MODBUSMQ_VERSION_STRING \
    MODBUSMQ_STRINGIFY(MODBUSMQ_VERSION_MAJOR) "." \
    MODBUSMQ_STRINGIFY(MODBUSMQ_VERSION_MINOR) "." \
    MODBUSMQ_STRINGIFY(MODBUSMQ_VERSION_BUILD)

// maximum length of a frame 
#define MODBUSMQ_FRAME_MAX 260

#define MODBUSMQ_MIN(x,y) ((x) < (y) ? (x) : (y))
#define MODBUSMQ_MAX(x,y) ((x) > (y) ? (x) : (y))

//
// Error codes returned by modbusmq_loop_write_read().
//
// MODBUSMQ_ERR_TRANSPORT means the connection itself is gone and the caller
// must reconnect before anything else will work.
//
// MODBUSMQ_ERR_PROTOCOL means a frame was rejected (bad slave id, function,
// byte count or CRC) and the library has already discarded it and resynced the
// stream. The connection is still good: report it and carry on — the next
// queued request starts from a clean stream. Tearing down the connection for
// this is both unnecessary and counterproductive, since it drops every other
// pending subscription with it.
//
#define MODBUSMQ_ERR_TRANSPORT (-1)
#define MODBUSMQ_ERR_PROTOCOL  (-2)

#ifdef __cplusplus
extern "C" {
#endif

    // FORWARD DECL
struct modbusmq_input_t;
struct modbusmq_channel_t;

//
// contains data to send or receive
//
typedef struct modbusmq_frame_t
{
    uint8_t    is_writer:1;      // writer or reader
    uint8_t    is_tcp:1;         // tcp or rtu
    uint8_t    reserved:6;
    
    int16_t    length; // length of buffer
    int16_t    xmit;   // bytes transmitted
    uint8_t    buf[MODBUSMQ_FRAME_MAX];
} modbusmq_frame_t;

//
//  one modbusmq request/response message
//
typedef struct modbusmq_msg_t
{
    int            msg_id;

    modbusmq_frame_t frame[2];
} modbusmq_msg_t;

// FORWARD DECLS /////////////////////////////////////////////////////////////
struct modbusmq_context_t;
struct modbusmq_config_t;

// FUNCTIONS /////////////////////////////////////////////////////////////////
//
extern struct modbusmq_context_t *modbusmq_tcp_context(const char *pzConnectString);
extern struct modbusmq_context_t *modbusmq_rtu_context(const char *device, int baud, char parity, int databit, int stopbit);

extern int  modbusmq_connect(   struct modbusmq_context_t *context);
extern int  modbusmq_close(     struct modbusmq_context_t *context);
extern void modbusmq_reset_queue(struct modbusmq_context_t *context);
extern int  modbusmq_set_slave( struct modbusmq_context_t *context, int slave_id);
extern int  modbusmq_get_slave( struct modbusmq_context_t *context);
extern void modbusmq_free(      struct modbusmq_context_t *context);
extern int  modbusmq_set_config(struct modbusmq_context_t *context, struct modbusmq_config_t *config);
extern int  modbusmq_tcp_flush( struct modbusmq_context_t *context);
    // run-handler (for use in production)
extern int  modbusmq_loop_write_read(struct modbusmq_context_t *context, int revents);
    // fills in correct descriptors to be used in select. returns recommended sleep time and filedescriptor
extern int modbusmq_loop_prepare(    struct modbusmq_context_t *context, millitime_t *sleep_time, int16_t *poll_events);

    // callbacks
extern void modbusmq_set_message_callback(struct modbusmq_context_t *context, void (*message_cb)(struct modbusmq_context_t *context, modbusmq_msg_t *msg));
extern void modbusmq_set_subscription_callback(struct modbusmq_context_t *context, void (*subscription_cb)(struct modbusmq_context_t *context, modbusmq_msg_t *msg, struct modbusmq_input_t *input));


    // libmodbusmq utility functions
extern void modbusmq_set_debug(int nlevel);
extern int  modbusmq_get_debug(void);

extern const char *modbusmq_strerror(int nerrno);

    // modbusmq messages
extern int  modbusmq_frame_write_bit(           struct modbusmq_context_t *context, modbusmq_frame_t *frame, int addr, int value);
extern int  modbusmq_frame_write_bits(          struct modbusmq_context_t *context, modbusmq_frame_t *frame, int addr, int nbits, const uint8_t *bits);
extern int  modbusmq_frame_read_bits(           struct modbusmq_context_t *context, modbusmq_frame_t *frame, int addr, int nbits);
extern int  modbusmq_frame_read_input_bits(     struct modbusmq_context_t *context, modbusmq_frame_t *frame, int addr, int nbits);
extern int  modbusmq_frame_write_register(      struct modbusmq_context_t *context, modbusmq_frame_t *frame, int addr, int value);
extern int  modbusmq_frame_write_registers(     struct modbusmq_context_t *context, modbusmq_frame_t *frame, int addr, int naddr, const uint16_t *values);
extern int  modbusmq_frame_read_holding_registers(struct modbusmq_context_t *context, modbusmq_frame_t *frame, int addr, int naddr);
extern int  modbusmq_frame_read_input_registers(  struct modbusmq_context_t *context, modbusmq_frame_t *frame, int addr, int naddr);
extern int  modbusmq_frame_write_mask_registers(  struct modbusmq_context_t *context, modbusmq_frame_t *frame, int addr, int and_mask, int or_mask);
extern void modbusmq_frame_debug(                 struct modbusmq_context_t *context, modbusmq_frame_t *frame);
    
// post a message
extern int  modbusmq_post(                struct modbusmq_context_t *context, modbusmq_msg_t *msg);
// subscribe to message every interval in ms
extern int  modbusmq_subscribe(           struct modbusmq_context_t *context, modbusmq_msg_t *msg, int interval_ms);
// send request and wait for response
extern int  modbusmq_send(                struct modbusmq_context_t *context, modbusmq_msg_t *msg, int mswait);

extern int modbusmq_frame_timeout(struct modbusmq_context_t *context, int timeout_ms);
    
// microsleep delay before writing a package (tune this is you experience package loss/missing frames)
extern int modbusmq_rtu_rts_delay(struct modbusmq_context_t *context, int interval_us);
    
// utility getters
extern int         modbusmq_frame_transaction_id(struct modbusmq_context_t *context, modbusmq_frame_t *frame);
extern int         modbusmq_frame_slave(         struct modbusmq_context_t *context, modbusmq_frame_t *frame);
extern int         modbusmq_frame_function(      struct modbusmq_context_t *context, modbusmq_frame_t *frame);
extern uint8_t *   modbusmq_frame_data(          struct modbusmq_context_t *context, modbusmq_frame_t *frame);
    
extern int         modbusmq_frame_addr(          struct modbusmq_context_t *context, modbusmq_frame_t *frame);
extern int         modbusmq_frame_naddr(         struct modbusmq_context_t *context, modbusmq_frame_t *frame);
    
extern int         modbusmq_frame_nbytes(        struct modbusmq_context_t *context, modbusmq_frame_t *frame);
extern int         modbusmq_frame_error_code(    struct modbusmq_context_t *context, modbusmq_frame_t *frame);

//
// other utility functions
//
extern int   modbusmq_read_int16_ab(const uint8_t *data);
extern int   modbusmq_read_int16_ba(const uint8_t *data);
extern int   modbusmq_read_int32_abcd(const uint8_t *data);
extern int   modbusmq_read_int32_badc(const uint8_t *data);
extern float modbusmq_read_float_abcd(const uint8_t *data);
extern float modbusmq_read_float_badc(const uint8_t *data);
extern float modbusmq_read_float_dcba(const uint8_t *data);
extern float modbusmq_read_channel(struct modbusmq_context_t *context, modbusmq_msg_t *msg, const struct modbusmq_input_t *input, const struct modbusmq_channel_t *channel);
// 0 when the channel lies inside the received data, < 0 (and logged) when it does not
extern int   modbusmq_channel_in_range(struct modbusmq_context_t *context, modbusmq_msg_t *msg, const struct modbusmq_input_t *input, const struct modbusmq_channel_t *channel);

extern void  modbusmq_write_int16_ab(uint8_t *data, uint16_t value);
extern void  modbusmq_write_int32_abcd(uint8_t *data, uint32_t value);
extern void  modbusmq_write_int64_abcdefgh(uint8_t *data, uint64_t value);

#define MODBUSMQ_CONNECT_TCP  1
#define MODBUSMQ_CONNECT_RTU  2
#define MODBUSMQ_CONNECT_MQTT 3

// tcp://hostname:port
// tcp://192.168.3.177:502
//    
// rtu:///device:baudrate:parity:databit:stopbit
// rtu:///dev/ttyUSB1:9600:N:8:1
typedef struct modbusmq_connect_t
{
    // either /dev/ttyUSB123 or tcp connect string
    int   connect_type;
    
    char  device[200];
    int   port;
    
    char  parity;
    int   baudrate;
    int   databits;
    int   stopbit;
    
} modbusmq_connect_t;
    
extern int modbusmq_parse_connect_string(const char *input_string, struct modbusmq_connect_t *connect);

// debug
extern void modbusmq_timer_debug_print(struct modbusmq_context_t *context);

#ifdef __cplusplus
}
#endif

#endif // modbusmq_h_
