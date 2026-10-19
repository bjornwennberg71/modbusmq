//////////////////////////////////////////////////////////////////////////////
//
// bjornwennberg71@gmail.com
//
// modbusmq_config.c
//
// Check a config for faults, and with --convert rewrite the key names that
// changed (2.5.3: time keys carry their unit) so an old config keeps working.
// The library knows only the new names; the old ones live here.
//

// INCLUDES //////////////////////////////////////////////////////////////////
#include "modbusmq.h"
#include "modbusmq_config.h"
#include "modbusmq_log.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>


const char *version = MODBUSMQ_VERSION_STRING;

//
// global parameters from the argument line
//
typedef struct global_info
{
    const char *filename;
    int         convert;  // --convert: print the converted file to stdout
    int         warnings; // log lines seen during a successful parse
} global_info;

static global_info GI;

//
// old key -> new key. Exact names first, then any key ending in one of the
// interval suffixes gains _ms, wherever it sits (input.N, channel, publish).
//
static const char *renamed_keys[][2] =
{
    { "modbusmq.frame_timeout",   "modbusmq.frame_timeout_ms" },
    { "modbusmq.rts_delay",       "modbusmq.rts_delay_us" },
    { "modbusmq.slave_retry_max", "modbusmq.slave_retry_max_ms" },
    { "modbusmq.request_gap",     "modbusmq.frame_delay_ms" },
};

static const char *interval_suffixes[] = { ".interval", ".min_interval", ".max_interval", ".round_interval" };

//
// print help
//
void
print_help(int argc, char **argv)
{
    (void)argc;
    printf("usage: %s -c file.config [--convert]\n", argv[0]);
    printf("-c file    : the config to check\n");
    printf("--convert  : rename old keys and print the converted config to stdout;\n");
    printf("             the check then runs on the converted text\n");
    printf("--version  : print the version\n");
    printf("\n");
    printf("Findings go to stderr. Exit code: 0 clean, 1 warnings, 255 errors.\n");
}

//////////////////////////////////////////////////////////////////////////////
//
// parse arguments
//
int
parse_argv(int argc, char **argv)
{
    for(int a = 1; a < argc; ++a)
    {
        if (strcmp(argv[a], "-h") == 0 || strcmp(argv[a], "--help") == 0)
        {
            print_help(argc, argv);
            return 1;
        }
        else if (strcmp(argv[a], "--version") == 0)
        {
            printf("modbusmq_config version: %s\n", version);
            return 1;
        }
        else if (strcmp(argv[a], "--convert") == 0)
        {
            GI.convert = 1;
        }
        else if (strcmp(argv[a], "-c") == 0 && a + 1 < argc)
        {
            GI.filename = argv[++a];
        }
        else
        {
            fprintf(stderr, "Unknown argument: %s\n", argv[a]);
            return -1;
        }
    }

    if (!GI.filename)
    {
        print_help(argc, argv);
        return -1;
    }
    return 0;
}

//
// @brief the new name of an old key
//
// @return newname filled in, or NULL when key was not renamed
//
static const char *
renamed_key(const char *key, char *newname, size_t size)
{
    size_t
        nkey = strlen(key);

    for(size_t i = 0; i < sizeof(renamed_keys) / sizeof(renamed_keys[0]); ++i)
    {
        if (strcmp(key, renamed_keys[i][0]) == 0)
        {
            snprintf(newname, size, "%s", renamed_keys[i][1]);
            return newname;
        }
    }
    for(size_t i = 0; i < sizeof(interval_suffixes) / sizeof(interval_suffixes[0]); ++i)
    {
        size_t
            nsuffix = strlen(interval_suffixes[i]);
        if (nkey >= nsuffix && strcmp(key + nkey - nsuffix, interval_suffixes[i]) == 0)
        {
            snprintf(newname, size, "%s_ms", key);
            return newname;
        }
    }
    return NULL;
}

//
// @brief write line to out with its key renamed, everything else untouched
//
// Comments and blank lines pass through, a commented-out key included.
//
// @return 1 when the key was renamed, 0 otherwise
//
static int
convert_line(const char *line, FILE *out)
{
    const char
        *start = line + strspn(line, " \t"),
        *end   = start + strcspn(start, " \t=\r\n");
    char
        key[256],
        newname[264];

    if (*start == '#' || end == start || !strchr(end, '=') || (size_t)(end - start) >= sizeof(key))
    {
        fputs(line, out);
        return 0;
    }

    snprintf(key, sizeof(key), "%.*s", (int)(end - start), start);
    if (!renamed_key(key, newname, sizeof(newname)))
    {
        fputs(line, out);
        return 0;
    }

    fprintf(out, "%.*s%s%s", (int)(start - line), line, newname, end);
    fprintf(stderr, "%s: %s -> %s\n", GI.filename, key, newname);
    return 1;
}

//
// @brief collect the library's findings instead of letting them print
//
static void
log_handler(int level, const char *message, void *user)
{
    (void)level;
    (void)user;
    fprintf(stderr, "%s: %s\n", GI.filename, message);
    GI.warnings++;
}

//
// @brief a key the library skipped: a typo, or a key another program keeps here
//
static void
unknown_handler(int line, const char *key, const char *value, void *user)
{
    (void)value;
    (void)user;
    fprintf(stderr, "%s: %d: %s: unknown key\n", GI.filename, line, key);
    GI.warnings++;
}

//
// @brief faults the parser accepts but that will not work as meant
//
static void
check_config(modbusmq_config_t *config)
{
    if (config->modbusmq_frame_delay_ms >= config->modbusmq_frame_timeout_ms)
    {
        fprintf(stderr, "%s: modbusmq.frame_delay_ms %d is not below modbusmq.frame_timeout_ms %d; requests will time out before they are sent\n",
                GI.filename, config->modbusmq_frame_delay_ms, config->modbusmq_frame_timeout_ms);
        GI.warnings++;
    }
}

int
main(int argc, char **argv)
{
    int
        rc = 0,
        nrenamed = 0;
    char
        *line = NULL,
        *text = NULL;
    size_t
        line_len = 0,
        text_len = 0;
    FILE
        *in = NULL,
        *converted = NULL;

    memset(&GI, 0, sizeof(GI));

    rc = parse_argv(argc, argv);
    if (rc < 0)
    {
        return -1;
    }
    if (rc > 0)
    {
        return 0;
    }

    modbusmq_log_set_handler(log_handler, NULL);
    modbusmq_config_set_unknown_handler(unknown_handler, NULL);

    if (!GI.convert)
    {
        rc = modbusmq_config_parse(GI.filename);
        goto report;
    }

    in = fopen(GI.filename, "r");
    if (!in)
    {
        fprintf(stderr, "%s: unable to open\n", GI.filename);
        return -1;
    }

    converted = open_memstream(&text, &text_len);
    if (!converted)
    {
        fprintf(stderr, "%s: unable to allocate\n", GI.filename);
        rc = -1;
        goto cleanup;
    }

    while (getline(&line, &line_len, in) > 0)
    {
        nrenamed += convert_line(line, converted);
    }
    fclose(converted);
    converted = NULL;

    fwrite(text, 1, text_len, stdout);
    fprintf(stderr, "%s: %d key%s renamed\n", GI.filename, nrenamed, nrenamed == 1 ? "" : "s");

    rc = modbusmq_config_parse_text(text, text_len);

report:
    if (rc < 0)
    {
        fprintf(stderr, "%s: error, the library refuses this config\n", GI.filename);
        rc = -1;
        goto cleanup;
    }

    check_config(modbusmq_config_get());

    if (GI.warnings > 0)
    {
        fprintf(stderr, "%s: %d warning%s\n", GI.filename, GI.warnings, GI.warnings == 1 ? "" : "s");
        rc = 1;
        goto cleanup;
    }

    fprintf(stderr, "%s: ok\n", GI.filename);

cleanup:
    if (rc < 0 && !GI.convert)
    {
        fprintf(stderr, "%s: a config from before 2.5.3 needs --convert for its time keys\n", GI.filename);
    }
    if (in)
    {
        fclose(in);
    }
    free(line);
    free(text);
    return rc;
}
