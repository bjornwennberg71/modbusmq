//////////////////////////////////////////////////////////////////////////////
// 
// bjornwennberg71@gmail.com
// 
// modbusmq_config.h
//
// key-value pairs
//
#ifndef modbusmq_config_h_
#define modbusmq_config_h_

#include <stdint.h>

//
// millitime_t, for the runtime publish-rate-limiting timestamps below. Pulled
// in here rather than duplicated as int64_t, since modbusmq_time.h has no
// dependency back on this header.
//
#include "modbusmq_time.h"

#ifdef __cplusplus
extern "C" {
#endif

//
// The values are the Modbus read function codes, so an input's type is also
// the function used to poll it.
//
typedef enum modbusmq_type_e
{
    modbusmq_type_coil             = 0x01,
    modbusmq_type_discrete_input   = 0x02,
    modbusmq_type_holding_register = 0x03,
    modbusmq_type_input_register   = 0x04
} modbusmq_type_e;

#define MODBUSMQ_TYPE_COIL             "coil"
#define MODBUSMQ_TYPE_DISCRETE_INPUT   "discrete_input"
#define MODBUSMQ_TYPE_INPUT_REGISTER   "input_register"
#define MODBUSMQ_TYPE_HOLDING_REGISTER "holding_register"

//
// Which Modbus function a write.N entry uses. The values are the function
// codes themselves, the same way modbusmq_type_e carries 0x03/0x04.
//
// Only single-coil (05) and single/multiple register (06/16) writes are
// offered. Function 15, write multiple coils, is deliberately absent: a block
// write entry lays out registers, no device so far has wanted a run of coils
// set in one go, and 15 buys nothing but a bit-packing order to get wrong.
//
typedef enum modbusmq_write_function_e
{
    modbusmq_write_function_unknown   = 0x00,
    modbusmq_write_function_coil      = 0x05,
    modbusmq_write_function_register  = 0x06,
    modbusmq_write_function_registers = 0x10
} modbusmq_write_function_e;

//
// Appended to a write topic to make its ack topic, when the config does not
// name one itself.
//
#define MODBUSMQ_ACK_SUFFIX               "/ack"

#define MODBUSMQ_FUNCTION_WRITE_COIL      "write_coil"
#define MODBUSMQ_FUNCTION_WRITE_REGISTER  "write_register"
#define MODBUSMQ_FUNCTION_WRITE_REGISTERS "write_registers"
    
//
// The name says the type, the suffix says the byte order: ab is high byte
// first, ba is low byte first, abcd and badc the 32-bit equivalents.
//
// int_* is signed and uint_* is unsigned, as everywhere else in C.
//
// int_ab and int_ba used to mean *unsigned*, which is how a sub-zero
// temperature came to publish as 6548.6 rather than -5.0. Configs declaring
// config.version 2.0 or later get the corrected meaning; older ones keep the
// old one and are warned at startup, so an already-deployed file never changes
// meaning underneath its owner. See modbusmq_config_apply_format_version().
//
typedef enum modbusmq_data_format_e
{
    modbusmq_data_format_unknown = 0,
    modbusmq_data_format_a,    // uint8_t
    modbusmq_data_format_ab,   // uint16_t, high byte first
    modbusmq_data_format_ba,   // uint16_t, low byte first
    modbusmq_data_format_int8,     // int8_t
    modbusmq_data_format_int16_ab, // int16_t, high byte first
    modbusmq_data_format_int16_ba, // int16_t, low byte first
    modbusmq_data_format_abcd,     // int32_t, high byte first
    modbusmq_data_format_badc,     // int32_t, mixed
    modbusmq_data_format_uint32_abcd,
    modbusmq_data_format_uint32_badc,
    modbusmq_data_format_float_ba, // float
    modbusmq_data_format_float_abcd, // float
    modbusmq_data_format_float_badc, // float
    modbusmq_data_format_float_dcba, // float
    modbusmq_data_format_float_cdab, // float, low word first (common in industrial Modbus devices)

    //
    // Text formats. These decode to a string rather than a number and never
    // travel through the float path — see modbusmq_read_channel_text().
    //
    // Appended at the end on purpose: the numeric values above are what a
    // config's format name resolves to, and shifting them would change the
    // meaning of nothing in a file but everything in a core dump.
    //
    modbusmq_data_format_ascii_ab,   // 2 chars per register, high byte first
    modbusmq_data_format_ascii_ba,   // ...with the two bytes of each register swapped
    modbusmq_data_format_bcd_ab,     // packed BCD digits, high nibble first
    modbusmq_data_format_bcd_ba,     // ...with the two bytes of each register swapped
    modbusmq_data_format_version_ab,     // 2 bytes  -> "a.b"
    modbusmq_data_format_version_abcd,   // 4 bytes  -> "a.b.c.d"
    modbusmq_data_format_version_regs,   // one uint16 field per register -> "a.b.c"
    modbusmq_data_format_date_ymd_abcd,  // uint16 year, uint8 month, uint8 day
    modbusmq_data_format_datetime_regs,  // 6 registers: year, month, day, hour, min, sec
    modbusmq_data_format_epoch32_abcd,   // uint32 seconds since the epoch
    modbusmq_data_format_epoch32_badc    // ...word-swapped

} modbusmq_data_format_e;

#define MODBUSMQ_FORMAT_A          "int_a"
#define MODBUSMQ_FORMAT_AB         "int_ab"
#define MODBUSMQ_FORMAT_BA         "int_ba"
#define MODBUSMQ_FORMAT_ABCD       "int_abcd"
#define MODBUSMQ_FORMAT_BADC       "int_badc"
#define MODBUSMQ_FORMAT_UA         "uint_a"
#define MODBUSMQ_FORMAT_UAB        "uint_ab"
#define MODBUSMQ_FORMAT_UBA        "uint_ba"
#define MODBUSMQ_FORMAT_UABCD      "uint_abcd"
#define MODBUSMQ_FORMAT_UBADC      "uint_badc"

//
// config.version at which int_* became signed
//
#define MODBUSMQ_SIGNED_INT_VERSION 2.0
#define MODBUSMQ_FORMAT_FLOAT_BA   "float_ba"
#define MODBUSMQ_FORMAT_FLOAT_ABCD "float_abcd"
#define MODBUSMQ_FORMAT_FLOAT_BADC "float_badc"
#define MODBUSMQ_FORMAT_FLOAT_DCBA "float_dcba"
#define MODBUSMQ_FORMAT_FLOAT_CDAB "float_cdab"

#define MODBUSMQ_FORMAT_ASCII_AB      "ascii_ab"
#define MODBUSMQ_FORMAT_ASCII_BA      "ascii_ba"
#define MODBUSMQ_FORMAT_BCD_AB        "bcd_ab"
#define MODBUSMQ_FORMAT_BCD_BA        "bcd_ba"
#define MODBUSMQ_FORMAT_VERSION_AB    "version_ab"
#define MODBUSMQ_FORMAT_VERSION_ABCD  "version_abcd"
#define MODBUSMQ_FORMAT_VERSION_REGS  "version_regs"
#define MODBUSMQ_FORMAT_DATE_YMD_ABCD "date_ymd_abcd"
#define MODBUSMQ_FORMAT_DATETIME_REGS "datetime_regs"
#define MODBUSMQ_FORMAT_EPOCH32_ABCD  "epoch32_abcd"
#define MODBUSMQ_FORMAT_EPOCH32_BADC  "epoch32_badc"

//
// How much wire a text channel may span, and how much text it may decode to.
//
// The wire cap is the bound that matters for safety; the text cap is derived
// from it — packed BCD is the worst expansion at two digits per byte, and a
// version string adds a separator per field on top of that.
//
#define MODBUSMQ_TEXT_BYTES_MAX 64
#define MODBUSMQ_TEXT_MAX      140

//
// Default strftime format for the time formats. ISO 8601, UTC, which is the
// only sane default for a value crossing a broker into somebody else's
// timezone. channel.timefmt overrides it, channel.timezone picks local.
//
// ...and the pattern for a device that reported digits with no timezone
// attached, where the trailing Z would be a guess rather than a fact.
#define MODBUSMQ_TIMEFMT_DEFAULT      "%Y-%m-%dT%H:%M:%SZ"
#define MODBUSMQ_TIMEFMT_NAIVE        "%Y-%m-%dT%H:%M:%S"
#define MODBUSMQ_DATEFMT_DEFAULT      "%Y-%m-%d"

typedef enum modbusmq_query_mode_e
{
    modbusmq_query_mode_min       = 0,
    modbusmq_query_mode_parallell = 0, // parallell
    modbusmq_query_mode_series    = 1,  // series
    modbusmq_query_mode_max 
} modbusmq_query_mode_e;

//
// info about one channel
//    
typedef struct modbusmq_channel_t
{
    int                     offset;
                                   // Wire bytes the channel occupies. Derived
                                   // from the format for every fixed-size one;
                                   // a text format that spans as much register
                                   // as the device feels like — ascii, bcd,
                                   // version_regs — has no size of its own and
                                   // takes it from channel.length /
                                   // channel.nregisters instead, which is what
                                   // has_length records.
    int                     length;
    uint8_t                 has_length;
    int                     format;
    int                     add;
    int                     mod;    // -10 = value/10, 100 = value * 100
    int                     mul;    // if mul != 0, value*mul
    char                   *topic;
    float                   value; // used for debug to hold a value
                                   // The same default, unparsed. A text
                                   // channel's default cannot survive a
                                   // strtod(), so modbusmq_server serves this
                                   // instead for those formats.
    char                   *value_text;

                                   // Time formats only: strftime pattern for
                                   // the decoded timestamp, and whether to
                                   // render it in local time. Unset means
                                   // MODBUSMQ_TIMEFMT_DEFAULT (or the date-only
                                   // variant) and UTC.
    char                   *timefmt;
    uint8_t                 localtime;

                                   // per-channel publish options. unset means
                                   // take mqtt.retain / mqtt.qos
    int                     retain;
    uint8_t                 has_retain;
    int                     qos;
    uint8_t                 has_qos;

                                   // Decimal places when formatting the value
                                   // for output/publish. Unset means fall back
                                   // to the type/format-driven default — see
                                   // modbusmq_channel_format_value().
    int                     decimals;
    uint8_t                 has_decimals;

                                   // Publish rate limiting. Unset means take
                                   // the input-level default, and failing that
                                   // the config-wide publish.* default,
                                   // resolved once by
                                   // modbusmq_config_apply_publish_defaults()
                                   // right after parsing — the runtime never
                                   // has to fall back to the input or config.
    uint8_t                 on_change; // suppress unless the printed text changed
    float                   min_change;
    float                   min_change_rel; // fraction of the value, e.g. 0.001 = 0.1%
    int                     min_interval;
    int                     max_interval;
    uint8_t                 has_on_change;
    uint8_t                 has_min_change;
    uint8_t                 has_min_change_rel;
    uint8_t                 has_min_interval;
    uint8_t                 has_max_interval;

                                   // Runtime state for the decision above, not
                                   // config: what was last actually published,
                                   // and when. modbusmq_channel_publish_decide()
                                   // owns these; modbusmq_config_reset_publish_state()
                                   // clears them on an MQTT (re)connect so a
                                   // fresh subscriber is not left waiting on a
                                   // suppressed channel.
                                   //
                                   // last_text is what "unchanged" is actually
                                   // judged against — the printed form, at the
                                   // channel's own decimal count, not the raw
                                   // float. last_value still backs min_change /
                                   // min_change_rel, which are magnitude checks
                                   // rather than text comparisons.
                                   // last_text must hold the longest thing a
                                   // channel can publish, not just a number:
                                   // an ASCII serial number is routinely 32
                                   // characters, and truncating it here would
                                   // make two different devices compare equal
                                   // and suppress the publish that says so.
    float                   last_value;
    char                    last_text[MODBUSMQ_TEXT_MAX];
    millitime_t             last_publish_ms;
    uint8_t                 published;
} modbusmq_channel_t;
    
//
// Info about one input subscription
//
typedef struct modbusmq_input_t
{
    int                 slave;
    int                 type;
    int                 address;
    int                 address_offset;
    int                 naddress;
    int                 interval;
    int                 channel_max;
    struct modbusmq_channel_t *channels;

                                   // Per-input defaults for the publish rate
                                   // limiting keys above. Copied into any
                                   // channel that does not set its own by
                                   // modbusmq_config_apply_publish_defaults();
                                   // nothing outside config parsing reads
                                   // these directly.
    uint8_t             on_change;
    float               min_change;
    float               min_change_rel;
    int                 min_interval;
    int                 max_interval;
    uint8_t             has_on_change;
    uint8_t             has_min_change;
    uint8_t             has_min_change_rel;
    uint8_t             has_min_interval;
    uint8_t             has_max_interval;
} modbusmq_input_t;

//
// One field inside a block write.
//
// The mirror of modbusmq_channel_t, kept as its own type rather than reused:
// a channel carries publish policy and publish state, and a write has neither.
// What it does carry that an input channel does not is a default value, which
// is what makes the trigger form of a block write possible.
//
// offset is in bytes from the start of the block, counted the same way an
// input channel counts them, so a register at block offset 2 is byte 4.
//
typedef struct modbusmq_write_channel_t
{
    char               *name;    // for the log and the nack detail; optional
    int                 offset;  // bytes into the block
    int                 format;
    int                 length;  // bytes, derived from format

                                 // Scaling, undone on the way out exactly as
                                 // modbusmq_write_t does it.
    int                 add;
    int                 mod;
    int                 mul;

                                 // The value written when the payload is a
                                 // trigger rather than a list. This is the
                                 // "initial settings" case: the numbers live
                                 // in the config, and publishing anything at
                                 // all to the topic sends them.
    double              value;
    uint8_t             has_value;
} modbusmq_write_channel_t;

//
// One MQTT topic that writes to the device.
//
// The mirror image of an input: instead of polling a register and publishing
// what came back, this subscribes to a topic and writes what arrives. It
// carries its own slave and address rather than hanging off an input, because
// devices almost never put the setpoint in the register you read the value
// from — and a writable register on a device nobody polls should not need a
// dummy input invented for it.
//
// Direction is settled by which section an entry lives in, so a write topic is
// never a topic we also publish to.
//
typedef struct modbusmq_write_t
{
    char               *name;    // for the log; optional
    int                 slave;
    uint8_t             has_slave;

    int                 type;     // modbusmq_type_coil or _HoldingRegister
    int                 function; // modbusmq_write_function_e
    int                 address;
    uint8_t             has_address;

    int                 format;   // register writes only
    int                 length;   // bytes, derived from format

                                  // Scaling, undone on the way out: the exact
                                  // reverse of a channel's, add included, which
                                  // lands before the multiply and not after.
    int                 add;
    int                 mod;
    int                 mul;

                                  // Optional discrete mapping. When set, the
                                  // incoming payload is matched against these
                                  // instead of being scaled — which is how an
                                  // on/off command works whether the device
                                  // takes it as a coil or as a register value.
    int                 on_value;
    int                 off_value;
    uint8_t             has_on_value;
    uint8_t             has_off_value;

    char               *topic;    // subscribed, never published

                                  // Block write (function 16 over more than
                                  // one value). naddress is the block length
                                  // in registers and channels describes what
                                  // sits where inside it. Both unset is the
                                  // single-value shape every config before
                                  // 2.5.0 used, and that shape still works
                                  // unchanged: format/add/mod/mul on the entry
                                  // itself, one value per message.
    int                 naddress;
    uint8_t             has_naddress;
    int                 channel_max;
    modbusmq_write_channel_t *channels;

                                  // Every channel carries a value, so the
                                  // block can be written from the config
                                  // alone. Worked out once by the validator:
                                  // the runtime should not have to walk the
                                  // channels to find out whether a trigger
                                  // payload is answerable.
    uint8_t             has_defaults;

                                  // Publish an ack or a nack for this write.
                                  // Unset takes mqtt.ack, which is off — an
                                  // existing deployment gains no new traffic
                                  // by upgrading. ack_topic defaults to the
                                  // write topic with "/ack" appended, after
                                  // mqtt.topic_prefix has been applied.
    int                 ack;
    uint8_t             has_ack;
    char               *ack_topic;
} modbusmq_write_t;
    

//
// master config
//    
typedef struct modbusmq_config_t
{
    char               *config_name;
    char               *config_version;

    char               *modbusmq_connect;
    int                 modbusmq_baudrate;
    char                modbusmq_parity;
    int                 modbusmq_stopbit;
    int                 modbusmq_databit;
    int                 modbusmq_rts_delay_us;
    int                 modbusmq_frame_timeout_ms;
    int                 modbusmq_slave;
    
    int                 input_max;
    int                 offset_size;
    int                 query_mode;
    modbusmq_input_t     *inputs;

    int                 write_max;
    modbusmq_write_t    *writes;

    char               *mqtt_name;
    char               *mqtt_connect;
    char               *mqtt_topic_prefix;
    int                 mqtt_retain; // default for every published channel
    int                 mqtt_qos;
                                   // Config-wide default for write.N.ack. Off,
                                   // so acks are something a config asks for.
    int                 mqtt_ack;

                                   // Config-wide defaults for the publish
                                   // rate-limiting keys, the bottom rung under
                                   // input and channel: a channel takes its
                                   // own value, else its input's, else this
                                   // one. See modbusmq_config_apply_publish_defaults().
    uint8_t             publish_on_change;
    float               publish_min_change;
    float               publish_min_change_rel;
    int                 publish_min_interval;
    int                 publish_max_interval;
    uint8_t             has_publish_on_change;
    uint8_t             has_publish_min_change;
    uint8_t             has_publish_min_change_rel;
    uint8_t             has_publish_min_interval;
    uint8_t             has_publish_max_interval;
} modbusmq_config_t;

    
extern int                 modbusmq_config_parse(const char *filename);
extern modbusmq_config_t * modbusmq_config_get();

//
// Command line overrides for config keys.
//
// Each entry is a "key=value" string using exactly the same key names the
// config file uses. While the file is parsed, every key found in it is looked
// up here first, and a match substitutes the override's value for the file's.
// That is the whole mechanism: an override edits a line the file already has,
// it does not add one the file is missing. So
//
//     modbusmq -c tcp_device.config -e modbusmq.connect=rtu:///dev/ttyUSB0:9600:1:8:N
//
// runs a TCP config over RTU, because modbusmq.connect is a line that config
// already carries.
//
// Set these before modbusmq_config_parse(); the list is not copied, so it must
// outlive the parse. modbusmq_config_override_unmatched() warns about entries
// that never matched a key — usually a typo, or a key the file does not set —
// and returns how many. It is advisory; what to do about it is the caller's.
//
extern void                modbusmq_config_set_overrides(char *const *overrides, int noverrides);
extern int                 modbusmq_config_override_unmatched(void);

//
// Clear every channel's publish state (published/last_value/last_publish_ms).
//
// Call this whenever the MQTT connection is (re)established. A broker restart
// forgets what we last sent it, so a channel gated by min_change would
// otherwise stay silent until its value happens to move again — this makes
// every channel look unpublished so the next poll of each one goes out fresh.
//
extern void                modbusmq_config_reset_publish_state(modbusmq_config_t *config);

// utility functions
// 
extern int modbusmq_config_input_type(const char *value);
    // format name -> enum modbusmq_data_format_e, _unknown (and logged) when unrecognised
extern int modbusmq_config_dataformat(const char *value);
    // ...and back, for messages: enum -> the name a config file spells it with
extern const char *modbusmq_config_dataformat_name(int format);
extern int modbusmq_config_write_function(const char *value);

//
// Coils and discrete inputs carry one bit per address; registers carry two
// bytes. Enough of the code has to branch on that distinction to be worth
// naming it once.
//
#define MODBUSMQ_TYPE_IS_BIT(t) ((t) == modbusmq_type_coil || (t) == modbusmq_type_discrete_input)

#ifdef __cplusplus
}
#endif

#endif // modbusmq_config_h_
