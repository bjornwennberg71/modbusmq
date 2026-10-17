//////////////////////////////////////////////////////////////////////////////
//
// bjornwennberg71@gmail.com
//
// test_loop.c
//
// The event loop: modbusmq_loop_prepare()/modbusmq_loop_write_read() driving
// subscriptions and one-shot posts against a running modbusmq_server, plus the
// sleep_time in/out contract.
//
// usage: test_loop <config-file>
//
#include "modbusmq.h"
#include "modbusmq_config.h"
#include "modbusmq_time.h"
#include "test_common.h"

#include <stdio.h>
#include <string.h>
#include <poll.h>

//
// callback bookkeeping. Single-threaded library, so plain globals are fine.
//
static int   subscription_calls = 0;
static int   message_calls      = 0;
static int   error_calls        = 0;
static float last_voltage       = 0.0f;
static int   voltage_seen       = 0;

static void
on_subscription(struct modbusmq_context_t *context, modbusmq_msg_t *msg, struct modbusmq_input_t *input)
{
    subscription_calls++;

    for(int c = 0; c < input->channel_max; ++c)
    {
        modbusmq_channel_t
            *channel = &input->channels[c];

        if (!channel->topic || strcmp(channel->topic, "t/voltage") != 0)
        {
            continue;
        }

        if (modbusmq_channel_in_range(context, msg, input, channel) == 0)
        {
            last_voltage = modbusmq_read_channel(context, msg, input, channel);
            voltage_seen++;
        }
    }
}

static void
on_message(struct modbusmq_context_t *context, modbusmq_msg_t *msg)
{
    (void)context;
    (void)msg;
    message_calls++;
}

static void
on_error(struct modbusmq_context_t *context, modbusmq_msg_t *msg, int error)
{
    (void)context;
    (void)msg;
    (void)error;
    error_calls++;
}

//
// @brief runs the event loop for a while, exactly as a real program would
//
// @return 0 on a clean run, the loop's error code if it reported one
//
static int
pump(struct modbusmq_context_t *context, int for_ms)
{
    millitime_t
        deadline = millitime() + for_ms;
    int
        last_rc = 0;

    while(millitime() < deadline)
    {
        struct pollfd
            pollfds[2];
        millitime_t
            millisleep = 50;

        memset(pollfds, 0, sizeof(pollfds));

        int
            fd = modbusmq_loop_prepare(context, &millisleep, &pollfds[0].events);

        if (fd < 0)
        {
            return MODBUSMQ_ERR_TRANSPORT;
        }

        pollfds[0].fd      = fd;
        pollfds[0].events |= POLLERR | POLLHUP;

        if (poll(pollfds, 1, millisleep) < 0)
        {
            break;
        }

        if (pollfds[0].revents & (POLLIN | POLLOUT | POLLERR | POLLHUP))
        {
            int rc = modbusmq_loop_write_read(context, pollfds[0].revents);
            if (rc < 0)
            {
                last_rc = rc;
            }
        }
    }

    return last_rc;
}

//
// modbusmq_raw_request() reply, kept for the checks
//
static char raw_reply[256];
static int  raw_replies = 0;

static void
on_raw_reply(struct modbusmq_context_t *context, const char *reply, void *user)
{
    (void)context;
    (void)user;
    snprintf(raw_reply, sizeof(raw_reply), "%s", reply);
    raw_replies++;
}

// post one raw request and pump until it is answered
static int
raw(struct modbusmq_context_t *context, const char *text)
{
    int
        before = raw_replies,
        rc = modbusmq_raw_request(context, text, on_raw_reply, NULL);

    for(int i = 0; i < 40 && raw_replies == before; ++i)
    {
        pump(context, 50);
    }
    return rc;
}

static void
test_raw_request(struct modbusmq_context_t *context, modbusmq_config_t *config)
{
    printf("raw request\n");

    CHECK_INT(raw(context, " 1 4 0 2\n"), 0, "raw read posted");
    CHECK_STR(raw_reply, "1 4 0 2 -> 0x12C0 0xFFCE", "...registers as hex words");

    raw(context, "@probe 1 3 0x100 1");
    CHECK_STR(raw_reply, "@probe 1 3 0x100 1 -> 0x04D2", "tag echoed with a holding register");

    raw(context, "1 1 0 3");
    CHECK_STR(raw_reply, "1 1 0 3 -> 101", "coils as a 0/1 string, lowest address first");

    CHECK(raw(context, "1 6 0x100 7") < 0, "write refused without raw_write");
    CHECK_STR(raw_reply, "1 6 0x100 7 -> error refused writes need modbusmq.raw_write = 1", "...and says why");
    CHECK(raw(context, "1 4") < 0, "short request refused");
    CHECK(raw(context, "1 4 0 126") < 0, "126 registers refused");
    CHECK(raw(context, "1 4 0 x") < 0, "garbage refused");

    int
        before = raw_replies;
    CHECK_INT(modbusmq_raw_request(context, "1 4 0 1", on_raw_reply, NULL), 0, "first of two posted");
    CHECK(modbusmq_raw_request(context, "1 4 0 1", on_raw_reply, NULL) < 0, "second refused while one is in flight");
    CHECK_STR(raw_reply, "1 4 0 1 -> error refused busy, one request at a time", "...as busy");
    for(int i = 0; i < 40 && raw_replies < before + 2; ++i)
    {
        pump(context, 50);
    }
    CHECK_STR(raw_reply, "1 4 0 1 -> 0x12C0", "the first is still answered");

    config->raw_write = 1;
    raw(context, "1 6 0x100 7");
    CHECK_STR(raw_reply, "1 6 0x100 7 -> ok", "write with raw_write = 1");
    raw(context, "1 15 0 1 0 1");
    CHECK_STR(raw_reply, "1 15 0 1 0 1 -> ok", "coil block write");
    config->raw_write = 0;
    CHECK(raw(context, "0 6 0x100 7") < 0, "broadcast refused without raw_write");
}

//
// sleep_time is in/out: the caller's value is a ceiling, 0 asks for the default
//
static void
test_sleep_time(struct modbusmq_context_t *context)
{
    printf("sleep_time contract\n");

    int16_t     events;
    millitime_t t;

    t = 50;  events = 0;
    CHECK(modbusmq_loop_prepare(context, &t, &events) >= 0, "loop_prepare with a 50 ms ceiling");
    CHECK(t <= 50, "caller ceiling of 50 is not raised");

    t = 0;   events = 0;
    CHECK(modbusmq_loop_prepare(context, &t, &events) >= 0, "loop_prepare with no ceiling");
    CHECK(t > 0 && t <= 1000, "zero asks for the default and gets at most 1000");

    t = 100000; events = 0;
    CHECK(modbusmq_loop_prepare(context, &t, &events) >= 0, "loop_prepare with a huge ceiling");
    CHECK(t <= 100000, "a due subscription only lowers the ceiling");
}

int
main(int argc, char **argv)
{
    printf("== test_loop ==\n");

    if (argc < 2)
    {
        printf("  usage: test_loop <config-file>\n");
        return 2;
    }

    if (modbusmq_config_parse(argv[1]) != 0)
    {
        printf("  FAIL: unable to parse %s\n", argv[1]);
        return 2;
    }

    modbusmq_config_t
        *config = modbusmq_config_get();

    struct modbusmq_context_t
        *context = modbusmq_tcp_context(config->modbusmq_connect);

    CHECK(context != NULL, "context created");
    if (!context)
    {
        return test_summary("test_loop");
    }

    modbusmq_set_config(context, config);
    modbusmq_frame_timeout(context, config->modbusmq_frame_timeout_ms);
    modbusmq_set_subscription_callback(context, on_subscription);
    modbusmq_set_message_callback(context, on_message);
    modbusmq_set_error_callback(context, on_error);

    CHECK_INT(modbusmq_connect(context), 0, "connected");

    //
    // subscriptions
    //
    printf("subscriptions\n");

    modbusmq_input_t
        *input = &config->inputs[0];

    modbusmq_msg_t
        msg;
    memset(&msg, 0, sizeof(msg));

    modbusmq_set_slave(context, input->slave);
    modbusmq_frame_read_input_registers(context, &msg.frame[0], input->address, input->naddress);

    CHECK_INT(modbusmq_subscribe(context, &msg, input->interval), 0, "subscribe accepted");

    // the message is copied on subscribe, so scribbling on ours must not matter
    memset(&msg, 0xAA, sizeof(msg));

    CHECK_INT(pump(context, 1200), 0, "loop ran without a transport error");

    // interval is 200 ms in the fixture, so 1200 ms is at least four polls;
    // assert conservatively to stay reliable on a loaded machine
    CHECK(subscription_calls >= 2, "subscription callback fired repeatedly");
    CHECK(voltage_seen > 0, "subscription decoded t/voltage");
    CHECK_FLT(last_voltage, 48.00, 0.001, "subscribed value is correct after the copy");
    CHECK_INT(error_calls, 0, "no errors during a healthy run");

    //
    // one-shot post alongside the running subscription
    //
    printf("post\n");

    int
        before = message_calls;

    modbusmq_msg_t
        one;
    memset(&one, 0, sizeof(one));

    modbusmq_set_slave(context, input->slave);
    modbusmq_frame_read_input_registers(context, &one.frame[0], input->address, input->naddress);

    CHECK_INT(modbusmq_post(context, &one), 0, "post accepted");

    pump(context, 600);

    CHECK(message_calls > before, "message callback fired for the posted one-shot");

    //
    // sleep_time contract
    //
    test_sleep_time(context);

    //
    // a negative mswait must not turn into an unbounded wait
    //
    printf("send timeout clamping\n");
    {
        modbusmq_msg_t
            neg;
        memset(&neg, 0, sizeof(neg));

        modbusmq_set_slave(context, input->slave);
        modbusmq_frame_read_input_registers(context, &neg.frame[0], input->address, input->naddress);

        millitime_t
            t0 = millitime();

        modbusmq_send(context, &neg, -1);

        millitime_t
            elapsed = millitime() - t0;

        CHECK(elapsed < 1000, "negative mswait returns promptly instead of waiting forever");
    }

    test_raw_request(context, config);

    //
    // modbusmq.frame_delay_ms: no request goes out within the gap after an answer,
    // and loop_prepare sleeps out what is left of it instead of asking for POLLOUT
    //
    printf("frame delay\n");
    {
        config->modbusmq_frame_delay_ms = 50;

        modbusmq_msg_t
            gap[2];
        memset(gap, 0, sizeof(gap));

        for(int i = 0; i < 2; ++i)
        {
            modbusmq_set_slave(context, input->slave);
            modbusmq_frame_read_input_registers(context, &gap[i].frame[0], input->address, input->naddress);
            CHECK_INT(modbusmq_post(context, &gap[i]), 0, "gap post accepted");
        }

        int
            first_calls = message_calls + 1,
            prepare_checked = 0;
        millitime_t
            first_done = 0,
            deadline = millitime() + 1000;

        while(message_calls < first_calls + 1 && millitime() < deadline)
        {
            struct pollfd
                pfd;
            millitime_t
                millisleep = 500;

            memset(&pfd, 0, sizeof(pfd));
            pfd.fd = modbusmq_loop_prepare(context, &millisleep, &pfd.events);

            if (first_done && !prepare_checked)
            {
                CHECK(!(pfd.events & POLLOUT), "no POLLOUT inside the gap");
                CHECK(millisleep > 0 && millisleep <= 50, "loop_prepare sleeps out the gap");
                prepare_checked = 1;
            }

            poll(&pfd, 1, millisleep);
            modbusmq_loop_write_read(context, pfd.revents);

            if (!first_done && message_calls >= first_calls)
            {
                first_done = millitime();
            }
        }

        CHECK_INT(message_calls, first_calls + 1, "both gap posts answered");
        CHECK(prepare_checked, "loop_prepare was checked inside the gap");
        CHECK(millitime() - first_done >= 50, "second answer came no sooner than the gap");

        config->modbusmq_frame_delay_ms = 0;
    }

    //
    // a closed connection is reported, not hidden
    //
    printf("teardown\n");

    modbusmq_close(context);

    int16_t     events = 0;
    millitime_t t      = 100;
    CHECK(modbusmq_loop_prepare(context, &t, &events) < 0, "loop_prepare reports a closed connection");

    modbusmq_reset_queue(context);
    modbusmq_free(context);

    return test_summary("test_loop");
}
