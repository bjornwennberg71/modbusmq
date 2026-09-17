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
#define MODBUSMQ_VERSION_MINOR 5
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
// MODBUSMQ_ERR_TIMEOUT means the request went out but nothing came back within
// the frame timeout. It is reported through the error callback only — the loop
// functions never return it, since a timed-out request is dropped and the queue
// simply moves on.
//
// MODBUSMQ_ERR_EXCEPTION means the device answered in time and in good order,
// to say no: the reply carries the function code with the exception bit set,
// and an exception code saying why. Nothing is wrong with the link or the
// stream, so like MODBUSMQ_ERR_TIMEOUT it is reported through the error
// callback only. The loop functions keep returning MODBUSMQ_ERR_PROTOCOL for
// it, because what has to happen to the stream is the same either way — the
// distinction is about the request, not the connection.
//
// The exception code itself is a third namespace and does not fit in this one.
// Read it off the response frame with modbusmq_frame_exception_code() and name
// it with modbusmq_exception_string(); the frame is valid for as long as the
// callback runs.
//
#define MODBUSMQ_ERR_TRANSPORT (-1)
#define MODBUSMQ_ERR_PROTOCOL  (-2)
#define MODBUSMQ_ERR_TIMEOUT   (-3)
#define MODBUSMQ_ERR_EXCEPTION (-4)

#ifdef __cplusplus
extern "C" {
#endif

    // FORWARD DECL
struct modbusmq_input_t;
struct modbusmq_channel_t;
struct modbusmq_write_t;
struct modbusmq_write_channel_t;

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
    // or -1 when there is no connection.
    //
    // sleep_time is in/out: pass your own ceiling in milliseconds and it comes
    // back lowered to whenever the next subscription is due, or unchanged if
    // none is due sooner. Pass 0 for no ceiling of your own and get the 1000 ms
    // default.
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

//
// Describes either a MODBUSMQ_ERR_* code or an errno — the library's codes are
// negative and an errno is not, so one function covers both without ambiguity.
// Modbus exception codes from a device are a separate namespace and are not
// handled here; read those with modbusmq_frame_exception_code() and name them
// with modbusmq_exception_string().
//
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
    // The exception code of a response, guarded. Returns 1..255 only when the
    // frame really is a complete exception reply, and 0 for anything else.
    //
    // This is the one to call from an error callback.
    // modbusmq_frame_error_code() reads the byte at the exception code's
    // offset whatever is there, and a write echo or a half-read frame has
    // something else at that offset that looks just as much like a code.
    //
extern int         modbusmq_frame_exception_code(struct modbusmq_context_t *context, modbusmq_frame_t *frame);

    //
    // Names a Modbus exception code: "illegal data address" for 2. Never 0;
    // an unknown code comes back as "unknown exception".
    //
extern const char *modbusmq_exception_string(int code);

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
// decimal count the same way for modbusmq and anything else that
// prints a channel: an explicit channel.decimals wins, otherwise a bit
// channel gets 0, a float format gets 3, and an integer format follows its
// mod divisor (mod=-100 -> 2 decimals, and so on) — see libmodbusmq.c for the
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
// is documented on the implementation in libmodbusmq.c.
//
// Returns 1 = publish, 0 = suppress, < 0 on bad arguments.
//
extern int   modbusmq_channel_publish_decide(struct modbusmq_channel_t *channel, float value, const char *text, millitime_t now_ms);

//
// Text channels: serial numbers, firmware versions, timestamps.
//
// These exist because the numeric path cannot carry them. modbusmq_read_channel()
// returns a float, and a float has 24 mantissa bits: an eight-digit serial
// number (10^8 > 2^24) loses its low digits outright, and an epoch-seconds
// timestamp (~2^31) quantises to steps of about two minutes. So a text format
// decodes from the wire bytes straight to characters and never touches a float
// on the way.
//
// modbusmq_format_is_text() is the one predicate everything else branches on:
// a caller reads a channel with modbusmq_read_channel_text() when it says yes
// and modbusmq_read_channel() when it says no. Both hand the result to
// modbusmq_channel_publish_decide() unchanged — that function already judges
// "unchanged" on the printed text, so on_change/min_interval/max_interval work
// on a serial number exactly as they do on a voltage. min_change and
// min_change_rel are magnitude gates and do not apply; they are rejected at
// parse time rather than silently ignored.
//
// modbusmq_read_channel_text() writes a NUL-terminated string and returns the
// characters written, or < 0 when the channel is out of range, the bytes are
// not valid for the format (a BCD nibble above 9, an impossible date), or the
// buffer is too small. A channel that fails is skipped, not published: a
// half-decoded serial number is worse than none.
//
// buf should be MODBUSMQ_TEXT_MAX bytes.
//
extern int   modbusmq_format_is_text(int format);
    //
    // Narrower questions about the same set. is_time() is what channel.timefmt
    // applies to and what still honours channel.add (as an epoch shift);
    // is_epoch() is the subset carrying a real instant, and so the only one
    // channel.timezone can meaningfully convert.
    //
extern int   modbusmq_format_is_time(int format);
extern int   modbusmq_format_is_epoch(int format);
extern int   modbusmq_read_channel_text(struct modbusmq_context_t *context, modbusmq_msg_t *msg, const struct modbusmq_input_t *input, const struct modbusmq_channel_t *channel, char *buf, size_t len);

    //
    // The inverse, for modbusmq_server: encode a channel's textual default
    // (channel.value) into channel->length wire bytes. Pads the way the format
    // expects — NUL for ascii, zero digits for bcd. Returns bytes written,
    // < 0 when the text does not fit or does not suit the format.
    //
extern int   modbusmq_encode_text(const struct modbusmq_channel_t *channel, const char *text, uint8_t *out, size_t len);

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
    //
    // The same thing for one channel of a block write, which carries its own
    // format and scaling rather than the write entry's.
    // Returns the register count (1 or 2), < 0 on error.
    //
extern int   modbusmq_write_channel_encode(struct modbusmq_context_t *context, const struct modbusmq_write_t *write, const struct modbusmq_write_channel_t *channel, double value, uint16_t *regs);
    //
    // Lay a whole block of values out as the registers of one function 16
    // write. values holds one engineering-unit value per channel, in channel
    // order; pass nvalues < 0 to take each channel's configured default
    // instead. regs must hold write->naddress registers.
    //
    // Every register of the block is written, so the caller never has to think
    // about what happens to the ones no channel claims: the config validator
    // has already refused a block with a gap in it.
    //
    // Returns the register count, < 0 on error.
    //
extern int   modbusmq_write_encode_block(struct modbusmq_context_t *context, const struct modbusmq_write_t *write, const double *values, int nvalues, uint16_t *regs, int nregs);

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
