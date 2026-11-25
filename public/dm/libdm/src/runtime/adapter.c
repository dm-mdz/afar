/* 由 tool/gen_dm_adapters.js 自动生成 —— 请勿手动修改。 */
/* 升级 libuv 后重新生成:  node tool/gen_dm_adapters.js */
/* 本文件保存全部生成的封装;runtime/dm.c 保存手写自有 API 与 dm_os_environ */
/* 变参函数(C 无法前向传递 `...`)不在此生成,由 dm.c 手工适配:
 *   uv_loop_configure
 */
/* 以下函数由 dm.c 手工实现(生成器只保留其类型别名,不生成直通函数):
 *   uv_version, uv_version_string, uv_replace_allocator, uv_os_environ, uv_os_free_environ
 */

#include <dm.h>
#include "../uv_adapter.h"

void dm_uv_library_shutdown(void)
{
  uv_library_shutdown();
}
dm_uv_loop_st *dm_uv_default_loop(void)
{
  return uv_default_loop();
}
int dm_uv_loop_init(dm_uv_loop_st *loop)
{
  return uv_loop_init(loop);
}
int dm_uv_loop_close(dm_uv_loop_st *loop)
{
  return uv_loop_close(loop);
}
dm_uv_loop_st *dm_uv_loop_new(void)
{
  return uv_loop_new();
}
void dm_uv_loop_delete(dm_uv_loop_st *arg1)
{
  uv_loop_delete(arg1);
}
size_t dm_uv_loop_size(void)
{
  return uv_loop_size();
}
int dm_uv_loop_alive(const dm_uv_loop_st *loop)
{
  return uv_loop_alive(loop);
}
int dm_uv_loop_fork(dm_uv_loop_st *loop)
{
  return uv_loop_fork(loop);
}
int dm_uv_run(dm_uv_loop_st *arg1, dm_uv_run_mode_et mode)
{
  return uv_run(arg1, mode);
}
void dm_uv_stop(dm_uv_loop_st *arg1)
{
  uv_stop(arg1);
}
void dm_uv_ref(dm_uv_handle_st *arg1)
{
  uv_ref(arg1);
}
void dm_uv_unref(dm_uv_handle_st *arg1)
{
  uv_unref(arg1);
}
int dm_uv_has_ref(const dm_uv_handle_st *arg1)
{
  return uv_has_ref(arg1);
}
void dm_uv_update_time(dm_uv_loop_st *arg1)
{
  uv_update_time(arg1);
}
uint64_t dm_uv_now(const dm_uv_loop_st *arg1)
{
  return uv_now(arg1);
}
int dm_uv_backend_fd(const dm_uv_loop_st *arg1)
{
  return uv_backend_fd(arg1);
}
int dm_uv_backend_timeout(const dm_uv_loop_st *arg1)
{
  return uv_backend_timeout(arg1);
}
int dm_uv_translate_sys_error(int sys_errno)
{
  return uv_translate_sys_error(sys_errno);
}
const char *dm_uv_strerror(int err)
{
  return uv_strerror(err);
}
char *dm_uv_strerror_r(int err, char *buf, size_t buflen)
{
  return uv_strerror_r(err, buf, buflen);
}
const char *dm_uv_err_name(int err)
{
  return uv_err_name(err);
}
char *dm_uv_err_name_r(int err, char *buf, size_t buflen)
{
  return uv_err_name_r(err, buf, buflen);
}
int dm_uv_shutdown(dm_uv_shutdown_st *req, dm_uv_stream_st *handle, dm_uv_shutdown_cb cb)
{
  return uv_shutdown(req, handle, cb);
}
size_t dm_uv_handle_size(dm_uv_handle_type_et type)
{
  return uv_handle_size(type);
}
dm_uv_handle_type_et dm_uv_handle_get_type(const dm_uv_handle_st *handle)
{
  return uv_handle_get_type(handle);
}
const char *dm_uv_handle_type_name(dm_uv_handle_type_et type)
{
  return uv_handle_type_name(type);
}
void *dm_uv_handle_get_data(const dm_uv_handle_st *handle)
{
  return uv_handle_get_data(handle);
}
dm_uv_loop_st *dm_uv_handle_get_loop(const dm_uv_handle_st *handle)
{
  return uv_handle_get_loop(handle);
}
void dm_uv_handle_set_data(dm_uv_handle_st *handle, void *data)
{
  uv_handle_set_data(handle, data);
}
size_t dm_uv_req_size(dm_uv_req_type_et type)
{
  return uv_req_size(type);
}
void *dm_uv_req_get_data(const dm_uv_req_st *req)
{
  return uv_req_get_data(req);
}
void dm_uv_req_set_data(dm_uv_req_st *req, void *data)
{
  uv_req_set_data(req, data);
}
dm_uv_req_type_et dm_uv_req_get_type(const dm_uv_req_st *req)
{
  return uv_req_get_type(req);
}
const char *dm_uv_req_type_name(dm_uv_req_type_et type)
{
  return uv_req_type_name(type);
}
int dm_uv_is_active(const dm_uv_handle_st *handle)
{
  return uv_is_active(handle);
}
void dm_uv_walk(dm_uv_loop_st *loop, dm_uv_walk_cb walk_cb, void *arg)
{
  uv_walk(loop, walk_cb, arg);
}
void dm_uv_print_all_handles(dm_uv_loop_st *loop, FILE *stream)
{
  uv_print_all_handles(loop, stream);
}
void dm_uv_print_active_handles(dm_uv_loop_st *loop, FILE *stream)
{
  uv_print_active_handles(loop, stream);
}
void dm_uv_close(dm_uv_handle_st *handle, dm_uv_close_cb close_cb)
{
  uv_close(handle, close_cb);
}
int dm_uv_send_buffer_size(dm_uv_handle_st *handle, int *value)
{
  return uv_send_buffer_size(handle, value);
}
int dm_uv_recv_buffer_size(dm_uv_handle_st *handle, int *value)
{
  return uv_recv_buffer_size(handle, value);
}
int dm_uv_fileno(const dm_uv_handle_st *handle, dm_uv_os_fd_t *fd)
{
  return uv_fileno(handle, fd);
}
dm_uv_buf_st dm_uv_buf_init(char *base, unsigned int len)
{
  return uv_buf_init(base, len);
}
int dm_uv_pipe(dm_uv_file fds[2], int read_flags, int write_flags)
{
  return uv_pipe(fds, read_flags, write_flags);
}
int dm_uv_socketpair(int type, int protocol, dm_uv_os_sock_t socket_vector[2], int flags0,
                     int flags1)
{
  return uv_socketpair(type, protocol, socket_vector, flags0, flags1);
}
size_t dm_uv_stream_get_write_queue_size(const dm_uv_stream_st *stream)
{
  return uv_stream_get_write_queue_size(stream);
}
int dm_uv_listen(dm_uv_stream_st *stream, int backlog, dm_uv_connection_cb cb)
{
  return uv_listen(stream, backlog, cb);
}
int dm_uv_accept(dm_uv_stream_st *server, dm_uv_stream_st *client)
{
  return uv_accept(server, client);
}
int dm_uv_read_start(dm_uv_stream_st *arg1, dm_uv_alloc_cb alloc_cb, dm_uv_read_cb read_cb)
{
  return uv_read_start(arg1, alloc_cb, read_cb);
}
int dm_uv_read_stop(dm_uv_stream_st *arg1)
{
  return uv_read_stop(arg1);
}
int dm_uv_write(dm_uv_write_st *req, dm_uv_stream_st *handle, const dm_uv_buf_st bufs[],
                unsigned int nbufs, dm_uv_write_cb cb)
{
  return uv_write(req, handle, bufs, nbufs, cb);
}
int dm_uv_write2(dm_uv_write_st *req, dm_uv_stream_st *handle, const dm_uv_buf_st bufs[],
                 unsigned int nbufs, dm_uv_stream_st *send_handle, dm_uv_write_cb cb)
{
  return uv_write2(req, handle, bufs, nbufs, send_handle, cb);
}
int dm_uv_try_write(dm_uv_stream_st *handle, const dm_uv_buf_st bufs[], unsigned int nbufs)
{
  return uv_try_write(handle, bufs, nbufs);
}
int dm_uv_try_write2(dm_uv_stream_st *handle, const dm_uv_buf_st bufs[], unsigned int nbufs,
                     dm_uv_stream_st *send_handle)
{
  return uv_try_write2(handle, bufs, nbufs, send_handle);
}
size_t dm_uv_write_nwritten(const dm_uv_write_st *req)
{
  return uv_write_nwritten(req);
}
int dm_uv_is_readable(const dm_uv_stream_st *handle)
{
  return uv_is_readable(handle);
}
int dm_uv_is_writable(const dm_uv_stream_st *handle)
{
  return uv_is_writable(handle);
}
int dm_uv_stream_set_blocking(dm_uv_stream_st *handle, int blocking)
{
  return uv_stream_set_blocking(handle, blocking);
}
int dm_uv_is_closing(const dm_uv_handle_st *handle)
{
  return uv_is_closing(handle);
}
int dm_uv_tcp_init(dm_uv_loop_st *arg1, dm_uv_tcp_st *handle)
{
  return uv_tcp_init(arg1, handle);
}
int dm_uv_tcp_init_ex(dm_uv_loop_st *arg1, dm_uv_tcp_st *handle, unsigned int flags)
{
  return uv_tcp_init_ex(arg1, handle, flags);
}
int dm_uv_tcp_open(dm_uv_tcp_st *handle, dm_uv_os_sock_t sock)
{
  return uv_tcp_open(handle, sock);
}
int dm_uv_tcp_nodelay(dm_uv_tcp_st *handle, int enable)
{
  return uv_tcp_nodelay(handle, enable);
}
int dm_uv_tcp_keepalive(dm_uv_tcp_st *handle, int enable, unsigned int delay)
{
  return uv_tcp_keepalive(handle, enable, delay);
}
int dm_uv_tcp_keepalive_ex(dm_uv_tcp_st *handle, int on, unsigned int idle, unsigned int intvl,
                           unsigned int cnt)
{
  return uv_tcp_keepalive_ex(handle, on, idle, intvl, cnt);
}
int dm_uv_tcp_simultaneous_accepts(dm_uv_tcp_st *handle, int enable)
{
  return uv_tcp_simultaneous_accepts(handle, enable);
}
int dm_uv_tcp_bind(dm_uv_tcp_st *handle, const struct sockaddr *addr, unsigned int flags)
{
  return uv_tcp_bind(handle, addr, flags);
}
int dm_uv_tcp_getsockname(const dm_uv_tcp_st *handle, struct sockaddr *name, int *namelen)
{
  return uv_tcp_getsockname(handle, name, namelen);
}
int dm_uv_tcp_getpeername(const dm_uv_tcp_st *handle, struct sockaddr *name, int *namelen)
{
  return uv_tcp_getpeername(handle, name, namelen);
}
int dm_uv_tcp_close_reset(dm_uv_tcp_st *handle, dm_uv_close_cb close_cb)
{
  return uv_tcp_close_reset(handle, close_cb);
}
int dm_uv_tcp_connect(dm_uv_connect_st *req, dm_uv_tcp_st *handle, const struct sockaddr *addr,
                      dm_uv_connect_cb cb)
{
  return uv_tcp_connect(req, handle, addr, cb);
}
int dm_uv_udp_init(dm_uv_loop_st *arg1, dm_uv_udp_st *handle)
{
  return uv_udp_init(arg1, handle);
}
int dm_uv_udp_init_ex(dm_uv_loop_st *arg1, dm_uv_udp_st *handle, unsigned int flags)
{
  return uv_udp_init_ex(arg1, handle, flags);
}
int dm_uv_udp_open(dm_uv_udp_st *handle, dm_uv_os_sock_t sock)
{
  return uv_udp_open(handle, sock);
}
int dm_uv_udp_open_ex(dm_uv_udp_st *handle, dm_uv_os_sock_t sock, unsigned int flags)
{
  return uv_udp_open_ex(handle, sock, flags);
}
int dm_uv_udp_bind(dm_uv_udp_st *handle, const struct sockaddr *addr, unsigned int flags)
{
  return uv_udp_bind(handle, addr, flags);
}
int dm_uv_udp_connect(dm_uv_udp_st *handle, const struct sockaddr *addr)
{
  return uv_udp_connect(handle, addr);
}
int dm_uv_udp_getpeername(const dm_uv_udp_st *handle, struct sockaddr *name, int *namelen)
{
  return uv_udp_getpeername(handle, name, namelen);
}
int dm_uv_udp_getsockname(const dm_uv_udp_st *handle, struct sockaddr *name, int *namelen)
{
  return uv_udp_getsockname(handle, name, namelen);
}
int dm_uv_udp_set_membership(dm_uv_udp_st *handle, const char *multicast_addr,
                             const char *interface_addr, dm_uv_membership_et membership)
{
  return uv_udp_set_membership(handle, multicast_addr, interface_addr, membership);
}
int dm_uv_udp_set_source_membership(dm_uv_udp_st *handle, const char *multicast_addr,
                                    const char *interface_addr, const char *source_addr,
                                    dm_uv_membership_et membership)
{
  return uv_udp_set_source_membership(handle, multicast_addr, interface_addr, source_addr,
                                      membership);
}
int dm_uv_udp_set_multicast_loop(dm_uv_udp_st *handle, int on)
{
  return uv_udp_set_multicast_loop(handle, on);
}
int dm_uv_udp_set_multicast_ttl(dm_uv_udp_st *handle, int ttl)
{
  return uv_udp_set_multicast_ttl(handle, ttl);
}
int dm_uv_udp_set_multicast_interface(dm_uv_udp_st *handle, const char *interface_addr)
{
  return uv_udp_set_multicast_interface(handle, interface_addr);
}
int dm_uv_udp_set_broadcast(dm_uv_udp_st *handle, int on)
{
  return uv_udp_set_broadcast(handle, on);
}
int dm_uv_udp_set_ttl(dm_uv_udp_st *handle, int ttl)
{
  return uv_udp_set_ttl(handle, ttl);
}
int dm_uv_udp_send(dm_uv_udp_send_st *req, dm_uv_udp_st *handle, const dm_uv_buf_st bufs[],
                   unsigned int nbufs, const struct sockaddr *addr, dm_uv_udp_send_cb send_cb)
{
  return uv_udp_send(req, handle, bufs, nbufs, addr, send_cb);
}
int dm_uv_udp_try_send(dm_uv_udp_st *handle, const dm_uv_buf_st bufs[], unsigned int nbufs,
                       const struct sockaddr *addr)
{
  return uv_udp_try_send(handle, bufs, nbufs, addr);
}
int dm_uv_udp_try_send2(dm_uv_udp_st *handle, unsigned int count, dm_uv_buf_st *bufs[],
                        unsigned int nbufs[], struct sockaddr *addrs[], unsigned int flags)
{
  return uv_udp_try_send2(handle, count, bufs, nbufs, addrs, flags);
}
int dm_uv_udp_recv_start(dm_uv_udp_st *handle, dm_uv_alloc_cb alloc_cb, dm_uv_udp_recv_cb recv_cb)
{
  return uv_udp_recv_start(handle, alloc_cb, recv_cb);
}
int dm_uv_udp_using_recvmmsg(const dm_uv_udp_st *handle)
{
  return uv_udp_using_recvmmsg(handle);
}
int dm_uv_udp_recv_stop(dm_uv_udp_st *handle)
{
  return uv_udp_recv_stop(handle);
}
size_t dm_uv_udp_get_send_queue_size(const dm_uv_udp_st *handle)
{
  return uv_udp_get_send_queue_size(handle);
}
size_t dm_uv_udp_get_send_queue_count(const dm_uv_udp_st *handle)
{
  return uv_udp_get_send_queue_count(handle);
}
int dm_uv_tty_init(dm_uv_loop_st *arg1, dm_uv_tty_st *arg2, dm_uv_file fd, int readable)
{
  return uv_tty_init(arg1, arg2, fd, readable);
}
int dm_uv_tty_set_mode(dm_uv_tty_st *arg1, dm_uv_tty_mode_et mode)
{
  return uv_tty_set_mode(arg1, mode);
}
int dm_uv_tty_reset_mode(void)
{
  return uv_tty_reset_mode();
}
int dm_uv_tty_get_winsize(dm_uv_tty_st *arg1, int *width, int *height)
{
  return uv_tty_get_winsize(arg1, width, height);
}
void dm_uv_tty_set_vterm_state(dm_uv_tty_vtermstate_et state)
{
  uv_tty_set_vterm_state(state);
}
int dm_uv_tty_get_vterm_state(dm_uv_tty_vtermstate_et *state)
{
  return uv_tty_get_vterm_state(state);
}
dm_uv_handle_type_et dm_uv_guess_handle(dm_uv_file file)
{
  return uv_guess_handle(file);
}
int dm_uv_pipe_init(dm_uv_loop_st *arg1, dm_uv_pipe_st *handle, int ipc)
{
  return uv_pipe_init(arg1, handle, ipc);
}
int dm_uv_pipe_open(dm_uv_pipe_st *arg1, dm_uv_file file)
{
  return uv_pipe_open(arg1, file);
}
int dm_uv_pipe_bind(dm_uv_pipe_st *handle, const char *name)
{
  return uv_pipe_bind(handle, name);
}
int dm_uv_pipe_bind2(dm_uv_pipe_st *handle, const char *name, size_t namelen, unsigned int flags)
{
  return uv_pipe_bind2(handle, name, namelen, flags);
}
void dm_uv_pipe_connect(dm_uv_connect_st *req, dm_uv_pipe_st *handle, const char *name,
                        dm_uv_connect_cb cb)
{
  uv_pipe_connect(req, handle, name, cb);
}
int dm_uv_pipe_connect2(dm_uv_connect_st *req, dm_uv_pipe_st *handle, const char *name,
                        size_t namelen, unsigned int flags, dm_uv_connect_cb cb)
{
  return uv_pipe_connect2(req, handle, name, namelen, flags, cb);
}
int dm_uv_pipe_getsockname(const dm_uv_pipe_st *handle, char *buffer, size_t *size)
{
  return uv_pipe_getsockname(handle, buffer, size);
}
int dm_uv_pipe_getpeername(const dm_uv_pipe_st *handle, char *buffer, size_t *size)
{
  return uv_pipe_getpeername(handle, buffer, size);
}
void dm_uv_pipe_pending_instances(dm_uv_pipe_st *handle, int count)
{
  uv_pipe_pending_instances(handle, count);
}
int dm_uv_pipe_pending_count(dm_uv_pipe_st *handle)
{
  return uv_pipe_pending_count(handle);
}
dm_uv_handle_type_et dm_uv_pipe_pending_type(dm_uv_pipe_st *handle)
{
  return uv_pipe_pending_type(handle);
}
int dm_uv_pipe_chmod(dm_uv_pipe_st *handle, int flags)
{
  return uv_pipe_chmod(handle, flags);
}
int dm_uv_poll_init(dm_uv_loop_st *loop, dm_uv_poll_st *handle, int fd)
{
  return uv_poll_init(loop, handle, fd);
}
int dm_uv_poll_init_socket(dm_uv_loop_st *loop, dm_uv_poll_st *handle, dm_uv_os_sock_t socket)
{
  return uv_poll_init_socket(loop, handle, socket);
}
int dm_uv_poll_start(dm_uv_poll_st *handle, int events, dm_uv_poll_cb cb)
{
  return uv_poll_start(handle, events, cb);
}
int dm_uv_poll_stop(dm_uv_poll_st *handle)
{
  return uv_poll_stop(handle);
}
int dm_uv_prepare_init(dm_uv_loop_st *arg1, dm_uv_prepare_st *prepare)
{
  return uv_prepare_init(arg1, prepare);
}
int dm_uv_prepare_start(dm_uv_prepare_st *prepare, dm_uv_prepare_cb cb)
{
  return uv_prepare_start(prepare, cb);
}
int dm_uv_prepare_stop(dm_uv_prepare_st *prepare)
{
  return uv_prepare_stop(prepare);
}
int dm_uv_check_init(dm_uv_loop_st *arg1, dm_uv_check_st *check)
{
  return uv_check_init(arg1, check);
}
int dm_uv_check_start(dm_uv_check_st *check, dm_uv_check_cb cb)
{
  return uv_check_start(check, cb);
}
int dm_uv_check_stop(dm_uv_check_st *check)
{
  return uv_check_stop(check);
}
int dm_uv_idle_init(dm_uv_loop_st *arg1, dm_uv_idle_st *idle)
{
  return uv_idle_init(arg1, idle);
}
int dm_uv_idle_start(dm_uv_idle_st *idle, dm_uv_idle_cb cb)
{
  return uv_idle_start(idle, cb);
}
int dm_uv_idle_stop(dm_uv_idle_st *idle)
{
  return uv_idle_stop(idle);
}
int dm_uv_async_init(dm_uv_loop_st *arg1, dm_uv_async_st *async, dm_uv_async_cb async_cb)
{
  return uv_async_init(arg1, async, async_cb);
}
int dm_uv_async_send(dm_uv_async_st *async)
{
  return uv_async_send(async);
}
int dm_uv_timer_init(dm_uv_loop_st *arg1, dm_uv_timer_st *handle)
{
  return uv_timer_init(arg1, handle);
}
int dm_uv_timer_start(dm_uv_timer_st *handle, dm_uv_timer_cb cb, uint64_t timeout, uint64_t repeat)
{
  return uv_timer_start(handle, cb, timeout, repeat);
}
int dm_uv_timer_stop(dm_uv_timer_st *handle)
{
  return uv_timer_stop(handle);
}
int dm_uv_timer_again(dm_uv_timer_st *handle)
{
  return uv_timer_again(handle);
}
void dm_uv_timer_set_repeat(dm_uv_timer_st *handle, uint64_t repeat)
{
  uv_timer_set_repeat(handle, repeat);
}
uint64_t dm_uv_timer_get_repeat(const dm_uv_timer_st *handle)
{
  return uv_timer_get_repeat(handle);
}
uint64_t dm_uv_timer_get_due_in(const dm_uv_timer_st *handle)
{
  return uv_timer_get_due_in(handle);
}
int dm_uv_getaddrinfo(dm_uv_loop_st *loop, dm_uv_getaddrinfo_st *req,
                      dm_uv_getaddrinfo_cb getaddrinfo_cb, const char *node, const char *service,
                      const struct addrinfo *hints)
{
  return uv_getaddrinfo(loop, req, getaddrinfo_cb, node, service, hints);
}
void dm_uv_freeaddrinfo(struct addrinfo *ai)
{
  uv_freeaddrinfo(ai);
}
int dm_uv_getnameinfo(dm_uv_loop_st *loop, dm_uv_getnameinfo_st *req,
                      dm_uv_getnameinfo_cb getnameinfo_cb, const struct sockaddr *addr, int flags)
{
  return uv_getnameinfo(loop, req, getnameinfo_cb, addr, flags);
}
int dm_uv_spawn(dm_uv_loop_st *loop, dm_uv_process_st *handle,
                const dm_uv_process_options_st *options)
{
  return uv_spawn(loop, handle, options);
}
int dm_uv_process_kill(dm_uv_process_st *arg1, int signum)
{
  return uv_process_kill(arg1, signum);
}
int dm_uv_kill(int pid, int signum)
{
  return uv_kill(pid, signum);
}
dm_uv_pid_t dm_uv_process_get_pid(const dm_uv_process_st *arg1)
{
  return uv_process_get_pid(arg1);
}
int dm_uv_queue_work(dm_uv_loop_st *loop, dm_uv_work_st *req, dm_uv_work_cb work_cb,
                     dm_uv_after_work_cb after_work_cb)
{
  return uv_queue_work(loop, req, work_cb, after_work_cb);
}
int dm_uv_cancel(dm_uv_req_st *req)
{
  return uv_cancel(req);
}
char **dm_uv_setup_args(int argc, char **argv)
{
  return uv_setup_args(argc, argv);
}
int dm_uv_get_process_title(char *buffer, size_t size)
{
  return uv_get_process_title(buffer, size);
}
int dm_uv_set_process_title(const char *title)
{
  return uv_set_process_title(title);
}
int dm_uv_resident_set_memory(size_t *rss)
{
  return uv_resident_set_memory(rss);
}
int dm_uv_uptime(double *uptime)
{
  return uv_uptime(uptime);
}
dm_uv_os_fd_t dm_uv_get_osfhandle(int fd)
{
  return uv_get_osfhandle(fd);
}
int dm_uv_open_osfhandle(dm_uv_os_fd_t os_fd)
{
  return uv_open_osfhandle(os_fd);
}
int dm_uv_getrusage(dm_uv_rusage_st *rusage)
{
  return uv_getrusage(rusage);
}
int dm_uv_getrusage_thread(dm_uv_rusage_st *rusage)
{
  return uv_getrusage_thread(rusage);
}
int dm_uv_os_homedir(char *buffer, size_t *size)
{
  return uv_os_homedir(buffer, size);
}
int dm_uv_os_tmpdir(char *buffer, size_t *size)
{
  return uv_os_tmpdir(buffer, size);
}
int dm_uv_os_get_passwd(dm_uv_passwd_st *pwd)
{
  return uv_os_get_passwd(pwd);
}
void dm_uv_os_free_passwd(dm_uv_passwd_st *pwd)
{
  uv_os_free_passwd(pwd);
}
int dm_uv_os_get_passwd2(dm_uv_passwd_st *pwd, dm_uv_uid_t uid)
{
  return uv_os_get_passwd2(pwd, uid);
}
int dm_uv_os_get_group(dm_uv_group_st *grp, dm_uv_uid_t gid)
{
  return uv_os_get_group(grp, gid);
}
void dm_uv_os_free_group(dm_uv_group_st *grp)
{
  uv_os_free_group(grp);
}
dm_uv_pid_t dm_uv_os_getpid(void)
{
  return uv_os_getpid();
}
dm_uv_pid_t dm_uv_os_getppid(void)
{
#ifdef _WIN32
  /* 确保 libuv 初始化 pNtQueryInformationProcess（W1 Win 修复） */
  (void)uv_hrtime();
#endif
  return uv_os_getppid();
}
int dm_uv_os_getpriority(dm_uv_pid_t pid, int *priority)
{
  return uv_os_getpriority(pid, priority);
}
int dm_uv_os_setpriority(dm_uv_pid_t pid, int priority)
{
  return uv_os_setpriority(pid, priority);
}
int dm_uv_thread_getpriority(dm_uv_thread_t tid, int *priority)
{
  return uv_thread_getpriority(tid, priority);
}
int dm_uv_thread_setpriority(dm_uv_thread_t tid, int priority)
{
  return uv_thread_setpriority(tid, priority);
}
unsigned int dm_uv_available_parallelism(void)
{
  return uv_available_parallelism();
}
int dm_uv_cpu_info(dm_uv_cpu_info_st **cpu_infos, int *count)
{
  return uv_cpu_info(cpu_infos, count);
}
void dm_uv_free_cpu_info(dm_uv_cpu_info_st *cpu_infos, int count)
{
  uv_free_cpu_info(cpu_infos, count);
}
int dm_uv_cpumask_size(void)
{
  return uv_cpumask_size();
}
int dm_uv_interface_addresses(dm_uv_interface_address_st **addresses, int *count)
{
  return uv_interface_addresses(addresses, count);
}
void dm_uv_free_interface_addresses(dm_uv_interface_address_st *addresses, int count)
{
  uv_free_interface_addresses(addresses, count);
}
int dm_uv_os_getenv(const char *name, char *buffer, size_t *size)
{
  return uv_os_getenv(name, buffer, size);
}
int dm_uv_os_setenv(const char *name, const char *value)
{
  return uv_os_setenv(name, value);
}
int dm_uv_os_unsetenv(const char *name)
{
  return uv_os_unsetenv(name);
}
int dm_uv_os_gethostname(char *buffer, size_t *size)
{
  return uv_os_gethostname(buffer, size);
}
int dm_uv_os_uname(dm_uv_utsname_st *buffer)
{
  return uv_os_uname(buffer);
}
int dm_uv_metrics_info(dm_uv_loop_st *loop, dm_uv_metrics_st *metrics)
{
  return uv_metrics_info(loop, metrics);
}
uint64_t dm_uv_metrics_idle_time(dm_uv_loop_st *loop)
{
  return uv_metrics_idle_time(loop);
}
dm_uv_fs_type_et dm_uv_fs_get_type(const dm_uv_fs_st *arg1)
{
  return uv_fs_get_type(arg1);
}
ssize_t dm_uv_fs_get_result(const dm_uv_fs_st *arg1)
{
  return uv_fs_get_result(arg1);
}
int dm_uv_fs_get_system_error(const dm_uv_fs_st *arg1)
{
  return uv_fs_get_system_error(arg1);
}
void *dm_uv_fs_get_ptr(const dm_uv_fs_st *arg1)
{
  return uv_fs_get_ptr(arg1);
}
const char *dm_uv_fs_get_path(const dm_uv_fs_st *arg1)
{
  return uv_fs_get_path(arg1);
}
dm_uv_stat_st *dm_uv_fs_get_statbuf(dm_uv_fs_st *arg1)
{
  return uv_fs_get_statbuf(arg1);
}
void dm_uv_fs_req_cleanup(dm_uv_fs_st *req)
{
  uv_fs_req_cleanup(req);
}
int dm_uv_fs_close(dm_uv_loop_st *loop, dm_uv_fs_st *req, dm_uv_file file, dm_uv_fs_cb cb)
{
  return uv_fs_close(loop, req, file, cb);
}
int dm_uv_fs_open(dm_uv_loop_st *loop, dm_uv_fs_st *req, const char *path, int flags, int mode,
                  dm_uv_fs_cb cb)
{
  return uv_fs_open(loop, req, path, flags, mode, cb);
}
int dm_uv_fs_read(dm_uv_loop_st *loop, dm_uv_fs_st *req, dm_uv_file file, const dm_uv_buf_st bufs[],
                  unsigned int nbufs, int64_t offset, dm_uv_fs_cb cb)
{
  return uv_fs_read(loop, req, file, bufs, nbufs, offset, cb);
}
int dm_uv_fs_unlink(dm_uv_loop_st *loop, dm_uv_fs_st *req, const char *path, dm_uv_fs_cb cb)
{
  return uv_fs_unlink(loop, req, path, cb);
}
int dm_uv_fs_write(dm_uv_loop_st *loop, dm_uv_fs_st *req, dm_uv_file file,
                   const dm_uv_buf_st bufs[], unsigned int nbufs, int64_t offset, dm_uv_fs_cb cb)
{
  return uv_fs_write(loop, req, file, bufs, nbufs, offset, cb);
}
int dm_uv_fs_copyfile(dm_uv_loop_st *loop, dm_uv_fs_st *req, const char *path, const char *new_path,
                      int flags, dm_uv_fs_cb cb)
{
  return uv_fs_copyfile(loop, req, path, new_path, flags, cb);
}
int dm_uv_fs_mkdir(dm_uv_loop_st *loop, dm_uv_fs_st *req, const char *path, int mode,
                   dm_uv_fs_cb cb)
{
  return uv_fs_mkdir(loop, req, path, mode, cb);
}
int dm_uv_fs_mkdtemp(dm_uv_loop_st *loop, dm_uv_fs_st *req, const char *tpl, dm_uv_fs_cb cb)
{
  return uv_fs_mkdtemp(loop, req, tpl, cb);
}
int dm_uv_fs_mkstemp(dm_uv_loop_st *loop, dm_uv_fs_st *req, const char *tpl, dm_uv_fs_cb cb)
{
  return uv_fs_mkstemp(loop, req, tpl, cb);
}
int dm_uv_fs_rmdir(dm_uv_loop_st *loop, dm_uv_fs_st *req, const char *path, dm_uv_fs_cb cb)
{
  return uv_fs_rmdir(loop, req, path, cb);
}
int dm_uv_fs_scandir(dm_uv_loop_st *loop, dm_uv_fs_st *req, const char *path, int flags,
                     dm_uv_fs_cb cb)
{
  return uv_fs_scandir(loop, req, path, flags, cb);
}
int dm_uv_fs_scandir_next(dm_uv_fs_st *req, dm_uv_dirent_st *ent)
{
  return uv_fs_scandir_next(req, ent);
}
int dm_uv_fs_opendir(dm_uv_loop_st *loop, dm_uv_fs_st *req, const char *path, dm_uv_fs_cb cb)
{
  return uv_fs_opendir(loop, req, path, cb);
}
int dm_uv_fs_readdir(dm_uv_loop_st *loop, dm_uv_fs_st *req, dm_uv_dir_st *dir, dm_uv_fs_cb cb)
{
  return uv_fs_readdir(loop, req, dir, cb);
}
int dm_uv_fs_closedir(dm_uv_loop_st *loop, dm_uv_fs_st *req, dm_uv_dir_st *dir, dm_uv_fs_cb cb)
{
  return uv_fs_closedir(loop, req, dir, cb);
}
int dm_uv_fs_stat(dm_uv_loop_st *loop, dm_uv_fs_st *req, const char *path, dm_uv_fs_cb cb)
{
  return uv_fs_stat(loop, req, path, cb);
}
int dm_uv_fs_fstat(dm_uv_loop_st *loop, dm_uv_fs_st *req, dm_uv_file file, dm_uv_fs_cb cb)
{
  return uv_fs_fstat(loop, req, file, cb);
}
int dm_uv_fs_rename(dm_uv_loop_st *loop, dm_uv_fs_st *req, const char *path, const char *new_path,
                    dm_uv_fs_cb cb)
{
  return uv_fs_rename(loop, req, path, new_path, cb);
}
int dm_uv_fs_fsync(dm_uv_loop_st *loop, dm_uv_fs_st *req, dm_uv_file file, dm_uv_fs_cb cb)
{
  return uv_fs_fsync(loop, req, file, cb);
}
int dm_uv_fs_fdatasync(dm_uv_loop_st *loop, dm_uv_fs_st *req, dm_uv_file file, dm_uv_fs_cb cb)
{
  return uv_fs_fdatasync(loop, req, file, cb);
}
int dm_uv_fs_ftruncate(dm_uv_loop_st *loop, dm_uv_fs_st *req, dm_uv_file file, int64_t offset,
                       dm_uv_fs_cb cb)
{
  return uv_fs_ftruncate(loop, req, file, offset, cb);
}
int dm_uv_fs_sendfile(dm_uv_loop_st *loop, dm_uv_fs_st *req, dm_uv_file out_fd, dm_uv_file in_fd,
                      int64_t in_offset, size_t length, dm_uv_fs_cb cb)
{
  return uv_fs_sendfile(loop, req, out_fd, in_fd, in_offset, length, cb);
}
int dm_uv_fs_access(dm_uv_loop_st *loop, dm_uv_fs_st *req, const char *path, int mode,
                    dm_uv_fs_cb cb)
{
  return uv_fs_access(loop, req, path, mode, cb);
}
int dm_uv_fs_chmod(dm_uv_loop_st *loop, dm_uv_fs_st *req, const char *path, int mode,
                   dm_uv_fs_cb cb)
{
  return uv_fs_chmod(loop, req, path, mode, cb);
}
int dm_uv_fs_utime(dm_uv_loop_st *loop, dm_uv_fs_st *req, const char *path, double atime,
                   double mtime, dm_uv_fs_cb cb)
{
  return uv_fs_utime(loop, req, path, atime, mtime, cb);
}
int dm_uv_fs_futime(dm_uv_loop_st *loop, dm_uv_fs_st *req, dm_uv_file file, double atime,
                    double mtime, dm_uv_fs_cb cb)
{
  return uv_fs_futime(loop, req, file, atime, mtime, cb);
}
int dm_uv_fs_lutime(dm_uv_loop_st *loop, dm_uv_fs_st *req, const char *path, double atime,
                    double mtime, dm_uv_fs_cb cb)
{
  return uv_fs_lutime(loop, req, path, atime, mtime, cb);
}
int dm_uv_fs_lstat(dm_uv_loop_st *loop, dm_uv_fs_st *req, const char *path, dm_uv_fs_cb cb)
{
  return uv_fs_lstat(loop, req, path, cb);
}
int dm_uv_fs_link(dm_uv_loop_st *loop, dm_uv_fs_st *req, const char *path, const char *new_path,
                  dm_uv_fs_cb cb)
{
  return uv_fs_link(loop, req, path, new_path, cb);
}
int dm_uv_fs_symlink(dm_uv_loop_st *loop, dm_uv_fs_st *req, const char *path, const char *new_path,
                     int flags, dm_uv_fs_cb cb)
{
  return uv_fs_symlink(loop, req, path, new_path, flags, cb);
}
int dm_uv_fs_readlink(dm_uv_loop_st *loop, dm_uv_fs_st *req, const char *path, dm_uv_fs_cb cb)
{
  return uv_fs_readlink(loop, req, path, cb);
}
int dm_uv_fs_realpath(dm_uv_loop_st *loop, dm_uv_fs_st *req, const char *path, dm_uv_fs_cb cb)
{
  return uv_fs_realpath(loop, req, path, cb);
}
int dm_uv_fs_fchmod(dm_uv_loop_st *loop, dm_uv_fs_st *req, dm_uv_file file, int mode,
                    dm_uv_fs_cb cb)
{
  return uv_fs_fchmod(loop, req, file, mode, cb);
}
int dm_uv_fs_chown(dm_uv_loop_st *loop, dm_uv_fs_st *req, const char *path, dm_uv_uid_t uid,
                   dm_uv_gid_t gid, dm_uv_fs_cb cb)
{
  return uv_fs_chown(loop, req, path, uid, gid, cb);
}
int dm_uv_fs_fchown(dm_uv_loop_st *loop, dm_uv_fs_st *req, dm_uv_file file, dm_uv_uid_t uid,
                    dm_uv_gid_t gid, dm_uv_fs_cb cb)
{
  return uv_fs_fchown(loop, req, file, uid, gid, cb);
}
int dm_uv_fs_lchown(dm_uv_loop_st *loop, dm_uv_fs_st *req, const char *path, dm_uv_uid_t uid,
                    dm_uv_gid_t gid, dm_uv_fs_cb cb)
{
  return uv_fs_lchown(loop, req, path, uid, gid, cb);
}
int dm_uv_fs_statfs(dm_uv_loop_st *loop, dm_uv_fs_st *req, const char *path, dm_uv_fs_cb cb)
{
  return uv_fs_statfs(loop, req, path, cb);
}
int dm_uv_fs_poll_init(dm_uv_loop_st *loop, dm_uv_fs_poll_st *handle)
{
  return uv_fs_poll_init(loop, handle);
}
int dm_uv_fs_poll_start(dm_uv_fs_poll_st *handle, dm_uv_fs_poll_cb poll_cb, const char *path,
                        unsigned int interval)
{
  return uv_fs_poll_start(handle, poll_cb, path, interval);
}
int dm_uv_fs_poll_stop(dm_uv_fs_poll_st *handle)
{
  return uv_fs_poll_stop(handle);
}
int dm_uv_fs_poll_getpath(dm_uv_fs_poll_st *handle, char *buffer, size_t *size)
{
  return uv_fs_poll_getpath(handle, buffer, size);
}
int dm_uv_signal_init(dm_uv_loop_st *loop, dm_uv_signal_st *handle)
{
  return uv_signal_init(loop, handle);
}
int dm_uv_signal_start(dm_uv_signal_st *handle, dm_uv_signal_cb signal_cb, int signum)
{
  return uv_signal_start(handle, signal_cb, signum);
}
int dm_uv_signal_start_oneshot(dm_uv_signal_st *handle, dm_uv_signal_cb signal_cb, int signum)
{
  return uv_signal_start_oneshot(handle, signal_cb, signum);
}
int dm_uv_signal_stop(dm_uv_signal_st *handle)
{
  return uv_signal_stop(handle);
}
void dm_uv_loadavg(double avg[3])
{
  uv_loadavg(avg);
}
int dm_uv_fs_event_init(dm_uv_loop_st *loop, dm_uv_fs_event_st *handle)
{
  return uv_fs_event_init(loop, handle);
}
int dm_uv_fs_event_start(dm_uv_fs_event_st *handle, dm_uv_fs_event_cb cb, const char *path,
                         unsigned int flags)
{
  return uv_fs_event_start(handle, cb, path, flags);
}
int dm_uv_fs_event_stop(dm_uv_fs_event_st *handle)
{
  return uv_fs_event_stop(handle);
}
int dm_uv_fs_event_getpath(dm_uv_fs_event_st *handle, char *buffer, size_t *size)
{
  return uv_fs_event_getpath(handle, buffer, size);
}
int dm_uv_ip4_addr(const char *ip, int port, struct sockaddr_in *addr)
{
  return uv_ip4_addr(ip, port, addr);
}
int dm_uv_ip6_addr(const char *ip, int port, struct sockaddr_in6 *addr)
{
  return uv_ip6_addr(ip, port, addr);
}
int dm_uv_ip4_name(const struct sockaddr_in *src, char *dst, size_t size)
{
  return uv_ip4_name(src, dst, size);
}
int dm_uv_ip6_name(const struct sockaddr_in6 *src, char *dst, size_t size)
{
  return uv_ip6_name(src, dst, size);
}
int dm_uv_ip_name(const struct sockaddr *src, char *dst, size_t size)
{
  return uv_ip_name(src, dst, size);
}
int dm_uv_inet_ntop(int af, const void *src, char *dst, size_t size)
{
  return uv_inet_ntop(af, src, dst, size);
}
int dm_uv_inet_pton(int af, const char *src, void *dst)
{
  return uv_inet_pton(af, src, dst);
}
int dm_uv_random(dm_uv_loop_st *loop, dm_uv_random_st *req, void *buf, size_t buflen,
                 unsigned flags, dm_uv_random_cb cb)
{
  return uv_random(loop, req, buf, buflen, flags, cb);
}
int dm_uv_if_indextoname(unsigned int ifindex, char *buffer, size_t *size)
{
  return uv_if_indextoname(ifindex, buffer, size);
}
int dm_uv_if_indextoiid(unsigned int ifindex, char *buffer, size_t *size)
{
  return uv_if_indextoiid(ifindex, buffer, size);
}
int dm_uv_exepath(char *buffer, size_t *size)
{
  return uv_exepath(buffer, size);
}
int dm_uv_cwd(char *buffer, size_t *size)
{
  return uv_cwd(buffer, size);
}
int dm_uv_chdir(const char *dir)
{
  return uv_chdir(dir);
}
uint64_t dm_uv_get_free_memory(void)
{
  return uv_get_free_memory();
}
uint64_t dm_uv_get_total_memory(void)
{
  return uv_get_total_memory();
}
uint64_t dm_uv_get_constrained_memory(void)
{
  return uv_get_constrained_memory();
}
uint64_t dm_uv_get_available_memory(void)
{
  return uv_get_available_memory();
}
int dm_uv_clock_gettime(dm_uv_clock_id_et clock_id, dm_uv_timespec64_st *ts)
{
  return uv_clock_gettime(clock_id, ts);
}
uint64_t dm_uv_hrtime(void)
{
  return uv_hrtime();
}
void dm_uv_sleep(unsigned int msec)
{
  uv_sleep(msec);
}
void dm_uv_disable_stdio_inheritance(void)
{
  uv_disable_stdio_inheritance();
}
int dm_uv_dlopen(const char *filename, dm_uv_lib_st *lib)
{
  return uv_dlopen(filename, lib);
}
void dm_uv_dlclose(dm_uv_lib_st *lib)
{
  uv_dlclose(lib);
}
int dm_uv_dlsym(dm_uv_lib_st *lib, const char *name, void **ptr)
{
  return uv_dlsym(lib, name, ptr);
}
const char *dm_uv_dlerror(const dm_uv_lib_st *lib)
{
  return uv_dlerror(lib);
}
int dm_uv_mutex_init(dm_uv_mutex_t *handle)
{
  return uv_mutex_init(handle);
}
int dm_uv_mutex_init_recursive(dm_uv_mutex_t *handle)
{
  return uv_mutex_init_recursive(handle);
}
void dm_uv_mutex_destroy(dm_uv_mutex_t *handle)
{
  uv_mutex_destroy(handle);
}
void dm_uv_mutex_lock(dm_uv_mutex_t *handle)
{
  uv_mutex_lock(handle);
}
int dm_uv_mutex_trylock(dm_uv_mutex_t *handle)
{
  return uv_mutex_trylock(handle);
}
void dm_uv_mutex_unlock(dm_uv_mutex_t *handle)
{
  uv_mutex_unlock(handle);
}
int dm_uv_rwlock_init(dm_uv_rwlock_t *rwlock)
{
  return uv_rwlock_init(rwlock);
}
void dm_uv_rwlock_destroy(dm_uv_rwlock_t *rwlock)
{
  uv_rwlock_destroy(rwlock);
}
void dm_uv_rwlock_rdlock(dm_uv_rwlock_t *rwlock)
{
  uv_rwlock_rdlock(rwlock);
}
int dm_uv_rwlock_tryrdlock(dm_uv_rwlock_t *rwlock)
{
  return uv_rwlock_tryrdlock(rwlock);
}
void dm_uv_rwlock_rdunlock(dm_uv_rwlock_t *rwlock)
{
  uv_rwlock_rdunlock(rwlock);
}
void dm_uv_rwlock_wrlock(dm_uv_rwlock_t *rwlock)
{
  uv_rwlock_wrlock(rwlock);
}
int dm_uv_rwlock_trywrlock(dm_uv_rwlock_t *rwlock)
{
  return uv_rwlock_trywrlock(rwlock);
}
void dm_uv_rwlock_wrunlock(dm_uv_rwlock_t *rwlock)
{
  uv_rwlock_wrunlock(rwlock);
}
int dm_uv_sem_init(dm_uv_sem_t *sem, unsigned int value)
{
  return uv_sem_init(sem, value);
}
void dm_uv_sem_destroy(dm_uv_sem_t *sem)
{
  uv_sem_destroy(sem);
}
void dm_uv_sem_post(dm_uv_sem_t *sem)
{
  uv_sem_post(sem);
}
void dm_uv_sem_wait(dm_uv_sem_t *sem)
{
  uv_sem_wait(sem);
}
int dm_uv_sem_trywait(dm_uv_sem_t *sem)
{
  return uv_sem_trywait(sem);
}
int dm_uv_cond_init(dm_uv_cond_t *cond)
{
  return uv_cond_init(cond);
}
void dm_uv_cond_destroy(dm_uv_cond_t *cond)
{
  uv_cond_destroy(cond);
}
void dm_uv_cond_signal(dm_uv_cond_t *cond)
{
  uv_cond_signal(cond);
}
void dm_uv_cond_broadcast(dm_uv_cond_t *cond)
{
  uv_cond_broadcast(cond);
}
int dm_uv_barrier_init(dm_uv_barrier_t *barrier, unsigned int count)
{
  return uv_barrier_init(barrier, count);
}
void dm_uv_barrier_destroy(dm_uv_barrier_t *barrier)
{
  uv_barrier_destroy(barrier);
}
int dm_uv_barrier_wait(dm_uv_barrier_t *barrier)
{
  return uv_barrier_wait(barrier);
}
void dm_uv_cond_wait(dm_uv_cond_t *cond, dm_uv_mutex_t *mutex)
{
  uv_cond_wait(cond, mutex);
}
int dm_uv_cond_timedwait(dm_uv_cond_t *cond, dm_uv_mutex_t *mutex, uint64_t timeout)
{
  return uv_cond_timedwait(cond, mutex, timeout);
}
void dm_uv_once(dm_uv_once_t *guard, void (*callback)(void))
{
  uv_once(guard, callback);
}
int dm_uv_key_create(dm_uv_key_t *key)
{
  return uv_key_create(key);
}
void dm_uv_key_delete(dm_uv_key_t *key)
{
  uv_key_delete(key);
}
void *dm_uv_key_get(dm_uv_key_t *key)
{
  return uv_key_get(key);
}
void dm_uv_key_set(dm_uv_key_t *key, void *value)
{
  uv_key_set(key, value);
}
int dm_uv_gettimeofday(dm_uv_timeval64_st *tv)
{
  return uv_gettimeofday(tv);
}
int dm_uv_thread_create(dm_uv_thread_t *tid, dm_uv_thread_cb entry, void *arg)
{
  return uv_thread_create(tid, entry, arg);
}
int dm_uv_thread_detach(dm_uv_thread_t *tid)
{
  return uv_thread_detach(tid);
}
int dm_uv_thread_create_ex(dm_uv_thread_t *tid, const dm_uv_thread_options_st *params,
                           dm_uv_thread_cb entry, void *arg)
{
  return uv_thread_create_ex(tid, params, entry, arg);
}
int dm_uv_thread_setaffinity(dm_uv_thread_t *tid, char *cpumask, char *oldmask, size_t mask_size)
{
  return uv_thread_setaffinity(tid, cpumask, oldmask, mask_size);
}
int dm_uv_thread_getaffinity(dm_uv_thread_t *tid, char *cpumask, size_t mask_size)
{
  return uv_thread_getaffinity(tid, cpumask, mask_size);
}
int dm_uv_thread_getcpu(void)
{
  return uv_thread_getcpu();
}
dm_uv_thread_t dm_uv_thread_self(void)
{
  return uv_thread_self();
}
int dm_uv_thread_join(dm_uv_thread_t *tid)
{
  return uv_thread_join(tid);
}
int dm_uv_thread_equal(const dm_uv_thread_t *t1, const dm_uv_thread_t *t2)
{
  return uv_thread_equal(t1, t2);
}
int dm_uv_thread_setname(const char *name)
{
  return uv_thread_setname(name);
}
int dm_uv_thread_getname(dm_uv_thread_t *tid, char *name, size_t size)
{
  return uv_thread_getname(tid, name, size);
}
void *dm_uv_loop_get_data(const dm_uv_loop_st *arg1)
{
  return uv_loop_get_data(arg1);
}
void dm_uv_loop_set_data(dm_uv_loop_st *arg1, void *data)
{
  uv_loop_set_data(arg1, data);
}
size_t dm_uv_utf16_length_as_wtf8(const uint16_t *utf16, ssize_t utf16_len)
{
  return uv_utf16_length_as_wtf8(utf16, utf16_len);
}
int dm_uv_utf16_to_wtf8(const uint16_t *utf16, ssize_t utf16_len, char **wtf8_ptr,
                        size_t *wtf8_len_ptr)
{
  return uv_utf16_to_wtf8(utf16, utf16_len, wtf8_ptr, wtf8_len_ptr);
}
ssize_t dm_uv_wtf8_length_as_utf16(const char *wtf8)
{
  return uv_wtf8_length_as_utf16(wtf8);
}
void dm_uv_wtf8_to_utf16(const char *wtf8, uint16_t *utf16, size_t utf16_len)
{
  uv_wtf8_to_utf16(wtf8, utf16, utf16_len);
}
