//////////////////////////////////////////////////////////////////////////////
// 
// bjornwennberg71@gmail.com
// 
// modbusmq_rtu.h
// 
#ifndef modbusmq_rtu_h_
#define modbusmq_rtu_h_

#ifdef __cplusplus
extern "C" {
#endif

#define MODBUSMQ_DEVICE_MAX 100

//
// Minimum inter-frame silence, in microseconds. Modbus specifies 3.5 character
// times, but fixes it at 1.750 ms for baud rates above 19200.
//
#define MODBUSMQ_RTU_QUIET_MIN_US 1750
typedef struct modbusmq_rtu_context_t
{
    char        device[100];
    int         baud;
    char        parity;
    int         databit;
    int         stopbit;
    uint64_t    onebyte_delay_us;
    uint64_t    frame_delay_us;
} modbusmq_rtu_context_t;

extern int  modbusmq_rtu_frame_init               (modbusmq_context_t *context, modbusmq_frame_t *frame, int function, int addr, int nb);
extern void modbusmq_rtu_free                     (modbusmq_context_t *rtu);
extern int  modbusmq_rtu_connect                  (modbusmq_context_t *context);
extern int  modbusmq_rtu_write                    (modbusmq_context_t *context, int fd, modbusmq_frame_t *frame);
extern int  modbusmq_rtu_read                     (modbusmq_context_t *context, int fd, modbusmq_frame_t *frame);
extern int  modbusmq_rtu_get_termios_speed        (int baud);
extern int  modbusmq_rtu_write_coil_bit           (modbusmq_context_t *context, modbusmq_frame_t *frame, int addr, int value);
extern int  modbusmq_rtu_write_coil_bits          (modbusmq_context_t *context, modbusmq_frame_t *frame, int addr, int nbits, const uint8_t *bits);
extern int  modbusmq_rtu_read_coil_bits           (modbusmq_context_t *context, modbusmq_frame_t *frame, int addr, int nbits);
extern int  modbusmq_rtu_read_input_bits          (modbusmq_context_t *context, modbusmq_frame_t *frame, int addr, int nbits);
extern int  modbusmq_rtu_write_register           (modbusmq_context_t *context, modbusmq_frame_t *frame, int addr, int value);
extern int  modbusmq_rtu_write_registers          (modbusmq_context_t *context, modbusmq_frame_t *frame, int addr, int nb, const uint16_t *values);
extern int  modbusmq_rtu_read_holding_registers   (modbusmq_context_t *context, modbusmq_frame_t *frame, int addr, int nb);
extern int  modbusmq_rtu_read_input_registers     (modbusmq_context_t *context, modbusmq_frame_t *frame, int addr, int nb);
extern int  modbusmq_rtu_write_mask_registers     (modbusmq_context_t *context, modbusmq_frame_t *frame, int addr, int and_mask, int or_mask);

extern int  modbusmq_rtu_msg_prepare              (modbusmq_context_t *context, modbusmq_msg_t *msg);
extern int  modbusmq_rtu_msg_check                (modbusmq_context_t *context, modbusmq_msg_t *msg);
extern int  modbusmq_rtu_msg_check_header         (modbusmq_context_t *context, modbusmq_msg_t *msg);

extern int  modbusmq_rtu_flush                    (modbusmq_context_t *context);

extern int      modbusmq_rtu_frame_slave          (modbusmq_context_t *context, modbusmq_frame_t *frame);
extern int      modbusmq_rtu_frame_function       (modbusmq_context_t *context, modbusmq_frame_t *frame);
extern uint8_t *modbusmq_rtu_frame_data           (modbusmq_context_t *context, modbusmq_frame_t *frame);
extern int      modbusmq_rtu_frame_addr           (modbusmq_context_t *context, modbusmq_frame_t *frame);
extern int      modbusmq_rtu_frame_naddr          (modbusmq_context_t *context, modbusmq_frame_t *frame);
extern int      modbusmq_rtu_frame_nbytes         (modbusmq_context_t *context, modbusmq_frame_t *frame);
extern int      modbusmq_rtu_frame_error_code     (modbusmq_context_t *context, modbusmq_frame_t *frame);

#ifdef __cplusplus
}
#endif

#endif // modbusmq_rtu_h_
