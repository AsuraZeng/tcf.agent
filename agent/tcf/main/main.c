/*******************************************************************************
 * Copyright (c) 2007, 2012 Wind River Systems, Inc. and others.
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the Eclipse Public License v1.0
 * and Eclipse Distribution License v1.0 which accompany this distribution.
 * The Eclipse Public License is available at
 * http://www.eclipse.org/legal/epl-v10.html
 * and the Eclipse Distribution License is available at
 * http://www.eclipse.org/org/documents/edl-v10.php.
 * You may elect to redistribute this code under either of these licenses.
 *
 * Contributors:
 *     Wind River Systems - initial API and implementation
 *******************************************************************************/

/*
 * Agent main module.
 */

#include <tcf/config.h>

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <assert.h>
#include <signal.h>
#include <tcf/framework/asyncreq.h>
#include <tcf/framework/events.h>
#include <tcf/framework/errors.h>
#include <tcf/framework/trace.h>
#include <tcf/framework/myalloc.h>
#include <tcf/framework/exceptions.h>
#include <tcf/framework/channel_tcp.h>
#include <tcf/framework/plugins.h>
#include <tcf/services/discovery.h>
#include <tcf/main/test.h>
#include <tcf/main/cmdline.h>
#include <tcf/main/services.h>
#include <tcf/main/server.h>

#ifndef ENABLE_SignalHandlers
#  define ENABLE_SignalHandlers 1
#endif

#ifndef DEFAULT_SERVER_URL
#  define DEFAULT_SERVER_URL "TCP:"
#endif

static const char * progname;
static unsigned int idle_timeout;
static unsigned int idle_count;

static void check_idle_timeout(void * args) {
    if (list_is_empty(&channel_root)) {
        idle_count++;
        if (idle_count > idle_timeout) {
            trace(LOG_ALWAYS, "No connections for %d seconds, shutting down", idle_timeout);
            discovery_stop();
            cancel_event_loop();
            return;
        }
    }
    post_event_with_delay(check_idle_timeout, NULL, 1000000);
}

static void channel_closed(Channel *c) {
    /* Reset idle_count if there are short lived connections */
    idle_count = 0;
}

#if ENABLE_SignalHandlers

static void shutdown_event(void * args) {
    discovery_stop();
    cancel_event_loop();
}

static void signal_handler(int sig) {
    if (is_dispatch_thread()) {
        discovery_stop();
        signal(sig, SIG_DFL);
        raise(sig);
    }
    else {
        post_event(shutdown_event, NULL);
    }
}

#if defined(_WIN32)
static LONG NTAPI VectoredExceptionHandler(PEXCEPTION_POINTERS x) {
    if (is_dispatch_thread()) {
        DWORD exception_code = x->ExceptionRecord->ExceptionCode;
        if (exception_code == EXCEPTION_IN_PAGE_ERROR) {
            int error = ERR_OTHER;
            if (x->ExceptionRecord->NumberParameters >= 3) {
                ULONG status = (ULONG)x->ExceptionRecord->ExceptionInformation[2];
                if (status != 0) error = set_nt_status_errno(status);
            }
            str_exception(error, "In page error");
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static BOOL CtrlHandler(DWORD ctrl) {
    switch(ctrl) {
    case CTRL_C_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        post_event(shutdown_event, NULL);
        return TRUE;
    }
    return FALSE;
}
#endif

#endif /* ENABLE_SignalHandlers */

#if !defined(_WRS_KERNEL)
static const char * help_text[] = {
    "Usage: agent [OPTION]...",
    "Start Target Communication Framework agent.",
    "  -d               run in daemon mode (output is sent to system logger)",
#if ENABLE_Cmdline
    "  -i               run in interactive mode",
#endif
#if ENABLE_RCBP_TEST
    "  -t               run in diagnostic mode",
#endif
    "  -L<file>         enable logging, use -L- to send log to stderr",
    "  -l<level>        set log level, the level is comma separated list of:",
    "@",
    "  -s<url>          set agent listening port and protocol, default is " DEFAULT_SERVER_URL,
    "  -S               print server properties in Json format to stdout",
    "  -I<idle-seconds> exit if are no connections for the specified time",
#if ENABLE_Plugins
    "  -P<dir>          set agent plugins directory name",
#endif
#if ENABLE_SSL
    "  -c               generate SSL certificate and exit",
#endif
    NULL
};

static void show_help(void) {
    const char ** p = help_text;
    while (*p != NULL) {
        if (**p == '@') {
            struct trace_mode * tm = trace_mode_table;
            while (tm->mode != 0) {
                fprintf(stderr, "      %-12s %s (%#x)\n", tm->name, tm->description, tm->mode);
                tm++;
            }
            p++;
        }
        else {
            fprintf(stderr, "%s\n", *p++);
        }
    }
}
#endif

#if defined(_WRS_KERNEL)
int tcf(void);
int tcf(void) {
#else
int main(int argc, char ** argv) {
    int c;
    int ind;
    int daemon = 0;
    const char * log_name = NULL;
    const char * log_level = NULL;
#endif
    int interactive = 0;
    int print_server_properties = 0;
    const char * url = DEFAULT_SERVER_URL;
    Protocol * proto;
    TCFBroadcastGroup * bcg;

    ini_mdep();
    ini_trace();
    ini_events_queue();
    ini_asyncreq();

#if defined(_WRS_KERNEL)

    progname = "tcf";
    open_log_file("-");
    log_mode = 0;

#else

    progname = argv[0];

    /* Parse arguments */
    for (ind = 1; ind < argc; ind++) {
        char * s = argv[ind];
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
#if defined(_WIN32)
                /* For Windows the only way to detach a process is to
                 * create a new process, so we patch the -d option to
                 * -D for the second time we get invoked so we don't
                 * keep on creating new processes forever. */
                s[-1] = 'D';
                daemon = 2;
                break;

            case 'D':
#endif
                daemon = 1;
                break;

            case 'c':
                generate_ssl_certificate();
                exit(0);
                break;

            case 'S':
                print_server_properties = 1;
                break;

            case 'h':
                show_help();
                exit(0);

            case 'I':
            case 'l':
            case 'L':
            case 's':
#if ENABLE_Plugins
            case 'P':
#endif
                if (*s == '\0') {
                    if (++ind >= argc) {
                        fprintf(stderr, "%s: error: no argument given to option '%c'\n", progname, c);
                        exit(1);
                    }
                    s = argv[ind];
                }
                switch (c) {
                case 'I':
                    idle_timeout = strtol(s, 0, 0);
                    break;

                case 'l':
                    log_level = s;
                    parse_trace_mode(log_level, &log_mode);
                    break;

                case 'L':
                    log_name = s;
                    break;

                case 's':
                    url = s;
                    break;

#if ENABLE_Plugins
                case 'P':
                    plugins_path = s;
                    break;
#endif
                }
                s = "";
                break;

            default:
                fprintf(stderr, "%s: error: illegal option '%c'\n", progname, c);
                show_help();
                exit(1);
            }
        }
    }

    if (daemon) {
#if defined(_WIN32)
        become_daemon(daemon > 1 ? argv : NULL);
#else
        become_daemon();
#endif
    }
    open_log_file(log_name);

#endif

    bcg = broadcast_group_alloc();
    proto = protocol_alloc();

    /* The static services must be initialised before the plugins */
#if ENABLE_Cmdline
    if (interactive) ini_cmdline_handler(interactive, proto);
#else
    if (interactive) fprintf(stderr, "Warning: This version does not support interactive mode.\n");
#endif

    ini_services(proto, bcg);

#if !defined(_WRS_KERNEL)
    /* Reparse log level in case initialization cause additional
     * levels to be registered */
    if (log_level != NULL && parse_trace_mode(log_level, &log_mode) != 0) {
        fprintf(stderr, "Cannot parse log level: %s\n", log_level);
        exit(1);
    }
#endif

    if (ini_server(url, proto, bcg) < 0) {
        fprintf(stderr, "Cannot create TCF server: %s\n", errno_to_str(errno));
        exit(1);
    }
    discovery_start();

    if (print_server_properties) {
        ChannelServer * s;
        char * server_properties;
        assert(!list_is_empty(&channel_server_root));
        s = servlink2channelserverp(channel_server_root.next);
        server_properties = channel_peer_to_json(s->ps);
        printf("Server-Properties: %s\n", server_properties);
        fflush(stdout);
        trace(LOG_ALWAYS, "Server-Properties: %s", server_properties);
        loc_free(server_properties);
    }

    if (daemon)
        close_out_and_err();

#if ENABLE_SignalHandlers
    signal(SIGABRT, signal_handler);
    signal(SIGILL, signal_handler);
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
#if defined(_WIN32)
    SetConsoleCtrlHandler((PHANDLER_ROUTINE)CtrlHandler, TRUE);
    AddVectoredExceptionHandler(1, VectoredExceptionHandler);
#endif
#endif /* ENABLE_SignalHandlers */

    if (idle_timeout != 0) {
        add_channel_close_listener(channel_closed);
        check_idle_timeout(NULL);
    }

    /* Process events - must run on the initial thread since ptrace()
     * returns ECHILD otherwise, thinking we are not the owner. */
    run_event_loop();

#if ENABLE_Plugins
    plugins_destroy();
#endif /* ENABLE_Plugins */

    return 0;
}
