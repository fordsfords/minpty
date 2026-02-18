/*
 * minconpty - A simplified pseudo-TTY launcher for Windows
 *
 * Windows equivalent of minpty using the ConPTY API (available
 * since Windows 10 version 1809, October 2018 Update).
 *
 * Demonstrates how terminal emulators work on Windows:
 *   1. Create a pseudo-console (ConPTY) with pipes
 *   2. Launch a child process attached to the pseudo-console
 *   3. Shuttle data between our console and the ConPTY pipes
 *
 * The child process believes it's running on a real console.
 *
 * Usage: minconpty <command> [args...]
 *
 * Build (MSVC):
 *   cl /W4 minconpty.c
 *
 * Build (MinGW-w64):
 *   gcc -Wall -o minconpty.exe minconpty.c
 *
 * Requires Windows SDK 10.0.17763.0 or later for ConPTY headers.
 *
 * Design notes:
 *   - Uses CreatePseudoConsole() for pty allocation
 *   - Uses two threads for I/O shuttling (stdin->pty, pty->stdout)
 *     since Windows lacks poll()/select() for console + pipe mixing
 *   - Sets the real console to raw mode with VT sequence pass-through
 *   - Propagates console window size to the pseudo-console
 */

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00  /* Windows 10 */
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Buffer size for read/write shuttling. */
#define BUF_SIZE 4096

/* Console handles, global for thread access. */
static HANDLE g_con_in  = INVALID_HANDLE_VALUE;
static HANDLE g_con_out = INVALID_HANDLE_VALUE;


/*
 * Build a single command-line string from argv[1..argc-1].
 * The caller must free the returned string.
 */
static char *build_cmd_line(int argc, char *argv[]) {
  size_t len = 0;
  int i;

  for (i = 1; i < argc; i++) {
    len += strlen(argv[i]) + 1;  /* +1 for space or null terminator. */
  }

  char *cmd = (char *)malloc(len);
  if (cmd == NULL) { return NULL; }  /* Handle error. */

  cmd[0] = '\0';
  for (i = 1; i < argc; i++) {
    if (i > 1) strcat(cmd, " ");
    strcat(cmd, argv[i]);
  }

  return cmd;
}  /* build_cmd_line */


/*
 * Create two pipes for ConPTY communication.
 *   pty_in_rd / pty_in_wr:  we write to wr, ConPTY reads from rd
 *   pty_out_rd / pty_out_wr: ConPTY writes to wr, we read from rd
 */
static int create_pty_pipes(HANDLE *pty_in_rd, HANDLE *pty_in_wr,
                            HANDLE *pty_out_rd, HANDLE *pty_out_wr) {
  SECURITY_ATTRIBUTES sa;
  memset(&sa, 0, sizeof(sa));
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;

  if (!CreatePipe(pty_in_rd, pty_in_wr, &sa, 0)) { return -1; }  /* Handle error. */
  if (!CreatePipe(pty_out_rd, pty_out_wr, &sa, 0)) { return -1; }  /* Handle error. */

  return 0;
}  /* create_pty_pipes */


/*
 * Get the console window size as a COORD for CreatePseudoConsole.
 * Falls back to 80x24 if the console info isn't available.
 */
static COORD get_console_size(void) {
  CONSOLE_SCREEN_BUFFER_INFO csbi;
  COORD size;

  size.X = 80;
  size.Y = 24;

  if (GetConsoleScreenBufferInfo(g_con_out, &csbi)) {
    size.X = csbi.srWindow.Right - csbi.srWindow.Left + 1;
    size.Y = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
  }

  return size;
}  /* get_console_size */


/*
 * Set the real console to raw mode for pass-through:
 *   - Input: disable line editing, echo; enable VT input sequences
 *   - Output: enable VT sequence processing
 *
 * Returns 0 on success, -1 if not a console.
 * Saves original modes for later restoration.
 */
static int set_raw_mode(DWORD *saved_in, DWORD *saved_out) {
  if (!GetConsoleMode(g_con_in, saved_in)) { return -1; }  /* Handle error. */
  if (!GetConsoleMode(g_con_out, saved_out)) { return -1; }  /* Handle error. */

  DWORD raw_in = ENABLE_VIRTUAL_TERMINAL_INPUT;
  DWORD raw_out = ENABLE_PROCESSED_OUTPUT
                | ENABLE_VIRTUAL_TERMINAL_PROCESSING
                | DISABLE_NEWLINE_AUTO_RETURN;

  if (!SetConsoleMode(g_con_in, raw_in)) { return -1; }  /* Handle error. */
  if (!SetConsoleMode(g_con_out, raw_out)) { return -1; }  /* Handle error. */

  return 0;
}  /* set_raw_mode */


static void restore_console(DWORD saved_in, DWORD saved_out) {
  SetConsoleMode(g_con_in, saved_in);
  SetConsoleMode(g_con_out, saved_out);
}  /* restore_console */


/*
 * Thread: read from real console stdin, write to pty input pipe.
 * Runs until console read fails or pipe write fails.
 */
static DWORD WINAPI stdin_to_pty(LPVOID arg) {
  HANDLE pty_in_wr = (HANDLE)arg;
  char buf[BUF_SIZE];
  DWORD n_read, n_written;

  while (ReadFile(g_con_in, buf, sizeof(buf), &n_read, NULL) && n_read > 0) {
    if (!WriteFile(pty_in_wr, buf, n_read, &n_written, NULL))
      break;
  }

  return 0;
}  /* stdin_to_pty */


/*
 * Thread: read from pty output pipe, write to real console stdout.
 * Runs until the pipe is broken (child exited, ConPTY closed).
 */
static DWORD WINAPI pty_to_stdout(LPVOID arg) {
  HANDLE pty_out_rd = (HANDLE)arg;
  char buf[BUF_SIZE];
  DWORD n_read, n_written;

  while (ReadFile(pty_out_rd, buf, sizeof(buf), &n_read, NULL) && n_read > 0) {
    WriteFile(g_con_out, buf, n_read, &n_written, NULL);
  }

  return 0;
}  /* pty_to_stdout */


int main(int argc, char *argv[]) {
  HRESULT hr;

  if (argc < 2) {
    fprintf(stderr, "Usage: %s <command> [args...]\n", argv[0]);
    fprintf(stderr, "\nRuns <command> inside a pseudo-console (ConPTY).\n");
    fprintf(stderr, "The child thinks it's on a real console.\n");
    return 1;
  }

  g_con_in  = GetStdHandle(STD_INPUT_HANDLE);
  g_con_out = GetStdHandle(STD_OUTPUT_HANDLE);

  /* Create pipes for ConPTY communication. */
  HANDLE pty_in_rd, pty_in_wr, pty_out_rd, pty_out_wr;
  if (create_pty_pipes(&pty_in_rd, &pty_in_wr,
                       &pty_out_rd, &pty_out_wr) < 0) {
    fprintf(stderr, "Failed to create pipes.\n");
    return 1;
  }

  /*
   * CreatePseudoConsole() is the Windows analog of forkpty().
   * It creates a hidden console that the child will be attached to.
   * The ConPTY reads child input from pty_in_rd and writes child
   * output to pty_out_wr.
   */
  COORD con_size = get_console_size();
  HPCON hpc;
  hr = CreatePseudoConsole(con_size, pty_in_rd, pty_out_wr, 0, &hpc);
  if (FAILED(hr)) {
    fprintf(stderr, "CreatePseudoConsole failed: 0x%08lX\n", (unsigned long)hr);
    return 1;
  }

  /* ConPTY now owns these pipe ends; close our copies. */
  CloseHandle(pty_in_rd);
  CloseHandle(pty_out_wr);

  /*
   * Build a PROC_THREAD_ATTRIBUTE_LIST with the pseudo-console
   * attribute. This tells CreateProcess to attach the child to
   * our ConPTY instead of inheriting our real console.
   */
  SIZE_T attr_size = 0;
  InitializeProcThreadAttributeList(NULL, 1, 0, &attr_size);

  LPPROC_THREAD_ATTRIBUTE_LIST attr_list =
      (LPPROC_THREAD_ATTRIBUTE_LIST)malloc(attr_size);
  if (attr_list == NULL) { return 1; }  /* Handle error. */

  if (!InitializeProcThreadAttributeList(attr_list, 1, 0, &attr_size)) {
    fprintf(stderr, "InitializeProcThreadAttributeList failed.\n");
    return 1;
  }

  if (!UpdateProcThreadAttribute(attr_list, 0,
      PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
      hpc, sizeof(hpc), NULL, NULL)) {
    fprintf(stderr, "UpdateProcThreadAttribute failed.\n");
    return 1;
  }

  /* Build command line and launch the child process. */
  char *cmd_line = build_cmd_line(argc, argv);
  if (cmd_line == NULL) { return 1; }  /* Handle error. */

  STARTUPINFOEXA si;
  memset(&si, 0, sizeof(si));
  si.StartupInfo.cb = sizeof(si);
  si.lpAttributeList = attr_list;

  PROCESS_INFORMATION pi;
  memset(&pi, 0, sizeof(pi));

  BOOL ok = CreateProcessA(
      NULL, cmd_line, NULL, NULL, FALSE,
      EXTENDED_STARTUPINFO_PRESENT, NULL, NULL,
      &si.StartupInfo, &pi);
  free(cmd_line);

  if (!ok) {
    fprintf(stderr, "CreateProcess failed: %lu\n", GetLastError());
    ClosePseudoConsole(hpc);
    DeleteProcThreadAttributeList(attr_list);
    free(attr_list);
    return 1;
  }

  CloseHandle(pi.hThread);

  /*
   * Put the real console into raw mode with VT sequence support.
   * Without this:
   *   - Keystrokes are line-buffered (must press Enter)
   *   - Special keys don't produce VT escape sequences
   *   - Child's VT output (colors, cursor movement) won't render
   */
  DWORD saved_in_mode, saved_out_mode;
  int is_console = (set_raw_mode(&saved_in_mode, &saved_out_mode) == 0);

  /*
   * Spawn two I/O threads. Unlike the Unix version which uses poll()
   * to multiplex, Windows can't easily poll a console handle and a
   * pipe handle together, so threads are the natural approach.
   */
  HANDLE h_in_thread = CreateThread(
      NULL, 0, stdin_to_pty, pty_in_wr, 0, NULL);
  HANDLE h_out_thread = CreateThread(
      NULL, 0, pty_to_stdout, pty_out_rd, 0, NULL);

  /* Wait for child to exit. */
  WaitForSingleObject(pi.hProcess, INFINITE);

  DWORD exit_code = 1;
  GetExitCodeProcess(pi.hProcess, &exit_code);

  /*
   * Shut down ConPTY. This closes the internal pipe endpoints,
   * which breaks the pty_to_stdout thread's ReadFile loop.
   */
  ClosePseudoConsole(hpc);

  /* Wait for the output thread to drain and finish. */
  if (h_out_thread != NULL)
    WaitForSingleObject(h_out_thread, 2000);

  /*
   * The stdin thread may be blocked on ReadFile(g_con_in).
   * CancelSynchronousIo unblocks it.
   */
  if (h_in_thread != NULL) {
    CancelSynchronousIo(h_in_thread);
    WaitForSingleObject(h_in_thread, 2000);
  }

  /* Restore console before printing exit message. */
  if (is_console)
    restore_console(saved_in_mode, saved_out_mode);

  /* Clean up. */
  CloseHandle(pty_in_wr);
  CloseHandle(pty_out_rd);
  if (h_in_thread != NULL) CloseHandle(h_in_thread);
  if (h_out_thread != NULL) CloseHandle(h_out_thread);
  CloseHandle(pi.hProcess);
  DeleteProcThreadAttributeList(attr_list);
  free(attr_list);

  fprintf(stderr, "\n[minconpty: child exited with status %lu]\n", exit_code);
  return (int)exit_code;
}  /* main */
