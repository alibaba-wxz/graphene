#include "ocall_types.h"
#include "ecall_types.h"
#include "sgx_internal.h"
#include "sgx_enclave.h"
#include "pal_security.h"
#include "pal_linux_error.h"

#include <asm/mman.h>
#include <asm/ioctls.h>
#include <asm/socket.h>
#include <linux/fs.h>
#include <linux/in.h>
#include <linux/in6.h>
#include <math.h>
#include <asm/errno.h>

#ifndef SOL_IPV6
# define SOL_IPV6 41
#endif

#define ODEBUG(code, ms) do {} while (0)

static int sgx_ocall_exit(void* pms)
{
    ms_ocall_exit_t * ms = (ms_ocall_exit_t *) pms;
    ODEBUG(OCALL_EXIT, NULL);

    if (ms->ms_exitcode != (int) ((uint8_t) ms->ms_exitcode)) {
        SGX_DBG(DBG_E, "Saturation error in exit code %d, getting rounded down to %u\n",
                ms->ms_exitcode, (uint8_t) ms->ms_exitcode);
        ms->ms_exitcode = 255;
    }

    /* exit the whole process if exit_group() */
    if (ms->ms_is_exitgroup)
        INLINE_SYSCALL(exit_group, 1, (int)ms->ms_exitcode);

    /* otherwise call SGX-related thread reset and exit this thread */
    block_async_signals(true);
    ecall_thread_reset();

    unmap_tcs();
    thread_exit((int)ms->ms_exitcode);
    return 0;
}

static int sgx_ocall_mmap_untrusted(void * pms)
{
    ms_ocall_mmap_untrusted_t * ms = (ms_ocall_mmap_untrusted_t *) pms;
    void * addr;

    ODEBUG(OCALL_MMAP_UNTRUSTED, ms);
    addr = (void *) INLINE_SYSCALL(mmap, 6, NULL, ms->ms_size,
                                   ms->ms_prot,
                                   (ms->ms_fd == -1) ? MAP_ANONYMOUS | MAP_PRIVATE
                                                     : MAP_FILE | MAP_SHARED,
                                   ms->ms_fd, ms->ms_offset);
    if (IS_ERR_P(addr))
        return -ERRNO_P(addr);

    ms->ms_mem = addr;
    return 0;
}

static int sgx_ocall_munmap_untrusted(void * pms)
{
    ms_ocall_munmap_untrusted_t * ms = (ms_ocall_munmap_untrusted_t *) pms;
    ODEBUG(OCALL_MUNMAP_UNTRUSTED, ms);
    INLINE_SYSCALL(munmap, 2, ALLOC_ALIGNDOWN(ms->ms_mem),
                   ALLOC_ALIGNUP(ms->ms_mem + ms->ms_size) -
                   ALLOC_ALIGNDOWN(ms->ms_mem));
    return 0;
}

static int sgx_ocall_cpuid(void * pms)
{
    ms_ocall_cpuid_t * ms = (ms_ocall_cpuid_t *) pms;
    ODEBUG(OCALL_CPUID, ms);
    __asm__ volatile ("cpuid"
                  : "=a"(ms->ms_values[0]),
                    "=b"(ms->ms_values[1]),
                    "=c"(ms->ms_values[2]),
                    "=d"(ms->ms_values[3])
                  : "a"(ms->ms_leaf), "c"(ms->ms_subleaf) : "memory");
    return 0;
}

static int sgx_ocall_open(void * pms)
{
    ms_ocall_open_t * ms = (ms_ocall_open_t *) pms;
    int ret;
    ODEBUG(OCALL_OPEN, ms);
    ret = INLINE_SYSCALL(open, 3, ms->ms_pathname, ms->ms_flags|O_CLOEXEC,
                         ms->ms_mode);
    return ret;
}

static int sgx_ocall_close(void * pms)
{
    ms_ocall_close_t * ms = (ms_ocall_close_t *) pms;
    ODEBUG(OCALL_CLOSE, ms);
    INLINE_SYSCALL(close, 1, ms->ms_fd);
    return 0;
}

static int sgx_ocall_read(void * pms)
{
    ms_ocall_read_t * ms = (ms_ocall_read_t *) pms;
    int ret;
    ODEBUG(OCALL_READ, ms);
    ret = INLINE_SYSCALL(read, 3, ms->ms_fd, ms->ms_buf, ms->ms_count);
    return ret;
}

static int sgx_ocall_write(void * pms)
{
    ms_ocall_write_t * ms = (ms_ocall_write_t *) pms;
    int ret;
    ODEBUG(OCALL_WRITE, ms);
    ret = INLINE_SYSCALL(write, 3, ms->ms_fd, ms->ms_buf, ms->ms_count);
    return ret;
}

static int sgx_ocall_fstat(void * pms)
{
    ms_ocall_fstat_t * ms = (ms_ocall_fstat_t *) pms;
    int ret;
    ODEBUG(OCALL_FSTAT, ms);
    ret = INLINE_SYSCALL(fstat, 2, ms->ms_fd, &ms->ms_stat);
    return ret;
}

static int sgx_ocall_fionread(void * pms)
{
    ms_ocall_fionread_t * ms = (ms_ocall_fionread_t *) pms;
    int ret, val;
    ODEBUG(OCALL_FIONREAD, ms);
    ret = INLINE_SYSCALL(ioctl, 3, ms->ms_fd, FIONREAD, &val);
    return IS_ERR(ret) ? ret : val;
}

static int sgx_ocall_fsetnonblock(void * pms)
{
    ms_ocall_fsetnonblock_t * ms = (ms_ocall_fsetnonblock_t *) pms;
    int ret, flags;
    ODEBUG(OCALL_FSETNONBLOCK, ms);

    ret = INLINE_SYSCALL(fcntl, 2, ms->ms_fd, F_GETFL);
    if (IS_ERR(ret))
        return ret;

    flags = ret;
    if (ms->ms_nonblocking) {
        if (!(flags & O_NONBLOCK))
            ret = INLINE_SYSCALL(fcntl, 3, ms->ms_fd, F_SETFL,
                                 flags | O_NONBLOCK);
    } else {
        if (flags & O_NONBLOCK)
            ret = INLINE_SYSCALL(fcntl, 3, ms->ms_fd, F_SETFL,
                                 flags & ~O_NONBLOCK);
    }

    return ret;
}

static int sgx_ocall_fchmod(void * pms)
{
    ms_ocall_fchmod_t * ms = (ms_ocall_fchmod_t *) pms;
    int ret;
    ODEBUG(OCALL_FCHMOD, ms);
    ret = INLINE_SYSCALL(fchmod, 2, ms->ms_fd, ms->ms_mode);
    return ret;
}

static int sgx_ocall_fsync(void * pms)
{
    ms_ocall_fsync_t * ms = (ms_ocall_fsync_t *) pms;
    ODEBUG(OCALL_FSYNC, ms);
    INLINE_SYSCALL(fsync, 1, ms->ms_fd);
    return 0;
}

static int sgx_ocall_ftruncate(void * pms)
{
    ms_ocall_ftruncate_t * ms = (ms_ocall_ftruncate_t *) pms;
    int ret;
    ODEBUG(OCALL_FTRUNCATE, ms);
    ret = INLINE_SYSCALL(ftruncate, 2, ms->ms_fd, ms->ms_length);
    return ret;
}

static int sgx_ocall_lseek(void* pms) {
    ms_ocall_lseek_t* ms = (ms_ocall_lseek_t*)pms;
    int ret;
    ODEBUG(OCALL_LSEEK, ms);
    ret = INLINE_SYSCALL(lseek, 3, ms->ms_fd, ms->ms_offset, ms->ms_whence);
    return ret;
}

static int sgx_ocall_mkdir(void * pms)
{
    ms_ocall_mkdir_t * ms = (ms_ocall_mkdir_t *) pms;
    int ret;
    ODEBUG(OCALL_MKDIR, ms);
    ret = INLINE_SYSCALL(mkdir, 2, ms->ms_pathname, ms->ms_mode);
    return ret;
}

static int sgx_ocall_getdents(void * pms)
{
    ms_ocall_getdents_t * ms = (ms_ocall_getdents_t *) pms;
    int ret;
    ODEBUG(OCALL_GETDENTS, ms);
    ret = INLINE_SYSCALL(getdents64, 3, ms->ms_fd, ms->ms_dirp, ms->ms_size);
    return ret;
}

static int sgx_ocall_resume_thread(void * pms)
{
    ODEBUG(OCALL_RESUME_THREAD, pms);
    return interrupt_thread(pms);
}

static int sgx_ocall_clone_thread(void * pms)
{
    __UNUSED(pms);
    ODEBUG(OCALL_CLONE_THREAD, pms);
    return clone_thread();
}

static int sgx_ocall_create_process(void * pms)
{
    ms_ocall_create_process_t * ms = (ms_ocall_create_process_t *) pms;
    ODEBUG(OCALL_CREATE_PROCESS, ms);
    int ret = sgx_create_process(ms->ms_uri, ms->ms_nargs, ms->ms_args, ms->ms_proc_fds);
    if (ret < 0)
        return ret;
    ms->ms_pid = ret;
    return 0;
}

static int sgx_ocall_futex(void * pms)
{
    ms_ocall_futex_t * ms = (ms_ocall_futex_t *) pms;
    int ret;
    ODEBUG(OCALL_FUTEX, ms);
    struct timespec* ts = NULL;
    if (ms->ms_timeout_us >= 0) {
        ts = __alloca(sizeof(struct timespec));
        ts->tv_sec = ms->ms_timeout_us / 1000000;
        ts->tv_nsec = (ms->ms_timeout_us - ts->tv_sec * 1000000) * 1000;
    }
    ret = INLINE_SYSCALL(futex, 6, ms->ms_futex, ms->ms_op, ms->ms_val,
                         ts, NULL, 0);
    return ret;
}

static int sgx_ocall_socketpair(void * pms)
{
    ms_ocall_socketpair_t * ms = (ms_ocall_socketpair_t *) pms;
    int ret;
    ODEBUG(OCALL_SOCKETPAIR, ms);
    ret = INLINE_SYSCALL(socketpair, 4, ms->ms_domain,
                         ms->ms_type|SOCK_CLOEXEC,
                         ms->ms_protocol, &ms->ms_sockfds);
    return ret;
}

static int sock_getopt(int fd, struct sockopt * opt)
{
    SGX_DBG(DBG_M, "sock_getopt (fd = %d, sockopt addr = %p) is not implemented \
            always returns 0\n", fd, opt);
    /* initialize *opt with constant */
    *opt = (struct sockopt){0};
    opt->reuseaddr = 1;
    return 0;
}

static int sgx_ocall_sock_listen(void * pms)
{
    ms_ocall_sock_listen_t * ms = (ms_ocall_sock_listen_t *) pms;
    int ret, fd;
    ODEBUG(OCALL_SOCK_LISTEN, ms);

    ret = INLINE_SYSCALL(socket, 3, ms->ms_domain,
                         ms->ms_type|SOCK_CLOEXEC,
                         ms->ms_protocol);
    if (IS_ERR(ret))
        goto err;

    fd = ret;
    if (ms->ms_addr->sa_family == AF_INET6) {
        int ipv6only = 1;
        INLINE_SYSCALL(setsockopt, 5, fd, SOL_IPV6, IPV6_V6ONLY, &ipv6only,
                       sizeof(int));
    }
    /* must set the socket to be reuseable */
    int reuseaddr = 1;
    INLINE_SYSCALL(setsockopt, 5, fd, SOL_SOCKET, SO_REUSEADDR, &reuseaddr,
                   sizeof(int));

    ret = INLINE_SYSCALL(bind, 3, fd, ms->ms_addr, ms->ms_addrlen);
    if (IS_ERR(ret))
        goto err_fd;

    if (ms->ms_addr) {
        socklen_t addrlen = ms->ms_addrlen;
        ret = INLINE_SYSCALL(getsockname, 3, fd, ms->ms_addr, &addrlen);
        if (IS_ERR(ret))
            goto err_fd;
        ms->ms_addrlen = addrlen;
    }

    if (ms->ms_type & SOCK_STREAM) {
        ret = INLINE_SYSCALL(listen, 2, fd, DEFAULT_BACKLOG);
        if (IS_ERR(ret))
            goto err_fd;
    }

    ret = sock_getopt(fd, &ms->ms_sockopt);
    if (IS_ERR(ret))
        goto err_fd;

    return fd;

err_fd:
    INLINE_SYSCALL(close, 1, fd);
err:
    return ret;
}

static int sgx_ocall_sock_accept(void * pms)
{
    ms_ocall_sock_accept_t * ms = (ms_ocall_sock_accept_t *) pms;
    int ret, fd;
    ODEBUG(OCALL_SOCK_ACCEPT, ms);
    socklen_t addrlen = ms->ms_addrlen;

    ret = INLINE_SYSCALL(accept4, 4, ms->ms_sockfd, ms->ms_addr,
                         &addrlen, O_CLOEXEC);
    if (IS_ERR(ret))
        goto err;

    fd = ret;
    ret = sock_getopt(fd, &ms->ms_sockopt);
    if (IS_ERR(ret))
        goto err_fd;

    ms->ms_addrlen = addrlen;
    return fd;

err_fd:
    INLINE_SYSCALL(close, 1, fd);
err:
    return ret;
}

static int sgx_ocall_sock_connect(void * pms)
{
    ms_ocall_sock_connect_t * ms = (ms_ocall_sock_connect_t *) pms;
    int ret, fd;
    ODEBUG(OCALL_SOCK_CONNECT, ms);

    ret = INLINE_SYSCALL(socket, 3, ms->ms_domain,
                         ms->ms_type|SOCK_CLOEXEC,
                         ms->ms_protocol);
    if (IS_ERR(ret))
        goto err;

    fd = ret;
    if (ms->ms_addr && ms->ms_addr->sa_family == AF_INET6) {
        int ipv6only = 1;
        INLINE_SYSCALL(setsockopt, 5, fd, SOL_IPV6, IPV6_V6ONLY, &ipv6only,
                       sizeof(int));
    }

    if (ms->ms_bind_addr && ms->ms_bind_addr->sa_family) {
        ret = INLINE_SYSCALL(bind, 3, fd, ms->ms_bind_addr,
                             ms->ms_bind_addrlen);
        if (IS_ERR(ret))
            goto err_fd;
    }

    if (ms->ms_addr) {
        ret = INLINE_SYSCALL(connect, 3, fd, ms->ms_addr, ms->ms_addrlen);

        if (IS_ERR(ret) && ERRNO(ret) == EINPROGRESS) {
            do {
                struct pollfd pfd = { .fd = fd, .events = POLLOUT, .revents = 0, };
                ret = INLINE_SYSCALL(ppoll, 4, &pfd, 1, NULL, NULL);
            } while (IS_ERR(ret) &&
                    ERRNO(ret) == -EWOULDBLOCK);
        }

        if (IS_ERR(ret))
            goto err_fd;
    }

    if (ms->ms_bind_addr && !ms->ms_bind_addr->sa_family) {
        socklen_t addrlen = ms->ms_bind_addrlen;
        ret = INLINE_SYSCALL(getsockname, 3, fd, ms->ms_bind_addr,
                             &addrlen);
        if (IS_ERR(ret))
            goto err_fd;
        ms->ms_bind_addrlen = addrlen;
    }

    ret = sock_getopt(fd, &ms->ms_sockopt);
    if (IS_ERR(ret))
        goto err_fd;

    return fd;

err_fd:
    INLINE_SYSCALL(close, 1, fd);
err:
    return ret;
}

static int sgx_ocall_sock_recv(void * pms)
{
    ms_ocall_sock_recv_t * ms = (ms_ocall_sock_recv_t *) pms;
    int ret;
    ODEBUG(OCALL_SOCK_RECV, ms);
    struct sockaddr * addr = ms->ms_addr;
    socklen_t addrlen = ms->ms_addr ? ms->ms_addrlen : 0;

    if (ms->ms_sockfd == pal_enclave.pal_sec.mcast_srv) {
        addr = NULL;
        addrlen = 0;
    }

    struct msghdr hdr;
    struct iovec iov[1];

    iov[0].iov_base    = ms->ms_buf;
    iov[0].iov_len     = ms->ms_count;
    hdr.msg_name       = addr;
    hdr.msg_namelen    = addrlen;
    hdr.msg_iov        = iov;
    hdr.msg_iovlen     = 1;
    hdr.msg_control    = ms->ms_control;
    hdr.msg_controllen = ms->ms_controllen;
    hdr.msg_flags      = 0;

    ret = INLINE_SYSCALL(recvmsg, 3, ms->ms_sockfd, &hdr, 0);

    if (!IS_ERR(ret) && hdr.msg_name) {
        /* note that ms->ms_addr is filled by recvmsg() itself */
        ms->ms_addrlen = hdr.msg_namelen;
    }

    if (!IS_ERR(ret) && hdr.msg_control) {
        /* note that ms->ms_control is filled by recvmsg() itself */
        ms->ms_controllen = hdr.msg_controllen;
    }

    return ret;
}

static int sgx_ocall_sock_send(void * pms)
{
    ms_ocall_sock_send_t * ms = (ms_ocall_sock_send_t *) pms;
    int ret;
    ODEBUG(OCALL_SOCK_SEND, ms);
    const struct sockaddr * addr = ms->ms_addr;
    socklen_t addrlen = ms->ms_addr ? ms->ms_addrlen : 0;
    struct sockaddr_in mcast_addr;

    if (ms->ms_sockfd == pal_enclave.pal_sec.mcast_srv) {
        mcast_addr.sin_family = AF_INET;
        inet_pton4(MCAST_GROUP, sizeof(MCAST_GROUP),  &mcast_addr.sin_addr.s_addr);
        mcast_addr.sin_port = htons(pal_enclave.pal_sec.mcast_port);
        addr = (struct sockaddr *) &mcast_addr;
        addrlen = sizeof(struct sockaddr_in);
    }

    struct msghdr hdr;
    struct iovec iov[1];

    iov[0].iov_base    = (void*)ms->ms_buf;
    iov[0].iov_len     = ms->ms_count;
    hdr.msg_name       = (void*)addr;
    hdr.msg_namelen    = addrlen;
    hdr.msg_iov        = iov;
    hdr.msg_iovlen     = 1;
    hdr.msg_control    = ms->ms_control;
    hdr.msg_controllen = ms->ms_controllen;
    hdr.msg_flags      = 0;

    ret = INLINE_SYSCALL(sendmsg, 3, ms->ms_sockfd, &hdr, MSG_NOSIGNAL);
    return ret;
}

static int sgx_ocall_sock_setopt(void * pms)
{
    ms_ocall_sock_setopt_t * ms = (ms_ocall_sock_setopt_t *) pms;
    int ret;
    ODEBUG(OCALL_SOCK_SETOPT, ms);
    ret = INLINE_SYSCALL(setsockopt, 5,
                         ms->ms_sockfd, ms->ms_level, ms->ms_optname,
                         ms->ms_optval, ms->ms_optlen);
    return ret;
}

static int sgx_ocall_sock_shutdown(void * pms)
{
    ms_ocall_sock_shutdown_t * ms = (ms_ocall_sock_shutdown_t *) pms;
    ODEBUG(OCALL_SOCK_SHUTDOWN, ms);
    INLINE_SYSCALL(shutdown, 2, ms->ms_sockfd, ms->ms_how);
    return 0;
}

static int sgx_ocall_gettime(void * pms)
{
    ms_ocall_gettime_t * ms = (ms_ocall_gettime_t *) pms;
    ODEBUG(OCALL_GETTIME, ms);
    struct timeval tv;
    INLINE_SYSCALL(gettimeofday, 2, &tv, NULL);
    ms->ms_microsec = tv.tv_sec * 1000000UL + tv.tv_usec;
    return 0;
}

static int sgx_ocall_sleep(void * pms)
{
    ms_ocall_sleep_t * ms = (ms_ocall_sleep_t *) pms;
    int ret;
    ODEBUG(OCALL_SLEEP, ms);
    if (!ms->ms_microsec) {
        INLINE_SYSCALL(sched_yield, 0);
        return 0;
    }
    struct timespec req, rem;
    unsigned long microsec = ms->ms_microsec;
    const unsigned long VERY_LONG_TIME_IN_US = 1000000L * 60 * 60 * 24 * 365 * 128;
    if (ms->ms_microsec > VERY_LONG_TIME_IN_US) {
        /* avoid overflow with time_t */
        req.tv_sec  = VERY_LONG_TIME_IN_US / 1000000;
        req.tv_nsec = 0;
    } else {
        req.tv_sec = ms->ms_microsec / 1000000;
        req.tv_nsec = (microsec - req.tv_sec * 1000000) * 1000;
    }

    ret = INLINE_SYSCALL(nanosleep, 2, &req, &rem);
    if (IS_ERR(ret) && ERRNO(ret) == EINTR)
        ms->ms_microsec = rem.tv_sec * 1000000UL + rem.tv_nsec / 1000UL;
    return ret;
}

static int sgx_ocall_poll(void * pms)
{
    ms_ocall_poll_t * ms = (ms_ocall_poll_t *) pms;
    int ret;
    ODEBUG(OCALL_POLL, ms);
    struct timespec * ts = NULL;
    if (ms->ms_timeout_us >= 0) {
        ts = __alloca(sizeof(struct timespec));
        ts->tv_sec = ms->ms_timeout_us / 1000000;
        ts->tv_nsec = (ms->ms_timeout_us - ts->tv_sec * 1000000) * 1000;
    }
    ret = INLINE_SYSCALL(ppoll, 4, ms->ms_fds, ms->ms_nfds, ts, NULL);
    return ret;
}

static int sgx_ocall_rename(void * pms)
{
    ms_ocall_rename_t * ms = (ms_ocall_rename_t *) pms;
    int ret;
    ODEBUG(OCALL_RENAME, ms);
    ret = INLINE_SYSCALL(rename, 2, ms->ms_oldpath, ms->ms_newpath);
    return ret;
}

static int sgx_ocall_delete(void * pms)
{
    ms_ocall_delete_t * ms = (ms_ocall_delete_t *) pms;
    int ret;
    ODEBUG(OCALL_DELETE, ms);

    ret = INLINE_SYSCALL(unlink, 1, ms->ms_pathname);

    if (IS_ERR(ret) && ERRNO(ret) == EISDIR)
        ret = INLINE_SYSCALL(rmdir, 1, ms->ms_pathname);

    return ret;
}

static int sgx_ocall_eventfd (void * pms)
{
    ms_ocall_eventfd_t * ms = (ms_ocall_eventfd_t *) pms;
    int ret;
    ODEBUG(OCALL_EVENTFD, ms);

    ret = INLINE_SYSCALL(eventfd2, 2, ms->ms_initval, ms->ms_flags);

    return ret;
}

void load_gdb_command (const char * command);

static int sgx_ocall_load_debug(void * pms)
{
    const char * command = (const char *) pms;
    ODEBUG(OCALL_LOAD_DEBUG, (void *) command);
    load_gdb_command(command);
    return 0;
}

static int sgx_ocall_get_attestation(void* pms) {
    ms_ocall_get_attestation_t * ms = (ms_ocall_get_attestation_t *) pms;
    ODEBUG(OCALL_GET_ATTESTATION, ms);
    return retrieve_verified_quote(&ms->ms_spid, ms->ms_subkey, ms->ms_linkable, &ms->ms_report,
                                   &ms->ms_nonce, &ms->ms_attestation);
}


static int sgx_ocall_fcntl(void* pms) {
    ms_ocall_fcntl_t * ms = (ms_ocall_fcntl_t *)pms;
    ODEBUG(OCALL_FCNTL, ms);

    int ret = INLINE_SYSCALL(fcntl, 3, ms->ms_fd, ms->ms_cmd, ms->ms_flock);

    return ret;
}

sgx_ocall_fn_t ocall_table[OCALL_NR] = {
        [OCALL_EXIT]            = sgx_ocall_exit,
        [OCALL_MMAP_UNTRUSTED]  = sgx_ocall_mmap_untrusted,
        [OCALL_MUNMAP_UNTRUSTED]= sgx_ocall_munmap_untrusted,
        [OCALL_CPUID]           = sgx_ocall_cpuid,
        [OCALL_OPEN]            = sgx_ocall_open,
        [OCALL_CLOSE]           = sgx_ocall_close,
        [OCALL_READ]            = sgx_ocall_read,
        [OCALL_WRITE]           = sgx_ocall_write,
        [OCALL_FSTAT]           = sgx_ocall_fstat,
        [OCALL_FIONREAD]        = sgx_ocall_fionread,
        [OCALL_FSETNONBLOCK]    = sgx_ocall_fsetnonblock,
        [OCALL_FCHMOD]          = sgx_ocall_fchmod,
        [OCALL_FSYNC]           = sgx_ocall_fsync,
        [OCALL_FTRUNCATE]       = sgx_ocall_ftruncate,
        [OCALL_LSEEK]           = sgx_ocall_lseek,
        [OCALL_MKDIR]           = sgx_ocall_mkdir,
        [OCALL_GETDENTS]        = sgx_ocall_getdents,
        [OCALL_RESUME_THREAD]   = sgx_ocall_resume_thread,
        [OCALL_CLONE_THREAD]    = sgx_ocall_clone_thread,
        [OCALL_CREATE_PROCESS]  = sgx_ocall_create_process,
        [OCALL_FUTEX]           = sgx_ocall_futex,
        [OCALL_SOCKETPAIR]      = sgx_ocall_socketpair,
        [OCALL_SOCK_LISTEN]     = sgx_ocall_sock_listen,
        [OCALL_SOCK_ACCEPT]     = sgx_ocall_sock_accept,
        [OCALL_SOCK_CONNECT]    = sgx_ocall_sock_connect,
        [OCALL_SOCK_RECV]       = sgx_ocall_sock_recv,
        [OCALL_SOCK_SEND]       = sgx_ocall_sock_send,
        [OCALL_SOCK_SETOPT]     = sgx_ocall_sock_setopt,
        [OCALL_SOCK_SHUTDOWN]   = sgx_ocall_sock_shutdown,
        [OCALL_GETTIME]         = sgx_ocall_gettime,
        [OCALL_SLEEP]           = sgx_ocall_sleep,
        [OCALL_POLL]            = sgx_ocall_poll,
        [OCALL_RENAME]          = sgx_ocall_rename,
        [OCALL_DELETE]          = sgx_ocall_delete,
        [OCALL_LOAD_DEBUG]      = sgx_ocall_load_debug,
        [OCALL_GET_ATTESTATION] = sgx_ocall_get_attestation,
        [OCALL_EVENTFD]         = sgx_ocall_eventfd,
        [OCALL_FCNTL]           = sgx_ocall_fcntl,
};

#define EDEBUG(code, ms) do {} while (0)

int ecall_enclave_start (char * args, size_t args_size, char * env, size_t env_size)
{
    ms_ecall_enclave_start_t ms;
    ms.ms_args = args;
    ms.ms_args_size = args_size;
    ms.ms_env = env;
    ms.ms_env_size = env_size;
    ms.ms_sec_info = &pal_enclave.pal_sec;
    EDEBUG(ECALL_ENCLAVE_START, &ms);
    return sgx_ecall(ECALL_ENCLAVE_START, &ms);
}

int ecall_thread_start (void)
{
    EDEBUG(ECALL_THREAD_START, NULL);
    return sgx_ecall(ECALL_THREAD_START, NULL);
}

int ecall_thread_reset(void) {
    EDEBUG(ECALL_THREAD_RESET, NULL);
    return sgx_ecall(ECALL_THREAD_RESET, NULL);
}

noreturn void __abort(void) {
    INLINE_SYSCALL(exit_group, 1, -1);
    while (true) {
        /* nothing */;
    }
}
