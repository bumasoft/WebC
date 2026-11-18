// server_lifecycle_test.c — a server must die when it is asked to.
//
// The dev loop in `webc dev` restarts the app process on every save: SIGTERM the old one,
// wait for it, fork the new one. If the old one does not exit, the supervisor's waitpid
// blocks and the app is down until a human notices (measured: 269 failed requests across one
// `touch`, and the app never came back).
//
// The cause is one line of bookkeeping nobody can see: `wtask_shutdown()` waits for
// `g_live == 0`, and the accept task is live by construction, parked on the listener. The
// kernel drops a closed fd's kqueue registration silently, so closing the listener leaves
// that task parked forever and the process unkillable. Two things are tested here: the
// process exits, quickly, on SIGTERM — and on a listener that is closed underneath it.
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <netinet/in.h>

#include <webc/test.h>
#include <webc/server.h>
#include <webc/web.h>

GET("/lifecycle/ok") { return res_text(200, str_lit("ok")); }

// A child that serves until signalled, then exits with 0.
static pid_t spawn_server(u16 port) {
  pid_t pid = fork();
  if (pid != 0) return pid;
  char addr[64];
  snprintf(addr, sizeof addr, "127.0.0.1:%u", (unsigned)port);
  webc_serve(&(WebcOptions){.addr = addr, .quiet = true, .cpus = 2});
  _exit(0);
}

// Is anything accepting? A refused connection means "not yet".
static bool listening(u16 port) {
  i32 fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return false;
  struct sockaddr_in in = {0};
  in.sin_family = AF_INET;
  in.sin_port = htons(port);
  in.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  bool ok = connect(fd, (struct sockaddr *)&in, sizeof in) == 0;
  close(fd);
  return ok;
}

static u16 pick_port(void) { return (u16)(19000 + (getpid() % 3000)); }

// Wait for the child to be gone. 0 = exited, 1 = still alive after `ms`.
static i32 wait_gone(pid_t pid, i32 ms, i32 *status_out) {
  for (i32 waited = 0; waited < ms; waited += 10) {
    int st = 0;
    pid_t r = waitpid(pid, &st, WNOHANG);
    if (r == pid) {
      if (status_out) *status_out = st;
      return 0;
    }
    usleep(10 * 1000);
  }
  if (status_out) *status_out = -1;
  return 1;
}

TEST(sigterm_exits_the_server_quickly) {
  u16 port = pick_port();
  pid_t pid = spawn_server(port);
  ASSERT_TRUE(pid > 0);
  for (i32 i = 0; i < 200 && !listening(port); i++) usleep(10 * 1000);
  ASSERT_TRUE(listening(port));

  kill(pid, SIGTERM);
  i32 st = 0;
  // 500 ms, not 1.5 s: falling back to "the stall monitor's sleep has to expire" costs a
  // full second, and a dev loop that pauses a second per save is the thing being fixed.
  i32 alive = wait_gone(pid, 500, &st);
  if (alive) kill(pid, SIGKILL);
  ASSERT_EQ(alive, 0);
  ASSERT_TRUE(WIFEXITED(st));
  ASSERT_EQ(WEXITSTATUS(st), 0);
  // the port must be free again: a leaked listener is what makes the *next* start fail
  ASSERT_FALSE(listening(port));
}

// A listener closed by somebody else must still release a parked accept: this is the
// invariant to hold. It is checked without a fork, because the failure mode
// (a task parked forever) is a hang inside one process, not an exit code.
static i32 g_accept_fd;
static _Atomic int g_accept_ended;

// false from wfd_accept means "the wait ended without a connection": either the
// listener broke or something woke us. Both are what shutdown needs.
static void accept_once(void *arg) {
  (void)arg;
  i32 fd = -1;
  if (!wfd_accept(g_accept_fd, &fd)) atomic_store(&g_accept_ended, 1);
  if (fd >= 0) close(fd);
}

TEST(closing_the_listener_under_accept_releases_the_task) {
  u16 port = pick_port() + 2;
  char addr[64];
  snprintf(addr, sizeof addr, "127.0.0.1:%u", (unsigned)port);
  u16 got = 0;
  i32 lfd = wfd_open_listen(addr, &got);
  ASSERT_TRUE(lfd >= 0);

  // A task that parks in accept() and records what it saw.
  atomic_store(&g_accept_ended, 0);
  g_accept_fd = lfd;
  task_async(accept_once, nullptr);
  wc_sleep_ms(30); // let it park

  // what a shutdown does, in this order: break the socket, wake the waiter, close
  shutdown(lfd, SHUT_RDWR);
  wtask_wake_fd(lfd);
  wc_sleep_ms(50);
  ASSERT_EQ(atomic_load(&g_accept_ended), 1);
  close(lfd);
}

TEST_MAIN()