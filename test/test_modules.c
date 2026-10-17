//////////////////////////////////////////////////////////////////////////////
//
// bjornwennberg71@gmail.com
//
// test_modules.c
//
// Module templates (module.count, module.slave_first, slave = module, %i):
// how the fixture expands, with and without -e overrides, the order of the
// expanded inputs, the topics, the writes, modbusmq_module_slave(), and the
// configs the parser must refuse. No server needed.
//
// usage: test_modules <modules.config>
//
#include "modbusmq.h"
#include "modbusmq_config.h"
#include "modbusmq_internal.h"
#include "test_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

//
// @brief Parse filename with these -e overrides.
//
// @return what modbusmq_config_parse() returned
//
static int
parse(const char *filename, char *const *overrides, int noverrides)
{
    modbusmq_config_close();
    modbusmq_config_set_overrides(overrides, noverrides);
    return modbusmq_config_parse(filename);
}

//
// @brief Parse a config from memory with modbusmq_config_parse_text().
//
// The header is a working one-input config; body is appended to it, so each
// case below only spells out what it is about.
//
// @return what modbusmq_config_parse_text() returned
//
static int
parse_text(const char *body, char *const *overrides, int noverrides)
{
    static const char
        *header = "config.name = t\n"
                  "config.version = 2.0\n"
                  "modbusmq.connect = tcp://localhost:1\n";
    size_t
        length = strlen(header) + strlen(body) + 1;
    char
        *text = malloc(length + 1);

    if (!text)
    {
        return -100;
    }
    snprintf(text, length + 1, "%s%s\n", header, body);

    modbusmq_config_close();
    modbusmq_config_set_overrides(overrides, noverrides);

    int
        rc = modbusmq_config_parse_text(text, length);

    free(text);
    return rc;
}

//
// One template input with one channel, topic given by the caller.
//
#define TEMPLATE_INPUT(n, topic)                 \
    "input." #n ".slave = module\n"              \
    "input." #n ".type = input_register\n"       \
    "input." #n ".address = 0\n"                 \
    "input." #n ".naddress = 1\n"                \
    "input." #n ".interval_ms = 1000\n"          \
    "input." #n ".channel.max = 1\n"             \
    "input." #n ".channel.1.offset = 0\n"        \
    "input." #n ".channel.1.format = uint_ab\n"  \
    "input." #n ".channel.1.topic = " topic "\n"

#define FIXED_INPUT(n, topic)                    \
    "input." #n ".slave = 7\n"                   \
    "input." #n ".type = input_register\n"       \
    "input." #n ".address = 0\n"                 \
    "input." #n ".naddress = 1\n"                \
    "input." #n ".interval_ms = 1000\n"          \
    "input." #n ".channel.max = 1\n"             \
    "input." #n ".channel.1.offset = 0\n"        \
    "input." #n ".channel.1.format = uint_ab\n"  \
    "input." #n ".channel.1.topic = " topic "\n"

//
// The fixture as the file has it: 2 modules from slave 1.
//
static void
test_file_values(const char *filename)
{
    printf("fixture, no overrides\n");

    CHECK_INT(parse(filename, NULL, 0), 0, "fixture parses");

    modbusmq_config_t
        *config = modbusmq_config_get();

    CHECK_INT(config->module_count, 2, "module.count from the file");
    CHECK_INT(config->module_slave_first, 1, "module.slave_first from the file");
    CHECK_INT(config->input_max, 1 + 2 * 2, "1 fixed + 2 templates x 2 modules");
    CHECK_INT(config->write_max, 2, "1 template write x 2 modules");
    CHECK_INT(config->inputs[0].slave, 1, "module 0 on slave 1");
    CHECK_INT(config->inputs[3].slave, 2, "module 1 on slave 2");
}

//
// The fixture with -e module.count=4 -e module.slave_first=39, the way a site
// runs it.
//
static void
test_overrides(const char *filename)
{
    printf("fixture, -e module.count=4 -e module.slave_first=39\n");

    char
        *overrides[] = { "module.count=4", "module.slave_first=39" };

    CHECK_INT(parse(filename, overrides, 2), 0, "fixture parses");
    CHECK_INT(modbusmq_config_override_unmatched(), 0, "both overrides matched");

    modbusmq_config_t
        *config = modbusmq_config_get();

    CHECK_INT(config->module_count, 4, "module.count overridden");
    CHECK_INT(config->module_slave_first, 39, "module.slave_first overridden");
    CHECK_INT(config->input_max, 1 + 2 * 4, "1 fixed + 2 templates x 4 modules");

    //
    // Module-major, the fixed input keeping its place among module 0's.
    //
    static const struct
    {
        int slave, module, address;
        const char *topic;
    } want[] =
    {
        { 39, 0, 0x0000, "battery/0/voltage" },
        { 100, -1, 0x0000, "string/voltage" },
        { 39, 0, 0x0100, "battery/0/cell/1/voltage" },
        { 40, 1, 0x0000, "battery/1/voltage" },
        { 40, 1, 0x0100, "battery/1/cell/1/voltage" },
        { 41, 2, 0x0000, "battery/2/voltage" },
        { 41, 2, 0x0100, "battery/2/cell/1/voltage" },
        { 42, 3, 0x0000, "battery/3/voltage" },
        { 42, 3, 0x0100, "battery/3/cell/1/voltage" },
    };

    for(int i = 0; i < (int)(sizeof(want) / sizeof(want[0])) && i < config->input_max; ++i)
    {
        modbusmq_input_t
            *input = &config->inputs[i];
        char
            what[64];

        snprintf(what, sizeof(what), "input %d slave", i+1);
        CHECK_INT(input->slave, want[i].slave, what);
        snprintf(what, sizeof(what), "input %d module", i+1);
        CHECK_INT(input->module, want[i].module, what);
        snprintf(what, sizeof(what), "input %d address", i+1);
        CHECK_INT(input->address, want[i].address, what);
        snprintf(what, sizeof(what), "input %d topic", i+1);
        CHECK_STR(input->channels[0].topic, want[i].topic, what);
    }

    // the second channel, and everything else copied, not shared
    CHECK_STR(config->inputs[7].channels[1].topic, "battery/3/soc", "second channel topic");
    CHECK(config->inputs[0].channels != config->inputs[3].channels, "each copy has its own channels");
    CHECK_INT(config->inputs[5].tier, modbusmq_tier_fast, "tier copied (fast)");
    CHECK_INT(config->inputs[6].tier, modbusmq_tier_deep, "tier copied (deep)");
    CHECK_FLT(config->inputs[7].channels[0].value, 5120, 0.01, "value copied");

    // writes expand the same way, ack topic derived from the expanded topic
    CHECK_INT(config->write_max, 4, "1 template write x 4 modules");
    CHECK_INT(config->writes[2].slave, 41, "write module 2 on slave 41");
    CHECK_INT(config->writes[2].module, 2, "write module index");
    CHECK_STR(config->writes[2].topic, "battery/2/set/charge_limit", "write topic");
    CHECK_STR(config->writes[2].ack_topic, "battery/2/set/charge_limit/ack", "write ack topic");

    // the index -> slave map an application uses for modbusmq_set_focus()
    struct modbusmq_context_t
        *context = modbusmq_tcp_context("tcp://localhost:1");

    modbusmq_set_config(context, config);
    CHECK_INT(modbusmq_module_slave(context, 0), 39, "module 0 -> slave 39");
    CHECK_INT(modbusmq_module_slave(context, 3), 42, "module 3 -> slave 42");
    CHECK(modbusmq_module_slave(context, -1) < 0, "module -1 is out of range");
    CHECK(modbusmq_module_slave(context, 4) < 0, "module 4 is out of range");
    modbusmq_free(context); // closes the config as well
}

//
// The parser's rules, one inline config each.
//
static void
test_rules(void)
{
    printf("rules\n");

    // a file that leaves module.slave_first at its default can still be moved
    {
        char
            *overrides[] = { "module.slave_first=32" };

        CHECK_INT(parse_text("module.count = 3\n"
                             "input.max = 1\n"
                             TEMPLATE_INPUT(1, "b/%i/v"), overrides, 1), 0, "slave_first not in the file");
        CHECK_INT(modbusmq_config_override_unmatched(), 0, "...override applied anyway");
        CHECK_INT(modbusmq_config_get()->inputs[2].slave, 34, "...module 2 on slave 34");
    }

    // interval_ms takes once as well as milliseconds
    CHECK_INT(parse_text("input.max = 1\n" FIXED_INPUT(1, "g")
                         "input.1.interval_ms = once\n", NULL, 0), 0, "interval_ms = once");
    CHECK_INT(modbusmq_config_get()->inputs[0].interval, MODBUSMQ_INTERVAL_ONCE, "...parses as once");

    // mixed: a fixed input first, then a template, %% is a literal %
    CHECK_INT(parse_text("module.count = 2\n"
                         "input.max = 2\n"
                         FIXED_INPUT(1, "s/100%%")
                         TEMPLATE_INPUT(2, "b/%i/v%%"), NULL, 0), 0, "mixed fixed + template");
    CHECK_INT(modbusmq_config_get()->input_max, 3, "...1 + 1 x 2");
    CHECK_STR(modbusmq_config_get()->inputs[0].channels[0].topic, "s/100%", "...%% in a fixed topic");
    CHECK_STR(modbusmq_config_get()->inputs[2].channels[0].topic, "b/1/v%", "...%% next to %i");

    // the prefix may carry %i too
    CHECK_INT(parse_text("module.count = 2\n"
                         "mqtt.topic_prefix = rack/%i/\n"
                         "input.max = 1\n"
                         TEMPLATE_INPUT(1, "v"), NULL, 0), 0, "%i in mqtt.topic_prefix");
    CHECK_STR(modbusmq_config_get()->inputs[1].channels[0].topic, "rack/1/v", "...prefix expanded");

    // count 0 is the feature off: a file without templates is untouched
    CHECK_INT(parse_text("input.max = 1\n"
                         FIXED_INPUT(1, "s/v"), NULL, 0), 0, "no templates, no module keys");
    CHECK_INT(modbusmq_config_get()->input_max, 1, "...one input");

    // the refusals
    CHECK(parse_text("module.count = 2\n"
                     "input.max = 1\n"
                     TEMPLATE_INPUT(1, "b/v"), NULL, 0) < 0, "template topic without %i is refused");
    CHECK(parse_text("module.count = 2\n"
                     "input.max = 1\n"
                     FIXED_INPUT(1, "s/%i/v"), NULL, 0) < 0, "%i in a fixed input is refused");
    CHECK(parse_text("input.max = 1\n"
                     TEMPLATE_INPUT(1, "b/%i/v"), NULL, 0) < 0, "template with module.count 0 is refused");
    CHECK(parse_text("module.count = 10\n"
                     "module.slave_first = 240\n"
                     "input.max = 1\n"
                     TEMPLATE_INPUT(1, "b/%i/v"), NULL, 0) < 0, "slaves past 247 are refused");

    // a 32-module rack: 10 inputs x 9 channels per module
    {
        char
            body[64 * 1024];
        int
            n = snprintf(body, sizeof(body), "module.count = 32\nmodule.slave_first = 39\ninput.max = 10\n");

        for(int i = 1; i <= 10; ++i)
        {
            n += snprintf(body + n, sizeof(body) - n,
                          "input.%d.slave = module\ninput.%d.type = input_register\ninput.%d.address = %d\n"
                          "input.%d.naddress = 9\ninput.%d.interval_ms = 1000\ninput.%d.channel.max = 9\n",
                          i, i, i, i * 0x100, i, i, i);
            for(int c = 1; c <= 9; ++c)
            {
                n += snprintf(body + n, sizeof(body) - n,
                              "input.%d.channel.%d.offset = %d\ninput.%d.channel.%d.format = uint_ab\n"
                              "input.%d.channel.%d.topic = b/%%i/%d/%d\n",
                              i, c, (c - 1) * 2, i, c, i, c, i, c);
            }
        }
        CHECK_INT(parse_text(body, NULL, 0), 0, "32 modules x 10 inputs x 9 channels");
        CHECK_INT(modbusmq_config_get()->input_max, 320, "...320 inputs");
        CHECK_STR(modbusmq_config_get()->inputs[319].channels[8].topic, "b/31/10/9", "...last topic");
        CHECK_INT(modbusmq_config_get()->inputs[319].slave, 70, "...last slave");
    }

    modbusmq_config_close();
}

int
main(int argc, char **argv)
{
    if (argc < 2)
    {
        fprintf(stderr, "usage: %s <modules.config>\n", argv[0]);
        return 2;
    }

    test_file_values(argv[1]);
    test_overrides(argv[1]);
    test_rules();

    return test_summary("test_modules");
}
