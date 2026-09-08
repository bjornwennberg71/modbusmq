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
#include <stddef.h> // size_t, for modbusmq_channel_format_value()

// DEFINES ///////////////////////////////////////////////////////////////////

#define MODBUSMQ_VERSION_MAJOR 2
#define MODBUSMQ_VERSION_MINOR 2
#define MODBUSMQ_VERSION_BUILD 1

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
// MODBUSMQ_ERR_TIMEOUT means the request went out but nothing came back within
// the frame timeout. It is reported through the error callback only — the loop
// functions never return it, since a timed-out request is dropped and the queue
// simply moves on.
//
#define MODBUSMQ_ERR_TRANSPORT (-1)
#define MODBUSMQ_ERR_PROTOCOL  (-2)
#define MODBUSMQ_ERR_TIMEOUT   (-3)

#ifdef __cplusplus
extern "C" {
#endif

    // FORWARD DECL
struct modbusmq_input_t;
struct modbusmq_channel_t;
struct modbusmq_write_t;

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
    int            msg_id;   // subscription timer id, 0 for one-shot messages

                             // Identifies this one request/response attempt.
                             // Stamped by the library on post/subscribe/send
                             // and never reused, so a subscription polling the
                             // same slave every 2 s gets a new req_id per poll.
                             // Every error line carries it, and it is readable
                             // from the message handed to the callbacks — that
                             // is how a caller ties an error on a shared RTU
                             // bus back to the device that produced it.
                             //
                             // RTU has no transaction id on the wire, so this
                             // is a local correlation id, not something the
                             // slave ever sees.
    uint32_t       req_id;

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
    // fills in correct descriptors to be used in select. returns recommended sleep time and filedescriptor,
    // or -1 when there is no connection
extern int modbusmq_loop_prepare(    struct modbusmq_context_t *context, millitime_t *sleep_time, int16_t *poll_events);

    // callbacks
extern void modbusmq_set_message_callback(struct modbusmq_context_t *context, void (*message_cb)(struct modbusmq_context_t *context, modbusmq_msg_t *msg));
extern void modbusmq_set_subscription_callback(struct modbusmq_context_t *context, void (*subscription_cb)(struct modbusmq_context_t *context, modbusmq_msg_t *msg, struct modbusmq_input_t *input));
    //
    // Called whenever a request is given up on: a rejected frame, a frame
    // timeout, or a dead connection. msg is the failed request/response pair —
    // read msg->req_id, and modbusmq_frame_slave()/modbusmq_frame_addr() on
    // msg->frame[0], to see which device on the bus is struggling. error is one
    // of the MODBUSMQ_ERR_* codes above.
    //
    // msg is owned by the library and is freed as soon as the callback returns;
    // copy anything that must outlive it.
    //
extern void modbusmq_set_error_callback(struct modbusmq_context_t *context, void (*error_cb)(struct modbusmq_context_t *context, modbusmq_msg_t *msg, int error));


    // libmodbusmq utility functions
extern void modbusmq_set_debug(int nlevel);
extern int  modbusmq_get_debug(void);

extern const char *modbusmq_strerror(int nerrno);

    // modbusmq messages
    //
    // Function 05, write single coil. addr is a coil address, value is treated
    // as a boolean and encoded as the protocol's 0xFF00 / 0x0000.
    //
extern int  modbusmq_frame_write_coil_bit(      struct modbusmq_context_t *context, modbusmq_frame_t *frame, int addr, int value);
    //
    // Function 15, write multiple coils. bits is one byte per coil, non-zero
    // meaning on — not packed bits. nbits is 1..1968.
    //
extern int  modbusmq_frame_write_coil_bits(     struct modbusmq_context_t *context, modbusmq_frame_t *frame, int addr, int nbits, const uint8_t *bits);
    // Function 01, read coils
extern int  modbusmq_frame_read_coil_bits(      struct modbusmq_context_t *context, modbusmq_frame_t *frame, int addr, int nbits);
    // Function 02, read discrete inputs
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
    
    //
    // Payload byte count of a read response. < 0 for anything without one: a
    // request, a write echo, or an exception reply.
    //
extern int         modbusmq_frame_nbytes(        struct modbusmq_context_t *context, modbusmq_frame_t *frame);
extern int         modbusmq_frame_error_code(    struct modbusmq_context_t *context, modbusmq_frame_t *frame);

//
// "[req 4711 slave 3 addr 0x01F4]" — the prefix every error line starts with,
// so a log from a bus with several slaves can be read one device at a time.
// Returns a pointer to a static buffer: fine in this single-threaded library,
// but never use it twice in the same printf-style call.
//
extern const char *modbusmq_msg_tag(struct modbusmq_context_t *context, modbusmq_msg_t *msg);

//
// other utility functions
//
extern int   modbusmq_read_int16_ab(const uint8_t *data);
extern int   modbusmq_read_int16_ba(const uint8_t *data);
    // signed two's-complement counterparts; the unsigned ones above turn a
    // negative reading into a large positive number
extern int   modbusmq_read_int16_ab_signed(const uint8_t *data);
extern int   modbusmq_read_int16_ba_signed(const uint8_t *data);
extern int   modbusmq_read_int32_abcd(const uint8_t *data);
extern int   modbusmq_read_int32_badc(const uint8_t *data);
extern float modbusmq_read_float_abcd(const uint8_t *data);
extern float modbusmq_read_float_badc(const uint8_t *data);
extern float modbusmq_read_float_dcba(const uint8_t *data);
extern float modbusmq_read_channel(struct modbusmq_context_t *context, modbusmq_msg_t *msg, const struct modbusmq_input_t *input, const struct modbusmq_channel_t *channel);
// 0 when the channel lies inside the received data, < 0 (and logged) when it does not
extern int   modbusmq_channel_in_range(struct modbusmq_context_t *context, modbusmq_msg_t *msg, const struct modbusmq_input_t *input, const struct modbusmq_channel_t *channel);

//
// Format an already-scaled channel value for output/publish, deciding the
// decimal count the same way for modbusmq_subscribe and anything else that
// prints a channel: an explicit channel.decimals wins, otherwise a bit
// channel gets 0, a float format gets 3, and an integer format follows its
// mod divisor (mod=-100 -> 2 decimals, and so on) — see modbusmq.c for the
// exact rule. A negative-zero result ("-0", "-0.000") is normalised to plain
// zero, since it reads as a sign flip that never happened.
//
// Returns the number of characters written, snprintf semantics; < 0 on error.
//
extern int   modbusmq_channel_format_value(const struct modbusmq_input_t *input, const struct modbusmq_channel_t *channel, float value, char *buf, size_t len);

//
// Decide whether a freshly read channel value should be published now, and
// record the outcome into the channel's runtime state (last_value, last_text,
// last_publish_ms, published) when it says yes.
//
// text is the value already formatted by modbusmq_channel_format_value() —
// "unchanged" is judged on what would actually be printed/published, at the
// channel's own decimal count, not on the raw float. Two readings a hair
// apart that print identically are the same value as far as on_change /
// min_change / min_change_rel are concerned.
//
// See modbusmq_channel_t in modbusmq_config.h for what on_change/min_change/
// min_change_rel/min_interval/max_interval mean; the precedence between them
// is documented on the implementation in modbusmq.c.
//
// Returns 1 = publish, 0 = suppress, < 0 on bad arguments.
//
extern int   modbusmq_channel_publish_decide(struct modbusmq_channel_t *channel, float value, const char *text, millitime_t now_ms);

    //
    // Encode a raw value as wire bytes for a data format. No scaling: the
    // value is already a register value. out needs 4 bytes.
    // Returns bytes written, < 0 for a format that cannot be encoded.
    //
extern int   modbusmq_encode_value(int format, double value, uint8_t *out);
    // bytes a data format occupies on the wire: 1, 2 or 4
extern int   modbusmq_format_size(int format);
    //
    // Undo a write entry's add/mod/mul and encode the result into up to two
    // registers in wire order — the exact inverse of modbusmq_read_channel().
    // Rejects values that do not fit the format rather than truncating them.
    // Returns the register count (1 or 2), < 0 on error. Not for coil writes.
    //
extern int   modbusmq_write_encode(struct modbusmq_context_t *context, const struct modbusmq_write_t *write, double value, uint16_t *regs);

extern void  modbusmq_write_int16_ab(uint8_t *data, uint16_t value);
extern void  modbusmq_write_int32_abcd(uint8_t *data, uint32_t value);
extern void  modbusmq_write_int64_abcdefgh(uint8_t *data, uint64_t value);

#define MODBUSMQ_CONNECT_TCP  1
#define MODBUSMQ_CONNECT_RTU  2
#define MODBUSMQ_CONNECT_MQTT 3

// tcp://hostname:port
// tcp://192.168.3.177:502
//    
// rtu:///device:baudrate:stopbit:databits:parity
// rtu:///dev/ttyUSB1:9600:1:8:N
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
