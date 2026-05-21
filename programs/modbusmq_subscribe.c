//////////////////////////////////////////////////////////////////////////////
// 
// bjornwennberg71@gmail.com
// 
// modbusmq_subscribe.cpp
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

const char *version = "1.03";

//
// global parameters from settings / argument line 
//
typedef struct global_info
{
    char config_filename[300];
    int  has_config;
    int  has_mqtt;
    
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

        float
            f = modbusmq_read_channel(context, msg, input, channel);
        
                
        sprintf(value, "%.3f", f);

#if MQTT_ENABLED
        if (GI.has_mqtt)
        {
            int
                rc = mosquitto_publish(GI.mosq, NULL, channel->topic, strlen(value), value, 0, true);
            if (rc != 0)
            {
                fprintf(stderr, "Unable to publish to MQTT %s=%s\n", channel->topic, value);
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
            exit(1);
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
        return -1;
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
        fprintf(stderr, "Unable to connect to device\n");
        exit(2);
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
            fprintf(stderr, "Unable to connect to MQTT server, hostname=%s, port=%s. Action=Disable MQTT\n", connect.device, connect.device);
            GI.has_mqtt = 0;
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
                modbusmq_fd = modbusmq_loop_prepare(context, &millisleep, &pollfds[0].events),
                mosq_fd = 0;

            pollfds[0].fd = modbusmq_fd;
            pollfds[0].events |= POLLERR | POLLHUP;
            nfds++;

#if MQTT_ENABLED
            if (GI.has_mqtt)
            {
                mosq_fd   = mosquitto_socket(GI.mosq);

                pollfds[1].fd = modbusmq_fd;
                pollfds[1].events = POLLIN | POLLOUT | POLLERR | POLLHUP;
                nfds++;
            }
#endif
            
            
            rc = poll(pollfds, nfds, millisleep);

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
                    //
                    // modbusmq
                    //
                    rc = modbusmq_loop_write_read(context, pollfds[0].revents);
                    if (rc < 0)
                    {
                        modbusmq_logf(LOG_ERROR, "modbusmq_loop_write_read: error rc=%d, err=%s\n", rc, strerror(errno));
                        exit(2);
                    }
                }

#if MQTT_ENABLED
                if (GI.has_mqtt && pollfds[1].revents & (POLLIN | POLLOUT))
                {
                    
                    //
                    // mosquitto
                    //
                    // process mosquitto read
                    rc = mosquitto_loop_read(GI.mosq, 1);
                    if (rc == MOSQ_ERR_CONN_LOST)
                    {
                        /* We've been disconnected from the server */
                        mosquitto_reconnect(GI.mosq);
                        mosq_fd = mosquitto_socket(GI.mosq);
                    }
                    
                    // process write
                    if (mosquitto_want_write(GI.mosq))
                    {
                        mosquitto_loop_write(GI.mosq, 1);
                    }
                    // process misc
                    mosquitto_loop_misc(GI.mosq);
                }
#endif
            }
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
