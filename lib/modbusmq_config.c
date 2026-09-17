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
#include <strings.h>  // strcasecmp
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
// 1/0, true/false, yes/no. < 0 when it is none of those
static int
config_boolean(const char *value)
{
    if (strcmp(value, "1") == 0 || strcasecmp(value, "true") == 0 || strcasecmp(value, "yes") == 0)
    {
        return 1;
    }
    if (strcmp(value, "0") == 0 || strcasecmp(value, "false") == 0 || strcasecmp(value, "no") == 0)
    {
        return 0;
    }
    return -1;
}

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
    // The text formats. These have no unsigned/signed history to carry, so
    // modbusmq_config_apply_format_version() leaves them alone.
    //
    else if (strcmp(value, MODBUSMQ_FORMAT_ASCII_AB) == 0)
    {
        return modbusmq_data_format_ascii_ab;
    }
    else if (strcmp(value, MODBUSMQ_FORMAT_ASCII_BA) == 0)
    {
        return modbusmq_data_format_ascii_ba;
    }
    else if (strcmp(value, MODBUSMQ_FORMAT_BCD_AB) == 0)
    {
        return modbusmq_data_format_bcd_ab;
    }
    else if (strcmp(value, MODBUSMQ_FORMAT_BCD_BA) == 0)
    {
        return modbusmq_data_format_bcd_ba;
    }
    else if (strcmp(value, MODBUSMQ_FORMAT_VERSION_AB) == 0)
    {
        return modbusmq_data_format_version_ab;
    }
    else if (strcmp(value, MODBUSMQ_FORMAT_VERSION_ABCD) == 0)
    {
        return modbusmq_data_format_version_abcd;
    }
    else if (strcmp(value, MODBUSMQ_FORMAT_VERSION_REGS) == 0)
    {
        return modbusmq_data_format_version_regs;
    }
    else if (strcmp(value, MODBUSMQ_FORMAT_DATE_YMD_ABCD) == 0)
    {
        return modbusmq_data_format_date_ymd_abcd;
    }
    else if (strcmp(value, MODBUSMQ_FORMAT_DATETIME_REGS) == 0)
    {
        return modbusmq_data_format_datetime_regs;
    }
    else if (strcmp(value, MODBUSMQ_FORMAT_EPOCH32_ABCD) == 0)
    {
        return modbusmq_data_format_epoch32_abcd;
    }
    else if (strcmp(value, MODBUSMQ_FORMAT_EPOCH32_BADC) == 0)
    {
        return modbusmq_data_format_epoch32_badc;
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
// The name a format goes by in a config file.
//
// Only for messages. An operator who wrote "ascii_ab" needs to be told about
// ascii_ab, not about format 17 — and the int_*/uint_* pairs share an enum
// value each, so this reports the config.version 2.0 spelling of them.
//
const char *
modbusmq_config_dataformat_name(int format)
{
    switch (format)
    {
    case modbusmq_data_format_int8:          return MODBUSMQ_FORMAT_A;
    case modbusmq_data_format_int16_ab:      return MODBUSMQ_FORMAT_AB;
    case modbusmq_data_format_int16_ba:      return MODBUSMQ_FORMAT_BA;
    case modbusmq_data_format_abcd:          return MODBUSMQ_FORMAT_ABCD;
    case modbusmq_data_format_badc:          return MODBUSMQ_FORMAT_BADC;
    case modbusmq_data_format_a:             return MODBUSMQ_FORMAT_UA;
    case modbusmq_data_format_ab:            return MODBUSMQ_FORMAT_UAB;
    case modbusmq_data_format_ba:            return MODBUSMQ_FORMAT_UBA;
    case modbusmq_data_format_uint32_abcd:   return MODBUSMQ_FORMAT_UABCD;
    case modbusmq_data_format_uint32_badc:   return MODBUSMQ_FORMAT_UBADC;
    case modbusmq_data_format_float_ba:      return MODBUSMQ_FORMAT_FLOAT_BA;
    case modbusmq_data_format_float_abcd:    return MODBUSMQ_FORMAT_FLOAT_ABCD;
    case modbusmq_data_format_float_badc:    return MODBUSMQ_FORMAT_FLOAT_BADC;
    case modbusmq_data_format_float_dcba:    return MODBUSMQ_FORMAT_FLOAT_DCBA;
    case modbusmq_data_format_float_cdab:    return MODBUSMQ_FORMAT_FLOAT_CDAB;
    case modbusmq_data_format_ascii_ab:      return MODBUSMQ_FORMAT_ASCII_AB;
    case modbusmq_data_format_ascii_ba:      return MODBUSMQ_FORMAT_ASCII_BA;
    case modbusmq_data_format_bcd_ab:        return MODBUSMQ_FORMAT_BCD_AB;
    case modbusmq_data_format_bcd_ba:        return MODBUSMQ_FORMAT_BCD_BA;
    case modbusmq_data_format_version_ab:    return MODBUSMQ_FORMAT_VERSION_AB;
    case modbusmq_data_format_version_abcd:  return MODBUSMQ_FORMAT_VERSION_ABCD;
    case modbusmq_data_format_version_regs:  return MODBUSMQ_FORMAT_VERSION_REGS;
    case modbusmq_data_format_date_ymd_abcd: return MODBUSMQ_FORMAT_DATE_YMD_ABCD;
    case modbusmq_data_format_datetime_regs: return MODBUSMQ_FORMAT_DATETIME_REGS;
    case modbusmq_data_format_epoch32_abcd:  return MODBUSMQ_FORMAT_EPOCH32_ABCD;
    case modbusmq_data_format_epoch32_badc:  return MODBUSMQ_FORMAT_EPOCH32_BADC;
    default:                                 return "?";
    }
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

        for(int c = 0; c < write->channel_max; ++c)
        {
            modbusmq_write_channel_t
                *channel = &write->channels[c];

            switch (channel->format)
            {
            case modbusmq_data_format_int8:     channel->format = modbusmq_data_format_a;  changed++; break;
            case modbusmq_data_format_int16_ab: channel->format = modbusmq_data_format_ab; changed++; break;
            case modbusmq_data_format_int16_ba: channel->format = modbusmq_data_format_ba; changed++; break;
            default: break;
            }
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

//////////////////////////////////////////////////////////////////////////////
//
// Resolve publish-rate-limiting inheritance across three levels: a channel
// that does not set one of these itself takes its input's value, and one an
// input does not set either falls back to the config-wide publish.* default.
// Each key is resolved independently — an input opting into min_change does
// not drag min_interval along with it for a channel that only wants that one.
//
// Doing this once here, right after parsing, means the runtime
// (modbusmq_channel_publish_decide()) only ever reads the channel fields and
// never has to reach up through the input or the config to find a default.
//
static void
modbusmq_config_apply_publish_defaults(modbusmq_config_t *config)
{
    for(int i = 0; i < config->input_max; ++i)
    {
        modbusmq_input_t
            *input = &config->inputs[i];

        for(int c = 0; c < input->channel_max; ++c)
        {
            modbusmq_channel_t
                *channel = &input->channels[c];

            if (!channel->has_on_change)
            {
                if (input->has_on_change)
                {
                    channel->on_change = input->on_change;
                }
                else if (config->has_publish_on_change)
                {
                    channel->on_change = config->publish_on_change;
                }
            }

            if (!channel->has_min_change)
            {
                if (input->has_min_change)
                {
                    channel->min_change = input->min_change;
                }
                else if (config->has_publish_min_change)
                {
                    channel->min_change = config->publish_min_change;
                }
            }

            if (!channel->has_min_change_rel)
            {
                if (input->has_min_change_rel)
                {
                    channel->min_change_rel = input->min_change_rel;
                }
                else if (config->has_publish_min_change_rel)
                {
                    channel->min_change_rel = config->publish_min_change_rel;
                }
            }

            if (!channel->has_min_interval)
            {
                if (input->has_min_interval)
                {
                    channel->min_interval = input->min_interval;
                }
                else if (config->has_publish_min_interval)
                {
                    channel->min_interval = config->publish_min_interval;
                }
            }

            if (!channel->has_max_interval)
            {
                if (input->has_max_interval)
                {
                    channel->max_interval = input->max_interval;
                }
                else if (config->has_publish_max_interval)
                {
                    channel->max_interval = config->publish_max_interval;
                }
            }
        }
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
//////////////////////////////////////////////////////////////////////////////
//
// Command line key overrides (-e key=value).
//
// The list is borrowed, not copied: it points straight at argv. matched[] runs
// parallel to it so we can tell afterwards which entries never fired.
//
static char *const *config_overrides      = 0;
static int          config_noverrides     = 0;
static uint8_t     *config_override_hit   = 0;

void
modbusmq_config_set_overrides(char *const *overrides, int noverrides)
{
    free(config_override_hit);
    config_override_hit = 0;

    config_overrides  = overrides;
    config_noverrides = (overrides && noverrides > 0) ? noverrides : 0;

    if (config_noverrides > 0)
    {
        config_override_hit = calloc(config_noverrides, sizeof(uint8_t));
        if (!config_override_hit)
        {
            perror("Unable to allocate");
            config_noverrides = 0;
        }
    }
}

//
// @brief Look up key in the override list.
//
// @return the override value to use instead of the file's, or 0 for no match.
//
static const char *
config_override_lookup(const char *key)
{
    if (!key || config_noverrides <= 0)
    {
        return 0;
    }

    int
        nkey = strlen(key);

    for(int o = 0; o < config_noverrides; ++o)
    {
        const char
            *entry = config_overrides[o];
        if (!entry)
        {
            continue;
        }

        // match "key" against "key=value" without copying either
        if (strncmp(entry, key, nkey) != 0 || entry[nkey] != '=')
        {
            continue;
        }

        if (config_override_hit)
        {
            config_override_hit[o] = 1;
        }
        return entry + nkey + 1;
    }

    return 0;
}

//
// @brief Warn about overrides that never matched a key in the config file.
//
// An override only replaces a line the file already has, so one that never
// fired is silently doing nothing — a typo, or a key this config does not set.
// This is a warning, not an error: the caller is told and decides for itself.
//
// @return number of overrides that never matched
//
int
modbusmq_config_override_unmatched(void)
{
    int
        unmatched = 0;

    for(int o = 0; o < config_noverrides; ++o)
    {
        if (config_override_hit && config_override_hit[o])
        {
            continue;
        }
        fprintf(stderr, "WARNING: -e %s: key never appears in the config file, override not applied\n",
                config_overrides[o] ? config_overrides[o] : "");
        unmatched++;
    }

    return unmatched;
}

//
// Validate the channels of one block write and settle how long the block is.
//
// A block is written whole, so the config has to describe it whole: every
// register from address to address+naddress has to belong to exactly one
// channel. A gap could only be filled by reading the device first, and a
// read-modify-write spread over two frames is not atomic on a bus that is
// also being polled — the poll in between is entitled to see either version,
// and so is anything else writing to the same device. One function 16 frame
// either lands whole or does not land at all, which is the property worth
// keeping. So a gap is refused here, while it is still a typo in a file
// rather than a zero in a fuse.
//
// @param w: zero-based index of the write entry, for the messages
// @param write: the entry, whose naddress and has_defaults this fills in
//
// @return 0 when the block is consistent, < 0 with a message on stderr
//
static int
config_validate_write_block(int w, modbusmq_write_t *write)
{
    const char
        *name = write->name ? write->name : "?";
    int
        span     = 0,
        defaults = 1;

    for(int c = 0; c < write->channel_max; ++c)
    {
        modbusmq_write_channel_t
            *channel = &write->channels[c];
        const char
            *cname = channel->name ? channel->name : "?";

        if (channel->format == modbusmq_data_format_unknown)
        {
            fprintf(stderr, "write.%d.channel.%d (%s): format is required\n", w+1, c+1, cname);
            return -1;
        }
        //
        // Same reason as the single-value path: a write scales a number, and
        // a serial number or a clock is not one.
        //
        if (modbusmq_format_is_text(channel->format))
        {
            fprintf(stderr, "write.%d.channel.%d (%s): text formats can be read but not written\n", w+1, c+1, cname);
            return -1;
        }
        if (channel->length != 2 && channel->length != 4)
        {
            fprintf(stderr, "write.%d.channel.%d (%s): a register write is 2 or 4 bytes, %s is %d\n",
                    w+1, c+1, cname, modbusmq_config_dataformat_name(channel->format), channel->length);
            return -1;
        }
        //
        // offset counts bytes from the start of the block, the way an input
        // channel counts them, so a register boundary is an even one.
        //
        if (channel->offset < 0 || (channel->offset % 2) != 0)
        {
            fprintf(stderr, "write.%d.channel.%d (%s): offset %d is not on a register boundary -- offset counts bytes, so it is even\n",
                    w+1, c+1, cname, channel->offset);
            return -1;
        }

        if (channel->offset + channel->length > span)
        {
            span = channel->offset + channel->length;
        }
        if (!channel->has_value)
        {
            defaults = 0;
        }
    }

    //
    // naddress is optional: the channels already say how long the block is.
    // Spelling it out is still allowed, because a device whose last register
    // is a reserved word nobody reads should be writable without inventing a
    // channel for it -- and that is exactly the case the gap check below
    // refuses, so the two together mean an explicit naddress has to be
    // covered like everything else.
    //
    int
        nregs = write->has_naddress ? write->naddress : span / 2;

    if (nregs <= 0 || nregs > 123)
    {
        fprintf(stderr, "write.%d (%s): a block is 1..123 registers, this one works out to %d\n", w+1, name, nregs);
        return -1;
    }

    //
    // One byte per register rather than a bitmap: 123 of them is nothing, and
    // counting the claims tells an overlap and a gap apart in one pass each.
    //
    uint8_t
        claimed[123];

    memset(claimed, 0, sizeof(claimed));

    for(int c = 0; c < write->channel_max; ++c)
    {
        modbusmq_write_channel_t
            *channel = &write->channels[c];
        const char
            *cname = channel->name ? channel->name : "?";
        int
            first = channel->offset / 2,
            last  = first + channel->length / 2;

        if (last > nregs)
        {
            fprintf(stderr, "write.%d.channel.%d (%s): offset %d is %d register(s) into a block that is only %d long\n",
                    w+1, c+1, cname, channel->offset, last, nregs);
            return -1;
        }

        for(int r = first; r < last; ++r)
        {
            if (claimed[r]++)
            {
                fprintf(stderr, "write.%d.channel.%d (%s): register offset %d (address 0x%04X) is already claimed by another channel\n",
                        w+1, c+1, cname, r, write->address + r);
                return -1;
            }
        }
    }

    for(int r = 0; r < nregs; ++r)
    {
        if (!claimed[r])
        {
            fprintf(stderr, "write.%d (%s): register offset %d (address 0x%04X) belongs to no channel. A block write sends every "
                            "register it covers, so this one would go out as a zero nobody asked for -- give it a channel, "
                            "or shorten the block\n",
                    w+1, name, r, write->address + r);
            return -1;
        }
    }

    write->naddress     = nregs;
    write->has_naddress = 1;
    write->has_defaults = defaults;

    return 0;
}

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
    modbusmq_config->mqtt_retain = 1; // what publishing has always done
    modbusmq_config->mqtt_qos    = 0;
    
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

        //
        // -e key=value wins over the file's value for this key. Everything
        // below sees the substituted value and neither knows nor cares.
        //
        const char
            *override = config_override_lookup(key);
        if (override)
        {
            if (modbusmq_get_debug() > 0)
            {
                fprintf(stderr, "%d: %s = %s (overridden by -e, file said %s)\n",
                        line_num, key, override, value);
            }
            value = (char *)override;
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
            //
            // Per-input defaults for the publish rate-limiting keys. Same
            // validation as the per-channel ones below; the two never
            // interact until modbusmq_config_apply_publish_defaults() copies
            // the input's value into whichever channels leave theirs unset.
            //
            else if (strcmp(input_key, "on_change") == 0)
            {
                int
                    on_change = config_boolean(value);
                if (on_change < 0)
                {
                    fprintf(stderr, "%d: %s=%s: expected 1/0, true/false or yes/no\n", line_num, key, value);
                    fclose(fp);
                    free(line);
                    return -1;
                }
                input->on_change     = on_change;
                input->has_on_change = 1;
            }
            else if (strcmp(input_key, "min_change_rel") == 0)
            {
                input->min_change_rel = strtod(value, NULL);
                if (input->min_change_rel < 0)
                {
                    fprintf(stderr, "%d: %s=%s: min_change_rel must not be negative\n", line_num, key, value);
                    fclose(fp);
                    free(line);
                    return -1;
                }
                input->has_min_change_rel = 1;
            }
            else if (strcmp(input_key, "min_change") == 0)
            {
                input->min_change = strtod(value, NULL);
                if (input->min_change < 0)
                {
                    fprintf(stderr, "%d: %s=%s: min_change must not be negative\n", line_num, key, value);
                    fclose(fp);
                    free(line);
                    return -1;
                }
                input->has_min_change = 1;
            }
            else if (strcmp(input_key, "min_interval") == 0)
            {
                input->min_interval = (int)strtol(value, NULL, 0);
                if (input->min_interval < 0)
                {
                    fprintf(stderr, "%d: %s=%s: min_interval must not be negative\n", line_num, key, value);
                    fclose(fp);
                    free(line);
                    return -1;
                }
                input->has_min_interval = 1;
            }
            else if (strcmp(input_key, "max_interval") == 0)
            {
                input->max_interval = (int)strtol(value, NULL, 0);
                if (input->max_interval < 0)
                {
                    fprintf(stderr, "%d: %s=%s: max_interval must not be negative\n", line_num, key, value);
                    fclose(fp);
                    free(line);
                    return -1;
                }
                input->has_max_interval = 1;
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
                    //
                    // A variable-width text format reports size 0 and takes
                    // its length from channel.length / channel.nregisters. An
                    // explicit length already read stands, since the keys may
                    // appear in either order; the validator afterwards is what
                    // catches a length that contradicts a fixed-size format.
                    //
                    if (!channel->has_length)
                    {
                        channel->length = modbusmq_format_size(channel->format);
                    }
                }
                else if (strcmp(channel_key, "length") == 0 ||
                         strcmp(channel_key, "nregisters") == 0)
                {
                    int
                        n = (int)strtol(value, NULL, 0);
                    int
                        bytes = (channel_key[0] == 'n') ? n * 2 : n;

                    if (n <= 0 || bytes > MODBUSMQ_TEXT_BYTES_MAX)
                    {
                        fprintf(stderr, "%d: %s=%s: %s must be between 1 and %d bytes\n",
                                line_num, key, value, channel_key, MODBUSMQ_TEXT_BYTES_MAX);
                        fclose(fp);
                        free(line);
                        return -1;
                    }

                    channel->length     = bytes;
                    channel->has_length = 1;
                }
                else if (strcmp(channel_key, "timefmt") == 0)
                {
                    config_set_string(&channel->timefmt, value);
                }
                else if (strcmp(channel_key, "timezone") == 0)
                {
                    if (strcmp(value, "utc") == 0)
                    {
                        channel->localtime = 0;
                    }
                    else if (strcmp(value, "local") == 0)
                    {
                        channel->localtime = 1;
                    }
                    else
                    {
                        fprintf(stderr, "%d: %s=%s: timezone must be utc or local\n", line_num, key, value);
                        fclose(fp);
                        free(line);
                        return -1;
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
                    config_set_string(&channel->topic, value);
                }
                else if (strcmp(channel_key, "value") == 0)
                {
                    //
                    // Kept both ways. A text channel's default cannot survive
                    // strtod() — "PIX-00123" becomes 0 — and the format that
                    // decides which one matters is free to appear later in the
                    // file, so neither reading can be skipped here.
                    //
                    channel->value = strtod(value, NULL);
                    config_set_string(&channel->value_text, value);
                }
                else if (strcmp(channel_key, "retain") == 0)
                {
                    channel->retain = config_boolean(value);
                    if (channel->retain < 0)
                    {
                        fprintf(stderr, "%d: %s=%s: expected 1/0, true/false or yes/no\n", line_num, key, value);
                        fclose(fp);
                        free(line);
                        return -1;
                    }
                    channel->has_retain = 1;
                }
                else if (strcmp(channel_key, "qos") == 0)
                {
                    channel->qos = (int)strtol(value, NULL, 0);
                    if (channel->qos < 0 || channel->qos > 2)
                    {
                        fprintf(stderr, "%d: %s=%s: qos must be 0, 1 or 2\n", line_num, key, value);
                        fclose(fp);
                        free(line);
                        return -1;
                    }
                    channel->has_qos = 1;
                }
                else if (strcmp(channel_key, "decimals") == 0)
                {
                    channel->decimals = (int)strtol(value, NULL, 0);
                    if (channel->decimals < 0 || channel->decimals > 9)
                    {
                        fprintf(stderr, "%d: %s=%s: decimals must be between 0 and 9\n", line_num, key, value);
                        fclose(fp);
                        free(line);
                        return -1;
                    }
                    channel->has_decimals = 1;
                }
                else if (strcmp(channel_key, "on_change") == 0)
                {
                    int
                        on_change = config_boolean(value);
                    if (on_change < 0)
                    {
                        fprintf(stderr, "%d: %s=%s: expected 1/0, true/false or yes/no\n", line_num, key, value);
                        fclose(fp);
                        free(line);
                        return -1;
                    }
                    channel->on_change     = on_change;
                    channel->has_on_change = 1;
                }
                else if (strcmp(channel_key, "min_change") == 0)
                {
                    channel->min_change = strtod(value, NULL);
                    if (channel->min_change < 0)
                    {
                        fprintf(stderr, "%d: %s=%s: min_change must not be negative\n", line_num, key, value);
                        fclose(fp);
                        free(line);
                        return -1;
                    }
                    channel->has_min_change = 1;
                }
                else if (strcmp(channel_key, "min_change_rel") == 0)
                {
                    channel->min_change_rel = strtod(value, NULL);
                    if (channel->min_change_rel < 0)
                    {
                        fprintf(stderr, "%d: %s=%s: min_change_rel must not be negative\n", line_num, key, value);
                        fclose(fp);
                        free(line);
                        return -1;
                    }
                    channel->has_min_change_rel = 1;
                }
                else if (strcmp(channel_key, "min_interval") == 0)
                {
                    channel->min_interval = (int)strtol(value, NULL, 0);
                    if (channel->min_interval < 0)
                    {
                        fprintf(stderr, "%d: %s=%s: min_interval must not be negative\n", line_num, key, value);
                        fclose(fp);
                        free(line);
                        return -1;
                    }
                    channel->has_min_interval = 1;
                }
                else if (strcmp(channel_key, "max_interval") == 0)
                {
                    channel->max_interval = (int)strtol(value, NULL, 0);
                    if (channel->max_interval < 0)
                    {
                        fprintf(stderr, "%d: %s=%s: max_interval must not be negative\n", line_num, key, value);
                        fclose(fp);
                        free(line);
                        return -1;
                    }
                    channel->has_max_interval = 1;
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
                //
                // Text formats are read-only. Writing a serial number or a
                // clock back to a device is a different job with different
                // failure modes, and the write path scales a number — there is
                // nothing here to scale. Say so plainly rather than letting a
                // zero length surface as an unrelated complaint further down.
                //
                if (modbusmq_format_is_text(write->format))
                {
                    fprintf(stderr, "%d: %s=%s: text formats can be read but not written\n", line_num, key, value);
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
            else if (strcmp(write_key, "ack_topic") == 0)
            {
                config_set_string(&write->ack_topic, value);
            }
            else if (strcmp(write_key, "ack") == 0)
            {
                write->ack = config_boolean(value);
                if (write->ack < 0)
                {
                    fprintf(stderr, "%d: %s=%s: expected 1/0, true/false or yes/no\n", line_num, key, value);
                    fclose(fp);
                    free(line);
                    return -1;
                }
                write->has_ack = 1;
            }
            else if (strcmp(write_key, "naddress") == 0)
            {
                write->naddress     = (int)strtoul(value, NULL, 0);
                write->has_naddress = 1;
            }
            else if (strcmp(write_key, "channel.max") == 0)
            {
                if (write->channels)
                {
                    fprintf(stderr, "%d: %s listed more than once. The array is allocated when this key is read, "
                                "so a second one would discard everything already parsed into the first.\n", line_num, key);
                    fclose(fp);
                    free(line);
                    return -1;
                }
                int
                    nvalue = (int)strtoul(value, NULL, 0);
                //
                // 123 registers is what function 16 carries, and a channel is
                // at least one register, so no block can hold more than that
                // many channels however they are laid out.
                //
                if (nvalue <= 0 || nvalue > 123)
                {
                    fprintf(stderr, "%d: %s must be between [1..123]\n", line_num, key);
                    fclose(fp);
                    free(line);
                    return -1;
                }
                write->channel_max = nvalue;
                write->channels    = malloc(nvalue * sizeof(modbusmq_write_channel_t));
                if (!write->channels)
                {
                    perror("Unable to allocate");
                    fclose(fp);
                    free(line);
                    return -1;
                }
                memset(write->channels, 0, nvalue * sizeof(modbusmq_write_channel_t));
            }
            else if (strncmp(write_key, "channel.", 8) == 0)
            {
                // write.{}.channel.{}
                int
                    channel_num = 0;
                char
                    channel_key[100];

                int
                    nscan = sscanf(write_key, "channel.%d.%99s", &channel_num, channel_key);
                if (nscan != 2)
                {
                    fprintf(stderr, "%d: %s: Unable to scan key\n", line_num, key);
                    continue;
                }

                //
                // Hard error rather than the input side's skip-and-warn: a
                // channel silently dropped from a block leaves a register with
                // nothing in it, and the block is written whole.
                //
                if (channel_num <= 0 || channel_num > write->channel_max)
                {
                    fprintf(stderr, "%d: %s, channel.{%d} must be between 1..%d (is write.%d.channel.max set, and set first?)\n",
                            line_num, key, channel_num, write->channel_max, write_num);
                    fclose(fp);
                    free(line);
                    return -1;
                }

                modbusmq_write_channel_t
                    *channel = &write->channels[channel_num-1];

                if (strcmp(channel_key, "name") == 0)
                {
                    config_set_string(&channel->name, value);
                }
                else if (strcmp(channel_key, "offset") == 0)
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
                else if (strcmp(channel_key, "add") == 0)
                {
                    channel->add = strtod(value, NULL);
                }
                else if (strcmp(channel_key, "mod") == 0)
                {
                    channel->mod = strtod(value, NULL);
                }
                else if (strcmp(channel_key, "mul") == 0)
                {
                    channel->mul = strtod(value, NULL);
                }
                else if (strcmp(channel_key, "value") == 0)
                {
                    channel->value     = strtod(value, NULL);
                    channel->has_value = 1;
                }
                else
                {
                    fprintf(stderr, "%d: %s: Unknown configuration\n", line_num, key);
                    continue;
                }
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
        else if (strcmp(key, "mqtt.retain") == 0)
        {
            modbusmq_config->mqtt_retain = config_boolean(value);
            if (modbusmq_config->mqtt_retain < 0)
            {
                fprintf(stderr, "%d: %s=%s: expected 1/0, true/false or yes/no\n", line_num, key, value);
                fclose(fp);
                free(line);
                return -1;
            }
        }
        else if (strcmp(key, "mqtt.qos") == 0)
        {
            modbusmq_config->mqtt_qos = (int)strtol(value, NULL, 0);
            if (modbusmq_config->mqtt_qos < 0 || modbusmq_config->mqtt_qos > 2)
            {
                fprintf(stderr, "%d: %s=%s: qos must be 0, 1 or 2\n", line_num, key, value);
                fclose(fp);
                free(line);
                return -1;
            }
        }
        else if (strcmp(key, "mqtt.topic_prefix") == 0)
        {
            config_set_string(&modbusmq_config->mqtt_topic_prefix, value);
        }
        //
        // Config-wide default for write.N.ack. Off, so nothing starts
        // publishing acks because a config was upgraded.
        //
        else if (strcmp(key, "mqtt.ack") == 0)
        {
            modbusmq_config->mqtt_ack = config_boolean(value);
            if (modbusmq_config->mqtt_ack < 0)
            {
                fprintf(stderr, "%d: %s=%s: expected 1/0, true/false or yes/no\n", line_num, key, value);
                fclose(fp);
                free(line);
                return -1;
            }
        }
        //
        // Config-wide defaults for the publish rate-limiting keys — the
        // bottom rung under input.N.* and input.N.channel.M.*. Same
        // validation as those; modbusmq_config_apply_publish_defaults()
        // resolves the three levels down onto each channel after parsing.
        //
        else if (strcmp(key, "publish.on_change") == 0)
        {
            int
                on_change = config_boolean(value);
            if (on_change < 0)
            {
                fprintf(stderr, "%d: %s=%s: expected 1/0, true/false or yes/no\n", line_num, key, value);
                fclose(fp);
                free(line);
                return -1;
            }
            modbusmq_config->publish_on_change     = on_change;
            modbusmq_config->has_publish_on_change = 1;
        }
        else if (strcmp(key, "publish.min_change") == 0)
        {
            modbusmq_config->publish_min_change = strtod(value, NULL);
            if (modbusmq_config->publish_min_change < 0)
            {
                fprintf(stderr, "%d: %s=%s: min_change must not be negative\n", line_num, key, value);
                fclose(fp);
                free(line);
                return -1;
            }
            modbusmq_config->has_publish_min_change = 1;
        }
        else if (strcmp(key, "publish.min_change_rel") == 0)
        {
            modbusmq_config->publish_min_change_rel = strtod(value, NULL);
            if (modbusmq_config->publish_min_change_rel < 0)
            {
                fprintf(stderr, "%d: %s=%s: min_change_rel must not be negative\n", line_num, key, value);
                fclose(fp);
                free(line);
                return -1;
            }
            modbusmq_config->has_publish_min_change_rel = 1;
        }
        else if (strcmp(key, "publish.min_interval") == 0)
        {
            modbusmq_config->publish_min_interval = (int)strtol(value, NULL, 0);
            if (modbusmq_config->publish_min_interval < 0)
            {
                fprintf(stderr, "%d: %s=%s: min_interval must not be negative\n", line_num, key, value);
                fclose(fp);
                free(line);
                return -1;
            }
            modbusmq_config->has_publish_min_interval = 1;
        }
        else if (strcmp(key, "publish.max_interval") == 0)
        {
            modbusmq_config->publish_max_interval = (int)strtol(value, NULL, 0);
            if (modbusmq_config->publish_max_interval < 0)
            {
                fprintf(stderr, "%d: %s=%s: max_interval must not be negative\n", line_num, key, value);
                fclose(fp);
                free(line);
                return -1;
            }
            modbusmq_config->has_publish_max_interval = 1;
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
    // Same idea, unrelated concern: resolve input-level publish defaults down
    // onto their channels before anything runs, so the runtime never has to.
    //
    modbusmq_config_apply_publish_defaults(modbusmq_config);

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

            int
                fixed = modbusmq_format_size(channel->format);

            if (!modbusmq_format_is_text(channel->format))
            {
                //
                // The text-only keys on a numeric channel are always a
                // misunderstanding, and a silently ignored key is a bug report
                // six months later.
                //
                if (channel->has_length)
                {
                    fprintf(stderr, "input.%d.channel.%d: length/nregisters applies to a text format only, %s is %d bytes\n",
                            i+1, c+1, modbusmq_config_dataformat_name(channel->format), fixed);
                    return -1;
                }
                if (channel->timefmt || channel->localtime)
                {
                    fprintf(stderr, "input.%d.channel.%d: timefmt/timezone applies to a time format only\n", i+1, c+1);
                    return -1;
                }
                continue;
            }

            //
            // A variable-width format has no size of its own — the device
            // decides how many registers the serial number spans, and only the
            // config knows. Defaulting it would read whatever length happened
            // to be plausible and publish the result as fact.
            //
            if (fixed == 0)
            {
                if (!channel->has_length)
                {
                    fprintf(stderr, "input.%d.channel.%d: %s needs length (bytes) or nregisters\n",
                            i+1, c+1, modbusmq_config_dataformat_name(channel->format));
                    return -1;
                }
            }
            else if (channel->has_length && channel->length != fixed)
            {
                fprintf(stderr, "input.%d.channel.%d: %s is %d bytes, not %d\n",
                        i+1, c+1, modbusmq_config_dataformat_name(channel->format), fixed, channel->length);
                return -1;
            }

            if (channel->format == modbusmq_data_format_version_regs && (channel->length % 2) != 0)
            {
                fprintf(stderr, "input.%d.channel.%d: %s reads whole registers, so length must be even (or use nregisters)\n",
                        i+1, c+1, modbusmq_config_dataformat_name(channel->format));
                return -1;
            }

            //
            // Scaling has no meaning on a string. add survives on an epoch
            // format alone, where it is the shift from the device's epoch onto
            // the Unix one.
            //
            if (channel->mod != 0 || channel->mul != 0)
            {
                fprintf(stderr, "input.%d.channel.%d: mod/mul does not apply to %s, there is no number to scale\n",
                        i+1, c+1, modbusmq_config_dataformat_name(channel->format));
                return -1;
            }
            if (channel->add != 0 && !modbusmq_format_is_epoch(channel->format))
            {
                fprintf(stderr, "input.%d.channel.%d: add applies to an epoch format only (as the epoch shift), not %s\n",
                        i+1, c+1, modbusmq_config_dataformat_name(channel->format));
                return -1;
            }
            if (channel->has_decimals)
            {
                fprintf(stderr, "input.%d.channel.%d: decimals does not apply to %s\n",
                        i+1, c+1, modbusmq_config_dataformat_name(channel->format));
                return -1;
            }

            //
            // Checked on has_*, not the value: a config-wide publish.min_change
            // is a default for the numeric channels and must not turn a text
            // channel into a parse error. Only a channel that asked for it
            // itself is told the key means nothing here.
            //
            if (channel->has_min_change || channel->has_min_change_rel)
            {
                fprintf(stderr, "input.%d.channel.%d: min_change/min_change_rel needs a magnitude, which %s has none of -- use on_change\n",
                        i+1, c+1, modbusmq_config_dataformat_name(channel->format));
                return -1;
            }

            if ((channel->timefmt || channel->localtime) && !modbusmq_format_is_time(channel->format))
            {
                fprintf(stderr, "input.%d.channel.%d: timefmt/timezone applies to a time format only, not %s\n",
                        i+1, c+1, modbusmq_config_dataformat_name(channel->format));
                return -1;
            }

            //
            // A date the device reported as plain digits carries no timezone,
            // so there is nothing to convert it from. Rendering it "in local
            // time" would shift it by an offset nobody supplied.
            //
            if (channel->localtime && !modbusmq_format_is_epoch(channel->format))
            {
                fprintf(stderr, "input.%d.channel.%d: timezone applies to an epoch format only -- %s carries no timezone to convert from\n",
                        i+1, c+1, modbusmq_config_dataformat_name(channel->format));
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
        // A block write is the entry that has channels. Everything the
        // single-value shape puts on the entry -- format, scaling, the on/off
        // mapping -- belongs on the individual channels there, since the whole
        // point of a block is that its registers are different things. An
        // entry carrying both would mean two incompatible things at once.
        //
        int
            is_block = (write->channel_max > 0);

        if (is_block)
        {
            if (write->format || write->add || write->mod || write->mul)
            {
                fprintf(stderr, "write.%d (%s): format/add/mod/mul belong on write.%d.channel.M.* in a block write, not on the entry\n",
                        w+1, name, w+1);
                return -1;
            }
            if (write->has_on_value)
            {
                fprintf(stderr, "write.%d (%s): on_value/off_value map a single on/off command and do not apply to a block write\n",
                        w+1, name);
                return -1;
            }
            if (write->type     == modbusmq_type_coil ||
                write->function == modbusmq_write_function_coil)
            {
                fprintf(stderr, "write.%d (%s): a block write is registers; coils are written one at a time\n", w+1, name);
                return -1;
            }
            if (write->function != modbusmq_write_function_unknown &&
                write->function != modbusmq_write_function_registers)
            {
                fprintf(stderr, "write.%d (%s): a block write needs function %s\n",
                        w+1, name, MODBUSMQ_FUNCTION_WRITE_REGISTERS);
                return -1;
            }

            write->function = modbusmq_write_function_registers;
            write->type     = modbusmq_type_holding_register;

            if (config_validate_write_block(w, write) < 0)
            {
                return -1;
            }

            continue;
        }

        //
        // naddress without channels describes a block nobody laid out.
        //
        if (write->has_naddress)
        {
            fprintf(stderr, "write.%d (%s): naddress describes a block, so it needs write.%d.channel.max and the channels to go with it\n",
                    w+1, name, w+1);
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

            if (!write->ack_topic)
            {
                continue;
            }

            int
                nack = strlen(write->ack_topic);

            char *ack_topic = malloc(nprefix + nack + 1);
            if (!ack_topic)
            {
                perror("Unable to allocate");
                return -1;
            }
            strcpy(ack_topic, modbusmq_config->mqtt_topic_prefix);
            strcat(ack_topic, write->ack_topic);
            free(write->ack_topic);
            write->ack_topic = ack_topic;
        }
    }

    //
    // Settle the ack once, here, rather than leaving the runtime to work out
    // per message whether this entry wants one: an entry says so itself, or
    // takes mqtt.ack, which is off.
    //
    // The default topic is derived after the prefix has gone on, so an ack
    // lands next to the topic that triggered it whatever namespace that is in.
    //
    for(int w = 0; w < modbusmq_config->write_max; ++w)
    {
        modbusmq_write_t
            *write = &modbusmq_config->writes[w];

        if (!write->has_ack)
        {
            write->ack     = modbusmq_config->mqtt_ack;
            write->has_ack = 1;
        }

        if (!write->ack || write->ack_topic || !write->topic)
        {
            continue;
        }

        const char
            *suffix = MODBUSMQ_ACK_SUFFIX;

        write->ack_topic = malloc(strlen(write->topic) + strlen(suffix) + 1);
        if (!write->ack_topic)
        {
            perror("Unable to allocate");
            return -1;
        }
        strcpy(write->ack_topic, write->topic);
        strcat(write->ack_topic, suffix);
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
        if (write->name)      { free(write->name); }
        if (write->topic)     { free(write->topic); }
        if (write->ack_topic) { free(write->ack_topic); }

        for(int c = 0; c < write->channel_max; ++c)
        {
            if (write->channels[c].name)
            {
                free(write->channels[c].name);
            }
        }
        free(write->channels);
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
// Clear every channel's publish state. See the declaration in
// modbusmq_config.h for when to call this.
void
modbusmq_config_reset_publish_state(modbusmq_config_t *config)
{
    if (!config)
    {
        return;
    }

    for(int i = 0; i < config->input_max; ++i)
    {
        modbusmq_input_t
            *input = &config->inputs[i];

        for(int c = 0; c < input->channel_max; ++c)
        {
            modbusmq_channel_t
                *channel = &input->channels[c];

            channel->published       = 0;
            channel->last_value      = 0;
            channel->last_text[0]    = 0;
            channel->last_publish_ms = 0;
        }
    }
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
