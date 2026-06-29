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

#ifdef __cplusplus
extern "C" {
#endif

enum ModbusmqType_e
{
    ModbusmqType_HoldingRegister = 0x03,
    ModbusmqType_InputRegister   = 0x04
};

#define MODBUSMQ_TYPE_INPUT_REGISTER   "input_register"
#define MODBUSMQ_TYPE_HOLDING_REGISTER "holding_register"
    
enum ModbusmqDataFormat
{
    ModbusmqDataFormat_unknown = 0,
    ModbusmqDataFormat_a,    // uint8_t
    ModbusmqDataFormat_ab,   // uint16_t little endian
    ModbusmqDataFormat_ba,   // uint16_t big endian
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

    char               *mqtt_name;
    char               *mqtt_connect;
    char               *mqtt_topic_prefix;
} modbusmq_config_t;

    
extern int                 modbusmq_config_parse(const char *filename);
extern const char *        modbusmq_config_find(const char *key);
extern modbusmq_config_t * modbusmq_config_get();

// utility functions
// 
extern int modbusmq_config_input_type(const char *value);

#ifdef __cplusplus
}
#endif

#endif // modbusmq_config_h_
