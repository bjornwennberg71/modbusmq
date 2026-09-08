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
#include <math.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>

#include <sys/ioctl.h>
#include <netdb.h>
#include <poll.h>

static int modbusmq_debug = 0;

static int  modbusmq_flush(modbusmq_context_t *context);
static void modbusmq_resync(modbusmq_context_t *context);

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
    
    if (context->fd >= 0)
    {
        close(context->fd);
        context->fd = -1;
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
 * @brief sets the callback to be called when a request is given up on
 *
 * The log lines name the failing device, but a caller that has to act on it —
 * mark a slave offline, back off, raise an alarm — should not have to parse
 * them. This hands over the failed message itself, so slave id, address and
 * req_id are readable directly.
 *
 * @param context: allocated context
 * @param error_cb: function called with the failed message and a MODBUSMQ_ERR_* code
 */
void
modbusmq_set_error_callback(modbusmq_context_t *context, void (*error_cb)(modbusmq_context_t *context, modbusmq_msg_t *msg, int error))
{
    if (!context)
    {
        errno = EINVAL;
        return;
    }

    context->error_cb = error_cb;
}

/**
 *
 * @brief the writer half of a message, or NULL when the request is not built yet
 *
 * frame[0] is the writer everywhere in practice, but the rest of the library
 * resolves it through is_writer rather than assuming, and the decoders are not
 * all forgiving about being handed the wrong half: modbusmq_tcp_frame_addr()
 * asserts on a reader. Resolve it the same way here.
 *
 * @param msg: message to inspect
 *
 * @return the writer frame, or NULL when neither half holds a built request
 */
static modbusmq_frame_t *
modbusmq_msg_writer(modbusmq_msg_t *msg)
{
    if (msg->frame[0].is_writer && msg->frame[0].length > 0)
    {
        return &msg->frame[0];
    }

    if (msg->frame[1].is_writer && msg->frame[1].length > 0)
    {
        return &msg->frame[1];
    }

    return NULL;
}

/**
 *
 * @brief build the "[req N slave S addr 0xAAAA]" prefix for an error line
 *
 * @param context: allocated context
 * @param msg: the message the error concerns
 *
 * @return pointer to a static buffer, never NULL
 */
const char *
modbusmq_msg_tag(modbusmq_context_t *context, modbusmq_msg_t *msg)
{
    static char
        tag[64];

    if (!context || !msg)
    {
        snprintf(tag, sizeof(tag), "[req ?]");
        return tag;
    }

    modbusmq_frame_t
        *writer = modbusmq_msg_writer(msg);

    //
    // A request that was never built has no slave or address to report — the
    // req_id alone still ties the line to the transaction.
    //
    if (!writer)
    {
        snprintf(tag, sizeof(tag), "[req %u]", msg->req_id);
        return tag;
    }

    snprintf(tag, sizeof(tag), "[req %u slave %d addr 0x%04X]",
             msg->req_id,
             modbusmq_frame_slave(context, writer),
             modbusmq_frame_addr(context, writer));

    return tag;
}

/**
 *
 * @brief allocate the next request id for this context
 *
 * @param context: allocated context
 *
 * @return a request id that has not been used before on this context
 */
static uint32_t
modbusmq_next_req_id(modbusmq_context_t *context)
{
    context->req_id_next++;

    // 0 reads as "never stamped" in a log line, so skip it on wrap
    if (context->req_id_next == 0)
    {
        context->req_id_next = 1;
    }

    return context->req_id_next;
}

/**
 *
 * @brief report a failed request to the caller, if it asked to hear about them
 *
 * @param context: allocated context
 * @param msg: the failed message
 * @param error: MODBUSMQ_ERR_* code describing what went wrong
 */
static void
modbusmq_report_error(modbusmq_context_t *context, modbusmq_msg_t *msg, int error)
{
    if (!context->error_cb)
    {
        return;
    }

    context->error_cb(context, msg, error);
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

    int
        rc = context->cb.modbusmq_connect(context);

    if (rc == 0)
    {
        //
        // A serial port can be opened part-way through someone else's frame,
        // or still hold bytes buffered from whoever had it before us. Neither
        // is a frame boundary, so drain before the first request goes out
        // instead of reading the first response through the leftovers.
        //
        context->resync_pending = 1;
    }

    return rc;
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
    context->fd = -1;
    return 0;
}

/**
 *
 * @brief discard all in-flight/queued request-response message wrappers
 *
 * Does NOT touch registered subscription timers (context->timer_head) — only
 * the one-shot/in-progress message queue. Intended to be called after a
 * transport reconnect so stale, half-transmitted messages from the old
 * connection don't block subscription timers from re-arming fresh requests.
 *
 * @param context: allocated context
 * @return nothing
 */
void
modbusmq_reset_queue(modbusmq_context_t *context)
{
    if (!context)
    {
        errno = EINVAL;
        return;
    }

    modbusmq_msg_wrapper_t
        *elem = context->msg_wrapper_head;

    while (elem)
    {
        modbusmq_msg_wrapper_t *next = elem->next;

        //
        // Report each one on the way out instead of freeing it quietly.
        //
        // These are requests that were accepted and never sent. For a poll
        // that is harmless — the next cycle asks again. For a write it is
        // not: the caller was told the setpoint was queued, the device never
        // received it, and nothing anywhere said so. A dropped command has to
        // be as visible as one that timed out.
        //
        modbusmq_logf(LOG_ERROR, "%s queued request dropped, the connection was reset before it was sent. action: discard request\n",
                      modbusmq_msg_tag(context, &elem->msg));
        modbusmq_report_error(context, &elem->msg, MODBUSMQ_ERR_TRANSPORT);

        free(elem);
        elem = next;
    }

    context->msg_wrapper_head = NULL;
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
 * @brief reads 2 bytes, high byte first, as a signed two's-complement value
 *
 * The signed counterpart of modbusmq_read_int16_ab(). Devices that report
 * temperatures need this: read unsigned, an ambient of -5.0 C arrives as
 * 65486 and scales to 6548.6 C rather than -5.0.
 *
 * @param data: 2 bytes
 * @return converted value, -32768..32767
 */
int
modbusmq_read_int16_ab_signed(const uint8_t *data)
{
    return (int16_t)((uint16_t)data[0] << 8 | data[1]);
}

/**
 *
 * @brief reads 2 bytes, low byte first, as a signed two's-complement value
 *
 * @param data: 2 bytes
 * @return converted value, -32768..32767
 */
int
modbusmq_read_int16_ba_signed(const uint8_t *data)
{
    return (int16_t)((uint16_t)data[1] << 8 | data[0]);
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

float
modbusmq_read_float_cdab(const uint8_t *data)
{
    float f;
    uint8_t a, b, c, d;
    a = data[0] & 0xff;
    b = data[1] & 0xff;
    c = data[2] & 0xff;
    d = data[3] & 0xff;
    uint32_t i = (c << 24) | (d << 16) | (a << 8) | (b << 0);
    memcpy(&f, &i, 4);
    return f;
}

/**
 *
 * @brief number of bytes a channel format occupies
 *
 * @param format: modbusmq_data_format_e value
 *
 * @return size in bytes
 */
int
modbusmq_format_size(int format)
{
    switch (format)
    {
    case modbusmq_data_format_a:
    case modbusmq_data_format_int8:
        return 1;
    case modbusmq_data_format_ab:
    case modbusmq_data_format_ba:
    case modbusmq_data_format_int16_ab:
    case modbusmq_data_format_int16_ba:
    case modbusmq_data_format_float_ba:
        return 2;
    default:
        return 4;
    }
}

//
// Byte position of a register channel inside the response.
//
// channel->offset is measured from input->address, the datasheet's base for
// the block — in bytes, or in registers when input.offset_size is 2.
// address_offset shifts only the request, and does so in registers, so the
// response begins that many registers, two bytes each, past the base. The
// two live in different units, which is why the shift is converted before it
// is taken off rather than subtracted as-is.
//
static int
modbusmq_channel_byte_offset(const modbusmq_config_t *config, const modbusmq_input_t *input, const modbusmq_channel_t *channel)
{
    int
        offset_size = (config && config->offset_size > 0) ? config->offset_size : 1;

    return channel->offset * offset_size - input->address_offset * 2;
}

/**
 *
 * @brief check that a channel lies inside the data actually received
 *
 * A channel whose offset falls outside the response payload would otherwise
 * read the untouched tail of the frame buffer and decode as a clean 0, which is
 * indistinguishable from a real measurement of zero. Report it instead.
 *
 * @param context: allocated context
 * @param msg    : modbusmq message holding the response
 * @param input  : input definition
 * @param channel: channel definition
 *
 * @return 0 when the channel is readable, < 0 when it is out of range
 */
int
modbusmq_channel_in_range(struct modbusmq_context_t *context, modbusmq_msg_t *msg, const modbusmq_input_t *input, const modbusmq_channel_t *channel)
{
    if (!context || !msg || !input || !channel)
    {
        errno = EINVAL;
        return -1;
    }

    modbusmq_config_t
        *config = modbusmq_config_get();
    int
        nbytes = modbusmq_frame_nbytes(context, &msg->frame[1]);

    //
    // For a coil or discrete input, channel->offset counts coils rather than
    // bytes — the natural unit of that address space, the same way it counts
    // bytes for a register block. One response byte carries eight of them.
    //
    if (MODBUSMQ_TYPE_IS_BIT(input->type))
    {
        int
            bit = channel->offset - input->address_offset;

        if (bit < 0 || nbytes < 0 || (bit / 8) >= nbytes)
        {
            modbusmq_logf(LOG_ERROR, "%s channel %s: coil %d is outside the %d coils received. action: skip channel\n",
                          modbusmq_msg_tag(context, msg),
                          channel->topic ? channel->topic : "?",
                          bit, nbytes * 8);
            return -1;
        }

        return 0;
    }

    int
        offset = modbusmq_channel_byte_offset(config, input, channel);
    int
        size   = modbusmq_format_size(channel->format);

    if (offset < 0 || nbytes < 0 || offset + size > nbytes)
    {
        modbusmq_logf(LOG_ERROR, "%s channel %s: offset %d (+%d bytes) is outside the %d bytes received. action: skip channel\n",
                      modbusmq_msg_tag(context, msg),
                      channel->topic ? channel->topic : "?",
                      offset, size, nbytes);
        return -1;
    }

    return 0;
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
        offset = modbusmq_channel_byte_offset(config, input, channel);
    float
        f = 0;

    if (modbusmq_channel_in_range(context, msg, input, channel) != 0)
    {
        return 0;
    }

    //
    // A bit is not a number in a format, so it never reaches the format switch
    // below. Modbus packs coils eight to a byte, lowest address in the lowest
    // bit — so coil N of the response is bit N%8 of byte N/8.
    //
    // Scaling still applies: mul=-1 or add=1 is how an active-low alarm gets
    // published the right way round without a second config key for it.
    //
    if (MODBUSMQ_TYPE_IS_BIT(input->type))
    {
        int
            bit = channel->offset - input->address_offset;

        f = (data[bit / 8] >> (bit % 8)) & 0x01;
    }
    else
    {
        switch(channel->format)
        {
        case modbusmq_data_format_float_abcd:  f = modbusmq_read_float_abcd(data + offset); break;
        case modbusmq_data_format_float_badc:  f = modbusmq_read_float_badc(data + offset); break;
        case modbusmq_data_format_float_dcba:  f = modbusmq_read_float_dcba(data + offset); break;
        case modbusmq_data_format_float_cdab:  f = modbusmq_read_float_cdab(data + offset); break;
        case modbusmq_data_format_ab:          f = modbusmq_read_int16_ab(data + offset);  value_len = 2; break;
        case modbusmq_data_format_ba:          f = modbusmq_read_int16_ba(data + offset); value_len = 2;break;
        case modbusmq_data_format_int16_ab:    f = modbusmq_read_int16_ab_signed(data + offset); value_len = 2; break;
        case modbusmq_data_format_int16_ba:    f = modbusmq_read_int16_ba_signed(data + offset); value_len = 2; break;
        case modbusmq_data_format_abcd:        f = (int32_t)(uint32_t)modbusmq_read_int32_abcd(data + offset); break;
        case modbusmq_data_format_badc:        f = (int32_t)(uint32_t)modbusmq_read_int32_badc(data + offset); break;
        case modbusmq_data_format_uint32_abcd: f = (uint32_t)modbusmq_read_int32_abcd(data + offset); break;
        case modbusmq_data_format_uint32_badc: f = (uint32_t)modbusmq_read_int32_badc(data + offset); break;
        case modbusmq_data_format_a:           f = data[offset]; value_len = 1; break;
        case modbusmq_data_format_int8:        f = (int8_t)data[offset]; value_len = 1; break;
        default:
            modbusmq_logf(LOG_ERROR, "channel %s: unhandled data format %d. action: skip channel\n",
                          channel->topic ? channel->topic : "?", (int)channel->format);
            return 0;
        }
    }

    f = (f + channel->add);
    if (channel->mod > 0)
    {
        f *= channel->mod;
    }
    else if (channel->mod < 0)
    {
        // mod is an int; negate after the conversion to float, since INT_MIN
        // has no positive counterpart to take the magnitude of
        f /= -(float)channel->mod;
    }
    if (channel->mul != 0)
    {
        f *= channel->mul;
    }
    
    return f;
}

/**
 *
 * @brief decide, and count, the decimal places a channel value formats to
 *
 * An explicit channel.decimals always wins. Otherwise a bit channel (coil or
 * discrete input) has nothing to round, a float format is printed to 3 places
 * regardless of scaling, and an integer format follows its own mod divisor —
 * mod=-100 means the raw value is hundredths, so 2 decimals recovers exactly
 * what was divided out. A mod that does not divide evenly by a power of ten
 * still needs *some* precision, so that case falls back to 3 rather than 0,
 * which would round away everything mod just un-scaled.
 *
 * @param input  : input definition, for MODBUSMQ_TYPE_IS_BIT()
 * @param channel: channel definition
 *
 * @return decimal places to print
 */
static int
modbusmq_channel_decimals(const modbusmq_input_t *input, const modbusmq_channel_t *channel)
{
    if (channel->has_decimals)
    {
        return channel->decimals;
    }

    if (MODBUSMQ_TYPE_IS_BIT(input->type))
    {
        return 0;
    }

    if (channel->format == modbusmq_data_format_float_ba   ||
        channel->format == modbusmq_data_format_float_abcd ||
        channel->format == modbusmq_data_format_float_badc ||
        channel->format == modbusmq_data_format_float_dcba ||
        channel->format == modbusmq_data_format_float_cdab)
    {
        return 3;
    }

    if (channel->mod < 0)
    {
        //
        // Peel factors of 10 off -mod by integer division rather than
        // log10(): a float log10 of e.g. 1000 can land a hair under 3.0 and
        // round the wrong way, and mod is an int to begin with.
        //
        int
            remaining = -channel->mod;
        int
            tens = 0;

        while (remaining > 1 && remaining % 10 == 0)
        {
            remaining /= 10;
            tens++;
        }

        return (remaining == 1) ? tens : 3;
    }

    return 0;
}

/**
 *
 * @brief format an already-scaled channel value the way it should be printed/published
 *
 * See modbusmq_channel_decimals() for how the decimal count is chosen.
 *
 * @param input  : input definition
 * @param channel: channel definition
 * @param value  : the scaled value, as returned by modbusmq_read_channel()
 * @param buf    : destination buffer
 * @param len    : size of buf
 *
 * @return characters written (snprintf semantics), < 0 on bad arguments
 */
int
modbusmq_channel_format_value(const modbusmq_input_t *input, const modbusmq_channel_t *channel, float value, char *buf, size_t len)
{
    if (!input || !channel || !buf || len == 0)
    {
        errno = EINVAL;
        return -1;
    }

    int
        decimals = modbusmq_channel_decimals(input, channel);
    int
        n = snprintf(buf, len, "%.*f", decimals, value);

    //
    // A value that is exactly zero, or close enough to round to it at this
    // decimal count, can still carry a negative sign into the printed form
    // ("-0", "-0.000") — technically what the float holds, but it reads as a
    // sign flip that never happened. Strip the sign when every digit printed
    // is a zero.
    //
    if (n > 0 && buf[0] == '-')
    {
        int
            all_zero = 1;

        for (int i = 1; i < n; ++i)
        {
            if (buf[i] != '0' && buf[i] != '.')
            {
                all_zero = 0;
                break;
            }
        }

        if (all_zero)
        {
            memmove(buf, buf + 1, n); // shift left, NUL included
            n--;
        }
    }

    return n;
}

//
// Record a publish decision into the channel's runtime state. Shared by every
// "publish" exit of modbusmq_channel_publish_decide() below so the bookkeeping
// cannot drift out of step between them.
//
static void
modbusmq_channel_publish_record(modbusmq_channel_t *channel, float value, const char *text, millitime_t now_ms)
{
    channel->last_value      = value;
    channel->last_publish_ms = now_ms;
    channel->published       = 1;

    strncpy(channel->last_text, text, sizeof(channel->last_text) - 1);
    channel->last_text[sizeof(channel->last_text) - 1] = 0;
}

/**
 *
 * @brief decide whether a channel value is worth publishing right now
 *
 * Order matters:
 *   1. never published before                    -> publish, nothing to compare to
 *   2. max_interval elapsed                       -> publish, this is the heartbeat
 *   3. inside min_interval                        -> suppress, too soon since the last publish
 *   4. on_change/min_change/min_change_rel active
 *      and the printed text is unchanged          -> suppress
 *   5. change below the min_change/min_change_rel
 *      threshold                                  -> suppress, not worth a message
 *   6. otherwise                                  -> publish
 *
 * "Unchanged" in step 4 is judged on text, not the float: two readings that
 * print identically at this channel's decimal count are the same value as far
 * as an MQTT subscriber can tell, so a raw value wobbling in the noise below
 * the last printed digit must not count as a change. min_change/min_change_rel
 * in step 5 are a magnitude gate instead, for a channel that wants to hold
 * back a real but small movement even though the text technically differs.
 *
 * min_change_rel is a fraction of the larger of the last and new magnitudes,
 * floored at 1.0 — without the floor a reading sitting near zero would need
 * an arbitrarily tiny absolute change to clear a relative threshold, which is
 * backwards from what "relative" is for.
 *
 * A changed value that arrives inside the min_interval window is dropped, not
 * queued for later: the next poll still compares against the last *published*
 * value and text (not the dropped one), so a real change is caught the first
 * time a poll lands outside the window rather than lost.
 *
 * On a publish decision this also updates channel->last_value,
 * channel->last_text, channel->last_publish_ms and channel->published, which
 * is why the channel pointer is non-const.
 *
 * @param channel: channel definition and runtime state
 * @param value  : the scaled value just read
 * @param text   : that same value already formatted by modbusmq_channel_format_value()
 * @param now_ms : current time, from millitime()
 *
 * @return 1 = publish, 0 = suppress, < 0 on bad arguments
 */
int
modbusmq_channel_publish_decide(modbusmq_channel_t *channel, float value, const char *text, millitime_t now_ms)
{
    if (!channel || !text)
    {
        errno = EINVAL;
        return -1;
    }

    if (!channel->published)
    {
        modbusmq_channel_publish_record(channel, value, text, now_ms);
        return 1;
    }

    millitime_t
        since_publish = now_ms - channel->last_publish_ms;

    if (channel->max_interval > 0 && since_publish >= (millitime_t)channel->max_interval)
    {
        modbusmq_channel_publish_record(channel, value, text, now_ms);
        return 1;
    }

    if (channel->min_interval > 0 && since_publish < (millitime_t)channel->min_interval)
    {
        return 0;
    }

    int
        change_gated = (channel->on_change || channel->min_change > 0 || channel->min_change_rel > 0);

    if (change_gated && strcmp(text, channel->last_text) == 0)
    {
        return 0;
    }

    float
        ref = MODBUSMQ_MAX(fabsf(channel->last_value), fabsf(value));
    float
        threshold = MODBUSMQ_MAX(channel->min_change, channel->min_change_rel * MODBUSMQ_MAX(1.0f, ref));

    if (threshold > 0 && fabsf(value - channel->last_value) < threshold)
    {
        return 0;
    }

    modbusmq_channel_publish_record(channel, value, text, now_ms);
    return 1;
}

/**
 *
 * @brief encode a raw value into wire bytes according to a data format
 *
 * The exact reverse of the byte orderings modbusmq_read_channel() decodes, and
 * nothing more: no scaling is applied here, so the value passed in is already a
 * raw register value. modbusmq_write_encode() is the one that undoes add/mod/mul;
 * modbusmq_server uses this directly to lay out its default values.
 *
 * @param format: one of enum modbusmq_data_format_e
 * @param value : raw value to encode
 * @param out   : at least 4 bytes of caller storage
 *
 * @return bytes written (1, 2 or 4), < 0 for a format that cannot be encoded
 */
int
modbusmq_encode_value(int format, double value, uint8_t *out)
{
    if (!out)
    {
        errno = EINVAL;
        return -1;
    }

    int
        length = modbusmq_format_size(format);

    if (length == 1)
    {
        out[0] = (uint8_t)((int)value & 0xff);
        return 1;
    }

    if (length == 2)
    {
        int
            ivalue = (int)(int64_t)value;

        switch (format)
        {
        case modbusmq_data_format_ab:
        case modbusmq_data_format_int16_ab: out[0] = (ivalue >> 8) & 0xff; out[1] = ivalue & 0xff; break;
        case modbusmq_data_format_ba:
        case modbusmq_data_format_int16_ba: out[1] = (ivalue >> 8) & 0xff; out[0] = ivalue & 0xff; break;
        default:
            return -1;
        }
        return 2;
    }

    if (length == 4)
    {
        uint32_t
            i;

        switch (format)
        {
        case modbusmq_data_format_float_abcd:
        case modbusmq_data_format_float_badc:
        case modbusmq_data_format_float_dcba:
        case modbusmq_data_format_float_cdab:
        {
            float f = value;
            memcpy(&i, &f, 4);
            break;
        }
        case modbusmq_data_format_abcd:
        case modbusmq_data_format_badc:
        case modbusmq_data_format_uint32_abcd:
        case modbusmq_data_format_uint32_badc:
            i = (uint32_t)(int64_t)value;
            break;
        default:
            return -1;
        }

        uint8_t
            a = (i >> 24) & 0xff,
            b = (i >> 16) & 0xff,
            c = (i >>  8) & 0xff,
            d =  i        & 0xff;

        switch (format)
        {
        case modbusmq_data_format_float_abcd:
        case modbusmq_data_format_abcd:
        case modbusmq_data_format_uint32_abcd:
            out[0] = a; out[1] = b; out[2] = c; out[3] = d;
            break;
        case modbusmq_data_format_float_badc:
        case modbusmq_data_format_badc:
        case modbusmq_data_format_uint32_badc:
            out[1] = a; out[0] = b; out[3] = c; out[2] = d;
            break;
        case modbusmq_data_format_float_dcba:
            out[3] = a; out[2] = b; out[1] = c; out[0] = d;
            break;
        case modbusmq_data_format_float_cdab:
            out[2] = a; out[3] = b; out[0] = c; out[1] = d;
            break;
        default:
            return -1;
        }
        return 4;
    }

    return -1;
}

/**
 *
 * @brief undo a write entry's scaling and encode the result as Modbus registers
 *
 * The exact inverse of modbusmq_read_channel(), applied in reverse order: that
 * function computes (raw + add), then mod, then mul, so this one undoes mul,
 * then mod, then add. The order matters — add lands before scaling, not after.
 *
 * Arithmetic is done in double rather than float so that a 32-bit raw value
 * survives the round trip; float runs out of mantissa above 2^24.
 *
 * Out-of-range values are rejected rather than truncated. A setpoint that
 * silently wraps to a small number is worse than one that never gets written,
 * because the device accepts it without complaint.
 *
 * Not for coil writes: a coil carries no format and no scaling, so
 * modbusmq_frame_write_coil_bit() takes the on/off decision directly.
 *
 * @param context: allocated context (for logging only)
 * @param write  : write entry supplying format and scaling
 * @param value  : the value as published, in engineering units
 * @param regs   : filled with up to 2 register values in wire order
 *
 * @return number of registers to write (1 or 2), < 0 on error
 */
int
modbusmq_write_encode(modbusmq_context_t *context, const modbusmq_write_t *write, double value, uint16_t *regs)
{
    if (!context || !write || !regs)
    {
        errno = EINVAL;
        return -1;
    }

    const char
        *name = write->name ? write->name : (write->topic ? write->topic : "?");

    if (!isfinite(value))
    {
        modbusmq_logf(LOG_ERROR, "write %s: value is not a finite number. action: skip write\n", name);
        return -1;
    }

    //
    // reverse of modbusmq_read_channel(): mul, then mod, then add
    //
    double
        raw = value;

    if (write->mul != 0)
    {
        raw /= write->mul;
    }
    if (write->mod > 0)
    {
        raw /= write->mod;
    }
    else if (write->mod < 0)
    {
        raw *= -(double)write->mod;
    }
    raw -= write->add;

    int
        is_float = (write->format == modbusmq_data_format_float_abcd ||
                    write->format == modbusmq_data_format_float_badc ||
                    write->format == modbusmq_data_format_float_dcba ||
                    write->format == modbusmq_data_format_float_cdab);

    if (!is_float)
    {
        double
            lo, hi;

        switch (write->format)
        {
        case modbusmq_data_format_int8:
            lo = -128.0;        hi = 127.0;
            break;
        case modbusmq_data_format_a:
            lo = 0.0;           hi = 255.0;
            break;
        case modbusmq_data_format_int16_ab:
        case modbusmq_data_format_int16_ba:
            lo = -32768.0;      hi = 32767.0;
            break;
        case modbusmq_data_format_ab:
        case modbusmq_data_format_ba:
            //
            // uint_ab/uint_ba read back as 0..65535. Accept the signed range
            // too and let it wrap into the same 16 bits, but refuse what fits
            // neither reading.
            //
            lo = -32768.0;      hi = 65535.0;
            break;
        case modbusmq_data_format_uint32_abcd:
        case modbusmq_data_format_uint32_badc:
            lo = 0.0;           hi = 4294967295.0;
            break;
        default:
            lo = -2147483648.0; hi = 2147483647.0;
            break;
        }

        //
        // Round rather than truncate: 25.5 C scaled by mod=-10 comes out as
        // 254.99999... in binary floating point and must not become 254.
        //
        // Guarded before the cast, not after: a double far outside the range
        // has no defined conversion to int64_t, so anything that cannot
        // possibly round into range is left alone for the check below to
        // reject. Done with a cast rather than floor() so the library does
        // not need libm for one rounding.
        //
        if (raw >= lo - 1.0 && raw <= hi + 1.0)
        {
            raw = (raw < 0) ? -(double)(int64_t)(-raw + 0.5)
                            :  (double)(int64_t)( raw + 0.5);
        }

        if (raw < lo || raw > hi)
        {
            modbusmq_logf(LOG_ERROR, "write %s: value %.6g scales to raw %.6g, outside [%.0f..%.0f] for this format. action: skip write\n",
                          name, value, raw, lo, hi);
            return -1;
        }
    }

    uint8_t
        buf[4] = {0};
    int
        nbytes = modbusmq_encode_value(write->format, raw, buf);

    if (nbytes != 2 && nbytes != 4)
    {
        modbusmq_logf(LOG_ERROR, "write %s: format %d cannot be written\n", name, (int)write->format);
        return -1;
    }

    //
    // The bytes are already in wire order, so pack them into registers as they
    // lie — that is what makes int_ba and the mixed-endian float layouts come
    // out right without a second byte-order table here.
    //
    regs[0] = (uint16_t)((buf[0] << 8) | buf[1]);
    if (nbytes == 4)
    {
        regs[1] = (uint16_t)((buf[2] << 8) | buf[3]);
    }

    return nbytes / 2;
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
        int matched = sscanf(input_string, "tcp://%199[^:]:%d", connect->device, &connect->port);

        if (matched == 2)
        {
            return 0;
        }
        return -2;
    }
    else if (connect->connect_type == MODBUSMQ_CONNECT_RTU)
    {
        // rtu:///device:baud:stopbit:databits:parity
        int matched = sscanf(input_string, "rtu://%199[^:]:%d:%d:%d:%c", connect->device, &connect->baudrate, &connect->stopbit, &connect->databits, &connect->parity);
        
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

        int matched = sscanf(input_string, "mqtt://%199[^:]:%d", connect->device, &connect->port);

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
    }

    assert(0);
    return -1;
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
    if (!context || !frame || !bits)
    {
        errno = EINVAL;
        return -1;
    }

    //
    // Modbus caps a single coil write at 1968 coils (246 payload bytes). The
    // cap also keeps the packing loop below inside frame->buf, which is
    // MODBUSMQ_FRAME_MAX and was previously written without any bound.
    //
    if (nbits <= 0 || nbits > 1968)
    {
        modbusmq_logf(LOG_ERROR, "write coils: %d coils is outside 1..1968\n", nbits);
        errno = EINVAL;
        return -1;
    }

    int
        nb = (nbits / 8) + (nbits % 8 ? 1 : 0);

    context->cb.modbusmq_write_coil_bits(context, frame, addr, nbits, bits);

    if (frame->length + 1 + nb > MODBUSMQ_FRAME_MAX)
    {
        modbusmq_logf(LOG_ERROR, "write coils: %d coils does not fit a frame\n", nbits);
        errno = EINVAL;
        return -1;
    }

    // number of bytes
    frame->buf[frame->length++] = nb;

    //
    // One byte of `bits` per coil, non-zero meaning on — the same shape
    // libmodbus uses, and far easier to get right than hand-packed bits.
    //
    // Packed low bit first: Modbus numbers coils from the least significant
    // bit up, so the coil at `addr` is bit 0 of the first byte. This used to
    // pack from the high bit down, which set entirely the wrong coils.
    //
    // The loop is bounded by nbits rather than by nb*8. Running the inner loop
    // a full 8 times per byte read past the end of the caller's array whenever
    // nbits was not a multiple of 8 — three coils read eight bytes.
    //
    for(int ibyte = 0; ibyte < nb; ++ibyte)
    {
        uint8_t
            byte = 0;

        for(int ibit = 0; ibit < 8; ++ibit)
        {
            int
                icoil = ibyte * 8 + ibit;

            if (icoil >= nbits)
            {
                break; // trailing bits of the last byte are padding, and zero
            }

            byte |= (bits[icoil] ? 1 : 0) << ibit;
        }

        frame->buf[frame->length++] = byte;
    }

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
    
    //
    // Modbus caps a single register write at 123 registers, which also keeps
    // the copy below inside frame->buf. naddr is caller-supplied and used to
    // be trusted unchecked.
    //
    if (naddr <= 0 || naddr > 123 || !values)
    {
        modbusmq_logf(LOG_ERROR, "write registers: %d registers is outside 1..123\n", naddr);
        errno = EINVAL;
        return -1;
    }

    context->cb.modbusmq_write_registers(context, frame, addr, naddr, values);

    if (frame->length + 1 + naddr * 2 > MODBUSMQ_FRAME_MAX)
    {
        modbusmq_logf(LOG_ERROR, "write registers: %d registers does not fit a frame\n", naddr);
        errno = EINVAL;
        return -1;
    }

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

    //
    // The transport laid down the address and the AND-mask; only the OR-mask
    // is left. Both used to be appended here on top of what the transport had
    // already written, so the frame carried each mask twice.
    //
    frame->buf[frame->length++] = or_mask >> 8;
    frame->buf[frame->length++] = or_mask & 0x00ff;

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
modbusmq_post_internal(modbusmq_context_t *context, modbusmq_msg_t *msg, int flags)
{

    modbusmq_msg_wrapper_t
        *wrapper = (modbusmq_msg_wrapper_t *)malloc(sizeof(modbusmq_msg_wrapper_t));
    if (!wrapper)
    {
        modbusmq_logf(LOG_ERROR, "Unable to allocate message wrapper\n");
        return -1;
    }

    //
    // Stamp before the copy so the caller's message — for a subscription, the
    // timer's template — carries the same id as the queued one.
    //
    msg->req_id = modbusmq_next_req_id(context);

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

    if (!context->config)
    {
        modbusmq_logf(LOG_ERROR, "modbusmq_subscribe: context->config not set — call modbusmq_set_config() first\n");
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
        if (context->config->query_mode == modbusmq_query_mode_parallell)
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
        else if (context->config->query_mode == modbusmq_query_mode_series)
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
 * @brief Size and validate a frame from its own bytes.
 *
 * Called when a response fails to match the request it was read for. Such a
 * frame is either a reply to a request already given up on, whose real answer
 * was merely late rather than lost, or genuine corruption — and the two are
 * told apart by asking whether the bytes form a complete, well-formed frame
 * on their own terms: the MBAP length field on tcp, the declared byte count
 * plus CRC on rtu. Neither transport needs to be told which requests are
 * outstanding to answer that.
 *
 * On a partial frame the reader's length is narrowed to the frame's own, so
 * the following reads stop on its boundary instead of on the boundary the
 * current request expected. The frame timeout remains the backstop for a
 * frame whose remainder never arrives.
 *
 * @param context: allocated context
 * @param frame: reader frame holding the unmatched bytes
 *
 * @return 1 when a whole valid frame has been read and can be discarded
 *         0 when more bytes are needed; frame->length sized to this frame
 *        < 0 when the bytes do not form a frame, so the stream is corrupt
 */
int
modbusmq_frame_drain(modbusmq_context_t *context, modbusmq_frame_t *frame)
{
    if (!context || !frame)
    {
        errno = EINVAL;
        return -1;
    }

    return context->cb.modbusmq_frame_drain(context, frame);
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
            //
            // About to put a fresh request on the wire: make sure nothing is
            // left over from a frame we gave up on, or its bytes become the
            // head of this request's response.
            //
            if (context->resync_pending && writer->xmit == 0)
            {
                modbusmq_resync(context);
            }

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
                return MODBUSMQ_ERR_TRANSPORT;
            }

            // Ensure what we have read matches what we requested
            if (modbusmq_check_header(context, msg) < 0)
            {
                //
                // A late reply to a request already given up on looks exactly
                // like this: a well-formed frame that identifies as someone
                // else's (wrong transaction id on tcp, wrong slave, function
                // or byte count on rtu). Ask the transport to size and check
                // the frame against its own bytes; one that holds up is a real
                // frame that simply is not ours, so step over exactly its own
                // bytes and keep waiting for the answer to this request.
                //
                // Flushing the whole receive buffer here instead is what turns
                // one late response into a permanent one-behind desync: at a
                // steady polling cadence this message's own real answer is
                // often already queued right behind the stale one, so the
                // flush discards it too and hands the backlog forward for
                // good.
                //
                rc = modbusmq_frame_drain(context, reader);

                if (rc > 0)
                {
                    // stale frame stepped over; wait for this request's own answer
                    memset(reader, 0, sizeof(*reader));
                    reader->length = context->header_length;
                }

                if (rc >= 0)
                {
                    //
                    // Either the frame is stepped over, or only part of it has
                    // arrived and reader->length now describes that frame
                    // rather than the one we asked for, so the following reads
                    // stop on its boundary. Both leave a reader waiting on
                    // bytes, which the accounting at the end of this function
                    // reports as work still pending; the frame timeout in
                    // modbusmq_loop_prepare() covers a remainder that never
                    // comes.
                    //
                    rc = 0;

                    goto pending;
                }

                if (modbusmq_debug >= 1)
                {
                    modbusmq_frame_debug(context, reader);
                }

                //
                // The bytes do not form a frame at all, so the stream itself is
                // out of step and there is no boundary to step over. Drain to
                // silence and give up on this message.
                //
                context->err++;
                modbusmq_flush(context);
                context->resync_pending = 1;

                memset(reader, 0, sizeof(*reader));
                reader->length = context->header_length;

                return MODBUSMQ_ERR_PROTOCOL;
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

pending:

    // return number of bytes pending to write+read
    rc =
        writer->length - writer->xmit +
        reader->length - reader->xmit;

    //
    // 0 means "request written, response complete" to the caller, so never let
    // a reader that has not actually received a whole frame reach it that way
    // (a zero-length reader trivially satisfies length == xmit).
    //
    if (rc == 0 && (reader->length < context->res_header_min || reader->xmit != reader->length))
    {
        rc = context->res_header_min - reader->xmit;
        if (rc <= 0)
        {
            rc = 1;
        }
    }

    return rc;

}


/**
 *
 * @brief Drain the underlying transport receive buffer.
 *
 * For RTU this drains until the line has been silent for a full inter-frame
 * gap, so the stream is left aligned on a real frame boundary. Used to recover
 * from framing or protocol errors.
 *
 * @param context: allocated context
 *
 * @return number of bytes discarded, or < 0 on error
 */

int
modbusmq_flush(modbusmq_context_t *context)
{
    if (!context)
    {
        errno = EINVAL;
        return -1;
    }
    else if (context->tcp)
    {
        return modbusmq_tcp_flush(context);
    }
    else if (context->rtu)
    {
        return modbusmq_rtu_flush(context);
    }

    assert(0);
    return -1;
}

/**
 *
 * @brief Bring the receive stream back to a known-clean state.
 *
 * Called after a frame has been discarded, and again before the next request
 * goes out. The two calls do different jobs: the first swallows whatever is
 * still arriving right now, the second catches a reply that only showed up
 * after we had already given up on it. Without the second one a late response
 * is still sitting in the buffer when the next request is sent, and gets read
 * as that request's answer — every following response then belongs to the
 * previous request, permanently, which maps every value onto the wrong topic.
 *
 * @param context: allocated context
 *
 * @return nothing
 */
void
modbusmq_resync(modbusmq_context_t *context)
{
    if (!context)
    {
        errno = EINVAL;
        return;
    }

    context->resync_pending = 0;
    modbusmq_flush(context);
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

    //
    // Only a fully received, CRC-verified frame may be decoded. rc > 0 means
    // the frame is still short: its buffer tail is whatever was there before,
    // and publishing that produces channels that read as clean zeros. rc < 0
    // means it was rejected outright. Either way, drop the message — leaving it
    // queued would stall this subscription until the frame timeout evicts it.
    //
    if (rc != 0)
    {
        modbusmq_logf(LOG_ERROR, "%s discarding response: %s (rc=%d). action: discard frame and resync\n",
                      modbusmq_msg_tag(context, &wrapper->msg),
                      rc < 0 ? "failed validation" : "incomplete frame",
                      rc);

        context->err++;

        // drain what is on the wire now, and again before the next request in
        // case the frame we gave up on is still on its way
        modbusmq_flush(context);
        context->resync_pending = 1;

        modbusmq_report_error(context, &wrapper->msg, MODBUSMQ_ERR_PROTOCOL);

        context->msg_wrapper_head = context->msg_wrapper_head->next;
        free(wrapper);

        return MODBUSMQ_ERR_PROTOCOL;
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

        //
        // Find the input block this response answers. Slave and address alone
        // do not identify it: coils, discrete inputs, holding and input
        // registers are four separate address spaces on the wire, so a status
        // block at input_register 0x0000 and an alarm block at discrete_input
        // 0x0000 are both legitimate and both match on address. Matching on
        // the function code too keeps a 5-byte bit response from being decoded
        // against a 31-register channel table, and register bytes from being
        // published as alarm bits. modbusmq_input_t.type is the read function
        // code, so it compares directly.
        //
        // Everything is taken from the request frame: it is what we sent, and
        // it is the one the address_offset was applied to.
        //
        int
            function = modbusmq_frame_function(context, &wrapper->msg.frame[0]),
            address  = modbusmq_frame_addr(    context, &wrapper->msg.frame[0]),
            slave    = modbusmq_frame_slave(   context, &wrapper->msg.frame[0]),
            matched  = 0;

        for(int i = 0; i < modbusmq_config->input_max; ++i)
        {
            modbusmq_input_t
                *input = &modbusmq_config->inputs[i];

            if (input->slave != slave     ||
                input->type  != function  ||
                input->address + input->address_offset != address)
            {
                continue;
            }

            context->subscribe_cb(context, &wrapper->msg, input);
            matched++;
        }

        if (matched == 0)
        {
            modbusmq_logf(LOG_ERROR, "%s response matches no input block (function %d). action: dropped\n",
                          modbusmq_msg_tag(context, &wrapper->msg), function);
        }
    }
                
    // clean up
    context->msg_wrapper_head = context->msg_wrapper_head->next;
    free(wrapper);
    wrapper = NULL;

    return 0;
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
 * @return the descriptor to poll, or -1 when there is no connection
 */

int
modbusmq_loop_prepare(modbusmq_context_t *context, millitime_t *sleep_time, int16_t *events)
{
    if (!context)
    {
        errno = EINVAL;
        return -1;
    }

    if (context->fd < 0)
    {
        return -1;
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

        if (wrapper->head_since_ms == 0)
        {
            wrapper->head_since_ms = time_now;
        }

        modbusmq_frame_t
            *head_writer = wrapper->msg.frame[0].is_writer ? &wrapper->msg.frame[0] : &wrapper->msg.frame[1];

        //
        // last_write_ms only means anything once this request is actually on
        // the wire. A message that is still queued behind the previous one
        // would otherwise be timed out against a write it never made — with a
        // poll interval longer than frame_timeout that discards requests before
        // they are ever sent.
        //
        int
            request_sent = (head_writer->length > 0 && head_writer->xmit == head_writer->length);

        if (request_sent && (time_now - context->last_write_ms) >= context->frame_timeout_ms)
        {
            // discard this packages
            modbusmq_logf(LOG_ERROR, "%s no response within %d ms. action: discard request and continue\n",
                          modbusmq_msg_tag(context, &wrapper->msg),
                          context->frame_timeout_ms);

            modbusmq_report_error(context, &wrapper->msg, MODBUSMQ_ERR_TIMEOUT);

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

            //
            // The response may simply be late rather than lost. Drain now, and
            // again before the next request goes out — otherwise it arrives
            // into an empty buffer and gets read as the answer to whichever
            // request follows, shifting request/response pairing by one for
            // good. Every frame then still passes slave, function and CRC
            // checks while carrying another block's registers.
            //
            // A late frame that slips past both drains is caught on arrival
            // instead: it fails the header check against whichever request is
            // then outstanding, and modbusmq_frame_drain() steps over it
            // without disturbing that request.
            //
            modbusmq_flush(context);
            context->resync_pending = 1;
        }
        else if (!request_sent && (time_now - wrapper->head_since_ms) >= context->frame_timeout_ms)
        {
            //
            // The clock here runs from when this wrapper became head of the
            // queue, not from when it was posted. In parallel query mode a
            // request sits behind others until it is its turn; timing it
            // from post would discard it the moment it reached the head on
            // a slow RTU link, before it ever had a chance to go out.
            //
            modbusmq_logf(LOG_ERROR, "%s not sent within %d ms, the link never became writable. action: discard request and continue\n",
                          modbusmq_msg_tag(context, &wrapper->msg),
                          context->frame_timeout_ms);

            modbusmq_report_error(context, &wrapper->msg, MODBUSMQ_ERR_TIMEOUT);

            context->msg_wrapper_head = context->msg_wrapper_head->next;
            free(wrapper);
            wrapper = NULL;

            context->err++;
            context->last_write_ms = 0;

            modbusmq_flush(context);
            context->resync_pending = 1;
        }
    }

    modbusmq_loop_prepare_subscription(context, sleep_time, events);
    
    wrapper = context->msg_wrapper_head;
    if (!wrapper)
    {
        // queue empty, but the connection is still live — the caller polls
        // the fd anyway so it notices POLLERR/POLLHUP.
        return context->fd;
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

    // neither frame needs write nor read right now, but the connection is
    // still live — the caller polls the fd anyway so it notices
    // POLLERR/POLLHUP.
    return context->fd;
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
 * @return 0 when a complete, validated response was received
 *         > 0 when the wait expired without one
 *         < 0 on error (MODBUSMQ_ERR_TRANSPORT / MODBUSMQ_ERR_PROTOCOL)
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

    msg->req_id = modbusmq_next_req_id(context);

    struct pollfd pollfds[10];

    millitime_t
        start_time_ms = millitime(),
        time_now      = start_time_ms;


    modbusmq_msg_prepare(context, msg);

    int
        result = 1; // no complete response yet

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
            if (errno == EINTR)
            {
                continue;
            }
            modbusmq_logf(LOG_ERROR, "poll failed: rc = %d, errno=%d, str=%s\n", rc, errno, strerror(errno));
            result = MODBUSMQ_ERR_TRANSPORT;
            break;
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
                if (rc == MODBUSMQ_ERR_PROTOCOL)
                {
                    //
                    // A frame was rejected and the stream resynced. The reader
                    // has been rearmed, so keep waiting out the remaining time
                    // in case the real answer is still coming, but remember the
                    // failure in case it never does.
                    //
                    modbusmq_logf(LOG_ERROR, "%s frame rejected, stream resynced. action: keep waiting for the real response\n",
                                  modbusmq_msg_tag(context, msg));
                    modbusmq_report_error(context, msg, MODBUSMQ_ERR_PROTOCOL);
                    result = MODBUSMQ_ERR_PROTOCOL;
                }
                else if (rc < 0)
                {
                    modbusmq_logf(LOG_ERROR, "%s transport error rc=%d, err=%s. action: give up\n",
                                  modbusmq_msg_tag(context, msg), rc, strerror(errno));
                    modbusmq_report_error(context, msg, MODBUSMQ_ERR_TRANSPORT);
                    result = MODBUSMQ_ERR_TRANSPORT;
                    break;
                }
                else if (rc == 0)
                {
                    //
                    // Frame is complete — validate it before reporting success,
                    // so a caller that only checks the return value never
                    // decodes an unverified response.
                    //
                    result = modbusmq_check(context, msg);
                    if (result != 0)
                    {
                        modbusmq_logf(LOG_ERROR, "%s response failed validation (rc=%d). action: discard frame and resync\n",
                                      modbusmq_msg_tag(context, msg), result);
                        modbusmq_flush(context);
                        context->resync_pending = 1;
                        modbusmq_report_error(context, msg, MODBUSMQ_ERR_PROTOCOL);
                        result = MODBUSMQ_ERR_PROTOCOL;
                    }
                    break;
                }

            }
        }
    } while ((time_now - start_time_ms) < mswait);

    if (result > 0)
    {
        modbusmq_logf(LOG_ERROR, "%s no complete response within %d ms. action: give up\n",
                      modbusmq_msg_tag(context, msg), mswait);
        modbusmq_report_error(context, msg, MODBUSMQ_ERR_TIMEOUT);
    }

    return result;
}

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
        //
        // Nothing queued, but the caller polls the descriptor even when idle so
        // a link that drops between requests is noticed now rather than when
        // the next timer fires and finds it dead.
        //
        if (revents & (POLLERR | POLLHUP))
        {
            context->err++;
            modbusmq_logf(LOG_ERROR, "connection closed while idle (revents=0x%x)\n", revents);
            return MODBUSMQ_ERR_TRANSPORT;
        }

        return 0;
    }
    
    int
        rc = modbusmq_handle_write_read(context, &wrapper->msg, revents);

    if (rc == 0)
    {
        // One message is complete: request + response
        context->tx++;
        context->rx++;
        rc = modbusmq_handle_msg(context, wrapper);
    }
    else if (rc == MODBUSMQ_ERR_PROTOCOL)
    {
        //
        // The frame was rejected and the stream has been resynced, but this
        // message is now unanswerable: drop it so the queue moves on to the
        // next request instead of stalling until the frame timeout.
        //
        modbusmq_logf(LOG_ERROR, "%s response rejected. action: discard request and continue\n",
                      modbusmq_msg_tag(context, &wrapper->msg));

        modbusmq_report_error(context, &wrapper->msg, MODBUSMQ_ERR_PROTOCOL);

        context->msg_wrapper_head = wrapper->next;
        free(wrapper);
    }
    else if (rc < 0)
    {
        context->err++;
        modbusmq_logf(LOG_ERROR, "%s transport error rc=%d, err=%s\n",
                      modbusmq_msg_tag(context, &wrapper->msg), rc, strerror(errno));
        modbusmq_report_error(context, &wrapper->msg, MODBUSMQ_ERR_TRANSPORT);

        //
        // The request is dead either way once the transport has failed.
        // Unlink and free it here so a caller that does not call
        // modbusmq_reset_queue() isn't left blocked behind it, and so it
        // doesn't get the same failure reported again on every subsequent
        // poll.
        //
        context->msg_wrapper_head = wrapper->next;
        free(wrapper);
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
