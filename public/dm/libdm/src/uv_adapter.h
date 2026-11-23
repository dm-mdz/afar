/* 由 tool/gen_dm_adapters.js 自动生成 —— 请勿手动修改。 */
/* 升级 libuv 后重新生成:  node tool/gen_dm_adapters.js */
#ifndef DM_UV_ADAPTER_H
#define DM_UV_ADAPTER_H

#include <dm.h>
/* 临时解除 dm/type.h 的 in/out/io 空宏，避免与 uv/win.h 的 struct 字段名冲突 */
#undef in
#undef out
#undef io
#include <uv.h>
#define in
#define out
#define io
#include <stddef.h>
#include <stdint.h>

#ifndef __cplusplus
_Static_assert(sizeof(dm_pid_t) == sizeof(uv_pid_t), "dm_pid_t must match uv_pid_t");
#endif

#ifdef __cplusplus
extern "C"
{
#endif

/* ---- 类型映射 uv_* -> dm_* ---- */
typedef uv_after_work_cb dm_after_work_cb;
typedef uv_alloc_cb dm_alloc_cb;
typedef uv_async_cb dm_async_cb;
typedef uv_async_t dm_async_st;
typedef uv_barrier_t dm_barrier_t;
typedef uv_buf_t dm_buf_st;
typedef uv_check_cb dm_check_cb;
typedef uv_check_t dm_check_st;
typedef uv_clock_id dm_clock_id_et;
typedef uv_close_cb dm_close_cb;
typedef uv_cond_t dm_cond_t;
typedef uv_connect_cb dm_connect_cb;
typedef uv_connect_t dm_connect_st;
typedef uv_connection_cb dm_connection_cb;
typedef uv_cpu_info_t dm_cpu_info_st;
typedef uv_dir_t dm_dir_st;
typedef uv_dirent_t dm_dirent_st;
typedef uv_exit_cb dm_exit_cb;
typedef uv_file dm_file;
typedef uv_fs_cb dm_fs_cb;
typedef uv_fs_event_cb dm_fs_event_cb;
typedef uv_fs_event_t dm_fs_event_st;
typedef uv_fs_poll_cb dm_fs_poll_cb;
typedef uv_fs_poll_t dm_fs_poll_st;
typedef uv_fs_t dm_fs_st;
typedef uv_fs_type dm_fs_type_et;
typedef uv_getaddrinfo_cb dm_getaddrinfo_cb;
typedef uv_getaddrinfo_t dm_getaddrinfo_st;
typedef uv_getnameinfo_cb dm_getnameinfo_cb;
typedef uv_getnameinfo_t dm_getnameinfo_st;
typedef uv_gid_t dm_gid_t;
typedef uv_group_t dm_group_st;
typedef uv_handle_t dm_handle_st;
typedef uv_handle_type dm_handle_type_et;
typedef uv_idle_cb dm_idle_cb;
typedef uv_idle_t dm_idle_st;
typedef uv_interface_address_t dm_interface_address_st;
typedef uv_key_t dm_key_t;
typedef uv_lib_t dm_lib_st;
typedef uv_loop_option dm_loop_option_et;
typedef uv_loop_t dm_loop_st;
typedef uv_membership dm_membership_et;
typedef uv_metrics_t dm_metrics_st;
typedef uv_mutex_t dm_mutex_t;
typedef uv_once_t dm_once_t;
typedef uv_os_fd_t dm_os_fd_t;
typedef uv_os_sock_t dm_os_sock_t;
typedef uv_passwd_t dm_passwd_st;
typedef uv_pipe_t dm_pipe_st;
typedef uv_poll_cb dm_poll_cb;
typedef uv_poll_t dm_poll_st;
typedef uv_prepare_cb dm_prepare_cb;
typedef uv_prepare_t dm_prepare_st;
typedef uv_process_options_t dm_process_options_st;
typedef uv_process_t dm_process_st;
typedef uv_random_cb dm_random_cb;
typedef uv_random_t dm_random_st;
typedef uv_read_cb dm_read_cb;
typedef uv_req_t dm_req_st;
typedef uv_req_type dm_req_type_et;
typedef uv_run_mode dm_run_mode_et;
typedef uv_rusage_t dm_rusage_st;
typedef uv_rwlock_t dm_rwlock_t;
typedef uv_sem_t dm_sem_t;
typedef uv_shutdown_cb dm_shutdown_cb;
typedef uv_shutdown_t dm_shutdown_st;
typedef uv_signal_cb dm_signal_cb;
typedef uv_signal_t dm_signal_st;
typedef uv_stat_t dm_stat_st;
typedef uv_statfs_t dm_statfs_st;
typedef uv_stdio_container_t dm_stdio_container_st;
typedef uv_stream_t dm_stream_st;
typedef uv_tcp_t dm_tcp_st;
typedef uv_thread_cb dm_thread_cb;
typedef uv_thread_options_t dm_thread_options_st;
typedef uv_thread_t dm_thread_t;
typedef uv_timer_cb dm_timer_cb;
typedef uv_timer_t dm_timer_st;
typedef uv_timespec64_t dm_timespec64_st;
typedef uv_timespec_t dm_timespec_st;
typedef uv_timeval64_t dm_timeval64_st;
typedef uv_timeval_t dm_timeval_st;
typedef uv_tty_mode_t dm_tty_mode_et;
typedef uv_tty_t dm_tty_st;
typedef uv_tty_vtermstate_t dm_tty_vtermstate_et;
typedef uv_udp_recv_cb dm_udp_recv_cb;
typedef uv_udp_send_cb dm_udp_send_cb;
typedef uv_udp_send_t dm_udp_send_st;
typedef uv_udp_t dm_udp_st;
typedef uv_uid_t dm_uid_t;
typedef uv_utsname_t dm_utsname_st;
typedef uv_walk_cb dm_walk_cb;
typedef uv_work_cb dm_work_cb;
typedef uv_work_t dm_work_st;
typedef uv_write_cb dm_write_cb;
typedef uv_write_t dm_write_st;
typedef uv_stdio_flags dm_stdio_flags;

/* ---- 常量:每个 DM_* 都是对应 UV_* 的别名 ---- */
#define DM_ASYNC UV_ASYNC
#define DM_CHANGE UV_CHANGE
#define DM_CHECK UV_CHECK
#define DM_CLOCK_MONOTONIC UV_CLOCK_MONOTONIC
#define DM_CLOCK_REALTIME UV_CLOCK_REALTIME
#define DM_CONNECT UV_CONNECT
#define DM_CREATE_PIPE UV_CREATE_PIPE
#define DM_DIRENT_BLOCK UV_DIRENT_BLOCK
#define DM_DIRENT_CHAR UV_DIRENT_CHAR
#define DM_DIRENT_DIR UV_DIRENT_DIR
#define DM_DIRENT_FIFO UV_DIRENT_FIFO
#define DM_DIRENT_FILE UV_DIRENT_FILE
#define DM_DIRENT_LINK UV_DIRENT_LINK
#define DM_DIRENT_SOCKET UV_DIRENT_SOCKET
#define DM_DIRENT_UNKNOWN UV_DIRENT_UNKNOWN
#define DM_DISCONNECT UV_DISCONNECT
#define DM_E2BIG UV_E2BIG
#define DM_EACCES UV_EACCES
#define DM_EADDRINUSE UV_EADDRINUSE
#define DM_EADDRNOTAVAIL UV_EADDRNOTAVAIL
#define DM_EAFNOSUPPORT UV_EAFNOSUPPORT
#define DM_EAGAIN UV_EAGAIN
#define DM_EAI_ADDRFAMILY UV_EAI_ADDRFAMILY
#define DM_EAI_AGAIN UV_EAI_AGAIN
#define DM_EAI_BADFLAGS UV_EAI_BADFLAGS
#define DM_EAI_BADHINTS UV_EAI_BADHINTS
#define DM_EAI_CANCELED UV_EAI_CANCELED
#define DM_EAI_FAIL UV_EAI_FAIL
#define DM_EAI_FAMILY UV_EAI_FAMILY
#define DM_EAI_MEMORY UV_EAI_MEMORY
#define DM_EAI_NODATA UV_EAI_NODATA
#define DM_EAI_NONAME UV_EAI_NONAME
#define DM_EAI_OVERFLOW UV_EAI_OVERFLOW
#define DM_EAI_PROTOCOL UV_EAI_PROTOCOL
#define DM_EAI_SERVICE UV_EAI_SERVICE
#define DM_EAI_SOCKTYPE UV_EAI_SOCKTYPE
#define DM_EALREADY UV_EALREADY
#define DM_EBADF UV_EBADF
#define DM_EBUSY UV_EBUSY
#define DM_ECANCELED UV_ECANCELED
#define DM_ECHARSET UV_ECHARSET
#define DM_ECONNABORTED UV_ECONNABORTED
#define DM_ECONNREFUSED UV_ECONNREFUSED
#define DM_ECONNRESET UV_ECONNRESET
#define DM_EDESTADDRREQ UV_EDESTADDRREQ
#define DM_EEXIST UV_EEXIST
#define DM_EFAULT UV_EFAULT
#define DM_EFBIG UV_EFBIG
#define DM_EFTYPE UV_EFTYPE
#define DM_EHOSTDOWN UV_EHOSTDOWN
#define DM_EHOSTUNREACH UV_EHOSTUNREACH
#define DM_EILSEQ UV_EILSEQ
#define DM_EINTR UV_EINTR
#define DM_EINVAL UV_EINVAL
#define DM_EIO UV_EIO
#define DM_EISCONN UV_EISCONN
#define DM_EISDIR UV_EISDIR
#define DM_ELOOP UV_ELOOP
#define DM_EMFILE UV_EMFILE
#define DM_EMLINK UV_EMLINK
#define DM_EMSGSIZE UV_EMSGSIZE
#define DM_ENAMETOOLONG UV_ENAMETOOLONG
#define DM_ENETDOWN UV_ENETDOWN
#define DM_ENETUNREACH UV_ENETUNREACH
#define DM_ENFILE UV_ENFILE
#define DM_ENOBUFS UV_ENOBUFS
#define DM_ENODATA UV_ENODATA
#define DM_ENODEV UV_ENODEV
#define DM_ENOENT UV_ENOENT
#define DM_ENOEXEC UV_ENOEXEC
#define DM_ENOMEM UV_ENOMEM
#define DM_ENONET UV_ENONET
#define DM_ENOPROTOOPT UV_ENOPROTOOPT
#define DM_ENOSPC UV_ENOSPC
#define DM_ENOSYS UV_ENOSYS
#define DM_ENOTCONN UV_ENOTCONN
#define DM_ENOTDIR UV_ENOTDIR
#define DM_ENOTEMPTY UV_ENOTEMPTY
#define DM_ENOTSOCK UV_ENOTSOCK
#define DM_ENOTSUP UV_ENOTSUP
#define DM_ENOTTY UV_ENOTTY
#define DM_ENXIO UV_ENXIO
#define DM_EOF UV_EOF
#define DM_EOVERFLOW UV_EOVERFLOW
#define DM_EPERM UV_EPERM
#define DM_EPIPE UV_EPIPE
#define DM_EPROTO UV_EPROTO
#define DM_EPROTONOSUPPORT UV_EPROTONOSUPPORT
#define DM_EPROTOTYPE UV_EPROTOTYPE
#define DM_ERANGE UV_ERANGE
#define DM_EREMOTEIO UV_EREMOTEIO
#define DM_EROFS UV_EROFS
#define DM_ERRNO_MAX UV_ERRNO_MAX
#define DM_ESHUTDOWN UV_ESHUTDOWN
#define DM_ESOCKTNOSUPPORT UV_ESOCKTNOSUPPORT
#define DM_ESPIPE UV_ESPIPE
#define DM_ESRCH UV_ESRCH
#define DM_ETIMEDOUT UV_ETIMEDOUT
#define DM_ETXTBSY UV_ETXTBSY
#define DM_EUNATCH UV_EUNATCH
#define DM_EXDEV UV_EXDEV
#define DM_FILE UV_FILE
#define DM_FS UV_FS
#define DM_FS_ACCESS UV_FS_ACCESS
#define DM_FS_CHMOD UV_FS_CHMOD
#define DM_FS_CHOWN UV_FS_CHOWN
#define DM_FS_CLOSE UV_FS_CLOSE
#define DM_FS_CLOSEDIR UV_FS_CLOSEDIR
#define DM_FS_COPYFILE UV_FS_COPYFILE
#define DM_FS_COPYFILE_EXCL UV_FS_COPYFILE_EXCL
#define DM_FS_COPYFILE_FICLONE UV_FS_COPYFILE_FICLONE
#define DM_FS_COPYFILE_FICLONE_FORCE UV_FS_COPYFILE_FICLONE_FORCE
#define DM_FS_CUSTOM UV_FS_CUSTOM
#define DM_FS_EVENT UV_FS_EVENT
#define DM_FS_EVENT_RECURSIVE UV_FS_EVENT_RECURSIVE
#define DM_FS_EVENT_STAT UV_FS_EVENT_STAT
#define DM_FS_EVENT_WATCH_ENTRY UV_FS_EVENT_WATCH_ENTRY
#define DM_FS_FCHMOD UV_FS_FCHMOD
#define DM_FS_FCHOWN UV_FS_FCHOWN
#define DM_FS_FDATASYNC UV_FS_FDATASYNC
#define DM_FS_FSTAT UV_FS_FSTAT
#define DM_FS_FSYNC UV_FS_FSYNC
#define DM_FS_FTRUNCATE UV_FS_FTRUNCATE
#define DM_FS_FUTIME UV_FS_FUTIME
#define DM_FS_LCHOWN UV_FS_LCHOWN
#define DM_FS_LINK UV_FS_LINK
#define DM_FS_LSTAT UV_FS_LSTAT
#define DM_FS_LUTIME UV_FS_LUTIME
#define DM_FS_MKDIR UV_FS_MKDIR
#define DM_FS_MKDTEMP UV_FS_MKDTEMP
#define DM_FS_MKSTEMP UV_FS_MKSTEMP
#define DM_FS_OPEN UV_FS_OPEN
#define DM_FS_OPENDIR UV_FS_OPENDIR
#define DM_FS_POLL UV_FS_POLL
#define DM_FS_READ UV_FS_READ
#define DM_FS_READDIR UV_FS_READDIR
#define DM_FS_READLINK UV_FS_READLINK
#define DM_FS_REALPATH UV_FS_REALPATH
#define DM_FS_RENAME UV_FS_RENAME
#define DM_FS_RMDIR UV_FS_RMDIR
#define DM_FS_SCANDIR UV_FS_SCANDIR
#define DM_FS_SENDFILE UV_FS_SENDFILE
#define DM_FS_STAT UV_FS_STAT
#define DM_FS_STATFS UV_FS_STATFS
#define DM_FS_SYMLINK UV_FS_SYMLINK
#define DM_FS_SYMLINK_DIR UV_FS_SYMLINK_DIR
#define DM_FS_SYMLINK_JUNCTION UV_FS_SYMLINK_JUNCTION
#define DM_FS_UNKNOWN UV_FS_UNKNOWN
#define DM_FS_UNLINK UV_FS_UNLINK
#define DM_FS_UTIME UV_FS_UTIME
#define DM_FS_UTIME_NOW UV_FS_UTIME_NOW
#define DM_FS_UTIME_OMIT UV_FS_UTIME_OMIT
#define DM_FS_WRITE UV_FS_WRITE
#define DM_GETADDRINFO UV_GETADDRINFO
#define DM_GETNAMEINFO UV_GETNAMEINFO
#define DM_HANDLE UV_HANDLE
#define DM_HANDLE_TYPE_MAX UV_HANDLE_TYPE_MAX
#define DM_IDLE UV_IDLE
#define DM_IGNORE UV_IGNORE
#define DM_INHERIT_FD UV_INHERIT_FD
#define DM_INHERIT_STREAM UV_INHERIT_STREAM
#define DM_JOIN_GROUP UV_JOIN_GROUP
#define DM_LEAVE_GROUP UV_LEAVE_GROUP
#define DM_LOOP_BLOCK_SIGNAL UV_LOOP_BLOCK_SIGNAL
#define DM_LOOP_USE_IO_URING_SQPOLL UV_LOOP_USE_IO_URING_SQPOLL
#define DM_METRICS_IDLE_TIME UV_METRICS_IDLE_TIME
#define DM_NAMED_PIPE UV_NAMED_PIPE
#define DM_NONBLOCK_PIPE UV_NONBLOCK_PIPE
#define DM_ONCE_INIT UV_ONCE_INIT
#define DM_OVERLAPPED_PIPE UV_OVERLAPPED_PIPE
#define DM_PIPE_NO_TRUNCATE UV_PIPE_NO_TRUNCATE
#define DM_POLL UV_POLL
#define DM_PREPARE UV_PREPARE
#define DM_PRIORITIZED UV_PRIORITIZED
#define DM_PROCESS UV_PROCESS
#define DM_PROCESS_DETACHED UV_PROCESS_DETACHED
#define DM_PROCESS_SETGID UV_PROCESS_SETGID
#define DM_PROCESS_SETUID UV_PROCESS_SETUID
#define DM_PROCESS_WINDOWS_FILE_PATH_EXACT_NAME UV_PROCESS_WINDOWS_FILE_PATH_EXACT_NAME
#define DM_PROCESS_WINDOWS_HIDE UV_PROCESS_WINDOWS_HIDE
#define DM_PROCESS_WINDOWS_HIDE_CONSOLE UV_PROCESS_WINDOWS_HIDE_CONSOLE
#define DM_PROCESS_WINDOWS_HIDE_GUI UV_PROCESS_WINDOWS_HIDE_GUI
#define DM_PROCESS_WINDOWS_VERBATIM_ARGUMENTS UV_PROCESS_WINDOWS_VERBATIM_ARGUMENTS
#define DM_RANDOM UV_RANDOM
#define DM_READABLE UV_READABLE
#define DM_READABLE_PIPE UV_READABLE_PIPE
#define DM_RENAME UV_RENAME
#define DM_REQ UV_REQ
#define DM_REQ_TYPE_MAX UV_REQ_TYPE_MAX
#define DM_REQ_TYPE_PRIVATE UV_REQ_TYPE_PRIVATE
#define DM_RUN_DEFAULT UV_RUN_DEFAULT
#define DM_RUN_NOWAIT UV_RUN_NOWAIT
#define DM_RUN_ONCE UV_RUN_ONCE
#define DM_SHUTDOWN UV_SHUTDOWN
#define DM_SIGNAL UV_SIGNAL
#define DM_STREAM UV_STREAM
#define DM_TCP UV_TCP
#define DM_TCP_IPV6ONLY UV_TCP_IPV6ONLY
#define DM_TCP_REUSEPORT UV_TCP_REUSEPORT
#define DM_THREAD_HAS_STACK_SIZE UV_THREAD_HAS_STACK_SIZE
#define DM_THREAD_NO_FLAGS UV_THREAD_NO_FLAGS
#define DM_THREAD_PRIORITY_ABOVE_NORMAL UV_THREAD_PRIORITY_ABOVE_NORMAL
#define DM_THREAD_PRIORITY_BELOW_NORMAL UV_THREAD_PRIORITY_BELOW_NORMAL
#define DM_THREAD_PRIORITY_HIGHEST UV_THREAD_PRIORITY_HIGHEST
#define DM_THREAD_PRIORITY_LOWEST UV_THREAD_PRIORITY_LOWEST
#define DM_THREAD_PRIORITY_NORMAL UV_THREAD_PRIORITY_NORMAL
#define DM_TIMER UV_TIMER
#define DM_TTY UV_TTY
#define DM_TTY_MODE_IO UV_TTY_MODE_IO
#define DM_TTY_MODE_NORMAL UV_TTY_MODE_NORMAL
#define DM_TTY_MODE_RAW UV_TTY_MODE_RAW
#define DM_TTY_MODE_RAW_VT UV_TTY_MODE_RAW_VT
#define DM_TTY_SUPPORTED UV_TTY_SUPPORTED
#define DM_TTY_UNSUPPORTED UV_TTY_UNSUPPORTED
#define DM_UDP UV_UDP
#define DM_UDP_IPV6ONLY UV_UDP_IPV6ONLY
#define DM_UDP_LINUX_RECVERR UV_UDP_LINUX_RECVERR
#define DM_UDP_MMSG_CHUNK UV_UDP_MMSG_CHUNK
#define DM_UDP_MMSG_FREE UV_UDP_MMSG_FREE
#define DM_UDP_PARTIAL UV_UDP_PARTIAL
#define DM_UDP_RECVMMSG UV_UDP_RECVMMSG
#define DM_UDP_REUSEADDR UV_UDP_REUSEADDR
#define DM_UDP_REUSEPORT UV_UDP_REUSEPORT
#define DM_UDP_SEND UV_UDP_SEND
#define DM_UNKNOWN UV_UNKNOWN
#define DM_UNKNOWN_HANDLE UV_UNKNOWN_HANDLE
#define DM_UNKNOWN_REQ UV_UNKNOWN_REQ
#define DM_WORK UV_WORK
#define DM_WRITABLE UV_WRITABLE
#define DM_WRITABLE_PIPE UV_WRITABLE_PIPE
#define DM_WRITE UV_WRITE
#define DM_FS_O_RDONLY UV_FS_O_RDONLY
#define DM_FS_O_WRONLY UV_FS_O_WRONLY
#define DM_FS_O_RDWR UV_FS_O_RDWR
#define DM_FS_O_CREAT UV_FS_O_CREAT
#define DM_FS_O_TRUNC UV_FS_O_TRUNC
#define DM_FS_O_EXCL UV_FS_O_EXCL
#define DM_FS_O_APPEND UV_FS_O_APPEND

/* ---- 函数适配:dm_<name> 封装 uv_<name> ---- */
/* 变参函数(C 无法前向传递 `...`)不在此生成,由 dm.c 手工适配:
 *   uv_loop_configure
 */
/* 以下函数由 dm.c 手工实现(生成器只保留其类型别名,不生成直通函数):
 *   uv_replace_allocator, uv_os_environ, uv_os_free_environ
 */
DM_API unsigned int dm_uv_version(void);
DM_API const char *dm_uv_version_string(void);
DM_API void dm_library_shutdown(void);
DM_API dm_loop_st *dm_default_loop(void);
DM_API int dm_loop_init(dm_loop_st *loop);
DM_API int dm_loop_close(dm_loop_st *loop);
DM_API dm_loop_st *dm_loop_new(void);
DM_API void dm_loop_delete(dm_loop_st *arg1);
DM_API size_t dm_loop_size(void);
DM_API int dm_loop_alive(const dm_loop_st *loop);
DM_API int dm_loop_fork(dm_loop_st *loop);
DM_API int dm_run(dm_loop_st *arg1, dm_run_mode_et mode);
DM_API void dm_stop(dm_loop_st *arg1);
DM_API void dm_ref(dm_handle_st *arg1);
DM_API void dm_unref(dm_handle_st *arg1);
DM_API int dm_has_ref(const dm_handle_st *arg1);
DM_API void dm_update_time(dm_loop_st *arg1);
DM_API uint64_t dm_now(const dm_loop_st *arg1);
DM_API int dm_backend_fd(const dm_loop_st *arg1);
DM_API int dm_backend_timeout(const dm_loop_st *arg1);
DM_API int dm_translate_sys_error(int sys_errno);
DM_API const char *dm_strerror(int err);
DM_API char *dm_strerror_r(int err, char *buf, size_t buflen);
DM_API const char *dm_err_name(int err);
DM_API char *dm_err_name_r(int err, char *buf, size_t buflen);
DM_API int dm_shutdown(dm_shutdown_st *req, dm_stream_st *handle, dm_shutdown_cb cb);
DM_API size_t dm_handle_size(dm_handle_type_et type);
DM_API dm_handle_type_et dm_handle_get_type(const dm_handle_st *handle);
DM_API const char *dm_handle_type_name(dm_handle_type_et type);
DM_API void *dm_handle_get_data(const dm_handle_st *handle);
DM_API dm_loop_st *dm_handle_get_loop(const dm_handle_st *handle);
DM_API void dm_handle_set_data(dm_handle_st *handle, void *data);
DM_API size_t dm_req_size(dm_req_type_et type);
DM_API void *dm_req_get_data(const dm_req_st *req);
DM_API void dm_req_set_data(dm_req_st *req, void *data);
DM_API dm_req_type_et dm_req_get_type(const dm_req_st *req);
DM_API const char *dm_req_type_name(dm_req_type_et type);
DM_API int dm_is_active(const dm_handle_st *handle);
DM_API void dm_walk(dm_loop_st *loop, dm_walk_cb walk_cb, void *arg);
DM_API void dm_print_all_handles(dm_loop_st *loop, FILE *stream);
DM_API void dm_print_active_handles(dm_loop_st *loop, FILE *stream);
DM_API void dm_close(dm_handle_st *handle, dm_close_cb close_cb);
DM_API int dm_send_buffer_size(dm_handle_st *handle, int *value);
DM_API int dm_recv_buffer_size(dm_handle_st *handle, int *value);
DM_API int dm_fileno(const dm_handle_st *handle, dm_os_fd_t *fd);
DM_API dm_buf_st dm_buf_init(char *base, unsigned int len);
DM_API int dm_pipe(dm_file fds[2], int read_flags, int write_flags);
DM_API int dm_socketpair(int type, int protocol, dm_os_sock_t socket_vector[2], int flags0,
                         int flags1);
DM_API size_t dm_stream_get_write_queue_size(const dm_stream_st *stream);
DM_API int dm_listen(dm_stream_st *stream, int backlog, dm_connection_cb cb);
DM_API int dm_accept(dm_stream_st *server, dm_stream_st *client);
DM_API int dm_read_start(dm_stream_st *arg1, dm_alloc_cb alloc_cb, dm_read_cb read_cb);
DM_API int dm_read_stop(dm_stream_st *arg1);
DM_API int dm_write(dm_write_st *req, dm_stream_st *handle, const dm_buf_st bufs[],
                    unsigned int nbufs, dm_write_cb cb);
DM_API int dm_write2(dm_write_st *req, dm_stream_st *handle, const dm_buf_st bufs[],
                     unsigned int nbufs, dm_stream_st *send_handle, dm_write_cb cb);
DM_API int dm_try_write(dm_stream_st *handle, const dm_buf_st bufs[], unsigned int nbufs);
DM_API int dm_try_write2(dm_stream_st *handle, const dm_buf_st bufs[], unsigned int nbufs,
                         dm_stream_st *send_handle);
DM_API size_t dm_write_nwritten(const dm_write_st *req);
DM_API int dm_is_readable(const dm_stream_st *handle);
DM_API int dm_is_writable(const dm_stream_st *handle);
DM_API int dm_stream_set_blocking(dm_stream_st *handle, int blocking);
DM_API int dm_is_closing(const dm_handle_st *handle);
DM_API int dm_tcp_init(dm_loop_st *arg1, dm_tcp_st *handle);
DM_API int dm_tcp_init_ex(dm_loop_st *arg1, dm_tcp_st *handle, unsigned int flags);
DM_API int dm_tcp_open(dm_tcp_st *handle, dm_os_sock_t sock);
DM_API int dm_tcp_nodelay(dm_tcp_st *handle, int enable);
DM_API int dm_tcp_keepalive(dm_tcp_st *handle, int enable, unsigned int delay);
DM_API int dm_tcp_keepalive_ex(dm_tcp_st *handle, int on, unsigned int idle, unsigned int intvl,
                               unsigned int cnt);
DM_API int dm_tcp_simultaneous_accepts(dm_tcp_st *handle, int enable);
DM_API int dm_tcp_bind(dm_tcp_st *handle, const struct sockaddr *addr, unsigned int flags);
DM_API int dm_tcp_getsockname(const dm_tcp_st *handle, struct sockaddr *name, int *namelen);
DM_API int dm_tcp_getpeername(const dm_tcp_st *handle, struct sockaddr *name, int *namelen);
DM_API int dm_tcp_close_reset(dm_tcp_st *handle, dm_close_cb close_cb);
DM_API int dm_tcp_connect(dm_connect_st *req, dm_tcp_st *handle, const struct sockaddr *addr,
                          dm_connect_cb cb);
DM_API int dm_udp_init(dm_loop_st *arg1, dm_udp_st *handle);
DM_API int dm_udp_init_ex(dm_loop_st *arg1, dm_udp_st *handle, unsigned int flags);
DM_API int dm_udp_open(dm_udp_st *handle, dm_os_sock_t sock);
DM_API int dm_udp_open_ex(dm_udp_st *handle, dm_os_sock_t sock, unsigned int flags);
DM_API int dm_udp_bind(dm_udp_st *handle, const struct sockaddr *addr, unsigned int flags);
DM_API int dm_udp_connect(dm_udp_st *handle, const struct sockaddr *addr);
DM_API int dm_udp_getpeername(const dm_udp_st *handle, struct sockaddr *name, int *namelen);
DM_API int dm_udp_getsockname(const dm_udp_st *handle, struct sockaddr *name, int *namelen);
DM_API int dm_udp_set_membership(dm_udp_st *handle, const char *multicast_addr,
                                 const char *interface_addr, dm_membership_et membership);
DM_API int dm_udp_set_source_membership(dm_udp_st *handle, const char *multicast_addr,
                                        const char *interface_addr, const char *source_addr,
                                        dm_membership_et membership);
DM_API int dm_udp_set_multicast_loop(dm_udp_st *handle, int on);
DM_API int dm_udp_set_multicast_ttl(dm_udp_st *handle, int ttl);
DM_API int dm_udp_set_multicast_interface(dm_udp_st *handle, const char *interface_addr);
DM_API int dm_udp_set_broadcast(dm_udp_st *handle, int on);
DM_API int dm_udp_set_ttl(dm_udp_st *handle, int ttl);
DM_API int dm_udp_send(dm_udp_send_st *req, dm_udp_st *handle, const dm_buf_st bufs[],
                       unsigned int nbufs, const struct sockaddr *addr, dm_udp_send_cb send_cb);
DM_API int dm_udp_try_send(dm_udp_st *handle, const dm_buf_st bufs[], unsigned int nbufs,
                           const struct sockaddr *addr);
DM_API int dm_udp_try_send2(dm_udp_st *handle, unsigned int count, dm_buf_st *bufs[],
                            unsigned int nbufs[], struct sockaddr *addrs[], unsigned int flags);
DM_API int dm_udp_recv_start(dm_udp_st *handle, dm_alloc_cb alloc_cb, dm_udp_recv_cb recv_cb);
DM_API int dm_udp_using_recvmmsg(const dm_udp_st *handle);
DM_API int dm_udp_recv_stop(dm_udp_st *handle);
DM_API size_t dm_udp_get_send_queue_size(const dm_udp_st *handle);
DM_API size_t dm_udp_get_send_queue_count(const dm_udp_st *handle);
DM_API int dm_tty_init(dm_loop_st *arg1, dm_tty_st *arg2, dm_file fd, int readable);
DM_API int dm_tty_set_mode(dm_tty_st *arg1, dm_tty_mode_et mode);
DM_API int dm_tty_reset_mode(void);
DM_API int dm_tty_get_winsize(dm_tty_st *arg1, int *width, int *height);
DM_API void dm_tty_set_vterm_state(dm_tty_vtermstate_et state);
DM_API int dm_tty_get_vterm_state(dm_tty_vtermstate_et *state);
DM_API dm_handle_type_et dm_guess_handle(dm_file file);
DM_API int dm_pipe_init(dm_loop_st *arg1, dm_pipe_st *handle, int ipc);
DM_API int dm_pipe_open(dm_pipe_st *arg1, dm_file file);
DM_API int dm_pipe_bind(dm_pipe_st *handle, const char *name);
DM_API int dm_pipe_bind2(dm_pipe_st *handle, const char *name, size_t namelen, unsigned int flags);
DM_API void dm_pipe_connect(dm_connect_st *req, dm_pipe_st *handle, const char *name,
                            dm_connect_cb cb);
DM_API int dm_pipe_connect2(dm_connect_st *req, dm_pipe_st *handle, const char *name,
                            size_t namelen, unsigned int flags, dm_connect_cb cb);
DM_API int dm_pipe_getsockname(const dm_pipe_st *handle, char *buffer, size_t *size);
DM_API int dm_pipe_getpeername(const dm_pipe_st *handle, char *buffer, size_t *size);
DM_API void dm_pipe_pending_instances(dm_pipe_st *handle, int count);
DM_API int dm_pipe_pending_count(dm_pipe_st *handle);
DM_API dm_handle_type_et dm_pipe_pending_type(dm_pipe_st *handle);
DM_API int dm_pipe_chmod(dm_pipe_st *handle, int flags);
DM_API int dm_poll_init(dm_loop_st *loop, dm_poll_st *handle, int fd);
DM_API int dm_poll_init_socket(dm_loop_st *loop, dm_poll_st *handle, dm_os_sock_t socket);
DM_API int dm_poll_start(dm_poll_st *handle, int events, dm_poll_cb cb);
DM_API int dm_poll_stop(dm_poll_st *handle);
DM_API int dm_prepare_init(dm_loop_st *arg1, dm_prepare_st *prepare);
DM_API int dm_prepare_start(dm_prepare_st *prepare, dm_prepare_cb cb);
DM_API int dm_prepare_stop(dm_prepare_st *prepare);
DM_API int dm_check_init(dm_loop_st *arg1, dm_check_st *check);
DM_API int dm_check_start(dm_check_st *check, dm_check_cb cb);
DM_API int dm_check_stop(dm_check_st *check);
DM_API int dm_idle_init(dm_loop_st *arg1, dm_idle_st *idle);
DM_API int dm_idle_start(dm_idle_st *idle, dm_idle_cb cb);
DM_API int dm_idle_stop(dm_idle_st *idle);
DM_API int dm_async_init(dm_loop_st *arg1, dm_async_st *async, dm_async_cb async_cb);
DM_API int dm_async_send(dm_async_st *async);
DM_API int dm_timer_init(dm_loop_st *arg1, dm_timer_st *handle);
DM_API int dm_timer_start(dm_timer_st *handle, dm_timer_cb cb, uint64_t timeout, uint64_t repeat);
DM_API int dm_timer_stop(dm_timer_st *handle);
DM_API int dm_timer_again(dm_timer_st *handle);
DM_API void dm_timer_set_repeat(dm_timer_st *handle, uint64_t repeat);
DM_API uint64_t dm_timer_get_repeat(const dm_timer_st *handle);
DM_API uint64_t dm_timer_get_due_in(const dm_timer_st *handle);
DM_API int dm_getaddrinfo(dm_loop_st *loop, dm_getaddrinfo_st *req,
                          dm_getaddrinfo_cb getaddrinfo_cb, const char *node, const char *service,
                          const struct addrinfo *hints);
DM_API void dm_freeaddrinfo(struct addrinfo *ai);
DM_API int dm_getnameinfo(dm_loop_st *loop, dm_getnameinfo_st *req,
                          dm_getnameinfo_cb getnameinfo_cb, const struct sockaddr *addr, int flags);
DM_API int dm_spawn(dm_loop_st *loop, dm_process_st *handle, const dm_process_options_st *options);
DM_API int dm_process_kill(dm_process_st *arg1, int signum);
DM_API int dm_kill(int pid, int signum);
DM_API dm_pid_t dm_process_get_pid(const dm_process_st *arg1);
DM_API int dm_queue_work(dm_loop_st *loop, dm_work_st *req, dm_work_cb work_cb,
                         dm_after_work_cb after_work_cb);
DM_API int dm_cancel(dm_req_st *req);
DM_API char **dm_setup_args(int argc, char **argv);
DM_API int dm_get_process_title(char *buffer, size_t size);
DM_API int dm_set_process_title(const char *title);
DM_API int dm_resident_set_memory(size_t *rss);
DM_API int dm_uptime(double *uptime);
DM_API dm_os_fd_t dm_get_osfhandle(int fd);
DM_API int dm_open_osfhandle(dm_os_fd_t os_fd);
DM_API int dm_getrusage(dm_rusage_st *rusage);
DM_API int dm_getrusage_thread(dm_rusage_st *rusage);
DM_API int dm_os_homedir(char *buffer, size_t *size);
DM_API int dm_os_tmpdir(char *buffer, size_t *size);
DM_API int dm_os_get_passwd(dm_passwd_st *pwd);
DM_API void dm_os_free_passwd(dm_passwd_st *pwd);
DM_API int dm_os_get_passwd2(dm_passwd_st *pwd, dm_uid_t uid);
DM_API int dm_os_get_group(dm_group_st *grp, dm_uid_t gid);
DM_API void dm_os_free_group(dm_group_st *grp);
DM_API dm_pid_t dm_os_getpid(void);
DM_API dm_pid_t dm_os_getppid(void);
DM_API int dm_os_getpriority(dm_pid_t pid, int *priority);
DM_API int dm_os_setpriority(dm_pid_t pid, int priority);
DM_API int dm_thread_getpriority(dm_thread_t tid, int *priority);
DM_API int dm_thread_setpriority(dm_thread_t tid, int priority);
DM_API unsigned int dm_available_parallelism(void);
DM_API int dm_cpu_info(dm_cpu_info_st **cpu_infos, int *count);
DM_API void dm_free_cpu_info(dm_cpu_info_st *cpu_infos, int count);
DM_API int dm_cpumask_size(void);
DM_API int dm_interface_addresses(dm_interface_address_st **addresses, int *count);
DM_API void dm_free_interface_addresses(dm_interface_address_st *addresses, int count);
DM_API int dm_os_getenv(const char *name, char *buffer, size_t *size);
DM_API int dm_os_setenv(const char *name, const char *value);
DM_API int dm_os_unsetenv(const char *name);
DM_API int dm_os_gethostname(char *buffer, size_t *size);
DM_API int dm_os_uname(dm_utsname_st *buffer);
DM_API int dm_metrics_info(dm_loop_st *loop, dm_metrics_st *metrics);
DM_API uint64_t dm_metrics_idle_time(dm_loop_st *loop);
DM_API dm_fs_type_et dm_fs_get_type(const dm_fs_st *arg1);
DM_API ssize_t dm_fs_get_result(const dm_fs_st *arg1);
DM_API int dm_fs_get_system_error(const dm_fs_st *arg1);
DM_API void *dm_fs_get_ptr(const dm_fs_st *arg1);
DM_API const char *dm_fs_get_path(const dm_fs_st *arg1);
DM_API dm_stat_st *dm_fs_get_statbuf(dm_fs_st *arg1);
DM_API void dm_fs_req_cleanup(dm_fs_st *req);
DM_API int dm_fs_close(dm_loop_st *loop, dm_fs_st *req, dm_file file, dm_fs_cb cb);
DM_API int dm_fs_open(dm_loop_st *loop, dm_fs_st *req, const char *path, int flags, int mode,
                      dm_fs_cb cb);
DM_API int dm_fs_read(dm_loop_st *loop, dm_fs_st *req, dm_file file, const dm_buf_st bufs[],
                      unsigned int nbufs, int64_t offset, dm_fs_cb cb);
DM_API int dm_fs_unlink(dm_loop_st *loop, dm_fs_st *req, const char *path, dm_fs_cb cb);
DM_API int dm_fs_write(dm_loop_st *loop, dm_fs_st *req, dm_file file, const dm_buf_st bufs[],
                       unsigned int nbufs, int64_t offset, dm_fs_cb cb);
DM_API int dm_fs_copyfile(dm_loop_st *loop, dm_fs_st *req, const char *path, const char *new_path,
                          int flags, dm_fs_cb cb);
DM_API int dm_fs_mkdir(dm_loop_st *loop, dm_fs_st *req, const char *path, int mode, dm_fs_cb cb);
DM_API int dm_fs_mkdtemp(dm_loop_st *loop, dm_fs_st *req, const char *tpl, dm_fs_cb cb);
DM_API int dm_fs_mkstemp(dm_loop_st *loop, dm_fs_st *req, const char *tpl, dm_fs_cb cb);
DM_API int dm_fs_rmdir(dm_loop_st *loop, dm_fs_st *req, const char *path, dm_fs_cb cb);
DM_API int dm_fs_scandir(dm_loop_st *loop, dm_fs_st *req, const char *path, int flags, dm_fs_cb cb);
DM_API int dm_fs_scandir_next(dm_fs_st *req, dm_dirent_st *ent);
DM_API int dm_fs_opendir(dm_loop_st *loop, dm_fs_st *req, const char *path, dm_fs_cb cb);
DM_API int dm_fs_readdir(dm_loop_st *loop, dm_fs_st *req, dm_dir_st *dir, dm_fs_cb cb);
DM_API int dm_fs_closedir(dm_loop_st *loop, dm_fs_st *req, dm_dir_st *dir, dm_fs_cb cb);
DM_API int dm_fs_stat(dm_loop_st *loop, dm_fs_st *req, const char *path, dm_fs_cb cb);
DM_API int dm_fs_fstat(dm_loop_st *loop, dm_fs_st *req, dm_file file, dm_fs_cb cb);
DM_API int dm_fs_rename(dm_loop_st *loop, dm_fs_st *req, const char *path, const char *new_path,
                        dm_fs_cb cb);
DM_API int dm_fs_fsync(dm_loop_st *loop, dm_fs_st *req, dm_file file, dm_fs_cb cb);
DM_API int dm_fs_fdatasync(dm_loop_st *loop, dm_fs_st *req, dm_file file, dm_fs_cb cb);
DM_API int dm_fs_ftruncate(dm_loop_st *loop, dm_fs_st *req, dm_file file, int64_t offset,
                           dm_fs_cb cb);
DM_API int dm_fs_sendfile(dm_loop_st *loop, dm_fs_st *req, dm_file out_fd, dm_file in_fd,
                          int64_t in_offset, size_t length, dm_fs_cb cb);
DM_API int dm_fs_access(dm_loop_st *loop, dm_fs_st *req, const char *path, int mode, dm_fs_cb cb);
DM_API int dm_fs_chmod(dm_loop_st *loop, dm_fs_st *req, const char *path, int mode, dm_fs_cb cb);
DM_API int dm_fs_utime(dm_loop_st *loop, dm_fs_st *req, const char *path, double atime,
                       double mtime, dm_fs_cb cb);
DM_API int dm_fs_futime(dm_loop_st *loop, dm_fs_st *req, dm_file file, double atime, double mtime,
                        dm_fs_cb cb);
DM_API int dm_fs_lutime(dm_loop_st *loop, dm_fs_st *req, const char *path, double atime,
                        double mtime, dm_fs_cb cb);
DM_API int dm_fs_lstat(dm_loop_st *loop, dm_fs_st *req, const char *path, dm_fs_cb cb);
DM_API int dm_fs_link(dm_loop_st *loop, dm_fs_st *req, const char *path, const char *new_path,
                      dm_fs_cb cb);
DM_API int dm_fs_symlink(dm_loop_st *loop, dm_fs_st *req, const char *path, const char *new_path,
                         int flags, dm_fs_cb cb);
DM_API int dm_fs_readlink(dm_loop_st *loop, dm_fs_st *req, const char *path, dm_fs_cb cb);
DM_API int dm_fs_realpath(dm_loop_st *loop, dm_fs_st *req, const char *path, dm_fs_cb cb);
DM_API int dm_fs_fchmod(dm_loop_st *loop, dm_fs_st *req, dm_file file, int mode, dm_fs_cb cb);
DM_API int dm_fs_chown(dm_loop_st *loop, dm_fs_st *req, const char *path, dm_uid_t uid,
                       dm_gid_t gid, dm_fs_cb cb);
DM_API int dm_fs_fchown(dm_loop_st *loop, dm_fs_st *req, dm_file file, dm_uid_t uid, dm_gid_t gid,
                        dm_fs_cb cb);
DM_API int dm_fs_lchown(dm_loop_st *loop, dm_fs_st *req, const char *path, dm_uid_t uid,
                        dm_gid_t gid, dm_fs_cb cb);
DM_API int dm_fs_statfs(dm_loop_st *loop, dm_fs_st *req, const char *path, dm_fs_cb cb);
DM_API int dm_fs_poll_init(dm_loop_st *loop, dm_fs_poll_st *handle);
DM_API int dm_fs_poll_start(dm_fs_poll_st *handle, dm_fs_poll_cb poll_cb, const char *path,
                            unsigned int interval);
DM_API int dm_fs_poll_stop(dm_fs_poll_st *handle);
DM_API int dm_fs_poll_getpath(dm_fs_poll_st *handle, char *buffer, size_t *size);
DM_API int dm_signal_init(dm_loop_st *loop, dm_signal_st *handle);
DM_API int dm_signal_start(dm_signal_st *handle, dm_signal_cb signal_cb, int signum);
DM_API int dm_signal_start_oneshot(dm_signal_st *handle, dm_signal_cb signal_cb, int signum);
DM_API int dm_signal_stop(dm_signal_st *handle);
DM_API void dm_loadavg(double avg[3]);
DM_API int dm_fs_event_init(dm_loop_st *loop, dm_fs_event_st *handle);
DM_API int dm_fs_event_start(dm_fs_event_st *handle, dm_fs_event_cb cb, const char *path,
                             unsigned int flags);
DM_API int dm_fs_event_stop(dm_fs_event_st *handle);
DM_API int dm_fs_event_getpath(dm_fs_event_st *handle, char *buffer, size_t *size);
DM_API int dm_ip4_addr(const char *ip, int port, struct sockaddr_in *addr);
DM_API int dm_ip6_addr(const char *ip, int port, struct sockaddr_in6 *addr);
DM_API int dm_ip4_name(const struct sockaddr_in *src, char *dst, size_t size);
DM_API int dm_ip6_name(const struct sockaddr_in6 *src, char *dst, size_t size);
DM_API int dm_ip_name(const struct sockaddr *src, char *dst, size_t size);
DM_API int dm_inet_ntop(int af, const void *src, char *dst, size_t size);
DM_API int dm_inet_pton(int af, const char *src, void *dst);
DM_API int dm_random(dm_loop_st *loop, dm_random_st *req, void *buf, size_t buflen, unsigned flags,
                     dm_random_cb cb);
DM_API int dm_if_indextoname(unsigned int ifindex, char *buffer, size_t *size);
DM_API int dm_if_indextoiid(unsigned int ifindex, char *buffer, size_t *size);
DM_API int dm_exepath(char *buffer, size_t *size);
DM_API int dm_cwd(char *buffer, size_t *size);
DM_API int dm_chdir(const char *dir);
DM_API uint64_t dm_get_free_memory(void);
DM_API uint64_t dm_get_total_memory(void);
DM_API uint64_t dm_get_constrained_memory(void);
DM_API uint64_t dm_get_available_memory(void);
DM_API int dm_clock_gettime(dm_clock_id_et clock_id, dm_timespec64_st *ts);
DM_API uint64_t dm_hrtime(void);
DM_API void dm_sleep(unsigned int msec);
DM_API void dm_disable_stdio_inheritance(void);
DM_API int dm_dlopen(const char *filename, dm_lib_st *lib);
DM_API void dm_dlclose(dm_lib_st *lib);
DM_API int dm_dlsym(dm_lib_st *lib, const char *name, void **ptr);
DM_API const char *dm_dlerror(const dm_lib_st *lib);
DM_API int dm_mutex_init(dm_mutex_t *handle);
DM_API int dm_mutex_init_recursive(dm_mutex_t *handle);
DM_API void dm_mutex_destroy(dm_mutex_t *handle);
DM_API void dm_mutex_lock(dm_mutex_t *handle);
DM_API int dm_mutex_trylock(dm_mutex_t *handle);
DM_API void dm_mutex_unlock(dm_mutex_t *handle);
DM_API int dm_rwlock_init(dm_rwlock_t *rwlock);
DM_API void dm_rwlock_destroy(dm_rwlock_t *rwlock);
DM_API void dm_rwlock_rdlock(dm_rwlock_t *rwlock);
DM_API int dm_rwlock_tryrdlock(dm_rwlock_t *rwlock);
DM_API void dm_rwlock_rdunlock(dm_rwlock_t *rwlock);
DM_API void dm_rwlock_wrlock(dm_rwlock_t *rwlock);
DM_API int dm_rwlock_trywrlock(dm_rwlock_t *rwlock);
DM_API void dm_rwlock_wrunlock(dm_rwlock_t *rwlock);
DM_API int dm_sem_init(dm_sem_t *sem, unsigned int value);
DM_API void dm_sem_destroy(dm_sem_t *sem);
DM_API void dm_sem_post(dm_sem_t *sem);
DM_API void dm_sem_wait(dm_sem_t *sem);
DM_API int dm_sem_trywait(dm_sem_t *sem);
DM_API int dm_cond_init(dm_cond_t *cond);
DM_API void dm_cond_destroy(dm_cond_t *cond);
DM_API void dm_cond_signal(dm_cond_t *cond);
DM_API void dm_cond_broadcast(dm_cond_t *cond);
DM_API int dm_barrier_init(dm_barrier_t *barrier, unsigned int count);
DM_API void dm_barrier_destroy(dm_barrier_t *barrier);
DM_API int dm_barrier_wait(dm_barrier_t *barrier);
DM_API void dm_cond_wait(dm_cond_t *cond, dm_mutex_t *mutex);
DM_API int dm_cond_timedwait(dm_cond_t *cond, dm_mutex_t *mutex, uint64_t timeout);
DM_API void dm_once(dm_once_t *guard, void (*callback)(void));
DM_API int dm_key_create(dm_key_t *key);
DM_API void dm_key_delete(dm_key_t *key);
DM_API void *dm_key_get(dm_key_t *key);
DM_API void dm_key_set(dm_key_t *key, void *value);
DM_API int dm_gettimeofday(dm_timeval64_st *tv);
DM_API int dm_thread_create(dm_thread_t *tid, dm_thread_cb entry, void *arg);
DM_API int dm_thread_detach(dm_thread_t *tid);
DM_API int dm_thread_create_ex(dm_thread_t *tid, const dm_thread_options_st *params,
                               dm_thread_cb entry, void *arg);
DM_API int dm_thread_setaffinity(dm_thread_t *tid, char *cpumask, char *oldmask, size_t mask_size);
DM_API int dm_thread_getaffinity(dm_thread_t *tid, char *cpumask, size_t mask_size);
DM_API int dm_thread_getcpu(void);
DM_API dm_thread_t dm_thread_self(void);
DM_API int dm_thread_join(dm_thread_t *tid);
DM_API int dm_thread_equal(const dm_thread_t *t1, const dm_thread_t *t2);
DM_API int dm_thread_setname(const char *name);
DM_API int dm_thread_getname(dm_thread_t *tid, char *name, size_t size);
DM_API void *dm_loop_get_data(const dm_loop_st *arg1);
DM_API void dm_loop_set_data(dm_loop_st *arg1, void *data);
DM_API size_t dm_utf16_length_as_wtf8(const uint16_t *utf16, ssize_t utf16_len);
DM_API int dm_utf16_to_wtf8(const uint16_t *utf16, ssize_t utf16_len, char **wtf8_ptr,
                            size_t *wtf8_len_ptr);
DM_API ssize_t dm_wtf8_length_as_utf16(const char *wtf8);
DM_API void dm_wtf8_to_utf16(const char *wtf8, uint16_t *utf16, size_t utf16_len);
DM_API int dm_loop_configure(dm_loop_st *loop, dm_loop_option_et option, ...);

#ifdef __cplusplus
}
#endif

#endif /* DM_UV_ADAPTER_H */
