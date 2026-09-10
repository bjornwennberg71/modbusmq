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

    int         is_write;    // --write was given
    double      write_value; // in engineering units, before scaling is undone
    int         format;      // modbusmq_data_format_*, register writes only
    int         mod;
    int         mul;
    int         add;
    int         dry_run;     // build and print the frame, send nothing
} global_info;

static global_info GI;

//
// print help
//
void
print_help(int argc, char **argv, int print_long)
{
    (void)argc;
    printf("read : %s connect-string slave-id register_type addr naddr byte-size\n", argv[0]);
    printf("write: %s connect-string slave-id register_type addr --write value [--format fmt] [--mod n] [--mul n] [--add n] [--dry-run]\n", argv[0]);
    printf("example: %s <rtu:///dev/ttyUSB3:9600:1:8:N | tcp://hostname:port> slave_id <%s | %s | %s>  addr naddr <-4|-2> \n", argv[0], MODBUSMQ_TYPE_INPUT_REGISTER, MODBUSMQ_TYPE_HOLDING_REGISTER, MODBUSMQ_TYPE_COIL);

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
        printf("\n");
        printf("--write value         : write instead of read. naddr is not used.\n");
        printf("                        %s writes a coil (function 05); a register\n", MODBUSMQ_TYPE_COIL);
        printf("                        write is function 06, or 16 for a 4-byte format.\n");
        printf("--format fmt          : data format for a register write, default %s.\n", MODBUSMQ_FORMAT_UAB);
        printf("                        int_ab/uint_ab/int_abcd/float_abcd and so on.\n");
        printf("--mod n               : scaling, same convention as a config channel.\n");
        printf("--mul n                 The value given to --write is in engineering units\n");
        printf("--add n                 and these undo the scaling, exactly as modbusmq does.\n");
        printf("--dry-run            : show the frame that would be sent, and send nothing.\n");
        printf("\n");
        printf("write examples:\n");
        printf("  # 25.5 degC into a signed register holding tenths\n");
        printf("  %s tcp://host:502 39 %s 0x0028 --write 25.5 --format int_ab --mod -10\n", argv[0], MODBUSMQ_TYPE_HOLDING_REGISTER);
        printf("  # switch a coil on\n");
        printf("  %s tcp://host:502 39 %s 0x000C --write 1\n", argv[0], MODBUSMQ_TYPE_COIL);
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
                 strstr(argv[a], "holding") ||
                 strstr(argv[a], "coil"))
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
        else if (strcmp(argv[a], "--dry-run") == 0)
        {
            GI.dry_run = 1;
        }
        else if (strcmp(argv[a], "--write") == 0 ||
                 strcmp(argv[a], "--format") == 0 ||
                 strcmp(argv[a], "--mod") == 0 ||
                 strcmp(argv[a], "--mul") == 0 ||
                 strcmp(argv[a], "--add") == 0)
        {
            const char
                *flag = argv[a];

            a++;
            if (a >= argc)
            {
                fprintf(stderr, "%s requires a value\n", flag);
                return -1;
            }

            if (strcmp(flag, "--write") == 0)
            {
                char
                    *end = NULL;

                GI.write_value = strtod(argv[a], &end);
                if (end == argv[a] || *end)
                {
                    fprintf(stderr, "--write: %s is not a number\n", argv[a]);
                    return -1;
                }
                GI.is_write = 1;
            }
            else if (strcmp(flag, "--format") == 0)
            {
                GI.format = modbusmq_config_dataformat(argv[a]);
                if (GI.format == modbusmq_data_format_unknown)
                {
                    return -1; // already reported, with the offending name
                }
            }
            else if (strcmp(flag, "--mod") == 0) { GI.mod = (int)strtol(argv[a], NULL, 0); }
            else if (strcmp(flag, "--mul") == 0) { GI.mul = (int)strtol(argv[a], NULL, 0); }
            else                                 { GI.add = (int)strtol(argv[a], NULL, 0); }
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
    else if (!GI.is_write && GI.naddr <= 0)
    {
        fprintf(stderr, "naddr missing\n");
        return -1;
    }

    if (GI.is_write)
    {
        if (GI.input == modbusmq_type_input_register)
        {
            fprintf(stderr, "%s is read-only, nothing can be written to it\n", MODBUSMQ_TYPE_INPUT_REGISTER);
            return -1;
        }
        //
        // A coil carries one bit, so a format would have nothing to describe.
        //
        if (GI.input != modbusmq_type_coil && GI.format == modbusmq_data_format_unknown)
        {
            GI.format = modbusmq_data_format_ab; // uint_ab, the common case
        }
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

    //
    // A dry run never touches the device, so it must not need one to be
    // reachable — checking a setpoint's scaling from a desk is the whole point.
    //
    if (!GI.dry_run)
    {
        rc = modbusmq_connect(context);
        if (rc != 0)
        {
            fprintf(stderr, "Unable to connect to device\n");
            exit(2);
        }
    }

    modbusmq_set_slave(context, GI.slave);

    if (GI.dry_run && !GI.is_write)
    {
        fprintf(stderr, "--dry-run only applies to --write\n");
        modbusmq_free(context);
        return -1;
    }

    //
    // Write path.
    //
    // The value is scaled by the same modbusmq_write_encode() that
    // modbusmq uses, so checking a setpoint here checks the code
    // that will carry it in production rather than something that resembles it.
    //
    if (GI.is_write)
    {
        int
            nregs = 0;
        uint16_t
            regs[2] = {0};

        if (GI.input == modbusmq_type_coil)
        {
            int
                on = (GI.write_value != 0);

            modbusmq_frame_write_coil_bit(context, &msg.frame[0], GI.addr, on);
            printf("write: slave %d coil 0x%04X = %d\n", GI.slave, GI.addr, on);
        }
        else
        {
            modbusmq_write_t
                write;

            memset(&write, 0, sizeof(write));
            write.name    = "query";
            write.slave   = GI.slave;
            write.type    = modbusmq_type_holding_register;
            write.address = GI.addr;
            write.format  = GI.format;
            write.length  = modbusmq_format_size(GI.format);
            write.mod     = GI.mod;
            write.mul     = GI.mul;
            write.add     = GI.add;

            nregs = modbusmq_write_encode(context, &write, GI.write_value, regs);
            if (nregs < 0)
            {
                // already reported, with the reason and the range
                modbusmq_free(context);
                return -1;
            }

            if (nregs == 1)
            {
                modbusmq_frame_write_register(context, &msg.frame[0], GI.addr, regs[0]);
                printf("write: slave %d reg 0x%04X = %g (raw 0x%04X, function 06)\n",
                       GI.slave, GI.addr, GI.write_value, regs[0]);
            }
            else
            {
                modbusmq_frame_write_registers(context, &msg.frame[0], GI.addr, nregs, regs);
                printf("write: slave %d reg 0x%04X = %g (raw 0x%04X%04X, function 16)\n",
                       GI.slave, GI.addr, GI.write_value, regs[0], regs[1]);
            }
        }

        if (GI.dry_run)
        {
            //
            // The frame has no transaction id or CRC yet — those are stamped
            // when it is posted — so say so rather than let the bytes be read
            // as what would go on the wire.
            //
            printf("dry run, nothing sent. Frame body (no transaction id or CRC yet):\n  ");
            for (int i = 0; i < msg.frame[0].length; ++i)
            {
                printf("%02X ", msg.frame[0].buf[i]);
            }
            printf("\n");
            modbusmq_free(context);
            return 0;
        }

        rc = modbusmq_send(context, &msg, 2000);
        if (rc < 0)
        {
            fprintf(stderr, "Write failed: rc=%d\n", rc);
            modbusmq_free(context);
            return 1;
        }
        else if (rc > 0)
        {
            fprintf(stderr, "Timeout waiting for the write to be acknowledged\n");
            modbusmq_free(context);
            return 1;
        }

        //
        // A device echoes the write back. Anything else has already been
        // rejected by modbusmq_send(), so reaching here means it took it.
        //
        printf("device acknowledged the write\n");

        if (GI.verbose)
        {
            modbusmq_frame_debug(context, &msg.frame[0]);
            modbusmq_frame_debug(context, &msg.frame[1]);
        }

        modbusmq_free(context);
        return 0;
    }

    switch(GI.input)
    {
    case modbusmq_type_holding_register:
        modbusmq_frame_read_holding_registers(context, &msg.frame[0], GI.addr, GI.naddr);
        break;
    case modbusmq_type_input_register:
        modbusmq_frame_read_input_registers(context, &msg.frame[0], GI.addr, GI.naddr);
        break;
    case modbusmq_type_coil:
        modbusmq_frame_read_coil_bits(context, &msg.frame[0], GI.addr, GI.naddr);
        break;
    case modbusmq_type_discrete_input:
        modbusmq_frame_read_input_bits(context, &msg.frame[0], GI.addr, GI.naddr);
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
