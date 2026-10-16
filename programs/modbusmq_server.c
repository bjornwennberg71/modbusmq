//////////////////////////////////////////////////////////////////////////////
//
// bjornwennberg71@gmail.com
//
// modbusmq_server.c
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
#include <signal.h>

#include <termios.h>
#include <math.h>
#include <ctype.h>

#if MQTT_ENABLED
#include <mosquitto.h>
#endif

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

#define MAX_CONFIG_OVERRIDES 64

typedef struct GlobalInfo
{
    const char *config_filename;
    int         verbose;
    int         sd;
    int         has_mqtt;

    // -e key=value entries, pointing straight into argv
    char       *overrides[MAX_CONFIG_OVERRIDES];
    int         noverrides;

    // -e emu.* entries, kept apart so the config parser never sees them
    char       *emu_options[MAX_CONFIG_OVERRIDES];
    int         nemu_options;
} GlobalInfo;


GlobalInfo GI;

static const char *program_name    = "modbusmq_server";
static const char *program_version = MODBUSMQ_VERSION_STRING;

//////////////////////////////////////////////////////////////////////////////
// 
// @brief prints usage
// 
// @param long_usage prints details about parameters
//
void
print_usage(int long_usage)
{
    printf("Usage: %s -c filename [-e key=value ...] [-v]\n", program_name);
    
    if (!long_usage)
    {
        return;
    }

    printf("-c filename  : Reads config from file\n");
    printf("-e key=value : Override a config key, repeatable\n");
    printf("               e.g. -e modbusmq.connect=tcp://localhost:1502\n");
    printf("-v           : increase verbosity\n");
    printf("\n");
    printf("Live values from MQTT (needs a build with MQTT, -e only, not config keys):\n");
    printf("emu.connect  : MQTT broker host:port. A message on <prefix><topic> sets the\n");
    printf("               value every channel with that topic serves, as an engineering\n");
    printf("               value (scaled back to registers). Absent: config defaults only\n");
    printf("emu.prefix   : topic prefix, default emu/\n");
    printf("               e.g. -e emu.connect=127.0.0.1:1883, then publish emu/soc 42.5\n");
    printf("\n");
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
        // exact match: the tests below are substring matches
        if (strcmp(argv[a], "-e") == 0) // -e key=value
        {
            a++;
            if (a >= argc)
            {
                fprintf(stderr, "-e requires key=value\n");
                print_usage(0);
                return -1;
            }
            if (!strchr(argv[a], '=') || argv[a][0] == '=')
            {
                fprintf(stderr, "-e %s: expected key=value\n", argv[a]);
                return -1;
            }
            if (GI.noverrides + GI.nemu_options >= MAX_CONFIG_OVERRIDES)
            {
                fprintf(stderr, "-e: too many overrides (max %d)\n", MAX_CONFIG_OVERRIDES);
                return -1;
            }
            if (strncmp(argv[a], "emu.", 4) == 0)
            {
                GI.emu_options[GI.nemu_options++] = argv[a];
            }
            else
            {
                GI.overrides[GI.noverrides++] = argv[a];
            }
        }
        else if (strstr(argv[a], "--ver")) // --version
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
// Live values from MQTT (emu.connect)
//

#if MQTT_ENABLED

static modbusmq_config_t *emu_config;
static const char        *emu_prefix_text;

//
// Engineering value to the register value the encoder wants: the inverse of
// modbusmq_read_channel(), so mul, then mod, then add, undone in that order.
// Integer formats round, because the encoder truncates.
//
static double
emu_raw_value(const modbusmq_channel_t *channel, double value)
{
    double
        raw = value;

    if (channel->mul != 0)
    {
        raw /= channel->mul;
    }
    if (channel->mod > 0)
    {
        raw /= channel->mod;
    }
    else if (channel->mod < 0)
    {
        raw *= -(double)channel->mod;
    }
    raw -= channel->add;

    switch (channel->format)
    {
    case modbusmq_data_format_float_abcd:
    case modbusmq_data_format_float_badc:
    case modbusmq_data_format_float_dcba:
    case modbusmq_data_format_float_cdab:
        return raw;
    default:
        break;
    }

    if (raw < 0)
    {
        return -(double)(int64_t)(-raw + 0.5);
    }
    return (double)(int64_t)(raw + 0.5);
}

//
// The parser puts mqtt.topic_prefix (with %i already turned into the module
// number) in front of every channel topic. Walk the prefix template along the
// topic and return what is left, the channel's own topic. A topic that does
// not start with the prefix is returned whole.
//
static const char *
emu_own_topic(const char *prefix, const char *topic)
{
    const char
        *p = prefix,
        *t = topic;

    if (!prefix)
    {
        return topic;
    }

    while (*p)
    {
        if (p[0] == '%' && p[1] == 'i')
        {
            if (!isdigit((unsigned char)*t))
            {
                return topic;
            }
            while (isdigit((unsigned char)*t))
            {
                t++;
            }
            p += 2;
        }
        else if (*p == *t)
        {
            p++;
            t++;
        }
        else
        {
            return topic;
        }
    }
    return t;
}

//
// Hand one payload to every channel whose own topic equals rest.
//
static void
emu_apply(modbusmq_config_t *config, const char *rest, const char *payload)
{
    double
        value = 0;
    char
        *end = NULL;

    value = strtod(payload, &end);
    int
        numeric_ok = (end != payload && *end == 0 && isfinite(value)),
        accepted   = 0;

    for(int i = 0; i < config->input_max; ++i)
    {
        modbusmq_input_t
            *input = &config->inputs[i];

        for(int c = 0; c < input->channel_max; ++c)
        {
            modbusmq_channel_t
                *channel = &input->channels[c];

            if (!channel->topic || strcmp(emu_own_topic(config->mqtt_topic_prefix, channel->topic), rest) != 0)
            {
                continue;
            }

            if (modbusmq_format_is_text(channel->format))
            {
                char
                    *copy = strdup(payload);

                if (!copy)
                {
                    continue;
                }
                free(channel->value_text);
                channel->value_text = copy;
            }
            else
            {
                if (!numeric_ok)
                {
                    continue;
                }
                channel->value = emu_raw_value(channel, value);
            }
            accepted = 1;
        }
    }

    if (!GI.verbose)
    {
        return;
    }
    if (accepted)
    {
        printf("emu: %s = %s\n", rest, payload);
    }
    else if (!numeric_ok)
    {
        printf("emu: %s: payload '%s' is not a number, ignored\n", rest, payload);
    }
}

static void
emu_connect_callback(struct mosquitto *mosq, void *userdata, int result)
{
    (void)userdata;

    if (result != 0)
    {
        return;
    }

    char
        topic[512];

    snprintf(topic, sizeof(topic), "%s#", emu_prefix_text);
    mosquitto_subscribe(mosq, NULL, topic, 0);
}

static void
emu_message_callback(struct mosquitto *mosq, void *userdata, const struct mosquitto_message *message)
{
    (void)mosq;
    (void)userdata;

    size_t
        nprefix = strlen(emu_prefix_text);

    if (!message->payload || message->payloadlen <= 0 || message->payloadlen > 1023)
    {
        return;
    }
    if (strncmp(message->topic, emu_prefix_text, nprefix) != 0)
    {
        return;
    }

    // the payload is not NUL terminated
    char
        payload[1024];

    memcpy(payload, message->payload, message->payloadlen);
    payload[message->payloadlen] = 0;

    emu_apply(emu_config, message->topic + nprefix, payload);
}

#define EMU_RECONNECT_INTERVAL_MS 1000

static struct mosquitto *emu_mosq;
static int               emu_connected;
static millitime_t       emu_reconnect_at_ms;

static void
emu_lost(const char *why)
{
    modbusmq_logf(MODBUSMQ_LOG_INFO, "emu: %s, will reconnect\n", why);
    emu_connected       = 0;
    emu_reconnect_at_ms = millitime() + EMU_RECONNECT_INTERVAL_MS;
}

//
// Add the broker socket to the server's select() sets, like programs/modbusmq.c.
// Only ask for write when mosquitto has output pending, or select busy-spins.
//
static void
emu_fd_set(fd_set *read_set, fd_set *write_set, int *fd_max)
{
    if (!emu_mosq || !emu_connected)
    {
        return;
    }

    int
        fd = mosquitto_socket(emu_mosq);

    if (fd < 0)
    {
        emu_lost("socket gone");
        return;
    }
    FD_SET(fd, read_set);
    if (mosquitto_want_write(emu_mosq))
    {
        FD_SET(fd, write_set);
    }
    *fd_max = MODBUSMQ_MAX(fd, *fd_max);
}

//
// Run after every select(), timeout included: reconnect on a timer, else
// read, write and keepalive. The connect callback resubscribes.
//
static void
emu_service(fd_set *read_set, fd_set *write_set)
{
    if (!emu_mosq)
    {
        return;
    }

    if (!emu_connected)
    {
        if (millitime() < emu_reconnect_at_ms)
        {
            return;
        }
        // TODO: blocks like mosquitto_connect() in emu_start(), same fix
        if (mosquitto_reconnect(emu_mosq) != MOSQ_ERR_SUCCESS)
        {
            emu_reconnect_at_ms = millitime() + EMU_RECONNECT_INTERVAL_MS;
            return;
        }
        modbusmq_logf(MODBUSMQ_LOG_INFO, "emu: connected\n");
        emu_connected = 1;
        return;
    }

    int
        fd = mosquitto_socket(emu_mosq);

    if (fd < 0)
    {
        emu_lost("socket gone");
        return;
    }
    if (FD_ISSET(fd, read_set) && mosquitto_loop_read(emu_mosq, 1) != MOSQ_ERR_SUCCESS)
    {
        emu_lost("connection lost");
        return;
    }
    if (FD_ISSET(fd, write_set) && mosquitto_loop_write(emu_mosq, 1) != MOSQ_ERR_SUCCESS)
    {
        emu_lost("connection lost");
        return;
    }
    mosquitto_loop_misc(emu_mosq);
}

//
// Create the client and connect once. A broker that is not up yet is not
// fatal: emu_service() retries from the select loop.
//
static int
emu_start(modbusmq_config_t *config, const char *connect_string, const char *prefix)
{
    modbusmq_connect_t
        connect;
    char
        full[256];

    // host:port is enough, the parser wants the tcp:// in front
    if (strstr(connect_string, "://"))
    {
        snprintf(full, sizeof(full), "%s", connect_string);
    }
    else
    {
        snprintf(full, sizeof(full), "tcp://%s", connect_string);
    }

    if (modbusmq_parse_connect_string(full, &connect) != 0)
    {
        fprintf(stderr, "Unable to parse emu.connect: %s\n", connect_string);
        return -1;
    }

    emu_config      = config;
    emu_prefix_text = prefix;

    mosquitto_lib_init();

    emu_mosq = mosquitto_new(NULL, true, NULL);
    if (!emu_mosq)
    {
        fprintf(stderr, "emu: unable to create mosquitto client\n");
        return -1;
    }

    mosquitto_connect_callback_set(emu_mosq, &emu_connect_callback);
    mosquitto_message_callback_set(emu_mosq, &emu_message_callback);

    // TODO: mosquitto_connect() blocks until TCP gives up on a dead remote host.
    //       Use mosquitto_connect_async() and finish the connect from the select
    //       loop (socket writable = connected). getaddrinfo() still blocks, but only ~10-30 s (resolv.conf timeout x attempts x nameservers).
    if (mosquitto_connect(emu_mosq, connect.device, connect.port, 60) != MOSQ_ERR_SUCCESS)
    {
        fprintf(stderr, "emu: unable to connect to %s, retrying every %d ms\n", connect_string, EMU_RECONNECT_INTERVAL_MS);
        emu_reconnect_at_ms = millitime() + EMU_RECONNECT_INTERVAL_MS;
    }
    else
    {
        emu_connected = 1;
    }

    printf("emu: values from %s, prefix %s\n", connect_string, prefix);
    return 0;
}

#endif // MQTT_ENABLED

//
// emu.connect and emu.prefix come only from -e (they are server options, not
// device config, so the library's config struct stays as it is).
//
static const char *
emu_option(const char *key)
{
    size_t
        nkey = strlen(key);

    for(int o = 0; o < GI.nemu_options; ++o)
    {
        if (strncmp(GI.emu_options[o], key, nkey) == 0 && GI.emu_options[o][nkey] == '=')
        {
            return GI.emu_options[o] + nkey + 1;
        }
    }
    return NULL;
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

        // Modbus caps a single read at 125 registers (250 bytes); this also
        // keeps the fill loops below well within the fixed
        // MODBUSMQ_FRAME_MAX response buffer (client-controlled naddress
        // used to be trusted unchecked here, which allowed a single crafted
        // request to overflow res->buf).
        //
        // Write functions: validate, log, and echo.
        //
        // A real device answers 05 and 06 by returning the request verbatim,
        // and 16 by returning the address and the register count without the
        // data. All three come to 12 bytes.
        //
        // Nothing is stored. The premise of the write path is that a device's
        // setpoint register is not the register you read the value back from,
        // so writing into a read channel would invent a round trip that real
        // hardware does not have.
        //
        if (function == MODBUSMQ_WRITE_SINGLE_COIL ||
            function == MODBUSMQ_WRITE_SINGLE_REGISTER ||
            function == MODBUSMQ_WRITE_MULTIPLE_REGISTERS)
        {
            int
                value = req->buf[10] << 8 | req->buf[11];
            int
                bad = 0;

            if (function == MODBUSMQ_WRITE_SINGLE_COIL)
            {
                // the protocol allows exactly these two
                if (value != 0xFF00 && value != 0x0000)
                {
                    bad = 1;
                }
                else
                {
                    printf("write: slave %d coil 0x%04X = %d\n", slave_id, address_start, value == 0xFF00);
                }
            }
            else if (function == MODBUSMQ_WRITE_SINGLE_REGISTER)
            {
                printf("write: slave %d reg 0x%04X = 0x%04X (%d)\n", slave_id, address_start, value, value);
            }
            else
            {
                //
                // value is the register count here, and the byte count that
                // follows it has to agree with it or the frame is malformed.
                //
                int
                    nbyte = req->buf[12];

                if (value < 1 || value > 123 || nbyte != value * 2 || req->xmit < 13 + nbyte)
                {
                    bad = 1;
                }
                else
                {
                    printf("write: slave %d reg 0x%04X x%d =", slave_id, address_start, value);
                    for (int i = 0; i < nbyte; ++i)
                    {
                        printf(" %02X", req->buf[13 + i]);
                    }
                    printf("\n");
                }
            }

            if (bad)
            {
                res->buf[7] = function | 0x80;
                res->buf[8] = 0x03; // illegal data value
                res->buf[4] = 0;
                res->buf[5] = 3;
                res->length = 9;
                return 0;
            }

            // echo: address and value (or register count for function 16)
            res->buf[8]  = req->buf[8];
            res->buf[9]  = req->buf[9];
            res->buf[10] = req->buf[10];
            res->buf[11] = req->buf[11];

            res->buf[4] = 0;
            res->buf[5] = 6; // unit + function + address(2) + value(2)
            res->length = 12;

            fflush(stdout);
            return 0;
        }

        //
        // Coils and discrete inputs answer with one bit per address packed
        // eight to a byte, so they need their own limit and their own payload
        // shape. Modbus caps them at 2000 per request.
        //
        int
            is_bit = (function == modbusmq_type_coil || function == modbusmq_type_discrete_input);

        if (naddress <= 0 || naddress > (is_bit ? 2000 : 125))
        {
            res->buf[7]  = function | 0x80; // exception response
            res->buf[8]  = 0x03;            // illegal data value
            res->buf[4]  = 0;
            res->buf[5]  = 3;
            res->length  = 9;
            return 0;
        }

        if (is_bit)
        {
            int
                nb = (naddress + 7) / 8;

            memset(&res->buf[9], 0, nb);

            for(int i = 0; i < config->input_max; ++i)
            {
                modbusmq_input_t
                    *input = &config->inputs[i];

                if (input->slave != slave_id || input->type != function)
                {
                    continue;
                }

                for(int c = 0; c < input->channel_max; ++c)
                {
                    modbusmq_channel_t
                        *channel = &input->channels[c];

                    if (!channel->topic)
                    {
                        continue; // an unused channel slot
                    }

                    //
                    // channel->offset is a coil index for a bit input, so the
                    // coil's position in this response is its absolute address
                    // minus where the request started.
                    //
                    int
                        icoil = (input->address + channel->offset) - address_start;

                    if (icoil < 0 || icoil >= naddress)
                    {
                        continue; // outside what was asked for
                    }

                    if (channel->value != 0)
                    {
                        res->buf[9 + (icoil / 8)] |= 1 << (icoil % 8);
                    }

                    printf("default_value: coil %d = %d\n",
                           input->address + channel->offset, channel->value != 0);
                }
            }

            res->buf[8] = nb;                       // byte count
            res->buf[4] = ((nb + 3) & 0xff00) >> 8; // payload length
            res->buf[5] =  (nb + 3) & 0x00ff;
            res->length = nb + 3 + 6;               // + 6 byte header

            return 0;
        }

        //
        // Registers: lay every channel that falls inside the request into the
        // response at its byte position, zeros everywhere else. The position
        // is measured from where the request starts, not from input.address,
        // so a block read through address_offset — or any client asking for a
        // sub-range — gets each value where a real device would put it. It
        // also means channels need not be listed in ascending order, and an
        // odd byte offset is as good as an even one.
        //
        int
            nbytes      = naddress * 2;
        int
            offset_size = config->offset_size > 0 ? config->offset_size : 1;

        memset(&res->buf[9], 0, nbytes);

        for(int i = 0; i < config->input_max; ++i)
        {
            modbusmq_input_t
                *input = &config->inputs[i];

            if (input->slave != slave_id || input->type != function)
            {
                continue;
            }

            for(int c = 0; c < input->channel_max; ++c)
            {
                modbusmq_channel_t
                    *channel = &input->channels[c];

                if (!channel->topic)
                {
                    continue; // an unused channel slot
                }

                int
                    pos = channel->offset * offset_size - (address_start - input->address) * 2;

                if (pos < 0 || pos + channel->length > nbytes)
                {
                    continue; // outside what was asked for
                }

                //
                // A text channel's default went through strtod() as well as
                // being kept verbatim, and only the verbatim copy means
                // anything: "PIX-00123" reads back as 0. Encode from that
                // instead, so the virtual server can serve a serial number,
                // a firmware version or a timestamp the way a real device does.
                //
                int
                    encoded;

                if (modbusmq_format_is_text(channel->format))
                {
                    if (!channel->value_text)
                    {
                        continue; // nothing configured to serve
                    }

                    encoded = modbusmq_encode_text(channel, channel->value_text,
                                                   &res->buf[9 + pos], (size_t)(nbytes - pos));
                }
                else
                {
                    encoded = modbusmq_encode_value(channel->format, channel->value, &res->buf[9 + pos]);
                }

                if (encoded <= 0)
                {
                    printf("default_value: channel %s: %s default does not fit format %s\n",
                           channel->topic,
                           modbusmq_format_is_text(channel->format) ? "text" : "numeric",
                           modbusmq_config_dataformat_name(channel->format));
                    continue;
                }

                if (modbusmq_format_is_text(channel->format))
                {
                    printf("default_value: %s = %s (reg 0x%04X)\n",
                           channel->topic, channel->value_text, address_start + pos / 2);
                }
                else
                {
                    printf("default_value: %s = %.2f (reg 0x%04X)\n",
                           channel->topic, channel->value, address_start + pos / 2);
                }
            }
        }

        res->buf[8] = nbytes;                        // byte count
        res->buf[4] = ((nbytes + 3) & 0xff00) >> 8;  // payload length
        res->buf[5] =  (nbytes + 3) & 0x00ff;
        res->length = nbytes + 3 + 6;                // + 6 byte header
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
        // modbusmq_logf(MODBUSMQ_LOG_INFO, "msg.res_length = %d\n", connection->msg.res_length);
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
        if (rc < 0)
        {
            return -1;
        }
        return 0;
    }

    if (GI.verbose)
    {
        modbusmq_set_debug(GI.verbose);
    }

    
    if (GI.config_filename)
    {
        modbusmq_config_set_overrides(GI.overrides, GI.noverrides);

        rc = modbusmq_config_parse(GI.config_filename);
        if (rc != 0)
        {
            fprintf(stderr, "Error reading config-file: %s\n", GI.config_filename);
            return -1;
        }

        // warn only, never fatal
        modbusmq_config_override_unmatched();
    }


    modbusmq_config_t
        *modbusmq_config = modbusmq_config_get();
    //
    // live values from MQTT, off unless emu.connect is set
    //
    const char
        *emu_connect = emu_option("emu.connect"),
        *emu_prefix  = emu_option("emu.prefix");

    if (emu_connect && strlen(emu_connect) > 0)
    {
#if MQTT_ENABLED
        if (!emu_prefix || strlen(emu_prefix) == 0)
        {
            emu_prefix = "emu/";
        }
        rc = emu_start(modbusmq_config, emu_connect, emu_prefix);
        if (rc < 0)
        {
            return -2;
        }
#else
        fprintf(stderr, "emu.connect is set but this build has MQTT disabled -- rebuild with --enable-mqtt\n");
        return -2;
#endif
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
        return -1;
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

    while(1)
    {

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

#if MQTT_ENABLED
        emu_fd_set(&read_set, &write_set, &fd_max);
#endif
        
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

#if MQTT_ENABLED
        // before the timeout check: reconnect and keepalive run on timeouts too
        if (rc >= 0)
        {
            emu_service(&read_set, &write_set);
        }
#endif

        

        if (rc < 0)
        {
            modbusmq_logf(MODBUSMQ_LOG_ERROR, "select failed: rc = %d\n", rc);
            exit(2);
        }
        else if (rc == 0)
        {
            // normal timeout
            //modbusmq_logf(MODBUSMQ_LOG_INFO, "select timeout\n");
            continue;
        }

        // modbusmq_logf(MODBUSMQ_LOG_INFO, "poll rc = %d\n", rc);

        if (FD_ISSET(GI.sd, &read_set))
        {
            int new_fd = accept(GI.sd, NULL, NULL);
            if (new_fd < 0)
            {
                modbusmq_logf(MODBUSMQ_LOG_ERROR, "accept failed: err=%s\n", strerror(errno));
            }
            else
            {
                set_nonblocking(new_fd);
                set_short_linger(new_fd);

                int accepted = 0;
                for (int i = 0; i < MODBUSMQ_CONNECTION_MAX; ++i)
                {
                    if (modbusmq_connections[i].fd == 0)
                    {
                        modbusmq_logf(MODBUSMQ_LOG_INFO, "TCP Client connect\n");
                        memset(&modbusmq_connections[i], 0, sizeof(modbusmq_connection_t));

                        modbusmq_connections[i].fd = new_fd;


                        modbusmq_connections[i].msg.frame[0].is_writer = 0;
                        modbusmq_connections[i].msg.frame[0].is_tcp = 1;

                        //modbusmq_connections[i].msg.frame[1].length = 12; // HACK
                        modbusmq_connections[i].msg.frame[1].is_writer = 1; // HACK
                        modbusmq_connections[i].msg.frame[1].is_tcp = 1;

                        accepted = 1;
                        break;
                    }
                }

                if (!accepted)
                {
                    modbusmq_logf(MODBUSMQ_LOG_ERROR, "No free connection slot (max %d), rejecting client\n", MODBUSMQ_CONNECTION_MAX);
                    close(new_fd);
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
                        modbusmq_logf(MODBUSMQ_LOG_ERROR, "Unable to read from socket: rc = %d\n", rc);
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
                        //modbusmq_logf(MODBUSMQ_LOG_INFO, "req read bytes=%d\n", rc);
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
                        modbusmq_logf(MODBUSMQ_LOG_ERROR, "Unable to write to socket: rc = %d, closing this connection\n", rc);
                        close(connection->fd);
                        connection->fd = 0;
                        continue;
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



