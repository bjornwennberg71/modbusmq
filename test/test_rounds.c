//////////////////////////////////////////////////////////////////////////////
//
// bjornwennberg71@gmail.com
//
// test_rounds.c
//
// input.query_mode = rounds against a running modbusmq_server: the order the
// requests go out in, the round-robin over the deep inputs, the focus slave,
// input.round_interval, a failed request inside a round, and a one-shot write
// jumping ahead of the polls left in a round.
//
// usage: test_rounds <rounds.config>
//
#include "modbusmq.h"
#include "modbusmq_config.h"
#include "modbusmq_time.h"
#include "test_common.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <poll.h>

#define LOG_MAX    1024
#define ROUND_MAX  64
#define NSLAVES    4

//
// One completed request, in completion order. kind is 'R' for a read handed
// to the subscription callback, 'E' for one given up on, 'W' for the one-shot
// write.
//
typedef struct entry_t
{
    uint32_t round;
    int      slave;
    int      address;
    char     kind;
} entry_t;

static entry_t     entries[LOG_MAX];
static int         nentries = 0;

// what the test saw when each round started, by round number
static millitime_t round_start[ROUND_MAX];
static int         round_focus[ROUND_MAX];

// the one-shot write test: post when this many reads of the round are in
static int         write_armed   = 0;
static int         write_trigger = -1;

static uint32_t
current_round(struct modbusmq_context_t *context)
{
    uint32_t
        round = 0;

    modbusmq_round_stats(context, &round, NULL);
    return round;
}

static void
record(struct modbusmq_context_t *context, modbusmq_msg_t *msg, char kind)
{
    if (nentries >= LOG_MAX)
    {
        return;
    }

    entry_t
        *e = &entries[nentries++];

    e->round   = current_round(context);
    e->slave   = modbusmq_frame_slave(context, &msg->frame[0]);
    e->address = modbusmq_frame_addr(context, &msg->frame[0]);
    e->kind    = kind;
}

static int
reads_in_round(uint32_t round)
{
    int
        n = 0;

    for(int i = 0; i < nentries; ++i)
    {
        if (entries[i].round == round && entries[i].kind != 'W')
        {
            n++;
        }
    }
    return n;
}

static void
on_subscription(struct modbusmq_context_t *context, modbusmq_msg_t *msg, struct modbusmq_input_t *input)
{
    (void)input;
    record(context, msg, 'R');

    if (write_armed && reads_in_round(current_round(context)) == 2)
    {
        modbusmq_msg_t
            one;
        memset(&one, 0, sizeof(one));

        modbusmq_set_slave(context, 2);
        modbusmq_frame_write_register(context, &one.frame[0], 0x0300, 1);

        CHECK_INT(modbusmq_post(context, &one), 0, "one-shot write posted mid-round");

        write_armed   = 0;
        write_trigger = nentries - 1;
    }
}

static void
on_message(struct modbusmq_context_t *context, modbusmq_msg_t *msg)
{
    record(context, msg, 'W');
}

static void
on_error(struct modbusmq_context_t *context, modbusmq_msg_t *msg, int error)
{
    (void)error;
    record(context, msg, 'E');
}

//
// @brief runs the event loop until round stop_round + 1 has started
//
// Stops right after the round is posted and before any of it is sent, so a
// change made between two calls lands on the round after that one.
//
// @return 0 when the round was reached, < 0 on a transport error or a timeout
//
static int
pump_until(struct modbusmq_context_t *context, uint32_t stop_round)
{
    millitime_t
        deadline = millitime() + 10000;

    while(millitime() < deadline)
    {
        struct pollfd
            pollfds[1];
        millitime_t
            millisleep = 50;
        uint32_t
            before = current_round(context);

        memset(pollfds, 0, sizeof(pollfds));

        int
            fd = modbusmq_loop_prepare(context, &millisleep, &pollfds[0].events);

        if (fd < 0)
        {
            return MODBUSMQ_ERR_TRANSPORT;
        }

        uint32_t
            after = current_round(context);

        if (after != before && after < ROUND_MAX)
        {
            modbusmq_round_start(context, &round_start[after]);
            round_focus[after] = modbusmq_get_focus(context);
        }

        if (after > stop_round)
        {
            return 0;
        }

        pollfds[0].fd      = fd;
        pollfds[0].events |= POLLERR | POLLHUP;

        if (poll(pollfds, 1, millisleep) < 0)
        {
            break;
        }

        if (pollfds[0].revents & (POLLIN | POLLOUT | POLLERR | POLLHUP))
        {
            modbusmq_loop_write_read(context, pollfds[0].revents);
        }
    }

    return -1;
}

//
// @brief compares one round's requests against what the algorithm says
//
// Every fast input in config order, then the two deep inputs of the slave the
// round-robin is on, then the focus slave's when that is another one.
//
static void
check_round(uint32_t round)
{
    int
        want_slave[32],
        want_addr[32],
        nwant = 0,
        deep  = (int)((round - 1) % NSLAVES) + 1,
        focus = round_focus[round];

    for(int s = 1; s <= NSLAVES; ++s)
    {
        want_slave[nwant] = s;
        want_addr[nwant++] = 0x0000;
    }

    want_slave[nwant] = deep;
    want_addr[nwant++] = 0x0100;
    want_slave[nwant] = deep;
    want_addr[nwant++] = 0x0200;

    if (focus >= 0 && focus != deep)
    {
        want_slave[nwant] = focus;
        want_addr[nwant++] = 0x0100;
        want_slave[nwant] = focus;
        want_addr[nwant++] = 0x0200;
    }

    char
        line[512],
        what[128];
    int
        len  = 0,
        n    = 0,
        good = 1;

    len += snprintf(line + len, sizeof(line) - len, "    round %2u (deep %d, focus %2d):", round, deep, focus);

    for(int i = 0; i < nentries; ++i)
    {
        entry_t
            *e = &entries[i];

        if (e->round != round)
        {
            continue;
        }

        if (e->kind == 'W')
        {
            len += snprintf(line + len, sizeof(line) - len, " WRITE");
            continue;
        }

        len += snprintf(line + len, sizeof(line) - len, " %d:%04x%s", e->slave, e->address, e->kind == 'E' ? "!" : "");

        if (n >= nwant || e->slave != want_slave[n] || e->address != want_addr[n])
        {
            good = 0;
        }
        n++;
    }

    printf("%s\n", line);

    snprintf(what, sizeof(what), "round %u: requests in the expected order", round);
    CHECK(good && n == nwant, what);
}

int
main(int argc, char **argv)
{
    printf("== test_rounds ==\n");

    if (argc < 2)
    {
        printf("  usage: test_rounds <rounds.config>\n");
        return 2;
    }

    if (modbusmq_config_parse(argv[1]) != 0)
    {
        printf("  FAIL: unable to parse %s\n", argv[1]);
        return 2;
    }

    modbusmq_config_t
        *config = modbusmq_config_get();

    //
    // the config keys
    //
    printf("config\n");

    CHECK_INT(config->query_mode, modbusmq_query_mode_rounds, "input.query_mode = rounds");
    CHECK_INT(config->round_interval_ms, 0, "input.round_interval = 0");
    CHECK_INT(config->inputs[0].tier, modbusmq_tier_fast, "input.1.tier = fast");
    CHECK_INT(config->inputs[1].tier, modbusmq_tier_deep, "input.2.tier = deep");

    struct modbusmq_context_t
        *context = modbusmq_tcp_context(config->modbusmq_connect);

    CHECK(context != NULL, "context created");
    if (!context)
    {
        return test_summary("test_rounds");
    }

    modbusmq_set_config(context, config);
    modbusmq_frame_timeout(context, config->modbusmq_frame_timeout_ms);
    modbusmq_set_subscription_callback(context, on_subscription);
    modbusmq_set_message_callback(context, on_message);
    modbusmq_set_error_callback(context, on_error);

    CHECK_INT(modbusmq_connect(context), 0, "connected");

    // subscribe the way modbusmq does, interval and all
    for(int i = 0; i < config->input_max; ++i)
    {
        modbusmq_input_t
            *input = &config->inputs[i];
        modbusmq_msg_t
            msg;

        memset(&msg, 0, sizeof(msg));
        modbusmq_set_slave(context, input->slave);
        modbusmq_frame_read_input_registers(context, &msg.frame[0], input->address, input->naddress);

        CHECK_INT(modbusmq_subscribe(context, &msg, input->interval), 0, "subscribe accepted");
    }

    // modbusmq calls this after subscribing; a round has no interval to assert on
    modbusmq_timer_debug_print(context);

    //
    // focus API
    //
    printf("focus\n");

    CHECK_INT(modbusmq_get_focus(context), -1, "no focus to start with");
    CHECK(modbusmq_set_focus(context, 9) < 0, "focus on a slave with no deep input is refused");
    CHECK_INT(modbusmq_get_focus(context), -1, "a refused focus changes nothing");

    config->query_mode = modbusmq_query_mode_parallell;
    CHECK(modbusmq_set_focus(context, 2) < 0, "focus outside rounds mode is refused");
    config->query_mode = modbusmq_query_mode_rounds;

    //
    // 1. round-robin, no focus: rounds 1..5
    //
    printf("round-robin\n");
    CHECK_INT(pump_until(context, 5), 0, "five rounds ran");

    //
    // 2. focus on slave 4: rounds 7..10 read it on top, round 10 visits it anyway
    //
    printf("focus on slave 4\n");
    CHECK_INT(modbusmq_set_focus(context, 4), 0, "focus on slave 4 accepted");
    CHECK_INT(modbusmq_get_focus(context), 4, "focus reads back");
    CHECK_INT(pump_until(context, 10), 0, "rounds with focus ran");

    //
    // 3. round_interval: rounds 12..15 start at least 150 ms apart
    //
    printf("round_interval\n");
    CHECK_INT(modbusmq_set_focus(context, -1), 0, "focus cleared");
    config->round_interval_ms = 150;
    CHECK_INT(pump_until(context, 15), 0, "rounds with an interval ran");

    //
    // 4. a one-shot write inside round 17 goes out before its remaining polls
    //
    printf("one-shot write\n");
    config->round_interval_ms = 0;
    CHECK_INT(pump_until(context, 16), 0, "round 17 started");
    write_armed = 1;
    CHECK_INT(pump_until(context, 17), 0, "round 17 ran");

    //
    // the order, round by round
    //
    printf("request order\n");
    for(uint32_t r = 1; r <= 17; ++r)
    {
        check_round(r);
    }

    for(uint32_t r = 7; r <= 10; ++r)
    {
        CHECK_INT(round_focus[r], 4, "focus applies from the round after it is set");
    }
    CHECK_INT(round_focus[6], -1, "the round already posted keeps its own focus");

    for(uint32_t r = 13; r <= 16; ++r)
    {
        char
            what[96];
        snprintf(what, sizeof(what), "round %u starts >= 150 ms after round %u (%d ms)",
                 r, r - 1, (int)(round_start[r] - round_start[r - 1]));
        CHECK(round_start[r] - round_start[r - 1] >= 150, what);
        printf("    %s\n", what);
    }

    //
    // slave 3's second deep input fails in every round that visits slave 3:
    // rounds 3, 7, 11 and 15. Reported, and the round still completed.
    //
    int
        errors = 0;
    for(int i = 0; i < nentries; ++i)
    {
        if (entries[i].kind == 'E')
        {
            errors++;
            CHECK(entries[i].slave == 3 && entries[i].address == 0x0200, "only the oversized read fails");
        }
    }
    CHECK_INT(errors, 4, "the failed read is reported once per round visiting slave 3");

    CHECK(write_trigger >= 0, "the write was posted");
    if (write_trigger >= 0 && write_trigger + 1 < nentries)
    {
        entry_t
            *w = &entries[write_trigger + 1];

        CHECK(w->kind == 'W', "the write completes right after the poll in flight");
        CHECK_INT(w->round, 17, "...inside the same round");
        CHECK(reads_in_round(17) > 2, "...ahead of polls still left in that round");
    }

    uint32_t
        count   = 0;
    int
        last_ms = -1;
    CHECK_INT(modbusmq_round_stats(context, &count, &last_ms), 0, "round stats");
    CHECK(count >= 18, "round count");
    CHECK(last_ms >= 0 && last_ms < 1000, "last round duration");

    modbusmq_close(context);
    modbusmq_reset_queue(context);
    modbusmq_free(context);

    return test_summary("test_rounds");
}
