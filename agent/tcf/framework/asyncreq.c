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

#if defined(__GNUC__) && !defined(_GNU_SOURCE)
/* pread() and pwrite() need _GNU_SOURCE */
#  define _GNU_SOURCE
#endif

#include <tcf/config.h>
#include <assert.h>
#include <stddef.h>
#include <fcntl.h>
#include <sys/stat.h>

#if defined(_WIN32)
#elif defined(_WRS_KERNEL)
#else
#  include <sys/wait.h>
#endif
#include <tcf/framework/mdep-threads.h>
#include <tcf/framework/mdep-inet.h>
#include <tcf/framework/mdep-fs.h>
#include <tcf/framework/myalloc.h>
#include <tcf/framework/trace.h>
#include <tcf/framework/events.h>
#include <tcf/framework/errors.h>
#include <tcf/framework/link.h>
#include <tcf/framework/asyncreq.h>
#include <tcf/framework/shutdown.h>

#define MAX_WORKER_THREADS 32

static LINK wtlist = TCF_LIST_INIT(wtlist);
static int wtlist_size = 0;
static int wtrunning_count = 0;
static pthread_mutex_t wtlock;

typedef struct WorkerThread {
    LINK wtlink;
    AsyncReqInfo * req;
    pthread_cond_t cond;
    pthread_t thread;
} WorkerThread;

#define wtlink2wt(A)  ((WorkerThread *)((char *)(A) - offsetof(WorkerThread, wtlink)))

static AsyncReqInfo shutdown_req;

static void trigger_async_shutdown(ShutdownInfo * obj) {
    check_error(pthread_mutex_lock(&wtlock));
    while (!list_is_empty(&wtlist)) {
        WorkerThread * wt = wtlink2wt(wtlist.next);
        list_remove(&wt->wtlink);
        wtlist_size--;
        assert(wt->req == NULL);
        wt->req = &shutdown_req;
        check_error(pthread_cond_signal(&wt->cond));
    }
    check_error(pthread_mutex_unlock(&wtlock));
}

static ShutdownInfo async_shutdown = { trigger_async_shutdown };


static void worker_thread_exit(void * x) {
    WorkerThread * wt = (WorkerThread *)x;

    check_error(pthread_cond_destroy(&wt->cond));
    pthread_join(wt->thread, NULL);
    check_error(pthread_mutex_lock(&wtlock));
    if (--wtrunning_count == 0)
        shutdown_set_stopped(&async_shutdown);
    trace(LOG_ASYNCREQ, "worker_thread_exit %p running threads %d", wt, wtrunning_count);
    check_error(pthread_mutex_unlock(&wtlock));
    loc_free(wt);
}

static void * worker_thread_handler(void * x) {
    WorkerThread * wt = (WorkerThread *)x;

    for (;;) {
        AsyncReqInfo * req = wt->req;

        assert(req != NULL);
        req->error = 0;
        switch(req->type) {
        case AsyncReqRead:              /* File read */
            req->u.fio.rval = read(req->u.fio.fd, req->u.fio.bufp, req->u.fio.bufsz);
            if (req->u.fio.rval == -1) {
                req->error = errno;
                assert(req->error);
            }
            break;

        case AsyncReqWrite:             /* File write */
            req->u.fio.rval = write(req->u.fio.fd, req->u.fio.bufp, req->u.fio.bufsz);
            if (req->u.fio.rval == -1) {
                req->error = errno;
                assert(req->error);
            }
            break;

        case AsyncReqSeekRead:          /* File read at offset */
            req->u.fio.rval = pread(req->u.fio.fd, req->u.fio.bufp, req->u.fio.bufsz, (off_t)req->u.fio.offset);
            if (req->u.fio.rval == -1) {
                req->error = errno;
                assert(req->error);
            }
            break;

        case AsyncReqSeekWrite:         /* File write at offset */
            req->u.fio.rval = pwrite(req->u.fio.fd, req->u.fio.bufp, req->u.fio.bufsz, (off_t)req->u.fio.offset);
            if (req->u.fio.rval == -1) {
                req->error = errno;
                assert(req->error);
            }
            break;

        case AsyncReqRecv:              /* Socket recv */
            req->u.sio.rval = recv(req->u.sio.sock, req->u.sio.bufp, req->u.sio.bufsz, req->u.sio.flags);
            if (req->u.sio.rval == -1) {
                req->error = errno;
                assert(req->error);
            }
            break;

        case AsyncReqSend:              /* Socket send */
            req->u.sio.rval = send(req->u.sio.sock, req->u.sio.bufp, req->u.sio.bufsz, req->u.sio.flags);
            if (req->u.sio.rval == -1) {
                req->error = errno;
                assert(req->error);
            }
            break;

        case AsyncReqRecvFrom:          /* Socket recvfrom */
            req->u.sio.rval = recvfrom(req->u.sio.sock, req->u.sio.bufp, req->u.sio.bufsz, req->u.sio.flags, req->u.sio.addr, &req->u.sio.addrlen);
            if (req->u.sio.rval == -1) {
                req->error = errno;
                trace(LOG_ASYNCREQ, "AsyncReqRecvFrom: req %p, type %d, error %d", req, req->type, req->error);
                assert(req->error);
            }
            break;

        case AsyncReqSendTo:            /* Socket sendto */
            req->u.sio.rval = sendto(req->u.sio.sock, req->u.sio.bufp, req->u.sio.bufsz, req->u.sio.flags, req->u.sio.addr, req->u.sio.addrlen);
            if (req->u.sio.rval == -1) {
                req->error = errno;
                assert(req->error);
            }
            break;

        case AsyncReqAccept:            /* Accept socket connections */
            req->u.acc.rval = accept(req->u.acc.sock, req->u.acc.addr, req->u.acc.addr ? &req->u.acc.addrlen : NULL);
            if (req->u.acc.rval == -1) {
                req->error = errno;
                trace(LOG_ASYNCREQ, "AsyncReqAccept: req %p, type %d, error %d", req, req->type, req->error);
                assert(req->error);
            }
            break;

        case AsyncReqConnect:           /* Connect to socket */
            req->u.con.rval = connect(req->u.con.sock, req->u.con.addr, req->u.con.addrlen);
            if (req->u.con.rval == -1) {
                req->error = errno;
                trace(LOG_ASYNCREQ, "AsyncReqConnect: req %p, type %d, error %d", req, req->type, req->error);
                assert(req->error);
            }
            break;

/* Platform dependant IO methods */
#if defined(_WIN32)
        case AsyncReqConnectPipe:
            req->u.cnp.rval = ConnectNamedPipe(req->u.cnp.pipe, NULL);
            if (!req->u.cnp.rval) {
                req->error = set_win32_errno(GetLastError());
                assert(req->error);
            }
            break;
#elif defined(_WRS_KERNEL)
#else
        case AsyncReqWaitpid:           /* Wait for process change */
            req->u.wpid.rval = waitpid(req->u.wpid.pid, &req->u.wpid.status, req->u.wpid.options);
            if (req->u.wpid.rval == -1) {
                req->error = errno;
                assert(req->error);
            }
            break;
#endif
        case AsyncReqSelect:
        {
            struct timeval tv;
            tv.tv_sec = (long)req->u.select.timeout.tv_sec;
            tv.tv_usec = req->u.select.timeout.tv_nsec / 1000;
            req->u.select.rval = select(req->u.select.nfds, &req->u.select.readfds,
                        &req->u.select.writefds, &req->u.select.errorfds, &tv);
            if (req->u.select.rval == -1) {
                req->error = errno;
                assert(req->error);
            }
            break;
        }
        case AsyncReqClose:
            req->u.fio.rval = close(req->u.fio.fd);
            if (req->u.fio.rval == -1) {
                req->error = errno;
                assert(req->error);
            }
        break;
        case AsyncReqOpen:
            req->u.fio.rval = open(req->u.fio.file_name, req->u.fio.flags, req->u.fio.permission);
            if (req->u.fio.rval == -1) {
                req->error = errno;
                assert(req->error);
            }
            loc_free(req->u.fio.file_name);
            break;
        case AsyncReqFstat:
            memset(&req->u.fio.statbuf, 0, sizeof(req->u.fio.statbuf));
            req->u.fio.rval = fstat(req->u.fio.fd, &req->u.fio.statbuf);
            if (req->u.fio.rval == -1) {
                req->error = errno;
                assert(req->error);
            }
            break;
        case AsyncReqStat:
            memset(&req->u.fio.statbuf, 0, sizeof(req->u.fio.statbuf));
            req->u.fio.rval = stat(req->u.fio.file_name, &req->u.fio.statbuf);
            if (req->u.fio.rval == -1) {
                req->error = errno;
                assert(req->error);
            }
            loc_free(req->u.fio.file_name);
            break;
        case AsyncReqLstat:
            memset(&req->u.fio.statbuf, 0, sizeof(req->u.fio.statbuf));
            req->u.fio.rval = lstat(req->u.fio.file_name, &req->u.fio.statbuf);
            if (req->u.fio.rval == -1) {
                req->error = errno;
                assert(req->error);
            }
            loc_free(req->u.fio.file_name);
            break;
        case AsyncReqRemove:
            req->u.fio.rval = remove(req->u.fio.file_name);
            if (req->u.fio.rval == -1) {
                req->error = errno;
                assert(req->error);
            }
            loc_free(req->u.fio.file_name);
            break;
        default:
            req->error = ENOSYS;
            break;
        }
        trace(LOG_ASYNCREQ, "async_req_complete: req %p, type %d, error %d", req, req->type, req->error);
        check_error(pthread_mutex_lock(&wtlock));
        /* Post event inside lock to make sure a new worker thread is
         * not created unnecessarily */
        post_event(req->done, req);
        wt->req = NULL;
        if (wtlist_size >= MAX_WORKER_THREADS ||
            async_shutdown.state == SHUTDOWN_STATE_PENDING) {
            check_error(pthread_mutex_unlock(&wtlock));
            break;
        }
        list_add_last(&wt->wtlink, &wtlist);
        wtlist_size++;
        for (;;) {
            check_error(pthread_cond_wait(&wt->cond, &wtlock));
            if (wt->req != NULL) break;
        }
        check_error(pthread_mutex_unlock(&wtlock));
        if (wt->req == &shutdown_req) break;
    }
    post_event(worker_thread_exit, wt);
    return NULL;
}

static void worker_thread_add(AsyncReqInfo * req) {
    WorkerThread * wt;

    assert(is_dispatch_thread());
    wt = (WorkerThread *)loc_alloc_zero(sizeof *wt);
    wt->req = req;
    check_error(pthread_cond_init(&wt->cond, NULL));
    check_error(pthread_create(&wt->thread, &pthread_create_attr, worker_thread_handler, wt));
    if (wtrunning_count++ == 0)
        shutdown_set_normal(&async_shutdown);
    trace(LOG_ASYNCREQ, "worker_thread_add %p running threads %d", wt, wtrunning_count);
}

static void worker_thread_add_deferred(void * x) {
    AsyncReqInfo * req = (AsyncReqInfo *)x;

    check_error(pthread_mutex_lock(&wtlock));
    worker_thread_add(req);
    check_error(pthread_mutex_unlock(&wtlock));
}

#if ENABLE_AIO
static void aio_done(union sigval arg) {
    AsyncReqInfo * req = (AsyncReqInfo *)arg.sival_ptr;
    req->u.fio.rval = aio_return(&req->u.fio.aio);
    if (req->u.fio.rval < 0) req->error = aio_error(&req->u.fio.aio);
    post_event(req->done, req);
}
#endif

void async_req_post(AsyncReqInfo * req) {
    WorkerThread * wt;

    trace(LOG_ASYNCREQ, "async_req_post: req %p, type %d", req, req->type);
    assert(req->done != NULL);

#if ENABLE_AIO
    {
        int res = 0;
        switch (req->type) {
        case AsyncReqSeekRead:
        case AsyncReqSeekWrite:
            memset(&req->u.fio.aio, 0, sizeof(req->u.fio.aio));
            req->u.fio.aio.aio_fildes = req->u.fio.fd;
            req->u.fio.aio.aio_offset = (off_t)req->u.fio.offset;
            req->u.fio.aio.aio_buf = req->u.fio.bufp;
            req->u.fio.aio.aio_nbytes = req->u.fio.bufsz;
            req->u.fio.aio.aio_sigevent.sigev_notify = SIGEV_THREAD;
            req->u.fio.aio.aio_sigevent.sigev_notify_function = aio_done;
            req->u.fio.aio.aio_sigevent.sigev_value.sival_ptr = req;
            res = req->type == AsyncReqSeekWrite ?
                aio_write(&req->u.fio.aio) :
                aio_read(&req->u.fio.aio);
            if (res < 0) {
                req->u.fio.rval = -1;
                req->error = errno;
                post_event(req->done, req);
            }
            return;
        }
    }
#endif
    check_error(pthread_mutex_lock(&wtlock));
    if (list_is_empty(&wtlist)) {
        assert(wtlist_size == 0);
        if (is_dispatch_thread()) {
            worker_thread_add(req);
        }
        else {
            post_event(worker_thread_add_deferred, req);
        }
    }
    else {
        wt = wtlink2wt(wtlist.next);
        list_remove(&wt->wtlink);
        wtlist_size--;
        assert(wt->req == NULL);
        wt->req = req;
        check_error(pthread_cond_signal(&wt->cond));
    }
    check_error(pthread_mutex_unlock(&wtlock));
}

void ini_asyncreq(void) {
    wtlist_size = 0;
    check_error(pthread_mutex_init(&wtlock, NULL));
}
