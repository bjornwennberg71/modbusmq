//////////////////////////////////////////////////////////////////////////////
// 
// bjornwennberg71@gmail.com
// 
// modbusmq_query.c
// 

// INCLUDES //////////////////////////////////////////////////////////////////
#include "modbusmq.h"
#include "modbusmq_config.h"

#include <stdio.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>


const char *version = MODBUSMQ_VERSION_STRING;
    
//
// global parameters from settings / argument line 
//
typedef struct global_info
{
    const char *connect;

    int         slave;
    int         input;
    int         addr;
    int         naddr;
    int         output_size; // 2 or 4 bytes
    int         verbose;
} global_info;

static global_info GI;

//
// print help
//
void
print_help(int argc, char **argv, int print_long)
{
    printf("Usage  : %s connect-string slave-id register_type  addr naddr byte-size \n", argv[0]);
    printf("example: %s <rtu:///dev/ttyUSB3:9600:1:8:N | tcp://hostname:port> slave_id <%s | %s>  addr naddr <-4|-2> \n", argv[0], MODBUSMQ_TYPE_INPUT_REGISTER, MODBUSMQ_TYPE_HOLDING_REGISTER);

    if (print_long)
    {
        printf("rtu      :              rtu://[device]:[baudrate]:[stopbit]:[databits]:[parity]  RTU connect string \n");
        printf("tcp      :              tcp://hostname:port TCP connect string \n");
        printf("slave-id :              slave-id to communicate to \n");
        printf("read_input_registers  : Read one or more input registers from address \n");
        printf("read_holding_registers: Read one or more holding registers from address \n");
        printf("addr                  : Read from this address (can be numberic or hex)\n");
        printf("naddr                 : Read this amount of addresses (can be numberic or hex)\n");
        printf("-4       :              4 bytes values \n");
        printf("-2       :              2 bytes values \n");
        printf("-v       :              increase verbosity\n");
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
        else if (strstr(argv[a], "tcp") || strstr(argv[a], "rtu"))
        {
            GI.connect = argv[a];
        }
        else if (strstr(argv[a], "--v")) // --version
        {
            printf("modbusmq_query version: %s\n", version);
            return 1;
        }
        
        else if (GI.slave < 0)
        {
            GI.slave = strtod(argv[a], NULL);
        }
        else if (strstr(argv[a], "input") ||
                 strstr(argv[a], "holding"))
        {
            GI.input = modbusmq_config_input_type(argv[a]);
            if (!GI.input)
            {
                printf("Unable to parse input type: %s\n", argv[a]);
                return -1;
            }
        }
        else if (strcmp(argv[a], "-4") == 0)
        {
            GI.output_size = 4;
        }
        else if (strcmp(argv[a], "-2") == 0)
        {
            GI.output_size = 2;
        }
        else if (strcmp(argv[a], "-v") == 0)
        {
            GI.verbose++;
        }
        else if (GI.addr < 0)
        {
            GI.addr = strtod(argv[a], NULL);
        }
        else if (!GI.naddr)
        {
            GI.naddr = strtod(argv[a], NULL);
        }
        
        else
        {
            printf("Unknown argument: %s\n", argv[a]);
            return -1;
        }

    }

    if (!GI.connect)
    {
        fprintf(stderr, "connect-string missing\n");
        return -1;
    }
    else if (GI.slave < 0)
    {
        fprintf(stderr, "Slave id missing\n");
        return -1;
    }
    else if (!GI.input)
    {
        fprintf(stderr, "input-type missing\n");
        return -1;
    }
    else if (GI.addr < 0)
    {
        fprintf(stderr, "addr missing\n");
        return -1;
    }
    else if (GI.naddr <= 0)
    {
        fprintf(stderr, "naddr missing\n");
        return -1;
    }

    return 0;
}

void
read_stdio()
{
    
}


//////////////////////////////////////////////////////////////////////////////
// 
//
int
main(int argc, char **argv)
{
    int
        rc;
    memset(&GI, 0, sizeof(GI));
    GI.addr  = -1;
    GI.slave = -1;

    rc = parse_argv(argc, argv);
    if (rc != 0)
    {
        if (rc < 0)
        {
            fprintf(stderr, "Unable to parse argument line\n");
            return -1;
        }
        return 0;
    }

    modbusmq_set_debug(GI.verbose);
    
    
    modbusmq_msg_t
        msg;
    memset(&msg, 0, sizeof(msg));
    msg.frame[0].is_writer = 1;
    msg.frame[1].is_writer = 0;
    
    struct modbusmq_context_t
        *context;

    {
        modbusmq_connect_t
            connect;

        rc = modbusmq_parse_connect_string(GI.connect, &connect);
        if (rc != 0)
        {
            fprintf(stderr, "Unable to parse connect string\n");
            return -2;
        }

        if (connect.connect_type == MODBUSMQ_CONNECT_TCP)
        {
            context = modbusmq_tcp_context(GI.connect);
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
            return -2;
        }
    }
    
    modbusmq_config_t
        *modbusmq_config = modbusmq_config_get();

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

    rc = modbusmq_connect(context);
    if (rc != 0)
    {
        fprintf(stderr, "Unable to connect to device\n");
        exit(2);
    }

    modbusmq_set_slave(context, GI.slave);
    
    switch(GI.input)
    {
    case ModbusmqType_HoldingRegister:
        modbusmq_frame_read_holding_registers(context, &msg.frame[0], GI.addr, GI.naddr);
        break;
    case ModbusmqType_InputRegister:
        modbusmq_frame_read_input_registers(context, &msg.frame[0], GI.addr, GI.naddr);
        break;
    default:
        fprintf(stderr, "Unknown reading type: %d\n", GI.input);
        exit(2);
    }

    rc = modbusmq_send(context, &msg, 2000);
    if (rc < 0)
    {
        fprintf(stderr, "Unable to send and receive a message: rc=%d\n", rc);
        exit(1);
    }
    else if (rc > 0)
    {
        fprintf(stderr, "Timeout while waiting for a complete frame\n");
    }

    modbusmq_set_debug(1);
    // writer:
    modbusmq_frame_debug(context, &msg.frame[0]);
    // reader:
    modbusmq_frame_debug(context, &msg.frame[1]);
    
    // msg.req[] holds the complete request
    // msg.res[] holds the complete response
    //
    // utility functions to extract information from the request and the response.
    //
    // Normally you are only interested in the response.
    //
    // TODO: Add support for formatting types like int_ba, float_abcd and so forth
    //
    uint8_t
        *data     = modbusmq_frame_data(context, &msg.frame[1]);
    int
        nbytes    = modbusmq_frame_nbytes(context, &msg.frame[1]);
    
    int
        value;
    if (GI.output_size <= 0)
    {
        GI.output_size = 2;
    }

    if (nbytes <= 0)
    {
        fprintf(stderr, "ERROR: no response!\n");

        return -1;
    }
    if (!data)
    {
        fprintf(stderr, "ERROR: no data received\n");
        return -1;
    }

    
    // Only process a full output_size chunk when it fits entirely within
    // the received data — nbytes isn't guaranteed to be a multiple of
    // output_size, and reading a partial trailing chunk could run past
    // the end of the frame buffer.
    for(int i = 0; i + GI.output_size <= nbytes; i += GI.output_size)
    {
        const uint8_t
            *ptr = data + i;

        printf("0x");
        
        if (GI.output_size == 2)
        {
            value = modbusmq_read_int16_ab(ptr);
        }
        else
        {
            value = modbusmq_read_int32_abcd(ptr);
        }
        for(int n = 0; n < GI.output_size; ++n)
        {
            printf("%02X", ptr[n]);
        }
        
        printf(" = %6d = 0x%04X\n", value, value);
            
    }
    
    modbusmq_free(context);
}
