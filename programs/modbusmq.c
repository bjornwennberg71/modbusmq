//////////////////////////////////////////////////////////////////////////////
// 
// bjornwennberg71@gmail.com
// 
// modbusmq_bridge.c
// 

// INCLUDES //////////////////////////////////////////////////////////////////
#include "modbusmq.h"
#include "modbusmq_log.h"
#include "modbusmq_config.h"

#if MQTT_ENABLED
#include <mosquitto.h>
#endif

#include <stdint.h>
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
typedef struct global_info
{
    char config_filename[300];
    int  has_config;
    int  has_mqtt;
    int  mqtt_connected;              // 1 = connected to broker, 0 = disconnected
    millitime_t mqtt_reconnect_at_ms; // when to attempt next reconnect

    int  modbus_connected;              // 1 = connected to device, 0 = disconnected
    millitime_t modbus_reconnect_at_ms; // when to attempt next reconnect

    int verbose;
    struct mosquitto *mosq;
} global_info;
 

 
static global_info GI;

//////////////////////////////////////////////////////////////////////////////
// 
// 
void
modbusmq_message_callback(struct modbusmq_context_t *context, modbusmq_msg_t *msg)
{
    modbusmq_logf(LOG_DEBUG, "modbusmq_message_callback\n");
    modbusmq_frame_debug(context, &msg->frame[1]);

    modbusmq_logf(LOG_DEBUG, "transaction_id = %d\n", modbusmq_frame_transaction_id(context, &msg->frame[0]));
           
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
    }

    modbusmq_logf(LOG_ERROR, "slave %d (addr 0x%04X, req %u) %s\n",
                  modbusmq_frame_slave(context, &msg->frame[0]),
                  modbusmq_frame_addr(context, &msg->frame[0]),
                  msg->req_id,
                  reason);
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
    modbusmq_config_t
        *modbusmq_config = modbusmq_config_get();
    char
        value[100];

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
            f = modbusmq_read_channel(context, msg, input, channel);

        //
        // min_change/min_interval/max_interval decide whether this reading is
        // worth sending at all — a status word flooding the broker unchanged
        // every poll helps nobody. The formatted value is still logged either
        // way, at debug level, so -v shows what a suppressed channel would
        // have published.
        //
        modbusmq_channel_format_value(input, channel, f, value, sizeof(value));

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
mqtt_payload_value(const modbusmq_write_t *write, const char *payload, double *out)
{
    const char
        *name = write->name ? write->name : write->topic;

    while (*payload == ' ' || *payload == '\t')
    {
        payload++;
    }

    if (!*payload)
    {
        modbusmq_logf(LOG_ERROR, "write %s: empty payload. action: skip write\n", name);
        return -1;
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
                modbusmq_logf(LOG_ERROR, "write %s: payload \"%s\" is not a command. action: skip write\n", name, payload);
                return -1;
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
                modbusmq_logf(LOG_ERROR, "write %s: payload \"%s\" is neither on_value %d nor off_value %d. action: skip write\n",
                              name, payload, write->on_value, write->off_value);
                return -1;
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
        modbusmq_logf(LOG_ERROR, "write %s: payload \"%s\" is not a number. action: skip write\n", name, payload);
        return -1;
    }
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')
    {
        end++;
    }
    if (*end)
    {
        modbusmq_logf(LOG_ERROR, "write %s: payload \"%s\" has trailing junk. action: skip write\n", name, payload);
        return -1;
    }

    *out = d;
    return 0;
}

//////////////////////////////////////////////////////////////////////////////
//
// Queue the Modbus write for one write entry.
//
// Fire and forget: the request goes on the same queue as every poll, so it is
// serialised with them and needs no locking, and nothing here waits for the
// echo. A write that fails reports through modbusmq_error_callback() like any
// other request.
//
// @return 0 when queued, < 0 when it was not
//
static int
modbus_write_post(struct modbusmq_context_t *context, const modbusmq_write_t *write, double value)
{
    const char
        *name = write->name ? write->name : write->topic;

    if (!GI.modbus_connected)
    {
        modbusmq_logf(LOG_ERROR, "write %s: device is not connected. action: skip write\n", name);
        return -1;
    }

    modbusmq_msg_t
        msg;

    memset(&msg, 0, sizeof(msg));
    modbusmq_set_slave(context, write->slave);

    if (write->function == modbusmq_write_function_coil)
    {
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
        modbusmq_logf(LOG_INFO, "write %s: slave %d coil 0x%04X = %d\n", name, write->slave, write->address, on);
    }
    else
    {
        uint16_t
            regs[2] = {0};
        int
            nregs = modbusmq_write_encode(context, write, value, regs);

        if (nregs < 0)
        {
            return -1; // already logged, with the reason
        }

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
        return -1;
    }

    return 0;
}

//////////////////////////////////////////////////////////////////////////////
//
// mosquitto message callback: an incoming publish on a write topic
//
// More than one write entry may share a topic, so every match is acted on
// rather than only the first.
//
static void
mqtt_message_callback(struct mosquitto *mosq, void *userdata, const struct mosquitto_message *message)
{
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
    char
        payload[64];
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

        double
            value = 0;

        if (mqtt_payload_value(write, payload, &value) != 0)
        {
            continue; // already logged
        }

        modbus_write_post(context, write, value);
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

        modbusmq_logf(LOG_INFO, "MQTT: subscribed to %s -> %s\n",
                      write->topic, write->name ? write->name : "write");
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
    printf("Usage: %s -c config\n", argv[0]);

    if (print_long)
    {
        printf("-c config: read from config-file\n");
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
            printf("modbusmq_bridge version: %s\n", version);
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
    modbusmq_config_t
        *modbusmq_config = modbusmq_config_get();
 
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
        rc = modbusmq_config_parse(GI.config_filename);
        if (rc != 0)
        {
            printf("Unable to parse config-file.\n");
            return -1;
        }
            
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
    //modbusmq_set_message_callback(     context, &modbusmq_message_callback);
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
        
        millitime_t
            start_time_ms = millitime();
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
}
