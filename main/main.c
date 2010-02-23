/*******************************************************************************
 * Copyright (c) 2007, 2009 Wind River Systems, Inc. and others.
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the Eclipse Public License v1.0
 * and Eclipse Distribution License v1.0 which accompany this distribution.
 * The Eclipse Public License is available at
 * http://www.eclipse.org/legal/epl-v10.html
 * and the Eclipse Distribution License is available at
 * http://www.eclipse.org/org/documents/edl-v10.php.
 *
 * Contributors:
 *     Wind River Systems - initial API and implementation
 *******************************************************************************/

/*
 * Agent main module.
 */

#define CONFIG_MAIN
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <assert.h>
#include "asyncreq.h"
#include "events.h"
#include "errors.h"
#include "trace.h"
#include "test.h"
#include "cmdline.h"
#include "channel_tcp.h"

static const char * progname;
static Protocol * proto;
static ChannelServer * serv;
static TCFBroadcastGroup * bcg;

static void channel_new_connection(ChannelServer * serv, Channel * c) {
    protocol_reference(proto);
    c->protocol = proto;
    channel_set_broadcast_group(c, bcg);
    channel_start(c);
}

#if defined(_WRS_KERNEL)
int tcf(void) {
#else
int main(int argc, char ** argv) {
#endif
    int c;
    int ind;
    int daemon = 0;
    int interactive = 0;
    const char * log_name = NULL;
    const char * url = "TCP:";
    PeerServer * ps = NULL;

    ini_mdep();
    ini_trace();
    ini_asyncreq();

#if defined(_WRS_KERNEL)

    progname = "tcf";
    open_log_file("-");
    log_mode = 0;

#else

    progname = argv[0];

    /* Parse arguments */
    for (ind = 1; ind < argc; ind++) {
        const char * s = argv[ind];
        if (*s != '-') {
            break;
        }
        s++;
        while ((c = *s++) != '\0') {
            switch (c) {
            case 'i':
                interactive = 1;
                break;

            case 't':
#if ENABLE_RCBP_TEST
                test_proc();
#endif
                exit(0);
                break;

            case 'd':
                daemon = 1;
                break;

            case 'c':
                generate_ssl_certificate();
                exit(0);
                break;

            case 'l':
            case 'L':
            case 's':
                if (*s == '\0') {
                    if (++ind >= argc) {
                        fprintf(stderr, "%s: error: no argument given to option '%c'\n", progname, c);
                        exit(1);
                    }
                    s = argv[ind];
                }
                switch (c) {
                case 'l':
                    log_mode = strtol(s, 0, 0);
                    break;

                case 'L':
                    log_name = s;
                    break;

                case 's':
                    url = s;
                    break;

                default:
                    fprintf(stderr, "%s: error: illegal option '%c'\n", progname, c);
                    exit(1);
                }
                s = "";
                break;

            default:
                fprintf(stderr, "%s: error: illegal option '%c'\n", progname, c);
                exit(1);
            }
        }
    }

    if (daemon) become_daemon();
    open_log_file(log_name);

#endif

    ini_events_queue();

    bcg = broadcast_group_alloc();
    proto = protocol_alloc();

    /* The static services must be initialised before the plugins */
#if ENABLE_Cmdline
    if (interactive) ini_cmdline_handler(interactive, proto);
#else
    if (interactive) fprintf(stderr, "Warning: This version does not support interactive mode.\n");
#endif

    ini_services(proto, bcg);

    ps = channel_peer_from_url(url);
    if (ps == NULL) {
        fprintf(stderr, "%s: invalid server URL (-s option value): %s\n", progname, url);
        exit(1);
    }
    serv = channel_server(ps);
    if (serv == NULL) {
        fprintf(stderr, "%s: cannot create TCF server: %s\n", progname, errno_to_str(errno));
        exit(1);
    }
    serv->new_conn = channel_new_connection;

    discovery_start();

    /* Process events - must run on the initial thread since ptrace()
     * returns ECHILD otherwise, thinking we are not the owner. */
    run_event_loop();

#if ENABLE_Plugins
    plugins_destroy();
#endif /* ENABLE_Plugins */

    return 0;
}
