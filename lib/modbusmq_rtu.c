//////////////////////////////////////////////////////////////////////////////
//
// bjornwennberg71@gmail.com
//
// modbusmq_rtu.c
//
// This page contains detailed information with examples of various write/read
// requestsd
//
// https://www.fernhillsoftware.com/help/drivers/modbusmq/modbusmq-protocol.html
//
// RTU req frame to read:
//    req[0] = context->slave_id;
//    req[1] = function;
//    req[2] = addr >> 8;        // address
//    req[3] = addr & 0x00ff;
//    req[4] = naddr >> 8;       // number of addresses
//    req[5] = naddr & 0x00ff;
//    optional: req[6...] = data (if any)
//    req[max-1] = crc1
//    req[max]   = crc2
//
// RTU req frame to write
// 6 bytes + data + 2 (crc) = minumum 8 bytes
//
// RTU res frame to read
//    res[0] = slave
//    res[1] = function
//    res[2] = number of bytes
//    res[3..nb] data
//    res[nb+1] = crc & 0x00ff
//    res[nb+2] = crc >> 8
//
// minumum bytes to read is 3 in order to read the 'number of bytes'
// 
// error res: TODO: CHECK
//
// res[0] = slave
// res[1] = function + 0x80
// res[x] = crc
// res[x+1] = crc


// INCLUDES //////////////////////////////////////////////////////////////////
#include "modbusmq_internal.h"
#include "modbusmq_rtu.h"
#include "modbusmq_log.h"

#include <stdlib.h>
#include <assert.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <termios.h>
#include <sys/ioctl.h>


//////////////////////////////////////////////////////////////////////////////
// 
// Allocate rtu context
//
modbusmq_context_t *
modbusmq_rtu_context(const char *device, int baud, char parity, int databit, int stopbit)
{

    if (!device || baud <= 0 || databit < 0 || stopbit < 0)
    {
        errno = EINVAL;
        return NULL;
    }

    
    // allocate modbusmq_context
    modbusmq_context_t
        *context = (modbusmq_context_t *)malloc(sizeof(modbusmq_context_t));
    assert(context);
    if (!context)
    {
        perror("Unable to allocate modbusmq context");
        exit(2);
    }
    memset(context, 0, sizeof(modbusmq_context_t));
    context->cb = (modbusmq_cb_t){
        .modbusmq_free             = modbusmq_rtu_free,
        .modbusmq_connect          = modbusmq_rtu_connect,
        
        .modbusmq_msg_prepare      = modbusmq_rtu_msg_prepare,
        .modbusmq_frame_init       = modbusmq_rtu_frame_init,
        .modbusmq_frame_slave      = modbusmq_rtu_frame_slave,
        .modbusmq_frame_function   = modbusmq_rtu_frame_function,
        .modbusmq_frame_error_code = modbusmq_rtu_frame_error_code,
        .modbusmq_frame_addr       = modbusmq_rtu_frame_addr,
        .modbusmq_frame_naddr      = modbusmq_rtu_frame_naddr,
        .modbusmq_frame_nbytes     = modbusmq_rtu_frame_nbytes,
        .modbusmq_frame_data       = modbusmq_rtu_frame_data,
        .modbusmq_msg_check        = modbusmq_rtu_msg_check,
        .modbusmq_msg_check_header = modbusmq_rtu_msg_check_header,

        .modbusmq_read_coil_bits         = modbusmq_rtu_read_coil_bits,
        .modbusmq_read_input_bits        = modbusmq_rtu_read_input_bits,
        .modbusmq_read_holding_registers = modbusmq_rtu_read_holding_registers,
        .modbusmq_read_input_registers   = modbusmq_rtu_read_input_registers,
        .modbusmq_write_coil_bit         = modbusmq_rtu_write_coil_bit,
        .modbusmq_write_register         = modbusmq_rtu_write_register,
        .modbusmq_write_coil_bits        = modbusmq_rtu_write_coil_bits,
        .modbusmq_write_registers        = modbusmq_rtu_write_registers,
        .modbusmq_write_mask_registers   = modbusmq_rtu_write_mask_registers,
    
    };
        
    context->slave_id = 0xff;

    // allocate modbusmq_rtu_context
    context->rtu     = (modbusmq_rtu_context_t *)malloc(sizeof(modbusmq_rtu_context_t));
    assert(context->rtu);
    if (!context->rtu)
    {
        perror("Unable to allocate modbusmq_rtu context");
        exit(2);
    }
    memset(context->rtu, 0, sizeof(modbusmq_rtu_context_t));

    strncpy(context->rtu->device, device, MODBUSMQ_DEVICE_MAX);
    context->rtu->baud = baud;
    context->rtu->parity = parity;
    context->rtu->databit = databit;
    context->rtu->stopbit = stopbit;

    /* Calculate estimated time in micro second to send one byte */
    context->rtu->onebyte_delay_us =
        1000000 * (1 + databit + (parity == 'N' ? 0 : 1) + stopbit) / baud;

    /* The delay before and after transmission when toggling the RTS pin */
    context->rtu->frame_delay_us = context->rtu->onebyte_delay_us;

    context->rts_delay_us = context->rtu->onebyte_delay_us;
    
    // header length for RTU is 2 bytes (slave[1] + function[1] + nbytes)
    context->header_length = 3;

    context->req_header_min = 8; // you need the complete request header with crc
    context->res_header_min = 3; // byte[2] contains the number of bytes in response
    return context;
}


//////////////////////////////////////////////////////////////////////////////
// 
//
void
modbusmq_rtu_free(modbusmq_context_t *context)
{
    if (!context || !context->rtu)
    {
        return;
    }

    free(context->rtu);
    context->rtu = NULL;
}


//////////////////////////////////////////////////////////////////////////////
// 
//
int
modbusmq_rtu_flush(modbusmq_context_t *context)
{
//    printf("modbusmq_rtu_flush START\n");
    if (!context)
    {
        errno = EINVAL;
        assert(context);
        return -1;
    }

    char
        c;
    int
        rc = 0;

    int ntotal = 0;
    
    while(rc = read(context->fd, &c, 1) > 0)
    {
        ntotal++;
    }

    // if (ntotal)
    // {
    //     printf("modbusmq_rtu: flushed %d bytes\n", ntotal);
    // }
    
    return ntotal;
}

//////////////////////////////////////////////////////////////////////////////
//
// Map numeric baud rate to termios speed_t.
//
// Returns B9600 as a fallback if the requested baud is not supported.
//
int
modbusmq_rtu_get_termios_speed(int baud)
{
    struct baud_map_entry
    {
        int     baud;
        speed_t speed;
    };

    /* Ordered from low to high, guarded by #ifdef for portability */
    static const struct baud_map_entry baud_map[] =
    {
        { 110,    B110    },
        { 300,    B300    },
        { 600,    B600    },
        { 1200,   B1200   },
        { 2400,   B2400   },
        { 4800,   B4800   },
        { 9600,   B9600   },
        { 19200,  B19200  },
        { 38400,  B38400  },
#ifdef B57600
        { 57600,  B57600  },
#endif
#ifdef B115200
        { 115200, B115200 },
#endif
#ifdef B230400
        { 230400, B230400 },
#endif
#ifdef B460800
        { 460800, B460800 },
#endif
#ifdef B500000
        { 500000, B500000 },
#endif
#ifdef B576000
        { 576000, B576000 },
#endif
#ifdef B921600
        { 921600, B921600 },
#endif
#ifdef B1000000
        { 1000000, B1000000 },
#endif
#ifdef B1152000
        { 1152000, B1152000 },
#endif
#ifdef B1500000
        { 1500000, B1500000 },
#endif
#ifdef B2500000
        { 2500000, B2500000 },
#endif
#ifdef B3000000
        { 3000000, B3000000 },
#endif
#ifdef B3500000
        { 3500000, B3500000 },
#endif
#ifdef B4000000
        { 4000000, B4000000 },
#endif
    };

    speed_t speed = B9600; /* default fallback */
    unsigned int i;

    for (i = 0U; i < (sizeof(baud_map) / sizeof(baud_map[0])); ++i)
    {
        if (baud_map[i].baud == baud)
        {
            speed = baud_map[i].speed;
            break;
        }
    }

    return (int)speed;
}

//////////////////////////////////////////////////////////////////////////////
//
// Simple RTS control helper for half-duplex RS-485.
//
// This toggles the TIOCM_RTS modem control bit explicitly.
//
void
modbusmq_rtu_ioctl_rts(int fd, int on)
{
    int flags;

    if (ioctl(fd, TIOCMGET, &flags) != 0)
    {
        return;
    }

    if (on)
    {
        flags |= TIOCM_RTS;
    }
    else
    {
        flags &= ~TIOCM_RTS;
    }

    (void)ioctl(fd, TIOCMSET, &flags);
}

//////////////////////////////////////////////////////////////////////////////
//
// CRC-16 (Modbusmq) calculation for RTU frames.
//
// Polynomial: 0xA001
// Init value: 0xFFFF
// RefIn/RefOut: true (this is the standard Modbusmq CRC-16)
//
uint16_t
modbusmq_rtu_crc16(const uint8_t *buf, int buflen)
{
    uint16_t crc = 0xFFFF;

    while (buflen-- > 0)
    {
        crc ^= (uint16_t)(*buf++);

        /* Process 8 bits */
        for (int i = 0; i < 8; ++i)
        {
            if (crc & 0x0001U)
            {
                crc >>= 1;
                crc ^= 0xA001U;
            }
            else
            {
                crc >>= 1;
            }
        }
    }

    return crc;
}


//////////////////////////////////////////////////////////////////////////////
// 
// TODO: Since we are using a select() loop to calculate wait time
// add the timing-wait there instead of calling usleep
//
int
modbusmq_rtu_write(modbusmq_context_t *context, int fd, modbusmq_frame_t *frame)
{
    //modbusmq_logf(LOG_INFO, "%s:%d:%s\n", __FILE__, __LINE__, __FUNCTION__);
    modbusmq_rtu_ioctl_rts(fd, 0);
    usleep(context->rts_delay_us);
        
    int
        rc;

    rc = write(fd, frame->buf + frame->xmit, frame->length - frame->xmit);
    
    if (rc < 0)
    {
        // already delayed some using usleep above
        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            rc = 0;
        }
    }
    else
    {
        frame->xmit += rc;
    }
    
    usleep(context->rtu->onebyte_delay_us * (frame->length - frame->xmit) + context->rts_delay_us);
    modbusmq_rtu_ioctl_rts(fd, 1);
    
    //modbusmq_logf(LOG_INFO, "%s:%d:%s rc=%d\n", __FILE__, __LINE__, __FUNCTION__, rc);
    return rc;
}

// internal function to check the header parameters
int
modbusmq_rtu_frame_check(modbusmq_context_t *context, modbusmq_frame_t *frame)
{
    // are we reading?
    if (frame->is_writer == 0)
    {
        // TODO: Add support for req-write functions
        if (frame->xmit >= 8) // we are ok
        {
                
        }
        else
        {
        }
    }
    else // reader
    {
        if (frame->xmit >= 3)
        {
            int new_length = 3 + frame->buf[2] + 2;
            if (new_length < 0 || new_length > 256)
            {
                return -1;
            }
            if (frame->length != new_length)
            {
                frame->length = new_length;
            }
        }
    }
    return 0;
}

int
modbusmq_rtu_read(modbusmq_context_t *context, int fd, modbusmq_frame_t *frame)
{
    int
        rc;

    if (frame->length == 0)
    {
        frame->length = 8;
        frame->xmit   = 0;
    }
    rc = read(fd, frame->buf + frame->xmit, frame->length - frame->xmit);

    if (rc == -1)
    {
        fprintf(stderr, "modbusmq_rtu_read: %d bytes, errno=%d\n", rc, errno);
        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            rc = 0;
        }
    }
    else if (rc > 0)
    {
        //fprintf(stderr, "modbusmq_rtu_read: %d bytes\n", rc);
        frame->xmit += rc;
    }
    else
    {
        ; // fprintf(stderr, "modbusmq_rtu_read: %d bytes, errno=%d\n", rc, errno);
    }
    
    if (modbusmq_rtu_frame_check(context, frame) < 0)
    {
        modbusmq_rtu_flush(context);
        return -1;
    }
    return rc;
       
}
//////////////////////////////////////////////////////////////////////////////
// 
// connect to a device
// internal function
int
modbusmq_rtu_connect(modbusmq_context_t *context)
{
    int rc;
    
    modbusmq_rtu_context_t
        *rtu = context->rtu;
    
    int
        sd = open(rtu->device, O_RDWR | O_NOCTTY | O_NDELAY | O_EXCL | O_CLOEXEC);
    if (sd < 0)
    {
        fprintf(stderr, "Unable to open device: %s, err=%s\n", rtu->device, strerror(errno));
        return -1;
    }

    struct termios
        tios;
    speed_t
        speed;
    memset(&tios, 0, sizeof(struct termios));
    
    rc = tcgetattr(sd, &tios);
    if (rc < 0)
    {
        perror("tcgetattr");
        close(sd);
        return -1;
    }
    
    /*
    On MacOS, constants of baud rates are equal to the integer in argument but
    that's not the case under Linux so we have to find the corresponding
    constant. Until the code is upgraded to termios2, the list of possible
    values is limited (no 14400 for example).
    */
    if (9600 == B9600)
    {
        speed = rtu->baud;
    }
    else
    {
        speed = modbusmq_rtu_get_termios_speed(rtu->baud);
    }

    if ((cfsetispeed(&tios, speed) < 0) || (cfsetospeed(&tios, speed) < 0))
    {
        close(sd);
        return -1;
    }

    /* C_CFLAG      Control options
       CLOCAL       Local line - do not change "owner" of port
       CREAD        Enable receiver
    */
    tios.c_cflag |= (CREAD | CLOCAL);
    /* CSIZE, HUPCL, CRTSCTS (hardware flow control) */

    /* Set data bits (5, 6, 7, 8 bits)
       CSIZE        Bit mask for data bits
    */
    tios.c_cflag &= ~CSIZE;
    switch (rtu->databit)
    {
    case 5:
        tios.c_cflag |= CS5;
        break;
    case 6:
        tios.c_cflag |= CS6;
        break;
    case 7:
        tios.c_cflag |= CS7;
        break;
    case 8:
    default:
        tios.c_cflag |= CS8;
        break;
    }

    /* Stop bit (1 or 2) */
    if (rtu->stopbit == 1)
    {
        tios.c_cflag &= ~CSTOPB;
    }
    else /* 2 */
    {
        tios.c_cflag |= CSTOPB;
    }

    /* PARENB: enable parity bit; PARODD: use odd instead of even */
    if (rtu->parity == 'N')
    {
        tios.c_cflag &= ~PARENB;
    }
    else if (rtu->parity == 'E')
    {
        tios.c_cflag |= PARENB;
        tios.c_cflag &= ~PARODD;
    }
    else
    {
        tios.c_cflag |= PARENB;
        tios.c_cflag |= PARODD;
    }
    
        /* Raw input */
    tios.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
    if (rtu->parity == 'N')
    {
        /* None */
        tios.c_iflag &= ~INPCK;
    }
    else
    {
        tios.c_iflag |= INPCK;
    }
    tios.c_iflag &= ~(IXON | IXOFF | IXANY);
    tios.c_oflag &= ~OPOST;

    tios.c_cc[VMIN] = 0;
    tios.c_cc[VTIME] = 0;

    rc = tcsetattr(sd, TCSANOW, &tios);
    if (rc < 0)
    {
        fprintf(stderr, "Unable to set termios on device %s, rc=%d, err=%s\n", rtu->device, rc, strerror(errno));
        close(sd);
        return -1;
    }

    context->fd = sd;

    return 0;
    /* assert(0); */
    /* return -1; */
}

//////////////////////////////////////////////////////////////////////////////
// 
// get slave id from message
int
modbusmq_rtu_frame_slave(modbusmq_context_t *context, modbusmq_frame_t *frame)
{
    return frame->buf[0];
}

//////////////////////////////////////////////////////////////////////////////
// 
// return function
// internal function
//
int
modbusmq_rtu_frame_function(modbusmq_context_t *context, modbusmq_frame_t *frame)
{
    return frame->buf[1];
}

//////////////////////////////////////////////////////////////////////////////
// 
// request address
// internal function
//
int
modbusmq_rtu_frame_addr(modbusmq_context_t *context, modbusmq_frame_t *frame)
{
    return frame->buf[2] << 8 | frame->buf[3];
}


//////////////////////////////////////////////////////////////////////////////
// 
// number of addresses
// internal function
// 
int
modbusmq_rtu_frame_naddr(modbusmq_context_t *context, modbusmq_frame_t *frame)
{
    return frame->buf[4] << 8 | frame->buf[5];
}

//////////////////////////////////////////////////////////////////////////////
// 
// response number of bytes
// internal function
// response
int
modbusmq_rtu_frame_nbytes(modbusmq_context_t *context, modbusmq_frame_t *frame)
{
    return frame->buf[2];
}

//////////////////////////////////////////////////////////////////////////////
// 
// response error code
// internal function
int
modbusmq_rtu_frame_error_code( modbusmq_context_t *context, modbusmq_frame_t *frame)
{
    return frame->buf[2];
}

//////////////////////////////////////////////////////////////////////////////
// 
// internal function
uint8_t *
modbusmq_rtu_frame_data( modbusmq_context_t *context, modbusmq_frame_t *frame)
{
    if (frame->is_writer)
    {
        return frame->buf + 6;
    }
    return frame->buf + 3;
}


//////////////////////////////////////////////////////////////////////////////
// 
// return length of message or < 0 on error
// internal function
int
modbusmq_rtu_frame_init(modbusmq_context_t *context, modbusmq_frame_t *frame, int function, int addr, int naddr)
{
    memset(frame, 0, sizeof(modbusmq_frame_t));

    frame->is_writer = 1;

    frame->buf[0] = context->slave_id;
    frame->buf[1] = function;
    frame->buf[2] = addr >> 8;         // set the start address
    frame->buf[3] = addr & 0x00ff;
    frame->buf[4] = naddr >> 8;          // set the number of addresses
    frame->buf[5] = naddr & 0x00ff;

    frame->length = 6;
    
    return frame->length; 
}

//////////////////////////////////////////////////////////////////////////////
// 
// prepare a message for sending
// calculate crc and append it to the message
// internal function
//
int
modbusmq_rtu_msg_prepare(modbusmq_context_t *context, modbusmq_msg_t *msg)
{
    modbusmq_frame_t *writer = msg->frame[0].is_writer ? &msg->frame[0] : &msg->frame[1];
    modbusmq_frame_t *reader = msg->frame[1].is_writer == 0 ? &msg->frame[1] : &msg->frame[0];

    uint16_t
        crc = modbusmq_rtu_crc16(writer->buf, writer->length);

    
    // set the length
    // libmodbusmq: According to the MODBUSMQ specs (p. 14), the low order byte of the CRC comes  first in the RTU message
    //
    writer->buf[writer->length++] = crc & 0x00ff;
    writer->buf[writer->length++] = (crc & 0xff00) >> 8;
    
    reader->length = 3; // minumum
    
    return writer->length;
}



//////////////////////////////////////////////////////////////////////////////
//
// Function 01 (01hex) Read Coils
// internal function
int
modbusmq_rtu_read_coil_bits(modbusmq_context_t *context, modbusmq_frame_t *frame, int addr, int nbits)
{
    return modbusmq_rtu_frame_init(context, frame, MODBUSMQ_READ_COILS, addr, nbits);
}

//////////////////////////////////////////////////////////////////////////////
// 
// Function 02(02hex) Read Discrete Inputs
int
modbusmq_rtu_read_input_bits(modbusmq_context_t *context, modbusmq_frame_t *frame, int addr, int nbits)
{
    return modbusmq_rtu_frame_init(context, frame, MODBUSMQ_READ_DISCRETE_INPUTS, addr, nbits);
}


//////////////////////////////////////////////////////////////////////////////
// 
// Function 03 (03hex) Read Holding Registers
// read holding registers
int
modbusmq_rtu_read_holding_registers(modbusmq_context_t *context, modbusmq_frame_t *frame, int addr, int naddr)
{
    return modbusmq_rtu_frame_init(context, frame, MODBUSMQ_READ_HOLDING_REGISTERS, addr, naddr);
}

//////////////////////////////////////////////////////////////////////////////
// 
// Function 04 (04hex) Read Input Registers
// read input registers
int
modbusmq_rtu_read_input_registers(modbusmq_context_t *context, modbusmq_frame_t *frame, int addr, int naddr)
{
    return modbusmq_rtu_frame_init(context, frame, MODBUSMQ_READ_INPUT_REGISTERS, addr, naddr);
}

//////////////////////////////////////////////////////////////////////////////
//
// Function 05 (05hex) Write Single Coil
//
// The requested ON / OFF state is specified by a constant in the request data field. A value of FF 00 hex requests the coil to be ON. A value of 00 00 requests it to be OFF. All other values are illegal and will not affect the coil.
// internal function
//
int
modbusmq_rtu_write_coil_bit(modbusmq_context_t *context, modbusmq_frame_t *frame, int addr, int value)
{
    return modbusmq_rtu_frame_init(context, frame, MODBUSMQ_WRITE_SINGLE_COIL, addr, value ? 0xff00 : 0);
}

//////////////////////////////////////////////////////////////////////////////
// 
// Function 06 (06hex) Write Single Register
// internal function
//
int
modbusmq_rtu_write_register(modbusmq_context_t *context, modbusmq_frame_t *frame, int addr, int value)
{
    return modbusmq_rtu_frame_init(context, frame, MODBUSMQ_WRITE_SINGLE_REGISTER, addr, value);
    
}

//////////////////////////////////////////////////////////////////////////////
//
// Function 15 (0Fhex) Write Multiple Coils
//
// internal function
int
modbusmq_rtu_write_coil_bits(modbusmq_context_t *context, modbusmq_frame_t *frame, int addr, int nbits, const uint8_t *bits)
{
    return modbusmq_rtu_frame_init(context, frame, MODBUSMQ_WRITE_MULTIPLE_COILS, addr, nbits);
}

//////////////////////////////////////////////////////////////////////////////
// 
// Function 16 (10hex) Write Multiple Registers
// 
// addr: address first register
// nb: number of registers
// values: uint16 values
//
int
modbusmq_rtu_write_registers(modbusmq_context_t *context, modbusmq_frame_t *frame, int addr, int naddr, const uint16_t *values)
{
    return modbusmq_rtu_frame_init(context, frame, MODBUSMQ_WRITE_MULTIPLE_REGISTERS, addr, naddr);
}

//////////////////////////////////////////////////////////////////////////////
// 
// Function 22 (16hex) Write mask registers
//
// addr: address first register
//
int
modbusmq_rtu_write_mask_registers( struct modbusmq_context_t *context, modbusmq_frame_t *frame, int addr, int and_mask, int or_mask)
{
    modbusmq_rtu_frame_init(context, frame, MODBUSMQ_WRITE_MULTIPLE_REGISTERS, addr, and_mask);
    frame->buf[frame->length++] = or_mask >> 8;
    frame->buf[frame->length++] = or_mask & 0x00ff;

    return frame->length;
}


//////////////////////////////////////////////////////////////////////////////
// 
// check partial res header
int
modbusmq_rtu_msg_check_header(modbusmq_context_t *context, modbusmq_msg_t *msg)
{
    if (!context || !msg)
    {
        return -1;
    }

    modbusmq_frame_t *writer = msg->frame[0].is_writer ? &msg->frame[0] : &msg->frame[1];
    modbusmq_frame_t *reader = msg->frame[1].is_writer == 0 ? &msg->frame[1] : &msg->frame[0];

    //fprintf(stderr, "BEFORE reader->length=%d\n", reader->length);
    // not enough header data to check header
    if (reader->xmit < 3)
    {
        return 1;
    }
    
    // check if slave is correct
    if (reader->xmit >= 1 && writer->buf[0] == reader->buf[0])
    {
        ; // ok
    }
    else
    {
        modbusmq_logf(LOG_ERROR, "slave id different req[%d] != res[%d]. action: discard frame\n", writer->buf[0], reader->buf[0]);
        return -2; 
    }

    // check function
    if (reader->xmit >= 2 && writer->buf[1] == reader->buf[1])
    {
        ; // OK
    }
    else
    {
        modbusmq_logf(LOG_ERROR, "function is different req[%02X] != res[%02X]. action: discard frame\n", writer->buf[1], reader->buf[1]);
        return -3;
    }

    // check length
    if (reader->xmit >= 3)
    {
        int newlen = reader->buf[2] + context->header_length + 2;
        
        // check and ajust res_length
        if (newlen != reader->length)
        {
            // this is normal. we first read 3 bytes to get the header and then we know how long the frame is
            
            // modbusmq_logf(LOG_INFO, "short frame received: %d bytes, expected %d bytes\n", newlen, reader->length);
            // CRC check will be performed when complete frame has been received and either discard or allow this frame
            reader->length = newlen;

            //fprintf(stderr, "BEFORE reader->length=%d\n", reader->length);
            return reader->length - reader->xmit;
        }
    }
    
    return reader->length - reader->xmit;
}


//////////////////////////////////////////////////////////////////////////////
//
// Check req + res and make sure they are correct
// internal function
//
int
modbusmq_rtu_msg_check(modbusmq_context_t *context, modbusmq_msg_t *msg)
{

    modbusmq_frame_t *writer = msg->frame[0].is_writer ? &msg->frame[0] : &msg->frame[1];
    modbusmq_frame_t *reader = msg->frame[1].is_writer == 0 ? &msg->frame[1] : &msg->frame[0];
    
    int rc = modbusmq_rtu_msg_check_header(context, msg);

    if (rc < 0)
    {
        return rc;
    }
    
    // incomplete message
    if (writer->xmit != writer->length)
    {
        return 1;
    }
    else if (reader->xmit != reader->length)
    {
        return 2;
    }

    // crc occupies 2 bytes
    int crc_res     = reader->buf[reader->length-1] << 8 | reader->buf[reader->length-2];
    int crc_compute = modbusmq_rtu_crc16(reader->buf, reader->length - 2);
    if (crc_res != crc_compute)
    {
        fprintf(stderr, "modbusmq_check: CRC check failed for response\n");
        fprintf(stderr, "modbusmq_check: req CRC != res CRC\n");
        fprintf(stderr, "req[slave][%d] res[slave][%d]\n", writer->buf[0], reader->buf[0]);
        fprintf(stderr, "req[func][%d]  res[func][%d]\n",  writer->buf[1], reader->buf[1]);
        fprintf(stderr, "req[address][0x%02X]\n",          (writer->buf[2] << 8 | writer->buf[3]));
        return -2;
    }
    
    return 0;
}

//////////////////////////////////////////////////////////////////////////////
// 
// external function
int
modbusmq_rtu_rts_delay(struct modbusmq_context_t *context, int interval_us)
{
    if (!context || interval_us < 0)
    {
        errno = EINVAL;
        return -1;
    }

    if (context->rtu)
    {
        context->rts_delay_us = interval_us;
    }

    return 0;
}

        
