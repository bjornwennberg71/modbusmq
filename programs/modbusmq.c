//////////////////////////////////////////////////////////////////////////////
// 
// bjornwennberg71@gmail.com
// 
// modbusmq.c
// 

// INCLUDES //////////////////////////////////////////////////////////////////
#include "modbusmq.h"
#include "modbusmq_log.h"
#include "modbusmq_config.h"

#if MQTT_ENABLED
#include <mosquitto.h>
#endif

#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <assert.h>
#include <errno.h>
#include <strings.h>

#include <fcntl.h>
#include <poll.h>

const char *version = MODBUSMQ_VERSION_STRING;
#define MQTT_RECONNECT_INTERVAL_MS    5000 // retry every 5 seconds
#define MODBUS_RECONNECT_INTERVAL_MS  5000 // retry every 5 seconds

//
// global parameters from settings / argument line
//
#define MAX_CONFIG_OVERRIDES 64

//
// One write waiting for its answer.
//
// The library reports success and failure through two different callbacks and
// identifies the request by nothing but its req_id, so everything the ack has
// to say -- which entry it belongs to, what was written -- is held here until
// one of the two fires. Exactly one always does: a posted message ends in the
// message callback, in the error callback, or in the queue reset that reports
// every outstanding request as a transport failure.
//
typedef struct pending_write_t
{
    uint32_t req_id;
    int      write_index;
    char     value[96];   // what was written, as the ack reports it
} pending_write_t;

//
// One write is on the wire at a time, but nothing bounds how many are waiting
// behind it: every MQTT message that arrives is posted on the spot, and only
// the head of that queue is in flight. A bulk init that publishes to a few
// hundred write topics at once therefore has a few hundred writes outstanding
// together, so the table grows with the queue rather than standing at a size
// such a burst walks straight past, losing the acks of everything it passed.
//
// It is still an array scanned end to end. The scan is over writes waiting for
// an answer, not over the run, and it drains as fast as the bus replies.
//
#define PENDING_WRITE_INITIAL 32

typedef struct global_info
{
    char config_filename[300];
    int  has_config;

    // -e key=value entries, pointing straight into argv
    char *overrides[MAX_CONFIG_OVERRIDES];
    int   noverrides;

    int  has_mqtt;
    int  mqtt_connected;              // 1 = connected to broker, 0 = disconnected
    millitime_t mqtt_reconnect_at_ms; // when to attempt next reconnect

    int  modbus_connected;              // 1 = connected to device, 0 = disconnected
    millitime_t modbus_reconnect_at_ms; // when to attempt next reconnect

    int verbose;
    struct mosquitto *mosq;

    pending_write_t *pending;
    int              npending;
    int              pending_max;   // entries allocated, high-water mark of one burst
} global_info;
 

 
static global_info GI;

#if MQTT_ENABLED
//////////////////////////////////////////////////////////////////////////////
//
// Append to a buffer being filled a piece at a time.
//
// The cursor is clamped rather than advanced by whatever snprintf() says it
// would have written. Plain "pos += snprintf(buf+pos, n-pos, ...)" walks the
// cursor past the end of the buffer the moment one piece truncates, and every
// append after that writes somewhere else entirely.
//
static void
json_append(char *buf, int nbuf, int *pos, const char *fmt, ...)
{
    if (*pos < 0 || *pos >= nbuf - 1)
    {
        return;
    }

    va_list
        ap;

    va_start(ap, fmt);
    int
        n = vsnprintf(buf + *pos, nbuf - *pos, fmt, ap);
    va_end(ap);

    if (n < 0)
    {
        return;
    }

    *pos += (n >= nbuf - *pos) ? (nbuf - *pos - 1) : n;
}

//////////////////////////////////////////////////////////////////////////////
//
// Copy text into a JSON string body, escaping what JSON will not take raw.
//
// Names and topics come from a config file and are not this program's to
// assume well behaved. An unescaped quote does not corrupt the ack, it
// re-parses it: the consumer reads a different document than the one meant,
// and finds "ok" in a field that was never about the outcome.
//
// Truncation lands on a whole escape, never half of one.
//
static void
json_escape(char *out, int nout, const char *in)
{
    int
        o = 0;

    if (nout <= 0)
    {
        return;
    }

    for(const unsigned char *p = (const unsigned char *)(in ? in : ""); *p; ++p)
    {
        char
            esc[8];
        int
            n;

        switch (*p)
        {
        case '"':  esc[0] = '\\'; esc[1] = '"';  n = 2; break;
        case '\\': esc[0] = '\\'; esc[1] = '\\'; n = 2; break;
        case '\n': esc[0] = '\\'; esc[1] = 'n';  n = 2; break;
        case '\r': esc[0] = '\\'; esc[1] = 'r';  n = 2; break;
        case '\t': esc[0] = '\\'; esc[1] = 't';  n = 2; break;
        default:
            if (*p < 0x20)
            {
                n = snprintf(esc, sizeof(esc), "\\u%04x", *p);
            }
            else
            {
                esc[0] = (char)*p;
                n = 1;
            }
            break;
        }

        if (o + n >= nout)
        {
            break;
        }

        memcpy(out + o, esc, n);
        o += n;
    }

    out[o] = 0;
}

//////////////////////////////////////////////////////////////////////////////
//
// Remember a write that is now on the queue, so its answer can be tied back
// to it when one of the callbacks fires.
//
static void
pending_write_add(uint32_t req_id, int write_index, const char *value)
{
    //
    // Grow to fit. Every posted message is reported exactly once, so the table
    // drains on its own and what is allocated here is one burst of writes, not
    // the run.
    //
    if (GI.npending >= GI.pending_max)
    {
        int
            nmax = GI.pending_max ? GI.pending_max * 2 : PENDING_WRITE_INITIAL;
        pending_write_t
            *grown = realloc(GI.pending, (size_t)nmax * sizeof(*grown));

        if (grown)
        {
            GI.pending     = grown;
            GI.pending_max = nmax;
        }
        else if (GI.npending > 0)
        {
            //
            // Out of memory is no reason to lose the newest write. Drop the
            // oldest instead -- it is the one the frame timeout is about to
            // answer anyway -- and say so.
            //
            modbusmq_logf(LOG_ERROR, "ack: out of memory for %d writes waiting for an answer, dropping the oldest (req %u)\n",
                          GI.npending, GI.pending[0].req_id);

            memmove(&GI.pending[0], &GI.pending[1], (size_t)(GI.pending_max - 1) * sizeof(GI.pending[0]));
            GI.npending = GI.pending_max - 1;
        }
        else
        {
            modbusmq_logf(LOG_ERROR, "ack: out of memory, write req %u goes out unacked\n", req_id);
            return;
        }
    }

    pending_write_t
        *pending = &GI.pending[GI.npending++];

    memset(pending, 0, sizeof(*pending));
    pending->req_id      = req_id;
    pending->write_index = write_index;

    snprintf(pending->value, sizeof(pending->value), "%s", value ? value : "");
}

//////////////////////////////////////////////////////////////////////////////
//
// Find the write a req_id belongs to and forget it.
//
// Every posted message is reported exactly once, so taking the entry out here
// is what keeps the table bounded. A req_id that is not ours is a poll or a
// request from somewhere else and is not an error.
//
// @return the entry, valid until the next call, or 0
//
static pending_write_t *
pending_write_take(uint32_t req_id)
{
    static pending_write_t
        taken;

    for(int i = 0; i < GI.npending; ++i)
    {
        if (GI.pending[i].req_id != req_id)
        {
            continue;
        }

        taken = GI.pending[i];

        memmove(&GI.pending[i], &GI.pending[i+1], (GI.npending - i - 1) * sizeof(GI.pending[0]));
        GI.npending--;

        return &taken;
    }

    return NULL;
}

//////////////////////////////////////////////////////////////////////////////
//
// Publish one ack or nack for a write.
//
// JSON, because the failure cases carry more than a word: which request, which
// entry, and for a device that said no, the exception code it said no with.
//
// Never retained. An ack is something that happened once, and a retained one
// would be replayed to every subscriber that connects afterwards, telling them
// a write just succeeded when it succeeded last Tuesday.
//
// @param write  : the entry this is about
// @param req_id : the library's request id, or 0 when the write never got one
// @param status : "ok" or "error"
// @param value  : what was written, or 0
// @param reason : short machine-readable cause, or 0
// @param message: human-readable detail, or 0
// @param code   : Modbus exception code, or 0
//
static void
mqtt_write_result(const modbusmq_write_t *write, uint32_t req_id, const char *status,
                  const char *value, const char *reason, const char *message, int code)
{
    if (!write->ack || !write->ack_topic)
    {
        return;
    }

    if (!GI.has_mqtt || !GI.mqtt_connected)
    {
        modbusmq_logf(LOG_ERROR, "ack %s: not connected to the broker. action: ack dropped\n", write->ack_topic);
        return;
    }

    modbusmq_config_t
        *modbusmq_config = modbusmq_config_get();

    char
        name[128],
        topic[256],
        vbuf[192],
        mbuf[192],
        payload[900];
    int
        pos = 0;

    json_escape(name,  sizeof(name),  write->name);
    json_escape(topic, sizeof(topic), write->topic);
    json_escape(vbuf,  sizeof(vbuf),  value);
    json_escape(mbuf,  sizeof(mbuf),  message);

    json_append(payload, sizeof(payload), &pos, "{\"status\":\"%s\"", status);

    if (write->name)
    {
        json_append(payload, sizeof(payload), &pos, ",\"name\":\"%s\"", name);
    }
    if (write->topic)
    {
        json_append(payload, sizeof(payload), &pos, ",\"topic\":\"%s\"", topic);
    }
    //
    // No req_id means the write was refused before it was ever queued, so
    // there is no request for the consumer to correlate with. Leaving the
    // field out says that; a zero would read as a request that exists.
    //
    if (req_id)
    {
        json_append(payload, sizeof(payload), &pos, ",\"req\":%u", req_id);
    }
    if (value)
    {
        json_append(payload, sizeof(payload), &pos, ",\"value\":\"%s\"", vbuf);
    }
    if (reason)
    {
        json_append(payload, sizeof(payload), &pos, ",\"reason\":\"%s\"", reason);
    }
    if (code > 0)
    {
        json_append(payload, sizeof(payload), &pos, ",\"code\":%d", code);
    }
    if (message)
    {
        json_append(payload, sizeof(payload), &pos, ",\"message\":\"%s\"", mbuf);
    }

    json_append(payload, sizeof(payload), &pos, "}");

    int
        rc = mosquitto_publish(GI.mosq, NULL, write->ack_topic, pos, payload, modbusmq_config->mqtt_qos, 0);

    if (rc == MOSQ_ERR_NO_CONN || rc == MOSQ_ERR_CONN_LOST)
    {
        modbusmq_logf(LOG_INFO, "MQTT: lost connection while publishing an ack, will reconnect\n");
        GI.mqtt_connected = 0;
        return;
    }
    if (rc != MOSQ_ERR_SUCCESS)
    {
        modbusmq_logf(LOG_ERROR, "MQTT: unable to publish ack to %s (rc=%d)\n", write->ack_topic, rc);
        return;
    }

    modbusmq_logf(LOG_DEBUG, "%s=%s\n", write->ack_topic, payload);
}

static void
mqtt_write_ack(const modbusmq_write_t *write, uint32_t req_id, const char *value)
{
    mqtt_write_result(write, req_id, "ok", value, NULL, NULL, 0);
}

static void
mqtt_write_nack(const modbusmq_write_t *write, uint32_t req_id, const char *reason, const char *message, int code)
{
    mqtt_write_result(write, req_id, "error", NULL, reason, message, code);
}

//////////////////////////////////////////////////////////////////////////////
//
// The machine-readable half of a nack: the same failure the log line spells
// out in words, in a form a consumer can branch on without parsing prose.
//
static const char *
modbusmq_error_tag(int error)
{
    switch (error)
    {
    case MODBUSMQ_ERR_TIMEOUT:   return "timeout";
    case MODBUSMQ_ERR_PROTOCOL:  return "protocol";
    case MODBUSMQ_ERR_TRANSPORT: return "transport";
    case MODBUSMQ_ERR_EXCEPTION: return "exception";
    default: break;
    }

    return "error";
}
#endif // MQTT_ENABLED

//////////////////////////////////////////////////////////////////////////////
// 
// 
// called when a posted request gets a valid response
//
// For this program a posted request is always a write, so this is the ack: the
// device returned a well-formed echo carrying the function and slave that were
// asked for. That is as much as an echo can say -- it is not a read-back, and
// a device that accepts a setpoint and then clamps it will still echo the
// frame -- so the ack means "the write was accepted", not "the register now
// reads what you sent".
//
// A req_id that is not in the pending table belongs to something else that was
// posted, which is not an error and not this function's business.
//
void
modbusmq_message_callback(struct modbusmq_context_t *context, modbusmq_msg_t *msg)
{
#if MQTT_ENABLED
    pending_write_t
        *pending = pending_write_take(msg->req_id);

    if (!pending)
    {
        return;
    }

    modbusmq_config_t
        *modbusmq_config = modbusmq_config_get();
    const modbusmq_write_t
        *write = &modbusmq_config->writes[pending->write_index];

    modbusmq_logf(LOG_INFO, "write %s: slave %d accepted req %u\n",
                  write->name ? write->name : write->topic,
                  modbusmq_frame_slave(context, &msg->frame[0]),
                  msg->req_id);

    mqtt_write_ack(write, msg->req_id, pending->value);
#else
    (void)context;
    (void)msg;
#endif
}

//////////////////////////////////////////////////////////////////////////////
//
// called when the library gives up on a request
//
// The library has already logged the details and recovered on its own; this is
// where an integrator hooks up whatever their system does about it — mark the
// slave offline, raise an alarm, back off its poll interval. Everything needed
// to tell the devices on a shared RTU bus apart is on msg.
//
void
modbusmq_error_callback(struct modbusmq_context_t *context, modbusmq_msg_t *msg, int error)
{
    const char
        *reason = "failed";

    int
        code = 0;
    char
        detail[160];

    switch(error)
    {
    case MODBUSMQ_ERR_TIMEOUT:
        reason = "did not answer";
        break;
    case MODBUSMQ_ERR_PROTOCOL:
        reason = "answered with a frame that was rejected";
        break;
    case MODBUSMQ_ERR_TRANSPORT:
        reason = "is unreachable, the connection is gone";
        break;
    case MODBUSMQ_ERR_EXCEPTION:
        //
        // The device heard the request and declined it. The code is the only
        // part of this worth passing on -- it is the difference between a
        // register that does not exist and one the device will not let you
        // write today.
        //
        code   = modbusmq_frame_exception_code(context, &msg->frame[1]);
        snprintf(detail, sizeof(detail), "refused the request: %s (%d)", modbusmq_exception_string(code), code);
        reason = detail;
        break;
    }

    modbusmq_logf(LOG_ERROR, "slave %d (addr 0x%04X, req %u) %s\n",
                  modbusmq_frame_slave(context, &msg->frame[0]),
                  modbusmq_frame_addr(context, &msg->frame[0]),
                  msg->req_id,
                  reason);

#if MQTT_ENABLED
    //
    // ...and if this was a write somebody asked for over MQTT, tell them so
    // rather than only the log.
    //
    pending_write_t
        *pending = pending_write_take(msg->req_id);

    if (pending)
    {
        modbusmq_config_t
            *modbusmq_config = modbusmq_config_get();

        mqtt_write_nack(&modbusmq_config->writes[pending->write_index], msg->req_id,
                        modbusmq_error_tag(error),
                        code > 0 ? modbusmq_exception_string(code) : modbusmq_strerror(error),
                        code);
    }
#endif
}

//////////////////////////////////////////////////////////////////////////////
//
// debug print channel information
void
modbusmq_channel_debug(modbusmq_channel_t *channel)
{
    printf("channel.offset = 0x%02X\n", channel->offset);
    printf("channel.topic  = %s\n",     channel->topic);
}

//////////////////////////////////////////////////////////////////////////////
// 
// every time we receive data from the modbusmq device, this function is called
//
void
modbusmq_subscription_callback(struct modbusmq_context_t *context, modbusmq_msg_t *msg, modbusmq_input_t *input)
{
#if MQTT_ENABLED
    // only the publish path below reads it, and that path is compiled out
    modbusmq_config_t
        *modbusmq_config = modbusmq_config_get();
#endif
    char
        value[MODBUSMQ_TEXT_MAX];

    for(int c = 0; c < input->channel_max; ++c)
    {
        modbusmq_channel_t
            *channel = &input->channels[c];

        //
        // Skip rather than publish: a channel outside the received data decodes
        // as 0, and a published 0 is indistinguishable from a real reading.
        //
        if (modbusmq_channel_in_range(context, msg, input, channel) != 0)
        {
            continue;
        }

        float
            f = 0;

        //
        // A serial number, a firmware version or a timestamp decodes straight
        // to characters — see modbusmq_read_channel_text(), which exists
        // because none of the three survives the trip through a float. It
        // fails rather than guessing, and a channel it could not decode is
        // skipped for the same reason an out-of-range one is: a wrong serial
        // number is indistinguishable from a right one.
        //
        if (modbusmq_format_is_text(channel->format))
        {
            if (modbusmq_read_channel_text(context, msg, input, channel, value, sizeof(value)) < 0)
            {
                continue;
            }
        }
        else
        {
            f = modbusmq_read_channel(context, msg, input, channel);

            //
            // min_change/min_interval/max_interval decide whether this reading
            // is worth sending at all — a status word flooding the broker
            // unchanged every poll helps nobody. The formatted value is still
            // logged either way, at debug level, so -v shows what a suppressed
            // channel would have published.
            //
            modbusmq_channel_format_value(input, channel, f, value, sizeof(value));
        }

        if (modbusmq_channel_publish_decide(channel, f, value, millitime()) == 0)
        {
            modbusmq_logf(LOG_DEBUG, "%s=%s suppressed (unchanged or too soon)\n", channel->topic, value);
            continue;
        }

#if MQTT_ENABLED
        if (GI.has_mqtt && GI.mqtt_connected)
        {
            //
            // A channel may override the config-wide default, which is how a
            // fast-moving reading avoids leaving a stale retained value behind
            // while the slow ones keep theirs.
            //
            int
                retain = channel->has_retain ? channel->retain : modbusmq_config->mqtt_retain;
            int
                qos    = channel->has_qos    ? channel->qos    : modbusmq_config->mqtt_qos;

            int
                rc = mosquitto_publish(GI.mosq, NULL, channel->topic, strlen(value), value, qos, retain);
            if (rc == MOSQ_ERR_NO_CONN || rc == MOSQ_ERR_CONN_LOST)
            {
                modbusmq_logf(LOG_INFO, "MQTT: lost connection while publishing, will reconnect\n");
                GI.mqtt_connected = 0;
            }
            else if (rc != MOSQ_ERR_SUCCESS)
            {
                fprintf(stderr, "Unable to publish to MQTT %s=%s (rc=%d)\n", channel->topic, value, rc);
            }
        }
#endif
        modbusmq_logf(LOG_INFO, "%s=%s\n", channel->topic, value);
    }
    fflush(stdout);
}

#if MQTT_ENABLED
//////////////////////////////////////////////////////////////////////////////
//
// Map an MQTT payload onto the value a write entry expects.
//
// Two shapes are accepted. With on_value/off_value configured the payload is a
// command: the usual textual spellings are folded onto 1/0 first, so a broker
// publishing "ON" works as well as one publishing "1", and the result selects
// the configured on or off value. Without them the payload is a number in
// engineering units, to be scaled by modbusmq_write_encode().
//
// @return 0 on success with *out set, < 0 when the payload is not usable
//
static int
mqtt_payload_value(const modbusmq_write_t *write, const char *payload, double *out,
                   char *why, int nwhy)
{
    const char
        *name = write->name ? write->name : write->topic;

    while (*payload == ' ' || *payload == '\t')
    {
        payload++;
    }

    if (!*payload)
    {
        snprintf(why, nwhy, "empty payload");
        goto fail;
    }

    if (write->has_on_value)
    {
        int
            on = -1;

        if (strcasecmp(payload, "on")   == 0 ||
            strcasecmp(payload, "true") == 0)
        {
            on = 1;
        }
        else if (strcasecmp(payload, "off")   == 0 ||
                 strcasecmp(payload, "false") == 0)
        {
            on = 0;
        }
        else
        {
            char
                *end = NULL;
            double
                d = strtod(payload, &end);

            if (end == payload || *end)
            {
                snprintf(why, nwhy, "payload \"%s\" is not a command", payload);
                goto fail;
            }
            //
            // Compare against the configured values rather than treating any
            // non-zero as on: a device whose off_value is 2 would otherwise be
            // switched on by the very value meant to switch it off.
            //
            if ((int)d == write->on_value)
            {
                on = 1;
            }
            else if ((int)d == write->off_value)
            {
                on = 0;
            }
            else
            {
                snprintf(why, nwhy, "payload \"%s\" is neither on_value %d nor off_value %d",
                         payload, write->on_value, write->off_value);
                goto fail;
            }
        }

        *out = on ? write->on_value : write->off_value;
        return 0;
    }

    char
        *end = NULL;
    double
        d = strtod(payload, &end);

    if (end == payload)
    {
        snprintf(why, nwhy, "payload \"%s\" is not a number", payload);
        goto fail;
    }
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')
    {
        end++;
    }
    if (*end)
    {
        snprintf(why, nwhy, "payload \"%s\" has trailing junk", payload);
        goto fail;
    }

    *out = d;
    return 0;

fail:
    modbusmq_logf(LOG_ERROR, "write %s: %s. action: skip write\n", name, why);
    return -1;
}

//////////////////////////////////////////////////////////////////////////////
//
// Split a block write's payload into one value per channel.
//
// "63,1,230", "63 1 230" and "[63, 1, 230]" are the same list: a JSON array of
// numbers is this with brackets on, and refusing it over punctuation would
// help nobody.
//
// An empty payload, or the word "default", asks for the values the config
// already carries. That is the commissioning case the block write exists for:
// the settings live in the file, reviewed and in version control, and
// publishing anything at all to the topic sends them.
//
// @return number of values parsed, 0 for the trigger form, < 0 when unusable
//
static int
mqtt_payload_block(const modbusmq_write_t *write, const char *payload, double *values, int nmax,
                   char *why, int nwhy)
{
    const char
        *name = write->name ? write->name : write->topic;
    const char
        *p = payload;
    int
        n = 0;

    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
    {
        p++;
    }

    if (!*p || strcasecmp(p, "default") == 0 || strcasecmp(p, "defaults") == 0)
    {
        if (!write->has_defaults)
        {
            snprintf(why, nwhy, "no value list, and not every channel has a default to fall back on");
            goto fail;
        }
        return 0;
    }

    if (*p == '[')
    {
        p++;
    }

    while (*p)
    {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ',')
        {
            p++;
        }
        if (!*p || *p == ']')
        {
            break;
        }

        if (n >= nmax)
        {
            snprintf(why, nwhy, "more values than the %d this block holds", nmax);
            goto fail;
        }

        char
            *end = NULL;
        double
            d = strtod(p, &end);

        if (end == p)
        {
            snprintf(why, nwhy, "payload \"%s\" is not a list of numbers", payload);
            goto fail;
        }

        values[n++] = d;
        p = end;
    }

    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ']')
    {
        p++;
    }
    if (*p)
    {
        snprintf(why, nwhy, "payload \"%s\" has trailing junk", payload);
        goto fail;
    }

    //
    // A short list is the dangerous one: it would leave the rest of a block
    // that gets written whole filled with whatever the caller did not say.
    //
    if (n != write->channel_max)
    {
        snprintf(why, nwhy, "%d value(s) for a block of %d channels", n, write->channel_max);
        goto fail;
    }

    return n;

fail:
    modbusmq_logf(LOG_ERROR, "write %s: %s. action: skip write\n", name, why);
    return -1;
}

//////////////////////////////////////////////////////////////////////////////
//
// Queue the Modbus write for one write entry.
//
// The request goes on the same queue as every poll, so it is serialised with
// them and needs no locking, and nothing here blocks waiting for the echo. A
// write with ack set is remembered in the pending table on the way out and
// answered on MQTT when modbusmq_message_callback() or
// modbusmq_error_callback() reports what became of it.
//
// A write that never gets as far as the queue is nacked here instead, since
// there will be no request id for either callback to report it under.
//
// @param write_index: which config entry, for the ack
// @param values     : one value per channel for a block, or one value
// @param nvalues    : how many, or < 0 for a block's configured defaults
//
// @return 0 when queued, < 0 when it was not
//
static int
modbus_write_post(struct modbusmq_context_t *context, const modbusmq_write_t *write, int write_index,
                  const double *values, int nvalues)
{
    const char
        *name = write->name ? write->name : write->topic;
    char
        written[96];

    if (!GI.modbus_connected)
    {
        modbusmq_logf(LOG_ERROR, "write %s: device is not connected. action: skip write\n", name);
        mqtt_write_nack(write, 0, "offline", "not connected to the device", 0);
        return -1;
    }

    modbusmq_msg_t
        msg;

    memset(&msg, 0, sizeof(msg));
    written[0] = 0;
    modbusmq_set_slave(context, write->slave);

    if (write->channel_max > 0)
    {
        //
        // A block. Function 16 carries at most 123 registers and the config
        // validator has already held the block to that, so the array below is
        // the largest one that can ever be asked for.
        //
        uint16_t
            regs[123];
        int
            nregs = modbusmq_write_encode_block(context, write, values, nvalues, regs, (int)(sizeof(regs)/sizeof(regs[0])));

        if (nregs < 0)
        {
            mqtt_write_nack(write, 0, "encode", "a value does not fit its channel format", 0);
            return -1; // already logged, with the reason
        }

        modbusmq_frame_write_registers(context, &msg.frame[0], write->address, nregs, regs);

        //
        // What the ack will report. Built from the same values that went into
        // the frame, in channel order, so a trigger write says what it sent
        // rather than only that it sent something.
        //
        int
            pos = 0;

        for(int c = 0; c < write->channel_max; ++c)
        {
            double
                v = (nvalues < 0) ? write->channels[c].value : values[c];

            pos += snprintf(written + pos, sizeof(written) - pos, "%s%.6g", pos ? "," : "", v);
            if (pos >= (int)sizeof(written))
            {
                pos = (int)sizeof(written) - 1;
                break;
            }
        }

        modbusmq_logf(LOG_INFO, "write %s: slave %d reg 0x%04X..0x%04X = %s%s\n",
                      name, write->slave, write->address, write->address + nregs - 1,
                      written, (nvalues < 0) ? " (defaults)" : "");
    }
    else if (write->function == modbusmq_write_function_coil)
    {
        double
            value = values[0];
        int
            on = (value != 0);

        //
        // A coil carries one bit, so on_value/off_value only decide which way
        // round it goes; the value itself never reaches the wire.
        //
        if (write->has_on_value)
        {
            on = ((int)value == write->on_value);
        }

        modbusmq_frame_write_coil_bit(context, &msg.frame[0], write->address, on);
        snprintf(written, sizeof(written), "%d", on);
        modbusmq_logf(LOG_INFO, "write %s: slave %d coil 0x%04X = %d\n", name, write->slave, write->address, on);
    }
    else
    {
        double
            value = values[0];
        uint16_t
            regs[2] = {0};
        int
            nregs = modbusmq_write_encode(context, write, value, regs);

        if (nregs < 0)
        {
            mqtt_write_nack(write, 0, "encode", "the value does not fit the configured format", 0);
            return -1; // already logged, with the reason
        }

        snprintf(written, sizeof(written), "%.6g", value);

        if (nregs == 1)
        {
            modbusmq_frame_write_register(context, &msg.frame[0], write->address, regs[0]);
            modbusmq_logf(LOG_INFO, "write %s: slave %d reg 0x%04X = %.6g (raw 0x%04X)\n",
                          name, write->slave, write->address, value, regs[0]);
        }
        else
        {
            modbusmq_frame_write_registers(context, &msg.frame[0], write->address, nregs, regs);
            modbusmq_logf(LOG_INFO, "write %s: slave %d reg 0x%04X = %.6g (raw 0x%04X%04X)\n",
                          name, write->slave, write->address, value, regs[0], regs[1]);
        }
    }

    int
        rc = modbusmq_post(context, &msg);
    if (rc != 0)
    {
        modbusmq_logf(LOG_ERROR, "write %s: unable to queue the request, rc=%d\n", name, rc);
        mqtt_write_nack(write, 0, "queue", "the request could not be queued", 0);
        return -1;
    }

    //
    // modbusmq_post() stamps the request id into our copy before queueing it,
    // so this is the id the callbacks will report the answer under.
    //
    if (write->ack)
    {
        pending_write_add(msg.req_id, write_index, written);
    }

    return 0;
}

//////////////////////////////////////////////////////////////////////////////
//
// mosquitto message callback: an incoming publish on a write topic
//
// More than one write entry may share a topic, so every match is acted on
// rather than only the first -- and each one answers for itself, which is why
// the ack topic hangs off the entry and not off the topic published to.
//
static void
mqtt_message_callback(struct mosquitto *mosq, void *userdata, const struct mosquitto_message *message)
{
    (void)mosq;
    struct modbusmq_context_t
        *context = (struct modbusmq_context_t *)userdata;
    modbusmq_config_t
        *modbusmq_config = modbusmq_config_get();

    if (!message || !message->topic)
    {
        return;
    }

    //
    // The payload is not NUL-terminated on the wire and is attacker-adjacent
    // input, so copy it into a bounded buffer before treating it as a string.
    //
    // a block of 123 registers spelled out as a list of numbers is the
    // largest thing any write entry can be asked to take
    char
        payload[1024];
    int
        len = message->payloadlen;

    if (len < 0 || len >= (int)sizeof(payload))
    {
        modbusmq_logf(LOG_ERROR, "MQTT: %s: payload of %d bytes is too long. action: ignore\n", message->topic, message->payloadlen);
        return;
    }
    if (len > 0)
    {
        memcpy(payload, message->payload, len);
    }
    payload[len] = 0;

    int
        matched = 0;

    for(int w = 0; w < modbusmq_config->write_max; ++w)
    {
        modbusmq_write_t
            *write = &modbusmq_config->writes[w];

        if (!write->topic || strcmp(write->topic, message->topic) != 0)
        {
            continue;
        }

        matched++;

        char
            why[160] = {0};

        if (write->channel_max > 0)
        {
            double
                values[123];
            int
                nvalues = mqtt_payload_block(write, payload, values,
                                             (int)(sizeof(values)/sizeof(values[0])), why, sizeof(why));

            if (nvalues < 0)
            {
                mqtt_write_nack(write, 0, "payload", why, 0);
                continue; // already logged
            }

            //
            // 0 values is the trigger form. modbusmq_write_encode_block()
            // spells that as a negative count, since 0 there would be a list
            // that happens to be empty.
            //
            modbus_write_post(context, write, w, values, nvalues > 0 ? nvalues : -1);
            continue;
        }

        double
            value = 0;

        if (mqtt_payload_value(write, payload, &value, why, sizeof(why)) != 0)
        {
            mqtt_write_nack(write, 0, "payload", why, 0);
            continue; // already logged
        }

        modbus_write_post(context, write, w, &value, 1);
    }

    if (!matched)
    {
        modbusmq_logf(LOG_ERROR, "MQTT: %s: no write entry for this topic. action: ignore\n", message->topic);
    }
}

//////////////////////////////////////////////////////////////////////////////
//
// Subscribe to every write topic.
//
// Kept separate from the initial connect because the broker forgets our
// subscriptions on every reconnect, so this has to be callable again.
//
// @return 0 when every subscribe succeeded, < 0 otherwise
//
static int
mqtt_subscribe_writes(void)
{
    modbusmq_config_t
        *modbusmq_config = modbusmq_config_get();
    int
        rc_all = 0;

    for(int w = 0; w < modbusmq_config->write_max; ++w)
    {
        modbusmq_write_t
            *write = &modbusmq_config->writes[w];

        if (!write->topic)
        {
            continue;
        }

        //
        // QoS 1: a lost setpoint is not something the publisher finds out
        // about, and the cost of the occasional duplicate is one redundant
        // write of a value we were asked for anyway.
        //
        int
            rc = mosquitto_subscribe(GI.mosq, NULL, write->topic, 1);
        if (rc != MOSQ_ERR_SUCCESS)
        {
            modbusmq_logf(LOG_ERROR, "MQTT: unable to subscribe to %s (rc=%d)\n", write->topic, rc);
            rc_all = -1;
            continue;
        }

        modbusmq_logf(LOG_INFO, "MQTT: subscribed to %s -> %s%s%s\n",
                      write->topic, write->name ? write->name : "write",
                      write->ack_topic ? ", ack on " : "",
                      write->ack_topic ? write->ack_topic : "");
    }

    return rc_all;
}
#endif // MQTT_ENABLED

//
// print help
//
void
print_help(int argc, char **argv, int print_long)
{
    (void)argc;
    printf("Usage: %s -c config [-e key=value ...] [-v]\n", argv[0]);

    if (print_long)
    {
        printf("-c config    : read from config-file\n");
        printf("-e key=value : override one config key, using the same key names the\n");
        printf("               config file uses. Repeatable. An override replaces the\n");
        printf("               value on a line the file already has; it does not add a\n");
        printf("               key the file is missing.\n");
        printf("               -e modbusmq.connect=rtu:///dev/ttyUSB0:9600:1:8:N\n");
        printf("                   run a tcp config over rtu\n");
        printf("               -e input.max=9\n");
        printf("                   read every input the file defines, not just the\n");
        printf("                   first input.max of them\n");
        printf("-v           : increase verbosity\n");
    }
    
}

//////////////////////////////////////////////////////////////////////////////
// 
// parse arguments
//
int
parse_argv(int argc, char **argv)
{
    for(int a = 1; a < argc; ++a)
    {
        if (strcmp(argv[a], "-h") == 0 ||
            strcmp(argv[a], "--help") == 0)
        {
            print_help(argc, argv, 1);
            return 1;
        }
        else if (strstr(argv[a], "--v")) // --version
        {
            printf("modbusmq version: %s\n", version);
            return 1;
        }
        else if (strcmp(argv[a], "-c") == 0 ||
                 strcmp(argv[a], "--config") == 0)
        {
            a++;
            if (a >= argc)
            {
                printf("-c requires a config filename\n");
                print_help(argc, argv, 0);
                return -1;
            }
            if (strlen(argv[a]) >= sizeof(GI.config_filename))
            {
                printf("-c config filename too long (max %zu chars)\n", sizeof(GI.config_filename) - 1);
                return -1;
            }
            strcpy(GI.config_filename, argv[a]);
            GI.has_config = 1;
        }
        else if (strcmp(argv[a], "-e") == 0)
        {
            a++;
            if (a >= argc)
            {
                printf("-e requires key=value\n");
                print_help(argc, argv, 0);
                return -1;
            }
            if (!strchr(argv[a], '=') || argv[a][0] == '=')
            {
                printf("-e %s: expected key=value\n", argv[a]);
                return -1;
            }
            if (GI.noverrides >= MAX_CONFIG_OVERRIDES)
            {
                printf("-e: too many overrides (max %d)\n", MAX_CONFIG_OVERRIDES);
                return -1;
            }
            GI.overrides[GI.noverrides++] = argv[a];
        }
        else if (strcmp(argv[a], "-v") == 0)
        {
            GI.verbose++;
        }
    }

    if (!GI.has_config)
    {
        printf("ERROR: Missing config\n");
        print_help(argc, argv, 0);
        return -2;
    }
    
    return 0;
    
}
 
 
 
// set nonblocking mode on socket
int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

//////////////////////////////////////////////////////////////////////////////
//
// Attempt to reconnect to MQTT broker.
// Returns 1 if connected, 0 if not.
//
#if MQTT_ENABLED
int
mqtt_try_reconnect(modbusmq_connect_t *connect)
{
    millitime_t now = millitime();
 
    if (now < GI.mqtt_reconnect_at_ms)
    {
        return 0; // not time yet
    }
 
    GI.mqtt_reconnect_at_ms = now + MQTT_RECONNECT_INTERVAL_MS;
 
    modbusmq_logf(LOG_INFO, "MQTT: attempting reconnect to %s:%d\n", connect->device, connect->port);
 
    int rc = mosquitto_reconnect(GI.mosq);
    if (rc == MOSQ_ERR_SUCCESS)
    {
        modbusmq_logf(LOG_INFO, "MQTT: reconnected successfully\n");
        GI.mqtt_connected = 1;
        //
        // This is a clean session, so the broker kept none of our
        // subscriptions across the drop. Without this the write topics go
        // quiet after the first blip and nothing says so.
        //
        mqtt_subscribe_writes();
        //
        // Also a clean session for retained state: any channel gated by
        // min_change would otherwise stay silent until its value happens to
        // move again, since it still believes the broker holds its last
        // published reading. Treat every channel as unpublished so the next
        // poll of each one goes out fresh.
        //
        modbusmq_config_reset_publish_state(modbusmq_config_get());
        return 1;
    }
 
    modbusmq_logf(LOG_INFO, "MQTT: reconnect failed (rc=%d), will retry in %d ms\n", rc, MQTT_RECONNECT_INTERVAL_MS);
    return 0;
}
#endif
 
//////////////////////////////////////////////////////////////////////////////
// 
// main
int
main(int argc, char **argv)
{
    int
        rc;

    memset(&GI, 0, sizeof(GI));

    rc = parse_argv(argc, argv);
    if (rc != 0)
    {
        if (rc < 0)
        {
            return -1;
        }
        return 0;
    }

    //
    // Set verbosity
    //
    if (GI.verbose)
    {
        modbusmq_set_debug(GI.verbose);
    }
    

    
    if (GI.has_config)
    {
        modbusmq_config_set_overrides(GI.overrides, GI.noverrides);

        rc = modbusmq_config_parse(GI.config_filename);
        if (rc != 0)
        {
            printf("Unable to parse config-file.\n");
            return -1;
        }

        //
        // An override that matched no key did nothing at all. Say so and keep
        // going: it is worth noticing, but it is not this program's business
        // to decide that a config key someone named is a mistake.
        //
        modbusmq_config_override_unmatched();
    }

    modbusmq_config_t
        *modbusmq_config = modbusmq_config_get();

    //
    // if the config does not have mqtt, then simply ignore it
    //
    if (modbusmq_config->mqtt_connect && strlen(modbusmq_config->mqtt_connect) > 0)
    {
        GI.has_mqtt = 1;
    }

    //
    // Writes arrive over MQTT, so without a broker they can never fire. Say so
    // rather than starting up looking healthy.
    //
    if (modbusmq_config->write_max > 0 && !GI.has_mqtt)
    {
        fprintf(stderr, "WARNING: %d write entries configured but no mqtt.connect — nothing can trigger them\n",
                modbusmq_config->write_max);
    }

    //
    // The trigger form of a block write needs every channel to carry a value.
    // Worth saying at startup: a commissioning block that can only be driven
    // by a full value list is a config someone probably meant to finish, and
    // finding that out from a nack in the field is finding it out late.
    //
    for(int w = 0; w < modbusmq_config->write_max; ++w)
    {
        modbusmq_write_t
            *write = &modbusmq_config->writes[w];

        if (write->channel_max <= 0 || write->has_defaults)
        {
            continue;
        }

        fprintf(stderr, "NOTE: write.%d (%s) is a %d-register block with no complete set of defaults — "
                        "it takes a list of %d values, an empty payload will not trigger it\n",
                w+1, write->name ? write->name : "?", write->naddress, write->channel_max);
    }
#if !MQTT_ENABLED
    if (modbusmq_config->write_max > 0)
    {
        fprintf(stderr, "WARNING: %d write entries configured but this build has MQTT disabled — rebuild with --enable-mqtt\n",
                modbusmq_config->write_max);
    }
#endif

    
    //
    // parse mqtt connect-string
    //
    modbusmq_connect_t
        connect;

    if (GI.has_mqtt)
    {
        rc = modbusmq_parse_connect_string(modbusmq_config->mqtt_connect, &connect);
        if (rc != 0)
        {
            fprintf(stderr, "Unable to parse mqtt connect string: %s\n", modbusmq_config->mqtt_connect);
            exit(2);
        }
    }
    
    //
    // connect to device
    //
    struct modbusmq_context_t
        *context = NULL;

    {
        modbusmq_connect_t
            connect;

        rc = modbusmq_parse_connect_string(modbusmq_config->modbusmq_connect, &connect);
        if (rc != 0)
        {
            fprintf(stderr, "Unable to parse connect string\n");
            return -2;
        }

        if (connect.connect_type == MODBUSMQ_CONNECT_TCP)
        {
            context = modbusmq_tcp_context(modbusmq_config->modbusmq_connect);
        }
        else if (connect.connect_type == MODBUSMQ_CONNECT_RTU)
        {
            context = modbusmq_rtu_context(connect.device, connect.baudrate, connect.parity, connect.databits, connect.stopbit);

        }
        else
        {
            assert(0);
        }

        if (!context)
        {
            fprintf(stderr, "Unable to connect to device: %s\n", connect.device);
            assert(context);
            return -2;
        }
    }

    modbusmq_set_config(context, modbusmq_config);
    
    //
    // RTU ONLY
    //
    // set delay between request-frames
    //
    if (modbusmq_config->modbusmq_rts_delay_us > 0)
    {
        modbusmq_rtu_rts_delay(context, modbusmq_config->modbusmq_rts_delay_us);
    }
    // set max delay to wait for a frame
    if (modbusmq_config->modbusmq_frame_timeout_ms > 0)
    {
        modbusmq_frame_timeout(context, modbusmq_config->modbusmq_frame_timeout_ms);
    }
    

    //
    // Connect to device
    //
    rc = modbusmq_connect(context);
    if (rc < 0)
    {
        fprintf(stderr, "Unable to connect to device, will retry every %d ms\n", MODBUS_RECONNECT_INTERVAL_MS);
        GI.modbus_connected = 0;
        GI.modbus_reconnect_at_ms = millitime() + MODBUS_RECONNECT_INTERVAL_MS;
    }
    else
    {
        GI.modbus_connected = 1;
    }
    //
    // set up callbacks
    //
    modbusmq_set_message_callback(     context, &modbusmq_message_callback);
    modbusmq_set_subscription_callback(context, &modbusmq_subscription_callback);
    modbusmq_set_error_callback(       context, &modbusmq_error_callback);

    //
    // mosquitto connection
    //
#if MQTT_ENABLED
    if (GI.has_mqtt)
    {
        GI.mosq = mosquitto_new(modbusmq_config->mqtt_name, true, context);
        assert(GI.mosq);

        if (modbusmq_config->write_max > 0)
        {
            mosquitto_message_callback_set(GI.mosq, &mqtt_message_callback);
        }

        rc = mosquitto_connect(GI.mosq, connect.device, connect.port, 3600);
        if (rc != 0)
        {
            fprintf(stderr, "Unable to connect to MQTT server, hostname=%s, port=%d. Will retry every %d ms\n",
                    connect.device, connect.port, MQTT_RECONNECT_INTERVAL_MS);
            GI.mqtt_connected = 0;
            GI.mqtt_reconnect_at_ms = millitime() + MQTT_RECONNECT_INTERVAL_MS;
        }
        else
        {
            GI.mqtt_connected = 1;
            mqtt_subscribe_writes();
            //
            // A no-op here — every channel starts unpublished anyway — but
            // kept for symmetry with mqtt_try_reconnect() so both places the
            // connection is (re)established agree on what "just connected"
            // means.
            //
            modbusmq_config_reset_publish_state(modbusmq_config_get());
        }
    }
#else
    GI.has_mqtt = 0;
#endif
    

    //
    // set up subscription based on config-file
    //
    modbusmq_msg_t
        msg;
    
    for(int i = 0; i < modbusmq_config->input_max; ++i)
    {
        memset(&msg, 0, sizeof(msg));
        
        modbusmq_input_t
            *input = &modbusmq_config->inputs[i];

        modbusmq_set_slave(context, input->slave);
        
        switch(input->type)
        {
        case modbusmq_type_holding_register:
            modbusmq_frame_read_holding_registers(context, &msg.frame[0], input->address + input->address_offset, input->naddress);
            break;
        case modbusmq_type_input_register:
            modbusmq_frame_read_input_registers(context, &msg.frame[0], input->address + input->address_offset, input->naddress);
            break;
        case modbusmq_type_coil:
            //
            // naddress is a coil count here, not a register count — the device
            // answers with them packed eight to a byte.
            //
            modbusmq_frame_read_coil_bits(context, &msg.frame[0], input->address + input->address_offset, input->naddress);
            break;
        case modbusmq_type_discrete_input:
            modbusmq_frame_read_input_bits(context, &msg.frame[0], input->address + input->address_offset, input->naddress);
            break;
        default:
            fprintf(stderr, "Unknown input-type for slave=%d: input_mode=%d\n", input->slave, input->type);
            break;
        }
        
        rc = modbusmq_subscribe(context, &msg, input->interval);
        if (rc != 0)
        {
            fprintf(stderr, "modbusmq_subscribe: failed, rc=%d\n", rc);
        }
    }
    


    modbusmq_timer_debug_print(context);

    //
    // main loop
    //
    {
        struct pollfd pollfds[10];

        while(1)
        {
            millitime_t
                time_now = millitime();

            int
                nfds = 0;
            
            millitime_t
                millisleep = 1000; // 1 second default wait time

            memset(pollfds, 0, sizeof(pollfds));

            int
                modbus_pollfd_idx = -1;

            if (GI.modbus_connected)
            {
                int
                    modbusmq_fd = modbusmq_loop_prepare(context, &millisleep, &pollfds[nfds].events);

                if (modbusmq_fd < 0)
                {
                    // fd was live a moment ago (modbus_connected is only set
                    // once modbusmq_connect() succeeds) but is gone now —
                    // treat this the same as a failed poll/read below and
                    // fall back into the reconnect path instead of polling
                    // a stale descriptor.
                    modbusmq_logf(LOG_ERROR, "Modbus: connection lost, will reconnect\n");
                    modbusmq_reset_queue(context);
                    GI.modbus_connected = 0;
                    GI.modbus_reconnect_at_ms = time_now + MODBUS_RECONNECT_INTERVAL_MS;
                }
                else
                {
                    pollfds[nfds].fd = modbusmq_fd;
                    pollfds[nfds].events |= POLLERR | POLLHUP;
                    modbus_pollfd_idx = nfds;
                    nfds++;
                }
            }
            else
            {
                // not connected — wake up in time for the reconnect timer
                // instead of sleeping for a full cycle
                millitime_t until_retry = (GI.modbus_reconnect_at_ms > time_now) ? (GI.modbus_reconnect_at_ms - time_now) : 0;
                if (until_retry < millisleep)
                {
                    millisleep = until_retry;
                }
            }

#if MQTT_ENABLED
            int
                mqtt_pollfd_idx = -1;

            if (GI.has_mqtt && GI.mqtt_connected)
            {
                int mosq_fd = mosquitto_socket(GI.mosq);
                if (mosq_fd >= 0)
                {
                    pollfds[nfds].fd = mosq_fd;
                    // Only ask for POLLOUT when mosquitto actually has
                    // pending output — an idle connected TCP socket is
                    // almost always writable, so requesting POLLOUT
                    // unconditionally turns this into a busy-spin.
                    pollfds[nfds].events = POLLIN | POLLERR | POLLHUP;
                    if (mosquitto_want_write(GI.mosq))
                    {
                        pollfds[nfds].events |= POLLOUT;
                    }
                    mqtt_pollfd_idx = nfds;
                    nfds++;
                }
            }
            else if (GI.has_mqtt && !GI.mqtt_connected)
            {
                millitime_t until_retry = (GI.mqtt_reconnect_at_ms > time_now) ? (GI.mqtt_reconnect_at_ms - time_now) : 0;
                if (until_retry < millisleep)
                {
                    millisleep = until_retry;
                }
            }
#endif

            rc = poll(pollfds, nfds, millisleep);

            if (rc < 0)
            {
                modbusmq_logf(LOG_ERROR, "poll failed: rc = %d, errno=%d, str=%s\n", rc, errno, strerror(errno));
                exit(2);
            }

            time_now = millitime();

            //
            // modbusmq: handle activity, or reconnect on a timer
            //
            if (GI.modbus_connected)
            {
                if (modbus_pollfd_idx >= 0 && (pollfds[modbus_pollfd_idx].revents & (POLLIN | POLLOUT | POLLERR | POLLHUP)))
                {
                    rc = modbusmq_loop_write_read(context, pollfds[modbus_pollfd_idx].revents);
                    if (rc == MODBUSMQ_ERR_PROTOCOL)
                    {
                        //
                        // A frame was rejected and the library has resynced the
                        // stream. The connection is fine — reconnecting here
                        // would throw away every other subscription's progress
                        // over one bad frame. Report it and keep polling.
                        //
                        modbusmq_logf(LOG_ERROR, "modbusmq_loop_write_read: frame rejected, stream resynced, continuing with next request\n");
                    }
                    else if (rc < 0)
                    {
                        modbusmq_logf(LOG_ERROR, "modbusmq_loop_write_read: error rc=%d, err=%s. Disconnecting, will reconnect\n", rc, strerror(errno));
                        modbusmq_close(context);
                        modbusmq_reset_queue(context);
                        GI.modbus_connected = 0;
                        GI.modbus_reconnect_at_ms = millitime() + MODBUS_RECONNECT_INTERVAL_MS;
                    }
                }
            }
            else if (time_now >= GI.modbus_reconnect_at_ms)
            {
                GI.modbus_reconnect_at_ms = time_now + MODBUS_RECONNECT_INTERVAL_MS;
                modbusmq_logf(LOG_INFO, "Modbus: attempting reconnect\n");
                if (modbusmq_connect(context) == 0)
                {
                    modbusmq_logf(LOG_INFO, "Modbus: reconnected successfully\n");
                    GI.modbus_connected = 1;
                }
                else
                {
                    modbusmq_logf(LOG_INFO, "Modbus: reconnect failed, will retry in %d ms\n", MODBUS_RECONNECT_INTERVAL_MS);
                }
            }

#if MQTT_ENABLED
            if (GI.has_mqtt)
            {
                if (!GI.mqtt_connected)
                {
                    // Not connected — try to reconnect on a timer
                    mqtt_try_reconnect(&connect);
                }
                else
                {
                    int mosq_fd = mosquitto_socket(GI.mosq);

                    if (mosq_fd < 0)
                    {
                        // Socket gone — mark as disconnected
                        modbusmq_logf(LOG_INFO, "MQTT: socket lost, will reconnect\n");
                        GI.mqtt_connected = 0;
                        GI.mqtt_reconnect_at_ms = millitime() + MQTT_RECONNECT_INTERVAL_MS;
                    }
                    else if (mqtt_pollfd_idx >= 0 && (pollfds[mqtt_pollfd_idx].revents & (POLLIN | POLLOUT)))
                    {
                        // process mosquitto read
                        rc = mosquitto_loop_read(GI.mosq, 1);
                        if (rc == MOSQ_ERR_CONN_LOST || rc == MOSQ_ERR_NO_CONN)
                        {
                            modbusmq_logf(LOG_INFO, "MQTT: connection lost, will reconnect\n");
                            GI.mqtt_connected = 0;
                            GI.mqtt_reconnect_at_ms = millitime() + MQTT_RECONNECT_INTERVAL_MS;
                        }
                        else
                        {
                            // process write
                            if (mosquitto_want_write(GI.mosq))
                            {
                                mosquitto_loop_write(GI.mosq, 1);
                            }
                            // process misc
                            mosquitto_loop_misc(GI.mosq);
                        }
                    }
                    else
                    {
                        // no fd activity this cycle — still run periodic
                        // housekeeping (keepalive etc.)
                        mosquitto_loop_misc(GI.mosq);
                    }
                }
            }
#endif
        }
    }
    
    //
    // free up resources
    //
#if MQTT_ENABLED
    mosquitto_disconnect(GI.mosq);
    mosquitto_destroy(GI.mosq);
#endif
    
    modbusmq_close(context);
    modbusmq_free(context);

    free(GI.pending);
}
