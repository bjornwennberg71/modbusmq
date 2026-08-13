//////////////////////////////////////////////////////////////////////////////
// 
// bjornwennberg71@gmail.com
// 
// modbusmq_tcp.h
// 
#ifndef modbusmq_tcp_h_
#define modbusmq_tcp_h_

// INCLUDES //////////////////////////////////////////////////////////////////
#include "modbusmq.h"

#define MODBUSMQ_HOSTNAME_MAX 200

//
// data specific for tcp
//
typedef struct modbusmq_tcp_context_t
{
    char                hostname[MODBUSMQ_HOSTNAME_MAX+1];
    int                 port;
    int                 transaction_id;
} modbusmq_tcp_context_t;

extern int modbusmq_tcp_connect(struct modbusmq_context_t *context);
extern int modbusmq_tcp_write  (struct modbusmq_context_t *context, int fd, modbusmq_frame_t *writer);
extern int modbusmq_tcp_read   (struct modbusmq_context_t *context, int fd, modbusmq_frame_t *reader);

extern int  modbusmq_tcp_frame_init               (modbusmq_context_t *context, modbusmq_frame_t *msg, int function, int addr, int nb);
extern void modbusmq_tcp_free                     (modbusmq_context_t *context);
extern int  modbusmq_tcp_write_coil_bit           (modbusmq_context_t *context, modbusmq_frame_t *frame, int addr, int value);
extern int  modbusmq_tcp_write_coil_bits          (modbusmq_context_t *context, modbusmq_frame_t *frame, int addr, int nbits, const uint8_t *bits);
extern int  modbusmq_tcp_read_coil_bits           (modbusmq_context_t *context, modbusmq_frame_t *frame, int addr, int nbits);
extern int  modbusmq_tcp_read_input_bits          (modbusmq_context_t *context, modbusmq_frame_t *frame, int addr, int nbits);
extern int  modbusmq_tcp_write_register           (modbusmq_context_t *context, modbusmq_frame_t *frame, int addr, int value);
extern int  modbusmq_tcp_write_registers          (modbusmq_context_t *context, modbusmq_frame_t *frame, int addr, int nb, const uint16_t *values);
extern int  modbusmq_tcp_read_holding_registers   (modbusmq_context_t *context, modbusmq_frame_t *frame, int addr, int nb);
extern int  modbusmq_tcp_read_input_registers     (modbusmq_context_t *context, modbusmq_frame_t *frame, int addr, int nb);
extern int  modbusmq_tcp_write_mask_registers     (modbusmq_context_t *context, modbusmq_frame_t *frame, int addr, int and_mask, int or_mask);
extern int      modbusmq_tcp_msg_prepare        (modbusmq_context_t *context, modbusmq_msg_t *msg);
extern int  modbusmq_tcp_msg_check                (modbusmq_context_t *context, modbusmq_msg_t *msg);
extern int  modbusmq_tcp_msg_check_header         (modbusmq_context_t *context, modbusmq_msg_t *msg);
extern int  modbusmq_tcp_frame_drain              (modbusmq_context_t *context, modbusmq_frame_t *frame);

extern int      modbusmq_tcp_frame_transaction_id (modbusmq_context_t *context, modbusmq_frame_t *frame);
extern int      modbusmq_tcp_frame_slave          (modbusmq_context_t *context, modbusmq_frame_t *frame);
extern int      modbusmq_tcp_frame_function       (modbusmq_context_t *context, modbusmq_frame_t *frame);
extern uint8_t *modbusmq_tcp_frame_data           (modbusmq_context_t *context, modbusmq_frame_t *frame);
extern int      modbusmq_tcp_frame_addr           (modbusmq_context_t *context, modbusmq_frame_t *writer);
extern int      modbusmq_tcp_frame_naddr          (modbusmq_context_t *context, modbusmq_frame_t *writer);
extern int      modbusmq_tcp_frame_nbytes         (modbusmq_context_t *context, modbusmq_frame_t *reader);
extern int      modbusmq_tcp_frame_error_code     (modbusmq_context_t *context, modbusmq_frame_t *reader);

#endif // modbusmq_tcp_h_
