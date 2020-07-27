/*-
 * Copyright (c) 2014-2020 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "util.h"

static int
usage(void)
{
    static const char *options[] = {"-F",
      "force salvage (by default salvage will refuse to salvage tables that fail basic tests)",
      NULL, NULL};

    util_usage("salvage [-F] uri", "options:", options);
    return (1);
}

int
util_salvage(WT_SESSION *session, int argc, char *argv[])
{
    WT_DECL_RET;
    int ch;
    char *uri;
    const char *force;

    force = NULL;
    uri = NULL;
    while ((ch = __wt_getopt(progname, argc, argv, "F")) != EOF)
        switch (ch) {
        case 'F':
            force = "force";
            break;
        case '?':
        default:
            return (usage());
        }
    argc -= __wt_optind;
    argv += __wt_optind;

    /* The remaining argument is the file name. */
    if (argc != 1)
        return (usage());
    if ((uri = util_uri(session, *argv, "file")) == NULL)
        return (1);

    if ((ret = session->salvage(session, uri, force)) != 0)
        (void)util_err(session, ret, "session.salvage: %s", uri);
    else {
        /*
         * Verbose configures a progress counter, move to the next line.
         */
        if (verbose)
            printf("\n");
    }

    free(uri);
    return (ret);
}
