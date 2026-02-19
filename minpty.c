/* minpty.c - A pseudo-TTY launcher for Windows
 * See https://github.com/fordsfords/minpty for documentation. */

/* This work is dedicated to the public domain under CC0 1.0 Universal:
 * http://creativecommons.org/publicdomain/zero/1.0/
 * 
 * To the extent possible under law, Steven Ford has waived all copyright
 * and related or neighboring rights to this work. In other words, you can 
 * use this code for any purpose without any restrictions.
 * This work is published from: United States.
 * Project home: https://github.com/fordsfords/minpty
 */

/* Demonstrates how programs like script(1) work:
 *   1. Create a pseudo-TTY pair (master/slave)
 *   2. Fork a child that runs a command on the slave side
 *   3. Parent shuttles data between stdin/stdout and the pty master
 *
 * The child process believes it's running on a real terminal.
 *
 * Usage: minpty <command> [args...]
 *
 * Design notes:
 *   - Uses poll() for multiplexed I/O (no threads needed)
 *   - Uses forkpty() which handles the pty allocation, fork, and
 *     slave-side setup (setsid, ioctl TIOCSCTTY, dup2) in one call
 *   - Detects child exit via POLLHUP on the master fd + waitpid()
 *   - Puts the real terminal into raw mode so keystrokes pass through
 *     immediately (Ctrl-C, arrow keys, tab completion all work)
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

/* Buffer size for read/write shuttling. */
#define BUF_SIZE 4096

/* Global so the signal handler can set it. */
static volatile sig_atomic_t child_exited = 0;
static volatile sig_atomic_t child_status = 0;

/* For signal handler access. */
static int g_master_fd = -1;


static void sigchld_handler(int sig) {
  (void)sig;
  int status;
  /* Reap the child (non-blocking). We only have one child. */
  if (waitpid(-1, &status, WNOHANG) > 0) {
    child_status = status;
    child_exited = 1;
  }
}  /* sigchld_handler */


/*
 * Put the real terminal (if any) into raw mode so that:
 *   - Characters are passed through immediately (no line buffering)
 *   - Special keys (Ctrl-C, Ctrl-Z, etc.) aren't intercepted by the
 *     outer terminal driver -- they go straight to the child's pty
 *   - The child's terminal handles all line editing and echo
 *
 * Returns 0 on success, -1 if stdin isn't a terminal.
 * Saves original settings into *saved for later restoration.
 */
static int set_raw_mode(struct termios *saved) {
  struct termios raw;

  if (tcgetattr(STDIN_FILENO, saved) < 0) { return -1; }  /* Handle error. */

  raw = *saved;
  cfmakeraw(&raw);

  /* Keep output processing so \n -> \r\n still works on our stdout. */
  raw.c_oflag |= OPOST;

  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) < 0) { return -1; }  /* Handle error. */

  return 0;
}  /* set_raw_mode */


static void restore_terminal(const struct termios *saved) {
  tcsetattr(STDIN_FILENO, TCSAFLUSH, saved);
}  /* restore_terminal */


/*
 * Propagate the real terminal's window size to the pty master
 * so the child sees the correct ROWS x COLS.
 */
static void copy_window_size(int master_fd) {
  struct winsize ws;
  if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == 0)
    ioctl(master_fd, TIOCSWINSZ, &ws);
}  /* copy_window_size */


/*
 * Handle SIGWINCH: when the outer terminal is resized, propagate
 * the new size to the child's pty.
 */
static void sigwinch_handler(int sig) {
  (void)sig;
  if (g_master_fd >= 0)
    copy_window_size(g_master_fd);
}  /* sigwinch_handler */


/*
 * Main I/O loop: shuttle bytes between stdin<->master and master<->stdout
 * using poll() for multiplexed, non-blocking I/O.
 *
 *   stdin  ------>  pty master  (user keystrokes -> child's tty input)
 *   stdout <------  pty master  (child's tty output -> our display)
 */
static void io_loop(int master_fd) {
  char buf[BUF_SIZE];
  struct pollfd fds[2];

  /*
   * fds[0] = pty master  (always poll for child output)
   * fds[1] = stdin        (poll for user input)
   */
  fds[0].fd     = master_fd;
  fds[0].events = POLLIN;

  fds[1].fd     = STDIN_FILENO;
  fds[1].events = POLLIN;

  while (!child_exited) {
    int ret = poll(fds, 2, 100 /* ms, allows periodic child_exited check */);

    if (ret < 0) {
      if (errno == EINTR)
        continue;  /* Interrupted by SIGCHLD or SIGWINCH. */
      break;       /* Real error. */
    }

    /* Child's pty produced output. */
    if (fds[0].revents & POLLIN) {
      ssize_t n = read(master_fd, buf, sizeof(buf));
      if (n > 0) {
        write(STDOUT_FILENO, buf, n);
      } else if (n <= 0) {
        break;  /* EOF or error on master -- child side closed. */
      }
    }

    /* Master side hung up (child closed its slave fd or exited). */
    if (fds[0].revents & (POLLHUP | POLLERR)) {
      /* Drain any remaining output first. */
      for (;;) {
        ssize_t n = read(master_fd, buf, sizeof(buf));
        if (n <= 0) break;
        write(STDOUT_FILENO, buf, n);
      }
      break;
    }

    /* User typed something on stdin. */
    if (fds[1].revents & POLLIN) {
      ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
      if (n > 0) {
        write(master_fd, buf, n);
      } else if (n <= 0) {
        /*
         * stdin EOF (e.g. pipe closed or user typed Ctrl-D at
         * the outer level). We could close master to signal
         * the child, but just stop reading stdin.
         */
        fds[1].fd = -1;  /* Tell poll() to skip this fd. */
      }
    }

    if (fds[1].revents & (POLLHUP | POLLERR)) {
      fds[1].fd = -1;
    }
  }
}  /* io_loop */


int main(int argc, char *argv[]) {
  if (argc < 2) {
    fprintf(stderr, "Usage: %s <command> [args...]\n", argv[0]);
    fprintf(stderr, "\nRuns <command> inside a pseudo-TTY.\n");
    fprintf(stderr, "The child thinks it's on a real terminal.\n");
    return 1;
  }

  /* Set up signal handlers. */
  struct sigaction sa;

  /* SIGCHLD: detect child exit. */
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = sigchld_handler;
  sa.sa_flags   = SA_RESTART;
  sigaction(SIGCHLD, &sa, NULL);

  /* SIGWINCH: propagate terminal resize. */
  sa.sa_handler = sigwinch_handler;
  sigaction(SIGWINCH, &sa, NULL);

  /*
   * forkpty() does the heavy lifting. In one call, it:
   *   1. Opens a pty master/slave pair (like openpty)
   *   2. Forks
   *   3. In the child:
   *      - Creates a new session (setsid)
   *      - Sets the slave as the controlling terminal
   *      - Dups the slave to stdin/stdout/stderr
   *      - Closes the master fd
   *   4. Returns the master fd to the parent
   */
  int master_fd;
  pid_t pid = forkpty(&master_fd, NULL, NULL, NULL);
  if (pid < 0) {
    perror("forkpty");
    return 1;
  }

  if (pid == 0) {
    /* Child process.
     * Running with the pty slave as stdin/stdout/stderr.
     * As far as we know, we're on a real terminal.
     */
    execvp(argv[1], &argv[1]);
    perror("execvp");
    _exit(127);
  }

  /* Parent process. */
  g_master_fd = master_fd;

  /* Copy the real terminal's size to the child's pty. */
  copy_window_size(master_fd);

  /*
   * Put the real terminal into raw mode. Without it:
   *   - Keystrokes are line-buffered (must press Enter)
   *   - Ctrl-C kills us instead of reaching the child
   *   - Arrow keys, tab completion, etc. don't work
   */
  struct termios saved_termios;
  int is_tty = (set_raw_mode(&saved_termios) == 0);

  io_loop(master_fd);

  /* Restore the terminal before printing exit message. */
  if (is_tty)
    restore_terminal(&saved_termios);

  close(master_fd);

  /* Make sure we've reaped the child. */
  if (!child_exited) {
    int status;
    waitpid(pid, &status, 0);
    child_status = status;
    child_exited = 1;
  }

  /* Report how the child exited. */
  if (WIFEXITED(child_status)) {
    int code = WEXITSTATUS(child_status);
    fprintf(stderr, "\n[minpty: child exited with status %d]\n", code);
    return code;
  } else if (WIFSIGNALED(child_status)) {
    int sig = WTERMSIG(child_status);
    fprintf(stderr, "\n[minpty: child killed by signal %d (%s)]\n",
            sig, strsignal(sig));
    return 128 + sig;
  }

  return 0;
}  /* main */
