//////////////////////////////////////////////////////////////////////////////
// 
// bjornwennberg71@gmail.com
// 
// modbusmq.c
//

// INCLUDES ////////////////////////////////////////////////////////////////
#include "modbusmq_internal.h"
#include "modbusmq_tcp.h"
#include "modbusmq_rtu.h"
#include "modbusmq_log.h"
#include "modbusmq_config.h"

#include <stdlib.h>
#include <assert.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <math.h>

#include <sys/ioctl.h>
#include <netdb.h>
#include <poll.h>

static int modbusmq_debug = 0;

/**
 * 
 * @brief set the debug level
 * 
 * @param nlevel: 0=off, > 0 increases verbosity level. level=1 and level=2 is in use
 */
void
modbusmq_set_debug(int nlevel)
{
    modbusmq_debug = nlevel;
}

/**
 * 
 * 
 * @brief returns the debug level
 * @return debug-level
 */
int
modbusmq_get_debug()
{
    return modbusmq_debug;
}

/**
 * @brief sets context->config to config
 * 
 * @param context: allocated context
 *  @param config: modbusmq config
 *
 */ 
int
modbusmq_set_config(struct modbusmq_context_t *context, struct modbusmq_config_t *config)
{
    if (!context)
    {
        errno = EINVAL;
        return -1;
    }

    context->config = config;
    return 0;
}


/**
 * 
 * @brief set the slave id
 * 
 * @param context: allocated context
 * @param slave_id: slave id to communicate with device
 *
 * @return 0 on success
 */
int
modbusmq_set_slave(modbusmq_context_t *context, int slave_id)
{
    if (!context)
    {
        errno = EINVAL;
        return -1;
    }

    context->slave_id = slave_id;
    return 0;
}

/**
 * 
 * @brief get the slave id
 * 
 * @param context: allocated context
 * @return slave_id or < 0 on error
 */
int
modbusmq_get_slave(modbusmq_context_t *context)
{
    if (!context)
    {
        errno = EINVAL;
        return -1;
    }

    return context->slave_id;
}


/**
 * 
 * @brief sets the frame_timeout in milliseconds
 * If a frame uses longer time to send/receive as specified here, then the library gives up
 * sending/receiving and returns an error in whichever function you were using
 * 
 * @param context: allocated context
 * @param timeout_ms: milliseconds to wait for a complete send/receive
 *
 * @return 0 on success or < 0 on error
 */
int
modbusmq_frame_timeout(struct modbusmq_context_t *context, int timeout_ms)
{

    if (!context || timeout_ms < 0)
    {
        errno = EINVAL;
        return -1;
    }
    
    context->frame_timeout_ms = timeout_ms;
    
    return 0;
}


/**
 * 
 * @brief frees up allocated resources
 * 
 * @param context: allocated context to free
 */
void
modbusmq_free(modbusmq_context_t *context)
{
    if (!context)
    {
        return;
    }

    if (context->config)
    {
        modbusmq_config_close();
        context->config = NULL;
    }

    context->cb.modbusmq_free(context);
    
    if (context->fd > 0)
    {
        close(context->fd);
        context->fd = 0;
    }

    {
        modbusmq_msg_wrapper_t *elem = context->msg_wrapper_head;
        while(elem)
        {
            modbusmq_msg_wrapper_t *next = elem->next;
            free(elem);
            elem = next;
        }
    }

    {
        modbusmq_timer_t
            *elem = context->timer_head,
            *next = NULL;
        while(elem)
        {
            if (!next)
            {
                next = elem->next;
            }
            free(elem);
            elem = next;
            if (next)
            {
                next = next->next;
            }
        }
    }
    
    free(context);
}

/**
 * 
 * @brief sets the callback to be executed when a message is received from the device
 *
 * For normal messages only.

 * @param context: allocated context
 * @param pointer to void function to be called
 */
void
modbusmq_set_message_callback(modbusmq_context_t *context,  void (*message_cb)(modbusmq_context_t *context, modbusmq_msg_t *msg))
{
    if (!context)
    {
        errno = EINVAL;
        return;
    }

    context->message_cb = message_cb;
}

/**
 * 
 * @brief sets the subscribe callback function to be called when a frame has been received from the device
 *
 * For subscription messages only
 * 
 * @param context: allocated context
 * @param void function that will be executed when a frame has been received
 */
void
modbusmq_set_subscription_callback(modbusmq_context_t *context,  void (*subscribe_cb)(modbusmq_context_t *context, modbusmq_msg_t *msg, modbusmq_input_t *input))
{
    if (!context)
    {
        errno = EINVAL;
        return;
    }

    context->subscribe_cb = subscribe_cb;
}

/**
 * 
 * @brief connects to the device specified in the context
 * 
 * @param context: allocated context
 * @return 0 on success, < 0 on error
 */
int
modbusmq_connect(modbusmq_context_t *context)
{
    if (!context)
    {
        errno = EINVAL;
        return -1;
    }

    return context->cb.modbusmq_connect(context);
    
}

/**
 * 
 * @brief close connection
 * 
 * @param context: allocated context
 * @return 0 on success, < 0 on error
 */ 
int
modbusmq_close(modbusmq_context_t *context)
{
    if (!context)
    {
        errno = EINVAL;
        return -1;
    }

    close(context->fd);
    context->fd = 0;
    return 0;
}

/**
 * 
 * @brief maps errno into string
 *
 * Simply calls strerror(nerrno).
 * TODO: Add own errors to the string which are unique to libmodbusmq
 *
 * @param nerrno: errno
 *
 * @return string description of errno
 */ 
const char *
modbusmq_strerror(int nerrno)
{
    return strerror(nerrno);
}


/**
 * 
 * @brief decodes and prints as much information as possible about a frame
 * 
 * @param context: allocated context
 * @param frame: frame to debug
 */
void
modbusmq_frame_debug(modbusmq_context_t *context, modbusmq_frame_t *frame)
{
    char
        azLine[2000];
    azLine[0] = 0;
    if (frame->xmit >= 0)
    {
        int nxmit = frame->xmit;
        if (nxmit > MODBUSMQ_FRAME_MAX)
        {
            nxmit = MODBUSMQ_FRAME_MAX;
        }
        
        char *ptr = azLine;
        if (frame->is_writer)
        {
            ptr += sprintf(azLine, "writer: xmit=%d length=%d ", frame->xmit, frame->length);
        }
        else
        {
            ptr += sprintf(azLine, "reader: xmit=%d length=%d ", frame->xmit, frame->length);
        }
        
        for(int i = 0; i < nxmit; ++i)
        {
            if (frame->is_writer)
            {
                ptr += sprintf(ptr, "[%.2X]", frame->buf[i]);
            }
            else
            {
                ptr += sprintf(ptr, "<%.2X>", frame->buf[i]);
            }
                
        }
        
        ptr += sprintf(ptr, "\n");
    }
    else
    {
        sprintf(azLine, "xmit = %d\n", frame->xmit);
    }
    modbusmq_logf(LOG_INFO, azLine);
}

/**
 * 
 * @brief read 2 bytes in order ab, and cast to float
 * 
 * @param data: 2 bytes data of ints
 *
 * @return float
 */ 
float
modbusmq_float_ab(const uint8_t *data)
{
    return (data[0] << 8 | data[1]);
}

/**
 * 
 * @brief read 2 bytes in order ba, and cast to float
 * 
 * @param data: 2 bytes data of ints
 *
 * @return float
 */ 
float
modbusmq_float_ba(const uint8_t *data)
{
    return (data[1] << 8 | data[0]);
}

/**
 * 
 * @brief reads out 2 bytes in ab order and converts to int
 * 
 * @param data: 2 bytes of data of ints
 * @return converted value
 */
int
modbusmq_read_int16_ab(const uint8_t *data)
{
    return (data[0] << 8 | data[1]);
}

/**
 * 
 * @brief reads out 2 bytes in ba order and converts to int
 * 
 * @param data: 2 bytes of data of ints
 * @return converted value
 */
int
modbusmq_read_int16_ba(const uint8_t *data)
{
    return (data[1] << 8 | data[0]);
}

/**
 * 
 * @brief reads out 4 bytes in abcd order and combines to an int
 * 
 * @param data: 4 bytes buffer
 * @return int
 */ 
int32_t
modbusmq_read_int32_abcd(const uint8_t *data)
{
    uint8_t a, b, c, d;
    a = data[0] & 0xff;
    b = data[1] & 0xff;
    c = data[2] & 0xff;
    d = data[3] & 0xff;
    uint32_t value = (a << 24) | (b << 16) | (c << 8) | (d << 0);
    return value;
}

/**
 * 
 * @brief reads out 4 bytes in badc order and combines to an int
 * 
 * @param data: 4 bytes buffer
 * @return int
 */ 
int32_t
modbusmq_read_int32_badc(const uint8_t *data)
{
    uint8_t a, b, c, d;
    a = data[0] & 0xff;
    b = data[1] & 0xff;
    c = data[2] & 0xff;
    d = data[3] & 0xff;
    uint32_t value = (b << 24) | (a << 16) | (d << 8) | (c << 0);
    return value;
}

int32_t
modbusmq_read_int32_dcba(const uint8_t *data)
{
    int32_t i;
    uint8_t a, b, c, d;

    a = data[0];
    b = data[1];
    c = data[2];
    d = data[3];

    i = (d << 24) | (c << 16) | (b << 8) | (a << 0);

    return i;
}
    
/**
 * 
 * @brief reads out 4 bytes in abcd order and combines to an int then convert to float
 * 
 * @param data: 4 bytes buffer
 * @return float
 */ 
float
modbusmq_read_float_abcd(const uint8_t *data)
{
    float f;
    uint32_t i = modbusmq_read_int32_abcd(data);
    
    memcpy(&f, &i, 4);

    return f;
}

/**
 * 
 * @brief reads out 4 bytes in badc order and combines to an int then convert to float
 * 
 * @param data: 4 bytes buffer
 * @return float
 */ 
float
modbusmq_read_float_badc(const uint8_t *data)
{
    float f;
    uint32_t i = modbusmq_read_int32_badc(data);
    
    memcpy(&f, &i, 4);

    return f;
}

/**
 * 
 * @brief reads out 4 bytes in dcba order and combines to an int then convert to float
 * 
 * @param data: 4 bytes buffer
 * @return float
 */ 
float
modbusmq_read_float_dcba(const uint8_t *data)
{
    float f;
    uint32_t i = modbusmq_read_int32_dcba(data);
    
    memcpy(&f, &i, 4);

    return f;
}

/**
 * 
 * @brief converts data from msg using input and channel to get a float value
 * this is a utility function used to print default value in type of float for all channels
 *
 *
 * @param context: allocated context
 * @param msg    : modbusmq message
 * @param input  : input definition
 * @param channel: channel definition
 *
 * @return float value
 */ 
float
modbusmq_read_channel(modbusmq_context_t *context, modbusmq_msg_t *msg, const modbusmq_input_t *input, const modbusmq_channel_t *channel)
{
    modbusmq_config_t
        *config  = modbusmq_config_get();
    uint8_t
        *data    = modbusmq_frame_data(context, &msg->frame[1]);
    int
        value_len = 4;
    int
        offset = (channel->offset  - input->address_offset) * config->offset_size;
    float
        f = 0;
    
    switch(channel->format)
    {
        case ModbusmqDataFormat_float_abcd:  f = modbusmq_read_float_abcd(data + offset); break;
        case ModbusmqDataFormat_float_badc:  f = modbusmq_read_float_badc(data + offset); break;
        case ModbusmqDataFormat_float_dcba:  f = modbusmq_read_float_dcba(data + offset); break;
        case ModbusmqDataFormat_ab:          f = modbusmq_read_int16_ab(data + offset);  value_len = 2; break;
        case ModbusmqDataFormat_ba:          f = modbusmq_read_int16_ba(data + offset); value_len = 2;break;
        case ModbusmqDataFormat_abcd:        f = modbusmq_read_int32_abcd(data + offset); break;
        case ModbusmqDataFormat_badc:        f = modbusmq_read_int32_badc(data + offset); break;
        default:
            modbusmq_logf(LOG_ERROR, "Unhandled data format: %d\n", (int)channel->format);
            assert(0);
            break;
    }
    
    f = (f + channel->add);
    if (channel->mod > 0)
    {
        f *= channel->mod;
    }
    else if (channel->mod < 0)
    {
        f /= fabs(channel->mod);
    }
    if (channel->mul != 0)
    {
        f *= channel->mul;
    }
    
    return f;
}

/**
 * 
 * @brief parse protocol:*hostname:port
 * 
 * example:
 * tcp:*localhost:port
 *
 * TODO: Avoid using strtok as this behaves differently on embedded platforms
 *
 * @param input-string: typically from argv that specifies how to connect to the device
 * @param connect: user allocated connect class that will be populated.
 *
 * @return 0 on success, < 0 on error
 */ 
int
modbusmq_parse_connect_string(const char *input_string, struct modbusmq_connect_t *connect)
{
    if (!input_string || !connect)
    {
        errno = EINVAL;
        return -1;
    }
    
    char
        azConnectString[200];

    char
        *ptr;
    int
        len;

    memset(connect, 0, sizeof(modbusmq_connect_t));
    
    memset(azConnectString, 0, sizeof(azConnectString));
    strncpy(azConnectString, input_string, sizeof(azConnectString)-1);
    
    ptr = strtok(azConnectString, "://");
    if (!ptr)
    {
        modbusmq_logf(LOG_ERROR, "Invalid connect-string, missing protocol: %s\n", input_string);
        return -1;
    }
    if (strstr(ptr, "tcp"))
    {
        connect->connect_type = MODBUSMQ_CONNECT_TCP;
    }
    else if (strstr(ptr, "rtu"))
    {
        connect->connect_type = MODBUSMQ_CONNECT_RTU;
    }
    else if (strstr(ptr, "mqtt"))
    {
        connect->connect_type = MODBUSMQ_CONNECT_MQTT;
    }
    else
    {
        fprintf(stderr, "Unknown protocol in connect-string: %s\n", ptr);
        return -2; // unknown connect type
    }
    

    // for some reason, strtok does not work the same way on MIPS32 5.4.72

    char
        buf[200];
    if (connect->connect_type == MODBUSMQ_CONNECT_TCP)
    {
        int matched = sscanf(input_string, "tcp://%255[^:]:%d", connect->device, &connect->port);

        if (matched == 2)
        {
            return 0;
        }
        return -2;
    }
    else if (connect->connect_type == MODBUSMQ_CONNECT_RTU)
    {
        // rtu:///device:baud:stopbit:databits:parity
        int matched = sscanf(input_string, "rtu://%255[^:]:%d:%d:%d:%c", connect->device, &connect->baudrate, &connect->stopbit, &connect->databits, &connect->parity);
        
        if (modbusmq_debug)
        {
            printf("Device=%s\n", connect->device);
            printf("Baudrate=%d\n", connect->baudrate);
            printf("Stopbit=%d\n", connect->stopbit);
            printf("Databits=%d\n", connect->databits);
            printf("Parity=%c\n", connect->parity);
        }
        
        if (matched == 5)
        {
            return 0;
        }
        
        return -2;
    }
    else if (connect->connect_type == MODBUSMQ_CONNECT_MQTT)
    {

        int matched = sscanf(input_string, "mqtt://%255[^:]:%d", connect->device, &connect->port);

        // connect->port = strtod(buf, NULL);
        if (modbusmq_debug)
        {
            printf("mqtt device = %s\n", connect->device);
            printf("mqtt port   = %d\n", connect->port);
        }
        
    }
    else
    {
        fprintf(stderr, "Unknown protocol: %s\n", ptr);
        return -1;
    }
    return 0;
}


/**
 * 
 * @brief generic init of a frame 
 * 
 * @param context: allocated context
 * @param frame: the frame to init
 * @param function: which function to use
 * @param addr: address
 * @param naddr: number of addresses
 *
 * @return length of message on success, < 0 on error
 */ 
int
modbusmq_frame_init(modbusmq_context_t *context, modbusmq_frame_t *frame, int function, int addr, int naddr)
{
    if (!context || !frame)
    {
        errno = EINVAL;
        return -1;
    }

    // a bit overkill to memset all
    memset(frame, 0, sizeof(*frame));
    
    return context->cb.modbusmq_frame_init(context, frame, function, addr, naddr);
}

/**
 * 
 * @brief prepare a message for sending
 * 
 * @param context: allocated context
 * @param msg: the message to prepare
 *
 * @return 0 on success, < 0 on error
 */ 
int
modbusmq_msg_prepare(modbusmq_context_t *context, modbusmq_msg_t *msg)
{
    if (!context || !msg)
    {
        errno = EINVAL;
        return -1;
    }

    return context->cb.modbusmq_msg_prepare(context, msg);
}

/**
 * 
 * @brief get the slave id from a frame
 * 
 * @param context: allocated context
 * @param frame: the frame to parse for a slave id

 * @return the slave id, < 0 on error
 */
int
modbusmq_frame_slave(modbusmq_context_t *context, modbusmq_frame_t *frame)
{
    if (!context || !frame)
    {
        errno = EINVAL;
        return -1;
    }
    
    return context->cb.modbusmq_frame_slave(context, frame);
}

/**
 * 
 * @brief get the transaction id for tcp frames
 * 
 * @param context: allocated context
 * @param frame: frame to decode
 *
 * @return transaction id or < 0 on error
 */ 
int
modbusmq_frame_transaction_id(modbusmq_context_t *context, modbusmq_frame_t *frame)
{
    if (!context || !frame)
    {
        errno = EINVAL;
        return -1;
    }

    if (context->tcp)
    {
        return context->cb.modbusmq_tcp_frame_transaction_id(context, frame);
    }
    else if (context->rtu)
    {
        // RTU does not have transaction id
        return -1;
        assert(0);
    }
}


/**
 * 
 * @brief find the function in a frame
 * 
 * @param context: allocated context
 * @param frame: the frame to decode
 *
 * @return function or < 0 on error
 */ 
int
modbusmq_frame_function(modbusmq_context_t *context, modbusmq_frame_t *frame)
{
    if (!context || !frame)
    {
        errno = EINVAL;
        return -1;
    }
    
    return context->cb.modbusmq_frame_function(context, frame);
}

/**
 * 
 * @brief get the error code from the frame (response)
 * 
 * @param context: allocated context
 * @param frame: frame to decode
 *
 * @return error code or < 0 on error
 */
int
modbusmq_frame_error_code(struct modbusmq_context_t *context, modbusmq_frame_t *frame)
{
    if (!context || !frame)
    {
        errno = EINVAL;
        return -1;
    }

    return context->cb.modbusmq_frame_error_code(context, frame);
}


/**
 * 
 * @brief find the request address in a frame
 * 
 * @param context: allocated context
 * @param frame: frame to decode
 *
 * @return address  or < 0 on error
 */ 
int
modbusmq_frame_addr(modbusmq_context_t *context, modbusmq_frame_t *frame)
{
    if (!context || !frame)
    {
        errno = EINVAL;
        return -1;
    }

    return context->cb.modbusmq_frame_addr(context, frame);
}

/**
 * 
 * @brief number of requested addresses
 * 
 * @param context: allocated context 
 * @return number of addresses, or < 0 on error
 */
int
modbusmq_naddr(modbusmq_context_t *context, modbusmq_frame_t *frame)
{
    if (!context || !frame)
    {
        errno = EINVAL;
        return -1;
    }

    return context->cb.modbusmq_frame_naddr(context, frame);
}

/**
 * 
 * @brief number of bytes in the response
 * 
 * @param context: allocated context
 * @param frame: frame to decode
 *
 * @return number of bytes in response
 */ 
int
modbusmq_frame_nbytes(modbusmq_context_t *context, modbusmq_frame_t *frame)
{
    if (!context || !frame)
    {
        errno = EINVAL;
        return -1;
    }

    return context->cb.modbusmq_frame_nbytes(context, frame);
}


/**
 * 
 * @brief return the data portion of a frame
 * 
 * @param context: allocated context
 * @param frame: frame to decode
 *
 * @return pointer to data or NULL on error
 */ 
uint8_t *
modbusmq_frame_data(modbusmq_context_t *context, modbusmq_frame_t *frame)
{
    if (!context || !frame)
    {
        errno = EINVAL;
        return NULL;
    }

    return context->cb.modbusmq_frame_data(context, frame);
}

/**
 * 
 * @brief Function 01 (01hex) Read Coils
 * 
 * @param context: allocated context
 * @param frame: frame to encode
 * @param addr: request address 
 * @param nbits: request bits
 *
 * @return 0 on success or < 0 on error
 */ 
int
modbusmq_frame_read_coil_bits(modbusmq_context_t *context, modbusmq_frame_t *frame, int addr, int nbits)
{
    if (!context || !frame)
    {
        errno = EINVAL;
        return -1;
    }

    int
        nb = (nbits / 8) + ((nbits%8) ? 1 : 0);
    
    context->cb.modbusmq_read_coil_bits(context, frame, addr, nbits);

    return frame->length;
}

/**
 * 
 * @brief Function 02 (0x02) Read Discrete Inputs.
 * 
 * Builds a request frame for reading a contiguous range of discrete inputs
 * from the slave. The internal callback is used to encode the request into
 * the protocol-specific buffer layout.
 * 
 * @param context: allocated context
 * @param frame: frame to encode
 * @param addr: starting input address
 * @param nbits: number of input bits to read
 * 
 * @return frame length on success, < 0 on error
 */

int
modbusmq_frame_read_input_bits(modbusmq_context_t *context, modbusmq_frame_t *frame, int addr, int nbits)
{
    if (!context || !frame)
    {
        errno = EINVAL;
        return -1;
    }
    
    int
        nb = (nbits / 8) + ((nbits%8) ? 1 : 0);
    context->cb.modbusmq_read_input_bits(context, frame, addr, nbits);

    // header[*] + byte count[1] + number-of-bytes[1]
    //frame->res_length = context->header_length + 1 + nb;
    
    return frame->length;
}

/**
 * 
 * @brief Function 03 (0x03) Read Holding Registers.
 * 
 * Builds a request frame for reading a contiguous range of holding
 * registers from the slave. The internal callback encodes the request
 * into the frame buffer.
 * 
 * @param context: allocated context
 * @param frame: frame to encode
 * @param addr: starting register address
 * @param naddr: number of registers to read
 * 
 * @return frame length on success, < 0 on error
 */

int
modbusmq_frame_read_holding_registers(modbusmq_context_t *context, modbusmq_frame_t *frame, int addr, int naddr)
{
    if (!context || !frame)
    {
        errno = EINVAL;
        return -1;
    }
    
    context->cb.modbusmq_read_holding_registers(context, frame, addr, naddr);

    // header[*] + byte-count[1] - data
    //frame->res_length = context->header_length + 1 + naddr*2;
    
    return frame->length;
}

/**
 * 
 * @brief Function 04 (0x04) Read Input Registers.
 * 
 * Builds a request frame for reading a contiguous range of input registers
 * from the slave. The internal callback encodes the request into the frame
 * buffer.
 * 
 * @param context: allocated context
 * @param frame: frame to encode
 * @param addr: starting input register address
 * @param naddr: number of input registers to read
 * 
 * @return frame length on success, < 0 on error
 */
int
modbusmq_frame_read_input_registers(modbusmq_context_t *context, modbusmq_frame_t *frame, int addr, int naddr)
{
    if (!context || !frame)
    {
        errno = EINVAL;
        return -1;
    }
    
    context->cb.modbusmq_read_input_registers(context, frame, addr, naddr);

    // header[*] + byte count[1] + number-of-records * 2 (each record is 2 byte)
    //frame->res_length = context->header_length + 1 + naddr*2;

    return frame->length;
}


/**
 * 
 * @brief Function 05 (0x05) Write Single Coil.
 * 
 * Builds a request frame for writing a single coil at the specified
 * address. The value is encoded as Modbusmq ON/OFF according to the
 * protocol rules.
 * 
 * @param context: allocated context
 * @param frame: frame to encode
 * @param addr: coil address to write
 * @param value: coil value (0 = OFF, non-zero = ON)
 * 
 * @return frame length on success, < 0 on error
 */

int
modbusmq_frame_write_coil_bit(modbusmq_context_t *context, modbusmq_frame_t *frame, int addr, int value)
{
    if (!context || !frame)
    {
        errno = EINVAL;
        return -1;
    }

    context->cb.modbusmq_write_coil_bit(context, frame, addr, value);
    //frame->res_length = frame->frame[index].buf_length;
    
    return frame->length;
}


/**
 * 
 * @brief Function 06 (0x06) Write Single Register.
 * 
 * Builds a request frame for writing a single holding register at the
 * specified address. The internal callback encodes the 16-bit value into
 * the frame buffer.
 * 
 * @param context: allocated context
 * @param frame: frame to encode
 * @param addr: register address to write
 * @param value: 16-bit register value
 * 
 * @return frame length on success, < 0 on error
 */

int
modbusmq_frame_write_register(modbusmq_context_t *context, modbusmq_frame_t *frame, int addr, int value)
{
    if (!context || !frame)
    {
        errno = EINVAL;
        return -1;
    }
    
    context->cb.modbusmq_write_register(context, frame, addr, value);

    //frame->res_length = frame->frame[index].buf_length;
    return frame->length;
}

/**
 * 
 * @brief Function 15 (0x0F) Write Multiple Coils.
 * 
 * Builds a request frame for writing a contiguous series of coils starting
 * at the given address. The coil states are provided as packed bits in the
 * supplied buffer.
 * 
 * @param context: allocated context
 * @param frame: frame to encode
 * @param addr: starting coil address
 * @param nbits: number of coils to write
 * @param bits: pointer to packed coil states (LSB-first)
 * 
 * @return frame length on success, < 0 on error
 */

int
modbusmq_frame_write_coil_bits(modbusmq_context_t *context, modbusmq_frame_t *frame, int addr, int nbits, const uint8_t *bits)
{
    if (!context || !frame)
    {
        errno = EINVAL;
        return -1;
    }
    
    int
        nb = (nbits / 8) + (nbits%8 ? 1 : 0);
    
    context->cb.modbusmq_write_coil_bits(context, frame, addr, nbits, bits);

    // number of bytes
    frame->buf[frame->length++] = nb;

    //
    // here comes the bits as characters
    // "11110000" => 0xcd
    //

    for(int ibyte = 0; ibyte < nb; ++ibyte)
    {
        uint8_t
            byte = 0;
        for(int ibit = 0; ibit < 8; ++ibit)
        {
            byte |= (*bits++ != '0' ? 1 : 0) << (7-ibit);
        }
        frame->buf[frame->length++] = byte;
    }

    // header[*] + address[2] + nb[2] 
    //frame->res_length = context->header_length + 2 + 2;

    return frame->length;
}


/**
 * 
 * @brief Function 16 (0x10) Write Multiple Registers.
 * 
 * Builds a request frame for writing a contiguous series of holding
 * registers starting at the given address. The values array contains the
 * 16-bit register values in host order which are encoded into the frame.
 * 
 * @param context: allocated context
 * @param frame: frame to encode
 * @param addr: starting register address
 * @param naddr: number of registers to write
 * @param values: pointer to naddr 16-bit register values
 * 
 * @return frame length on success, < 0 on error
 */

int
modbusmq_frame_write_registers(modbusmq_context_t *context, modbusmq_frame_t *frame, int addr, int naddr, const uint16_t *values)
{
    if (!context || !frame)
    {
        errno = EINVAL;
        return -1;
    }
    
    context->cb.modbusmq_write_registers(context, frame, addr, naddr, values);

    // data-length: is number of records * 2 
    frame->buf[frame->length++] = naddr*2; 

    
    for(int i = 0; i < naddr; ++i)
    {
        frame->buf[frame->length++] = values[i] >> 8;
        frame->buf[frame->length++] = values[i] & 0x00ff;
    }

    // response: header[*] + address[2] + naddr[2]
    //frame->res_length = context->header_length + 2 + 2;

    return frame->length;
}



/**
 * 
 * @brief Function 22 (0x16) Mask Write Register.
 * 
 * Builds a request frame for a mask write operation on a single holding
 * register. The AND- and OR-masks are encoded according to the Modbusmq
 * mask write semantics.
 * 
 * @param context: allocated context
 * @param frame: frame to encode
 * @param addr: register address to modify
 * @param and_mask: AND mask applied to the current register value
 * @param or_mask: OR mask applied after the AND operation
 * 
 * @return frame length on success, < 0 on error
 */

int
modbusmq_frame_write_mask_registers( struct modbusmq_context_t *context, modbusmq_frame_t *frame, int addr, int and_mask, int or_mask)
{
    if (!context || !frame)
    {
        errno = EINVAL;
        return -1;
    }
    
    context->cb.modbusmq_write_mask_registers(context, frame, addr, and_mask, or_mask);

    frame->buf[frame->length++] = and_mask >> 8;
    frame->buf[frame->length++] = and_mask & 0x00ff;
    frame->buf[frame->length++] = or_mask >> 8;
    frame->buf[frame->length++] = or_mask & 0x00ff;

    // response: header[*] + address[2] + naddr[2]
    // TODO: Need to verify and check this
    //frame->res_length = context->header_length + 2 + 2;

    return frame->length;
}


#define MODBUSMQ_MSG_POST      1
#define MODBUSMQ_MSG_SUBSRIBE  2

/**
 * 
 * @brief Internal helper to enqueue a Modbusmq message.
 * 
 * Allocates a new modbusmq_msg_wrapper_t, copies the message into it and
 * appends it to the tail of the context message queue. Flags indicate
 * whether this is a one-shot post or a subscription.
 * 
 * @param context: allocated context
 * @param msg: message to enqueue
 * @param flags: message flags (e.g. MODBUSMQ_MSG_POST, MODBUSMQ_MSG_SUBSCRIBE)
 * 
 * @return 0 on success, < 0 on error
 */

int
modbusmq_post_internal(modbusmq_context_t *context, const modbusmq_msg_t *msg, int flags)
{
    
    modbusmq_msg_wrapper_t
        *wrapper = (modbusmq_msg_wrapper_t *)malloc(sizeof(modbusmq_msg_wrapper_t));
    memset(wrapper, 0, sizeof(modbusmq_msg_wrapper_t));
    memcpy(&wrapper->msg, msg, sizeof(modbusmq_msg_t));

    int
        rc = modbusmq_msg_prepare(context, &wrapper->msg);
    
    wrapper->flags        = flags;

    if (!context->msg_wrapper_head)
    {
        context->msg_wrapper_head = wrapper;
    }
    else
    {
        modbusmq_msg_wrapper_t
            *last = context->msg_wrapper_head;
        while(last && last->next)
            last = last->next;

        last->next = wrapper;
    }

    return 0;
}

/**
 * 
 * @brief Post an asynchronous Modbusmq message for transmission.
 * 
 * Performs basic argument validation and enqueues the message for later
 * processing using modbusmq_post_internal() with MODBUSMQ_MSG_POST semantics.
 * 
 * @param context: allocated context
 * @param msg: message to post
 * 
 * @return 0 on success, < 0 on error
 */
int
modbusmq_post(modbusmq_context_t *context, modbusmq_msg_t *msg)
{
    if (!context || !msg)
    {
        errno = EINVAL;
        return -1;
    }

    return modbusmq_post_internal(context, msg, MODBUSMQ_MSG_POST);
}

/**
 * 
 * @brief Validate a Modbusmq message before sending.
 * 
 * Delegates to the registered modbusmq_msg_check() callback to verify that
 * the message is consistent with the current configuration and protocol
 * rules.
 * 
 * @param context: allocated context
 * @param msg: message to validate
 * 
 * @return 0 on success, < 0 on validation error
 */
int
modbusmq_check(modbusmq_context_t *context, modbusmq_msg_t *msg)
{
    if (!context || !msg)
    {
        errno = EINVAL;
        return -1;
    }

    return context->cb.modbusmq_msg_check(context, msg);

    assert(0);
    return -1;
}

/**
 * 
 * @brief Verify that the request and response headers are consistent.
 * 
 * Checks that the message contains a valid request and, if applicable,
 * an associated response frame with matching header information according
 * to the current Modbusmq transport (TCP/RTU).
 * 
 * @param context: allocated context
 * @param msg: message containing request/response frames
 * 
 * @return 0 on success, < 0 on error
 */

int
modbusmq_check_header(modbusmq_context_t *context, modbusmq_msg_t *msg)
{
    if (!context || !msg)
    {
        assert(0);
        errno = EINVAL;
        return -1;
    }

    return context->cb.modbusmq_msg_check_header(context, msg);

    assert(0);
    return -1;
}


/**
 * 
 * @brief Allocate and return a unique timer identifier.
 * 
 * Returns a monotonically increasing integer that can be used to identify
 * individual subscription timers attached to the context.
 * 
 * @return unique timer id (> 0)
 */
int
modbusmq_timer_id()
{
    static int timer_id = 0;

    timer_id++;
    return timer_id;
}

    
/**
 * 
 * @brief Subscribe to a Modbusmq request at a fixed interval.
 * 
 * Creates a timer entry which periodically sends the given request and
 * processes the response. When a response is received, the registered
 * subscribe callback is invoked with the resulting message.
 * 
 * @param context: allocated context
 * @param msg: message template used for each periodic request
 * @param interval: interval between requests in milliseconds
 * 
 * @return 0 on success, < 0 on error
 */
int
modbusmq_subscribe(modbusmq_context_t *context, modbusmq_msg_t *msg, int interval)
{
    if (!context || !msg)
    {
        errno = EINVAL;
        return -1;
    }

    assert(interval > 0);
    
    modbusmq_timer_t
        *timer = (modbusmq_timer_t *)malloc(sizeof(modbusmq_timer_t));
    if (!timer)
    {
        perror("Allocation error");
        assert(0);
        return -1;
    }
    memset(timer, 0, sizeof(modbusmq_timer_t));

    timer->timer_id        = modbusmq_timer_id();
    timer->timer_start     = millitime();
    timer->timer_interval  = interval;
    timer->orig_interval   = interval;
    timer->timer_next_time = timer->timer_start;
    
    memcpy(&timer->msg, msg, sizeof(modbusmq_msg_t));

    if (!context->timer_head)
    {
        context->timer_head = timer;
    }
    else
    {
        modbusmq_timer_t
            *head = context->timer_head,
            *elem = head,
            *prev = elem;

        timer->timer_start = head->timer_start;
        
        // there are two timer-modes
        // 0: each timer is independent
        // 1: each timer executes after the other timer
        if (context->config->query_mode == ModbusmqQueryModeParallell)
        {
            timer->timer_next_time = timer->timer_start;

            if (timer->timer_interval < head->timer_interval)
            {
                timer->next = context->timer_head;
                context->timer_head = timer;
            }
            else
            {
                while(elem)
                {
                    if (timer->timer_interval < elem->timer_interval)
                    {
                        prev->next  = timer;
                        timer->next = elem;
                        break;
                    }
                    prev = elem;
                    elem = elem->next;
                }

                if (!prev->next)
                {
                    prev->next = timer;
                }
            }
        }
        else if (context->config->query_mode == ModbusmqQueryModeSeries)
        {
            // add previours timer->interval
            //
            // timer1: 1000ms
            // no change
            // add:
            // timer2: 1000ms
            //
            // timer1: start    0, interval 2000ms
            // timer2: start 1000, interval 2000ms (start = timer1.start[0] + timer2.orig_interval[1000]
            //
            // add
            // timer3: 3000ms
            // timer1: start    0, interval 5000ms 
            // timer2: start 1000, interval 5000ms (start = timer1.start[0] + timer2.orig_interval[1000])
            // timer3: start 4000, interval 5000ms (start = timer2.start[1000] + timer3.orig_interval[3000])
            //
            // add
            // timer4: 2000ms
            // timer1: next    0, interval 7000ms
            // timer2: next 1000, interval 7000ms
            // timer3: next 4000, interval 7000ms
            // timer4: next 6000, interval 7000ms
            
            millitime_t
                total_interval_ms = 0;

            // find tail
            while(elem)
            {
                // sum up all intervals for later use
                total_interval_ms += elem->orig_interval;

                // if (prev)
                // {
                //     // set new start-time
                //     elem->timer_next_time = prev->timer_start + elem->orig_interval;
                // }
                prev = elem;
                elem = elem->next;
            }

            // add new timer to the tail (prev is now tail)
            prev->next = timer;
            total_interval_ms += timer->orig_interval;

            // new start time
            timer->timer_next_time = prev->timer_next_time + timer->orig_interval;

            // now loop through the list and set the new calculated fixed interval
            elem = context->timer_head;
            while(elem)
            {
                elem->timer_interval = total_interval_ms;
                elem = elem->next;
            }
            
        }
    }

    //    modbusmq_timer_debug_print(context);
    
    return 0;
}

/**
 * 
 * @brief Print the internal timer list for debugging.
 * 
 * Iterates over the context timer list and, when enabled, prints relative
 * timing and interval information for each active timer. Intended for
 * development-time inspection only.
 * 
 * @param context: allocated context
 * 
 * @return nothing
 */
void
modbusmq_timer_debug_print(modbusmq_context_t *context)
{
    assert(context);
    
    modbusmq_timer_t
        *head = context->timer_head,
        *elem = head;
    int
        nElem = 1;
    
    while(elem)
    {
        assert(elem->orig_interval > 0);

        if (0)
        {
            printf("timer%d.id=%d start=%d orig_interval=%d timer_interval=%d\n",
                   nElem,
                   elem->timer_id,
                   (int)(elem->timer_next_time - head->timer_next_time),
                   (int)elem->orig_interval,
                   (int)elem->timer_interval);
        }
        elem = elem->next;
    }
}


/**
 * 
 * @brief Determine whether a request or response frame is complete.
 * 
 * For writer frames this function checks that at least the minimum request
 * header length has been transmitted and that all bytes in the frame have
 * been written. For reader frames it returns how many additional bytes are
 * required to satisfy the minimum response header length, or 0 when the
 * frame is complete.
 * 
 * @param context: allocated context
 * @param frame: frame to inspect
 * 
 * @return 0 when the frame is complete
 *         > 0 when more data is required
 */

int
modbusmq_frame_incomplete(modbusmq_context_t *context, modbusmq_frame_t *frame)
{
    int rc = 1;
    // printf("frame.is_req2 = %d\n", frame->is_req2);
    // printf("frame.is_writer = %d\n", frame->is_writer);
    // printf("frame.is_tcp = %d\n", frame->is_tcp);
    // printf("frame.length = %d\n", frame->length);
    // printf("frame.xmit = %d\n", frame->xmit);
    
    // if (frame->xmit < frame->length)
    // {
    //     rc = frame->length - frame->xmit;
    // }

    if (frame->is_writer)
    {
        if (frame->length >= context->req_header_min && frame->xmit == frame->length)
        {
            rc = 0;
        }
    }
    else // reader
    {
        if (frame->length == 0)
        {
            rc = context->res_header_min;
        }
        else if (frame->xmit < context->res_header_min)
        {
            rc = context->res_header_min - frame->xmit;

        }
        else if (frame->xmit == frame->length)
        {
            rc = 0;
        }
        // if (frame->xmit < context->res_header_min)
        // {
        //     rc = context->res_header_min - frame->xmit;
        // }
    }

    //printf("modbusmq_frame_incomplete = %d [%s]\n", rc, frame->is_writer ? "writer" : "reader");
    return rc;
}


/**
 * 
 * @brief Write a Modbusmq frame to the underlying transport.
 * 
 * Dispatches the write operation to either the Modbusmq TCP or Modbusmq RTU
 * backend depending on how the context was initialized. The frame is
 * written starting at frame->xmit and the field is updated with the number
 * of bytes successfully transmitted.
 * 
 * @param context: allocated context
 * @param fd: file descriptor or socket for the transport
 * @param frame: frame to write
 * 
 * @return > 0 number of bytes written in this call
 *         0  when the transport would block
 *        < 0 on write error
 */
int
modbusmq_write(modbusmq_context_t *context, int fd, modbusmq_frame_t *frame)
{
    int
        rc;
    assert(context);
    
    if (context->tcp)
    {
        rc = modbusmq_tcp_write(context, fd, frame);
    }
    else if (context->rtu)
    {
        rc = modbusmq_rtu_write(context, fd, frame);
    }
    else
    {
        fprintf(stderr, "modbusmq tcp/rtu context missing\n");
        assert(0);
        return -1;
    }

    return rc;
}

/**
 * 
 * @brief Read a Modbusmq frame from the underlying transport.
 * 
 * Dispatches the read operation to either the Modbusmq TCP or Modbusmq RTU
 * backend depending on the context configuration. The function appends
 * data to the frame buffer and updates frame->xmit.
 * 
 * @param context: allocated context
 * @param fd: file descriptor or socket for the transport
 * @param frame: frame to fill
 * 
 * @return > 0 number of bytes read in this call
 *         0  when the transport would block
 *        < 0 on read or framing error
 */
int
modbusmq_read(modbusmq_context_t *context, int fd, modbusmq_frame_t *frame)
{
    int
        rc;

    //fprintf(stderr, "modbusmq_read start\n");
    if (context->tcp)
    {
        rc = modbusmq_tcp_read(context, fd, frame);
    }
    else if (context->rtu)
    {
        rc = modbusmq_rtu_read(context, fd, frame);
    }
    else
    {
        fprintf(stderr, "modbusmq tcp/rtu context missing\n");
        assert(0);
        return -1;
    }


    // if (modbusmq_debug)
    // {
    //     fprintf(stderr, "modbusmq_read %d bytes\n", rc);
    //     assert(rc < 300);
    // }
    
    //fprintf(stderr, "modbusmq_read end\n");
    return rc;
}


/**
 * 
 * @brief Handle write/read processing for a single Modbusmq message.
 * 
 * Given a message and a set of poll() revents, this function advances the
 * state of the associated request/response frames: it writes pending data,
 * reads response data when available, and invokes callbacks when a full
 * transaction completes or fails.
 * 
 * @param context: allocated context
 * @param msg: message being processed
 * @param revents: poll() events for the associated file descriptor
 * 
 * @return 0 on success or when more work remains
 *        < 0 on fatal error
 */

int
modbusmq_handle_write_read(modbusmq_context_t *context, modbusmq_msg_t *msg, int revents)
{
    int
        rc = 0;

    modbusmq_frame_t *writer = msg->frame[0].is_writer ? &msg->frame[0] : &msg->frame[1];
    modbusmq_frame_t *reader = msg->frame[1].is_writer == 0 ? &msg->frame[1] : &msg->frame[0];
        
    
    // Check if we need to write:
    if (modbusmq_frame_incomplete(context, writer))
    {
        //printf("need to write\n");
        if (revents & POLLOUT)
        {
            context->last_write_ms = millitime();

            rc = modbusmq_write(context, context->fd, writer);
            if (rc < 0)
            {
                context->err++;
                modbusmq_logf(LOG_ERROR, "Unable to write to socket: rc = %d\n", rc);
                return -1;
            }
            
            
            if (writer->length == writer->xmit)
            {
                if (modbusmq_debug >= 1)
                {
                    modbusmq_frame_debug(context, writer);
                }
                memset(reader, 0, sizeof(*reader));
                reader->length = context->header_length;
                //printf("writer->length = %d, setting reader->length = %d\n", writer->length, reader->length);
            }
            else
            {
                ; //printf("writer->length = %d, writer->xmit = %d\n", writer->length, writer->xmit);
            }
            // printf("writer->length = %d, writer->xmit = %d\n", writer->length, writer->xmit);
            
        }
    }

    // Check if we need to read
    else if (modbusmq_frame_incomplete(context, reader))
    {
        //printf("need to read\n");
        if (revents & POLLIN)
        {
            context->last_read_ms = millitime();

            rc = modbusmq_read(context, context->fd, reader);

            if (rc < 0)
            {
                context->err++;
                modbusmq_logf(LOG_ERROR, "%s:%d:%s: Unable to read from socket: rc = %d, errno=%d, str=%s\n", __FILE__, __LINE__, __FUNCTION__, rc, errno, strerror(errno));
                return -1;
            }
            
            // Ensure what we have read matches what we requested
            if (modbusmq_check_header(context, msg) < 0)
            {
                //assert(0);
                rc = -2;
            }
            
            if (reader->xmit == reader->length)
            {
                rc = 0;
                
                if (modbusmq_debug >= 1)
                {
                    modbusmq_frame_debug(context, reader);
                }
            }
        }
    }
    else
    {
        // message is complete
        return 0;
    }

    if (rc != 0)
    {
        return rc;
    }
    
    // return number of bytes pending to write+read
    rc =
        writer->length - writer->xmit +
        reader->length - reader->xmit;

    
    return rc;
    
}


/**
 * 
 * @brief Drain the underlying transport receive buffer.
 * 
 * Reads and discards any pending data from the Modbusmq transport until no
 * more bytes are available. Used to recover from framing or protocol
 * errors.
 * 
 * @param context: allocated context
 * 
 * @return nothing
 */

void
modbusmq_flush(modbusmq_context_t *context)
{
    if (!context)
    {
        errno = EINVAL;
        return;
    }
    else if (context->tcp)
    {
        modbusmq_tcp_flush(context);
    }
    else if (context->rtu)
    {
        modbusmq_rtu_flush(context);
    }
    else
    {
        assert(0);
    }
}

/**
 * 
 * @brief Process a single queued Modbusmq message wrapper.
 * 
 * Performs write/read handling for the wrapped message and, if the message
 * represents a subscription, may reschedule the timer for the next
 * request. For normal (non-subscription) messages it may dequeue and free
 * the wrapper when the transaction is complete.
 * 
 * @param context: allocated context
 * @param wrapper: message wrapper from the internal queue
 * 
 * @return 0 on success or when more work remains
 *        < 0 on fatal error
 */
int
modbusmq_handle_msg(modbusmq_context_t *context, modbusmq_msg_wrapper_t *wrapper)
{
    // modbusmq_logf(LOG_INFO, "modbusmq_handle_msg\n");
    // written request complete
    // read    response complete
    // call user function
    // deallocate element
    //
    int
        rc = modbusmq_check(context, &wrapper->msg);
    if (rc < 0)
    {
        modbusmq_flush(context);
        return -1;
    }
                
    if (wrapper->flags == MODBUSMQ_MSG_POST)
    {
        // callback to user
        if (context->message_cb)
        {
            context->message_cb(context, &wrapper->msg);
        }
    }
    else if (wrapper->flags == MODBUSMQ_MSG_SUBSRIBE && context->subscribe_cb)
    {
        modbusmq_config_t
            *modbusmq_config = modbusmq_config_get();
        
        int
            input_max = modbusmq_config->input_max;
        for(int i = 0; i < input_max; ++i)
        {
            modbusmq_input_t
                *input = &modbusmq_config->inputs[i];
            int
                address = modbusmq_frame_addr(context, &wrapper->msg.frame[0]);
            int
                slave = modbusmq_frame_slave(context, &wrapper->msg.frame[1]);
            
            if (input->address == address &&
                input->slave   == slave)
            {
                // have the correct input-element
                context->subscribe_cb(context, &wrapper->msg, input);
            }
        }
    }
                
    // clean up
    context->msg_wrapper_head = context->msg_wrapper_head->next;
    free(wrapper);
    wrapper = NULL;
}

/**
 * 
 * @brief Prepare poll() events for active Modbusmq subscriptions.
 * 
 * Walks all active subscription timers, schedules due requests, and updates
 * the caller-provided sleep_time and poll_events with the next timeout and
 * the appropriate POLLIN/POLLOUT mask for the Modbusmq file descriptor.
 * 
 * @param context: allocated context
 * @param sleep_time: in/out pointer to the minimum sleep time in milliseconds
 * @param poll_events: in/out pointer to poll event mask for Modbusmq fd
 * 
 * @return 0 on success, < 0 on error
 */

int
modbusmq_loop_prepare_subscription(modbusmq_context_t *context, millitime_t *sleep_time, int16_t *poll_events)
{
    millitime_t millisleep = 1000;

    //    modbusmq_timer_debug_print(context);
    
    //
    // foreach timer (sorted by minimal delay first)
    // find out if our timer has been reach and call function
    // calculate new time
    //
    // calculate minimal sleep time
    if (context->timer_head)
    {
        millitime_t
            time_now = millitime();
        modbusmq_timer_t
            *elem = context->timer_head;
        
        while(elem)
        {
            if (elem->timer_next_time <= time_now)
            {
                // ensure there are no other subscriptions active for this timer
                modbusmq_msg_wrapper_t
                    *w1 = context->msg_wrapper_head;
                while(w1)
                {
                    if (w1->msg.msg_id == elem->timer_id)
                    {
                        break;
                    }
                    w1 = w1->next;
                }

                // only add new subscription if there are no subscriptions for this timer_id
                if (!w1 || w1->msg.msg_id != elem->timer_id)
                {
                    elem->msg.msg_id = elem->timer_id;
                
                    //
                    // post a request
                    //
                    modbusmq_post_internal(context, &elem->msg, MODBUSMQ_MSG_SUBSRIBE);


                    elem->timer_next_time += elem->timer_interval;
                }
                
            }

            millisleep = MODBUSMQ_MIN(millisleep, elem->timer_next_time - time_now);
            elem = elem->next;
        }
    }

    *sleep_time = millisleep;
    return 0;
}


/**
 * 
 * @brief Prepare the main Modbusmq loop poll() parameters.
 * 
 * Computes the appropriate sleep time and event mask for the Modbusmq
 * connection based on the state of queued messages and subscription
 * timers. This is typically called before blocking in poll().
 * 
 * @param context: allocated context
 * @param sleep_time: out parameter with maximum time to sleep in milliseconds
 * @param events: out parameter with POLLIN/POLLOUT mask for Modbusmq fd
 * 
 * @return 0 on success, < 0 on error
 */

int
modbusmq_loop_prepare(modbusmq_context_t *context, millitime_t *sleep_time, int16_t *events)
{
    if (!context)
    {
        errno = EINVAL;
        return -1;
    }

    if (context->fd <= 0)
    {
        return 0;
    }

    
    //
    // first check if current msg is handled properly
    //
    modbusmq_msg_wrapper_t
        *wrapper = context->msg_wrapper_head;
    if (wrapper)
    {
        millitime_t
            time_now = millitime();

        if ((time_now - context->last_write_ms) >= context->frame_timeout_ms)
        {
            // discard this packages
            modbusmq_logf(LOG_ERROR, "Waited %d ms for a response, discarding request and continuing...\n", context->frame_timeout_ms);
            
            if (modbusmq_debug)
            {
                // modbusmq_logf(LOG_ERROR, "res.res_length = %d\nres.res_received = %d\n", wrapper->msg.res_length, wrapper->msg.res_xmit);
                modbusmq_logf(LOG_ERROR, "time_diff_ms = %d\n", (time_now - context->last_write_ms));
            }
            
            if (modbusmq_debug)
            {
                modbusmq_frame_debug(context, &wrapper->msg.frame[0]);
                modbusmq_frame_debug(context, &wrapper->msg.frame[1]);
            }
            
            context->msg_wrapper_head = context->msg_wrapper_head->next;
            free(wrapper);
            wrapper = NULL;

            // have already sent request, but received no response                    
            context->err++;
            context->tx++;

            context->last_write_ms = 0;
            modbusmq_flush(context);
        }
    }
    
    modbusmq_loop_prepare_subscription(context, sleep_time, events);
    
    wrapper = context->msg_wrapper_head;
    if (!wrapper)
    {
        return 0;
    }

    modbusmq_frame_t
        *writer = &wrapper->msg.frame[0],
        *reader = &wrapper->msg.frame[1];

    if (!wrapper->msg.frame[0].is_writer)
    {
        writer = &wrapper->msg.frame[1];
        reader = &wrapper->msg.frame[0];
    }
    writer->is_writer = 1;
    reader->is_writer = 0;
    
    
    // must write request first
    if (modbusmq_frame_incomplete(context, writer))
    {
        *events |= POLLOUT;
        return context->fd;
    }
    else if (modbusmq_frame_incomplete(context, reader))
    {
        // sometimes the device will give a shorter answer compared to requested
        // e.g. you ask for 22 values, but only get 8 values in return.
        // check and adjust the expected res_length

        *events |= POLLIN;

        return context->fd;
    }

    return 0;
}


/**
 * 
 * @brief Send a Modbusmq message and optionally wait for completion.
 * 
 * Enqueues the message for processing and, if mswait is > 0, blocks in a
 * loop calling poll() and modbusmq_loop_write_read() until the transaction
 * completes or the timeout expires.
 * 
 * @param context: allocated context
 * @param msg: message to send
 * @param mswait: maximum time to wait in milliseconds (0 = non-blocking)
 * 
 * @return 0 on success, < 0 on error or timeout
 */

int
modbusmq_send(modbusmq_context_t *context, modbusmq_msg_t *msg, int mswait)
{
    
    if (!context || !msg)
    {
        errno = EINVAL;
        return -1;
    }

    modbusmq_frame_t *writer =  msg->frame[0].is_writer ? &msg->frame[0] : &msg->frame[1];
    modbusmq_frame_t *reader = (!msg->frame[0].is_writer) ? &msg->frame[0] : &msg->frame[1];

    assert(writer != reader);

    struct pollfd pollfds[10];

    millitime_t
        start_time_ms = millitime(),
        time_now      = start_time_ms;


    modbusmq_msg_prepare(context, msg);
    
    do 
    {
        time_now = millitime();

        int
            nfds = 0;
            
        millitime_t
            millisleep = 100; 

        memset(pollfds, 0, sizeof(pollfds));

        pollfds[0].fd = context->fd;
        pollfds[0].events |= POLLERR | POLLHUP;
        
        
        if (modbusmq_frame_incomplete(context, writer))
        {
            pollfds[0].events |= POLLOUT;
        }
        else if (modbusmq_frame_incomplete(context, reader))
        {
            pollfds[0].events |= POLLIN;
        }
        
        nfds++;

        int rc = poll(pollfds, nfds, millisleep);

        if (rc < 0)
        {
            modbusmq_logf(LOG_ERROR, "poll failed: rc = %d, errno=%d, str=%s\n", rc, errno, strerror(errno));
            exit(2);
        }
        else if (rc == 0)
        {
            // normal timeout
        }
        else
        {
            if (pollfds[0].revents & (POLLIN | POLLOUT))
            {
                // if (pollfds[0].revents & POLLIN)
                //     printf("pollfds[0] POLLIN\n");
                // if (pollfds[0].revents & POLLOUT)
                //     printf("pollfds[0] POLLOUT\n");
                //
                // modbusmq
                //
                rc = modbusmq_handle_write_read(context, msg, pollfds[0].revents);
                if (rc < 0)
                {
                    modbusmq_logf(LOG_ERROR, "modbusmq_loop_write_read: error rc=%d, err=%s\n", rc, strerror(errno));
                    exit(2);
                }
                else if (rc == 0)
                {
                    if (reader->length == reader->xmit)
                    {
                        break; // we are done
                    }
                }
                
            }
        }
    } while ((time_now - start_time_ms) < mswait);

    return 0;
}

#if 0
/**
 * 
 * @brief Debug helper main loop (never exits).
 * 
 * Runs a blocking Modbusmq loop that continuously prepares poll() parameters,
 * waits for activity, and processes messages. Intended for standalone
 * debugging and is normally compiled out.
 * 
 * @param context: allocated context
 * 
 * @return this function does not return under normal circumstances
 */
int
modbusmq_run(modbusmq_context_t *context)
{
    int
        rc;
    fd_set
        read_set,
        write_set,
        err_set;
    
    struct timeval
        tv;
    int
        fd_max = 0;
    millitime_t
        start_time_ms = millitime();

    while(1)
    {
        millitime_t
            time_now = millitime();

        millitime_t
            millisleep = 1000; // 1 second default wait time

        modbusmq_loop_prepare_subscription(context, &millisleep);
        //
        // make sure we at least have a sleep-time
        //
        if (millisleep <= 0)
        {
            millisleep = 1000;
        }

        


        //
        // set up timeout for select
        //
        tv.tv_sec  = 0;
        tv.tv_usec = millisleep * 1000;

        //
        // register all file handlers
        // TODO: Add write/exception sets
        //
        fd_max = 0;
        FD_ZERO(&read_set);
        FD_ZERO(&write_set);
        FD_ZERO(&err_set);

        int
            fd = context->fd;
        modbusmq_msg_wrapper_t
            *wrapper = context->msg_wrapper_head;
        if (fd > 0 && wrapper)
        {
            // must write request first
            if (wrapper->msg.frame[index].xmit < wrapper->msg.req_length)
            {
                FD_SET(fd, &write_set);
                fd_max = MODBUSMQ_MAX(fd_max, fd);
            }
            // can read after request has been written
            else if (wrapper->msg.res_xmit < wrapper->msg.res_length)
            {
                FD_SET(fd, &read_set);
                fd_max = MODBUSMQ_MAX(fd_max, fd);
            }
        }
            
        //
        // call select
        //
        rc = select(fd_max+1, &read_set, &write_set, NULL, &tv);
        
        if (rc < 0)
        {
            modbusmq_logf(LOG_ERROR, "select failed: rc = %d\n", rc);
            return -2;
        }
        else if (rc == 0)
        {
            // need to check for timeout and discard request and let the system continue
            if (wrapper && wrapper->msg.res_xmit < wrapper->msg.res_length &&
                context->last_write_ms)
            {
                time_now = millitime();
                
                // TODO: Use timeout slot from context and not hardcoded 2000 ms
                if ((time_now - context->last_write_ms) >= context->frame_timeout_ms)
                {
                    // discard this packages
                    modbusmq_logf(LOG_ERROR, "Waited %d ms for a response, discarding request and continuing...\n", context->frame_timeout_ms);
                    if (modbusmq_debug)
                    {
                        modbusmq_frame_debug(context, &wrapper->msg.frame[0]);
                        modbusmq_frame_debug(context, &wrapper->msg.frame[1]);

                    }
                    
                    context->msg_wrapper_head = context->msg_wrapper_head->next;
                    free(wrapper);
                    wrapper = NULL;

                    // have already sent request, but received no response                    
                    context->err++;
                    context->tx++; 
                }
                
            }
            
            continue; // timeout, normal, just continue
        }
        else
        {
            rc = modbusmq_handle_write_read(context, &wrapper->msg);

            if (rc == 0) // means this msg_t was completed
            {
                context->tx++;
                context->rx++;
                modbusmq_handle_msg(context, wrapper);
            }
            else if (rc < 0)
            {
                modbusmq_logf(LOG_ERROR, "readwrite error rc=%d\n", rc);
            }
            else if (rc > 0)
            {
                // still data to be read/written
            }
        }
    }
}
#endif

/**
 * 
 * @brief One iteration of the Modbusmq event loop write/read handling.
 * 
 * Called after poll() returns to process the poll revents for the Modbusmq
 * file descriptor. It delegates to modbusmq_handle_write_read() for the
 * head of the message queue and advances message state accordingly.
 * 
 * @param context: allocated context
 * @param revents: poll() revents for the Modbusmq file descriptor
 * 
 * @return 0 on success or when more work remains
 *        < 0 on fatal error
 */

int
modbusmq_loop_write_read(modbusmq_context_t *context, int revents)
{
    if (!context)
    {
        errno = EINVAL;
        return -1;
    }

    modbusmq_msg_wrapper_t
        *wrapper = context->msg_wrapper_head;
    if (!wrapper)
    {
        // nothing to read/write
        return 0;
    }
    
    int
        rc = modbusmq_handle_write_read(context, &wrapper->msg, revents);

    if (rc == 0) 
    {
        // One message is complete: request + response
        context->tx++;
        context->rx++;
        modbusmq_handle_msg(context, wrapper);
    }
    else if (rc < 0)
    {
        context->err++;
        modbusmq_logf(LOG_ERROR, "%s: error rc=%d\n", __FUNCTION__, rc);
    }
    else if (rc > 0)
    {
        // modbusmq_logf(LOG_INFO, "%s:%d:%s: msg.req=[%d:%d] msg.res=[%d:%d] revents=%s | %s\n", __FILE__, __LINE__, __FUNCTION__,
        //             wrapper->msg.frame[index].xmit, wrapper->msg.req_length,
        //             wrapper->msg.res_xmit, wrapper->msg.res_length,
        //             (revents & POLLIN) ? "POLLIN" : "",
        //             (revents & POLLOUT) ? "POLLOUT" : ""
        // );
        // still data to be read/written
    }
    
    return rc;
}
