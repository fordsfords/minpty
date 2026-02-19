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
&nbsp;&nbsp;&nbsp;&nbsp;&bull; [License](#license)  
<!-- TOC created by '../mdtoc/mdtoc.pl ./README.md' (see https://github.com/fordsfords/mdtoc) -->
<!-- mdtoc-end -->

## Introduction

The `minpty` program runs a command inside a pseudo-TTY so that the
child process believes it is connected to a real terminal.
This is the same basic mechanism used by `script(1)`, `expect`, terminal
emulators, and similar tools.

The `minconpty` is the Windows equivalent program.

My expectation is that this code will be used as cut-and-paste fodder.

Usage (Linux):
````
minpty <command> [args...]
````

For example:
````
./minpty vi myfile.txt
````

See the source code for detailed design notes.

## Included Scripts

* `bld.sh` script compiles `minpty` with gcc.

* `tst.sh` script runs `bld.sh` and then does a basic test.

* `bld.bat` batch file compiles `minconpty` with cl.

* `tst.bat` batch file runs `bld.bat` and then does a basic test.

## License

I want there to be NO barriers to using this code, so I am releasing it to the public domain.  But "public domain" does not have an internationally agreed upon definition, so I use CC0:

This work is dedicated to the public domain under CC0 1.0 Universal:
http://creativecommons.org/publicdomain/zero/1.0/

To the extent possible under law, Steven Ford has waived all copyright
and related or neighboring rights to this work. In other words, you can 
use this code for any purpose without any restrictions.
This work is published from: United States.
Project home: https://github.com/fordsfords/minpty
