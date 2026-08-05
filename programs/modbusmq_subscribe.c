//////////////////////////////////////////////////////////////////////////////
// 
// bjornwennberg71@gmail.com
// 
// modbusmq_subscribe.c
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
    int
        function = modbusmq_frame_function(context, &msg->frame[1]);
    int
        nb       = modbusmq_frame_nbytes(context, &msg->frame[1]);
    uint8_t
        *data    = modbusmq_frame_data(context, &msg->frame[1]);
    char
        value[100];
    int
        value_len = 4;
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

                
        sprintf(value, "%.3f", f);

#if MQTT_ENABLED
        if (GI.has_mqtt && GI.mqtt_connected)
        {
            int
                rc = mosquitto_publish(GI.mosq, NULL, channel->topic, strlen(value), value, 0, true);
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
        char
            azLine[200] = {0};
        
        modbusmq_logf(LOG_INFO, "%s=%s %s\n", channel->topic, value, azLine);
        
    }
    fflush(stdout);
}

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
            printf("modbusmq_subscribe version: %s\n", version);
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

    //
    // mosquitto connection
    //
#if MQTT_ENABLED
    if (GI.has_mqtt)
    {
        GI.mosq = mosquitto_new(modbusmq_config->mqtt_name, true, NULL);
        assert(GI.mosq);
        
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
        case ModbusmqType_HoldingRegister:
            modbusmq_frame_read_holding_registers(context, &msg.frame[0], input->address + input->address_offset, input->naddress);
            break;
        case ModbusmqType_InputRegister:
            modbusmq_frame_read_input_registers(context, &msg.frame[0], input->address + input->address_offset, input->naddress);
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

                pollfds[nfds].fd = modbusmq_fd;
                pollfds[nfds].events |= POLLERR | POLLHUP;
                modbus_pollfd_idx = nfds;
                nfds++;
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
