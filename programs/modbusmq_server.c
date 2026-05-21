//
// modbusmq_server.cpp
//

// modbusmq_server tcp://localhost:1502 
// or use a config file to support e.g. shoto batteries:
// modbusmq_server -c shoto.config

// 
// INCLUDES //////////////////////////////////////////////////////////////////

#include "modbusmq_internal.h"
#include "modbusmq_tcp.h"
#include "modbusmq_rtu.h"

#include "modbusmq.h"
#include "modbusmq_config.h"
#include "modbusmq_log.h"


#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>

#include <termios.h>

#define MODBUSMQ_CONNECTION_MAX 10

typedef struct modbusmq_connection_t
{
    int            fd;
    modbusmq_msg_t msg;
} modbusmq_connection_t;

modbusmq_connection_t modbusmq_connections[MODBUSMQ_CONNECTION_MAX];

//
// 
// 

typedef struct GlobalInfo
{
    const char *config_filename;
    int         verbose;
    int         sd;
    int         has_mqtt;
} GlobalInfo;


GlobalInfo GI;
static const char *program_name    = "modbusmq_server";
static const char *program_version = "0.1 20250613";

//////////////////////////////////////////////////////////////////////////////
// 
// @brief prints usage
// 
// @param long_usage prints details about parameters
//
void
print_usage(int long_usage)
{
    printf("Usage: %s -c filename\n", program_name);
    
    if (!long_usage)
    {
        return;
    }

    printf("-c filename  : Reads config from file\n");
    printf("-h|--help : prints this help-text\n");
}

//////////////////////////////////////////////////////////////////////////////
// 
// @brief print version information
// 
// @param 
// @return 
// 
void
print_version()
{
    printf("Version: %s %s\n", program_name, program_version);
}

//////////////////////////////////////////////////////////////////////////////
// 
// @brief parses input arguments
// 
// @param argc, argc
// @return 0 on success
// 
int
parse_argv(int argc, char **argv)
{
    for(int a = 1; a < argc; ++a)
    {
        if (strstr(argv[a], "--ver")) // --version
        {
            print_version();
            return 1;
        }
        else if (strstr(argv[a], "-h")) // --help
        {
            print_usage(1);
            return 1;
        }
        else if (strstr(argv[a], "-v")) // -v search
        {
            GI.verbose++;
        }
        else if (strstr(argv[a], "-c")) // -c filename
        {
            a++;
            GI.config_filename = argv[a];
        }
    }

    return 0;
}

//////////////////////////////////////////////////////////////////////////////
// 
// @brief inits program
// 
void
modbusmq_server_init()
{
    memset(&GI, 0, sizeof(GI));
    memset(&modbusmq_connections, 0, sizeof(modbusmq_connections));
}

int set_nonblocking(int fd)
{
    int
        flags = fcntl(fd, F_GETFL, 0);
    
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

void set_short_linger(int fd)
{
    struct linger sl;
    sl.l_onoff = 1;        // enable linger
    sl.l_linger = 1;       // timeout in seconds

    if (setsockopt(fd, SOL_SOCKET, SO_LINGER, &sl, sizeof(sl)) < 0)
    {
        perror("setsockopt(SO_LINGER) failed");
    }
}


int
modbusmq_tcp_server(modbusmq_context_t *context)
{
    if (!context || !context->tcp)
    {
        return -1;
    }

    int sd;

    sd = socket(AF_INET, SOCK_STREAM, 0);
    if (sd < 0)
    {
        perror("Unable to create socket");
        return -1;
    }

    int opt = 1;
    setsockopt(sd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(context->tcp->port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sd, (struct sockaddr*)&addr, sizeof(addr)) < 0)
    {
        fprintf(stderr, "Unable to bind socket to localhost:%d: %d: %s", context->tcp->port, errno, strerror(errno));
        close(sd);
        return -1;
    }

    if (listen(sd, 5) < 0)
    {
        perror("Unable to listen to socket");
        close(sd);
        return -1;
    }

    set_nonblocking(sd);
    return sd;
}

//#include <pty.h>

// int
// modbusmq_rtu_server(modbusmq_context_t *context)
// {
//     if (!context || !context->rtu)
//     {
//         return -1;
//     }

//     modbusmq_rtu_context_t
//         *rtu = context->rtu;

//     int rc;
//     //int master_fd, slave_fd;
//     int master_fd, sd;
//     char slave_name[128];

//     if (openpty(&master_fd, &sd, slave_name, NULL, NULL) < 0)
//     {
//         perror("openpty");
//         return -1;
//     }
//     {
//         char azLine[200];
//         sprintf(azLine, "sudo chmod 777 %s", slave_name);
//         system(azLine);
//     }
//     unlink(rtu->device);
//     rc = symlink(slave_name, rtu->device);
//     if (rc != 0)
//     {
//         fprintf(stderr, "Unable to link to device file: %s, rc=%d, err=%s\n", rtu->device, rc, strerror(errno));
//         close(sd);
//         close(master_fd);
//         return -1;
//     }
    
    
//     // int
//     //     sd = open(rtu->device, O_RDWR | O_NOCTTY | O_NDELAY | O_EXCL | O_CLOEXEC);
//     if (sd < 0)
//     {
//         fprintf(stderr, "Unable to open device: %s, err=%s\n", rtu->device, strerror(errno));
//         return -1;
//     }

//     struct termios
//         tios;
//     speed_t
//         speed;
//     memset(&tios, 0, sizeof(struct termios));

//     rc = tcgetattr(sd, &tios);
//     if (rc < 0)
//     {
//         perror("tcgetattr");
//         close(sd);
//         return -1;
//     }
//     /*
//     On MacOS, constants of baud rates are equal to the integer in argument but
//     that's not the case under Linux so we have to find the corresponding
//     constant. Until the code is upgraded to termios2, the list of possible
//     values is limited (no 14400 for example).
//     */
//     if (9600 == B9600)
//     {
//         speed = rtu->baud;
//     }
//     else
//     {
//         speed = modbusmq_rtu_get_termios_speed(rtu->baud);
//     }

//     if ((cfsetispeed(&tios, speed) < 0) || (cfsetospeed(&tios, speed) < 0))
//     {
//         close(sd);
//         return -1;
//     }

//     /* C_CFLAG      Control options
//        CLOCAL       Local line - do not change "owner" of port
//        CREAD        Enable receiver
//     */
//     tios.c_cflag |= (CREAD | CLOCAL);
//     /* CSIZE, HUPCL, CRTSCTS (hardware flow control) */

//     /* Set data bits (5, 6, 7, 8 bits)
//        CSIZE        Bit mask for data bits
//     */
//     tios.c_cflag &= ~CSIZE;
//     switch (rtu->databit)
//     {
//     case 5:
//         tios.c_cflag |= CS5;
//         break;
//     case 6:
//         tios.c_cflag |= CS6;
//         break;
//     case 7:
//         tios.c_cflag |= CS7;
//         break;
//     case 8:
//     default:
//         tios.c_cflag |= CS8;
//         break;
//     }

//     /* Stop bit (1 or 2) */
//     if (rtu->stopbit == 1)
//     {
//         tios.c_cflag &= ~CSTOPB;
//     }
//     else /* 2 */
//     {
//         tios.c_cflag |= CSTOPB;
//     }

//     // dont set partity on pseudo terminals
//     if (0)
//     {
//         /* PARENB       Enable parity bit
//         PARODD       Use odd parity instead of even */
//         if (rtu->parity == 'N')
//         {
//             /* None */
//             tios.c_cflag &= ~PARENB;
//         }
//         else if (rtu->parity == 'E')
//         {
//             /* Even */
//         tios.c_cflag |= PARENB;
//         tios.c_cflag &= ~PARODD;
//         }
//         else
//         {
//             /* Odd */
//             tios.c_cflag |= PARENB;
//             tios.c_cflag |= PARODD;
//         }
//     }

//     /* Raw input */
//     tios.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
//     if (rtu->parity == 'N')
//     {
//         /* None */
//         tios.c_iflag &= ~INPCK;
//     }
//     else
//     {
//         tios.c_iflag |= INPCK;
//     }
//     tios.c_iflag &= ~(IXON | IXOFF | IXANY);
//     tios.c_oflag &= ~OPOST;

//     tios.c_cc[VMIN] = 0;
//     tios.c_cc[VTIME] = 0;

//     if (tcsetattr(sd, TCSANOW, &tios) < 0)
//     {
//         close(sd);
//         return -1;
//     }

//     context->fd = sd;

//     return sd;
// }


int
modbusmq_server(modbusmq_context_t *context)
{
    if (!context)
    {
        return -1;
    }

    if (context->tcp)
    {
        return modbusmq_tcp_server(context);
    }
    return -1; // rtu not supported for server
}

//////////////////////////////////////////////////////////////////////////////
//
// We have received a complete request in connection.msg.req
// Now create a response and place it in connection.msg.res
int
modbusmq_prepare_response(modbusmq_context_t *context, modbusmq_config_t *config, modbusmq_connection_t *connection)
{
    if (!context || !config || !connection)
    {
        return -1;
    }

    // this is TCP !
    int slave_id, function;
    int write = 0; 

    modbusmq_frame_t
        *req = &connection->msg.frame[0],
        *res = &connection->msg.frame[1];
    
    if (context->tcp)
    {
        res->buf[0] = req->buf[0]; // transaction id 0
        res->buf[1] = req->buf[1]; // transaction id 1
        res->buf[2] = 0;           // protocol id = 0
        res->buf[3] = 0;           // protocol id = 0
        slave_id    = res->buf[6] = req->buf[6]; // slave id
        function    = res->buf[7] = req->buf[7]; // function
    
        int address_start  = req->buf[8] << 8 | req->buf[9]; // 2-byte address
        int naddress       = req->buf[10] << 8 | req->buf[11];
        int address_end    = address_start + naddress * 2;
    
        // input.1.slave          = 1
        // input.1.type           = input_register
        // input.1.address        = 0x0F
        // input.1.naddress       = 6
        // input.1.interval       = 5000
        // input.1.channel.max    = 6

        for(int i = 0; i < config->input_max; ++i)
        {
            modbusmq_input_t
                *input = &config->inputs[i];

            // slave must match
            // function must match
            if (input->slave != slave_id || input->type  != function)
            {
                continue;
            }

            //
            // for each address in the request, fill in response
            //
            for(int c = 0; c < input->channel_max && c < naddress; ++c)
            {
                modbusmq_channel_t
                    *channel = &input->channels[c];
            
                int
                    channel_address = input->address + channel->offset;
                int value = 0;

                while(address_start < channel_address && address_start < address_end)
                {
                    res->buf[9 + write++] = 0;
                    res->buf[9 + write++] = 0;
                    address_start += 2;
                }

                if (channel_address == address_start)
                {
                    // we have a matching address
                    // # Nominal Voltage mV, 2 bytes, offset=0x000f
                    // input.1.channel.1.offset  = 0
                    // input.1.channel.1.format  = int_ab
                    // input.1.channel.1.mod     = -1000
                    // input.1.channel.1.topic   = voltage_nominal
                    
                    float f = channel->value;
                    // debug test:
                    value = (int)f;
            
                    res->buf[9 + write++] = (value & 0xff00) >> 8;
                    res->buf[9 + write++] = value & 0x00ff;

                    address_start += 2;

                    printf("default_value: %02X = %.2f\n", address_start, channel->value);
                }
                else
                {
                    printf("default_value: no address match for %02X\n", channel_address);
                }
            }
        }

        // now fill in blanks at the end if we did not have enough channels to fulfill the request
        while(address_start < address_end)
        {
            res->buf[9 + write++] = 0;
            res->buf[9 + write++] = 0;
            address_start += 2;
        }
    
    
        // payload is naddress * 2 (write) + unit_id + function + byte_count
        res->buf[4] = ((write + 3 ) & 0xff00) >> 8;
        res->buf[5] =  (write + 3 ) & 0x00ff ;
    
        // payload length= naddress *  (write)
        res->buf[8] = write;

        // header is 6 bytes
        res->length = write + 3 + 6;

        if (write <= 0)
        {
            // this means we did not find a respons, so mark the message with error
            res->buf[7] |= 0x80; // function + 0x80
            res->buf[8] = 0x02;  // illegal data access
        }
    }
    else if (context->rtu)
    {
        // RTU req frame to read:
        //    req[0] = context->slave_id;
        //    req[1] = function;
        //    req[2] = addr >> 8;        // address
        //    req[3] = addr & 0x00ff;
        //    req[4] = naddr >> 8;       // number of addresses
        //    req[5] = naddr & 0x00ff;
        //    req[6] = crc
        //    req[7] = crc

        // RTU res frame to read
        //    res[0] = slave
        //    res[1] = function
        //    res[2] = number of bytes
        //    res[3..nb] data
        //    res[nb+1] = crc & 0x00ff
        //    res[nb+2] = crc >> 8

        // slave_id = connection->msg.res[0] = connection->msg.req[0]; // slave id
        // function = connection->msg.res[1] = connection->msg.req[1]; // function
    
        // int address_start  = connection->msg.req[2] >> 8 | connection->msg.req[3];
        // int naddress       = connection->msg.req[4] >> 8 | connection->msg.req[5];
        // int address_end    = address_start + naddress * 2;

        // int write_start    = 3; // data start at this byte-position
        
        // // input.1.slave          = 1
        // // input.1.type           = input_register
        // // input.1.address        = 0x0F
        // // input.1.naddress       = 6
        // // input.1.interval       = 5000
        // // input.1.channel.max    = 6

        // for(int i = 0; i < config->input_max; ++i)
        // {
        //     modbusmq_input_t
        //         *input = &config->inputs[i];

        //     // slave must match
        //     // function must match
        //     if (input->slave != slave_id ||
        //         input->type  != function)
        //     {
        //         continue;
        //     }

        //     //
        //     // for each address in the request, fill in response
        //     //
        //     for(int c = 0; c < input->channel_max && c < naddress; ++c)
        //     {
        //         modbusmq_channel_t
        //             *channel = &input->channels[c];
            
        //         int
        //             channel_address = input->address + channel->offset;
        //         int value = 0;

        //         while(address_start < channel_address && address_start < address_end)
        //         {
        //             connection->msg.res[write_start + write++] = 0;
        //             connection->msg.res[write_start + write++] = 0;
        //             address_start += 2;
        //         }

        //         if (channel_address == address_start)
        //         {
        //             // we have a matching address
        //             // # Nominal Voltage mV, 2 bytes, offset=0x000f
        //             // input.1.channel.1.offset  = 0
        //             // input.1.channel.1.format  = int_ab
        //             // input.1.channel.1.mod     = -1000
        //             // input.1.channel.1.topic   = voltage_nominal
                    
        //             float f = channel->value;
        //             // debug test:
        //             value = (int)f;
            
        //             connection->msg.res[write_start + write++] = (value & 0xff00) >> 8;
        //             connection->msg.res[write_start + write++] = value & 0x00ff;

        //             address_start += 2;
        //         }
        //     }
        // }

        // // now fill in blanks at the end if we did not have enough channels to fulfill the request
        // while(address_start < address_end)
        // {
        //     connection->msg.res[write_start + write++] = 0;
        //     connection->msg.res[write_start + write++] = 0;
        //     address_start += 2;
        // }
    
    
        // // payload is naddress * 2 
        // connection->msg.res[2] = write;
    
        // // payload length= naddress *  (write)
        // connection->msg.res[8] = write;

        // // header is 4 bytes
        // connection->msg.res_length = write + context->header_length + 3;
        // modbusmq_logf(LOG_INFO, "msg.res_length = %d\n", connection->msg.res_length);
        // if (write <= 0)
        // {
        //     // this means we did not find a respons, so mark the message with error
        //     connection->msg.res[1] |= 0x80; // function + 0x80
        // }
    }
    
    return 0;
    
}
    



//////////////////////////////////////////////////////////////////////////////
// 
// @brief  main
// 
int
main(int argc, char **argv)
{
    int rc;

    modbusmq_server_init();
    
    rc = parse_argv(argc, argv);
    if (rc != 0)
    {
        return 1;
    }

    if (GI.verbose)
    {
        modbusmq_set_debug(GI.verbose);
    }

    
    if (GI.config_filename)
    {
        rc = modbusmq_config_parse(GI.config_filename);
        if (rc != 0)
        {
            fprintf(stderr, "Error reading config-file: %s\n", GI.config_filename);
            return 1;
        }
    }


    modbusmq_config_t
        *modbusmq_config = modbusmq_config_get();
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
    
    rc = modbusmq_server(context);
    if (rc < 0)
    {
        fprintf(stderr, "Unable to setup server\n");
        modbusmq_close(context);
        return 1;
    }
    
    
    GI.sd = rc;



    //
    // main loop
    //
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

            
        //
        // register all file handlers
        //
        fd_max = 0;
        FD_ZERO(&read_set);
        FD_ZERO(&write_set);
        FD_ZERO(&err_set);

        // main incoming socket
        FD_SET(GI.sd, &read_set);
        FD_SET(GI.sd, &err_set);
        fd_max = MODBUSMQ_MAX(GI.sd, fd_max);

        
        for(int i = 0; i < MODBUSMQ_CONNECTION_MAX; ++i)
        {
            modbusmq_connection_t
                *connection = &modbusmq_connections[i];
            if (connection->fd > 0)
            {
                // you always want to read
                FD_SET(connection->fd, &read_set);

                // If read is complete, and write is incomplete, then you want to write
                if ((modbusmq_frame_incomplete(context, &connection->msg.frame[0]) == 0) &&
                    modbusmq_frame_incomplete(context, &connection->msg.frame[1]) )
                {
                    FD_SET(connection->fd, &write_set);
                }
                fd_max = MODBUSMQ_MAX(connection->fd, fd_max);
            }
        }
        
        //
        // set up timeout for select
        //
        tv.tv_sec  = 0;
        tv.tv_usec = millisleep * 1000;
        
        rc = select(fd_max+1, &read_set, &write_set, NULL, &tv);

        

        if (rc < 0)
        {
            modbusmq_logf(LOG_ERROR, "select failed: rc = %d\n", rc);
            exit(2);
        }
        else if (rc == 0)
        {
            // normal timeout
            //modbusmq_logf(LOG_INFO, "select timeout\n");
            continue;
        }

        // modbusmq_logf(LOG_INFO, "poll rc = %d\n", rc);

        if (FD_ISSET(GI.sd, &read_set))
        {
            int new_fd = accept(GI.sd, NULL, NULL);
            set_nonblocking(new_fd);
            set_short_linger(new_fd);
                            
            for (int i = 0; i < MODBUSMQ_CONNECTION_MAX; ++i)
            {
                if (modbusmq_connections[i].fd == 0)
                {
                    modbusmq_logf(LOG_INFO, "TCP Client connect\n");
                    memset(&modbusmq_connections[i], 0, sizeof(modbusmq_connection_t));
                            
                    modbusmq_connections[i].fd = new_fd;

                    
                    modbusmq_connections[i].msg.frame[0].is_writer = 0;
                    modbusmq_connections[i].msg.frame[0].is_tcp = 1;
                    
                    //modbusmq_connections[i].msg.frame[1].length = 12; // HACK
                    modbusmq_connections[i].msg.frame[1].is_writer = 1; // HACK
                    modbusmq_connections[i].msg.frame[1].is_tcp = 1;
                    
                                
                    break;
                }
            }
        }

                    
        modbusmq_connection_t
            *connection = NULL;
        for(int i = 0; i < MODBUSMQ_CONNECTION_MAX; ++i)
        {
            connection = &modbusmq_connections[i];
            if (connection->fd == 0)
            {
                continue;
            }
            
            // here we can read/write without thinking about tcp/rtu
            if (FD_ISSET(connection->fd, &read_set))
            {
                // check if we need to read more
                
                if (modbusmq_frame_incomplete(context, &connection->msg.frame[0]))
                {
                    
                    rc = modbusmq_read(context, connection->fd, &connection->msg.frame[0]);
                    if (rc < 0)
                    {
                        context->err++;
                        modbusmq_logf(LOG_ERROR, "Unable to read from socket: rc = %d\n", rc);
                        continue;
                    }
                    else if (rc == 0)
                    {
                        printf("Client %d disconnected normal\n", connection->fd);
                        
                        if (context->tcp)
                        {
                            close(connection->fd);
                            connection->fd = 0;
                        }
                    }                        
                    else
                    {
                        //modbusmq_logf(LOG_INFO, "req read bytes=%d\n", rc);
                        //modbusmq_msg_debug(context, &connection->msg, 0);
                                
                        rc = 0;
                        if (connection->msg.frame[0].xmit == connection->msg.frame[0].length)
                        {
                            if (GI.verbose >= 1)
                            {
                                modbusmq_frame_debug(context, &connection->msg.frame[0]);
                            }
                                    
                            modbusmq_prepare_response(context, modbusmq_config, connection);
                        }
                    }
                }
            } // end read

            if (FD_ISSET(connection->fd, &write_set))
            {
                // check if we need to write more
                if (modbusmq_frame_incomplete(context, &connection->msg.frame[1]))
                {
                    // write some
                    rc = modbusmq_write(context, connection->fd, &connection->msg.frame[1]);
                    if (rc < 0)
                    {
                        context->err++;
                        modbusmq_logf(LOG_ERROR, "Unable to write to socket: rc = %d\n", rc);
                        return -1;
                    }

                    context->last_write_ms = millitime();

                    if (connection->msg.frame[1].length == connection->msg.frame[1].xmit)
                    {
                        if (GI.verbose >= 1)
                        {
                            modbusmq_frame_debug(context, &connection->msg.frame[1]);
                        }
                    }
                    // done writing response, now clear the msg for re-use
                    memset(&connection->msg, 0, sizeof(connection->msg));
                    connection->msg.frame[1].is_writer = 1;
                    if (context->tcp)
                    {
                        //connection->msg.frame[0].length = 12; // HACK
                    }
                    else if (context->rtu)
                    {
                        connection->msg.frame[0].length = 8; // HACK
                    }
                    else
                    {
                        assert(0);
                    }
                            
                }
            } // end write

            if (FD_ISSET(connection->fd, &err_set))
            {
                printf("Client %d disconnected or error, errno=%d:%s\n", connection->fd, errno, strerror(errno));
                        
                if (context->tcp)
                {
                    close(connection->fd);
                    connection->fd = 0;
                }
                else if (context->rtu)
                {
                    ; // do nothing
                }
            }
        }
    }
}



