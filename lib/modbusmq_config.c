//////////////////////////////////////////////////////////////////////////////
// 
// bjornwennberg71@gmail.com
// 
// modbusmq_config.c
// 

// INCLUDES //////////////////////////////////////////////////////////////////
#include "modbusmq_config.h"
#include "modbusmq_internal.h"
#include "modbusmq_log.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h> // debug
#include <assert.h> // debug

static modbusmq_config_t *modbusmq_config = NULL;

/**
* @brief Find input value. trim whitespace before and after.
* 
* @param **value: input/output value
* @return length of value
*/ 
//////////////////////////////////////////////////////////////////////////////
// 
// input:  [   some value is here    # and a comment    ]
// output: [some value is here]
// return length
int
config_handle_value(char **value)
{
    char
        *ptr = *value;
    int
        nlen;

    if (!ptr)
    {
        return 0;
    }

    // eat whitespace / find comment from start of line
    while(ptr && *ptr)
    {
        if (*ptr == ' ' || *ptr == '\t')
        {
            // eat whitespace
            (*value)++; // increase start of value
            ptr = *value;
        }
        else if (*ptr == '#')
        {
            // terminate string at comment
            *ptr = 0;
            break;
        }
        else
        {
            break; // non-space character, we are done
        }
    }

    // eat whitespace at end
    if (ptr)
    {
        nlen = strlen(ptr);
        if (nlen > 0)
        {
            ptr = *value + nlen -1;
            
            while(ptr && *ptr && ptr > *value && (*ptr == ' ' || *ptr == '\t'))
            {
                *ptr-- = 0; nlen--;
            }
        }
    }
    return nlen;
}

//////////////////////////////////////////////////////////////////////////////
// 
//
//////////////////////////////////////////////////////////////////////////////
//
// Replace a config string, keeping the last value seen.
//
// Repeating a key is deliberate and supported: it lets a config override an
// earlier value further down the file. Only the old string needs releasing on
// the way past, which is what this is for.
//
static void
config_set_string(char **dst, const char *value)
{
    if (!dst)
    {
        return;
    }

    free(*dst);          // NULL on the first assignment, which free() accepts
    *dst = strdup(value);
}

void
modbusmq_config_debug_print(const char *key, const char *value, int line_num)
{
    printf("%3d: [%s]=[%s]\n", line_num, key, value);
}


//////////////////////////////////////////////////////////////////////////////
// 
// 
int
modbusmq_config_dataformat(const char *value)
{
    //
    // int_* is parsed as signed here, which is the config.version 2.0 meaning.
    // A config declaring an older version has these mapped back to unsigned
    // afterwards by modbusmq_config_apply_format_version(), once the whole
    // file has been read and the version is actually known — config.version
    // is not required to appear before the keys it governs.
    //
    if (strcmp(value, MODBUSMQ_FORMAT_A) == 0)
    {
        return modbusmq_data_format_int8;
    }
    else if (strcmp(value, MODBUSMQ_FORMAT_AB) == 0)
    {
        return modbusmq_data_format_int16_ab;
    }
    else if (strcmp(value, MODBUSMQ_FORMAT_BA) == 0)
    {
        return modbusmq_data_format_int16_ba;
    }
    else if (strcmp(value, MODBUSMQ_FORMAT_ABCD) == 0)
    {
        return modbusmq_data_format_abcd;
    }
    else if (strcmp(value, MODBUSMQ_FORMAT_BADC) == 0)
    {
        return modbusmq_data_format_badc;
    }
    else if (strcmp(value, MODBUSMQ_FORMAT_UA) == 0)
    {
        return modbusmq_data_format_a;
    }
    else if (strcmp(value, MODBUSMQ_FORMAT_UAB) == 0)
    {
        return modbusmq_data_format_ab;
    }
    else if (strcmp(value, MODBUSMQ_FORMAT_UBA) == 0)
    {
        return modbusmq_data_format_ba;
    }
    else if (strcmp(value, MODBUSMQ_FORMAT_UABCD) == 0)
    {
        return modbusmq_data_format_uint32_abcd;
    }
    else if (strcmp(value, MODBUSMQ_FORMAT_UBADC) == 0)
    {
        return modbusmq_data_format_uint32_badc;
    }
    else if (strcmp(value, MODBUSMQ_FORMAT_FLOAT_BA) == 0)
    {
        return modbusmq_data_format_float_ba;
    }
    else if (strcmp(value, MODBUSMQ_FORMAT_FLOAT_ABCD) == 0)
    {
        return modbusmq_data_format_float_abcd;
    }
    else if (strcmp(value, MODBUSMQ_FORMAT_FLOAT_BADC) == 0)
    {
        return modbusmq_data_format_float_badc;
    }
    else if (strcmp(value, MODBUSMQ_FORMAT_FLOAT_DCBA) == 0)
    {
        return modbusmq_data_format_float_dcba;
    }
    else if (strcmp(value, MODBUSMQ_FORMAT_FLOAT_CDAB) == 0)
    {
        return modbusmq_data_format_float_cdab;
    }

    //
    // No assert here. A typo in a config file is the operator's mistake to
    // see and fix, not grounds for aborting the process — and with NDEBUG set
    // the assert vanished and left format 0 to be discovered much later.
    //
    modbusmq_logf(LOG_ERROR, "Unsupported format: %s\n", value);
    return modbusmq_data_format_unknown;
}

//////////////////////////////////////////////////////////////////////////////
//
// Apply the config.version meaning of int_a / int_ab / int_ba.
//
// Before version 2.0 those names meant *unsigned*. Everything is parsed as
// signed, so this walks back the ones belonging to an older config and says
// so — once, with a count, rather than per channel.
//
// The point is that no already-deployed config ever changes meaning because
// the library was upgraded. It keeps behaving exactly as it did and is told
// how to opt in.
//
static void
modbusmq_config_apply_format_version(modbusmq_config_t *config, const char *filename)
{
    double
        version = config->config_version ? strtod(config->config_version, NULL) : 0.0;

    if (version >= MODBUSMQ_SIGNED_INT_VERSION)
    {
        return; // int_* is signed, which is how it was already parsed
    }

    int
        changed = 0;

    for(int i = 0; i < config->input_max; ++i)
    {
        modbusmq_input_t
            *input = &config->inputs[i];

        for(int c = 0; c < input->channel_max; ++c)
        {
            modbusmq_channel_t
                *channel = &input->channels[c];

            switch (channel->format)
            {
            case modbusmq_data_format_int8:     channel->format = modbusmq_data_format_a;  changed++; break;
            case modbusmq_data_format_int16_ab: channel->format = modbusmq_data_format_ab; changed++; break;
            case modbusmq_data_format_int16_ba: channel->format = modbusmq_data_format_ba; changed++; break;
            default: break;
            }
        }
    }

    for(int w = 0; w < config->write_max; ++w)
    {
        modbusmq_write_t
            *write = &config->writes[w];

        switch (write->format)
        {
        case modbusmq_data_format_int8:     write->format = modbusmq_data_format_a;  changed++; break;
        case modbusmq_data_format_int16_ab: write->format = modbusmq_data_format_ab; changed++; break;
        case modbusmq_data_format_int16_ba: write->format = modbusmq_data_format_ba; changed++; break;
        default: break;
        }
    }

    if (changed > 0)
    {
        modbusmq_logf(LOG_ERROR,
                      "%s: config.version is %s, so %d channel(s) using int_a/int_ab/int_ba keep the old "
                      "UNSIGNED meaning. A negative reading will publish as a large positive number. "
                      "To fix: set config.version = 2.0 and spell the genuinely unsigned ones uint_ab/uint_ba.\n",
                      filename, config->config_version ? config->config_version : "unset", changed);
    }
}

int modbusmq_config_query_mode(const char *key)
{
    if (strcmp(key, "parallell") == 0)
    {
        return modbusmq_query_mode_parallell;
    }
    else if (strcmp(key, "series") == 0)
    {
        return modbusmq_query_mode_series;
    }

    // you can also use numbers instead of text
    //
    int
        value = strtol(key, NULL, 0);
    if (value >= modbusmq_query_mode_min && value <= modbusmq_query_mode_max)
    {
        return value;
    }
    
    return -1;
}

//////////////////////////////////////////////////////////////////////////////
// 
// 
int
modbusmq_config_input_type(const char *value)
{
    if (strcmp(value, MODBUSMQ_TYPE_INPUT_REGISTER) == 0)
    {
        return modbusmq_type_input_register;
    }
    else if (strcmp(value, MODBUSMQ_TYPE_HOLDING_REGISTER) == 0)
    {
        return modbusmq_type_holding_register;
    }
    else if (strcmp(value, MODBUSMQ_TYPE_COIL) == 0)
    {
        return modbusmq_type_coil;
    }
    else if (strcmp(value, MODBUSMQ_TYPE_DISCRETE_INPUT) == 0)
    {
        return modbusmq_type_discrete_input;
    }
    return 0;
}

//////////////////////////////////////////////////////////////////////////////
// 
// write function name -> modbusmq_write_function_e, _Unknown when unrecognised
int
modbusmq_config_write_function(const char *value)
{
    if (strcmp(value, MODBUSMQ_FUNCTION_WRITE_COIL) == 0)
    {
        return modbusmq_write_function_coil;
    }
    else if (strcmp(value, MODBUSMQ_FUNCTION_WRITE_REGISTER) == 0)
    {
        return modbusmq_write_function_register;
    }
    else if (strcmp(value, MODBUSMQ_FUNCTION_WRITE_REGISTERS) == 0)
    {
        return modbusmq_write_function_registers;
    }
    return modbusmq_write_function_unknown;
}


//////////////////////////////////////////////////////////////////////////////
// 
// 
int
modbusmq_config_parse(const char *filename)
{
    FILE
        *fp = fopen(filename, "r");
    if (!fp)
    {
        fprintf(stderr, "Unable to open config-file: %s\n", filename);
        return -1;
    }

    char
        *line = 0,
        *key,
        *value;
    ssize_t
        nread;
    size_t
        line_len;
    int
        line_num = 0;

    if (!modbusmq_config)
    {
        modbusmq_config = malloc(sizeof(modbusmq_config_t));
        if (!modbusmq_config)
        {
            perror("Unable to allocate");
            fclose(fp);
            free(line);
            return -1;
        }
        memset(modbusmq_config, 0, sizeof(modbusmq_config_t));
    }

    modbusmq_config->modbusmq_rts_delay_us    = 10000; // default 10 ms wait before send
    modbusmq_config->modbusmq_frame_timeout_ms = 1000; // maximum 1000 ms waiting for a frame before giving up
    modbusmq_config->offset_size = 1; // default 1 byte offset calculation
    
    while ((nread = getline(&line, &line_len, fp)) != -1)
    {
        line_num++;

        // comment
        // find '#' and remove rest of this line
        key = line;
        while(key && *key && *key != '#' && *key != '\n' && *key != '\r')
        {
            key++;
        }
        if (key && (*key == '#' || *key == '\n' || *key == '\r'))
        {
            *key = 0;
        }

        // same as strtok(line, "=")
        key = strchr(line, '=');
        if (key && *key == '=')
        {
            value = key + 1;
            *key = 0;
            key = line;
        }
        else
        {
            continue;
        }
        
        config_handle_value(&key);
        config_handle_value(&value);

        if (!key || !key[0] || !value || !value[0])
        {
            continue;
        }

        if (strcmp(key, "config.name") == 0)
        {
            config_set_string(&modbusmq_config->config_name, value);
        }
        else if (strcmp(key, "config.version") == 0)
        {
            config_set_string(&modbusmq_config->config_version, value);
        }
        else if (strcmp(key, "modbusmq.connect") == 0)
        {
            config_set_string(&modbusmq_config->modbusmq_connect, value);
        }
        else if (strcmp(key, "modbusmq.baudrate") == 0)
        {
            modbusmq_config->modbusmq_baudrate = strtol(value, NULL, 0);
        }
        else if (strcmp(key, "modbusmq.stopbit") == 0)
        {
            modbusmq_config->modbusmq_stopbit = strtol(value, NULL, 0);
        }
        else if (strcmp(key, "modbusmq.databit") == 0)
        {
            modbusmq_config->modbusmq_databit = strtol(value, NULL, 0);
        }
        else if (strcmp(key, "modbusmq.parity") == 0)
        {
            modbusmq_config->modbusmq_parity = value[0];
        }
        else if (strcmp(key, "modbusmq.rts_delay") == 0)
        {
            modbusmq_config->modbusmq_rts_delay_us = strtol(value, NULL, 0);
        }
        else if (strcmp(key, "modbusmq.frame_timeout") == 0)
        {
            modbusmq_config->modbusmq_frame_timeout_ms = strtol(value, NULL, 0);
        }
        else if (strcmp(key, "input.offset_size") == 0)
        {
            modbusmq_config->offset_size = strtol(value, NULL, 0);
        }
        else if (strcmp(key, "input.query_mode") == 0)
        {
            modbusmq_config->query_mode = modbusmq_config_query_mode(value);
            if (modbusmq_config->query_mode < 0)
            {
                fprintf(stderr, "%d: invalid query_mode: %s, setting query_type=parallell\n", line_num, key);
                modbusmq_config->query_mode = modbusmq_query_mode_parallell;
            }
            if (modbusmq_config->query_mode < modbusmq_query_mode_min || modbusmq_config->query_mode >= modbusmq_query_mode_max)
            {
                modbusmq_config->query_mode = modbusmq_query_mode_parallell;
            }
        }
        else if (strcmp(key, "input.max") == 0)
        {
            if (modbusmq_config->inputs)
            {
                fprintf(stderr, "%d: %s listed more than once. The array is allocated when this key is read, "
                                "so a second one would discard everything already parsed into the first.\n", line_num, key);
                fclose(fp);
                free(line);
                return -1;
            }
            int
                nvalue = (int)strtoul(value, NULL, 0);
            if (nvalue < 0 || nvalue > 100)
            {
                fprintf(stderr, "%d: %s must be between [0..100]\n", line_num, key);
                fclose(fp);
                free(line);
                return -1;
            }
            if (nvalue > 0)
            {
                modbusmq_config->input_max = nvalue;
                modbusmq_config->inputs      = malloc(nvalue * sizeof(modbusmq_input_t));
                if (!modbusmq_config->inputs)
                {
                    perror("Unable to allocate");
                    exit(2);
                }
                memset(modbusmq_config->inputs, 0, nvalue * sizeof(modbusmq_input_t));
            }
        }
        // input.1.something
        // input.2.something
        else if (strncmp(key, "input.", 6) == 0)
        {
            int
                input_num = 0;
            char
                input_key[100];

            int
                n = sscanf(key, "input.%d.%99s", &input_num, input_key);
            if (n != 2)
            {
                fprintf(stderr, "%d: %s: Unable to scan key\n", line_num, key);
                continue;
            }

            if (input_num <= 0 || input_num > modbusmq_config->input_max)
            {
                if (modbusmq_get_debug() > 0)
                {
                    fprintf(stderr, "%d: %s, input=%d must be between 1..%d\n", line_num, key, input_num, modbusmq_config->input_max);
                }
                continue;
            }

            modbusmq_input_t
                *input = &modbusmq_config->inputs[input_num-1];
            
            if (strcmp(input_key, "slave") == 0)
            {
                input->slave = (int)strtoul(value, NULL, 0);
            }
            else if (strcmp(input_key, "type") == 0)
            {
                input->type = modbusmq_config_input_type(value);
                if (input->type == 0)
                {
                    fprintf(stderr, "%d: %s=%s: Unknown input_type\n", line_num, key, value);
                }
            }
            else if (strcmp(input_key, "address") == 0)
            {
                input->address = (int)strtoul(value, NULL, 0);
            }
            else if (strcmp(input_key, "address_offset") == 0)
            {
                input->address_offset = (int)strtoul(value, NULL, 0);
            }
            else if (strcmp(input_key, "naddress") == 0)
            {
                input->naddress = (int)strtoul(value, NULL, 0);
            }
            else if (strcmp(input_key, "interval") == 0)
            {
                input->interval = strtod(value, NULL);
            }
            else if (strcmp(input_key, "channel.max") == 0)
            {
                if (input->channels)
                {
                    fprintf(stderr, "%d: %s listed more than once. The array is allocated when this key is read, "
                                "so a second one would discard everything already parsed into the first.\n", line_num, key);
                    fclose(fp);
                    free(line);
                    return -1;
                }
                int
                    nvalue = (int)strtoul(value, NULL, 0);
                if (nvalue <= 0 || nvalue > 100)
                {
                    fprintf(stderr, "%d: %s must be between [1..100]\n", line_num, key);
                    fclose(fp);
                    free(line);
                    return -1;
                }
                input->channel_max = nvalue;
                input->channels      = malloc(nvalue * sizeof(modbusmq_channel_t));
                if (!input->channels)
                {
                    perror("Unable to allocate");
                    exit(2);
                }
                memset(input->channels, 0, nvalue * sizeof(modbusmq_channel_t));
            }
            else if (strncmp(input_key, "channel.", 8) == 0)
            {
                // input.{}.channel.{}
                int
                    channel_num = 0;
                char
                    channel_key[100];

                int
                    n = sscanf(input_key, "channel.%d.%99s", &channel_num, channel_key);
                if (n != 2)
                {
                    fprintf(stderr, "%d: %s: Unable to scan key\n", line_num, key);
                    continue;
                }

                if (channel_num <= 0 || channel_num > input->channel_max)
                {
                    if (modbusmq_get_debug() > 0)
                    {
                        fprintf(stderr, "%d: %s, channel.{%d} must be between 1..%d\n", line_num, key, channel_num, input->channel_max);
                    }
                    continue;
                }
            
                modbusmq_channel_t
                    *channel = &input->channels[channel_num-1];


                if (strcmp(channel_key, "offset") == 0)
                {
                    channel->offset = (int)strtoul(value, NULL, 0);
                }
                else if (strcmp(channel_key, "format") == 0)
                {
                    channel->format = modbusmq_config_dataformat(value);
                    if (channel->format == modbusmq_data_format_unknown)
                    {
                        fprintf(stderr, "%d: %s=%s: unknown format\n", line_num, key, value);
                        fclose(fp);
                        free(line);
                        return -1;
                    }
                    channel->length = modbusmq_format_size(channel->format);
                }
                else if (strcmp(channel_key, "mod") == 0)
                {
                    channel->mod = strtod(value, NULL);
                }
                else if (strcmp(channel_key, "mul") == 0)
                {
                    channel->mul = strtod(value, NULL);
                }
                else if (strcmp(channel_key, "add") == 0)
                {
                    channel->add = strtod(value, NULL);
                }
                else if (strcmp(channel_key, "topic") == 0)
                {
                    config_set_string(&channel->topic, value);
                }
                else if (strcmp(channel_key, "value") == 0)
                {
                    channel->value = strtod(value, NULL);
                }
            }
            else
            {
                fprintf(stderr, "%d: %s: Unknown configuration\n", line_num, key);
                continue;
            }
            
            //printf("%d: [%d][%s]=%s\n", line_num, channel_num, channel_key, value);
            
            
        }
        else if (strcmp(key, "write.max") == 0)
        {
            if (modbusmq_config->writes)
            {
                fprintf(stderr, "%d: %s listed more than once. The array is allocated when this key is read, "
                                "so a second one would discard everything already parsed into the first.\n", line_num, key);
                fclose(fp);
                free(line);
                return -1;
            }
            int
                nvalue = (int)strtoul(value, NULL, 0);
            if (nvalue < 0 || nvalue > 100)
            {
                fprintf(stderr, "%d: %s must be between [0..100]\n", line_num, key);
                fclose(fp);
                free(line);
                return -1;
            }
            if (nvalue > 0)
            {
                modbusmq_config->write_max = nvalue;
                modbusmq_config->writes    = malloc(nvalue * sizeof(modbusmq_write_t));
                if (!modbusmq_config->writes)
                {
                    perror("Unable to allocate");
                    fclose(fp);
                    free(line);
                    return -1;
                }
                memset(modbusmq_config->writes, 0, nvalue * sizeof(modbusmq_write_t));
            }
        }
        // write.1.something
        else if (strncmp(key, "write.", 6) == 0)
        {
            int
                write_num = 0;
            char
                write_key[100];

            int
                n = sscanf(key, "write.%d.%99s", &write_num, write_key);
            if (n != 2)
            {
                fprintf(stderr, "%d: %s: Unable to scan key\n", line_num, key);
                continue;
            }

            if (write_num <= 0 || write_num > modbusmq_config->write_max)
            {
                fprintf(stderr, "%d: %s, write=%d must be between 1..%d (is write.max set, and set first?)\n",
                        line_num, key, write_num, modbusmq_config->write_max);
                fclose(fp);
                free(line);
                return -1;
            }

            modbusmq_write_t
                *write = &modbusmq_config->writes[write_num-1];

            if (strcmp(write_key, "name") == 0)
            {
                config_set_string(&write->name, value);
            }
            else if (strcmp(write_key, "slave") == 0)
            {
                write->slave     = (int)strtoul(value, NULL, 0);
                write->has_slave = 1;
            }
            else if (strcmp(write_key, "type") == 0)
            {
                write->type = modbusmq_config_input_type(value);
                if (write->type == 0)
                {
                    fprintf(stderr, "%d: %s=%s: expected %s or %s\n",
                            line_num, key, value, MODBUSMQ_TYPE_COIL, MODBUSMQ_TYPE_HOLDING_REGISTER);
                    fclose(fp);
                    free(line);
                    return -1;
                }
                //
                // A discrete input is read-only by definition, and an input
                // register has no write function at all.
                //
                if (write->type != modbusmq_type_coil && write->type != modbusmq_type_holding_register)
                {
                    fprintf(stderr, "%d: %s=%s: not writable, expected %s or %s\n",
                            line_num, key, value, MODBUSMQ_TYPE_COIL, MODBUSMQ_TYPE_HOLDING_REGISTER);
                    fclose(fp);
                    free(line);
                    return -1;
                }
            }
            else if (strcmp(write_key, "function") == 0)
            {
                write->function = modbusmq_config_write_function(value);
                if (write->function == modbusmq_write_function_unknown)
                {
                    fprintf(stderr, "%d: %s=%s: expected %s, %s or %s\n",
                            line_num, key, value,
                            MODBUSMQ_FUNCTION_WRITE_COIL, MODBUSMQ_FUNCTION_WRITE_REGISTER,
                            MODBUSMQ_FUNCTION_WRITE_REGISTERS);
                    fclose(fp);
                    free(line);
                    return -1;
                }
            }
            else if (strcmp(write_key, "address") == 0)
            {
                write->address     = (int)strtoul(value, NULL, 0);
                write->has_address = 1;
            }
            else if (strcmp(write_key, "format") == 0)
            {
                write->format = modbusmq_config_dataformat(value);
                if (write->format == modbusmq_data_format_unknown)
                {
                    fprintf(stderr, "%d: %s=%s: unknown format\n", line_num, key, value);
                    fclose(fp);
                    free(line);
                    return -1;
                }
                write->length = modbusmq_format_size(write->format);
            }
            else if (strcmp(write_key, "add") == 0)
            {
                write->add = strtod(value, NULL);
            }
            else if (strcmp(write_key, "mod") == 0)
            {
                write->mod = strtod(value, NULL);
            }
            else if (strcmp(write_key, "mul") == 0)
            {
                write->mul = strtod(value, NULL);
            }
            else if (strcmp(write_key, "on_value") == 0)
            {
                write->on_value     = (int)strtol(value, NULL, 0);
                write->has_on_value = 1;
            }
            else if (strcmp(write_key, "off_value") == 0)
            {
                write->off_value     = (int)strtol(value, NULL, 0);
                write->has_off_value = 1;
            }
            else if (strcmp(write_key, "topic") == 0)
            {
                config_set_string(&write->topic, value);
            }
            else
            {
                fprintf(stderr, "%d: %s: Unknown configuration\n", line_num, key);
                continue;
            }
        }
        else if (strcmp(key, "mqtt.name") == 0)
        {
            config_set_string(&modbusmq_config->mqtt_name, value);
        }
        else if (strcmp(key, "mqtt.connect") == 0)
        {
            config_set_string(&modbusmq_config->mqtt_connect, value);
        }
        else if (strcmp(key, "mqtt.topic_prefix") == 0)
        {
            config_set_string(&modbusmq_config->mqtt_topic_prefix, value);
        }
        else
        {
            if (strlen(key))
            {
                printf("%d: %s: Unknown config\n", line_num, key);
            }
        }
    }

    fclose(fp);
    free(line);

    //
    // Resolve what int_* means for this config before anything validates or
    // uses a format. config.version may appear anywhere in the file, so this
    // cannot be decided while parsing.
    //
    modbusmq_config_apply_format_version(modbusmq_config, filename);

    //
    // A coil or discrete input carries one bit per address, so its channels
    // need no format — and a format on one is a sign the config was written
    // against the wrong address space. A register channel needs one.
    //
    for(int i = 0; i < modbusmq_config->input_max; ++i)
    {
        modbusmq_input_t
            *input = &modbusmq_config->inputs[i];

        for(int c = 0; c < input->channel_max; ++c)
        {
            modbusmq_channel_t
                *channel = &input->channels[c];

            if (!channel->topic)
            {
                continue; // an unused channel slot
            }

            if (MODBUSMQ_TYPE_IS_BIT(input->type))
            {
                if (channel->format != modbusmq_data_format_unknown)
                {
                    fprintf(stderr, "input.%d.channel.%d: format does not apply to a %s input, offset is a coil index\n",
                            i+1, c+1,
                            input->type == modbusmq_type_coil ? MODBUSMQ_TYPE_COIL : MODBUSMQ_TYPE_DISCRETE_INPUT);
                    return -1;
                }
                continue;
            }

            if (channel->format == modbusmq_data_format_unknown)
            {
                fprintf(stderr, "input.%d.channel.%d: format is required\n", i+1, c+1);
                return -1;
            }
        }
    }

    //
    // Validate the write entries here rather than at the first MQTT message.
    // A misconfigured write is a config error, and discovering it only when
    // someone finally publishes a setpoint is far too late — by then the
    // operator believes the command went through.
    //
    for(int w = 0; w < modbusmq_config->write_max; ++w)
    {
        modbusmq_write_t
            *write = &modbusmq_config->writes[w];
        const char
            *name = write->name ? write->name : "?";

        if (!write->topic || !write->topic[0])
        {
            fprintf(stderr, "write.%d (%s): a topic to subscribe to is required\n", w+1, name);
            return -1;
        }
        if (!write->has_address)
        {
            fprintf(stderr, "write.%d (%s): address is required\n", w+1, name);
            return -1;
        }
        //
        // No default slave. Guessing which device to write to is not a
        // recoverable mistake the way a misread register is.
        //
        if (!write->has_slave)
        {
            fprintf(stderr, "write.%d (%s): slave is required\n", w+1, name);
            return -1;
        }
        if (write->has_on_value != write->has_off_value)
        {
            fprintf(stderr, "write.%d (%s): set both on_value and off_value, or neither\n", w+1, name);
            return -1;
        }

        //
        // type and function are two ways of saying the same thing. Accept
        // either, derive the missing one, and refuse a config that says both
        // and disagrees with itself.
        //
        if (write->type == 0 && write->function == modbusmq_write_function_unknown)
        {
            fprintf(stderr, "write.%d (%s): either type or function is required\n", w+1, name);
            return -1;
        }

        if (write->function == modbusmq_write_function_unknown)
        {
            if (write->type == modbusmq_type_coil)
            {
                write->function = modbusmq_write_function_coil;
            }
            else
            {
                write->function = (write->length == 4) ? modbusmq_write_function_registers
                                                       : modbusmq_write_function_register;
            }
        }
        else if (write->type == 0)
        {
            write->type = (write->function == modbusmq_write_function_coil) ? modbusmq_type_coil
                                                                          : modbusmq_type_holding_register;
        }
        else
        {
            int
                coil_type = (write->type     == modbusmq_type_coil);
            int
                coil_func = (write->function == modbusmq_write_function_coil);

            if (coil_type != coil_func)
            {
                fprintf(stderr, "write.%d (%s): type and function disagree\n", w+1, name);
                return -1;
            }
        }

        if (write->function == modbusmq_write_function_coil)
        {
            //
            // A coil is one bit. Scaling it is always a mistake, so say so
            // instead of quietly ignoring the keys.
            //
            if (write->add || write->mod || write->mul || write->format)
            {
                fprintf(stderr, "write.%d (%s): format/add/mod/mul do not apply to a coil write\n", w+1, name);
                return -1;
            }
            continue;
        }

        if (write->format == modbusmq_data_format_unknown)
        {
            fprintf(stderr, "write.%d (%s): format is required for a register write\n", w+1, name);
            return -1;
        }
        //
        // A register write moves whole registers, so a 1-byte format has no
        // unambiguous meaning: nothing says which half it belongs in.
        //
        if (write->length != 2 && write->length != 4)
        {
            fprintf(stderr, "write.%d (%s): format is not writable, a register write is 2 or 4 bytes\n", w+1, name);
            return -1;
        }
        if (write->length == 4 && write->function == modbusmq_write_function_register)
        {
            fprintf(stderr, "write.%d (%s): a 4-byte format needs %s, function 06 writes one register\n",
                    w+1, name, MODBUSMQ_FUNCTION_WRITE_REGISTERS);
            return -1;
        }
    }

    if (modbusmq_config->mqtt_topic_prefix)
    {
        int
            nprefix = strlen(modbusmq_config->mqtt_topic_prefix);
        
        for(int i = 0; i < modbusmq_config->input_max; ++i)
        {
            modbusmq_input_t
                *input = &modbusmq_config->inputs[i];
            for(int c = 0; c < input->channel_max; ++c)
            {
                modbusmq_channel_t
                    *channel = &input->channels[c];
                if (channel->topic)
                {
                    int
                        ntopic = strlen(channel->topic);
                
                    char *topic = malloc(nprefix + ntopic + 1);
                    strcpy(topic, modbusmq_config->mqtt_topic_prefix);
                    strcat(topic, channel->topic);
                    free(channel->topic);
                    channel->topic = topic;
                }
            }
        }

        //
        // Write topics get the same prefix as published ones, so a config
        // lives under one namespace in both directions.
        //
        for(int w = 0; w < modbusmq_config->write_max; ++w)
        {
            modbusmq_write_t
                *write = &modbusmq_config->writes[w];

            if (!write->topic)
            {
                continue;
            }
            int
                ntopic = strlen(write->topic);

            char *topic = malloc(nprefix + ntopic + 1);
            if (!topic)
            {
                perror("Unable to allocate");
                return -1;
            }
            strcpy(topic, modbusmq_config->mqtt_topic_prefix);
            strcat(topic, write->topic);
            free(write->topic);
            write->topic = topic;
        }
    }


    
    return 0;
}

//////////////////////////////////////////////////////////////////////////////
// 
// clean up config entries
void
modbusmq_config_close()
{
    if (!modbusmq_config)
        return;

    for(int i = 0; i < modbusmq_config->input_max; ++i)
    {
        modbusmq_input_t
            *input = &modbusmq_config->inputs[i];

        for(int c = 0; c < input->channel_max; ++c)
        {
            modbusmq_channel_t
                *channel = &input->channels[c];
            if (channel->topic)
            {
                free(channel->topic);
            }
        }

        free(input->channels);
    }
    for(int w = 0; w < modbusmq_config->write_max; ++w)
    {
        modbusmq_write_t
            *write = &modbusmq_config->writes[w];
        if (write->name)  { free(write->name); }
        if (write->topic) { free(write->topic); }
    }
    free(modbusmq_config->writes);

    if (modbusmq_config->mqtt_name) { free(modbusmq_config->mqtt_name); }
    if (modbusmq_config->mqtt_connect) { free(modbusmq_config->mqtt_connect); }
    if (modbusmq_config->mqtt_topic_prefix) { free(modbusmq_config->mqtt_topic_prefix); }
    if (modbusmq_config->config_name) { free(modbusmq_config->config_name); }
    if (modbusmq_config->config_version) { free(modbusmq_config->config_version); }
    if (modbusmq_config->modbusmq_connect) { free(modbusmq_config->modbusmq_connect); }

    free(modbusmq_config->inputs);
    free(modbusmq_config);

    modbusmq_config = 0;
    
}

//////////////////////////////////////////////////////////////////////////////
// 
// 
modbusmq_config_t *
modbusmq_config_get()
{
    if (!modbusmq_config)
    {
        modbusmq_config = malloc(sizeof(modbusmq_config_t));
        if (!modbusmq_config)
        {
            perror("Unable to allocate");
            return NULL;
        }
        memset(modbusmq_config, 0, sizeof(modbusmq_config_t));
    }
    return modbusmq_config;
}
