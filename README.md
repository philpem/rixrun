# Rixrun

v0.1 19 Feb 2022


Run Acorn RISCiX binaries from the comfort of your own laptop!
This program is a simple userspace instruction/syscall emulator that
can run _unsqueezed_ RISCiX ZMAGIC and SPZMAGIC binaries.

It uses ARMulator to run arm26 code, and with some shockingly-simple
SWI syscall emulation it's able to run some basic programs.

My personal motivation for this is to run the RISCiX compilers on Linux,
so programs/drivers can be "cross-developed" without a cross-compiler.  To
that end, rixrun can now run cc and ld:

```
$ export RIX_ROOT=/path/to/riscix/
$ rixrun usr/lib/cc -help
Norcroft Unix ARM C 345/345/345/dbx [Oct 29 1990, 12:46:07]

Usage :       ./cc [-options] file1 file2 ... filen

Main options:

-ansi          Compile ANSI-style C source code
-pcc           Compile BSD UNIX PCC-style C source code
-c             Do not invoke the linker to link the files being compiled
-C             Prevent the preprocessor from removing comments (Use with -E)
-D<symbol>     Define preprocessor <symbol> on entry to the compiler
-E             Preprocess the C source code only, do not compile or link it
-F<options>    Enable a selection of compiler defined features
-g             Generate code that may be used with the debugger
-I<directory>  Include <directory> on the '#include' search path
-list          Generate a compilation listing
-M<options>    Generate a 'makefile' style list of dependencies
-o <file>      Instruct the linker to name the object code produced <file>
-O             Invoke the object code improver
-p<options>    Generate code to produce 'profile' information
-R             Place all compile time strings in a 'Read only' segment
-S             Generate ARM assembly code instead of object code
-U<symbol>     Undefine preprocessor <symbol> on entry to the compiler
-w<options>    Disable all or selected warning and error messages
```

This tool can compile and link a simple helloworld.c into a working binary.
C345's automatic link step runs the guest shell and linker. Prepare unsqueezed
copies of `usr/lib/cc`, `usr/bin/sh`, `sbin/sh` and `usr/bin/ld` beneath
`RIX_ROOT`, then compile from your working directory:

```
$ rixrun "$RIX_ROOT/usr/lib/cc" hello.c -o hello
$ rixrun ./hello
Hello world!
```

Headers and libraries use the installation beneath `RIX_ROOT`; source and
output files use the working directory. The BSD/X/Open frontend options also
need an unsqueezed `usr/bin/symrename`.

## Limitations

Many!  Only a handful of very simple syscalls are implemented so far:

   * Terminal ioctls are unsupported and return `ENOTTY`; interactive terminal use is unsupported
   * No networking
   * Guest signal handlers and job control are not implemented
   * Only unsqueezed ZMAGIC and SPZMAGIC executables are supported, including after exec

Experimental `fork`, `vfork`, `execve` and wait support uses one host process
per guest. `vfork` provides fork semantics, with private child memory and no
parent suspension. Failed exec leaves the caller intact; successful exec
preserves the guest PID, children and open descriptors.

Guest PIDs are positive 16-bit IDs local to a process and its direct children.
They do not provide a global PID namespace. Wait supports a particular child,
any child and the current host process group, with `WNOHANG` and `WUNTRACED`.
Resource usage is still reported as zero.

See [the process implementation notes](docs/fork-experimental.md) for the plan,
ABI details, corner cases and validation. Run `make check` for self-contained
ARM26 process tests; no RISCiX installation is needed.


## Usage

### Environment variables

`RIX_ROOT` indicates the host path to a RISCiX installation. It locates shared
libraries and prefixes absolute filenames passed to guest `execve`, so that
`/usr/bin/ld` executes `$RIX_ROOT/usr/bin/ld`. Those executables must be
unsqueezed and have execute permission; host executables are not run as a fallback.

Guest file operations resolve paths beneath `/usr/lib` and `/usr/include`
inside `RIX_ROOT` too. This includes the compiler's default headers, startup
object, libraries and symbol-renaming tables. Other file paths, relative exec
filenames and the initial command-line filename use host paths. This is not a
chroot.

`RIX_VERBOSE` can be set to `1` or `2` for increasing debug output:  syscall
trace and instruction execution trace.


### Squeezedness

RISCiX binaries are by default squeezed.  (If you haven't already, go read about their novel
on-demand executable decompression, it's really cool!)  AFAIK the algorithm isn't documented anwhere, and I haven't reverse-engineered the algorithm for this,
so the current ZMAGIC loader can only load unsqueezed binaries.

As a workaround, this tool can execute an already-unsqueezed `unsqueeze` binary, in order to
prepare any given binary for execution.  Solve the chicken & egg by unsqueezing `unsqueeze` from RISCiX itself.

`unsqueeze` writes to a temporary file and then invokes a guest shell and `cp`
to move the output. Its final copy step needs a runnable guest `cp`. If that
step fails, copy the temporary output to the right place manually.


# Licence

Copyright 2022 Matt Evans (and ARMulator authors, and small portions borrowed from QEMU's linux-user binary loaders).

This program is licenced under GPLv2.
