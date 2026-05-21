//////////////////////////////////////////////////////////////////////////////
// 
// bjornwennberg71@gmail.com
// 
// modbusmq_tcp.c
//
// TCP header is always 6 bytes long. After that, the regular RTU message start
// req[0]  = transaction_id >> 8
// req[1]  = transaction_id & 0x00ff
// req[2]  = 0 protocol
// req[3]  = 0 procotol
// req[4]  = message-length >> 8      [total message-length minus header(6 bytes)]
// req[5]  = message-length & 0x00ff
//
// req[6]  = slave-id
// req[7]  = function
// req[8]  = address-start >> 8
// req[9]  = address-start & 0x00ff
// req[10] = number of addresses >> 8
// req[11] = number of addresses & 0x00ff
// req[12..] = data (optional)
// length = 12 bytes
//
// TCP res (6 bytes header)
// res[0] = transaction_id >> 8
// res[1] = transaction_id & 0x00ff
// res[2] = 0 protocol
// res[3] = 0 procotol
// res[4] = message-length >> 8     [total message-length minus header (6 bytes)]
// res[5] = message-length & 0x00ff
//
// res[6] = slave-id
// res[7] = function
// res[8] = number of bytes
// res[9..n] = data
//
//
// 
#include "modbusmq_internal.h"
#include "modbusmq_tcp.h"
#include "modbusmq_log.h"

#include <stdlib.h>
#include <assert.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>

#include <sys/ioctl.h>
#include <netdb.h>

    

//////////////////////////////////////////////////////////////////////////////
// 
// Allocate tcp context
//
modbusmq_context_t *
modbusmq_tcp_context(const char *pzConnectString)
{

    if (!pzConnectString)
    {
        errno = EINVAL;
        return NULL;
    }

    modbusmq_connect_t
        connect;
    int
        rc,
        len;
    
    rc = modbusmq_parse_connect_string(pzConnectString, &connect);
    if (rc != 0)
    {
        modbusmq_logf(LOG_ERROR, "Unable to parse connect-string\n");
        return NULL;
    }

    //
    // allocate modbusmq_context
    //
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
        .modbusmq_free             = modbusmq_tcp_free,
        .modbusmq_connect          = modbusmq_tcp_connect,
        
        .modbusmq_msg_prepare      = modbusmq_tcp_msg_prepare,
        .modbusmq_msg_check        = modbusmq_tcp_msg_check,
        .modbusmq_msg_check_header = modbusmq_tcp_msg_check_header,

        .modbusmq_frame_init       = modbusmq_tcp_frame_init,
        .modbusmq_frame_slave      = modbusmq_tcp_frame_slave,
        .modbusmq_frame_function   = modbusmq_tcp_frame_function,
        .modbusmq_frame_error_code = modbusmq_tcp_frame_error_code,
        .modbusmq_frame_addr       = modbusmq_tcp_frame_addr,
        .modbusmq_frame_naddr      = modbusmq_tcp_frame_naddr,
        .modbusmq_frame_nbytes     = modbusmq_tcp_frame_nbytes,
        .modbusmq_frame_data       = modbusmq_tcp_frame_data,

        .modbusmq_read_coil_bits          = modbusmq_tcp_read_coil_bits,
        .modbusmq_read_input_bits         = modbusmq_tcp_read_input_bits,
        .modbusmq_read_holding_registers  = modbusmq_tcp_read_holding_registers,
        .modbusmq_read_input_registers    = modbusmq_tcp_read_input_registers,
        .modbusmq_write_coil_bit          = modbusmq_tcp_write_coil_bit,
        .modbusmq_write_register          = modbusmq_tcp_write_register,
        .modbusmq_write_coil_bits         = modbusmq_tcp_write_coil_bits,
        .modbusmq_write_registers         = modbusmq_tcp_write_registers,
        .modbusmq_write_mask_registers    = modbusmq_tcp_write_mask_registers,
    
        // tcp only
        .modbusmq_tcp_frame_transaction_id = modbusmq_tcp_frame_transaction_id,
        
    };
        


    context->slave_id = 0xff;

    //
    // allocate modbusmq_tcp_context
    //
    context->tcp     = (modbusmq_tcp_context_t *)malloc(sizeof(modbusmq_tcp_context_t));

    assert(context->tcp);
    if (!context->tcp)
    {
        perror("Unable to allocate modbusmq_tcp context");
        exit(2);
    }
    memset(context->tcp, 0, sizeof(modbusmq_tcp_context_t));

    // header length for TCP is 8 bytes (transaction[2] + protocol[2] + length[2] + slave[1] + function[1])
    context->header_length = 8;

    len = strlen(connect.device);
    if (len >= MODBUSMQ_HOSTNAME_MAX)
    {
        len = MODBUSMQ_HOSTNAME_MAX;
    }
    strncpy(context->tcp->hostname, connect.device, len);
    context->tcp->hostname[len] = 0;
        
    context->tcp->port = connect.port;
    if (context->tcp->port <= 0)
    {
        context->tcp->port = 502;
    }

    // response:
    // byte[5] and byte[6] is the message-length for both request and response
    context->res_header_min = context->req_header_min = 6;
    
    return context;
}

//////////////////////////////////////////////////////////////////////////////
// 
//
void
modbusmq_tcp_free(modbusmq_context_t *context)
{
    if (!context || !context->tcp)
    {
        return;
    }

    free(context->tcp);
    context->tcp = NULL;
}

//////////////////////////////////////////////////////////////////////////////
// 
// not sure why this would be needed
//
int
modbusmq_tcp_flush(modbusmq_context_t *context)
{
    return 0;
}


int
modbusmq_tcp_write(modbusmq_context_t *context, int fd, modbusmq_frame_t *frame)
{
    int
        rc;


    rc = write(fd, frame->buf + frame->xmit, frame->length - frame->xmit);
    
    if (rc < 0)
    {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            rc = 0;
        }
    }
    else
    {
        frame->xmit += rc;
    }
    
    return rc;
       
}


// internal function to check the header parameters
int
modbusmq_tcp_frame_check(modbusmq_context_t *context, modbusmq_frame_t *frame)
{
    // writing
    if (frame->is_writer == 0)
    {
        // TODO: Add support for req-write functions
        if (frame->xmit >= 12) // we are ok
        {
                
        }
        else
        {
        }
    }
    else // reader
    {
        if (frame->xmit >= 6)
        {
            int body_length = frame->buf[4] >> 8 | frame->buf[5];
            int msg_length   = body_length + 6; // add header
            if (frame->length != msg_length)
            {
                frame->length = msg_length;
            }
        }
    }
    
    return 0;
}

int
modbusmq_tcp_read(modbusmq_context_t *context, int fd, modbusmq_frame_t *frame)
{
    int
        rc;
    if (frame->length == 0)
    {
        frame->length = 12;
    }
    
    rc = read(fd, frame->buf + frame->xmit, frame->length - frame->xmit);

    
    if (rc < 0)
    {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            rc = 0;
        }
    }
    else
    {
        frame->xmit += rc;
    }
    modbusmq_tcp_frame_check(context, frame);
    
    return rc;
       
}


//////////////////////////////////////////////////////////////////////////////
// 
// connect to a server
// external function
//
int
modbusmq_tcp_connect(modbusmq_context_t *context)
{
    int
        rc,
        len;
    
    if (!context)
    {
        errno = EINVAL;
        return -1;
    }

    modbusmq_tcp_context_t
        *tcp = context->tcp;
    
    
    //
    // resolve hostname
    //
    struct addrinfo
        addr_hints,
        *addr_result,
        *addr_rp;
    char
        port[20];

    sprintf(port, "%d", tcp->port);
    
    memset(&addr_hints, 0, sizeof(addr_hints));
    addr_hints.ai_family   = AF_UNSPEC;
    addr_hints.ai_socktype = SOCK_STREAM;
    addr_hints.ai_flags    = 0; 
    addr_hints.ai_protocol = 0;

    // int
    //     socket_flags = SOCK_CLOEXEC | SOCK_NONBLOCK;
    
    rc = getaddrinfo(tcp->hostname, port, &addr_hints, &addr_result);
    if (rc != 0)
    {
        modbusmq_logf(LOG_ERROR, "Unable to resolve address: %s, err=%s\n", tcp->hostname, gai_strerror(rc));
        return -1;
    }
    
    int
        sd = -1;
    
    for (addr_rp = addr_result; addr_rp; addr_rp = addr_rp->ai_next)
    {
        sd = socket(addr_rp->ai_family, addr_rp->ai_socktype, addr_rp->ai_protocol);
        if (sd < 0)
        {
            continue;
        }

        rc = connect(sd, addr_rp->ai_addr, addr_rp->ai_addrlen);
        if (rc < 0)
        {
            modbusmq_logf(LOG_ERROR, "Connect failed: rc = %d, err=%s\n", rc, strerror(errno));
            close(sd);
            return -1;
        }
        else if (rc == 0)
        {
            break;                  /* Success */
        }
    }
        
    if (sd < 0 || rc < 0)
    {
        if (sd > 0)
        {
            close(sd);
        }
        modbusmq_logf(LOG_ERROR, "Unable to connect to %s:%s, err=%s\n", tcp->hostname, port, strerror(rc));
        return -1;
    }
    
    context->fd = sd;


    
    // now turn on flags
    {
        int flags = fcntl(context->fd, F_GETFL);
        if (flags < 0)
        {
            modbusmq_logf(LOG_ERROR, "Unable to query flags from connect socket\n");
            return -1;
        }

        flags |= FD_CLOEXEC;
        flags |= O_NONBLOCK;

        flags = fcntl(context->fd, F_SETFL, flags);
        if (flags < 0)
        {
            modbusmq_logf(LOG_ERROR, "Unable to set flags on socket: CLOEXEC | NONBLOCK\n");
            return -1;
        }
    }


    
    return 0;
}



//////////////////////////////////////////////////////////////////////////////
// 
// return length of message or < 0 on error
// internal function
int
modbusmq_tcp_frame_init(modbusmq_context_t *context, modbusmq_frame_t *frame, int function, int addr, int nb)
{
    uint8_t
        *req = frame->buf;

    memset(frame, 0, sizeof(modbusmq_frame_t));
    frame->is_writer = 1;
    frame->is_tcp    = 1;
    
    // TCP - will be filled in during post_message
    req[0] = 0;
    req[1] = 0;
    
    // protocol modbusmq
    req[2] = 0;
    req[3] = 0;

    // will be filled out during post_message
    req[4] = 0;
    req[5] = 0;

    req[6] = context->slave_id; // set the slave id of the device
    req[7] = function;          // set the function to call
    req[8] = addr >> 8;         // set the start address
    req[9] = addr & 0x00ff;
    
    req[10] = nb >> 8;          // set the number of addresses
    req[11] = nb & 0x00ff;

    frame->length = 12; // standard req header size for tcp

    // pre-fill out response info

    return frame->length; 
}

//////////////////////////////////////////////////////////////////////////////
// 
// prepare a message for sending
// calculate the PDU length and put it into the req
// internal function
//
int
modbusmq_tcp_msg_prepare(modbusmq_context_t *context, modbusmq_msg_t *msg)
{
    modbusmq_tcp_context_t
        *tcp       = context->tcp;

    tcp->transaction_id++;
    tcp->transaction_id %= 0xffff;

    modbusmq_frame_t *writer = msg->frame[0].is_writer ? &msg->frame[0] : &msg->frame[1];
    modbusmq_frame_t *reader = msg->frame[1].is_writer == 0 ? &msg->frame[1] : &msg->frame[0];
    
    // set the transaction id
    writer->buf[0] = tcp->transaction_id >> 8;
    writer->buf[1] = tcp->transaction_id & 0x00ff;

    // set the length (minus the tcp header=6)
    writer->buf[4] = (writer->length - 6) >> 8;
    writer->buf[5] = (writer->length - 6) & 0x00ff;

    //
    // prepare the response
    //

    // copy transaction id
    reader->buf[0] = writer->buf[0];
    reader->buf[1] = writer->buf[1];

    // modbusmq protocol is always 0
    reader->buf[2] = 0;
    reader->buf[3] = 0;

    // copy slave-id
    reader->buf[6] = writer->buf[6]; // slave
    // copy function
    reader->buf[7] = writer->buf[7]; // function
    
    return writer->length;
}

//////////////////////////////////////////////////////////////////////////////
// 
// get the transaction id
// internal function
int
modbusmq_tcp_frame_transaction_id( modbusmq_context_t *context, modbusmq_frame_t *frame)
{
    uint16_t
        transaction_id;
    
    transaction_id = frame->buf[0] << 8 | (frame->buf[1] & 0x00ff);

    return transaction_id;
}

//////////////////////////////////////////////////////////////////////////////
// 
// get slave id from message
int
modbusmq_tcp_frame_slave(modbusmq_context_t *context, modbusmq_frame_t *frame)
{
    return frame->buf[6];
}

//////////////////////////////////////////////////////////////////////////////
//
// internal function
// request data starts at byte 12
// response data start at byte 9
uint8_t *
modbusmq_tcp_frame_data(modbusmq_context_t *context, modbusmq_frame_t *frame)
{
    if (frame->is_writer)
    {
        return frame->buf + 12;
    }
    return frame->buf + 9;
}

//////////////////////////////////////////////////////////////////////////////
// 
// internal function
// 
int
modbusmq_tcp_frame_function( modbusmq_context_t *context, modbusmq_frame_t *frame)
{
    return frame->buf[7];
}

//////////////////////////////////////////////////////////////////////////////
//
// request address
// internal function
//
// address start
//
int
modbusmq_tcp_frame_addr(modbusmq_context_t *context, modbusmq_frame_t *frame)
{
    if (frame->is_writer)
    {
        uint16_t
            address = (frame->buf[8] << 8) | frame->buf[9];
        return address;
    }
    assert(0);
    return 0;
}

//////////////////////////////////////////////////////////////////////////////
//
// request naddr
//
// internal function
//
// number of addresses
int
modbusmq_tcp_frame_naddr(modbusmq_context_t *context, modbusmq_frame_t *frame)
{
    if (frame->is_writer)
    {
        uint16_t
            value = (frame->buf[10] << 8) | frame->buf[11];
        return value;
    }
    assert(0);
    return 0;
}

//////////////////////////////////////////////////////////////////////////////
//
// return number of data-bytes
//
// internal function
//
// number of bytes in response
int
modbusmq_tcp_frame_nbytes(modbusmq_context_t *context, modbusmq_frame_t *frame)
{
    if (!frame->is_writer)
    {
        return frame->buf[8];
    }
    assert(0);
    return 0;
}

//////////////////////////////////////////////////////////////////////////////
//
// internal function
// response 
// return error code. If function res[7] >= 0x80, then the following byte is the
// error-code
// 
int
modbusmq_tcp_frame_error_code( modbusmq_context_t *context, modbusmq_frame_t *frame)
{
    if (!frame->is_writer)
    {
        return frame->buf[8];
    }
    assert(0);
    return 0;
}

//////////////////////////////////////////////////////////////////////////////
// 
// Function 01 (01hex) Read Coils
// internal function
int
modbusmq_tcp_read_coil_bits(modbusmq_context_t *context, modbusmq_frame_t *frame, int addr, int nbits)
{
    return modbusmq_tcp_frame_init(context, frame, MODBUSMQ_READ_COILS, addr, nbits);
}

//////////////////////////////////////////////////////////////////////////////
// 
// Function 02(02hex) Read Discrete Inputs
// internal function
int
modbusmq_tcp_read_input_bits(modbusmq_context_t *context, modbusmq_frame_t *frame, int addr, int nbits)
{
    return modbusmq_tcp_frame_init(context, frame, MODBUSMQ_READ_DISCRETE_INPUTS, addr, nbits);
}

//////////////////////////////////////////////////////////////////////////////
// 
// Function 03 (03hex) Read Holding Registers
// internal function
int
modbusmq_tcp_read_holding_registers(modbusmq_context_t *context, modbusmq_frame_t *frame, int addr, int nb)
{
    return modbusmq_tcp_frame_init(context, frame, MODBUSMQ_READ_HOLDING_REGISTERS, addr, nb);
}

//////////////////////////////////////////////////////////////////////////////
// 
// Function 04 (04hex) Read Input Registers
// internal function
int
modbusmq_tcp_read_input_registers(modbusmq_context_t *context, modbusmq_frame_t *frame, int addr, int nb)
{
    return modbusmq_tcp_frame_init(context, frame, MODBUSMQ_READ_INPUT_REGISTERS, addr, nb);
}

//////////////////////////////////////////////////////////////////////////////
// 
// Function 05 (05hex) Write Single Coil
//
// The requested ON / OFF state is specified by a constant in the request data field. A value of FF 00 hex requests the coil to be ON. A value of 00 00 requests it to be OFF. All other values are illegal and will not affect the coil.
// internal function
int
modbusmq_tcp_write_coil_bit(modbusmq_context_t *context, modbusmq_frame_t *frame, int addr, int value)
{
    return modbusmq_tcp_frame_init(context, frame, MODBUSMQ_WRITE_SINGLE_COIL, addr, value ? 0xff00 : 0);
}

//////////////////////////////////////////////////////////////////////////////
// 
// Function 06 (06hex) Write Single Register
// internal function
// 
int
modbusmq_tcp_write_register(modbusmq_context_t *context, modbusmq_frame_t *frame, int addr, int value)
{
    return modbusmq_tcp_frame_init(context, frame, MODBUSMQ_WRITE_SINGLE_REGISTER, addr, value);
}

//////////////////////////////////////////////////////////////////////////////
//
// Function 15 (0Fhex) Write Multiple Coils
// internal function
int
modbusmq_tcp_write_coil_bits(modbusmq_context_t *context, modbusmq_frame_t *frame, int addr, int nbits, const uint8_t *bits)
{
    return modbusmq_tcp_frame_init(context, frame, MODBUSMQ_WRITE_MULTIPLE_COILS, addr, nbits);
}



//////////////////////////////////////////////////////////////////////////////
// 
// Function 16 (10hex) Write Multiple Registers
//
// addr: address first register
// nb: number of registers
// values: uint16 values
// internal function
//
int
modbusmq_tcp_write_registers(modbusmq_context_t *context, modbusmq_frame_t *frame, int addr, int nb, const uint16_t *values)
{
    return modbusmq_tcp_frame_init(context, frame, MODBUSMQ_WRITE_MULTIPLE_REGISTERS, addr, nb);
}

//////////////////////////////////////////////////////////////////////////////
// 
// Function 22 (16hex) Write mask registers
//
// addr: address first register
//
int
modbusmq_tcp_write_mask_registers( struct modbusmq_context_t *context, modbusmq_frame_t *frame, int addr, int and_mask, int or_mask)
{
    modbusmq_tcp_frame_init(context, frame, MODBUSMQ_WRITE_MULTIPLE_REGISTERS, addr, and_mask);
    
    frame->buf[frame->length++] = or_mask >> 8;
    frame->buf[frame->length++] = or_mask & 0x00ff;

    return frame->length;
}

//////////////////////////////////////////////////////////////////////////////
// 
// check partial received header
int
modbusmq_tcp_msg_check_header(modbusmq_context_t *context, modbusmq_msg_t *msg)
{
    if (!context || !msg)
    {
        return -1;
    }
    modbusmq_frame_t *writer = msg->frame[0].is_writer ? &msg->frame[0] : &msg->frame[1];
    modbusmq_frame_t *reader = msg->frame[1].is_writer == 0 ? &msg->frame[1] : &msg->frame[0];
    
    // check transaction id
    if (reader->xmit >= 2)
    {
        if (writer->buf[0] == reader->buf[0] && writer->buf[1] == reader->buf[1])
        {
            ; // transaction id OK
        }
        else
        {
            modbusmq_logf(LOG_ERROR, "res: transaction id incorrect\n");
            return -2;
        }
    }

    // check protocol == 0
    if (reader->xmit >= 4)
    {
        if (reader->buf[2] == 0 && reader->buf[3] == 0 &&
            writer->buf[2] == 0 && writer->buf[3] == 0)
        {
            ; // OK
        }
        else
        {
            modbusmq_logf(LOG_ERROR, "protocol != 0\n");
            return -3; // protocol != 0
        }
    }
    
    // check and adjust res_length
    // normal to ask for 10 values, but only receive 8 values and thus we must adjust the expected res_length
    if (reader->xmit >= 6)
    {
        // message length minus header (6 bytes)
        int
            msg_length = (reader->buf[4] << 8) | reader->buf[5];
        msg_length += 6; // add tcp header
        
        if (msg_length != reader->xmit)
        {
            if (msg_length > 6)
            {
                if (msg_length != reader->length)
                {
                    reader->length = msg_length;
                    //modbusmq_logf(LOG_INFO, "Setting new res_length=%d\n", msg_length);
                }
            }
            else
            {
                modbusmq_logf(LOG_ERROR, "res_length mismatch: res_length = %d\n", msg_length);
                return -4;
            }
        }
    }
    
    // check slave-id
    if (reader->xmit >= 7)
    {
        if (writer->buf[6] == reader->buf[6])
        {
            ; // OK
        }
        else
        {
            modbusmq_logf(LOG_ERROR, "slave id mismatch\n");
            return -5; // slave id not the same
        }
    }

    // check function
    if (reader->xmit >= 8)
    {
        if (writer->buf[7] == reader->buf[7])
        {
            ; // OK
        }
        else
        {
            if (reader->buf[7] > 0x80)
            {
                // error
                modbusmq_logf(LOG_ERROR, "error in response[code=%d]: Please check request!\n", reader->buf[8]);
            }
            else
            {
                modbusmq_logf(LOG_ERROR, "function not the same: req[%d] != res[%d]\n", writer->buf[7], reader->buf[8]);
            }            
            return -6; // error in frame
        }
    }

    return 0; // all OK
}


//////////////////////////////////////////////////////////////////////////////9
// 
// Check req + res and make sure they are correct
// internal function
//
int
modbusmq_tcp_msg_check(modbusmq_context_t *context, modbusmq_msg_t *msg)
{
    modbusmq_frame_t *writer = msg->frame[0].is_writer ? &msg->frame[0] : &msg->frame[1];
    modbusmq_frame_t *reader = msg->frame[1].is_writer == 0 ? &msg->frame[1] : &msg->frame[0];
    
    int rc = modbusmq_tcp_msg_check_header(context, msg);

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
    
    return 0;
}
