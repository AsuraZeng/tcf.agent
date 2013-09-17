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
 *     Nokia - Symbian support
 *******************************************************************************/

/*
 * Machine and OS dependent definitions.
 * This module implements host OS abstraction layer that helps make
 * agent code portable between Linux, Windows, VxWorks and potentially other OSes.
 */

#include <tcf/config.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <tcf/framework/mdep-threads.h>
#include <tcf/framework/mdep-inet.h>
#include <tcf/framework/mdep-fs.h>
#include <tcf/framework/errors.h>
#include <tcf/framework/myalloc.h>

pthread_attr_t pthread_create_attr;
int utf8_locale = 0;

#if defined(_WIN32)

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR,12)
#endif
#ifndef SIO_UDP_NETRESET
#define SIO_UDP_NETRESET _WSAIOW(IOC_VENDOR,15)
#endif
#undef socket
int wsa_socket(int af, int type, int protocol) {
    int res = 0;

    SetLastError(0);
    WSASetLastError(0);
    res = socket(af, type, protocol);
    if (res < 0) {
        set_win32_errno(WSAGetLastError());
        return -1;
    }
    if (type == SOCK_DGRAM && protocol == IPPROTO_UDP) {
        DWORD dw = 0;
        BOOL b = FALSE;
        WSAIoctl(res, SIO_UDP_CONNRESET, &b, sizeof(b), NULL, 0, &dw, NULL, NULL);
        WSAIoctl(res, SIO_UDP_NETRESET, &b, sizeof(b), NULL, 0, &dw, NULL, NULL);
    }
    return res;
}

#undef connect
int wsa_connect(int socket, const struct sockaddr * addr, int addr_size) {
    int res = 0;
    SetLastError(0);
    WSASetLastError(0);
    res = connect(socket, addr, addr_size);
    if (res != 0) {
        set_win32_errno(WSAGetLastError());
        return -1;
    }
    return 0;
}

#undef bind
int wsa_bind(int socket, const struct sockaddr * addr, int addr_size) {
    int res = 0;
    SetLastError(0);
    WSASetLastError(0);
    res = bind(socket, addr, addr_size);
    if (res != 0) {
        set_win32_errno(WSAGetLastError());
        return -1;
    }
    return 0;
}

#undef listen
int wsa_listen(int socket, int size) {
    int res = 0;
    SetLastError(0);
    WSASetLastError(0);
    res = listen(socket, size);
    if (res != 0) {
        set_win32_errno(WSAGetLastError());
        return -1;
    }
    return 0;
}

#undef recv
int wsa_recv(int socket, void * buf, size_t size, int flags) {
    int res = 0;
    SetLastError(0);
    WSASetLastError(0);
    res = recv(socket, (char *)buf, size, flags);
    if (res < 0) {
        set_win32_errno(WSAGetLastError());
        return -1;
    }
    return res;
}

#undef recvfrom
int wsa_recvfrom(int socket, void * buf, size_t size, int flags,
                 struct sockaddr * addr, socklen_t * addr_size) {
    int res = 0;
    SetLastError(0);
    WSASetLastError(0);
    res = recvfrom(socket, (char *)buf, size, flags, addr, addr_size);
    if (res < 0) {
        set_win32_errno(WSAGetLastError());
        return -1;
    }
    return res;
}

#undef send
int wsa_send(int socket, const void * buf, size_t size, int flags) {
    int res = 0;
    SetLastError(0);
    WSASetLastError(0);
    res = send(socket, (char *)buf, size, flags);
    if (res < 0) {
        set_win32_errno(WSAGetLastError());
        return -1;
    }
    return res;
}

#undef sendto
int wsa_sendto(int socket, const void * buf, size_t size, int flags,
               const struct sockaddr * dest_addr, socklen_t dest_size) {
    int res = 0;
    SetLastError(0);
    WSASetLastError(0);
    res = sendto(socket, (char *)buf, size, flags, dest_addr, dest_size);
    if (res < 0) {
        set_win32_errno(WSAGetLastError());
        return -1;
    }
    return res;
}

#undef setsockopt
int wsa_setsockopt(int socket, int level, int opt, const char * value, int size) {
    int res = 0;
    SetLastError(0);
    WSASetLastError(0);
    res = setsockopt(socket, level, opt, value, size);
    if (res != 0) {
        set_win32_errno(WSAGetLastError());
        return -1;
    }
    return 0;
}

#undef getsockname
int wsa_getsockname(int socket, struct sockaddr * name, int * size) {
    int res = 0;
    SetLastError(0);
    WSASetLastError(0);
    res = getsockname(socket, name, size);
    if (res != 0) {
        set_win32_errno(WSAGetLastError());
        return -1;
    }
    return 0;
}

#undef select
int wsa_select(int nfds, fd_set * readfds, fd_set * writefds, fd_set * exceptfds, const struct timeval * timeout) {
    int res = 0;
    SetLastError(0);
    WSASetLastError(0);
    res = select(nfds, readfds, writefds, exceptfds, timeout);
    if (res != 0) {
        set_win32_errno(WSAGetLastError());
        return -1;
    }
    return 0;
}

/* inet_ntop()/inet_pton() are not available before Windows Vista */
const char * inet_ntop(int af, const void * src, char * dst, socklen_t size) {
    char * str = NULL;
    if (af != AF_INET) {
#ifdef EAFNOSUPPORT
        errno = EAFNOSUPPORT;
#else
        errno = EINVAL;
#endif
        return NULL;
    }
    str = inet_ntoa(*(struct in_addr *)src);
    if ((socklen_t)strlen(str) >= size) {
        errno = ENOSPC;
        return NULL;
    }
    return strcpy(dst, str);
}

int inet_pton(int af, const char * src, void * dst) {
    if (af != AF_INET) {
#ifdef EAFNOSUPPORT
        errno = EAFNOSUPPORT;
#else
        errno = EINVAL;
#endif
        return -1;
    }
    if (src == NULL || *src == 0) return 0;
    if ((((struct in_addr *)dst)->s_addr = inet_addr(src)) == INADDR_NONE) return 0;
    return 1;
}

#endif /* _WIN32 */

#if defined(_WIN32) && !defined(__CYGWIN__)

static __int64 file_time_to_unix_time(const FILETIME * ft) {
    __int64 res = (__int64)ft->dwHighDateTime << 32;

    res |= ft->dwLowDateTime;
    res /= 10;                   /* from 100 nano-sec periods to usec */
    res -= 11644473600000000ull; /* from Win epoch to Unix epoch */
    return res;
}

int clock_gettime(clockid_t clock_id, struct timespec * tp) {
    FILETIME ft;
    __int64 tim;

    assert(clock_id == CLOCK_REALTIME);
    if (!tp) {
        errno = EINVAL;
        return -1;
    }
    GetSystemTimeAsFileTime(&ft);
    tim = file_time_to_unix_time(&ft);
    tp->tv_sec  = (long)(tim / 1000000L);
    tp->tv_nsec = (long)(tim % 1000000L) * 1000;
    return 0;
}

void usleep(useconds_t useconds) {
    Sleep(useconds / 1000);
}

int truncate(const char * path, int64_t size) {
    int res = 0;
    int f = open(path, _O_RDWR | _O_BINARY, 0);
    if (f < 0) return -1;
    res = ftruncate(f, size);
    _close(f);
    return res;
}

int ftruncate(int fd, int64_t size) {
    int64_t cur, pos;
    BOOL ret = FALSE;
    HANDLE handle = (HANDLE)_get_osfhandle(fd);

    if (handle == INVALID_HANDLE_VALUE) {
        errno = EBADF;
        return -1;
    }
    /* save the current file pointer */
    cur = _lseeki64(fd, 0, SEEK_CUR);
    if (cur >= 0) {
        pos = _lseeki64(fd, size, SEEK_SET);
        if (pos >= 0) {
            ret = SetEndOfFile(handle);
            if (!ret) errno = EBADF;
        }
        /* restore the file pointer */
        _lseeki64(fd, cur, SEEK_SET);
    }
    return ret ? 0 : -1;
}

int getuid(void) {
    /* Windows user is always a superuser :) */
    return 0;
}

int geteuid(void) {
    return 0;
}

int getgid(void) {
    return 0;
}

int getegid(void) {
    return 0;
}

static wchar_t * str_to_wide_char(const char * str) {
    wchar_t * res = NULL;
    int len = MultiByteToWideChar(CP_UTF8, 0, str, -1, NULL, 0);
    if (len == 0) {
        set_win32_errno(GetLastError());
        return NULL;
    }
    len += 16; /* utf8_opendir() needs extra space at the end of the string */
    res = (wchar_t *)malloc(sizeof(wchar_t) * len);
    if (res == NULL) {
        errno = ENOMEM;
        return NULL;
    }
    len = MultiByteToWideChar(CP_UTF8, 0, str, -1, res, len);
    if (len == 0) {
        set_win32_errno(GetLastError());
        return NULL;
    }
    return res;
}

int utf8_stat(const char * name, struct utf8_stat * buf) {
    int error = 0;
    struct _stati64 tmp;
    wchar_t * path = str_to_wide_char(name);
    if (path == NULL) return -1;
    memset(&tmp, 0, sizeof(tmp));
    if (_wstati64(path, &tmp)) error = errno;
    if (!error) {
        buf->st_dev = tmp.st_dev;
        buf->st_ino = tmp.st_ino;
        buf->st_mode = tmp.st_mode;
        buf->st_nlink = tmp.st_nlink;
        buf->st_uid = tmp.st_uid;
        buf->st_gid = tmp.st_gid;
        buf->st_rdev = tmp.st_rdev;
        buf->st_size = tmp.st_size;
        buf->st_atime = tmp.st_atime;
        buf->st_mtime = tmp.st_mtime;
        buf->st_ctime = tmp.st_ctime;
    }
    free(path);
    if (error) {
        errno = error;
        return -1;
    }
    return 0;
}

int utf8_fstat(int fd, struct utf8_stat * buf) {
    struct _stati64 tmp;
    memset(&tmp, 0, sizeof(tmp));
    if (_fstati64(fd, &tmp)) return -1;
    buf->st_dev = tmp.st_dev;
    buf->st_ino = tmp.st_ino;
    buf->st_mode = tmp.st_mode;
    buf->st_nlink = tmp.st_nlink;
    buf->st_uid = tmp.st_uid;
    buf->st_gid = tmp.st_gid;
    buf->st_rdev = tmp.st_rdev;
    buf->st_size = tmp.st_size;
    buf->st_atime = tmp.st_atime;
    buf->st_mtime = tmp.st_mtime;
    buf->st_ctime = tmp.st_ctime;
    return 0;
}

int utf8_open(const char * name, int flags, int perms) {
    int fd = -1;
    int error = 0;
    wchar_t * path = str_to_wide_char(name);
    if (path == NULL) return -1;
    if ((fd = _wopen(path, flags, perms)) < 0) error = errno;
    free(path);
    if (error) {
        errno = error;
        return -1;
    }
    return fd;
}

int utf8_chmod(const char * name, int mode) {
    int error = 0;
    wchar_t * path = str_to_wide_char(name);
    if (path == NULL) return -1;
    if (_wchmod(path, mode) < 0) error = errno;
    free(path);
    if (error) {
        errno = error;
        return -1;
    }
    return 0;
}

int utf8_remove(const char * name) {
    int error = 0;
    wchar_t * path = str_to_wide_char(name);
    if (path == NULL) return -1;
    if (_wremove(path) < 0) error = errno;
    free(path);
    if (error) {
        errno = error;
        return -1;
    }
    return 0;
}

int utf8_rmdir(const char * name) {
    int error = 0;
    wchar_t * path = str_to_wide_char(name);
    if (path == NULL) return -1;
    if (_wrmdir(path) < 0) error = errno;
    free(path);
    if (error) {
        errno = error;
        return -1;
    }
    return 0;
}

int utf8_mkdir(const char * name, int mode) {
    int error = 0;
    wchar_t * path = str_to_wide_char(name);
    if (path == NULL) return -1;
    if (_wmkdir(path) < 0) error = errno;
    free(path);
    if (error) {
        errno = error;
        return -1;
    }
    return 0;
}

int utf8_rename(const char * name1, const char * name2) {
    int error = 0;
    wchar_t * path1 = NULL;
    wchar_t * path2 = NULL;
    path1 = str_to_wide_char(name1);
    if (path1 == NULL) {
        error = errno;
    }
    else {
        path2 = str_to_wide_char(name2);
        if (path2 == NULL) error = errno;
    }
    if (!error && _wrename(path1, path2) < 0) error = errno;
    free(path1);
    free(path2);
    if (error) {
        errno = error;
        return -1;
    }
    return 0;
}

DIR * utf8_opendir(const char * path) {
    DIR * d = (DIR *)malloc(sizeof(DIR));
    if (!d) { errno = ENOMEM; return 0; }
    d->path = str_to_wide_char(path);
    if (!d->path) { free(d); return NULL; }
    wcscat(d->path, L"/*.*");
    d->hdl = -1;
    return d;
}

struct dirent * utf8_readdir(DIR * d) {
    static struct dirent de;

    if (d->hdl < 0) {
        int error = 0;
        d->hdl = _wfindfirsti64(d->path, &d->blk);
        if (d->hdl < 0) error = errno;
        if (error) {
            if (errno == ENOENT) errno = 0;
            return NULL;
        }
    }
    else {
        int r = _wfindnexti64(d->hdl, &d->blk);
        if (r < 0) {
            if (errno == ENOENT) errno = 0;
            return NULL;
        }
    }
    if (!WideCharToMultiByte(CP_UTF8, 0, d->blk.name, -1, de.d_name, sizeof(de.d_name), NULL, NULL)) {
        set_win32_errno(GetLastError());
        return 0;
    }
    de.d_size = d->blk.size;
    de.d_atime = d->blk.time_access;
    de.d_ctime = d->blk.time_create;
    de.d_wtime = d->blk.time_write;
    return &de;
}

int utf8_closedir(DIR * d) {
    int r = 0;
    if (!d) {
        errno = EBADF;
        return -1;
    }
    if (d->hdl >= 0) r = _findclose(d->hdl);
    free(d->path);
    free(d);
    return r;
}

#endif /* defined(_WIN32) && !defined(__CYGWIN__) */

#if defined(_WIN32) && !defined(__CYGWIN__) || defined(_WRS_KERNEL) || defined(__SYMBIAN32__)

ssize_t pread(int fd, void * buf, size_t size, off_t offset) {
    off_t offs0;
    ssize_t rd;
    if ((offs0 = lseek(fd, 0, SEEK_CUR)) == (off_t)-1) return -1;
    if (lseek(fd, offset, SEEK_SET) == (off_t)-1) return -1;
    rd = read(fd, (void *)buf, size);
    if (lseek(fd, offs0, SEEK_SET) == (off_t)-1) return -1;
    return rd;
}

ssize_t pwrite(int fd, const void * buf, size_t size, off_t offset) {
    off_t offs0;
    ssize_t wr;
    if ((offs0 = lseek(fd, 0, SEEK_CUR)) == (off_t)-1) return -1;
    if (lseek(fd, offset, SEEK_SET) == (off_t)-1) return -1;
    wr = write(fd, (void *)buf, size);
    if (lseek(fd, offs0, SEEK_SET) == (off_t)-1) return -1;
    return wr;
}

#endif /* defined(_WIN32) && !defined(__CYGWIN__) || defined(_WRS_KERNEL) || defined(__SYMBIAN32__) */

#ifndef big_endian_host
extern int big_endian_host(void) {
    uint16_t n = 0x0201;
    uint8_t * p = (uint8_t *)&n;
    return *p == 0x02;
}
#endif

void swap_bytes(void * buf, size_t size) {
    size_t i, j, n;
    char * p = (char *)buf;
    n = size >> 1;
    for (i = 0, j = size - 1; i < n; i++, j--) {
        char x = p[i];
        p[i] = p[j];
        p[j] = x;
    }
}

#if defined(_WIN32)

#include <shlobj.h>

const char * get_os_name(void) {
    static char str[256];
    OSVERSIONINFOEX info;
    memset(&info, 0, sizeof(info));
    info.dwOSVersionInfoSize = sizeof(info);
    GetVersionEx((OSVERSIONINFO *)&info);
    switch (info.dwMajorVersion) {
    case 4:
        return "Windows NT";
    case 5:
        switch (info.dwMinorVersion) {
        case 0: return "Windows 2000";
        case 1: return "Windows XP";
        case 2: return "Windows Server 2003";
        }
        break;
    case 6:
        switch (info.dwMinorVersion) {
        case 0:
            if (info.wProductType == VER_NT_WORKSTATION) return "Windows Vista";
            return "Windows Server 2008";
        case 1:
            if (info.wProductType == VER_NT_WORKSTATION) return "Windows 7";
            return "Windows Server 2008 R2";
        case 2:
            if (info.wProductType == VER_NT_WORKSTATION) return "Windows 8";
            return "Windows Server 2012";
        }
    }
    snprintf(str, sizeof(str), "Windows %d.%d", (int)info.dwMajorVersion, (int)info.dwMinorVersion);
    return str;
}

const char * get_user_home(void) {
    WCHAR w_buf[MAX_PATH];
    static char a_buf[MAX_PATH];
    if (a_buf[0] != 0) return a_buf;
    if (!SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_PROFILE, NULL, SHGFP_TYPE_CURRENT, w_buf))) {
        errno = ERR_OTHER;
        return NULL;
    }
    if (!WideCharToMultiByte(CP_UTF8, 0, w_buf, -1, a_buf, sizeof(a_buf), NULL, NULL)) {
        set_win32_errno(GetLastError());
        return NULL;
    }
    return a_buf;
}

#define MAX_USER_NAME 256

const char * get_user_name(void) {
    DWORD size = MAX_USER_NAME;
    WCHAR w_buf[MAX_USER_NAME];
    static char a_buf[MAX_USER_NAME];
    if (a_buf[0] != 0) return a_buf;
    if (!GetUserNameW(w_buf, &size)) {
        set_win32_errno(GetLastError());
        return NULL;
    }
    if (!WideCharToMultiByte(CP_UTF8, 0, w_buf, -1, a_buf, sizeof(a_buf), NULL, NULL)) {
        set_win32_errno(GetLastError());
        return NULL;
    }
    return a_buf;
}

void ini_mdep(void) {
    WORD wVersionRequested = MAKEWORD(1, 1);
    WSADATA wsaData;
    int err;

    SetErrorMode(SEM_FAILCRITICALERRORS);
    err = WSAStartup(wVersionRequested, &wsaData);
    if (err != 0) {
        fprintf(stderr, "Couldn't access winsock.dll.\n");
        exit(1);
    }
    /* Confirm that the Windows Sockets DLL supports 1.1.*/
    /* Note that if the DLL supports versions greater */
    /* than 1.1 in addition to 1.1, it will still return */
    /* 1.1 in wVersion since that is the version we */
    /* requested.     */
    if (LOBYTE(wsaData.wVersion) != 1 || HIBYTE(wsaData.wVersion) != 1) {
        fprintf(stderr, "Unacceptable version of winsock.dll.\n");
        WSACleanup();
        exit(1);
    }
    pthread_attr_init(&pthread_create_attr);
#if defined(_DEBUG) && defined(_MSC_VER)
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF /* | _CRTDBG_LEAK_CHECK_DF */);
#endif
}

#elif defined(_WRS_KERNEL)

void usleep(useconds_t useconds) {
    struct timespec tv;
    tv.tv_sec = useconds / 1000000;
    tv.tv_nsec = (useconds % 1000000) * 1000;
    nanosleep(&tv, NULL);
}

int truncate(char * path, int64_t size) {
    int f = open(path, O_RDWR, 0);
    if (f < 0) return -1;
    if (ftruncate(f, size) < 0) {
        int err = errno;
        close(f);
        errno = err;
        return -1;
    }
    return close(f);
}

int getuid(void) {
    return 0;
}

int geteuid(void) {
    return 0;
}

int getgid(void) {
    return 0;
}

int getegid(void) {
    return 0;
}

const char * get_os_name(void) {
#if _WRS_VXWORKS_MAJOR > 6 || _WRS_VXWORKS_MAJOR == 6 && _WRS_VXWORKS_MINOR >= 7
    return VXWORKS_VERSION;
#else
    static char str[256];
    snprintf(str, sizeof(str), "VxWorks %s", kernelVersion());
    return str;
#endif
}

const char * get_user_home(void) {
    return "/";
}

const char * get_user_name(void) {
    errno = ERR_UNSUPPORTED;
    return NULL;
}

void ini_mdep(void) {
    pthread_attr_init(&pthread_create_attr);
    pthread_attr_setstacksize(&pthread_create_attr, 0x8000);
    pthread_attr_setname(&pthread_create_attr, "tTcf");
    pthread_attr_setopt (&pthread_create_attr, VX_FP_TASK|VX_UNBREAKABLE);
}

#elif defined(__SYMBIAN32__)

int truncate(const char * path, int64_t size) {
    int res = 0;
    int f = open(path, O_RDWR | O_BINARY, 0);
    if (f < 0) return -1;
    res = ftruncate(f, size);
    close(f);
    return res;
}

const char * get_os_name(void) {
   static char str[] = "SYMBIAN";
   return str;
}

const char * get_user_home(void) {
    static char buf[] = "C:";
    return buf;
}

const char * get_user_name(void) {
    errno = ERR_UNSUPPORTED;
    return NULL;
}

void ini_mdep(void) {
    pthread_attr_init(&pthread_create_attr);
}

int loc_clock_gettime(int clock_id, struct timespec * now) {
    /*
     * OpenC has a bug for several releases using a timezone-sensitive time in clock_realtime().
     * gettimeofday() is more reliable.
     */
    struct timeval timenowval;
    int ret;
    assert(clock_id == CLOCK_REALTIME);
    if (!now) {
        errno = EINVAL;
        return -1;
    }
    ret = gettimeofday(&timenowval, NULL);
    if (ret < 0)
        return ret;
    now->tv_sec = timenowval.tv_sec;
    now->tv_nsec = timenowval.tv_usec * 1000L;
    return 0;
}

/**
 * Some of the dynamic IP interface scanning routines are unreliable, so
 * include a workaround to manually set the desired interface from outside.
 */
#include <tcf/framework/ip_ifc.h>

static ip_ifc_info* gSelectedIPInterface;

void set_ip_ifc(ip_ifc_info* info) {
    gSelectedIPInterface = info;
}
ip_ifc_info* get_ip_ifc(void) {
    return gSelectedIPInterface;
}

#else

#include <pwd.h>
#include <locale.h>
#include <langinfo.h>
#include <sys/utsname.h>
#if defined(__linux__)
#  include <asm/unistd.h>
#endif

#if !defined(USE_clock_gettime)
#  define USE_clock_gettime (!defined(__FreeBSD__) && !defined(__NetBSD__) && !defined(__APPLE__))
#endif

#if !USE_clock_gettime
int clock_gettime(clockid_t clock_id, struct timespec * tp) {
    struct timeval tv;

    assert(clock_id == CLOCK_REALTIME);
    if (!tp) {
        errno = EINVAL;
        return -1;
    }
    if (gettimeofday(&tv, NULL) < 0) {
        return -1;
    }
    tp->tv_sec  = tv.tv_sec;
    tp->tv_nsec = tv.tv_usec * 1000;
    return 0;
}
#endif

#if defined(__UCLIBC__)
#include <fcntl.h>

int posix_openpt(int flags) {
    return (open("/dev/ptmx", flags));
}
#endif

const char * get_os_name(void) {
    static char str[256];
    struct utsname info;
    memset(&info, 0, sizeof(info));
    uname(&info);
    assert(strlen(info.sysname) + strlen(info.release) < sizeof(str));
    snprintf(str, sizeof(str), "%s %s", info.sysname, info.release);
    return str;
}

const char * get_user_home(void) {
    static char buf[PATH_MAX];
    if (buf[0] == 0) {
        struct passwd * pwd = getpwuid(getuid());
        if (pwd == NULL) return NULL;
        strcpy(buf, pwd->pw_dir);
    }
    return buf;
}

const char * get_user_name(void) {
    static const char * login = NULL;
    if (login == NULL) {
        login = getlogin();
        if (login == NULL) {
            struct passwd * pwd = getpwuid(getuid());
            if (pwd == NULL) return NULL;
            login = pwd->pw_name;
        }
        login = loc_strdup(login);
    }
    return login;
}

int tkill(pid_t pid, int signal) {
#if defined(__linux__)
    return syscall(__NR_tkill, pid, signal);
#else
    return kill(pid, signal);
#endif
}

void ini_mdep(void) {
    setlocale(LC_ALL, "");
#ifdef CODESET
    utf8_locale = (strcmp(nl_langinfo(CODESET), "UTF-8") == 0);
#endif
    signal(SIGPIPE, SIG_IGN);
    pthread_attr_init(&pthread_create_attr);
    pthread_attr_setstacksize(&pthread_create_attr, 0x8000);
}

#endif


/** canonicalize_file_name ****************************************************/

#if defined(_WIN32)

char * canonicalize_file_name(const char * name) {
    DWORD len;
    int i = 0;
    wchar_t buf[FILE_PATH_SIZE];
    wchar_t * basename = NULL;
    wchar_t path[FILE_PATH_SIZE];
    char res[FILE_PATH_SIZE];

    assert(name != NULL);
    if (!MultiByteToWideChar(CP_UTF8, 0, name, -1, path, sizeof(path) / sizeof(wchar_t))) {
        set_win32_errno(GetLastError());
        return NULL;
    }
    len = GetFullPathNameW(path, sizeof(buf) / sizeof(wchar_t), buf, &basename);
    if (len == 0) {
        errno = ENOENT;
        return NULL;
    }
    if (len > FILE_PATH_SIZE - 1) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    while (buf[i] != 0) {
        if (buf[i] == '\\') buf[i] = '/';
        i++;
    }
    len = WideCharToMultiByte(CP_UTF8, 0, buf, -1, res, sizeof(res), NULL, NULL);
    if (len == 0) {
        set_win32_errno(GetLastError());
        return NULL;
    }
    if (len > FILE_PATH_SIZE - 1) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    return strdup(res);
}

#elif defined(_WRS_KERNEL)

#include <string.h>

char * canonicalize_file_name(const char * path) {
    char buf[PATH_MAX];
    size_t i = 0;
    size_t j = 0;
    if (path[0] == '.' && (path[1] == '/' || path[1] == '\\' || path[1] == 0)) {
        getcwd(buf, sizeof(buf));
        j = strlen(buf);
        if (j == 1 && buf[0] == '/') j = 0;
        i = 1;
    }
    else if (path[0] == '.' && path[1] == '.' && (path[2] == '/' || path[2] == '\\' || path[2] == 0)) {
        getcwd(buf, sizeof(buf));
        j = strlen(buf);
        while (j > 0 && buf[j - 1] != '/') j--;
        if (j > 0 && buf[j - 1] == '/') j--;
        i = 2;
    }
    while (path[i] && j < PATH_MAX - 1) {
        char ch = path[i];
        if (ch == '\\') ch = '/';
        if (ch == '/') {
            if (path[i + 1] == '/' || path[i + 1] == '\\') {
                i++;
                continue;
            }
            if (path[i + 1] == '.') {
                if (path[i + 2] == 0) {
                    break;
                }
                if (path[i + 2] == '/' || path[i + 2] == '\\') {
                    i += 2;
                    continue;
                }
                if ((j == 0 || buf[0] == '/') && path[i + 2] == '.') {
                    if (path[i + 3] == '/' || path[i + 3] == '\\' || path[i + 3] == 0) {
                        while (j > 0 && buf[j - 1] != '/') j--;
                        if (j > 0 && buf[j - 1] == '/') j--;
                        i += 3;
                        continue;
                    }
                }
            }
        }
        buf[j++] = ch;
        i++;
    }
    if (j == 0 && path[0] != 0) buf[j++] = '/';
    buf[j] = 0;
    return strdup(buf);
}

#elif defined(__FreeBSD__) || defined(__NetBSD__) || defined(__APPLE__)

char * canonicalize_file_name(const char * path) {
    char buf[PATH_MAX];
    char * res = realpath(path, buf);
    if (res == NULL) return NULL;
    return strdup(res);
}

#elif defined(__UCLIBC__)

char * canonicalize_file_name(const char * path) {
    return realpath(path, NULL);
}

#endif


/** getaddrinfo ***************************************************************/

#if defined(_WRS_KERNEL) && defined(USE_VXWORKS_GETADDRINFO)

/* TODO: VxWorks 6.6 getaddrinfo returns error when port is empty string, should return port 0 */
/* TODO: VxWorks 6.6 source (as shipped at 2007 fall release) does not include ipcom header files. */
extern void ipcom_freeaddrinfo();
extern int ipcom_getaddrinfo();

static struct ai_errlist {
    const char * str;
    int code;
} ai_errlist[] = {
    { "Success", 0 },
    /*
    { "Invalid value for ai_flags", IP_EAI_BADFLAGS },
    { "Non-recoverable failure in name resolution", IP_EAI_FAIL },
    { "ai_family not supported", IP_EAI_FAMILY },
    { "Memory allocation failure", IP_EAI_MEMORY },
    { "hostname nor servname provided, or not known", IP_EAI_NONAME },
    { "servname not supported for ai_socktype",     IP_EAI_SERVICE },
    { "ai_socktype not supported", IP_EAI_SOCKTYPE },
    { "System error returned in errno", IP_EAI_SYSTEM },
     */
    /* backward compatibility with userland code prior to 2553bis-02 */
    { "Address family for hostname not supported", 1 },
    { "No address associated with hostname", 7 },
    { NULL, -1 },
};

void loc_freeaddrinfo(struct addrinfo * ai) {
    ipcom_freeaddrinfo(ai);
}

int loc_getaddrinfo(const char * nodename, const char * servname,
       const struct addrinfo * hints, struct addrinfo ** res) {
    return ipcom_getaddrinfo(nodename, servname, hints, res);
}

const char * loc_gai_strerror(int ecode) {
    struct ai_errlist * p;
    static char buf[32];
    for (p = ai_errlist; p->str; p++) {
        if (p->code == ecode) return p->str;
    }
    snprintf(buf, sizeof(buf), "Error code %d", ecode);
    return buf;
}

#elif defined(_WRS_KERNEL)

union sockaddr_union {
    struct sockaddr sa;
    struct sockaddr_in sin;
    struct sockaddr_in6 sin6;
};

extern int ipcom_getsockaddrbyaddr();

void loc_freeaddrinfo(struct addrinfo * ai) {
    while (ai != NULL) {
        struct addrinfo * next = ai->ai_next;
        if (ai->ai_canonname != NULL) loc_free(ai->ai_canonname);
        if (ai->ai_addr != NULL) loc_free(ai->ai_addr);
        loc_free(ai);
        ai = next;
    }
}

int loc_getaddrinfo(const char * nodename, const char * servname,
       const struct addrinfo * hints, struct addrinfo ** res) {
    int family = 0;
    int flags = 0;
    int socktype = 0;
    int protocol = 0;
    int err;
    int port = 0;
    char * canonname = NULL;
    const char * host;
    struct addrinfo * ai;
    union sockaddr_union * sa;

    *res = NULL;

    if (hints != NULL) {
        flags = hints->ai_flags;
        family = hints->ai_family;
        socktype = hints->ai_socktype;
        protocol = hints->ai_protocol;
    }
    if (family == AF_UNSPEC) {
        struct addrinfo lhints;
        int err_v6;

        if (hints == NULL) memset(&lhints, 0, sizeof(lhints));
        else memcpy(&lhints, hints, sizeof(lhints));
        lhints.ai_family = AF_INET6;
        err_v6 = loc_getaddrinfo(nodename, servname, &lhints, res);
        lhints.ai_family = AF_INET;
        while (*res != NULL) res = &(*res)->ai_next;
        err = loc_getaddrinfo(nodename, servname, &lhints, res);
        return err && err_v6 ? err : 0;
    }
    if (servname != NULL && servname[0] != 0) {
        char * p = NULL;
        port = (unsigned int) strtoul(servname, &p, 10);
        if (port < 0 || port > 0xffff || *p != '\0' || p == servname) {
            return 1;
        }
    }
    if (nodename != NULL && nodename[0] != 0) {
        host = nodename;
    }
    else if (flags & AI_PASSIVE) {
        host = family == AF_INET ? "0.0.0.0" : "::";
    }
    else {
        host = family == AF_INET ? "127.0.0.1" : "::1";
    }
    if (socktype == 0) {
        socktype = SOCK_STREAM;
    }
    if (protocol == 0) {
        protocol = socktype == SOCK_STREAM ? IPPROTO_TCP : IPPROTO_UDP;
    }

    sa = loc_alloc_zero(sizeof(*sa));
    err = ipcom_getsockaddrbyaddr(family, host, (struct sockaddr *)sa);
    if (err) {
        loc_free(sa);
        return err;
    }

    ai = loc_alloc_zero(sizeof(*ai));
    switch (family) {
    case AF_INET:
        assert(sa->sin.sin_family == AF_INET);
        sa->sin.sin_port = (unsigned short) htons(port);
        ai->ai_addrlen = sizeof(struct sockaddr_in);
        break;
    case AF_INET6:
        assert(sa->sin6.sin6_family == AF_INET6);
        sa->sin6.sin6_port = (unsigned short) htons(port);
        ai->ai_addrlen = sizeof(struct sockaddr_in6);
        break;
    default:
        loc_free(sa);
        loc_free(ai);
        return 2;
    }

    ai->ai_flags = 0;
    ai->ai_family = family;
    ai->ai_socktype = socktype;
    ai->ai_protocol = protocol;
    ai->ai_canonname = canonname;
    ai->ai_addr = (struct sockaddr *)sa;
    ai->ai_next = NULL;
    *res = ai;
    return 0;
}

const char * loc_gai_strerror(int ecode) {
    static char buf[32];
    if (ecode == 0) return "Success";
    snprintf(buf, sizeof(buf), "Error code %d", ecode);
    return buf;
}

#elif defined(_WIN32)

const char * loc_gai_strerror(int ecode) {
    WCHAR * buf = NULL;
    static char msg[512];
    if (ecode == 0) return "Success";
    if (!FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER |
        FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS |
        FORMAT_MESSAGE_MAX_WIDTH_MASK,
        NULL,
        ecode,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        (LPWSTR)&buf, 0, NULL) ||
        !WideCharToMultiByte(CP_UTF8, 0, buf, -1, msg, sizeof(msg), NULL, NULL))
    {
        snprintf(msg, sizeof(msg), "GAI Error Code %d", ecode);
    }
    if (buf != NULL) LocalFree(buf);
    return msg;
}

#elif defined(__SYMBIAN32__)

const char * loc_gai_strerror(int ecode) {
    static char buf[32];
    if (ecode == 0) return "Success";
    snprintf(buf, sizeof(buf), "Error code %d", ecode);
    return buf;
}

#endif

#if defined(_WIN32)
#  include <tlhelp32.h>
#  ifdef _MSC_VER
#    pragma warning(disable:4201) /* nonstandard extension used : nameless struct/union (in winternl.h) */
#    include <winternl.h>
#  else
#    include <ntdef.h>
#  endif
#  ifndef STATUS_INFO_LENGTH_MISMATCH
#   define STATUS_INFO_LENGTH_MISMATCH      ((NTSTATUS)0xC0000004L)
#  endif
#  ifndef SystemHandleInformation
#    define SystemHandleInformation 16
#  endif

/* Disable inheritance for all handles owned by process */
static NTSTATUS disable_handle_inheritance(void) {
    typedef struct _HANDLE_INFORMATION {
            USHORT ProcessId;
            USHORT CreatorBackTraceIndex;
            UCHAR ObjectTypeNumber;
            UCHAR Flags;
            USHORT Handle;
            PVOID Object;
            ACCESS_MASK GrantedAccess;
    } HANDLE_INFORMATION;
    typedef struct _SYSTEM_HANDLE_INFORMATION {
        ULONG Count;
        HANDLE_INFORMATION Handles[1];
    } SYSTEM_HANDLE_INFORMATION;
    typedef NTSTATUS (FAR WINAPI * QuerySystemInformationTypedef)(int, PVOID, ULONG, PULONG);
    QuerySystemInformationTypedef QuerySystemInformationProc = (QuerySystemInformationTypedef)GetProcAddress(
        GetModuleHandle("NTDLL.DLL"), "NtQuerySystemInformation");
    DWORD size;
    NTSTATUS status;
    SYSTEM_HANDLE_INFORMATION * hi = NULL;

    size = sizeof(SYSTEM_HANDLE_INFORMATION) + sizeof(HANDLE_INFORMATION) * 256;
    hi = (SYSTEM_HANDLE_INFORMATION *)tmp_alloc_zero(size);
    for (;;) {
        status = QuerySystemInformationProc(SystemHandleInformation, hi, size, &size);
        if (status != STATUS_INFO_LENGTH_MISMATCH) break;
        hi = (SYSTEM_HANDLE_INFORMATION *)tmp_realloc(hi, size);
    }
    if (status == 0) {
        ULONG i;
        DWORD id = GetCurrentProcessId();
        for (i = 0; i < hi->Count; i++) {
            if (hi->Handles[i].ProcessId != id) continue;
            SetHandleInformation((HANDLE)(uintptr_t)hi->Handles[i].Handle, HANDLE_FLAG_INHERIT, 0);
        }
    }
    return status;
}

static char *make_cmd_from_args(char **args) {
    int i = 0;
    int cmd_size = 0;
    int cmd_pos = 0;
    char *cmd = NULL;

#  define cmd_append(ch) { \
        if (cmd_pos >= cmd_size) { \
            cmd_size += 0x1000; \
            cmd = (char *)loc_realloc(cmd, cmd_size); \
        } \
        cmd[cmd_pos++] = (ch); \
    }
    while (args[i] != NULL) {
        const char * p = args[i++];
        if (cmd_pos > 0) cmd_append(' ');
        cmd_append('"');
        while (*p) {
            if (*p == '"') cmd_append('\\');
            cmd_append(*p);
            p++;
        }
        cmd_append('"');
    }
    cmd_append(0);
#  undef cmd_append
    return cmd;
}

static int running_as_daemon = 0;

int is_daemon(void) {
    return running_as_daemon;
}

#  if !defined(__CYGWIN__)
#    define pipe(fds) _pipe((fds), 1024, 0)
#  endif

void become_daemon(char **args) {
    int fdpairs[4];
    int npairs = 2;
    char fnm[FILE_PATH_SIZE];
    STARTUPINFO startupInfo;
    PROCESS_INFORMATION prs_info;
    int i;

    assert(!running_as_daemon);
    running_as_daemon = 1;

    if (args == NULL)
        return;

    fflush(stdout);
    fflush(stderr);

    /* Make sure no handles are inherited by new process */
    disable_handle_inheritance();

    if (pipe(fdpairs) < 0) {
        perror("pipe");
        exit(1);
    }

    if (pipe(fdpairs + 2) < 0) {
        perror("pipe");
        exit(1);
    }

    if (GetModuleFileName(NULL, fnm, sizeof(fnm)) == 0) {
        fprintf(stderr, "GetModuleFileName failed\n");
        exit(1);
    }

    memset(&startupInfo, 0, sizeof startupInfo);
    startupInfo.dwFlags |= STARTF_USESTDHANDLES;
    startupInfo.hStdInput = INVALID_HANDLE_VALUE;
    startupInfo.hStdOutput = (HANDLE)_get_osfhandle(fdpairs[1]);
    startupInfo.hStdError = (HANDLE)_get_osfhandle(fdpairs[3]);
    SetHandleInformation(startupInfo.hStdOutput, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    SetHandleInformation(startupInfo.hStdError, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);

    if (!CreateProcess(fnm, make_cmd_from_args(args), NULL, NULL, TRUE,
                       DETACHED_PROCESS, NULL, NULL, &startupInfo, &prs_info)) {
        fprintf(stderr, "CreateProcess failed 0x%lx\n", GetLastError());
        exit(1);
    }

    for (i = 0; i < 2; i++) {
        /* Make read side non-blocking */
        DWORD pipemode = PIPE_READMODE_BYTE | PIPE_NOWAIT;
        if (!SetNamedPipeHandleState((HANDLE)_get_osfhandle(fdpairs[i*2]), &pipemode, NULL, NULL)) {
            fprintf(stderr, "SetNamedPipeHandleState failed 0x%lx\n", GetLastError());
            exit(1);
        }

        /* Close write side of pipes so we get end of file as soon as
         * the new them or exits */
        close(fdpairs[i*2 + 1]);
        fdpairs[i*2 + 1] = 1;
    }

    while (npairs > 0) {
        for (i = 0; i < npairs; i++) {
            char tmpbuf[1000];
            DWORD size;
            if (ReadFile((HANDLE)_get_osfhandle(fdpairs[i*2]), tmpbuf, sizeof tmpbuf, &size, NULL)) {
                if (write(fdpairs[i*2 + 1], tmpbuf, size) < 0)
                    perror("write");
            } else if (GetLastError() != ERROR_NO_DATA) {
                if (GetLastError() != ERROR_BROKEN_PIPE) {
                    fprintf(stderr, "SetNamedPipeHandleState failed 0x%lx\n", GetLastError());
                }
                fdpairs[i*2] = fdpairs[(npairs - 1)*2];
                fdpairs[i*2 + 1] = fdpairs[(npairs - 1)*2 + 1];
                npairs--;
            }
        }
        /* Neither select nor overlapped I/O works for anonymous pipes, so use
         * polling for now until a better solution if found... */
        usleep(1000);
    }

    exit(0);
}

void close_out_and_err(void) {
    int fd = open("nul", O_WRONLY, 0);
    if (fd < 0) {
        perror("open nul");
        exit(1);
    }

    fflush(stdout);
    fflush(stderr);

    dup2(fd, 1);
    dup2(fd, 2);
}

#elif defined(_WRS_KERNEL) || defined (__SYMBIAN32__)

int is_daemon(void) {
    return 0;
}

void become_daemon(void) {
    fprintf(stderr, "tcf-agent: Running in the background is not supported on %s\n", get_os_name());
    exit(1);
}

void close_out_and_err(void) {
}

#else

static int running_as_daemon = 0;

int is_daemon(void) {
    return running_as_daemon;
}

void become_daemon(void) {
    int fdpairs[4];
    int npairs = 2;

    assert(!running_as_daemon);

    fflush(stdout);
    fflush(stderr);

    if (pipe(fdpairs) < 0) {
        perror("pipe");
        exit(1);
    }

    if (pipe(fdpairs + 2) < 0) {
        perror("pipe");
        exit(1);
    }

    /* Fork a new process so we can close everything except the pipes */
    switch (fork()) {
    default:                        /* Parent process */
        /* Close write side of pipes so we get end of file as soon as
         * child closes them or exits */
        close(fdpairs[1]);
        close(fdpairs[3]);
        fdpairs[1] = 1;
        fdpairs[3] = 2;

        while (npairs > 0) {
            int i;
            int rval;
            int nfds = 0;
            fd_set readfds;
            char tmpbuf[1000];

            FD_ZERO(&readfds);
            for (i = 0; i < npairs; i++) {
                int fd = fdpairs[i*2];
                FD_SET(fd, &readfds);
                if (nfds < fd)
                    nfds = fd;
            }
            rval = select(nfds + 1, &readfds, NULL, NULL, NULL);
            if (rval < 0) {
                if (errno == EINTR)
                    continue;
                perror("select");
                _exit(1);
            }
            assert(rval > 0);
            for (i = 0; i < npairs; i++) {
                int fd = fdpairs[i*2];
                if (FD_ISSET(fd, &readfds)) {
                    rval = read(fd, tmpbuf, sizeof tmpbuf);
                    if (rval > 0) {
                        if (write(fdpairs[i*2 + 1], tmpbuf, rval) < 0)
                            perror("write");
                    } else {
                        if (rval < 0)
                            perror("read");
                        fdpairs[i*2] = fdpairs[(npairs - 1)*2];
                        fdpairs[i*2 + 1] = fdpairs[(npairs - 1)*2 + 1];
                        npairs--;
                    }
                }
            }
        }
        _exit(0);
        break;

    case 0: {                       /* Child process */
        /* Replace stdin with /dev/null */
        int fd = open("/dev/null", O_RDONLY);
        if (fd < 0) {
            perror("open /dev/null");
            exit(1);
        }
        dup2(fd, 0);
        dup2(fdpairs[1], 1);
        dup2(fdpairs[3], 2);

        /* Close all open files except stdin, stdout and stderr */
        fd = sysconf(_SC_OPEN_MAX);
        while (fd-- > 3)
            close(fd);

        /* Fork a new process so it is owned by init */
        switch (fork()) {
        default:                    /* Parent process */
            _exit(0);
            break;

        case 0:                     /* Child process */
            /* Create new session */
            if (setsid() == (pid_t)-1) {
                perror("setsid");
                _exit(1);
            }

            running_as_daemon = 1;
            return;

        case -1:                    /* Fork failed */
            perror("fork");
            exit(1);
        }
    }

    case -1:                        /* Fork failed */
        perror("fork");
        exit(1);
    }
}

void close_out_and_err(void) {
    int fd = open("/dev/null", O_WRONLY);
    if (fd < 0) {
        perror("open /dev/null");
        exit(1);
    }

    fflush(stdout);
    fflush(stderr);

    dup2(fd, 1);
    dup2(fd, 2);
}
#endif

#if !defined(__FreeBSD__) && !defined(__NetBSD__) && !defined(__APPLE__) && !defined(__VXWORKS__)

size_t strlcpy(char * dst, const char * src, size_t size) {
    char ch;
    const char * src0 = src;
    const char * dst1 = dst + size - 1;

    while ((ch = *src) != 0) {
        if (dst < dst1) *dst++ = ch;
        src++;
    }
    if (dst <= dst1) *dst = 0;
    return src - src0;
}

size_t strlcat(char * dst, const char * src, size_t size) {
    char ch;
    const char * dst0 = dst;
    const char * src0 = src;
    const char * dst1 = dst + size - 1;

    while (dst <= dst1 && *dst != 0) dst++;

    while ((ch = *src) != 0) {
        if (dst < dst1) *dst++ = ch;
        src++;
    }
    if (dst <= dst1) *dst = 0;
    return (dst - dst0) + (src - src0);
}

#endif

#if !defined(USE_uuid_generate)
#  define USE_uuid_generate (defined(__linux__) && !defined(__UCLIBC__) && !defined(ANDROID))
#endif

#if USE_uuid_generate

#include <uuid/uuid.h>

const char * create_uuid(void) {
    uuid_t id;
    static char buf[64];
    uuid_generate(id);
    uuid_unparse(id, buf);
    return buf;
}

#else

const char * create_uuid(void) {
    static char buf[40];
    struct timespec time_now;
    memset(&time_now, 0, sizeof(time_now));
    if (clock_gettime(CLOCK_REALTIME, &time_now)) check_error(errno);
    if (buf[0] == 0) srand((unsigned int)time_now.tv_sec ^ (unsigned int)time_now.tv_nsec);
    snprintf(buf, sizeof(buf), "%08x-%04x-4%03x-8%03x-%04x%04x%04x",
        (int)time_now.tv_sec & 0xffffffff, (int)(time_now.tv_nsec >> 13) & 0xffff,
        rand() & 0xfff, rand() & 0xfff, rand() & 0xffff, rand() & 0xffff, rand() & 0xffff);
    return buf;
}

#endif
