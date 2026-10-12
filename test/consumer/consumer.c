//////////////////////////////////////////////////////////////////////////////
//
// consumer.c
//
// Includes every installed header, routes the library's log into a handler,
// and checks a config parse error arrives there instead of on stdout/stderr.
//
#include "modbusmq.h"
#include "modbusmq_config.h"
#include "modbusmq_log.h"

#include <stdio.h>
#include <string.h>

static int errors_seen = 0;

static void
log_handler(int level, const char *message, void *user)
{
    int *count = user;

    if (level == MODBUSMQ_LOG_ERROR)
    {
        (*count)++;
    }

    if (strchr(message, '\n'))
    {
        printf("FAIL: handler got a newline: '%s'\n", message);
        (*count) += 1000;
    }
}

int
main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    modbusmq_log_set_handler(log_handler, &errors_seen);

    if (modbusmq_config_parse("/nonexistent/modbusmq.config") >= 0)
    {
        printf("FAIL: parse of a missing file succeeded\n");
        return 1;
    }

    if (errors_seen < 1 || errors_seen >= 1000)
    {
        printf("FAIL: handler saw %d error lines\n", errors_seen);
        return 1;
    }

    printf("ok: %s, handler saw %d error line(s)\n", MODBUSMQ_VERSION_STRING, errors_seen);
    return 0;
}
