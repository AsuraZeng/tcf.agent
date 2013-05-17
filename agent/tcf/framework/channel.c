/*******************************************************************************
 * Copyright (c) 2007, 2013 Wind River Systems, Inc. and others.
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
 * Transport agnostic channel implementation.
 */

/* TODO: Somehow we should make it clear what needs to be done to add another transport layer.
 * Perhaps have a template or a readme file for it. */

#if defined(__GNUC__) && !defined(_GNU_SOURCE)
/* pread() need _GNU_SOURCE */
#  define _GNU_SOURCE
#endif

#include <tcf/config.h>
#include <stddef.h>
#include <errno.h>
#include <assert.h>
#include <ctype.h>
#include <string.h>
#include <tcf/framework/tcf.h>
#include <tcf/framework/channel.h>
#include <tcf/framework/channel_tcp.h>
#include <tcf/framework/channel_pipe.h>
#include <tcf/framework/protocol.h>
#include <tcf/framework/myalloc.h>
#include <tcf/framework/events.h>
#include <tcf/framework/exceptions.h>
#include <tcf/framework/trace.h>
#include <tcf/framework/link.h>
#include <tcf/framework/json.h>

#ifndef DEFAULT_SERVER_NAME
#  define DEFAULT_SERVER_NAME "TCF Agent"
#endif

static void trigger_channel_shutdown(ShutdownInfo * obj);

ShutdownInfo channel_shutdown = { trigger_channel_shutdown };
LINK channel_root = TCF_LIST_INIT(channel_root);
LINK channel_server_root = TCF_LIST_INIT(channel_server_root);

#define BCAST_MAGIC 0x1463e328

#define out2bcast(A)    ((TCFBroadcastGroup *)((char *)(A) - offsetof(TCFBroadcastGroup, out)))
#define bclink2channel(A) ((Channel *)((char *)(A) - offsetof(Channel, bclink)))
#define susplink2channel(A) ((Channel *)((char *)(A) - offsetof(Channel, susplink)))

static ChannelCreateListener create_listeners[16];
static int create_listeners_cnt = 0;

static ChannelOpenListener open_listeners[16];
static int open_listeners_cnt = 0;

static ChannelCloseListener close_listeners[16];
static int close_listeners_cnt = 0;

static const int BROADCAST_OK_STATES = (1 << ChannelStateConnected) | (1 << ChannelStateRedirectSent) | (1 << ChannelStateRedirectReceived);
#define isBoardcastOkay(c) ((1 << (c)->state) & BROADCAST_OK_STATES)

static void trigger_channel_shutdown(ShutdownInfo * obj) {
    LINK * l;

    l = channel_root.next;
    while (l != &channel_root) {
        Channel * c = chanlink2channelp(l);
        l = l->next;
        if (!is_channel_closed(c)) {
            channel_close(c);
        }
    }

    l = channel_server_root.next;
    while (l != &channel_server_root) {
        ChannelServer * s = servlink2channelserverp(l);
        l = l->next;
        s->close(s);
    }
}

static void write_all(OutputStream * out, int byte) {
    TCFBroadcastGroup * bcg = out2bcast(out);
    LINK * l = bcg->channels.next;

    assert(is_dispatch_thread());
    assert(bcg->magic == BCAST_MAGIC);
    while (l != &bcg->channels) {
        Channel * c = bclink2channel(l);
        if (isBoardcastOkay(c)) write_stream(&c->out, byte);
        l = l->next;
    }
}

static void write_block_all(OutputStream * out, const char * bytes, size_t size) {
    TCFBroadcastGroup * bcg = out2bcast(out);
    LINK * l = bcg->channels.next;

    assert(is_dispatch_thread());
    assert(bcg->magic == BCAST_MAGIC);
    while (l != &bcg->channels) {
        Channel * c = bclink2channel(l);
        if (isBoardcastOkay(c)) c->out.write_block(&c->out, bytes, size);
        l = l->next;
    }
}

static ssize_t splice_block_all(OutputStream * out, int fd, size_t size, int64_t * offset) {
    char buffer[0x400];
    ssize_t rd;

    assert(is_dispatch_thread());
    if (size > sizeof(buffer)) size = sizeof(buffer);
    if (offset != NULL) {
        rd = pread(fd, buffer, size, (off_t)*offset);
        if (rd > 0) *offset += rd;
    }
    else {
        rd = read(fd, buffer, size);
    }
    if (rd > 0) write_block_all(out, buffer, rd);
    return rd;
}

void add_channel_create_listener(ChannelCreateListener listener) {
    assert(create_listeners_cnt < (int)(sizeof(create_listeners) / sizeof(ChannelCreateListener)));
    create_listeners[create_listeners_cnt++] = listener;
}

void add_channel_open_listener(ChannelOpenListener listener) {
    assert(open_listeners_cnt < (int)(sizeof(open_listeners) / sizeof(ChannelOpenListener)));
    open_listeners[open_listeners_cnt++] = listener;
}

void add_channel_close_listener(ChannelCloseListener listener) {
    assert(close_listeners_cnt < (int)(sizeof(close_listeners) / sizeof(ChannelCloseListener)));
    close_listeners[close_listeners_cnt++] = listener;
}

void notify_channel_created(Channel * c) {
    int i;
    for (i = 0; i < create_listeners_cnt; i++) {
        create_listeners[i](c);
    }
}

void notify_channel_opened(Channel * c) {
    int i;
    for (i = 0; i < open_listeners_cnt; i++) {
        open_listeners[i](c);
    }
}

void notify_channel_closed(Channel * c) {
    int i;
    for (i = 0; i < close_listeners_cnt; i++) {
        close_listeners[i](c);
    }
}

TCFBroadcastGroup * broadcast_group_alloc(void) {
    TCFBroadcastGroup * p = (TCFBroadcastGroup*)loc_alloc_zero(sizeof(TCFBroadcastGroup));

    list_init(&p->channels);
    p->magic = BCAST_MAGIC;
    p->out.write = write_all;
    p->out.write_block = write_block_all;
    p->out.splice_block = splice_block_all;
    return p;
}

void broadcast_group_free(TCFBroadcastGroup * p) {
    LINK * l = p->channels.next;

    assert(is_dispatch_thread());
    while (l != &p->channels) {
        Channel * c = bclink2channel(l);
        assert(c->bcg == p);
        l = l->next;
        c->bcg = NULL;
        list_remove(&c->bclink);
    }
    assert(list_is_empty(&p->channels));
    p->magic = 0;
    loc_free(p);
}

void channel_set_broadcast_group(Channel * c, TCFBroadcastGroup * bcg) {
    if (c->bcg != NULL) channel_clear_broadcast_group(c);
    list_add_last(&c->bclink, &bcg->channels);
    c->bcg = bcg;
}

void channel_clear_broadcast_group(Channel * c) {
    if (c->bcg == NULL) return;
    list_remove(&c->bclink);
    c->bcg = NULL;
}

void channel_lock(Channel * c) {
    c->lock(c);
}

void channel_unlock(Channel * c) {
    c->unlock(c);
}

int is_channel_closed(Channel * c) {
    return c->is_closed(c);
}

PeerServer * channel_peer_from_url(const char * url) {
    int i;
    const char * s;
    const char * user = get_user_name();
    char transport[16];
    PeerServer * ps = peer_server_alloc();

    peer_server_addprop(ps, loc_strdup("Name"), loc_strdup(DEFAULT_SERVER_NAME));
    peer_server_addprop(ps, loc_strdup("OSName"), loc_strdup(get_os_name()));
    if (user != NULL) peer_server_addprop(ps, loc_strdup("UserName"), loc_strdup(user));
    peer_server_addprop(ps, loc_strdup("AgentID"), loc_strdup(get_agent_id()));

    s = url;
    i = 0;
    while (*s && isalpha((int)*s) && i < (int)sizeof transport) transport[i++] = (char)toupper((int)*s++);
    if (*s == ':' && i < (int)sizeof transport) {
        s++;
        peer_server_addprop(ps, loc_strdup("TransportName"), loc_strndup(transport, i));
        url = s;
    }
    else {
        s = url;
    }
    while (*s && *s != ':' && *s != ';') s++;
    if (s != url) peer_server_addprop(ps, loc_strdup("Host"), loc_strndup(url, s - url));
    if (*s == ':') {
        s++;
        url = s;
        while (*s && *s != ';') s++;
        if (s != url) peer_server_addprop(ps, loc_strdup("Port"), loc_strndup(url, s - url));
    }

    while (*s == ';') {
        char * name;
        char * value;
        s++;
        url = s;
        while (*s && *s != '=') s++;
        if (*s != '=' || s == url) {
            s = url - 1;
            break;
        }
        name = loc_strndup(url, s - url);
        s++;
        url = s;
        while (*s && *s != ';') s++;
        value = loc_strndup(url, s - url);
        peer_server_addprop(ps, name, value);
    }
    if (*s != '\0') {
        peer_server_free(ps);
        return NULL;
    }
    return ps;
}

char * channel_peer_to_json(PeerServer * ps) {
    unsigned i;
    char * rval;
    ByteArrayOutputStream buf;
    OutputStream * out;

    out = create_byte_array_output_stream(&buf);
    write_stream(out, '{');
    for (i = 0; i < ps->ind; i++) {
        if (i > 0) write_stream(out, ',');
        json_write_string(out, ps->list[i].name);
        write_stream(out, ':');
        json_write_string(out, ps->list[i].value);
    }
    write_stream(out, '}');
    write_stream(out, '\0');
    get_byte_array_output_stream_data(&buf, &rval, NULL);
    return rval;
}

/*
 * Start TCF channel server
 */
ChannelServer * channel_server(PeerServer * ps) {
    const char * transportname = peer_server_getprop(ps, "TransportName", NULL);

    if (transportname == NULL) {
        transportname = "TCP";
        peer_server_addprop(ps, "TransportName", transportname);
    }

    if (strcmp(transportname, "TCP") == 0 || strcmp(transportname, "SSL") == 0) {
        return channel_tcp_server(ps);
    }
    else if (strcmp(transportname, "PIPE") == 0) {
        return channel_pipe_server(ps);
    }
    else if (strcmp(transportname, "UNIX") == 0) {
        return channel_unix_server(ps);
    }
    else {
        errno = ERR_INV_TRANSPORT;
        return NULL;
    }
}

/*
 * Connect to TCF channel server
 */
void channel_connect(PeerServer * ps, ChannelConnectCallBack callback, void * callback_args) {
    const char * transportname = peer_server_getprop(ps, "TransportName", NULL);

    if (transportname == NULL || strcmp(transportname, "TCP") == 0 || strcmp(transportname, "SSL") == 0) {
        channel_tcp_connect(ps, callback, callback_args);
    }
    else if (strcmp(transportname, "PIPE") == 0) {
        channel_pipe_connect(ps, callback, callback_args);
    }
    else if (strcmp(transportname, "UNIX") == 0) {
        channel_unix_connect(ps, callback, callback_args);
    }
    else {
        callback(callback_args, ERR_INV_TRANSPORT, NULL);
    }
}

/*
 * Start communication of a newly created channel
 */
void channel_start(Channel * c) {
    trace(LOG_PROTOCOL, "Starting channel %#lx %s", c, c->peer_name);
    assert(c->protocol != NULL);
    assert(c->state == ChannelStateStartWait);
    c->state = ChannelStateStarted;
    c->start_comm(c);
}

/*
 * Close communication channel
 */
void channel_close(Channel * c) {
    trace(LOG_PROTOCOL, "Closing channel %#lx %s", c, c->peer_name);
    c->close(c, 0);
}
