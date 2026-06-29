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
    if (strcmp(value, MODBUSMQ_FORMAT_A) == 0)
    {
        return ModbusmqDataFormat_a;
    }
    else if (strcmp(value, MODBUSMQ_FORMAT_AB) == 0)
    {
        return ModbusmqDataFormat_ab;
    }
    else if (strcmp(value, MODBUSMQ_FORMAT_BA) == 0)
    {
        return ModbusmqDataFormat_ba;
    }
    else if (strcmp(value, MODBUSMQ_FORMAT_ABCD) == 0)
    {
        return ModbusmqDataFormat_abcd;
    }
    else if (strcmp(value, MODBUSMQ_FORMAT_BADC) == 0)
    {
        return ModbusmqDataFormat_badc;
    }
    else if (strcmp(value, MODBUSMQ_FORMAT_FLOAT_BA) == 0)
    {
        return ModbusmqDataFormat_float_ba;
    }
    else if (strcmp(value, MODBUSMQ_FORMAT_FLOAT_ABCD) == 0)
    {
        return ModbusmqDataFormat_float_abcd;
    }
    else if (strcmp(value, MODBUSMQ_FORMAT_FLOAT_BADC) == 0)
    {
        return ModbusmqDataFormat_float_badc;
    }
    else if (strcmp(value, MODBUSMQ_FORMAT_FLOAT_DCBA) == 0)
    {
        return ModbusmqDataFormat_float_dcba;
    }
    else if (strcmp(value, MODBUSMQ_FORMAT_FLOAT_CDAB) == 0)
    {
        return ModbusmqDataFormat_float_cdab;
    }

    modbusmq_logf(LOG_ERROR, "Unsupported format: %s\n", value);
    assert(0);
    return 0;
}

int modbusmq_config_query_mode(const char *key)
{
    if (strcmp(key, "parallell") == 0)
    {
        return ModbusmqQueryModeParallell;
    }
    else if (strcmp(key, "series") == 0)
    {
        return ModbusmqQueryModeSeries;
    }

    // you can also use numbers instead of text
    //
    int
        value = strtod(key, NULL);
    if (value >= ModbusmqQueryModeMin || value <= ModbusmqQueryModeMax)
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
        return ModbusmqType_InputRegister;
    }
    else if (strcmp(value, MODBUSMQ_TYPE_HOLDING_REGISTER) == 0)
    {
        return ModbusmqType_HoldingRegister;
    }
    return 0;
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
        klen,
        vlen,
        line_num = 0;

    if (!modbusmq_config)
    {
        modbusmq_config = malloc(sizeof(modbusmq_config_t));
        if (!modbusmq_config)
        {
            perror("Unable to allocate");
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
            modbusmq_config->config_name = strdup(value);
        }
        else if (strcmp(key, "config.version") == 0)
        {
            modbusmq_config->config_version = strdup(value);
        }
        else if (strcmp(key, "modbusmq.connect") == 0)
        {
            modbusmq_config->modbusmq_connect = strdup(value);
        }
        else if (strcmp(key, "modbusmq.baudrate") == 0)
        {
            modbusmq_config->modbusmq_baudrate = strtod(value, NULL);
        }
        else if (strcmp(key, "modbusmq.stopbit") == 0)
        {
            modbusmq_config->modbusmq_stopbit = strtod(value, NULL);
        }
        else if (strcmp(key, "modbusmq.databit") == 0)
        {
            modbusmq_config->modbusmq_databit = strtod(value, NULL);
        }
        else if (strcmp(key, "modbusmq.parity") == 0)
        {
            modbusmq_config->modbusmq_parity = value[0];
        }
        else if (strcmp(key, "modbusmq.rts_delay") == 0)
        {
            modbusmq_config->modbusmq_rts_delay_us = strtod(value, NULL);
        }
        else if (strcmp(key, "modbusmq.frame_timeout") == 0)
        {
            modbusmq_config->modbusmq_frame_timeout_ms = strtod(value, NULL);
        }
        else if (strcmp(key, "input.offset_size") == 0)
        {
            modbusmq_config->offset_size = strtod(value, NULL);
        }
        else if (strcmp(key, "input.query_mode") == 0)
        {
            modbusmq_config->query_mode = modbusmq_config_query_mode(value);
            if (modbusmq_config->query_mode < 0)
            {
                fprintf(stderr, "%d: invalid query_mode: %s, setting query_type=parallell\n", line_num, key);
                modbusmq_config->query_mode = ModbusmqQueryModeParallell;
            }
            if (modbusmq_config->query_mode < ModbusmqQueryModeMin || modbusmq_config->query_mode >= ModbusmqQueryModeMax)
            {
                modbusmq_config->query_mode = ModbusmqQueryModeParallell;
            }
        }
        else if (strcmp(key, "input.max") == 0)
        {
            if (modbusmq_config->inputs)
            {
                fprintf(stderr, "%d: %s listed more than once\n", line_num, key);
                return -1;
            }
            int
                nvalue = (int)strtoul(value, NULL, 0);
            if (nvalue < 0 || nvalue > 100)
            {
                fprintf(stderr, "%d: %s must be between [0..100]\n", line_num, key);
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
            char
                buf[100];
            
            int
                n = sscanf(key, "input.%d.%s", &input_num, input_key);
            if (n != 2)
            {
                fprintf(stderr, "%d: %s: Unable to scan key\n", line_num, key);
                continue;
            }

            if (input_num <= 0 || input_num > modbusmq_config->input_max)
            {
                fprintf(stderr, "%d: %s, input=%d must be between 1..%d\n", line_num, key, input_num, modbusmq_config->input_max);
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
                    fprintf(stderr, "%d: %s listed more than once\n", line_num, key);
                    return -1;
                }
                int
                    nvalue = (int)strtoul(value, NULL, 0);
                if (nvalue <= 0 || nvalue > 100)
                {
                    fprintf(stderr, "%d: %s must be between [1..100]\n", line_num, key);
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
                char
                    buf[100];
            
                int
                    n = sscanf(input_key, "channel.%d.%s", &channel_num, channel_key);
                if (n != 2)
                {
                    fprintf(stderr, "%d: %s: Unable to scan key\n", line_num, key);
                    continue;
                }

                if (channel_num <= 0 || channel_num > input->channel_max)
                {
                    fprintf(stderr, "%d: %s, channel.{%d} must be between 1..%d\n", line_num, key, channel_num, input->channel_max);
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
                    switch(channel->format)
                    {
                        case ModbusmqDataFormat_a: channel->length = 1; break;
                        case ModbusmqDataFormat_ab: 
                        case ModbusmqDataFormat_ba:
                        case ModbusmqDataFormat_float_ba:
                        channel->length = 2; break;
                        case ModbusmqDataFormat_abcd:
                        case ModbusmqDataFormat_badc:
                        case ModbusmqDataFormat_float_abcd:
                        case ModbusmqDataFormat_float_badc:
                        case ModbusmqDataFormat_float_dcba:
                        case ModbusmqDataFormat_float_cdab:
                        channel->length = 4; break;
                        default:
                        channel->length = 2; break; // TODO: write warning of unknown length
                    }
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
                    channel->topic = strdup(value);
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
        else if (strcmp(key, "mqtt.name") == 0)
        {
            modbusmq_config->mqtt_name = strdup(value);
        }
        else if (strcmp(key, "mqtt.connect") == 0)
        {
            modbusmq_config->mqtt_connect = strdup(value);
        }
        else if (strcmp(key, "mqtt.topic_prefix") == 0)
        {
            modbusmq_config->mqtt_topic_prefix = strdup(value);
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
    if (modbusmq_config->mqtt_name) { free(modbusmq_config->mqtt_name); }
    if (modbusmq_config->mqtt_connect) { free(modbusmq_config->mqtt_connect); }
    if (modbusmq_config->mqtt_topic_prefix) { free(modbusmq_config->mqtt_topic_prefix); }
    
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
