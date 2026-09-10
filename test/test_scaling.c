//////////////////////////////////////////////////////////////////////////////
//
// bjornwennberg71@gmail.com
//
// test_scaling.c
//
// Pure tests: decoders, encoders, connect string parsing, error strings, value
// formatting and the publish policy. Nothing here touches a socket, so this
// runs without modbusmq_server.
//
#include "modbusmq.h"
#include "modbusmq_config.h"
#include "test_common.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>

//
// decoders
//
static void
test_decoders(void)
{
    printf("decoders\n");

    const uint8_t ab[2]   = { 0x12, 0x34 };
    const uint8_t neg[2]  = { 0xFF, 0xCE };   // -50 as int16
    const uint8_t top[2]  = { 0xFF, 0xFF };   // 65535 unsigned, -1 signed

    CHECK_INT(modbusmq_read_int16_ab(ab),  0x1234, "int16_ab");
    CHECK_INT(modbusmq_read_int16_ba(ab),  0x3412, "int16_ba");

    // the unsigned pair turns a negative reading into a large positive one --
    // this is the bug the _signed variants exist to avoid
    CHECK_INT(modbusmq_read_int16_ab(neg),        0xFFCE, "int16_ab of -50 is unsigned");
    CHECK_INT(modbusmq_read_int16_ab_signed(neg), -50,    "int16_ab_signed of -50");
    CHECK_INT(modbusmq_read_int16_ba_signed(neg), modbusmq_read_int16_ba_signed(neg), "int16_ba_signed callable");

    CHECK_INT(modbusmq_read_int16_ab(top),        65535,  "uint16 top bit set stays positive");
    CHECK_INT(modbusmq_read_int16_ab_signed(top), -1,     "int16 top bit set is -1");

    const uint8_t abcd[4] = { 0x00, 0x01, 0xE2, 0x40 };  // 123456
    CHECK_INT(modbusmq_read_int32_abcd(abcd), 123456, "int32_abcd");

    const uint8_t f[4] = { 0x40, 0x50, 0x00, 0x00 };     // 3.25f big endian
    CHECK_FLT(modbusmq_read_float_abcd(f), 3.25, 0.0001, "float_abcd");
}

//
// encode_value / format_size, and the round trip back through the decoders
//
static void
test_encoders(void)
{
    printf("encoders\n");

    uint8_t buf[4];

    CHECK_INT(modbusmq_format_size(modbusmq_data_format_ab),         2, "format_size ab");
    CHECK_INT(modbusmq_format_size(modbusmq_data_format_abcd),       4, "format_size abcd");
    CHECK_INT(modbusmq_format_size(modbusmq_data_format_float_abcd), 4, "format_size float_abcd");

    memset(buf, 0, sizeof(buf));
    CHECK_INT(modbusmq_encode_value(modbusmq_data_format_ab, 4800, buf), 2, "encode ab returns 2");
    CHECK_INT(modbusmq_read_int16_ab(buf), 4800, "encode/decode ab round trip");

    memset(buf, 0, sizeof(buf));
    CHECK_INT(modbusmq_encode_value(modbusmq_data_format_int16_ab, -50, buf), 2, "encode int16_ab returns 2");
    CHECK_INT(modbusmq_read_int16_ab_signed(buf), -50, "encode/decode int16_ab round trip, negative");

    memset(buf, 0, sizeof(buf));
    CHECK_INT(modbusmq_encode_value(modbusmq_data_format_abcd, 123456, buf), 4, "encode abcd returns 4");
    CHECK_INT(modbusmq_read_int32_abcd(buf), 123456, "encode/decode abcd round trip");

    memset(buf, 0, sizeof(buf));
    CHECK_INT(modbusmq_encode_value(modbusmq_data_format_float_abcd, 3.25, buf), 4, "encode float_abcd returns 4");
    CHECK_FLT(modbusmq_read_float_abcd(buf), 3.25, 0.0001, "encode/decode float round trip");
}

//
// connect strings, including the documented parity-last gotcha
//
static void
test_connect_string(void)
{
    printf("connect strings\n");

    modbusmq_connect_t c;

    memset(&c, 0, sizeof(c));
    CHECK_INT(modbusmq_parse_connect_string("tcp://192.168.1.50:502", &c), 0, "parse tcp");
    CHECK_INT(c.connect_type, MODBUSMQ_CONNECT_TCP, "tcp connect_type");
    CHECK_STR(c.device, "192.168.1.50", "tcp host");
    CHECK_INT(c.port, 502, "tcp port");

    memset(&c, 0, sizeof(c));
    CHECK_INT(modbusmq_parse_connect_string("rtu:///dev/ttyUSB1:9600:1:8:N", &c), 0, "parse rtu");
    CHECK_INT(c.connect_type, MODBUSMQ_CONNECT_RTU, "rtu connect_type");
    CHECK_STR(c.device, "/dev/ttyUSB1", "rtu device");
    CHECK_INT(c.baudrate, 9600, "rtu baud");
    CHECK_INT(c.stopbit,  1,    "rtu stopbit");
    CHECK_INT(c.databits, 8,    "rtu databits");
    CHECK_INT(c.parity,   'N',  "rtu parity");

    // parity is last; putting it earlier must fail rather than silently
    // parsing something else
    memset(&c, 0, sizeof(c));
    CHECK(modbusmq_parse_connect_string("rtu:///dev/ttyUSB1:N:9600:1:8", &c) != 0,
          "parity out of position is rejected");

    memset(&c, 0, sizeof(c));
    CHECK(modbusmq_parse_connect_string("nonsense", &c) != 0, "garbage is rejected");
}

//
// modbusmq_strerror covers both its own codes and errno
//
static void
test_strerror(void)
{
    printf("strerror\n");

    CHECK_STR(modbusmq_strerror(MODBUSMQ_ERR_TRANSPORT), "connection lost",
              "strerror TRANSPORT");
    CHECK_STR(modbusmq_strerror(MODBUSMQ_ERR_PROTOCOL),  "frame rejected, stream resynced",
              "strerror PROTOCOL");
    CHECK_STR(modbusmq_strerror(MODBUSMQ_ERR_TIMEOUT),   "no response within frame timeout",
              "strerror TIMEOUT");

    // an errno still falls through to the C library
    CHECK_STR(modbusmq_strerror(ENOENT), strerror(ENOENT), "strerror falls through to errno");

    // and never returns NULL, whatever it is handed
    CHECK(modbusmq_strerror(0)     != NULL, "strerror(0) is not null");
    CHECK(modbusmq_strerror(-9999) != NULL, "strerror(unknown) is not null");
}

//
// modbusmq_channel_format_value: decimals follow the mod divisor unless the
// channel says otherwise
//
static void
test_format_value(void)
{
    printf("format_value\n");

    modbusmq_input_t   input;
    modbusmq_channel_t channel;
    char               buf[64];

    memset(&input,   0, sizeof(input));
    memset(&channel, 0, sizeof(channel));

    // integer format, no mod -> whole number
    channel.format = modbusmq_data_format_ab;
    modbusmq_channel_format_value(&input, &channel, 42.0f, buf, sizeof(buf));
    CHECK_STR(buf, "42", "integer format prints whole");

    // mod = -100 implies two decimals
    channel.mod = -100;
    modbusmq_channel_format_value(&input, &channel, 48.0f, buf, sizeof(buf));
    CHECK_STR(buf, "48.00", "mod -100 gives two decimals");

    // mod = -10 implies one
    channel.mod = -10;
    modbusmq_channel_format_value(&input, &channel, -5.0f, buf, sizeof(buf));
    CHECK_STR(buf, "-5.0", "mod -10 gives one decimal");

    // float format defaults to three
    memset(&channel, 0, sizeof(channel));
    channel.format = modbusmq_data_format_float_abcd;
    modbusmq_channel_format_value(&input, &channel, 3.25f, buf, sizeof(buf));
    CHECK_STR(buf, "3.250", "float format gives three decimals");

    // an explicit decimals wins. 3.26 rather than 3.25 on purpose: 3.25 is an
    // exact binary tie and printf rounds half to even, so it would be testing
    // the C library's rounding mode rather than this override.
    channel.decimals     = 1;
    channel.has_decimals = 1;
    modbusmq_channel_format_value(&input, &channel, 3.26f, buf, sizeof(buf));
    CHECK_STR(buf, "3.3", "explicit decimals overrides");

    // negative zero is normalised -- it reads as a sign flip that never happened
    memset(&channel, 0, sizeof(channel));
    channel.format = modbusmq_data_format_float_abcd;
    modbusmq_channel_format_value(&input, &channel, -0.0f, buf, sizeof(buf));
    CHECK(strchr(buf, '-') == NULL, "negative zero prints without a sign");
}

//
// modbusmq_channel_publish_decide: the policy, in its documented order
//
static void
test_publish_decide(void)
{
    printf("publish_decide\n");

    modbusmq_channel_t channel;

    // defaults are all off, so every poll publishes -- that is deliberate,
    // consumers read cadence as liveness
    memset(&channel, 0, sizeof(channel));
    CHECK_INT(modbusmq_channel_publish_decide(&channel, 1.0f, "1", 1000), 1, "first publish");
    CHECK_INT(modbusmq_channel_publish_decide(&channel, 1.0f, "1", 1100), 1, "unchanged still publishes by default");

    // on_change suppresses an identical printed value
    memset(&channel, 0, sizeof(channel));
    channel.on_change = 1;
    CHECK_INT(modbusmq_channel_publish_decide(&channel, 1.0f, "1", 1000), 1, "on_change first publish");
    CHECK_INT(modbusmq_channel_publish_decide(&channel, 1.0f, "1", 1100), 0, "on_change suppresses identical text");
    CHECK_INT(modbusmq_channel_publish_decide(&channel, 2.0f, "2", 1200), 1, "on_change publishes a change");

    // min_interval rate limits
    memset(&channel, 0, sizeof(channel));
    channel.min_interval = 1000;
    CHECK_INT(modbusmq_channel_publish_decide(&channel, 1.0f, "1", 1000), 1, "min_interval first publish");
    CHECK_INT(modbusmq_channel_publish_decide(&channel, 2.0f, "2", 1500), 0, "min_interval suppresses too-soon");
    CHECK_INT(modbusmq_channel_publish_decide(&channel, 3.0f, "3", 2000), 1, "min_interval allows after the wait");

    // max_interval is a heartbeat: it beats on_change
    memset(&channel, 0, sizeof(channel));
    channel.on_change    = 1;
    channel.max_interval = 1000;
    CHECK_INT(modbusmq_channel_publish_decide(&channel, 1.0f, "1", 1000), 1, "heartbeat first publish");
    CHECK_INT(modbusmq_channel_publish_decide(&channel, 1.0f, "1", 1500), 0, "unchanged before heartbeat is due");
    CHECK_INT(modbusmq_channel_publish_decide(&channel, 1.0f, "1", 2000), 1, "heartbeat fires though unchanged");

    // min_change deadband
    memset(&channel, 0, sizeof(channel));
    channel.min_change = 1.0f;
    CHECK_INT(modbusmq_channel_publish_decide(&channel, 10.0f, "10",   1000), 1, "deadband first publish");
    CHECK_INT(modbusmq_channel_publish_decide(&channel, 10.5f, "10.5", 1100), 0, "below deadband suppressed");
    CHECK_INT(modbusmq_channel_publish_decide(&channel, 11.5f, "11.5", 1200), 1, "above deadband published");

    CHECK(modbusmq_channel_publish_decide(NULL, 1.0f, "1", 1000) < 0, "null channel rejected");
}

//
// modbusmq_write_encode: the inverse of a channel read, add included
//
static void
test_write_encode(void)
{
    printf("write_encode\n");

    // a context is needed for the logging tag only; it is never connected
    struct modbusmq_context_t *context = modbusmq_tcp_context("tcp://localhost:1");
    CHECK(context != NULL, "context allocated without connecting");
    if (!context)
    {
        return;
    }

    modbusmq_write_t write;
    uint16_t         regs[2];

    // 25.5 degC into a register holding tenths: mod -10 means divide on read,
    // so the write multiplies back up
    memset(&write, 0, sizeof(write));
    write.format = modbusmq_data_format_int16_ab;
    write.mod    = -10;
    CHECK_INT(modbusmq_write_encode(context, &write, 25.5, regs), 1, "write_encode returns one register");
    CHECK_INT(regs[0], 255, "25.5 with mod -10 encodes as 255");

    // add is undone before the divide, matching (raw + add) on the read side
    memset(&write, 0, sizeof(write));
    write.format = modbusmq_data_format_ab;
    write.add    = 5;
    write.mul    = 2;
    CHECK_INT(modbusmq_write_encode(context, &write, 210.0, regs), 1, "write_encode with add/mul");
    CHECK_INT(regs[0], 100, "(100 + 5) * 2 = 210 inverts to 100");

    // a 4-byte format takes two registers
    memset(&write, 0, sizeof(write));
    write.format = modbusmq_data_format_abcd;
    CHECK_INT(modbusmq_write_encode(context, &write, 123456, regs), 2, "32-bit write takes two registers");

    // out of range is rejected, not truncated
    memset(&write, 0, sizeof(write));
    write.format = modbusmq_data_format_ab;
    CHECK(modbusmq_write_encode(context, &write, 99999999.0, regs) < 0, "out-of-range write rejected");

    modbusmq_free(context);
}

int
main(void)
{
    printf("== test_scaling ==\n");

    test_decoders();
    test_encoders();
    test_connect_string();
    test_strerror();
    test_format_value();
    test_publish_decide();
    test_write_encode();

    return test_summary("test_scaling");
}
