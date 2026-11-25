/* 由 tool/gen_dm_adapters.js 自动生成 —— 请勿手动修改。 */
/* 升级 libuv 后重新生成:  node tool/gen_dm_adapters.js */
#ifndef DM_UV_ADAPTER_H
#define DM_UV_ADAPTER_H

#include <dm.h>
#include <uv.h>
#include <stddef.h>
#include <stdint.h>

#ifndef __cplusplus
_Static_assert(sizeof(dm_pid_t) == sizeof(uv_pid_t), "dm_pid_t must match uv_pid_t");
#endif

#ifdef __cplusplus
extern "C"
{
#endif

/* ---- 类型映射 uv_* -> dm_uv_* ---- */
typedef uv_after_work_cb dm_uv_after_work_cb;
typedef uv_alloc_cb dm_uv_alloc_cb;
typedef uv_async_cb dm_uv_async_cb;
typedef uv_async_t dm_uv_async_st;
typedef uv_barrier_t dm_uv_barrier_t;
typedef uv_buf_t dm_uv_buf_st;
typedef uv_calloc_func dm_uv_calloc_func;
typedef uv_check_cb dm_uv_check_cb;
typedef uv_check_t dm_uv_check_st;
typedef uv_clock_id dm_uv_clock_id_et;
typedef uv_close_cb dm_uv_close_cb;
typedef uv_cond_t dm_uv_cond_t;
typedef uv_connect_cb dm_uv_connect_cb;
typedef uv_connect_t dm_uv_connect_st;
typedef uv_connection_cb dm_uv_connection_cb;
typedef uv_cpu_info_t dm_uv_cpu_info_st;
typedef uv_dir_t dm_uv_dir_st;
typedef uv_dirent_t dm_uv_dirent_st;
typedef uv_env_item_t dm_uv_env_item_st;
typedef uv_exit_cb dm_uv_exit_cb;
typedef uv_file dm_uv_file;
typedef uv_free_func dm_uv_free_func;
typedef uv_fs_cb dm_uv_fs_cb;
typedef uv_fs_event_cb dm_uv_fs_event_cb;
typedef uv_fs_event_t dm_uv_fs_event_st;
typedef uv_fs_poll_cb dm_uv_fs_poll_cb;
typedef uv_fs_poll_t dm_uv_fs_poll_st;
typedef uv_fs_t dm_uv_fs_st;
typedef uv_fs_type dm_uv_fs_type_et;
typedef uv_getaddrinfo_cb dm_uv_getaddrinfo_cb;
typedef uv_getaddrinfo_t dm_uv_getaddrinfo_st;
typedef uv_getnameinfo_cb dm_uv_getnameinfo_cb;
typedef uv_getnameinfo_t dm_uv_getnameinfo_st;
typedef uv_gid_t dm_uv_gid_t;
typedef uv_group_t dm_uv_group_st;
typedef uv_handle_t dm_uv_handle_st;
typedef uv_handle_type dm_uv_handle_type_et;
typedef uv_idle_cb dm_uv_idle_cb;
typedef uv_idle_t dm_uv_idle_st;
typedef uv_interface_address_t dm_uv_interface_address_st;
typedef uv_key_t dm_uv_key_t;
typedef uv_lib_t dm_uv_lib_st;
typedef uv_loop_option dm_uv_loop_option_et;
typedef uv_loop_t dm_uv_loop_st;
typedef uv_malloc_func dm_uv_malloc_func;
typedef uv_membership dm_uv_membership_et;
typedef uv_metrics_t dm_uv_metrics_st;
typedef uv_mutex_t dm_uv_mutex_t;
typedef uv_once_t dm_uv_once_t;
typedef uv_os_fd_t dm_uv_os_fd_t;
typedef uv_os_sock_t dm_uv_os_sock_t;
typedef uv_passwd_t dm_uv_passwd_st;
typedef uv_pid_t dm_uv_pid_t;
typedef uv_pipe_t dm_uv_pipe_st;
typedef uv_poll_cb dm_uv_poll_cb;
typedef uv_poll_t dm_uv_poll_st;
typedef uv_prepare_cb dm_uv_prepare_cb;
typedef uv_prepare_t dm_uv_prepare_st;
typedef uv_process_options_t dm_uv_process_options_st;
typedef uv_process_t dm_uv_process_st;
typedef uv_random_cb dm_uv_random_cb;
typedef uv_random_t dm_uv_random_st;
typedef uv_read_cb dm_uv_read_cb;
typedef uv_realloc_func dm_uv_realloc_func;
typedef uv_req_t dm_uv_req_st;
typedef uv_req_type dm_uv_req_type_et;
typedef uv_run_mode dm_uv_run_mode_et;
typedef uv_rusage_t dm_uv_rusage_st;
typedef uv_rwlock_t dm_uv_rwlock_t;
typedef uv_sem_t dm_uv_sem_t;
typedef uv_shutdown_cb dm_uv_shutdown_cb;
typedef uv_shutdown_t dm_uv_shutdown_st;
typedef uv_signal_cb dm_uv_signal_cb;
typedef uv_signal_t dm_uv_signal_st;
typedef uv_stat_t dm_uv_stat_st;
typedef uv_statfs_t dm_uv_statfs_st;
typedef uv_stdio_container_t dm_uv_stdio_container_st;
typedef uv_stream_t dm_uv_stream_st;
typedef uv_tcp_t dm_uv_tcp_st;
typedef uv_thread_cb dm_uv_thread_cb;
typedef uv_thread_options_t dm_uv_thread_options_st;
typedef uv_thread_t dm_uv_thread_t;
typedef uv_timer_cb dm_uv_timer_cb;
typedef uv_timer_t dm_uv_timer_st;
typedef uv_timespec64_t dm_uv_timespec64_st;
typedef uv_timespec_t dm_uv_timespec_st;
typedef uv_timeval64_t dm_uv_timeval64_st;
typedef uv_timeval_t dm_uv_timeval_st;
typedef uv_tty_mode_t dm_uv_tty_mode_et;
typedef uv_tty_t dm_uv_tty_st;
typedef uv_tty_vtermstate_t dm_uv_tty_vtermstate_et;
typedef uv_udp_recv_cb dm_uv_udp_recv_cb;
typedef uv_udp_send_cb dm_uv_udp_send_cb;
typedef uv_udp_send_t dm_uv_udp_send_st;
typedef uv_udp_t dm_uv_udp_st;
typedef uv_uid_t dm_uv_uid_t;
typedef uv_utsname_t dm_uv_utsname_st;
typedef uv_walk_cb dm_uv_walk_cb;
typedef uv_work_cb dm_uv_work_cb;
typedef uv_work_t dm_uv_work_st;
typedef uv_write_cb dm_uv_write_cb;
typedef uv_write_t dm_uv_write_st;
typedef uv_stdio_flags dm_uv_stdio_flags;

/* ---- 常量:每个 DM_UV_* 都是对应 UV_* 的别名 ---- */
#define DM_UV_ASYNC UV_ASYNC
#define DM_UV_CHANGE UV_CHANGE
#define DM_UV_CHECK UV_CHECK
#define DM_UV_CLOCK_MONOTONIC UV_CLOCK_MONOTONIC
#define DM_UV_CLOCK_REALTIME UV_CLOCK_REALTIME
#define DM_UV_CONNECT UV_CONNECT
#define DM_UV_CREATE_PIPE UV_CREATE_PIPE
#define DM_UV_DIRENT_BLOCK UV_DIRENT_BLOCK
#define DM_UV_DIRENT_CHAR UV_DIRENT_CHAR
#define DM_UV_DIRENT_DIR UV_DIRENT_DIR
#define DM_UV_DIRENT_FIFO UV_DIRENT_FIFO
#define DM_UV_DIRENT_FILE UV_DIRENT_FILE
#define DM_UV_DIRENT_LINK UV_DIRENT_LINK
#define DM_UV_DIRENT_SOCKET UV_DIRENT_SOCKET
#define DM_UV_DIRENT_UNKNOWN UV_DIRENT_UNKNOWN
#define DM_UV_DISCONNECT UV_DISCONNECT
#define DM_UV_E2BIG UV_E2BIG
#define DM_UV_EACCES UV_EACCES
#define DM_UV_EADDRINUSE UV_EADDRINUSE
#define DM_UV_EADDRNOTAVAIL UV_EADDRNOTAVAIL
#define DM_UV_EAFNOSUPPORT UV_EAFNOSUPPORT
#define DM_UV_EAGAIN UV_EAGAIN
#define DM_UV_EAI_ADDRFAMILY UV_EAI_ADDRFAMILY
#define DM_UV_EAI_AGAIN UV_EAI_AGAIN
#define DM_UV_EAI_BADFLAGS UV_EAI_BADFLAGS
#define DM_UV_EAI_BADHINTS UV_EAI_BADHINTS
#define DM_UV_EAI_CANCELED UV_EAI_CANCELED
#define DM_UV_EAI_FAIL UV_EAI_FAIL
#define DM_UV_EAI_FAMILY UV_EAI_FAMILY
#define DM_UV_EAI_MEMORY UV_EAI_MEMORY
#define DM_UV_EAI_NODATA UV_EAI_NODATA
#define DM_UV_EAI_NONAME UV_EAI_NONAME
#define DM_UV_EAI_OVERFLOW UV_EAI_OVERFLOW
#define DM_UV_EAI_PROTOCOL UV_EAI_PROTOCOL
#define DM_UV_EAI_SERVICE UV_EAI_SERVICE
#define DM_UV_EAI_SOCKTYPE UV_EAI_SOCKTYPE
#define DM_UV_EALREADY UV_EALREADY
#define DM_UV_EBADF UV_EBADF
#define DM_UV_EBUSY UV_EBUSY
#define DM_UV_ECANCELED UV_ECANCELED
#define DM_UV_ECHARSET UV_ECHARSET
#define DM_UV_ECONNABORTED UV_ECONNABORTED
#define DM_UV_ECONNREFUSED UV_ECONNREFUSED
#define DM_UV_ECONNRESET UV_ECONNRESET
#define DM_UV_EDESTADDRREQ UV_EDESTADDRREQ
#define DM_UV_EEXIST UV_EEXIST
#define DM_UV_EFAULT UV_EFAULT
#define DM_UV_EFBIG UV_EFBIG
#define DM_UV_EFTYPE UV_EFTYPE
#define DM_UV_EHOSTDOWN UV_EHOSTDOWN
#define DM_UV_EHOSTUNREACH UV_EHOSTUNREACH
#define DM_UV_EILSEQ UV_EILSEQ
#define DM_UV_EINTR UV_EINTR
#define DM_UV_EINVAL UV_EINVAL
#define DM_UV_EIO UV_EIO
#define DM_UV_EISCONN UV_EISCONN
#define DM_UV_EISDIR UV_EISDIR
#define DM_UV_ELOOP UV_ELOOP
#define DM_UV_EMFILE UV_EMFILE
#define DM_UV_EMLINK UV_EMLINK
#define DM_UV_EMSGSIZE UV_EMSGSIZE
#define DM_UV_ENAMETOOLONG UV_ENAMETOOLONG
#define DM_UV_ENETDOWN UV_ENETDOWN
#define DM_UV_ENETUNREACH UV_ENETUNREACH
#define DM_UV_ENFILE UV_ENFILE
#define DM_UV_ENOBUFS UV_ENOBUFS
#define DM_UV_ENODATA UV_ENODATA
#define DM_UV_ENODEV UV_ENODEV
#define DM_UV_ENOENT UV_ENOENT
#define DM_UV_ENOEXEC UV_ENOEXEC
#define DM_UV_ENOMEM UV_ENOMEM
#define DM_UV_ENONET UV_ENONET
#define DM_UV_ENOPROTOOPT UV_ENOPROTOOPT
#define DM_UV_ENOSPC UV_ENOSPC
#define DM_UV_ENOSYS UV_ENOSYS
#define DM_UV_ENOTCONN UV_ENOTCONN
#define DM_UV_ENOTDIR UV_ENOTDIR
#define DM_UV_ENOTEMPTY UV_ENOTEMPTY
#define DM_UV_ENOTSOCK UV_ENOTSOCK
#define DM_UV_ENOTSUP UV_ENOTSUP
#define DM_UV_ENOTTY UV_ENOTTY
#define DM_UV_ENXIO UV_ENXIO
#define DM_UV_EOF UV_EOF
#define DM_UV_EOVERFLOW UV_EOVERFLOW
#define DM_UV_EPERM UV_EPERM
#define DM_UV_EPIPE UV_EPIPE
#define DM_UV_EPROTO UV_EPROTO
#define DM_UV_EPROTONOSUPPORT UV_EPROTONOSUPPORT
#define DM_UV_EPROTOTYPE UV_EPROTOTYPE
#define DM_UV_ERANGE UV_ERANGE
#define DM_UV_EREMOTEIO UV_EREMOTEIO
#define DM_UV_EROFS UV_EROFS
#define DM_UV_ERRNO_MAX UV_ERRNO_MAX
#define DM_UV_ESHUTDOWN UV_ESHUTDOWN
#define DM_UV_ESOCKTNOSUPPORT UV_ESOCKTNOSUPPORT
#define DM_UV_ESPIPE UV_ESPIPE
#define DM_UV_ESRCH UV_ESRCH
#define DM_UV_ETIMEDOUT UV_ETIMEDOUT
#define DM_UV_ETXTBSY UV_ETXTBSY
#define DM_UV_EUNATCH UV_EUNATCH
#define DM_UV_EXDEV UV_EXDEV
#define DM_UV_FILE UV_FILE
#define DM_UV_FS UV_FS
#define DM_UV_FS_ACCESS UV_FS_ACCESS
#define DM_UV_FS_CHMOD UV_FS_CHMOD
#define DM_UV_FS_CHOWN UV_FS_CHOWN
#define DM_UV_FS_CLOSE UV_FS_CLOSE
#define DM_UV_FS_CLOSEDIR UV_FS_CLOSEDIR
#define DM_UV_FS_COPYFILE UV_FS_COPYFILE
#define DM_UV_FS_COPYFILE_EXCL UV_FS_COPYFILE_EXCL
#define DM_UV_FS_COPYFILE_FICLONE UV_FS_COPYFILE_FICLONE
#define DM_UV_FS_COPYFILE_FICLONE_FORCE UV_FS_COPYFILE_FICLONE_FORCE
#define DM_UV_FS_CUSTOM UV_FS_CUSTOM
#define DM_UV_FS_EVENT UV_FS_EVENT
#define DM_UV_FS_EVENT_RECURSIVE UV_FS_EVENT_RECURSIVE
#define DM_UV_FS_EVENT_STAT UV_FS_EVENT_STAT
#define DM_UV_FS_EVENT_WATCH_ENTRY UV_FS_EVENT_WATCH_ENTRY
#define DM_UV_FS_FCHMOD UV_FS_FCHMOD
#define DM_UV_FS_FCHOWN UV_FS_FCHOWN
#define DM_UV_FS_FDATASYNC UV_FS_FDATASYNC
#define DM_UV_FS_FSTAT UV_FS_FSTAT
#define DM_UV_FS_FSYNC UV_FS_FSYNC
#define DM_UV_FS_FTRUNCATE UV_FS_FTRUNCATE
#define DM_UV_FS_FUTIME UV_FS_FUTIME
#define DM_UV_FS_LCHOWN UV_FS_LCHOWN
#define DM_UV_FS_LINK UV_FS_LINK
#define DM_UV_FS_LSTAT UV_FS_LSTAT
#define DM_UV_FS_LUTIME UV_FS_LUTIME
#define DM_UV_FS_MKDIR UV_FS_MKDIR
#define DM_UV_FS_MKDTEMP UV_FS_MKDTEMP
#define DM_UV_FS_MKSTEMP UV_FS_MKSTEMP
#define DM_UV_FS_OPEN UV_FS_OPEN
#define DM_UV_FS_OPENDIR UV_FS_OPENDIR
#define DM_UV_FS_POLL UV_FS_POLL
#define DM_UV_FS_READ UV_FS_READ
#define DM_UV_FS_READDIR UV_FS_READDIR
#define DM_UV_FS_READLINK UV_FS_READLINK
#define DM_UV_FS_REALPATH UV_FS_REALPATH
#define DM_UV_FS_RENAME UV_FS_RENAME
#define DM_UV_FS_RMDIR UV_FS_RMDIR
#define DM_UV_FS_SCANDIR UV_FS_SCANDIR
#define DM_UV_FS_SENDFILE UV_FS_SENDFILE
#define DM_UV_FS_STAT UV_FS_STAT
#define DM_UV_FS_STATFS UV_FS_STATFS
#define DM_UV_FS_SYMLINK UV_FS_SYMLINK
#define DM_UV_FS_SYMLINK_DIR UV_FS_SYMLINK_DIR
#define DM_UV_FS_SYMLINK_JUNCTION UV_FS_SYMLINK_JUNCTION
#define DM_UV_FS_UNKNOWN UV_FS_UNKNOWN
#define DM_UV_FS_UNLINK UV_FS_UNLINK
#define DM_UV_FS_UTIME UV_FS_UTIME
#define DM_UV_FS_UTIME_NOW UV_FS_UTIME_NOW
#define DM_UV_FS_UTIME_OMIT UV_FS_UTIME_OMIT
#define DM_UV_FS_WRITE UV_FS_WRITE
#define DM_UV_GETADDRINFO UV_GETADDRINFO
#define DM_UV_GETNAMEINFO UV_GETNAMEINFO
#define DM_UV_HANDLE UV_HANDLE
#define DM_UV_HANDLE_TYPE_MAX UV_HANDLE_TYPE_MAX
#define DM_UV_IDLE UV_IDLE
#define DM_UV_IGNORE UV_IGNORE
#define DM_UV_INHERIT_FD UV_INHERIT_FD
#define DM_UV_INHERIT_STREAM UV_INHERIT_STREAM
#define DM_UV_JOIN_GROUP UV_JOIN_GROUP
#define DM_UV_LEAVE_GROUP UV_LEAVE_GROUP
#define DM_UV_LOOP_BLOCK_SIGNAL UV_LOOP_BLOCK_SIGNAL
#define DM_UV_LOOP_USE_IO_URING_SQPOLL UV_LOOP_USE_IO_URING_SQPOLL
#define DM_UV_METRICS_IDLE_TIME UV_METRICS_IDLE_TIME
#define DM_UV_NAMED_PIPE UV_NAMED_PIPE
#define DM_UV_NONBLOCK_PIPE UV_NONBLOCK_PIPE
#define DM_UV_ONCE_INIT UV_ONCE_INIT
#define DM_UV_OVERLAPPED_PIPE UV_OVERLAPPED_PIPE
#define DM_UV_PIPE_NO_TRUNCATE UV_PIPE_NO_TRUNCATE
#define DM_UV_POLL UV_POLL
#define DM_UV_PREPARE UV_PREPARE
#define DM_UV_PRIORITIZED UV_PRIORITIZED
#define DM_UV_PROCESS UV_PROCESS
#define DM_UV_PROCESS_DETACHED UV_PROCESS_DETACHED
#define DM_UV_PROCESS_SETGID UV_PROCESS_SETGID
#define DM_UV_PROCESS_SETUID UV_PROCESS_SETUID
#define DM_UV_PROCESS_WINDOWS_FILE_PATH_EXACT_NAME UV_PROCESS_WINDOWS_FILE_PATH_EXACT_NAME
#define DM_UV_PROCESS_WINDOWS_HIDE UV_PROCESS_WINDOWS_HIDE
#define DM_UV_PROCESS_WINDOWS_HIDE_CONSOLE UV_PROCESS_WINDOWS_HIDE_CONSOLE
#define DM_UV_PROCESS_WINDOWS_HIDE_GUI UV_PROCESS_WINDOWS_HIDE_GUI
#define DM_UV_PROCESS_WINDOWS_VERBATIM_ARGUMENTS UV_PROCESS_WINDOWS_VERBATIM_ARGUMENTS
#define DM_UV_RANDOM UV_RANDOM
#define DM_UV_READABLE UV_READABLE
#define DM_UV_READABLE_PIPE UV_READABLE_PIPE
#define DM_UV_RENAME UV_RENAME
#define DM_UV_REQ UV_REQ
#define DM_UV_REQ_TYPE_MAX UV_REQ_TYPE_MAX
#define DM_UV_REQ_TYPE_PRIVATE UV_REQ_TYPE_PRIVATE
#define DM_UV_RUN_DEFAULT UV_RUN_DEFAULT
#define DM_UV_RUN_NOWAIT UV_RUN_NOWAIT
#define DM_UV_RUN_ONCE UV_RUN_ONCE
#define DM_UV_SHUTDOWN UV_SHUTDOWN
#define DM_UV_SIGNAL UV_SIGNAL
#define DM_UV_STREAM UV_STREAM
#define DM_UV_TCP UV_TCP
#define DM_UV_TCP_IPV6ONLY UV_TCP_IPV6ONLY
#define DM_UV_TCP_REUSEPORT UV_TCP_REUSEPORT
#define DM_UV_THREAD_HAS_STACK_SIZE UV_THREAD_HAS_STACK_SIZE
#define DM_UV_THREAD_NO_FLAGS UV_THREAD_NO_FLAGS
#define DM_UV_THREAD_PRIORITY_ABOVE_NORMAL UV_THREAD_PRIORITY_ABOVE_NORMAL
#define DM_UV_THREAD_PRIORITY_BELOW_NORMAL UV_THREAD_PRIORITY_BELOW_NORMAL
#define DM_UV_THREAD_PRIORITY_HIGHEST UV_THREAD_PRIORITY_HIGHEST
#define DM_UV_THREAD_PRIORITY_LOWEST UV_THREAD_PRIORITY_LOWEST
#define DM_UV_THREAD_PRIORITY_NORMAL UV_THREAD_PRIORITY_NORMAL
#define DM_UV_TIMER UV_TIMER
#define DM_UV_TTY UV_TTY
#define DM_UV_TTY_MODE_IO UV_TTY_MODE_IO
#define DM_UV_TTY_MODE_NORMAL UV_TTY_MODE_NORMAL
#define DM_UV_TTY_MODE_RAW UV_TTY_MODE_RAW
#define DM_UV_TTY_MODE_RAW_VT UV_TTY_MODE_RAW_VT
#define DM_UV_TTY_SUPPORTED UV_TTY_SUPPORTED
#define DM_UV_TTY_UNSUPPORTED UV_TTY_UNSUPPORTED
#define DM_UV_UDP UV_UDP
#define DM_UV_UDP_IPV6ONLY UV_UDP_IPV6ONLY
#define DM_UV_UDP_LINUX_RECVERR UV_UDP_LINUX_RECVERR
#define DM_UV_UDP_MMSG_CHUNK UV_UDP_MMSG_CHUNK
#define DM_UV_UDP_MMSG_FREE UV_UDP_MMSG_FREE
#define DM_UV_UDP_PARTIAL UV_UDP_PARTIAL
#define DM_UV_UDP_RECVMMSG UV_UDP_RECVMMSG
#define DM_UV_UDP_REUSEADDR UV_UDP_REUSEADDR
#define DM_UV_UDP_REUSEPORT UV_UDP_REUSEPORT
#define DM_UV_UDP_SEND UV_UDP_SEND
#define DM_UV_UNKNOWN UV_UNKNOWN
#define DM_UV_UNKNOWN_HANDLE UV_UNKNOWN_HANDLE
#define DM_UV_UNKNOWN_REQ UV_UNKNOWN_REQ
#define DM_UV_WORK UV_WORK
#define DM_UV_WRITABLE UV_WRITABLE
#define DM_UV_WRITABLE_PIPE UV_WRITABLE_PIPE
#define DM_UV_WRITE UV_WRITE
#define DM_UV_FS_O_RDONLY UV_FS_O_RDONLY
#define DM_UV_FS_O_WRONLY UV_FS_O_WRONLY
#define DM_UV_FS_O_RDWR UV_FS_O_RDWR
#define DM_UV_FS_O_CREAT UV_FS_O_CREAT
#define DM_UV_FS_O_TRUNC UV_FS_O_TRUNC
#define DM_UV_FS_O_EXCL UV_FS_O_EXCL
#define DM_UV_FS_O_APPEND UV_FS_O_APPEND

/* ---- 函数适配:dm_uv_<name> 封装 uv_<name>（内部可见，无 DM_API） ---- */
/* 变参函数(C 无法前向传递 `...`)不在此生成,由 dm.c 手工适配:
 *   uv_loop_configure
 */
/* 以下函数由 dm.c 手工实现(生成器只保留其类型别名,不生成直通函数):
 *   uv_version, uv_version_string, uv_replace_allocator, uv_os_environ, uv_os_free_environ
 */
void dm_uv_library_shutdown(void);
dm_uv_loop_st *dm_uv_default_loop(void);
int dm_uv_loop_init(dm_uv_loop_st *loop);
int dm_uv_loop_close(dm_uv_loop_st *loop);
dm_uv_loop_st *dm_uv_loop_new(void);
void dm_uv_loop_delete(dm_uv_loop_st *arg1);
size_t dm_uv_loop_size(void);
int dm_uv_loop_alive(const dm_uv_loop_st *loop);
int dm_uv_loop_fork(dm_uv_loop_st *loop);
int dm_uv_run(dm_uv_loop_st *arg1, dm_uv_run_mode_et mode);
void dm_uv_stop(dm_uv_loop_st *arg1);
void dm_uv_ref(dm_uv_handle_st *arg1);
void dm_uv_unref(dm_uv_handle_st *arg1);
int dm_uv_has_ref(const dm_uv_handle_st *arg1);
void dm_uv_update_time(dm_uv_loop_st *arg1);
uint64_t dm_uv_now(const dm_uv_loop_st *arg1);
int dm_uv_backend_fd(const dm_uv_loop_st *arg1);
int dm_uv_backend_timeout(const dm_uv_loop_st *arg1);
int dm_uv_translate_sys_error(int sys_errno);
const char *dm_uv_strerror(int err);
char *dm_uv_strerror_r(int err, char *buf, size_t buflen);
const char *dm_uv_err_name(int err);
char *dm_uv_err_name_r(int err, char *buf, size_t buflen);
int dm_uv_shutdown(dm_uv_shutdown_st *req, dm_uv_stream_st *handle, dm_uv_shutdown_cb cb);
size_t dm_uv_handle_size(dm_uv_handle_type_et type);
dm_uv_handle_type_et dm_uv_handle_get_type(const dm_uv_handle_st *handle);
const char *dm_uv_handle_type_name(dm_uv_handle_type_et type);
void *dm_uv_handle_get_data(const dm_uv_handle_st *handle);
dm_uv_loop_st *dm_uv_handle_get_loop(const dm_uv_handle_st *handle);
void dm_uv_handle_set_data(dm_uv_handle_st *handle, void *data);
size_t dm_uv_req_size(dm_uv_req_type_et type);
void *dm_uv_req_get_data(const dm_uv_req_st *req);
void dm_uv_req_set_data(dm_uv_req_st *req, void *data);
dm_uv_req_type_et dm_uv_req_get_type(const dm_uv_req_st *req);
const char *dm_uv_req_type_name(dm_uv_req_type_et type);
int dm_uv_is_active(const dm_uv_handle_st *handle);
void dm_uv_walk(dm_uv_loop_st *loop, dm_uv_walk_cb walk_cb, void *arg);
void dm_uv_print_all_handles(dm_uv_loop_st *loop, FILE *stream);
void dm_uv_print_active_handles(dm_uv_loop_st *loop, FILE *stream);
void dm_uv_close(dm_uv_handle_st *handle, dm_uv_close_cb close_cb);
int dm_uv_send_buffer_size(dm_uv_handle_st *handle, int *value);
int dm_uv_recv_buffer_size(dm_uv_handle_st *handle, int *value);
int dm_uv_fileno(const dm_uv_handle_st *handle, dm_uv_os_fd_t *fd);
dm_uv_buf_st dm_uv_buf_init(char *base, unsigned int len);
int dm_uv_pipe(dm_uv_file fds[2], int read_flags, int write_flags);
int dm_uv_socketpair(int type, int protocol, dm_uv_os_sock_t socket_vector[2], int flags0,
                     int flags1);
size_t dm_uv_stream_get_write_queue_size(const dm_uv_stream_st *stream);
int dm_uv_listen(dm_uv_stream_st *stream, int backlog, dm_uv_connection_cb cb);
int dm_uv_accept(dm_uv_stream_st *server, dm_uv_stream_st *client);
int dm_uv_read_start(dm_uv_stream_st *arg1, dm_uv_alloc_cb alloc_cb, dm_uv_read_cb read_cb);
int dm_uv_read_stop(dm_uv_stream_st *arg1);
int dm_uv_write(dm_uv_write_st *req, dm_uv_stream_st *handle, const dm_uv_buf_st bufs[],
                unsigned int nbufs, dm_uv_write_cb cb);
int dm_uv_write2(dm_uv_write_st *req, dm_uv_stream_st *handle, const dm_uv_buf_st bufs[],
                 unsigned int nbufs, dm_uv_stream_st *send_handle, dm_uv_write_cb cb);
int dm_uv_try_write(dm_uv_stream_st *handle, const dm_uv_buf_st bufs[], unsigned int nbufs);
int dm_uv_try_write2(dm_uv_stream_st *handle, const dm_uv_buf_st bufs[], unsigned int nbufs,
                     dm_uv_stream_st *send_handle);
size_t dm_uv_write_nwritten(const dm_uv_write_st *req);
int dm_uv_is_readable(const dm_uv_stream_st *handle);
int dm_uv_is_writable(const dm_uv_stream_st *handle);
int dm_uv_stream_set_blocking(dm_uv_stream_st *handle, int blocking);
int dm_uv_is_closing(const dm_uv_handle_st *handle);
int dm_uv_tcp_init(dm_uv_loop_st *arg1, dm_uv_tcp_st *handle);
int dm_uv_tcp_init_ex(dm_uv_loop_st *arg1, dm_uv_tcp_st *handle, unsigned int flags);
int dm_uv_tcp_open(dm_uv_tcp_st *handle, dm_uv_os_sock_t sock);
int dm_uv_tcp_nodelay(dm_uv_tcp_st *handle, int enable);
int dm_uv_tcp_keepalive(dm_uv_tcp_st *handle, int enable, unsigned int delay);
int dm_uv_tcp_keepalive_ex(dm_uv_tcp_st *handle, int on, unsigned int idle, unsigned int intvl,
                           unsigned int cnt);
int dm_uv_tcp_simultaneous_accepts(dm_uv_tcp_st *handle, int enable);
int dm_uv_tcp_bind(dm_uv_tcp_st *handle, const struct sockaddr *addr, unsigned int flags);
int dm_uv_tcp_getsockname(const dm_uv_tcp_st *handle, struct sockaddr *name, int *namelen);
int dm_uv_tcp_getpeername(const dm_uv_tcp_st *handle, struct sockaddr *name, int *namelen);
int dm_uv_tcp_close_reset(dm_uv_tcp_st *handle, dm_uv_close_cb close_cb);
int dm_uv_tcp_connect(dm_uv_connect_st *req, dm_uv_tcp_st *handle, const struct sockaddr *addr,
                      dm_uv_connect_cb cb);
int dm_uv_udp_init(dm_uv_loop_st *arg1, dm_uv_udp_st *handle);
int dm_uv_udp_init_ex(dm_uv_loop_st *arg1, dm_uv_udp_st *handle, unsigned int flags);
int dm_uv_udp_open(dm_uv_udp_st *handle, dm_uv_os_sock_t sock);
int dm_uv_udp_open_ex(dm_uv_udp_st *handle, dm_uv_os_sock_t sock, unsigned int flags);
int dm_uv_udp_bind(dm_uv_udp_st *handle, const struct sockaddr *addr, unsigned int flags);
int dm_uv_udp_connect(dm_uv_udp_st *handle, const struct sockaddr *addr);
int dm_uv_udp_getpeername(const dm_uv_udp_st *handle, struct sockaddr *name, int *namelen);
int dm_uv_udp_getsockname(const dm_uv_udp_st *handle, struct sockaddr *name, int *namelen);
int dm_uv_udp_set_membership(dm_uv_udp_st *handle, const char *multicast_addr,
                             const char *interface_addr, dm_uv_membership_et membership);
int dm_uv_udp_set_source_membership(dm_uv_udp_st *handle, const char *multicast_addr,
                                    const char *interface_addr, const char *source_addr,
                                    dm_uv_membership_et membership);
int dm_uv_udp_set_multicast_loop(dm_uv_udp_st *handle, int on);
int dm_uv_udp_set_multicast_ttl(dm_uv_udp_st *handle, int ttl);
int dm_uv_udp_set_multicast_interface(dm_uv_udp_st *handle, const char *interface_addr);
int dm_uv_udp_set_broadcast(dm_uv_udp_st *handle, int on);
int dm_uv_udp_set_ttl(dm_uv_udp_st *handle, int ttl);
int dm_uv_udp_send(dm_uv_udp_send_st *req, dm_uv_udp_st *handle, const dm_uv_buf_st bufs[],
                   unsigned int nbufs, const struct sockaddr *addr, dm_uv_udp_send_cb send_cb);
int dm_uv_udp_try_send(dm_uv_udp_st *handle, const dm_uv_buf_st bufs[], unsigned int nbufs,
                       const struct sockaddr *addr);
int dm_uv_udp_try_send2(dm_uv_udp_st *handle, unsigned int count, dm_uv_buf_st *bufs[],
                        unsigned int nbufs[], struct sockaddr *addrs[], unsigned int flags);
int dm_uv_udp_recv_start(dm_uv_udp_st *handle, dm_uv_alloc_cb alloc_cb, dm_uv_udp_recv_cb recv_cb);
int dm_uv_udp_using_recvmmsg(const dm_uv_udp_st *handle);
int dm_uv_udp_recv_stop(dm_uv_udp_st *handle);
size_t dm_uv_udp_get_send_queue_size(const dm_uv_udp_st *handle);
size_t dm_uv_udp_get_send_queue_count(const dm_uv_udp_st *handle);
int dm_uv_tty_init(dm_uv_loop_st *arg1, dm_uv_tty_st *arg2, dm_uv_file fd, int readable);
int dm_uv_tty_set_mode(dm_uv_tty_st *arg1, dm_uv_tty_mode_et mode);
int dm_uv_tty_reset_mode(void);
int dm_uv_tty_get_winsize(dm_uv_tty_st *arg1, int *width, int *height);
void dm_uv_tty_set_vterm_state(dm_uv_tty_vtermstate_et state);
int dm_uv_tty_get_vterm_state(dm_uv_tty_vtermstate_et *state);
dm_uv_handle_type_et dm_uv_guess_handle(dm_uv_file file);
int dm_uv_pipe_init(dm_uv_loop_st *arg1, dm_uv_pipe_st *handle, int ipc);
int dm_uv_pipe_open(dm_uv_pipe_st *arg1, dm_uv_file file);
int dm_uv_pipe_bind(dm_uv_pipe_st *handle, const char *name);
int dm_uv_pipe_bind2(dm_uv_pipe_st *handle, const char *name, size_t namelen, unsigned int flags);
void dm_uv_pipe_connect(dm_uv_connect_st *req, dm_uv_pipe_st *handle, const char *name,
                        dm_uv_connect_cb cb);
int dm_uv_pipe_connect2(dm_uv_connect_st *req, dm_uv_pipe_st *handle, const char *name,
                        size_t namelen, unsigned int flags, dm_uv_connect_cb cb);
int dm_uv_pipe_getsockname(const dm_uv_pipe_st *handle, char *buffer, size_t *size);
int dm_uv_pipe_getpeername(const dm_uv_pipe_st *handle, char *buffer, size_t *size);
void dm_uv_pipe_pending_instances(dm_uv_pipe_st *handle, int count);
int dm_uv_pipe_pending_count(dm_uv_pipe_st *handle);
dm_uv_handle_type_et dm_uv_pipe_pending_type(dm_uv_pipe_st *handle);
int dm_uv_pipe_chmod(dm_uv_pipe_st *handle, int flags);
int dm_uv_poll_init(dm_uv_loop_st *loop, dm_uv_poll_st *handle, int fd);
int dm_uv_poll_init_socket(dm_uv_loop_st *loop, dm_uv_poll_st *handle, dm_uv_os_sock_t socket);
int dm_uv_poll_start(dm_uv_poll_st *handle, int events, dm_uv_poll_cb cb);
int dm_uv_poll_stop(dm_uv_poll_st *handle);
int dm_uv_prepare_init(dm_uv_loop_st *arg1, dm_uv_prepare_st *prepare);
int dm_uv_prepare_start(dm_uv_prepare_st *prepare, dm_uv_prepare_cb cb);
int dm_uv_prepare_stop(dm_uv_prepare_st *prepare);
int dm_uv_check_init(dm_uv_loop_st *arg1, dm_uv_check_st *check);
int dm_uv_check_start(dm_uv_check_st *check, dm_uv_check_cb cb);
int dm_uv_check_stop(dm_uv_check_st *check);
int dm_uv_idle_init(dm_uv_loop_st *arg1, dm_uv_idle_st *idle);
int dm_uv_idle_start(dm_uv_idle_st *idle, dm_uv_idle_cb cb);
int dm_uv_idle_stop(dm_uv_idle_st *idle);
int dm_uv_async_init(dm_uv_loop_st *arg1, dm_uv_async_st *async, dm_uv_async_cb async_cb);
int dm_uv_async_send(dm_uv_async_st *async);
int dm_uv_timer_init(dm_uv_loop_st *arg1, dm_uv_timer_st *handle);
int dm_uv_timer_start(dm_uv_timer_st *handle, dm_uv_timer_cb cb, uint64_t timeout, uint64_t repeat);
int dm_uv_timer_stop(dm_uv_timer_st *handle);
int dm_uv_timer_again(dm_uv_timer_st *handle);
void dm_uv_timer_set_repeat(dm_uv_timer_st *handle, uint64_t repeat);
uint64_t dm_uv_timer_get_repeat(const dm_uv_timer_st *handle);
uint64_t dm_uv_timer_get_due_in(const dm_uv_timer_st *handle);
int dm_uv_getaddrinfo(dm_uv_loop_st *loop, dm_uv_getaddrinfo_st *req,
                      dm_uv_getaddrinfo_cb getaddrinfo_cb, const char *node, const char *service,
                      const struct addrinfo *hints);
void dm_uv_freeaddrinfo(struct addrinfo *ai);
int dm_uv_getnameinfo(dm_uv_loop_st *loop, dm_uv_getnameinfo_st *req,
                      dm_uv_getnameinfo_cb getnameinfo_cb, const struct sockaddr *addr, int flags);
int dm_uv_spawn(dm_uv_loop_st *loop, dm_uv_process_st *handle,
                const dm_uv_process_options_st *options);
int dm_uv_process_kill(dm_uv_process_st *arg1, int signum);
int dm_uv_kill(int pid, int signum);
dm_uv_pid_t dm_uv_process_get_pid(const dm_uv_process_st *arg1);
int dm_uv_queue_work(dm_uv_loop_st *loop, dm_uv_work_st *req, dm_uv_work_cb work_cb,
                     dm_uv_after_work_cb after_work_cb);
int dm_uv_cancel(dm_uv_req_st *req);
char **dm_uv_setup_args(int argc, char **argv);
int dm_uv_get_process_title(char *buffer, size_t size);
int dm_uv_set_process_title(const char *title);
int dm_uv_resident_set_memory(size_t *rss);
int dm_uv_uptime(double *uptime);
dm_uv_os_fd_t dm_uv_get_osfhandle(int fd);
int dm_uv_open_osfhandle(dm_uv_os_fd_t os_fd);
int dm_uv_getrusage(dm_uv_rusage_st *rusage);
int dm_uv_getrusage_thread(dm_uv_rusage_st *rusage);
int dm_uv_os_homedir(char *buffer, size_t *size);
int dm_uv_os_tmpdir(char *buffer, size_t *size);
int dm_uv_os_get_passwd(dm_uv_passwd_st *pwd);
void dm_uv_os_free_passwd(dm_uv_passwd_st *pwd);
int dm_uv_os_get_passwd2(dm_uv_passwd_st *pwd, dm_uv_uid_t uid);
int dm_uv_os_get_group(dm_uv_group_st *grp, dm_uv_uid_t gid);
void dm_uv_os_free_group(dm_uv_group_st *grp);
dm_uv_pid_t dm_uv_os_getpid(void);
dm_uv_pid_t dm_uv_os_getppid(void);
int dm_uv_os_getpriority(dm_uv_pid_t pid, int *priority);
int dm_uv_os_setpriority(dm_uv_pid_t pid, int priority);
int dm_uv_thread_getpriority(dm_uv_thread_t tid, int *priority);
int dm_uv_thread_setpriority(dm_uv_thread_t tid, int priority);
unsigned int dm_uv_available_parallelism(void);
int dm_uv_cpu_info(dm_uv_cpu_info_st **cpu_infos, int *count);
void dm_uv_free_cpu_info(dm_uv_cpu_info_st *cpu_infos, int count);
int dm_uv_cpumask_size(void);
int dm_uv_interface_addresses(dm_uv_interface_address_st **addresses, int *count);
void dm_uv_free_interface_addresses(dm_uv_interface_address_st *addresses, int count);
int dm_uv_os_getenv(const char *name, char *buffer, size_t *size);
int dm_uv_os_setenv(const char *name, const char *value);
int dm_uv_os_unsetenv(const char *name);
int dm_uv_os_gethostname(char *buffer, size_t *size);
int dm_uv_os_uname(dm_uv_utsname_st *buffer);
int dm_uv_metrics_info(dm_uv_loop_st *loop, dm_uv_metrics_st *metrics);
uint64_t dm_uv_metrics_idle_time(dm_uv_loop_st *loop);
dm_uv_fs_type_et dm_uv_fs_get_type(const dm_uv_fs_st *arg1);
ssize_t dm_uv_fs_get_result(const dm_uv_fs_st *arg1);
int dm_uv_fs_get_system_error(const dm_uv_fs_st *arg1);
void *dm_uv_fs_get_ptr(const dm_uv_fs_st *arg1);
const char *dm_uv_fs_get_path(const dm_uv_fs_st *arg1);
dm_uv_stat_st *dm_uv_fs_get_statbuf(dm_uv_fs_st *arg1);
void dm_uv_fs_req_cleanup(dm_uv_fs_st *req);
int dm_uv_fs_close(dm_uv_loop_st *loop, dm_uv_fs_st *req, dm_uv_file file, dm_uv_fs_cb cb);
int dm_uv_fs_open(dm_uv_loop_st *loop, dm_uv_fs_st *req, const char *path, int flags, int mode,
                  dm_uv_fs_cb cb);
int dm_uv_fs_read(dm_uv_loop_st *loop, dm_uv_fs_st *req, dm_uv_file file, const dm_uv_buf_st bufs[],
                  unsigned int nbufs, int64_t offset, dm_uv_fs_cb cb);
int dm_uv_fs_unlink(dm_uv_loop_st *loop, dm_uv_fs_st *req, const char *path, dm_uv_fs_cb cb);
int dm_uv_fs_write(dm_uv_loop_st *loop, dm_uv_fs_st *req, dm_uv_file file,
                   const dm_uv_buf_st bufs[], unsigned int nbufs, int64_t offset, dm_uv_fs_cb cb);
int dm_uv_fs_copyfile(dm_uv_loop_st *loop, dm_uv_fs_st *req, const char *path, const char *new_path,
                      int flags, dm_uv_fs_cb cb);
int dm_uv_fs_mkdir(dm_uv_loop_st *loop, dm_uv_fs_st *req, const char *path, int mode,
                   dm_uv_fs_cb cb);
int dm_uv_fs_mkdtemp(dm_uv_loop_st *loop, dm_uv_fs_st *req, const char *tpl, dm_uv_fs_cb cb);
int dm_uv_fs_mkstemp(dm_uv_loop_st *loop, dm_uv_fs_st *req, const char *tpl, dm_uv_fs_cb cb);
int dm_uv_fs_rmdir(dm_uv_loop_st *loop, dm_uv_fs_st *req, const char *path, dm_uv_fs_cb cb);
int dm_uv_fs_scandir(dm_uv_loop_st *loop, dm_uv_fs_st *req, const char *path, int flags,
                     dm_uv_fs_cb cb);
int dm_uv_fs_scandir_next(dm_uv_fs_st *req, dm_uv_dirent_st *ent);
int dm_uv_fs_opendir(dm_uv_loop_st *loop, dm_uv_fs_st *req, const char *path, dm_uv_fs_cb cb);
int dm_uv_fs_readdir(dm_uv_loop_st *loop, dm_uv_fs_st *req, dm_uv_dir_st *dir, dm_uv_fs_cb cb);
int dm_uv_fs_closedir(dm_uv_loop_st *loop, dm_uv_fs_st *req, dm_uv_dir_st *dir, dm_uv_fs_cb cb);
int dm_uv_fs_stat(dm_uv_loop_st *loop, dm_uv_fs_st *req, const char *path, dm_uv_fs_cb cb);
int dm_uv_fs_fstat(dm_uv_loop_st *loop, dm_uv_fs_st *req, dm_uv_file file, dm_uv_fs_cb cb);
int dm_uv_fs_rename(dm_uv_loop_st *loop, dm_uv_fs_st *req, const char *path, const char *new_path,
                    dm_uv_fs_cb cb);
int dm_uv_fs_fsync(dm_uv_loop_st *loop, dm_uv_fs_st *req, dm_uv_file file, dm_uv_fs_cb cb);
int dm_uv_fs_fdatasync(dm_uv_loop_st *loop, dm_uv_fs_st *req, dm_uv_file file, dm_uv_fs_cb cb);
int dm_uv_fs_ftruncate(dm_uv_loop_st *loop, dm_uv_fs_st *req, dm_uv_file file, int64_t offset,
                       dm_uv_fs_cb cb);
int dm_uv_fs_sendfile(dm_uv_loop_st *loop, dm_uv_fs_st *req, dm_uv_file out_fd, dm_uv_file in_fd,
                      int64_t in_offset, size_t length, dm_uv_fs_cb cb);
int dm_uv_fs_access(dm_uv_loop_st *loop, dm_uv_fs_st *req, const char *path, int mode,
                    dm_uv_fs_cb cb);
int dm_uv_fs_chmod(dm_uv_loop_st *loop, dm_uv_fs_st *req, const char *path, int mode,
                   dm_uv_fs_cb cb);
int dm_uv_fs_utime(dm_uv_loop_st *loop, dm_uv_fs_st *req, const char *path, double atime,
                   double mtime, dm_uv_fs_cb cb);
int dm_uv_fs_futime(dm_uv_loop_st *loop, dm_uv_fs_st *req, dm_uv_file file, double atime,
                    double mtime, dm_uv_fs_cb cb);
int dm_uv_fs_lutime(dm_uv_loop_st *loop, dm_uv_fs_st *req, const char *path, double atime,
                    double mtime, dm_uv_fs_cb cb);
int dm_uv_fs_lstat(dm_uv_loop_st *loop, dm_uv_fs_st *req, const char *path, dm_uv_fs_cb cb);
int dm_uv_fs_link(dm_uv_loop_st *loop, dm_uv_fs_st *req, const char *path, const char *new_path,
                  dm_uv_fs_cb cb);
int dm_uv_fs_symlink(dm_uv_loop_st *loop, dm_uv_fs_st *req, const char *path, const char *new_path,
                     int flags, dm_uv_fs_cb cb);
int dm_uv_fs_readlink(dm_uv_loop_st *loop, dm_uv_fs_st *req, const char *path, dm_uv_fs_cb cb);
int dm_uv_fs_realpath(dm_uv_loop_st *loop, dm_uv_fs_st *req, const char *path, dm_uv_fs_cb cb);
int dm_uv_fs_fchmod(dm_uv_loop_st *loop, dm_uv_fs_st *req, dm_uv_file file, int mode,
                    dm_uv_fs_cb cb);
int dm_uv_fs_chown(dm_uv_loop_st *loop, dm_uv_fs_st *req, const char *path, dm_uv_uid_t uid,
                   dm_uv_gid_t gid, dm_uv_fs_cb cb);
int dm_uv_fs_fchown(dm_uv_loop_st *loop, dm_uv_fs_st *req, dm_uv_file file, dm_uv_uid_t uid,
                    dm_uv_gid_t gid, dm_uv_fs_cb cb);
int dm_uv_fs_lchown(dm_uv_loop_st *loop, dm_uv_fs_st *req, const char *path, dm_uv_uid_t uid,
                    dm_uv_gid_t gid, dm_uv_fs_cb cb);
int dm_uv_fs_statfs(dm_uv_loop_st *loop, dm_uv_fs_st *req, const char *path, dm_uv_fs_cb cb);
int dm_uv_fs_poll_init(dm_uv_loop_st *loop, dm_uv_fs_poll_st *handle);
int dm_uv_fs_poll_start(dm_uv_fs_poll_st *handle, dm_uv_fs_poll_cb poll_cb, const char *path,
                        unsigned int interval);
int dm_uv_fs_poll_stop(dm_uv_fs_poll_st *handle);
int dm_uv_fs_poll_getpath(dm_uv_fs_poll_st *handle, char *buffer, size_t *size);
int dm_uv_signal_init(dm_uv_loop_st *loop, dm_uv_signal_st *handle);
int dm_uv_signal_start(dm_uv_signal_st *handle, dm_uv_signal_cb signal_cb, int signum);
int dm_uv_signal_start_oneshot(dm_uv_signal_st *handle, dm_uv_signal_cb signal_cb, int signum);
int dm_uv_signal_stop(dm_uv_signal_st *handle);
void dm_uv_loadavg(double avg[3]);
int dm_uv_fs_event_init(dm_uv_loop_st *loop, dm_uv_fs_event_st *handle);
int dm_uv_fs_event_start(dm_uv_fs_event_st *handle, dm_uv_fs_event_cb cb, const char *path,
                         unsigned int flags);
int dm_uv_fs_event_stop(dm_uv_fs_event_st *handle);
int dm_uv_fs_event_getpath(dm_uv_fs_event_st *handle, char *buffer, size_t *size);
int dm_uv_ip4_addr(const char *ip, int port, struct sockaddr_in *addr);
int dm_uv_ip6_addr(const char *ip, int port, struct sockaddr_in6 *addr);
int dm_uv_ip4_name(const struct sockaddr_in *src, char *dst, size_t size);
int dm_uv_ip6_name(const struct sockaddr_in6 *src, char *dst, size_t size);
int dm_uv_ip_name(const struct sockaddr *src, char *dst, size_t size);
int dm_uv_inet_ntop(int af, const void *src, char *dst, size_t size);
int dm_uv_inet_pton(int af, const char *src, void *dst);
int dm_uv_random(dm_uv_loop_st *loop, dm_uv_random_st *req, void *buf, size_t buflen,
                 unsigned flags, dm_uv_random_cb cb);
int dm_uv_if_indextoname(unsigned int ifindex, char *buffer, size_t *size);
int dm_uv_if_indextoiid(unsigned int ifindex, char *buffer, size_t *size);
int dm_uv_exepath(char *buffer, size_t *size);
int dm_uv_cwd(char *buffer, size_t *size);
int dm_uv_chdir(const char *dir);
uint64_t dm_uv_get_free_memory(void);
uint64_t dm_uv_get_total_memory(void);
uint64_t dm_uv_get_constrained_memory(void);
uint64_t dm_uv_get_available_memory(void);
int dm_uv_clock_gettime(dm_uv_clock_id_et clock_id, dm_uv_timespec64_st *ts);
uint64_t dm_uv_hrtime(void);
void dm_uv_sleep(unsigned int msec);
void dm_uv_disable_stdio_inheritance(void);
int dm_uv_dlopen(const char *filename, dm_uv_lib_st *lib);
void dm_uv_dlclose(dm_uv_lib_st *lib);
int dm_uv_dlsym(dm_uv_lib_st *lib, const char *name, void **ptr);
const char *dm_uv_dlerror(const dm_uv_lib_st *lib);
int dm_uv_mutex_init(dm_uv_mutex_t *handle);
int dm_uv_mutex_init_recursive(dm_uv_mutex_t *handle);
void dm_uv_mutex_destroy(dm_uv_mutex_t *handle);
void dm_uv_mutex_lock(dm_uv_mutex_t *handle);
int dm_uv_mutex_trylock(dm_uv_mutex_t *handle);
void dm_uv_mutex_unlock(dm_uv_mutex_t *handle);
int dm_uv_rwlock_init(dm_uv_rwlock_t *rwlock);
void dm_uv_rwlock_destroy(dm_uv_rwlock_t *rwlock);
void dm_uv_rwlock_rdlock(dm_uv_rwlock_t *rwlock);
int dm_uv_rwlock_tryrdlock(dm_uv_rwlock_t *rwlock);
void dm_uv_rwlock_rdunlock(dm_uv_rwlock_t *rwlock);
void dm_uv_rwlock_wrlock(dm_uv_rwlock_t *rwlock);
int dm_uv_rwlock_trywrlock(dm_uv_rwlock_t *rwlock);
void dm_uv_rwlock_wrunlock(dm_uv_rwlock_t *rwlock);
int dm_uv_sem_init(dm_uv_sem_t *sem, unsigned int value);
void dm_uv_sem_destroy(dm_uv_sem_t *sem);
void dm_uv_sem_post(dm_uv_sem_t *sem);
void dm_uv_sem_wait(dm_uv_sem_t *sem);
int dm_uv_sem_trywait(dm_uv_sem_t *sem);
int dm_uv_cond_init(dm_uv_cond_t *cond);
void dm_uv_cond_destroy(dm_uv_cond_t *cond);
void dm_uv_cond_signal(dm_uv_cond_t *cond);
void dm_uv_cond_broadcast(dm_uv_cond_t *cond);
int dm_uv_barrier_init(dm_uv_barrier_t *barrier, unsigned int count);
void dm_uv_barrier_destroy(dm_uv_barrier_t *barrier);
int dm_uv_barrier_wait(dm_uv_barrier_t *barrier);
void dm_uv_cond_wait(dm_uv_cond_t *cond, dm_uv_mutex_t *mutex);
int dm_uv_cond_timedwait(dm_uv_cond_t *cond, dm_uv_mutex_t *mutex, uint64_t timeout);
void dm_uv_once(dm_uv_once_t *guard, void (*callback)(void));
int dm_uv_key_create(dm_uv_key_t *key);
void dm_uv_key_delete(dm_uv_key_t *key);
void *dm_uv_key_get(dm_uv_key_t *key);
void dm_uv_key_set(dm_uv_key_t *key, void *value);
int dm_uv_gettimeofday(dm_uv_timeval64_st *tv);
int dm_uv_thread_create(dm_uv_thread_t *tid, dm_uv_thread_cb entry, void *arg);
int dm_uv_thread_detach(dm_uv_thread_t *tid);
int dm_uv_thread_create_ex(dm_uv_thread_t *tid, const dm_uv_thread_options_st *params,
                           dm_uv_thread_cb entry, void *arg);
int dm_uv_thread_setaffinity(dm_uv_thread_t *tid, char *cpumask, char *oldmask, size_t mask_size);
int dm_uv_thread_getaffinity(dm_uv_thread_t *tid, char *cpumask, size_t mask_size);
int dm_uv_thread_getcpu(void);
dm_uv_thread_t dm_uv_thread_self(void);
int dm_uv_thread_join(dm_uv_thread_t *tid);
int dm_uv_thread_equal(const dm_uv_thread_t *t1, const dm_uv_thread_t *t2);
int dm_uv_thread_setname(const char *name);
int dm_uv_thread_getname(dm_uv_thread_t *tid, char *name, size_t size);
void *dm_uv_loop_get_data(const dm_uv_loop_st *arg1);
void dm_uv_loop_set_data(dm_uv_loop_st *arg1, void *data);
size_t dm_uv_utf16_length_as_wtf8(const uint16_t *utf16, ssize_t utf16_len);
int dm_uv_utf16_to_wtf8(const uint16_t *utf16, ssize_t utf16_len, char **wtf8_ptr,
                        size_t *wtf8_len_ptr);
ssize_t dm_uv_wtf8_length_as_utf16(const char *wtf8);
void dm_uv_wtf8_to_utf16(const char *wtf8, uint16_t *utf16, size_t utf16_len);
int dm_uv_loop_configure(dm_uv_loop_st *loop, dm_uv_loop_option_et option, ...);

#ifdef __cplusplus
}
#endif

#endif /* DM_UV_ADAPTER_H */
