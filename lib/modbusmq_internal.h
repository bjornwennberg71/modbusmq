//////////////////////////////////////////////////////////////////////////////
// 
// bjornwennberg71@gmail.com
// 
// modbusmq_internal.h
// 
#ifndef modbusmq_internal_h_
#define modbusmq_internal_h_

// INCLUDES //////////////////////////////////////////////////////////////////
#include "modbusmq.h"

#define MODBUSMQ_READ_COILS               0x01 
#define MODBUSMQ_READ_DISCRETE_INPUTS     0x02 
#define MODBUSMQ_READ_HOLDING_REGISTERS   0x03 
#define MODBUSMQ_READ_INPUT_REGISTERS     0x04 
#define MODBUSMQ_WRITE_SINGLE_COIL        0x05 
#define MODBUSMQ_WRITE_SINGLE_REGISTER    0x06 
#define MODBUSMQ_READ_EXCEPTION_STATUS    0x07
#define MODBUSMQ_WRITE_MULTIPLE_COILS     0x0F 
#define MODBUSMQ_WRITE_MULTIPLE_REGISTERS 0x10 
#define MODBUSMQ_REPORT_SLAVE_ID          0x11
#define MODBUSMQ_MASK_WRITE_REGISTER      0x16
#define MODBUSMQ_WRITE_AND_READ_REGISTERS 0x17

#define MODBUSMQ_BROADCAST_ADDRESS 0

#ifdef __cplusplus
extern "C" {
#endif

//
// wrap one modbusmq_msg_t
// this is part of a linked list, especially used for subscriptions
//
typedef struct modbusmq_msg_wrapper_t
{
    int                   flags;
    modbusmq_msg_t          msg;
    struct modbusmq_msg_wrapper_t *next;
} modbusmq_msg_wrapper_t;


//
// timer used for modbusmq_subscribe
//
typedef struct modbusmq_timer_t
{
    int          timer_id;
    millitime_t  timer_start;
    millitime_t  timer_next_time;
    millitime_t  timer_interval;
    millitime_t  orig_interval;
    
    modbusmq_msg_t msg;

    struct modbusmq_timer_t *next;
} modbusmq_timer_t;

//
// callbacks to all functions for different transport protocols
//
typedef struct modbusmq_cb_t
{
    void      (*modbusmq_free)            (struct modbusmq_context_t *context);
    int       (*modbusmq_connect)         (struct modbusmq_context_t *context);
    
    int       (*modbusmq_frame_init)      (struct modbusmq_context_t *context, modbusmq_frame_t *frame, int function, int addr, int nb);
    int       (*modbusmq_msg_prepare)     (struct modbusmq_context_t *context, modbusmq_msg_t *msg);
    int       (*modbusmq_msg_check)       (struct modbusmq_context_t *context, modbusmq_msg_t *msg);
    int       (*modbusmq_msg_check_header)(struct modbusmq_context_t *context, modbusmq_msg_t *msg);

    int       (*modbusmq_frame_slave)     (struct modbusmq_context_t *context, modbusmq_frame_t *frame);
    int       (*modbusmq_frame_function)  (struct modbusmq_context_t *context, modbusmq_frame_t *frame);
    int       (*modbusmq_frame_error_code)(struct modbusmq_context_t *context, modbusmq_frame_t *frame);
    int       (*modbusmq_frame_addr)      (struct modbusmq_context_t *context, modbusmq_frame_t *frame);
    int       (*modbusmq_frame_naddr)     (struct modbusmq_context_t *context, modbusmq_frame_t *frame);
    int       (*modbusmq_frame_nbytes)    (struct modbusmq_context_t *context, modbusmq_frame_t *frame);
    uint8_t * (*modbusmq_frame_data)      (struct modbusmq_context_t *context, modbusmq_frame_t *frame);

    int       (*modbusmq_read_coil_bits)        (struct modbusmq_context_t *context, modbusmq_frame_t *frame, int addr, int nbits);
    int       (*modbusmq_read_input_bits)       (struct modbusmq_context_t *context, modbusmq_frame_t *frame, int addr, int nbits);
    int       (*modbusmq_read_holding_registers)(struct modbusmq_context_t *context, modbusmq_frame_t *frame, int addr, int nb);
    int       (*modbusmq_read_input_registers)  (struct modbusmq_context_t *context, modbusmq_frame_t *frame, int addr, int nb);
    int       (*modbusmq_write_coil_bit)        (struct modbusmq_context_t *context, modbusmq_frame_t *frame, int addr, int value);
    int       (*modbusmq_write_register)        (struct modbusmq_context_t *context, modbusmq_frame_t *frame, int addr, int value);
    int       (*modbusmq_write_coil_bits)       (struct modbusmq_context_t *context, modbusmq_frame_t *frame, int addr, int nbits, const uint8_t *bits);
    int       (*modbusmq_write_registers)       (struct modbusmq_context_t *context, modbusmq_frame_t *frame, int addr, int nb, const uint16_t *values);
    int       (*modbusmq_write_mask_registers)  (struct modbusmq_context_t *context, modbusmq_frame_t *frame, int addr, int and_mask, int or_mask);
    
    // tcp only
    int  (*modbusmq_tcp_frame_transaction_id)( struct modbusmq_context_t *context, modbusmq_frame_t *frame);
    
    
    
} modbusmq_cb_t;

//
// modbusmq context
//
typedef struct modbusmq_context_t
{
    int                   slave_id;        // device-id
    int                   fd;              // socket/file
    uint64_t              rts_delay_us;
    
    millitime_t           last_write_ms; // timestamp last write
    millitime_t           last_read_ms;  // timestamp last read
    millitime_t           frame_timeout_ms; // max time to wait for a request/response per frame
    
    modbusmq_msg_wrapper_t *msg_wrapper_head; // queue of requests
    modbusmq_timer_t       *timer_head;       // queue of repeating requests

    int                   req_header_min;       // minimum number of bytes of header needed to compute the length
    int                   res_header_min;       // minimum number of bytes of header needed to compute the length

                         // a frame was discarded, so the stream may still hold
                         // bytes belonging to it. Drain to silence before the
                         // next request goes out, or its response gets read
                         // through the leftovers of the previous one.
    int                   resync_pending;
    
                         // callback for post_message
    void                 (*message_cb)(struct modbusmq_context_t *, modbusmq_msg_t *);
    
                         // callback for repeating subscriptions
    void                 (*subscribe_cb)(struct modbusmq_context_t *, modbusmq_msg_t *, struct modbusmq_input_t *input);
    
                         // callback for errors
    void                 (*error_cb)(struct modbusmq_context_t *, modbusmq_msg_t *);

    struct modbusmq_config_t      *config;
    struct modbusmq_tcp_context_t *tcp;
    struct modbusmq_rtu_context_t *rtu;
    
    int                 header_length;

    // the following variables are used to keep track of sent frames, received frames, and error-frames
    uint32_t            tx;
    uint32_t            rx;
    uint32_t            err;

    modbusmq_cb_t         cb;
} modbusmq_context_t;

extern void modbusmq_config_close();

extern int modbusmq_read(modbusmq_context_t *context, int fd, modbusmq_frame_t *frame);
extern int modbusmq_write(modbusmq_context_t *context, int fd, modbusmq_frame_t *frame);

extern int modbusmq_frame_incomplete(modbusmq_context_t *context, modbusmq_frame_t *frame);

#ifdef __cplusplus
}
#endif

#endif // modbusmq_internal_h_
