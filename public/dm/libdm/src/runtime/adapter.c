/* 由 tool/gen_dm_adapters.js 自动生成 —— 请勿手动修改。 */
/* 升级 libuv 后重新生成:  node tool/gen_dm_adapters.js */
/* 本文件保存全部生成的封装;runtime/dm.c 保存手写自有 API 与 dm_os_environ */
/* 变参函数(C 无法前向传递 `...`)不在此生成,由 dm.c 手工适配:
 *   uv_loop_configure
 */
/* 以下函数由 dm.c 手工实现(生成器只保留其类型别名,不生成直通函数):
 *   uv_replace_allocator, uv_os_environ, uv_os_free_environ
 */

#include <dm.h>
#include "../uv_adapter.h"

DM_API unsigned int dm_uv_version(void)
{
  return uv_version();
}
DM_API const char *dm_uv_version_string(void)
{
  return uv_version_string();
}
DM_API void dm_library_shutdown(void)
{
  uv_library_shutdown();
}
DM_API dm_loop_st *dm_default_loop(void)
{
  return uv_default_loop();
}
DM_API int dm_loop_init(dm_loop_st *loop)
{
  return uv_loop_init(loop);
}
DM_API int dm_loop_close(dm_loop_st *loop)
{
  return uv_loop_close(loop);
}
DM_API dm_loop_st *dm_loop_new(void)
{
  return uv_loop_new();
}
DM_API void dm_loop_delete(dm_loop_st *arg1)
{
  uv_loop_delete(arg1);
}
DM_API size_t dm_loop_size(void)
{
  return uv_loop_size();
}
DM_API int dm_loop_alive(const dm_loop_st *loop)
{
  return uv_loop_alive(loop);
}
DM_API int dm_loop_fork(dm_loop_st *loop)
{
  return uv_loop_fork(loop);
}
DM_API int dm_run(dm_loop_st *arg1, dm_run_mode_et mode)
{
  return uv_run(arg1, mode);
}
DM_API void dm_stop(dm_loop_st *arg1)
{
  uv_stop(arg1);
}
DM_API void dm_ref(dm_handle_st *arg1)
{
  uv_ref(arg1);
}
DM_API void dm_unref(dm_handle_st *arg1)
{
  uv_unref(arg1);
}
DM_API int dm_has_ref(const dm_handle_st *arg1)
{
  return uv_has_ref(arg1);
}
DM_API void dm_update_time(dm_loop_st *arg1)
{
  uv_update_time(arg1);
}
DM_API uint64_t dm_now(const dm_loop_st *arg1)
{
  return uv_now(arg1);
}
DM_API int dm_backend_fd(const dm_loop_st *arg1)
{
  return uv_backend_fd(arg1);
}
DM_API int dm_backend_timeout(const dm_loop_st *arg1)
{
  return uv_backend_timeout(arg1);
}
DM_API int dm_translate_sys_error(int sys_errno)
{
  return uv_translate_sys_error(sys_errno);
}
DM_API const char *dm_strerror(int err)
{
  return uv_strerror(err);
}
DM_API char *dm_strerror_r(int err, char *buf, size_t buflen)
{
  return uv_strerror_r(err, buf, buflen);
}
DM_API const char *dm_err_name(int err)
{
  return uv_err_name(err);
}
DM_API char *dm_err_name_r(int err, char *buf, size_t buflen)
{
  return uv_err_name_r(err, buf, buflen);
}
DM_API int dm_shutdown(dm_shutdown_st *req, dm_stream_st *handle, dm_shutdown_cb cb)
{
  return uv_shutdown(req, handle, cb);
}
DM_API size_t dm_handle_size(dm_handle_type_et type)
{
  return uv_handle_size(type);
}
DM_API dm_handle_type_et dm_handle_get_type(const dm_handle_st *handle)
{
  return uv_handle_get_type(handle);
}
DM_API const char *dm_handle_type_name(dm_handle_type_et type)
{
  return uv_handle_type_name(type);
}
DM_API void *dm_handle_get_data(const dm_handle_st *handle)
{
  return uv_handle_get_data(handle);
}
DM_API dm_loop_st *dm_handle_get_loop(const dm_handle_st *handle)
{
  return uv_handle_get_loop(handle);
}
DM_API void dm_handle_set_data(dm_handle_st *handle, void *data)
{
  uv_handle_set_data(handle, data);
}
DM_API size_t dm_req_size(dm_req_type_et type)
{
  return uv_req_size(type);
}
DM_API void *dm_req_get_data(const dm_req_st *req)
{
  return uv_req_get_data(req);
}
DM_API void dm_req_set_data(dm_req_st *req, void *data)
{
  uv_req_set_data(req, data);
}
DM_API dm_req_type_et dm_req_get_type(const dm_req_st *req)
{
  return uv_req_get_type(req);
}
DM_API const char *dm_req_type_name(dm_req_type_et type)
{
  return uv_req_type_name(type);
}
DM_API int dm_is_active(const dm_handle_st *handle)
{
  return uv_is_active(handle);
}
DM_API void dm_walk(dm_loop_st *loop, dm_walk_cb walk_cb, void *arg)
{
  uv_walk(loop, walk_cb, arg);
}
DM_API void dm_print_all_handles(dm_loop_st *loop, FILE *stream)
{
  uv_print_all_handles(loop, stream);
}
DM_API void dm_print_active_handles(dm_loop_st *loop, FILE *stream)
{
  uv_print_active_handles(loop, stream);
}
DM_API void dm_close(dm_handle_st *handle, dm_close_cb close_cb)
{
  uv_close(handle, close_cb);
}
DM_API int dm_send_buffer_size(dm_handle_st *handle, int *value)
{
  return uv_send_buffer_size(handle, value);
}
DM_API int dm_recv_buffer_size(dm_handle_st *handle, int *value)
{
  return uv_recv_buffer_size(handle, value);
}
DM_API int dm_fileno(const dm_handle_st *handle, dm_os_fd_t *fd)
{
  return uv_fileno(handle, fd);
}
DM_API dm_buf_st dm_buf_init(char *base, unsigned int len)
{
  return uv_buf_init(base, len);
}
DM_API int dm_pipe(dm_file fds[2], int read_flags, int write_flags)
{
  return uv_pipe(fds, read_flags, write_flags);
}
DM_API int dm_socketpair(int type, int protocol, dm_os_sock_t socket_vector[2], int flags0,
                         int flags1)
{
  return uv_socketpair(type, protocol, socket_vector, flags0, flags1);
}
DM_API size_t dm_stream_get_write_queue_size(const dm_stream_st *stream)
{
  return uv_stream_get_write_queue_size(stream);
}
DM_API int dm_listen(dm_stream_st *stream, int backlog, dm_connection_cb cb)
{
  return uv_listen(stream, backlog, cb);
}
DM_API int dm_accept(dm_stream_st *server, dm_stream_st *client)
{
  return uv_accept(server, client);
}
DM_API int dm_read_start(dm_stream_st *arg1, dm_alloc_cb alloc_cb, dm_read_cb read_cb)
{
  return uv_read_start(arg1, alloc_cb, read_cb);
}
DM_API int dm_read_stop(dm_stream_st *arg1)
{
  return uv_read_stop(arg1);
}
DM_API int dm_write(dm_write_st *req, dm_stream_st *handle, const dm_buf_st bufs[],
                    unsigned int nbufs, dm_write_cb cb)
{
  return uv_write(req, handle, bufs, nbufs, cb);
}
DM_API int dm_write2(dm_write_st *req, dm_stream_st *handle, const dm_buf_st bufs[],
                     unsigned int nbufs, dm_stream_st *send_handle, dm_write_cb cb)
{
  return uv_write2(req, handle, bufs, nbufs, send_handle, cb);
}
DM_API int dm_try_write(dm_stream_st *handle, const dm_buf_st bufs[], unsigned int nbufs)
{
  return uv_try_write(handle, bufs, nbufs);
}
DM_API int dm_try_write2(dm_stream_st *handle, const dm_buf_st bufs[], unsigned int nbufs,
                         dm_stream_st *send_handle)
{
  return uv_try_write2(handle, bufs, nbufs, send_handle);
}
DM_API size_t dm_write_nwritten(const dm_write_st *req)
{
  return uv_write_nwritten(req);
}
DM_API int dm_is_readable(const dm_stream_st *handle)
{
  return uv_is_readable(handle);
}
DM_API int dm_is_writable(const dm_stream_st *handle)
{
  return uv_is_writable(handle);
}
DM_API int dm_stream_set_blocking(dm_stream_st *handle, int blocking)
{
  return uv_stream_set_blocking(handle, blocking);
}
DM_API int dm_is_closing(const dm_handle_st *handle)
{
  return uv_is_closing(handle);
}
DM_API int dm_tcp_init(dm_loop_st *arg1, dm_tcp_st *handle)
{
  return uv_tcp_init(arg1, handle);
}
DM_API int dm_tcp_init_ex(dm_loop_st *arg1, dm_tcp_st *handle, unsigned int flags)
{
  return uv_tcp_init_ex(arg1, handle, flags);
}
DM_API int dm_tcp_open(dm_tcp_st *handle, dm_os_sock_t sock)
{
  return uv_tcp_open(handle, sock);
}
DM_API int dm_tcp_nodelay(dm_tcp_st *handle, int enable)
{
  return uv_tcp_nodelay(handle, enable);
}
DM_API int dm_tcp_keepalive(dm_tcp_st *handle, int enable, unsigned int delay)
{
  return uv_tcp_keepalive(handle, enable, delay);
}
DM_API int dm_tcp_keepalive_ex(dm_tcp_st *handle, int on, unsigned int idle, unsigned int intvl,
                               unsigned int cnt)
{
  return uv_tcp_keepalive_ex(handle, on, idle, intvl, cnt);
}
DM_API int dm_tcp_simultaneous_accepts(dm_tcp_st *handle, int enable)
{
  return uv_tcp_simultaneous_accepts(handle, enable);
}
DM_API int dm_tcp_bind(dm_tcp_st *handle, const struct sockaddr *addr, unsigned int flags)
{
  return uv_tcp_bind(handle, addr, flags);
}
DM_API int dm_tcp_getsockname(const dm_tcp_st *handle, struct sockaddr *name, int *namelen)
{
  return uv_tcp_getsockname(handle, name, namelen);
}
DM_API int dm_tcp_getpeername(const dm_tcp_st *handle, struct sockaddr *name, int *namelen)
{
  return uv_tcp_getpeername(handle, name, namelen);
}
DM_API int dm_tcp_close_reset(dm_tcp_st *handle, dm_close_cb close_cb)
{
  return uv_tcp_close_reset(handle, close_cb);
}
DM_API int dm_tcp_connect(dm_connect_st *req, dm_tcp_st *handle, const struct sockaddr *addr,
                          dm_connect_cb cb)
{
  return uv_tcp_connect(req, handle, addr, cb);
}
DM_API int dm_udp_init(dm_loop_st *arg1, dm_udp_st *handle)
{
  return uv_udp_init(arg1, handle);
}
DM_API int dm_udp_init_ex(dm_loop_st *arg1, dm_udp_st *handle, unsigned int flags)
{
  return uv_udp_init_ex(arg1, handle, flags);
}
DM_API int dm_udp_open(dm_udp_st *handle, dm_os_sock_t sock)
{
  return uv_udp_open(handle, sock);
}
DM_API int dm_udp_open_ex(dm_udp_st *handle, dm_os_sock_t sock, unsigned int flags)
{
  return uv_udp_open_ex(handle, sock, flags);
}
DM_API int dm_udp_bind(dm_udp_st *handle, const struct sockaddr *addr, unsigned int flags)
{
  return uv_udp_bind(handle, addr, flags);
}
DM_API int dm_udp_connect(dm_udp_st *handle, const struct sockaddr *addr)
{
  return uv_udp_connect(handle, addr);
}
DM_API int dm_udp_getpeername(const dm_udp_st *handle, struct sockaddr *name, int *namelen)
{
  return uv_udp_getpeername(handle, name, namelen);
}
DM_API int dm_udp_getsockname(const dm_udp_st *handle, struct sockaddr *name, int *namelen)
{
  return uv_udp_getsockname(handle, name, namelen);
}
DM_API int dm_udp_set_membership(dm_udp_st *handle, const char *multicast_addr,
                                 const char *interface_addr, dm_membership_et membership)
{
  return uv_udp_set_membership(handle, multicast_addr, interface_addr, membership);
}
DM_API int dm_udp_set_source_membership(dm_udp_st *handle, const char *multicast_addr,
                                        const char *interface_addr, const char *source_addr,
                                        dm_membership_et membership)
{
  return uv_udp_set_source_membership(handle, multicast_addr, interface_addr, source_addr,
                                      membership);
}
DM_API int dm_udp_set_multicast_loop(dm_udp_st *handle, int on)
{
  return uv_udp_set_multicast_loop(handle, on);
}
DM_API int dm_udp_set_multicast_ttl(dm_udp_st *handle, int ttl)
{
  return uv_udp_set_multicast_ttl(handle, ttl);
}
DM_API int dm_udp_set_multicast_interface(dm_udp_st *handle, const char *interface_addr)
{
  return uv_udp_set_multicast_interface(handle, interface_addr);
}
DM_API int dm_udp_set_broadcast(dm_udp_st *handle, int on)
{
  return uv_udp_set_broadcast(handle, on);
}
DM_API int dm_udp_set_ttl(dm_udp_st *handle, int ttl)
{
  return uv_udp_set_ttl(handle, ttl);
}
DM_API int dm_udp_send(dm_udp_send_st *req, dm_udp_st *handle, const dm_buf_st bufs[],
                       unsigned int nbufs, const struct sockaddr *addr, dm_udp_send_cb send_cb)
{
  return uv_udp_send(req, handle, bufs, nbufs, addr, send_cb);
}
DM_API int dm_udp_try_send(dm_udp_st *handle, const dm_buf_st bufs[], unsigned int nbufs,
                           const struct sockaddr *addr)
{
  return uv_udp_try_send(handle, bufs, nbufs, addr);
}
DM_API int dm_udp_try_send2(dm_udp_st *handle, unsigned int count, dm_buf_st *bufs[],
                            unsigned int nbufs[], struct sockaddr *addrs[], unsigned int flags)
{
  return uv_udp_try_send2(handle, count, bufs, nbufs, addrs, flags);
}
DM_API int dm_udp_recv_start(dm_udp_st *handle, dm_alloc_cb alloc_cb, dm_udp_recv_cb recv_cb)
{
  return uv_udp_recv_start(handle, alloc_cb, recv_cb);
}
DM_API int dm_udp_using_recvmmsg(const dm_udp_st *handle)
{
  return uv_udp_using_recvmmsg(handle);
}
DM_API int dm_udp_recv_stop(dm_udp_st *handle)
{
  return uv_udp_recv_stop(handle);
}
DM_API size_t dm_udp_get_send_queue_size(const dm_udp_st *handle)
{
  return uv_udp_get_send_queue_size(handle);
}
DM_API size_t dm_udp_get_send_queue_count(const dm_udp_st *handle)
{
  return uv_udp_get_send_queue_count(handle);
}
DM_API int dm_tty_init(dm_loop_st *arg1, dm_tty_st *arg2, dm_file fd, int readable)
{
  return uv_tty_init(arg1, arg2, fd, readable);
}
DM_API int dm_tty_set_mode(dm_tty_st *arg1, dm_tty_mode_et mode)
{
  return uv_tty_set_mode(arg1, mode);
}
DM_API int dm_tty_reset_mode(void)
{
  return uv_tty_reset_mode();
}
DM_API int dm_tty_get_winsize(dm_tty_st *arg1, int *width, int *height)
{
  return uv_tty_get_winsize(arg1, width, height);
}
DM_API void dm_tty_set_vterm_state(dm_tty_vtermstate_et state)
{
  uv_tty_set_vterm_state(state);
}
DM_API int dm_tty_get_vterm_state(dm_tty_vtermstate_et *state)
{
  return uv_tty_get_vterm_state(state);
}
DM_API dm_handle_type_et dm_guess_handle(dm_file file)
{
  return uv_guess_handle(file);
}
DM_API int dm_pipe_init(dm_loop_st *arg1, dm_pipe_st *handle, int ipc)
{
  return uv_pipe_init(arg1, handle, ipc);
}
DM_API int dm_pipe_open(dm_pipe_st *arg1, dm_file file)
{
  return uv_pipe_open(arg1, file);
}
DM_API int dm_pipe_bind(dm_pipe_st *handle, const char *name)
{
  return uv_pipe_bind(handle, name);
}
DM_API int dm_pipe_bind2(dm_pipe_st *handle, const char *name, size_t namelen, unsigned int flags)
{
  return uv_pipe_bind2(handle, name, namelen, flags);
}
DM_API void dm_pipe_connect(dm_connect_st *req, dm_pipe_st *handle, const char *name,
                            dm_connect_cb cb)
{
  uv_pipe_connect(req, handle, name, cb);
}
DM_API int dm_pipe_connect2(dm_connect_st *req, dm_pipe_st *handle, const char *name,
                            size_t namelen, unsigned int flags, dm_connect_cb cb)
{
  return uv_pipe_connect2(req, handle, name, namelen, flags, cb);
}
DM_API int dm_pipe_getsockname(const dm_pipe_st *handle, char *buffer, size_t *size)
{
  return uv_pipe_getsockname(handle, buffer, size);
}
DM_API int dm_pipe_getpeername(const dm_pipe_st *handle, char *buffer, size_t *size)
{
  return uv_pipe_getpeername(handle, buffer, size);
}
DM_API void dm_pipe_pending_instances(dm_pipe_st *handle, int count)
{
  uv_pipe_pending_instances(handle, count);
}
DM_API int dm_pipe_pending_count(dm_pipe_st *handle)
{
  return uv_pipe_pending_count(handle);
}
DM_API dm_handle_type_et dm_pipe_pending_type(dm_pipe_st *handle)
{
  return uv_pipe_pending_type(handle);
}
DM_API int dm_pipe_chmod(dm_pipe_st *handle, int flags)
{
  return uv_pipe_chmod(handle, flags);
}
DM_API int dm_poll_init(dm_loop_st *loop, dm_poll_st *handle, int fd)
{
  return uv_poll_init(loop, handle, fd);
}
DM_API int dm_poll_init_socket(dm_loop_st *loop, dm_poll_st *handle, dm_os_sock_t socket)
{
  return uv_poll_init_socket(loop, handle, socket);
}
DM_API int dm_poll_start(dm_poll_st *handle, int events, dm_poll_cb cb)
{
  return uv_poll_start(handle, events, cb);
}
DM_API int dm_poll_stop(dm_poll_st *handle)
{
  return uv_poll_stop(handle);
}
DM_API int dm_prepare_init(dm_loop_st *arg1, dm_prepare_st *prepare)
{
  return uv_prepare_init(arg1, prepare);
}
DM_API int dm_prepare_start(dm_prepare_st *prepare, dm_prepare_cb cb)
{
  return uv_prepare_start(prepare, cb);
}
DM_API int dm_prepare_stop(dm_prepare_st *prepare)
{
  return uv_prepare_stop(prepare);
}
DM_API int dm_check_init(dm_loop_st *arg1, dm_check_st *check)
{
  return uv_check_init(arg1, check);
}
DM_API int dm_check_start(dm_check_st *check, dm_check_cb cb)
{
  return uv_check_start(check, cb);
}
DM_API int dm_check_stop(dm_check_st *check)
{
  return uv_check_stop(check);
}
DM_API int dm_idle_init(dm_loop_st *arg1, dm_idle_st *idle)
{
  return uv_idle_init(arg1, idle);
}
DM_API int dm_idle_start(dm_idle_st *idle, dm_idle_cb cb)
{
  return uv_idle_start(idle, cb);
}
DM_API int dm_idle_stop(dm_idle_st *idle)
{
  return uv_idle_stop(idle);
}
DM_API int dm_async_init(dm_loop_st *arg1, dm_async_st *async, dm_async_cb async_cb)
{
  return uv_async_init(arg1, async, async_cb);
}
DM_API int dm_async_send(dm_async_st *async)
{
  return uv_async_send(async);
}
DM_API int dm_timer_init(dm_loop_st *arg1, dm_timer_st *handle)
{
  return uv_timer_init(arg1, handle);
}
DM_API int dm_timer_start(dm_timer_st *handle, dm_timer_cb cb, uint64_t timeout, uint64_t repeat)
{
  return uv_timer_start(handle, cb, timeout, repeat);
}
DM_API int dm_timer_stop(dm_timer_st *handle)
{
  return uv_timer_stop(handle);
}
DM_API int dm_timer_again(dm_timer_st *handle)
{
  return uv_timer_again(handle);
}
DM_API void dm_timer_set_repeat(dm_timer_st *handle, uint64_t repeat)
{
  uv_timer_set_repeat(handle, repeat);
}
DM_API uint64_t dm_timer_get_repeat(const dm_timer_st *handle)
{
  return uv_timer_get_repeat(handle);
}
DM_API uint64_t dm_timer_get_due_in(const dm_timer_st *handle)
{
  return uv_timer_get_due_in(handle);
}
DM_API int dm_getaddrinfo(dm_loop_st *loop, dm_getaddrinfo_st *req,
                          dm_getaddrinfo_cb getaddrinfo_cb, const char *node, const char *service,
                          const struct addrinfo *hints)
{
  return uv_getaddrinfo(loop, req, getaddrinfo_cb, node, service, hints);
}
DM_API void dm_freeaddrinfo(struct addrinfo *ai)
{
  uv_freeaddrinfo(ai);
}
DM_API int dm_getnameinfo(dm_loop_st *loop, dm_getnameinfo_st *req,
                          dm_getnameinfo_cb getnameinfo_cb, const struct sockaddr *addr, int flags)
{
  return uv_getnameinfo(loop, req, getnameinfo_cb, addr, flags);
}
DM_API int dm_spawn(dm_loop_st *loop, dm_process_st *handle, const dm_process_options_st *options)
{
  return uv_spawn(loop, handle, options);
}
DM_API int dm_process_kill(dm_process_st *arg1, int signum)
{
  return uv_process_kill(arg1, signum);
}
DM_API int dm_kill(int pid, int signum)
{
  return uv_kill(pid, signum);
}
DM_API dm_pid_t dm_process_get_pid(const dm_process_st *arg1)
{
  return uv_process_get_pid(arg1);
}
DM_API int dm_queue_work(dm_loop_st *loop, dm_work_st *req, dm_work_cb work_cb,
                         dm_after_work_cb after_work_cb)
{
  return uv_queue_work(loop, req, work_cb, after_work_cb);
}
DM_API int dm_cancel(dm_req_st *req)
{
  return uv_cancel(req);
}
DM_API char **dm_setup_args(int argc, char **argv)
{
  return uv_setup_args(argc, argv);
}
DM_API int dm_get_process_title(char *buffer, size_t size)
{
  return uv_get_process_title(buffer, size);
}
DM_API int dm_set_process_title(const char *title)
{
  return uv_set_process_title(title);
}
DM_API int dm_resident_set_memory(size_t *rss)
{
  return uv_resident_set_memory(rss);
}
DM_API int dm_uptime(double *uptime)
{
  return uv_uptime(uptime);
}
DM_API dm_os_fd_t dm_get_osfhandle(int fd)
{
  return uv_get_osfhandle(fd);
}
DM_API int dm_open_osfhandle(dm_os_fd_t os_fd)
{
  return uv_open_osfhandle(os_fd);
}
DM_API int dm_getrusage(dm_rusage_st *rusage)
{
  return uv_getrusage(rusage);
}
DM_API int dm_getrusage_thread(dm_rusage_st *rusage)
{
  return uv_getrusage_thread(rusage);
}
DM_API int dm_os_homedir(char *buffer, size_t *size)
{
  return uv_os_homedir(buffer, size);
}
DM_API int dm_os_tmpdir(char *buffer, size_t *size)
{
  return uv_os_tmpdir(buffer, size);
}
DM_API int dm_os_get_passwd(dm_passwd_st *pwd)
{
  return uv_os_get_passwd(pwd);
}
DM_API void dm_os_free_passwd(dm_passwd_st *pwd)
{
  uv_os_free_passwd(pwd);
}
DM_API int dm_os_get_passwd2(dm_passwd_st *pwd, dm_uid_t uid)
{
  return uv_os_get_passwd2(pwd, uid);
}
DM_API int dm_os_get_group(dm_group_st *grp, dm_uid_t gid)
{
  return uv_os_get_group(grp, gid);
}
DM_API void dm_os_free_group(dm_group_st *grp)
{
  uv_os_free_group(grp);
}
DM_API dm_pid_t dm_os_getpid(void)
{
  return uv_os_getpid();
}
DM_API dm_pid_t dm_os_getppid(void)
{
#ifdef _WIN32
  /* 确保 libuv 初始化 pNtQueryInformationProcess（W1 Win 修复） */
  (void)uv_hrtime();
#endif
  return uv_os_getppid();
}
DM_API int dm_os_getpriority(dm_pid_t pid, int *priority)
{
  return uv_os_getpriority(pid, priority);
}
DM_API int dm_os_setpriority(dm_pid_t pid, int priority)
{
  return uv_os_setpriority(pid, priority);
}
DM_API int dm_thread_getpriority(dm_thread_t tid, int *priority)
{
  return uv_thread_getpriority(tid, priority);
}
DM_API int dm_thread_setpriority(dm_thread_t tid, int priority)
{
  return uv_thread_setpriority(tid, priority);
}
DM_API unsigned int dm_available_parallelism(void)
{
  return uv_available_parallelism();
}
DM_API int dm_cpu_info(dm_cpu_info_st **cpu_infos, int *count)
{
  return uv_cpu_info(cpu_infos, count);
}
DM_API void dm_free_cpu_info(dm_cpu_info_st *cpu_infos, int count)
{
  uv_free_cpu_info(cpu_infos, count);
}
DM_API int dm_cpumask_size(void)
{
  return uv_cpumask_size();
}
DM_API int dm_interface_addresses(dm_interface_address_st **addresses, int *count)
{
  return uv_interface_addresses(addresses, count);
}
DM_API void dm_free_interface_addresses(dm_interface_address_st *addresses, int count)
{
  uv_free_interface_addresses(addresses, count);
}
DM_API int dm_os_getenv(const char *name, char *buffer, size_t *size)
{
  return uv_os_getenv(name, buffer, size);
}
DM_API int dm_os_setenv(const char *name, const char *value)
{
  return uv_os_setenv(name, value);
}
DM_API int dm_os_unsetenv(const char *name)
{
  return uv_os_unsetenv(name);
}
DM_API int dm_os_gethostname(char *buffer, size_t *size)
{
  return uv_os_gethostname(buffer, size);
}
DM_API int dm_os_uname(dm_utsname_st *buffer)
{
  return uv_os_uname(buffer);
}
DM_API int dm_metrics_info(dm_loop_st *loop, dm_metrics_st *metrics)
{
  return uv_metrics_info(loop, metrics);
}
DM_API uint64_t dm_metrics_idle_time(dm_loop_st *loop)
{
  return uv_metrics_idle_time(loop);
}
DM_API dm_fs_type_et dm_fs_get_type(const dm_fs_st *arg1)
{
  return uv_fs_get_type(arg1);
}
DM_API ssize_t dm_fs_get_result(const dm_fs_st *arg1)
{
  return uv_fs_get_result(arg1);
}
DM_API int dm_fs_get_system_error(const dm_fs_st *arg1)
{
  return uv_fs_get_system_error(arg1);
}
DM_API void *dm_fs_get_ptr(const dm_fs_st *arg1)
{
  return uv_fs_get_ptr(arg1);
}
DM_API const char *dm_fs_get_path(const dm_fs_st *arg1)
{
  return uv_fs_get_path(arg1);
}
DM_API dm_stat_st *dm_fs_get_statbuf(dm_fs_st *arg1)
{
  return uv_fs_get_statbuf(arg1);
}
DM_API void dm_fs_req_cleanup(dm_fs_st *req)
{
  uv_fs_req_cleanup(req);
}
DM_API int dm_fs_close(dm_loop_st *loop, dm_fs_st *req, dm_file file, dm_fs_cb cb)
{
  return uv_fs_close(loop, req, file, cb);
}
DM_API int dm_fs_open(dm_loop_st *loop, dm_fs_st *req, const char *path, int flags, int mode,
                      dm_fs_cb cb)
{
  return uv_fs_open(loop, req, path, flags, mode, cb);
}
DM_API int dm_fs_read(dm_loop_st *loop, dm_fs_st *req, dm_file file, const dm_buf_st bufs[],
                      unsigned int nbufs, int64_t offset, dm_fs_cb cb)
{
  return uv_fs_read(loop, req, file, bufs, nbufs, offset, cb);
}
DM_API int dm_fs_unlink(dm_loop_st *loop, dm_fs_st *req, const char *path, dm_fs_cb cb)
{
  return uv_fs_unlink(loop, req, path, cb);
}
DM_API int dm_fs_write(dm_loop_st *loop, dm_fs_st *req, dm_file file, const dm_buf_st bufs[],
                       unsigned int nbufs, int64_t offset, dm_fs_cb cb)
{
  return uv_fs_write(loop, req, file, bufs, nbufs, offset, cb);
}
DM_API int dm_fs_copyfile(dm_loop_st *loop, dm_fs_st *req, const char *path, const char *new_path,
                          int flags, dm_fs_cb cb)
{
  return uv_fs_copyfile(loop, req, path, new_path, flags, cb);
}
DM_API int dm_fs_mkdir(dm_loop_st *loop, dm_fs_st *req, const char *path, int mode, dm_fs_cb cb)
{
  return uv_fs_mkdir(loop, req, path, mode, cb);
}
DM_API int dm_fs_mkdtemp(dm_loop_st *loop, dm_fs_st *req, const char *tpl, dm_fs_cb cb)
{
  return uv_fs_mkdtemp(loop, req, tpl, cb);
}
DM_API int dm_fs_mkstemp(dm_loop_st *loop, dm_fs_st *req, const char *tpl, dm_fs_cb cb)
{
  return uv_fs_mkstemp(loop, req, tpl, cb);
}
DM_API int dm_fs_rmdir(dm_loop_st *loop, dm_fs_st *req, const char *path, dm_fs_cb cb)
{
  return uv_fs_rmdir(loop, req, path, cb);
}
DM_API int dm_fs_scandir(dm_loop_st *loop, dm_fs_st *req, const char *path, int flags, dm_fs_cb cb)
{
  return uv_fs_scandir(loop, req, path, flags, cb);
}
DM_API int dm_fs_scandir_next(dm_fs_st *req, dm_dirent_st *ent)
{
  return uv_fs_scandir_next(req, ent);
}
DM_API int dm_fs_opendir(dm_loop_st *loop, dm_fs_st *req, const char *path, dm_fs_cb cb)
{
  return uv_fs_opendir(loop, req, path, cb);
}
DM_API int dm_fs_readdir(dm_loop_st *loop, dm_fs_st *req, dm_dir_st *dir, dm_fs_cb cb)
{
  return uv_fs_readdir(loop, req, dir, cb);
}
DM_API int dm_fs_closedir(dm_loop_st *loop, dm_fs_st *req, dm_dir_st *dir, dm_fs_cb cb)
{
  return uv_fs_closedir(loop, req, dir, cb);
}
DM_API int dm_fs_stat(dm_loop_st *loop, dm_fs_st *req, const char *path, dm_fs_cb cb)
{
  return uv_fs_stat(loop, req, path, cb);
}
DM_API int dm_fs_fstat(dm_loop_st *loop, dm_fs_st *req, dm_file file, dm_fs_cb cb)
{
  return uv_fs_fstat(loop, req, file, cb);
}
DM_API int dm_fs_rename(dm_loop_st *loop, dm_fs_st *req, const char *path, const char *new_path,
                        dm_fs_cb cb)
{
  return uv_fs_rename(loop, req, path, new_path, cb);
}
DM_API int dm_fs_fsync(dm_loop_st *loop, dm_fs_st *req, dm_file file, dm_fs_cb cb)
{
  return uv_fs_fsync(loop, req, file, cb);
}
DM_API int dm_fs_fdatasync(dm_loop_st *loop, dm_fs_st *req, dm_file file, dm_fs_cb cb)
{
  return uv_fs_fdatasync(loop, req, file, cb);
}
DM_API int dm_fs_ftruncate(dm_loop_st *loop, dm_fs_st *req, dm_file file, int64_t offset,
                           dm_fs_cb cb)
{
  return uv_fs_ftruncate(loop, req, file, offset, cb);
}
DM_API int dm_fs_sendfile(dm_loop_st *loop, dm_fs_st *req, dm_file out_fd, dm_file in_fd,
                          int64_t in_offset, size_t length, dm_fs_cb cb)
{
  return uv_fs_sendfile(loop, req, out_fd, in_fd, in_offset, length, cb);
}
DM_API int dm_fs_access(dm_loop_st *loop, dm_fs_st *req, const char *path, int mode, dm_fs_cb cb)
{
  return uv_fs_access(loop, req, path, mode, cb);
}
DM_API int dm_fs_chmod(dm_loop_st *loop, dm_fs_st *req, const char *path, int mode, dm_fs_cb cb)
{
  return uv_fs_chmod(loop, req, path, mode, cb);
}
DM_API int dm_fs_utime(dm_loop_st *loop, dm_fs_st *req, const char *path, double atime,
                       double mtime, dm_fs_cb cb)
{
  return uv_fs_utime(loop, req, path, atime, mtime, cb);
}
DM_API int dm_fs_futime(dm_loop_st *loop, dm_fs_st *req, dm_file file, double atime, double mtime,
                        dm_fs_cb cb)
{
  return uv_fs_futime(loop, req, file, atime, mtime, cb);
}
DM_API int dm_fs_lutime(dm_loop_st *loop, dm_fs_st *req, const char *path, double atime,
                        double mtime, dm_fs_cb cb)
{
  return uv_fs_lutime(loop, req, path, atime, mtime, cb);
}
DM_API int dm_fs_lstat(dm_loop_st *loop, dm_fs_st *req, const char *path, dm_fs_cb cb)
{
  return uv_fs_lstat(loop, req, path, cb);
}
DM_API int dm_fs_link(dm_loop_st *loop, dm_fs_st *req, const char *path, const char *new_path,
                      dm_fs_cb cb)
{
  return uv_fs_link(loop, req, path, new_path, cb);
}
DM_API int dm_fs_symlink(dm_loop_st *loop, dm_fs_st *req, const char *path, const char *new_path,
                         int flags, dm_fs_cb cb)
{
  return uv_fs_symlink(loop, req, path, new_path, flags, cb);
}
DM_API int dm_fs_readlink(dm_loop_st *loop, dm_fs_st *req, const char *path, dm_fs_cb cb)
{
  return uv_fs_readlink(loop, req, path, cb);
}
DM_API int dm_fs_realpath(dm_loop_st *loop, dm_fs_st *req, const char *path, dm_fs_cb cb)
{
  return uv_fs_realpath(loop, req, path, cb);
}
DM_API int dm_fs_fchmod(dm_loop_st *loop, dm_fs_st *req, dm_file file, int mode, dm_fs_cb cb)
{
  return uv_fs_fchmod(loop, req, file, mode, cb);
}
DM_API int dm_fs_chown(dm_loop_st *loop, dm_fs_st *req, const char *path, dm_uid_t uid,
                       dm_gid_t gid, dm_fs_cb cb)
{
  return uv_fs_chown(loop, req, path, uid, gid, cb);
}
DM_API int dm_fs_fchown(dm_loop_st *loop, dm_fs_st *req, dm_file file, dm_uid_t uid, dm_gid_t gid,
                        dm_fs_cb cb)
{
  return uv_fs_fchown(loop, req, file, uid, gid, cb);
}
DM_API int dm_fs_lchown(dm_loop_st *loop, dm_fs_st *req, const char *path, dm_uid_t uid,
                        dm_gid_t gid, dm_fs_cb cb)
{
  return uv_fs_lchown(loop, req, path, uid, gid, cb);
}
DM_API int dm_fs_statfs(dm_loop_st *loop, dm_fs_st *req, const char *path, dm_fs_cb cb)
{
  return uv_fs_statfs(loop, req, path, cb);
}
DM_API int dm_fs_poll_init(dm_loop_st *loop, dm_fs_poll_st *handle)
{
  return uv_fs_poll_init(loop, handle);
}
DM_API int dm_fs_poll_start(dm_fs_poll_st *handle, dm_fs_poll_cb poll_cb, const char *path,
                            unsigned int interval)
{
  return uv_fs_poll_start(handle, poll_cb, path, interval);
}
DM_API int dm_fs_poll_stop(dm_fs_poll_st *handle)
{
  return uv_fs_poll_stop(handle);
}
DM_API int dm_fs_poll_getpath(dm_fs_poll_st *handle, char *buffer, size_t *size)
{
  return uv_fs_poll_getpath(handle, buffer, size);
}
DM_API int dm_signal_init(dm_loop_st *loop, dm_signal_st *handle)
{
  return uv_signal_init(loop, handle);
}
DM_API int dm_signal_start(dm_signal_st *handle, dm_signal_cb signal_cb, int signum)
{
  return uv_signal_start(handle, signal_cb, signum);
}
DM_API int dm_signal_start_oneshot(dm_signal_st *handle, dm_signal_cb signal_cb, int signum)
{
  return uv_signal_start_oneshot(handle, signal_cb, signum);
}
DM_API int dm_signal_stop(dm_signal_st *handle)
{
  return uv_signal_stop(handle);
}
DM_API void dm_loadavg(double avg[3])
{
  uv_loadavg(avg);
}
DM_API int dm_fs_event_init(dm_loop_st *loop, dm_fs_event_st *handle)
{
  return uv_fs_event_init(loop, handle);
}
DM_API int dm_fs_event_start(dm_fs_event_st *handle, dm_fs_event_cb cb, const char *path,
                             unsigned int flags)
{
  return uv_fs_event_start(handle, cb, path, flags);
}
DM_API int dm_fs_event_stop(dm_fs_event_st *handle)
{
  return uv_fs_event_stop(handle);
}
DM_API int dm_fs_event_getpath(dm_fs_event_st *handle, char *buffer, size_t *size)
{
  return uv_fs_event_getpath(handle, buffer, size);
}
DM_API int dm_ip4_addr(const char *ip, int port, struct sockaddr_in *addr)
{
  return uv_ip4_addr(ip, port, addr);
}
DM_API int dm_ip6_addr(const char *ip, int port, struct sockaddr_in6 *addr)
{
  return uv_ip6_addr(ip, port, addr);
}
DM_API int dm_ip4_name(const struct sockaddr_in *src, char *dst, size_t size)
{
  return uv_ip4_name(src, dst, size);
}
DM_API int dm_ip6_name(const struct sockaddr_in6 *src, char *dst, size_t size)
{
  return uv_ip6_name(src, dst, size);
}
DM_API int dm_ip_name(const struct sockaddr *src, char *dst, size_t size)
{
  return uv_ip_name(src, dst, size);
}
DM_API int dm_inet_ntop(int af, const void *src, char *dst, size_t size)
{
  return uv_inet_ntop(af, src, dst, size);
}
DM_API int dm_inet_pton(int af, const char *src, void *dst)
{
  return uv_inet_pton(af, src, dst);
}
DM_API int dm_random(dm_loop_st *loop, dm_random_st *req, void *buf, size_t buflen, unsigned flags,
                     dm_random_cb cb)
{
  return uv_random(loop, req, buf, buflen, flags, cb);
}
DM_API int dm_if_indextoname(unsigned int ifindex, char *buffer, size_t *size)
{
  return uv_if_indextoname(ifindex, buffer, size);
}
DM_API int dm_if_indextoiid(unsigned int ifindex, char *buffer, size_t *size)
{
  return uv_if_indextoiid(ifindex, buffer, size);
}
DM_API int dm_exepath(char *buffer, size_t *size)
{
  return uv_exepath(buffer, size);
}
DM_API int dm_cwd(char *buffer, size_t *size)
{
  return uv_cwd(buffer, size);
}
DM_API int dm_chdir(const char *dir)
{
  return uv_chdir(dir);
}
DM_API uint64_t dm_get_free_memory(void)
{
  return uv_get_free_memory();
}
DM_API uint64_t dm_get_total_memory(void)
{
  return uv_get_total_memory();
}
DM_API uint64_t dm_get_constrained_memory(void)
{
  return uv_get_constrained_memory();
}
DM_API uint64_t dm_get_available_memory(void)
{
  return uv_get_available_memory();
}
DM_API int dm_clock_gettime(dm_clock_id_et clock_id, dm_timespec64_st *ts)
{
  return uv_clock_gettime(clock_id, ts);
}
DM_API uint64_t dm_hrtime(void)
{
  return uv_hrtime();
}
DM_API void dm_sleep(unsigned int msec)
{
  uv_sleep(msec);
}
DM_API void dm_disable_stdio_inheritance(void)
{
  uv_disable_stdio_inheritance();
}
DM_API int dm_dlopen(const char *filename, dm_lib_st *lib)
{
  return uv_dlopen(filename, lib);
}
DM_API void dm_dlclose(dm_lib_st *lib)
{
  uv_dlclose(lib);
}
DM_API int dm_dlsym(dm_lib_st *lib, const char *name, void **ptr)
{
  return uv_dlsym(lib, name, ptr);
}
DM_API const char *dm_dlerror(const dm_lib_st *lib)
{
  return uv_dlerror(lib);
}
DM_API int dm_mutex_init(dm_mutex_t *handle)
{
  return uv_mutex_init(handle);
}
DM_API int dm_mutex_init_recursive(dm_mutex_t *handle)
{
  return uv_mutex_init_recursive(handle);
}
DM_API void dm_mutex_destroy(dm_mutex_t *handle)
{
  uv_mutex_destroy(handle);
}
DM_API void dm_mutex_lock(dm_mutex_t *handle)
{
  uv_mutex_lock(handle);
}
DM_API int dm_mutex_trylock(dm_mutex_t *handle)
{
  return uv_mutex_trylock(handle);
}
DM_API void dm_mutex_unlock(dm_mutex_t *handle)
{
  uv_mutex_unlock(handle);
}
DM_API int dm_rwlock_init(dm_rwlock_t *rwlock)
{
  return uv_rwlock_init(rwlock);
}
DM_API void dm_rwlock_destroy(dm_rwlock_t *rwlock)
{
  uv_rwlock_destroy(rwlock);
}
DM_API void dm_rwlock_rdlock(dm_rwlock_t *rwlock)
{
  uv_rwlock_rdlock(rwlock);
}
DM_API int dm_rwlock_tryrdlock(dm_rwlock_t *rwlock)
{
  return uv_rwlock_tryrdlock(rwlock);
}
DM_API void dm_rwlock_rdunlock(dm_rwlock_t *rwlock)
{
  uv_rwlock_rdunlock(rwlock);
}
DM_API void dm_rwlock_wrlock(dm_rwlock_t *rwlock)
{
  uv_rwlock_wrlock(rwlock);
}
DM_API int dm_rwlock_trywrlock(dm_rwlock_t *rwlock)
{
  return uv_rwlock_trywrlock(rwlock);
}
DM_API void dm_rwlock_wrunlock(dm_rwlock_t *rwlock)
{
  uv_rwlock_wrunlock(rwlock);
}
DM_API int dm_sem_init(dm_sem_t *sem, unsigned int value)
{
  return uv_sem_init(sem, value);
}
DM_API void dm_sem_destroy(dm_sem_t *sem)
{
  uv_sem_destroy(sem);
}
DM_API void dm_sem_post(dm_sem_t *sem)
{
  uv_sem_post(sem);
}
DM_API void dm_sem_wait(dm_sem_t *sem)
{
  uv_sem_wait(sem);
}
DM_API int dm_sem_trywait(dm_sem_t *sem)
{
  return uv_sem_trywait(sem);
}
DM_API int dm_cond_init(dm_cond_t *cond)
{
  return uv_cond_init(cond);
}
DM_API void dm_cond_destroy(dm_cond_t *cond)
{
  uv_cond_destroy(cond);
}
DM_API void dm_cond_signal(dm_cond_t *cond)
{
  uv_cond_signal(cond);
}
DM_API void dm_cond_broadcast(dm_cond_t *cond)
{
  uv_cond_broadcast(cond);
}
DM_API int dm_barrier_init(dm_barrier_t *barrier, unsigned int count)
{
  return uv_barrier_init(barrier, count);
}
DM_API void dm_barrier_destroy(dm_barrier_t *barrier)
{
  uv_barrier_destroy(barrier);
}
DM_API int dm_barrier_wait(dm_barrier_t *barrier)
{
  return uv_barrier_wait(barrier);
}
DM_API void dm_cond_wait(dm_cond_t *cond, dm_mutex_t *mutex)
{
  uv_cond_wait(cond, mutex);
}
DM_API int dm_cond_timedwait(dm_cond_t *cond, dm_mutex_t *mutex, uint64_t timeout)
{
  return uv_cond_timedwait(cond, mutex, timeout);
}
DM_API void dm_once(dm_once_t *guard, void (*callback)(void))
{
  uv_once(guard, callback);
}
DM_API int dm_key_create(dm_key_t *key)
{
  return uv_key_create(key);
}
DM_API void dm_key_delete(dm_key_t *key)
{
  uv_key_delete(key);
}
DM_API void *dm_key_get(dm_key_t *key)
{
  return uv_key_get(key);
}
DM_API void dm_key_set(dm_key_t *key, void *value)
{
  uv_key_set(key, value);
}
DM_API int dm_gettimeofday(dm_timeval64_st *tv)
{
  return uv_gettimeofday(tv);
}
DM_API int dm_thread_create(dm_thread_t *tid, dm_thread_cb entry, void *arg)
{
  return uv_thread_create(tid, entry, arg);
}
DM_API int dm_thread_detach(dm_thread_t *tid)
{
  return uv_thread_detach(tid);
}
DM_API int dm_thread_create_ex(dm_thread_t *tid, const dm_thread_options_st *params,
                               dm_thread_cb entry, void *arg)
{
  return uv_thread_create_ex(tid, params, entry, arg);
}
DM_API int dm_thread_setaffinity(dm_thread_t *tid, char *cpumask, char *oldmask, size_t mask_size)
{
  return uv_thread_setaffinity(tid, cpumask, oldmask, mask_size);
}
DM_API int dm_thread_getaffinity(dm_thread_t *tid, char *cpumask, size_t mask_size)
{
  return uv_thread_getaffinity(tid, cpumask, mask_size);
}
DM_API int dm_thread_getcpu(void)
{
  return uv_thread_getcpu();
}
DM_API dm_thread_t dm_thread_self(void)
{
  return uv_thread_self();
}
DM_API int dm_thread_join(dm_thread_t *tid)
{
  return uv_thread_join(tid);
}
DM_API int dm_thread_equal(const dm_thread_t *t1, const dm_thread_t *t2)
{
  return uv_thread_equal(t1, t2);
}
DM_API int dm_thread_setname(const char *name)
{
  return uv_thread_setname(name);
}
DM_API int dm_thread_getname(dm_thread_t *tid, char *name, size_t size)
{
  return uv_thread_getname(tid, name, size);
}
DM_API void *dm_loop_get_data(const dm_loop_st *arg1)
{
  return uv_loop_get_data(arg1);
}
DM_API void dm_loop_set_data(dm_loop_st *arg1, void *data)
{
  uv_loop_set_data(arg1, data);
}
DM_API size_t dm_utf16_length_as_wtf8(const uint16_t *utf16, ssize_t utf16_len)
{
  return uv_utf16_length_as_wtf8(utf16, utf16_len);
}
DM_API int dm_utf16_to_wtf8(const uint16_t *utf16, ssize_t utf16_len, char **wtf8_ptr,
                            size_t *wtf8_len_ptr)
{
  return uv_utf16_to_wtf8(utf16, utf16_len, wtf8_ptr, wtf8_len_ptr);
}
DM_API ssize_t dm_wtf8_length_as_utf16(const char *wtf8)
{
  return uv_wtf8_length_as_utf16(wtf8);
}
DM_API void dm_wtf8_to_utf16(const char *wtf8, uint16_t *utf16, size_t utf16_len)
{
  uv_wtf8_to_utf16(wtf8, utf16, utf16_len);
}
