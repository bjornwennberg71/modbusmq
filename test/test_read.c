//////////////////////////////////////////////////////////////////////////////
//
// bjornwennberg71@gmail.com
//
// test_read.c
//
// Reads every input in the fixture config off a running modbusmq_server and
// checks the decoded, scaled value of each channel. This is the end-to-end
// path: request framing, transport, response validation, byte order, scaling.
//
// Expected values are hardcoded here and correspond to the "value =" defaults
// in test.config. Change one there and this table must change with it.
//
// usage: test_read <config-file>
//
#include "modbusmq.h"
#include "modbusmq_config.h"
#include "test_common.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

//
// what each topic must decode to, after scaling
//
typedef struct expected_t
{
    const char *topic;
    float       value;
} expected_t;

static const expected_t expected[] = {
    { "t/voltage",     48.00f  },  // raw 4800, mod -100
    { "t/current",     -5.0f   },  // raw -50 signed, mod -10
    { "t/energy",      123456.0f },  // 4-byte signed, unscaled
    { "t/temperature", 3.25f   },  // 4-byte float
    { "t/flags",       65535.0f  },  // unsigned, top bit set
    { "t/scaled",      210.0f  },  // (100 + 5) * 2
    { "t/setpoint",    1234.0f },  // holding register
    { "t/coil_on",     1.0f    },
    { "t/coil_off",    0.0f    },
    { "t/coil_on2",    1.0f    },
    { "t/di_on",       1.0f    },
    { "t/di_on3",      1.0f    },
};

static const expected_t *
expected_for(const char *topic)
{
    for(size_t i = 0; i < sizeof(expected)/sizeof(expected[0]); ++i)
    {
        if (strcmp(expected[i].topic, topic) == 0)
        {
            return &expected[i];
        }
    }
    return NULL;
}

//
// @brief builds the read request for one input
//
// @return 0 on success, < 0 for a type we do not know how to request
//
static int
build_request(struct modbusmq_context_t *context, modbusmq_msg_t *msg, modbusmq_input_t *input)
{
    modbusmq_set_slave(context, input->slave);

    switch(input->type)
    {
    case modbusmq_type_input_register:
        modbusmq_frame_read_input_registers(context, &msg->frame[0], input->address, input->naddress);
        return 0;
    case modbusmq_type_holding_register:
        modbusmq_frame_read_holding_registers(context, &msg->frame[0], input->address, input->naddress);
        return 0;
    case modbusmq_type_coil:
        modbusmq_frame_read_coil_bits(context, &msg->frame[0], input->address, input->naddress);
        return 0;
    case modbusmq_type_discrete_input:
        modbusmq_frame_read_input_bits(context, &msg->frame[0], input->address, input->naddress);
        return 0;
    default:
        break;
    }

    return -1;
}

int
main(int argc, char **argv)
{
    printf("== test_read ==\n");

    if (argc < 2)
    {
        printf("  usage: test_read <config-file>\n");
        return 2;
    }

    if (modbusmq_config_parse(argv[1]) != 0)
    {
        printf("  FAIL: unable to parse %s\n", argv[1]);
        return 2;
    }

    modbusmq_config_t
        *config = modbusmq_config_get();

    CHECK(config != NULL, "config loaded");
    CHECK_INT(config->input_max, 4, "fixture has four inputs");

    struct modbusmq_context_t
        *context = modbusmq_tcp_context(config->modbusmq_connect);

    CHECK(context != NULL, "context created");
    if (!context)
    {
        return test_summary("test_read");
    }

    modbusmq_set_config(context, config);
    modbusmq_frame_timeout(context, config->modbusmq_frame_timeout_ms);

    CHECK_INT(modbusmq_connect(context), 0, "connected to the virtual server");

    int
        checked_channels = 0;

    for(int i = 0; i < config->input_max; ++i)
    {
        modbusmq_input_t
            *input = &config->inputs[i];

        modbusmq_msg_t
            msg;
        memset(&msg, 0, sizeof(msg));

        if (build_request(context, &msg, input) != 0)
        {
            printf("  FAIL: input %d has an unknown type %d\n", i + 1, input->type);
            tests_failed++;
            tests_checked++;
            continue;
        }

        char what[128];

        snprintf(what, sizeof(what), "input %d (type 0x%02X) answered", i + 1, input->type);
        int rc = modbusmq_send(context, &msg, 2000);
        CHECK_INT(rc, 0, what);

        if (rc != 0)
        {
            continue;
        }

        //
        // the response must actually be the one we asked for
        //
        snprintf(what, sizeof(what), "input %d response slave", i + 1);
        CHECK_INT(modbusmq_frame_slave(context, &msg.frame[1]), input->slave, what);

        snprintf(what, sizeof(what), "input %d response function", i + 1);
        CHECK_INT(modbusmq_frame_function(context, &msg.frame[1]), input->type, what);

        for(int c = 0; c < input->channel_max; ++c)
        {
            modbusmq_channel_t
                *channel = &input->channels[c];

            if (!channel->topic)
            {
                continue;
            }

            const expected_t
                *want = expected_for(channel->topic);

            snprintf(what, sizeof(what), "%s is a known topic", channel->topic);
            CHECK(want != NULL, what);
            if (!want)
            {
                continue;
            }

            snprintf(what, sizeof(what), "%s lies inside the response", channel->topic);
            CHECK_INT(modbusmq_channel_in_range(context, &msg, input, channel), 0, what);

            float
                got = modbusmq_read_channel(context, &msg, input, channel);

            snprintf(what, sizeof(what), "%s value", channel->topic);
            CHECK_FLT(got, want->value, 0.001, what);

            checked_channels++;
        }
    }

    CHECK_INT(checked_channels, (int)(sizeof(expected)/sizeof(expected[0])),
              "every expected channel was checked");

    modbusmq_close(context);
    modbusmq_free(context);

    return test_summary("test_read");
}
