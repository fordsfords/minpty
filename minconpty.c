/* minconpty.c - A pseudo-TTY launcher for Windows
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

/* Uses the ConPTY API (Windows 10 1809+) to run a child process
 * inside a pseudo-console.  The child believes it has a real
 * console, enabling automation of interactive console programs
 * (similar to Unix "expect").
 *
 * Data flows through pipes:
 *   our stdin  -->  pty input pipe  -->  child's console input
 *   child's console output  -->  pty output pipe  -->  our stdout
 *
 * Works with redirected stdin/stdout:
 *   minconpty cmd < input.txt > output.log
 *
 * Also works interactively (stdin/stdout connected to a console).
 *
 * Requires Windows SDK 10.0.17763.0 or later for ConPTY headers.
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

/* Delay in ms after writing ESC to pty input.  Gives ConPTY's VT
 * parser time to recognize a bare Escape keypress vs. the start
 * of a VT escape sequence. */
#define ESC_DELAY_MS 50

/* Data I/O handles - may be console, file, or pipe. */
static HANDLE g_data_in  = INVALID_HANDLE_VALUE;
static HANDLE g_data_out = INVALID_HANDLE_VALUE;


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
  if (cmd == NULL) { return NULL; }

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

  if (!CreatePipe(pty_in_rd, pty_in_wr, &sa, 0)) { return -1; }
  if (!CreatePipe(pty_out_rd, pty_out_wr, &sa, 0)) { return -1; }

  return 0;
}  /* create_pty_pipes */


/* ----------------------------------------------------------------
 * VT query detection and synthetic response generation.
 *
 * ConPTY's internal conhost sends VT query sequences and expects
 * responses in its input stream.  Normally a real terminal provides
 * these, but when running headless (no console) there's nothing to
 * answer.  We scan ConPTY output for common queries and inject
 * synthetic responses into the pty input pipe.
 *
 * Handled queries:
 *   ESC[6n   DSR cursor position  ->  ESC[1;1R
 *   ESC[5n   DSR device status    ->  ESC[0n
 *   ESC[c    Primary DA           ->  ESC[?1;2c
 *   ESC[0c   Primary DA           ->  ESC[?1;2c
 *   ESC[>c   Secondary DA         ->  ESC[>0;0;0c
 *   ESC[>0c  Secondary DA         ->  ESC[>0;0;0c
 * ----------------------------------------------------------------
 */

#define VT_NORMAL  0
#define VT_ESC     1
#define VT_CSI     2

static void handle_vt_queries(const char *buf, DWORD len,
                              HANDLE pty_in_wr) {
  static int state = VT_NORMAL;
  static char csi_buf[64];
  static int csi_len = 0;

  DWORD i;
  for (i = 0; i < len; i++) {
    unsigned char c = (unsigned char)buf[i];

    switch (state) {
    case VT_NORMAL:
      if (c == 0x1B)
        state = VT_ESC;
      break;

    case VT_ESC:
      if (c == '[') {
        state = VT_CSI;
        csi_len = 0;
      } else {
        state = VT_NORMAL;
      }
      break;

    case VT_CSI:
      if (csi_len < (int)sizeof(csi_buf) - 1)
        csi_buf[csi_len++] = (char)c;

      /* CSI final bytes are 0x40-0x7E. */
      if (c >= 0x40 && c <= 0x7E) {
        const char *resp = NULL;
        DWORD n_written;

        csi_buf[csi_len] = '\0';

        if (strcmp(csi_buf, "6n") == 0) {
          resp = "\x1b[1;1R";         /* cursor at row 1, col 1 */
        } else if (strcmp(csi_buf, "5n") == 0) {
          resp = "\x1b[0n";           /* device OK */
        } else if (strcmp(csi_buf, "c") == 0 ||
                   strcmp(csi_buf, "0c") == 0) {
          resp = "\x1b[?1;2c";        /* VT100 with AVO */
        } else if (strcmp(csi_buf, ">c") == 0 ||
                   strcmp(csi_buf, ">0c") == 0) {
          resp = "\x1b[>0;0;0c";      /* secondary DA */
        }

        if (resp != NULL) {
          WriteFile(pty_in_wr, resp, (DWORD)strlen(resp),
                    &n_written, NULL);
        }

        state = VT_NORMAL;
      }
      break;
    }  /* switch */
  }  /* for */
}  /* handle_vt_queries */


/* Context passed to the output thread. */
typedef struct {
  HANDLE pty_out_rd;   /* read child output from here */
  HANDLE pty_in_wr;    /* write VT responses back here */
} pty_out_ctx;


/*
 * Thread: read from data stdin, write to pty input pipe.
 * Runs until read fails (EOF) or pipe write fails (child exited).
 *
 * When stdin is a console, writes pass through directly (the
 * human provides natural pacing).  When stdin is a file or pipe,
 * writes are paced byte-by-byte to handle ESC disambiguation.
 */
static DWORD WINAPI stdin_to_pty(LPVOID arg) {
  HANDLE pty_in_wr = (HANDLE)arg;
  char buf[BUF_SIZE];
  DWORD n_read, n_written;
  DWORD i;

  while (ReadFile(g_data_in, buf, sizeof(buf), &n_read, NULL) && n_read > 0) {
    /*
     * Write the buffer to the pty input pipe, one byte at a time.
     * After each ESC byte, pause to let ConPTY's VT parser timeout
     * recognize it as a bare Escape keypress rather than the start
     * of a VT escape sequence.
     */
    for (i = 0; i < n_read; i++) {
      if (!WriteFile(pty_in_wr, &buf[i], 1, &n_written, NULL))
        break;

      if (buf[i] == '\x1b')
        Sleep(ESC_DELAY_MS);
    }
  }

  return 0;
}  /* stdin_to_pty */


/*
 * Thread: read from pty output pipe, write to data stdout.
 * Also scans for VT queries and injects synthetic responses.
 * Runs until pipe breaks (child exited, ConPTY closed).
 */
static DWORD WINAPI pty_to_stdout(LPVOID arg) {
  pty_out_ctx *ctx = (pty_out_ctx *)arg;
  char buf[BUF_SIZE];
  DWORD n_read, n_written;

  while (ReadFile(ctx->pty_out_rd, buf, sizeof(buf), &n_read, NULL)
         && n_read > 0) {
    handle_vt_queries(buf, n_read, ctx->pty_in_wr);
    WriteFile(g_data_out, buf, n_read, &n_written, NULL);
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

  g_data_in  = GetStdHandle(STD_INPUT_HANDLE);
  g_data_out = GetStdHandle(STD_OUTPUT_HANDLE);

  /* Create pipes for ConPTY communication. */
  HANDLE pty_in_rd, pty_in_wr, pty_out_rd, pty_out_wr;
  if (create_pty_pipes(&pty_in_rd, &pty_in_wr,
                       &pty_out_rd, &pty_out_wr) < 0) {
    fprintf(stderr, "Failed to create pipes.\n");
    return 1;
  }

  /*
   * Prevent handle leakage into conhost.  CreatePseudoConsole()
   * internally spawns conhost.exe, which inherits all inheritable
   * handles.  Lock down everything except the two pipe ends that
   * ConPTY needs (pty_in_rd, pty_out_wr).
   */
  SetHandleInformation(pty_in_wr, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(pty_out_rd, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(g_data_in, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(g_data_out, HANDLE_FLAG_INHERIT, 0);

  /*
   * CreatePseudoConsole() creates a hidden console backed by
   * conhost.exe.  The child will be attached to it.  ConPTY reads
   * child input from pty_in_rd and writes output to pty_out_wr.
   * Fixed 80x24 - no need to probe a real console.
   */
  COORD con_size = { 80, 24 };
  HPCON hpc;
  hr = CreatePseudoConsole(con_size, pty_in_rd, pty_out_wr, 0, &hpc);
  if (FAILED(hr)) {
    fprintf(stderr, "CreatePseudoConsole failed: 0x%08lX\n",
            (unsigned long)hr);
    return 1;
  }

  /*
   * Keep pty_in_rd and pty_out_wr open until after ClosePseudoConsole.
   * CreatePseudoConsole may not duplicate these handles internally,
   * so closing them here can cause the child to see EOF and exit.
   */

  /*
   * Build a PROC_THREAD_ATTRIBUTE_LIST with the pseudo-console
   * attribute.  This tells CreateProcess to attach the child to
   * our ConPTY instead of inheriting our real console.
   */
  SIZE_T attr_size = 0;
  InitializeProcThreadAttributeList(NULL, 1, 0, &attr_size);

  LPPROC_THREAD_ATTRIBUTE_LIST attr_list =
      (LPPROC_THREAD_ATTRIBUTE_LIST)malloc(attr_size);
  if (attr_list == NULL) { return 1; }

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
  if (cmd_line == NULL) { return 1; }

  STARTUPINFOEXA si;
  memset(&si, 0, sizeof(si));
  si.StartupInfo.cb = sizeof(si);
  si.lpAttributeList = attr_list;

  PROCESS_INFORMATION pi;
  memset(&pi, 0, sizeof(pi));

  /*
   * Temporarily clear the parent's standard handles.  Windows
   * propagates these to the child via the PEB regardless of
   * bInheritHandles.  If stdin/stdout are redirected to files,
   * the child would receive those file handles instead of the
   * ConPTY's console handles.  Clearing them forces the child
   * to get its handles exclusively from the pseudo-console.
   */
  HANDLE prev_in  = GetStdHandle(STD_INPUT_HANDLE);
  HANDLE prev_out = GetStdHandle(STD_OUTPUT_HANDLE);
  HANDLE prev_err = GetStdHandle(STD_ERROR_HANDLE);
  SetStdHandle(STD_INPUT_HANDLE, NULL);
  SetStdHandle(STD_OUTPUT_HANDLE, NULL);
  SetStdHandle(STD_ERROR_HANDLE, NULL);

  BOOL ok = CreateProcessA(
      NULL, cmd_line, NULL, NULL, FALSE,
      EXTENDED_STARTUPINFO_PRESENT, NULL, NULL,
      &si.StartupInfo, &pi);

  /* Restore parent's handles immediately. */
  SetStdHandle(STD_INPUT_HANDLE, prev_in);
  SetStdHandle(STD_OUTPUT_HANDLE, prev_out);
  SetStdHandle(STD_ERROR_HANDLE, prev_err);

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
   * Spawn two I/O threads.  Windows can't easily poll a console
   * handle and a pipe handle together, so threads are the natural
   * approach.  The output thread also handles the VT feedback loop.
   */
  pty_out_ctx out_ctx;
  out_ctx.pty_out_rd = pty_out_rd;
  out_ctx.pty_in_wr = pty_in_wr;

  HANDLE h_in_thread = CreateThread(
      NULL, 0, stdin_to_pty, pty_in_wr, 0, NULL);
  HANDLE h_out_thread = CreateThread(
      NULL, 0, pty_to_stdout, &out_ctx, 0, NULL);

  /* Wait for child to exit. */
  WaitForSingleObject(pi.hProcess, INFINITE);

  DWORD exit_code = 1;
  GetExitCodeProcess(pi.hProcess, &exit_code);

  /*
   * Shut down ConPTY.  This closes the internal pipe endpoints,
   * which breaks the pty_to_stdout thread's ReadFile loop.
   */
  ClosePseudoConsole(hpc);

  /* Wait for the output thread to drain and finish. */
  if (h_out_thread != NULL)
    WaitForSingleObject(h_out_thread, 2000);

  /*
   * The stdin thread may be blocked on ReadFile.  If stdin is a
   * console, CancelSynchronousIo unblocks it.  If stdin is a file
   * or pipe, the read will complete on its own (EOF or broken pipe).
   */
  if (h_in_thread != NULL) {
    CancelSynchronousIo(h_in_thread);
    WaitForSingleObject(h_in_thread, 2000);
  }

  /* Clean up. */
  CloseHandle(pty_in_rd);
  CloseHandle(pty_out_wr);
  CloseHandle(pty_in_wr);
  CloseHandle(pty_out_rd);
  if (h_in_thread != NULL) CloseHandle(h_in_thread);
  if (h_out_thread != NULL) CloseHandle(h_out_thread);
  CloseHandle(pi.hProcess);
  DeleteProcThreadAttributeList(attr_list);
  free(attr_list);

  fprintf(stderr, "\n[minconpty: child exited with status %lu]\n",
          exit_code);
  return (int)exit_code;
}  /* main */
