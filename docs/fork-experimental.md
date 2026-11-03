# Experimental process support

The aim is to run C345's automatic compile-and-link workflow with one host
process per guest process. Keep the existing ARMulator and flat memory model.

## Implementation plan

1. Replace the saved-state vfork workaround with host `fork()`. Return the
   RISCiX fork child flag in `r1`; vfork returns zero directly in the child.
   Flush host diagnostics before forking so inherited stdio buffers cannot be
   flushed twice. Guest exit flushes diagnostics and uses `_exit()` without
   running host exit handlers.
2. Map direct children to positive signed 16-bit guest PIDs. Keep the mapping
   until a child is reaped; a forked child starts with an empty child table.
   Preserve its own guest PID and child table across exec. Return the parent's
   guest PID in r1 for libc getppid. Support waiting for
   a particular child, any child, or children in the host process group.
3. Implement waitpid, the older register-returning wait syscall used by libc,
   and wait4. Translate option bits and signal numbers; write status only when
   a child reports an event. Validate output pointers before reaping. Continue
   the existing zero-filled resource-usage approximation.
4. Decode exec filename, argv and envp with bounds checks and an aggregate
   argument limit. Preserve argv[0] independently of the filename. Keep the
   host environment (including RIX_ROOT) separate from the guest environment.
5. Load exec into a zeroed temporary 32 MiB image using a copy of the initial
   CPU state. On failure, discard it and return the error to the old guest.
   On success, copy it into guest memory, install the new CPU state and flush
   the instruction pipeline. Reinstall the floating-point emulator. Make the
   loader's text cursor local to each load, check segment lengths and argument
   stack space, and reject truncated files rather than executing partial data.
   Support static ZMAGIC programs as well as shared SPZMAGIC programs: C345's
   shell wrapper execs the statically linked /sbin/sh.
6. Resolve absolute exec filenames beneath RIX_ROOT when it is set; resolve
   relative exec filenames in the host working directory. Resolve /usr/lib
   and /usr/include consistently beneath RIX_ROOT for guest file operations;
   keep working-file paths and the initial command-line filename as host paths.
   Execute only supported guest images; remove the host-shell cp special case.
   Implement chmod so the linker can make its output executable before an exec.
7. Add self-contained synthetic ARM26 process tests and a make check target.
   Exercise both syscall results and execution through the emulator, including
   the instruction immediately after successful and failed exec.
8. Implement stat and real/effective UID/GID queries needed by C345 and its
   non-interactive shell. Fail unsupported terminal ioctls with ENOTTY instead
   of reporting pipes as terminals. Include the separately committed ARM STM
   saved-PC fix from fix-signed-operations; no STM regression test was added.

## Boundaries and corner cases

- vfork provides fork semantics: private memory and no parent suspension.
- Guest PIDs identify the process and its direct children locally. They are
  not globally unique and cannot safely identify temporary files shared by
  independent emulator instances. The initial process uses PID 1 and parent
  PID 0. Parent IDs are recorded at fork; reparenting is not emulated. Negative
  process-group selectors are unsupported; job control, guest signal handlers
  and kill remain outside scope.
- Preserve mappings for stopped children. WNOHANG must leave output memory
  untouched when returning zero. A second wait after reaping returns ECHILD.
- Translate normal exits, stopped signals and fatal signals to the RISCiX
  wait layout. Host signals with no RISCiX equivalent are reported as SIGSYS.
- Failed exec must preserve memory, registers other than the error return,
  file descriptors and PID mappings, including after a partial library load.
- Successful exec must clear old memory and registers, preserve descriptors
  and file offsets, and use the requested guest environment. No guest syscall
  currently sets close-on-exec flags; adding fcntl will need to implement them.
- Null argv/envp vectors are treated as empty. A null filename or a non-null
  invalid string/vector returns EFAULT. The combined argument stack is limited
  to 24,576 bytes, including strings, pointer tables and argc.
- Reject unterminated or out-of-range strings/vectors, oversized arguments,
  invalid wait buffers/options, bad executable formats, missing libraries,
  overflowing segments and exhausted guest PID slots with guest errors.
- The loader still has the project's existing host-endian assumptions and
  limited executable-format support. Process support does not add shebangs,
  executable decompression, a chroot, or host-command fallback.
- Path remapping recognises complete /usr/lib and /usr/include components;
  names such as /usr/library retain their host meaning. All supported pathname
  syscalls use the same policy, including create, link, unlink and chmod.
- Signal-related syscalls remain no-ops. Terminal ioctl layouts are not
  translated; valid descriptors return ENOTTY and invalid ones return EBADF.
  This supports the compiler's non-interactive shell path, not job control.

## ABI evidence and validation

The local RISCiX installation's usr/include/sys/{syscall,types,wait}.h,
usr/include/signal.h and libc c:9010241403.11 establish the syscall numbers,
signed 16-bit pid_t, wait layout and signal numbering. Disassembly shows
fork testing r1 bit zero, vfork returning r0 directly, and wait syscall 84
returning status in r1. Its wait3 wrapper passes options in r0 and a resource
usage pointer in r1. The getppid wrapper uses getpid syscall 20's r1 result.

Tests generate small unsqueezed executables and a shared-library fixture;
they need Python 3 and a host C compiler, but no RISCiX distribution or ARM
toolchain. Run make check. Real compiler/linker validation additionally needs
unsqueezed RISCiX tools and libraries.

All 24 process tests pass with GCC at -O3 and Clang at -O2 with AddressSanitizer
and UndefinedBehaviorSanitizer. An additional native-handler probe checked
SIGUSR1 translation to signal 30, stopped-child retention and PID exhaustion.

## Norcroft stress test

The test in tests/norcroft-fork.c alternates fork and vfork for 32 child
processes. Each child checks a failed exec and then runs the original linker.
The parent checks memory isolation, child status and ECHILD after reaping.

With unsqueezed tools, set RIX_CC, RIX_LD and RIX_SYMRENAME to their host
filenames, RIX_INCLUDE and RIX_LIB to the installation's include and library
directories, and RIXRUN to the absolute path of this checkout's executable.
Prepare an RIX_ROOT containing the shared libraries and an unsqueezed
usr/bin/ld. In a separate writable build directory, compile a small input
program, then compile and run the harness:

```sh
printf 'int main(void) { return 42; }\n' > hello.c
"$RIXRUN" "$RIX_CC" -c "-j$RIX_INCLUDE" hello.c -o hello.o
"$RIXRUN" "$RIX_CC" -c "-j$RIX_INCLUDE" /path/to/rixrun/tests/norcroft-fork.c -o probe.o
"$RIXRUN" "$RIX_SYMRENAME" "-$RIX_LIB/BSD" probe.o
"$RIXRUN" "$RIX_LD" probe.o "$RIX_LIB/crt0.o" "-L$RIX_LIB" -lc -o probe
"$RIXRUN" ./probe hello.o "$RIX_LIB/crt0.o" "-L$RIX_LIB"
"$RIXRUN" ./linked
# Expected: 32 linker children passed; linked exits with status 42.
```

Validation used copies of the tools under /mnt/zfs/dev/riscix, with outputs
in a separate /tmp build directory; the supplied installation was unchanged.
Norcroft C345 (29 October 1990) compiled the harness without warnings. The
original symrename applied BSD.R before linking. Four concurrent harnesses
completed 128 linker children, and all four generated executables returned 42.

The 300/331 compiler (27 July 1989) reaches help with the existing page-root
compatibility library. Compilation stops at unsupported syscall 5 (oopen).
With the unadjusted old library it instead starts at the wrong address and
hits syscall 76; that is a pre-existing library-placement issue.

## C345 automatic compile-and-link validation

C345's automatic link command invokes /usr/bin/sh, which then execs /sbin/sh.
After unsqueezing, /sbin/sh has static ZMAGIC (0x10b). Static loading skips
shared-library traversal and uses the existing segment and stack layout.
ARM STM must save the instruction address plus 12 so the shell's setjmp can
recognise the caller prologue; that correction is a separate CPU commit.

Integrated validation used a temporary RIX_ROOT with unsqueezed usr/bin/sh,
sbin/sh, usr/bin/ld and usr/bin/symrename. Its usr/lib and usr/include pointed
to the supplied installation. C345 ran without -j or -L overrides:

- A single-file stdio program compiled, linked and printed its expected output.
- Two translation units compiled, linked and printed answer=42.
- An optimised (-O) build compiled, linked and ran successfully.
- The BSD frontend compiled and automatically renamed/linked the fork harness;
  its 32 alternating fork/vfork linker children passed.
- Syntax errors and undefined symbols both returned nonzero compiler status.
- Successful builds produced no shell prompt or other stderr output.

The same C345 builds, generated programs, failure cases and 32-child harness
also passed with AddressSanitizer and UndefinedBehaviorSanitizer enabled.

The BSD invocation passes its frontend file as the first source argument:

```sh
"$RIXRUN" "$RIX_CC" -ZiBSD "$RIX_ROOT/usr/ucb/cc" /path/to/rixrun/tests/norcroft-fork.c -o probe
```

The older compiler's syscall-5 ABI remains deferred; C345 is the acceptance
target for this experimental branch.
