# Experimental process support

Rixrun supports C345's automatic compile-and-link workflow with one host
process per guest process. It uses ARMulator and the existing flat memory
model. This document describes the implementation, ABI details and limits.

## Implementation

- **Process creation.** Guest `fork` and `vfork` use host `fork()`. The
  `fork` handler returns the RISCiX child flag in `r1`; libc's `vfork` wrapper
  uses the zero result in `r0` directly. Host diagnostics are flushed before
  forking to prevent duplicate output from inherited stdio buffers. Guest
  exit flushes diagnostics and calls `_exit()` without running host exit
  handlers.
- **PID mapping.** Direct children receive positive signed 16-bit guest PIDs.
  Mappings remain until children are reaped. A forked child starts with an
  empty child table; `execve` preserves its PID and child table. The `getpid`
  syscall returns the parent's guest PID in `r1` for libc's `getppid` wrapper.
- **Waiting.** The `waitpid`, `wait4` and older register-returning `wait`
  handlers translate option bits and signal numbers. They validate output
  pointers before reaping and write status only when a child reports an event.
  Resource-usage data is zero-filled.
- **Arguments and environment.** `execve` validates the filename, `argv` and
  `envp`, including their bounds and combined size. It preserves `argv[0]`
  independently of the filename. Host configuration, including `RIX_ROOT`,
  remains separate from the guest environment supplied to `execve`.
- **Image replacement.** `execve` loads into a zeroed temporary 32 MiB image
  with a copy of the initial CPU state and reinstalls the floating-point
  emulator.
  On failure, it discards that image and returns an error to the caller. On
  success, it copies the image into guest memory, installs the CPU state and
  flushes the instruction pipeline. The loader resets its text cursor for
  each image, checks segment and argument-stack bounds, and rejects truncated
  files. It supports static ZMAGIC and shared SPZMAGIC programs; C345's shell
  wrapper execs the statically linked `/sbin/sh`.
- **Paths and file operations.** With `RIX_ROOT` set, absolute `execve`
  filenames resolve beneath it. Relative filenames use the host working
  directory. Guest file operations also resolve `/usr/lib` and `/usr/include`
  beneath `RIX_ROOT`; other working-file paths and the initial command-line
  filename remain host paths. Guest `chmod` lets the linker make its output
  executable. The previous host-shell `cp` special case has been removed.
- **Checks.** `make check` runs self-contained ARM26 process fixtures. They
  exercise syscall results and execution after successful and failed `execve`
  calls.
- **Compiler support.** The `stat` and real/effective UID/GID queries support
  C345 and its non-interactive shell. Unsupported terminal ioctls return
  `ENOTTY`, so pipes are not reported as terminals. The ARMulator `STM`
  correction lets the shell recognise its caller's saved PC, as described
  under [C345 validation](#c345-automatic-compile-and-link-validation).

## Behaviour and limitations

- `vfork` provides `fork` semantics: private child memory and no parent suspension.
- Guest PIDs identify the process and its direct children locally. They are
  not globally unique. Programs that form temporary filenames from guest PIDs
  can collide across emulator instances. The initial process uses PID 1 and
  parent PID 0. Parent IDs are recorded at fork; reparenting is not emulated.
  Negative process-group selectors, job control, guest signal handlers and
  `kill` are unsupported.
- Stopped children retain their PID mappings. When `WNOHANG` returns zero,
  output memory is unchanged. A second wait after reaping returns `ECHILD`.
- Normal exits, stopped signals and fatal signals use the RISCiX wait-status
  layout. Host signals with no RISCiX equivalent are reported as `SIGSYS`.
- A failed `execve` preserves memory, file descriptors, PID mappings and CPU
  state except for the syscall error result and carry flag. This also applies
  after a partial library load.
- A successful `execve` replaces the old memory and registers, preserves file
  descriptors and their offsets, and uses the supplied guest environment.
  No guest syscall currently sets close-on-exec flags; adding `fcntl` support
  would also require implementing those flags.
- Null `argv` and `envp` vectors are treated as empty. A null filename or an
  invalid guest string or vector returns `EFAULT`. The argument block is
  limited to 24,576 bytes, including strings, pointer tables and `argc`.
- Invalid wait buffers or options, unsupported executable formats, missing
  libraries, overflowing segments and exhausted guest PID slots return guest
  errors. Unterminated strings and oversized argument blocks are rejected.
- Parts of the loader still assume host endianness matches the guest, and
  executable-format support is limited. Process support does not add shebangs,
  a chroot, or host-command fallback. Squeezed-image loading is described in
  [the squeeze format notes](squeezed-binaries.md).
- System-path remapping recognises complete `/usr/lib` and `/usr/include`
  components; names such as `/usr/library` retain their host meaning. This
  applies to `open`, `creat`, `link`, `unlink`, `access`, `chmod` and `stat`.
- The `sigvec`, `sigblock`, `sigsetmask` and `sigstack` handlers remain no-ops.
  Terminal ioctl layouts are not translated: valid descriptors return
  `ENOTTY` and invalid ones return `EBADF`.

## ABI evidence and validation

The ABI references in the RISCiX installation used for validation were
`usr/include/sys/{syscall,types,wait}.h`, `usr/include/signal.h` and
`usr/lib/c:9010241403.11`. They establish the syscall numbers, signed 16-bit
`pid_t`, wait-status layout and signal numbering. Disassembly of libc showed:

| Interface | Register convention |
| --- | --- |
| `fork` | The wrapper tests bit 0 of `r1` to identify the child. |
| `vfork` | The wrapper uses the result in `r0` directly. |
| `wait`, syscall 84 | Status is returned in `r1`; the `wait3` wrapper passes options in `r0` and a resource-usage pointer in `r1`. |
| `getppid` | The wrapper uses `r1` from `getpid`, syscall 20. |

The process checks generate small unsqueezed executables and a shared-library
fixture. Run `make check` with Python 3 and a host C compiler installed; no
RISCiX distribution or ARM toolchain is required. Testing the original
compiler and linker additionally requires a RISCiX installation.

The 24 process checks passed with GCC at `-O3` and Clang at `-O2` with
AddressSanitizer and UndefinedBehaviorSanitizer enabled in the host emulator.
A separate native-handler probe checked `SIGUSR1` translation to signal 30,
stopped-child retention and PID exhaustion.

## Norcroft stress test

The harness in [tests/norcroft-fork.c](../tests/norcroft-fork.c) alternates
`fork` and `vfork` for 32 child processes. Each child checks a failed `execve`
and then runs the original linker. The parent checks memory isolation, child
status and `ECHILD` after reaping.

Set these variables before running the harness:

| Variable | Value |
| --- | --- |
| `RIXRUN` | Absolute host path to the built `rixrun` executable |
| `RIX_ROOT` | RISCiX installation containing the shared libraries and `usr/bin/ld` |
| `RIX_CC`, `RIX_LD`, `RIX_SYMRENAME` | Host filenames of the RISCiX compiler, linker and symbol-renaming tool |
| `RIX_INCLUDE`, `RIX_LIB` | Host paths to the installation's include and library directories |

In a separate writable build directory, compile the input program and
harness, then run them. Replace `/path/to/rixrun` with the checkout's absolute
path:

```sh
printf 'int main(void) { return 42; }\n' > hello.c
"$RIXRUN" "$RIX_CC" -c "-j$RIX_INCLUDE" hello.c -o hello.o
"$RIXRUN" "$RIX_CC" -c "-j$RIX_INCLUDE" \
    "/path/to/rixrun/tests/norcroft-fork.c" -o probe.o
"$RIXRUN" "$RIX_SYMRENAME" "-$RIX_LIB/BSD" probe.o
"$RIXRUN" "$RIX_LD" probe.o "$RIX_LIB/crt0.o" "-L$RIX_LIB" -lc -o probe
"$RIXRUN" ./probe hello.o "$RIX_LIB/crt0.o" "-L$RIX_LIB"
"$RIXRUN" ./linked
```

The harness should print `32 linker children passed`; `linked` should exit
with status 42.

Stress testing used tools from `/mnt/zfs/dev/riscix`, with build products in
temporary directories under `/tmp`. The installation was unchanged. Norcroft
C345 (29 October 1990) compiled the harness without warnings. The original
`symrename` applied `BSD.R` before linking. Four concurrent harnesses completed
128 linker children, and all four generated executables returned 42.

The C300/331 compiler (27 July 1989) reached its help output with an adjusted
library mapping, but compilation stopped at unsupported syscall 5 (`oopen`).
With its unadjusted library it entered code at the wrong address and reached
syscall 76. Older-ABI support remains outside the C345 target.

## C345 automatic compile-and-link validation

C345's automatic link command invokes `/usr/bin/sh`, which then execs
`/sbin/sh`. The latter uses static ZMAGIC (`0x10b`): the loader skips
shared-library traversal and uses the existing segment and stack layout.
ARM `STM` must save the instruction address plus 12 for `r15` so the shell's
`setjmp` can recognise the caller's prologue. ARMulator includes that
correction.

Initial validation used a temporary `RIX_ROOT` containing unsqueezed copies
of `usr/bin/sh`, `sbin/sh`, `usr/bin/ld` and `usr/bin/symrename`. Its `usr/lib`
and `usr/include` pointed to the installation used for validation. C345 ran
without `-j` or `-L` overrides:

- A single-file program using stdio compiled, linked and printed its expected output.
- Two translation units compiled, linked and printed `answer=42`.
- An optimised (`-O`) build compiled, linked and ran successfully.
- The BSD frontend compiled the fork harness, renamed its symbols and linked
  it automatically; its 32 alternating `fork`/`vfork` linker children passed.
- Syntax errors and undefined symbols both returned nonzero compiler status.
- Successful builds produced no shell prompt or other stderr output.

The same C345 builds, generated programs, failure cases and 32-child harness
also passed with AddressSanitizer and UndefinedBehaviorSanitizer enabled in
the host emulator. Subsequent validation ran the original squeezed tools
directly; see [the squeeze validation results](squeezed-binaries.md#validation).

To use the BSD frontend, pass its file as the first source argument:

```sh
"$RIXRUN" "$RIX_CC" -ZiBSD "$RIX_ROOT/usr/ucb/cc" \
    "/path/to/rixrun/tests/norcroft-fork.c" -o probe
```

C345 is the acceptance target for this experimental branch; the older
compiler's syscall-5 ABI remains deferred.
