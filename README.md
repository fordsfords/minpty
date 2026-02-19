# minpty

Simple example of using a pseudo-TTY to drive a sub-process.

Huge thanks to Claude.ai for writing this! I certify that I have reviewed the Unix code carefully.
Unfortunately, I don't have the Windows expertise to make the same claim for minconpty.c.

## Table of contents

<!-- mdtoc-start -->
&bull; [minpty](#minpty)  
&nbsp;&nbsp;&nbsp;&nbsp;&bull; [Table of contents](#table-of-contents)  
&nbsp;&nbsp;&nbsp;&nbsp;&bull; [Introduction](#introduction)  
&nbsp;&nbsp;&nbsp;&nbsp;&bull; [Included Scripts](#included-scripts)  
&nbsp;&nbsp;&nbsp;&nbsp;&bull; [Windows Notes](#windows-notes)  
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&bull; [Why handle_vt_queries() exists](#why-handle_vt_queries-exists)  
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&bull; [Underdocumented ConPTY behaviors](#underdocumented-conpty-behaviors)  
&nbsp;&nbsp;&nbsp;&nbsp;&bull; [License](#license)  
<!-- TOC created by '../mdtoc/mdtoc.pl ./README.md' (see https://github.com/fordsfords/mdtoc) -->
<!-- mdtoc-end -->

## Introduction

The `minpty` Linux program runs a command inside a pseudo-TTY so that the
child process believes it is connected to a real terminal.
This is the same basic mechanism used by `expect`, terminal
emulators, and similar tools.

`minconpty` is the Windows equivalent program.

My expectation is that this code will be used as cut-and-paste fodder.

Usage (Linux):
````
minpty <command> [args...]
````

For example:
````
./minpty vim myfile.txt <vi_cmds.txt >vi_output.log
````
This will run the vim text editor which will think it is connected
to an interactive terminal but actually is getting its commands
from a file. Note that the log file will contain cursor addressing
sequences as would be sent to a terminal.

See the source code for detailed design notes.

## Included Scripts

* `bld.sh` script compiles `minpty` with gcc.

* `tst.sh` script runs `bld.sh` and then does a basic test with vim.

* `bld.bat` batch file compiles `minconpty` with cl.

* `tst.bat` batch file runs `bld.bat` and then does a basic test with vim.
  (Expects vim to be installed and on the PATH.)

## Windows Notes

### Why handle_vt_queries() exists

On Unix, the kernel's PTY layer is a transparent bidirectional byte pipe.
The kernel doesn't interpret VT sequences at all.
Queries originate from the child, flow passively through the PTY master side,
and most Unix programs handle missing responses gracefully via timeouts
or avoid runtime queries entirely by using terminfo/termcap.

ConPTY is fundamentally different.
It has `conhost.exe` sitting in the middle, actively interpreting the VT
stream in both directions.
Conhost itself generates VT query sequences (DSR, Device Attributes, etc.)
on the output pipe and expects responses on the input pipe — these queries
don't even originate from the child process.
When running headless with no real terminal attached, nothing answers
those queries unless `handle_vt_queries()` does.

Beyond conhost's own queries, many child programs (shells, ncurses/readline
programs, vim, less, etc.) send DA or DSR sequences during startup regardless
of whether they're being scripted or used interactively.
Unanswered queries can cause hangs or degraded behavior.
Since the whole point of ConPTY is to make the child believe it has a real
terminal, something needs to be answering.

### Underdocumented ConPTY behaviors

Several behaviors discovered during development are poorly documented
by Microsoft:

**Handle inheritance into conhost.**
`CreatePseudoConsole()` internally spawns `conhost.exe`, which inherits
all inheritable handles from the calling process.
To prevent handle leakage, you must explicitly clear the inherit flag
on every handle that conhost shouldn't receive (our ends of the pipes,
stdin, stdout) using `SetHandleInformation()`.
Only the two pipe ends that ConPTY needs (`pty_in_rd`, `pty_out_wr`)
should remain inheritable.

**Standard handle propagation via the PEB.**
Windows propagates the parent's standard handles to the child process
through the Process Environment Block (PEB), regardless of the
`bInheritHandles` parameter to `CreateProcess`.
If the parent's stdin/stdout are redirected to files, the child receives
those file handles *instead of* the ConPTY's console handles.
The workaround is to temporarily clear the parent's standard handles
(`SetStdHandle(..., NULL)`) before `CreateProcess` and restore them
immediately after.
This forces the child to get its handles exclusively from the
pseudo-console.

**Pipe lifetime across ClosePseudoConsole.**
`CreatePseudoConsole()` may not duplicate the pipe handles it receives
(`pty_in_rd`, `pty_out_wr`) internally.
Closing these handles before calling `ClosePseudoConsole()` can cause
the child to see a premature EOF and exit.
Both must remain open until after the pseudo-console is closed.

**ESC byte disambiguation.**
ConPTY's VT parser cannot distinguish a bare Escape keypress from the
first byte of a multi-byte VT escape sequence without a timing gap.
When feeding input, a short delay (ESC_DELAY_MS) after each ESC byte
gives the parser time to recognize it as a standalone keypress.

## License

I want there to be NO barriers to using this code, so I am releasing it to the public domain.  But "public domain" does not have an internationally agreed upon definition, so I use CC0:

This work is dedicated to the public domain under CC0 1.0 Universal:
http://creativecommons.org/publicdomain/zero/1.0/

To the extent possible under law, Steven Ford has waived all copyright
and related or neighboring rights to this work. In other words, you can 
use this code for any purpose without any restrictions.
This work is published from: United States.
Project home: https://github.com/fordsfords/minpty
