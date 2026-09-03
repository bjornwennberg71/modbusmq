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

#ifdef __cplusplus
extern "C" {
#endif

//
// The values are the Modbus read function codes, so an input's type is also
// the function used to poll it.
//
enum ModbusmqType_e
{
    ModbusmqType_Coil            = 0x01,
    ModbusmqType_DiscreteInput   = 0x02,
    ModbusmqType_HoldingRegister = 0x03,
    ModbusmqType_InputRegister   = 0x04
};

#define MODBUSMQ_TYPE_COIL             "coil"
#define MODBUSMQ_TYPE_DISCRETE_INPUT   "discrete_input"
#define MODBUSMQ_TYPE_INPUT_REGISTER   "input_register"
#define MODBUSMQ_TYPE_HOLDING_REGISTER "holding_register"

//
// Which Modbus function a write.N entry uses. The values are the function
// codes themselves, the same way ModbusmqType_e carries 0x03/0x04.
//
// Only single-coil (05) and single/multiple register (06/16) writes are
// offered. Function 15, write multiple coils, is deliberately absent: one
// write entry addresses one thing, so there is never more than one coil to
// set, and 15 buys nothing but a bit-packing order to get wrong.
//
enum ModbusmqWriteFunction_e
{
    ModbusmqWriteFunction_Unknown   = 0x00,
    ModbusmqWriteFunction_Coil      = 0x05,
    ModbusmqWriteFunction_Register  = 0x06,
    ModbusmqWriteFunction_Registers = 0x10
};

#define MODBUSMQ_FUNCTION_WRITE_COIL      "write_coil"
#define MODBUSMQ_FUNCTION_WRITE_REGISTER  "write_register"
#define MODBUSMQ_FUNCTION_WRITE_REGISTERS "write_registers"
    
enum ModbusmqDataFormat
{
    ModbusmqDataFormat_unknown = 0,
    ModbusmqDataFormat_a,    // uint8_t
    ModbusmqDataFormat_ab,   // uint16_t little endian
    ModbusmqDataFormat_ba,   // uint16_t big endian
    ModbusmqDataFormat_int16_ab, // int16_t, signed, high byte first
    ModbusmqDataFormat_int16_ba, // int16_t, signed, low byte first
    ModbusmqDataFormat_abcd,
    ModbusmqDataFormat_badc,
    ModbusmqDataFormat_float_ba, // float
    ModbusmqDataFormat_float_abcd, // float
    ModbusmqDataFormat_float_badc, // float
    ModbusmqDataFormat_float_dcba, // float
    ModbusmqDataFormat_float_cdab  // float, low word first (common in industrial Modbus devices)

};

#define MODBUSMQ_FORMAT_A          "int_a"
#define MODBUSMQ_FORMAT_AB         "int_ab"
#define MODBUSMQ_FORMAT_BA         "int_ba"
#define MODBUSMQ_FORMAT_INT16_AB   "int16"
#define MODBUSMQ_FORMAT_INT16_BA   "int16_ba"
#define MODBUSMQ_FORMAT_ABCD       "int_abcd"
#define MODBUSMQ_FORMAT_BADC       "int_badc"
#define MODBUSMQ_FORMAT_FLOAT_BA   "float_ba"
#define MODBUSMQ_FORMAT_FLOAT_ABCD "float_abcd"
#define MODBUSMQ_FORMAT_FLOAT_BADC "float_badc"
#define MODBUSMQ_FORMAT_FLOAT_DCBA "float_dcba"
#define MODBUSMQ_FORMAT_FLOAT_CDAB "float_cdab"

typedef enum ModbusmqQueryMode_e
{
    ModbusmqQueryModeMin       = 0,
    ModbusmqQueryModeParallell = 0, // parallell
    ModbusmqQueryModeSeries    = 1,  // series
    ModbusmqQueryModeMax 
} ModbusmqQueryTypeMode_e;

//
// info about one channel
//    
typedef struct modbusmq_channel_t
{
    int                     offset;
    int                     length; // length of value, internally computed
    int                     format;
    int                     add;
    int                     mod;    // -10 = value/10, 100 = value * 100
    int                     mul;    // if mul != 0, value*mul
    char                   *topic;
    float                   value; // used for debug to hold a value
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
} modbusmq_input_t;

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

    int                 type;     // ModbusmqType_Coil or _HoldingRegister
    int                 function; // ModbusmqWriteFunction_e
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
} modbusmq_config_t;

    
extern int                 modbusmq_config_parse(const char *filename);
extern modbusmq_config_t * modbusmq_config_get();

// utility functions
// 
extern int modbusmq_config_input_type(const char *value);
extern int modbusmq_config_write_function(const char *value);

//
// Coils and discrete inputs carry one bit per address; registers carry two
// bytes. Enough of the code has to branch on that distinction to be worth
// naming it once.
//
#define MODBUSMQ_TYPE_IS_BIT(t) ((t) == ModbusmqType_Coil || (t) == ModbusmqType_DiscreteInput)

#ifdef __cplusplus
}
#endif

#endif // modbusmq_config_h_
