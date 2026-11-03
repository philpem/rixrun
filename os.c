/* rixrun OS emulation routines
 *
 * This "fuse" provides a trivial RISCiX syscall emulation on Linux.
 * There are many shortcuts/hacks and lazinesses within, this is not
 * Good Code (TM)!
 *
 * Copyright (C) 2022 Matt Evans
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version 2
 *  of the License, or (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <signal.h>
#include <limits.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>

#include "utils.h"
#include "armdefs.h"
#include "armemu.h"
#include "rixrun.h"
#include "rix_os.h"
#include "zload.h"

#ifdef __APPLE__
#include <libkern/OSByteOrder.h>
#define htole32(x) OSSwapHostToLittleInt32(x)
#define le32toh(x) OSSwapLittleToHostInt32(x)
#define htole16(x) OSSwapHostToLittleInt16(x)
#define le16toh(x) OSSwapLittleToHostInt16(x)
#else
#include <endian.h>
#endif

#define SC_TRACE
#ifdef SC_TRACE
#define SYSTRACE(x...)          do { if (sc_trace) fprintf(stderr, "SC: " x); } while(0)
#define SYSTRACE_OUT(x...)      do { if (sc_trace) fprintf(stderr, x); } while(0)
#define SDBG(x...)              do { if (sc_trace) fprintf(stderr, x); } while(0)
#else
#define SYSTRACE(x...)          do { } while(0);
#define SYSTRACE_OUT(x...)      do { } while(0);
#define SDBG(x...)              do { } while(0);
#endif

#define SC_1ARG                                                 \
        uint32_t a0 = ARMul_GetReg(state, state->Mode, 0)
#define SC_2ARG                                                 \
        uint32_t a0 = ARMul_GetReg(state, state->Mode, 0);      \
        uint32_t a1 = ARMul_GetReg(state, state->Mode, 1)
#define SC_3ARG                                                 \
        uint32_t a0 = ARMul_GetReg(state, state->Mode, 0);      \
        uint32_t a1 = ARMul_GetReg(state, state->Mode, 1);      \
        uint32_t a2 = ARMul_GetReg(state, state->Mode, 2)
#define SC_4ARG                                                 \
        uint32_t a0 = ARMul_GetReg(state, state->Mode, 0);      \
        uint32_t a1 = ARMul_GetReg(state, state->Mode, 1);      \
        uint32_t a2 = ARMul_GetReg(state, state->Mode, 2);      \
        uint32_t a3 = ARMul_GetReg(state, state->Mode, 3)

#define SC_RET_VAL(fmt, v) do {                                 \
                SYSTRACE_OUT(" = " fmt "\n", v);                \
                ARMul_SetReg(state, state->Mode, 0, v);         \
                CLEARC;                                         \
        } while(0)

#define SC_RET_ERROR(e) do {                                    \
                SYSTRACE_OUT(" = ERR(%d)\n", e);                \
                ARMul_SetReg(state, state->Mode, 0, (e));       \
                SETC;                                           \
        } while(0)

#define SC_RET_NONE()   do {                                    \
                CLEARC;                                         \
        } while(0)


static  int     sc_trace = 0;

/* Startup state only: guest signal delivery is not implemented. */
static uint32_t guest_sigmask;
static uint32_t guest_sigstack[2];
static ARMul_State initial_state;

/* RISCiX pid_t is a signed short. IDs are local to a process and its children;
 * job control and signalling unrelated guest processes are not implemented.
 */
static int guest_pid = 1;
static int guest_ppid = 0;
static pid_t child_pids[INT16_MAX + 1];

////////////////////////////////////////////////////////////////////////////////
// Mappings of stuff

static int      host_to_rix_errno(int e)
{
        switch (e) {
        case EAGAIN:         return 11;
        case ELOOP:          return 62;
        case ENAMETOOLONG:   return 63;
        }
        if (e < 35) {
                return e;
        } else {
                fprintf(stderr, "rixrun: errno %d not mapped, FIXME!\n", e);
                return e;
        }
}

static int      rix_to_host_openflags(int f)
{
        int o = f & 3; // ACCMODE is easy

        if (f & (1<<9))         o |= O_CREAT;
        if (f & (1<<11))        o |= O_EXCL;
        if (f & (1<<12))        o |= O_NOCTTY;
        if (f & (1<<10))        o |= O_TRUNC;
        if (f & (1<<3))         o |= O_APPEND;
        if (f & (1<<14))        o |= O_NONBLOCK;
        if (f & (1<<13))        o |= O_SYNC;

        return o;
}

static unsigned int     host_to_rix_mode(unsigned int im)
{
        unsigned int om = im;

        // S_IFMT is in the same place, and values the same.
        return om;
}

static void     host_to_rix_stat(struct rix_stat *osb, struct stat *isb)
{
        // FIXME: massive endianness crimes committed here!  (Assumes LE :( )
        osb->st_dev = htole16(0x0101);           // Who cares?
        osb->st_ino = htole32(isb->st_ino);      // trunc!
        osb->st_mode = htole16(host_to_rix_mode(isb->st_mode));
        osb->st_nlink = htole16(isb->st_nlink);
        osb->st_uid = htole16(isb->st_uid);      // XXX truncates!
        osb->st_gid = htole16(isb->st_gid);
        osb->st_rdev = htole16(0x0101);
        osb->st_size = htole32(isb->st_size);
#ifdef __APPLE__
        osb->st_a_time = htole32(isb->st_atimespec.tv_sec);
        osb->st_m_time = htole32(isb->st_mtimespec.tv_sec);
        osb->st_c_time = htole32(isb->st_ctimespec.tv_sec);
#else
        osb->st_a_time = htole32(isb->st_atim.tv_sec);
        osb->st_m_time = htole32(isb->st_mtim.tv_sec);
        osb->st_c_time = htole32(isb->st_ctim.tv_sec);
#endif
        /* RISC iX MAXBSIZE is 8192. st_blksize is an I/O hint:
         * passing a ZFS 128 KiB hint wraps original tools' short counters. */
        osb->st_blksize = htole32(isb->st_blksize > 8192 ? 8192 : isb->st_blksize);
	osb->st_blocks = htole32(isb->st_blocks); // Convert to 4K?
        /* SDBG("Stat %04x %08x %03x %04x %04x %04x %04x\n" */
        /*      "   %08x %08x:%08x:%08x %08x %08x\n", */
        /*      osb->st_dev, osb->st_ino, osb->st_mode, osb->st_nlink, */
        /*      osb->st_uid, osb->st_gid, osb->st_rdev, */
        /*      osb->st_size, osb->st_a_time, osb->st_m_time, osb->st_c_time, */
        /*      osb->st_blksize, osb->st_blocks); */
}

static  uint32_t        read32(addr_t a)
{
        uint32_t data;
        memcpy(&data, mem_base + a, sizeof(data));
        return le32toh(data);
}

static  uint8_t         read8(addr_t a)
{
        return *(uint8_t *)(mem_base + a);
}

static  void        	write32(addr_t a, uint32_t data)
{
        data = htole32(data);
        memcpy(mem_base + a, &data, sizeof(data));
}

static int      guest_range(addr_t p, size_t len)
{
        return p < MEM_SIZE && len <= MEM_SIZE - p;
}

static int      guest_string(addr_t p, char **s)
{
        if (!p || !guest_range(p, 1) || !memchr(mem_base + p, 0, MEM_SIZE - p))
                return -EFAULT;
        *s = (char *)mem_base + p;
        return 0;
}

/* System headers and libraries belong to the RISCiX installation. Other
 * working-file paths remain host paths; absolute exec paths are guest paths.
 */
static int      guest_path(addr_t p, char *path, int exec)
{
        char *name;
        int r = guest_string(p, &name);
        if (r < 0) return r;
        const char *root = getenv("RIX_ROOT");
        int system_path = (!strncmp(name, "/usr/lib", 8) &&
                           (name[8] == '/' || name[8] == 0)) ||
                          (!strncmp(name, "/usr/include", 12) &&
                           (name[12] == '/' || name[12] == 0));
        if (root && *root && name[0] == '/' && (exec || system_path))
                r = snprintf(path, PATH_MAX, "%s%s", root, name);
        else
                r = snprintf(path, PATH_MAX, "%s", name);
        return r < 0 || r >= PATH_MAX ? -ENAMETOOLONG : 0;
}

////////////////////////////////////////////////////////////////////////////////

void    rix_sc_exit(ARMul_State *state)
{
        SC_1ARG;
        SYSTRACE("exit(%d)", a0);

        fflush(NULL);
        _exit(a0);
}

void    rix_sc_read(ARMul_State *state)
{
        SC_3ARG;
        SYSTRACE("read(%d, %08x, %08x)", a0, a1, a2);
        int r = read(a0, mem_base + a1, a2);
        if (r < 0)
                SC_RET_ERROR(host_to_rix_errno(errno));
        else
                SC_RET_VAL("%d", r);
}

void    rix_sc_write(ARMul_State *state)
{
        SC_3ARG;
        SYSTRACE("write(%d, %08x, %08x)", a0, a1, a2);
        int r = write(a0, mem_base + a1, a2);
        if (r < 0)
                SC_RET_ERROR(host_to_rix_errno(errno));
        else
                SC_RET_VAL("%d", r);
}

void    rix_sc_close(ARMul_State *state)
{
        SC_1ARG;
        SYSTRACE("close(%d)", a0);
        int r;
        if (a0 > 2)
                r = close(a0);
        else
                r = 0;

        if (r < 0)
                SC_RET_ERROR(host_to_rix_errno(errno));
        else
                SC_RET_VAL("%d", r);
}

void    rix_sc_creat(ARMul_State *state)
{
        SC_2ARG;
        char path[PATH_MAX];
        SYSTRACE("creat(%08x, %08x)", a0, a1);
        int r = guest_path(a0, path, 0);
        if (r == 0) {
                r = creat(path, a1);
                if (r < 0) r = -errno;
        }
        if (r < 0)
                SC_RET_ERROR(host_to_rix_errno(-r));
        else
                SC_RET_VAL("%d", r);
}

void    rix_sc_link(ARMul_State *state)
{
        SC_2ARG;
        char oldpath[PATH_MAX], newpath[PATH_MAX];
        SYSTRACE("link(%08x, %08x)", a0, a1);
        int r = guest_path(a0, oldpath, 0);
        if (r == 0) r = guest_path(a1, newpath, 0);
        if (r == 0 && link(oldpath, newpath) < 0) r = -errno;
        if (r < 0)
                SC_RET_ERROR(host_to_rix_errno(-r));
        else
                SC_RET_VAL("%d", 0);
}

void    rix_sc_unlink(ARMul_State *state)
{
        SC_1ARG;
        char path[PATH_MAX];
        SYSTRACE("unlink(%08x)", a0);
        int r = guest_path(a0, path, 0);
        if (r == 0 && unlink(path) < 0) r = -errno;
        if (r < 0)
                SC_RET_ERROR(host_to_rix_errno(-r));
        else
                SC_RET_VAL("%d", 0);
}

static int      host_to_rix_signal(int sig)
{
        static const int signals[] = {
                [SIGHUP] = 1, [SIGINT] = 2, [SIGQUIT] = 3, [SIGILL] = 4,
                [SIGTRAP] = 5, [SIGABRT] = 6, [SIGFPE] = 8, [SIGKILL] = 9,
                [SIGBUS] = 10, [SIGSEGV] = 11, [SIGSYS] = 12, [SIGPIPE] = 13,
                [SIGALRM] = 14, [SIGTERM] = 15, [SIGURG] = 16, [SIGSTOP] = 17,
                [SIGTSTP] = 18, [SIGCONT] = 19, [SIGCHLD] = 20, [SIGTTIN] = 21,
                [SIGTTOU] = 22, [SIGIO] = 23, [SIGXCPU] = 24, [SIGXFSZ] = 25,
                [SIGVTALRM] = 26, [SIGPROF] = 27, [SIGWINCH] = 28,
                [SIGUSR1] = 30, [SIGUSR2] = 31,
#ifdef SIGEMT
                [SIGEMT] = 7,
#endif
#ifdef SIGLOST
                [SIGLOST] = 29,
#endif
        };
        if (sig > 0 && (size_t)sig < sizeof(signals)/sizeof(signals[0]) && signals[sig])
                return signals[sig];
        return 12;      /* SIGSYS: no RISCiX equivalent */
}

static void     rix_wait(ARMul_State *state, int32_t pid, addr_t statusp,
                     unsigned int options, addr_t usagep)
{
        int status, flags = 0;
        pid_t host_pid = pid;

        if (options & ~3U || pid < -1) {
                SC_RET_ERROR(EINVAL);
                return;
        }
        if ((statusp && !guest_range(statusp, 4)) ||
            (usagep && !guest_range(usagep, 72))) {
                SC_RET_ERROR(EFAULT);
                return;
        }
        if (pid > 0) {
                if (pid > INT16_MAX || !child_pids[pid]) {
                        SC_RET_ERROR(ECHILD);
                        return;
                }
                host_pid = child_pids[pid];
        }
        if (options & 1) flags |= WNOHANG;
        if (options & 2) flags |= WUNTRACED;
        pid_t r = waitpid(host_pid, &status, flags);
        if (r < 0) {
                SC_RET_ERROR(host_to_rix_errno(errno));
                return;
        }
        if (r == 0) {
                SC_RET_VAL("%d", 0);
                return;
        }
        int id;
        for (id = 1; id <= INT16_MAX; id++)
                if (child_pids[id] == r)
                        break;
        if (id > INT16_MAX) {
                SC_RET_ERROR(ECHILD);
                return;
        }
        int guest_status;
        if (WIFSTOPPED(status)) {
                guest_status = (host_to_rix_signal(WSTOPSIG(status)) << 8) | 0x7f;
        } else {
                child_pids[id] = 0;
                if (WIFEXITED(status))
                        guest_status = WEXITSTATUS(status) << 8;
                else {
                        guest_status = host_to_rix_signal(WTERMSIG(status));
#ifdef WCOREDUMP
                        if (WCOREDUMP(status)) guest_status |= 0x80;
#endif
                }
        }
        if (statusp) write32(statusp, guest_status);
        if (usagep) memset(mem_base + usagep, 0, 72);
        ARMul_SetReg(state, state->Mode, 1, guest_status);
        SC_RET_VAL("%d", id);
}

void    rix_sc_waitpid(ARMul_State *state)
{
        SC_3ARG;
        SYSTRACE("waitpid(%d, 0x%x, 0x%x)", (int32_t)a0, a1, a2);
        rix_wait(state, (int32_t)a0, a1, a2, 0);
}

void    rix_sc_wait4(ARMul_State *state)
{
        SC_4ARG;
        SYSTRACE("wait4(%d, 0x%x, 0x%x, 0x%x)", (int32_t)a0, a1, a2, a3);
        /* BSD wait4 uses zero for any child, unlike waitpid's process group. */
        rix_wait(state, a0 ? (int32_t)a0 : -1, a1, a2, a3);
}

void    rix_sc_wait(ARMul_State *state)
{
        SC_2ARG;
        SYSTRACE("wait(0x%x, 0x%x)", a0, a1);
        /* libc wait/wait3 pass options and rusage; status is returned in r1. */
        rix_wait(state, -1, 0, a0, a1);
}

void    rix_sc_sbreak(ARMul_State *state)
{
        static addr_t current_sbrk = 0;
        SC_1ARG;
        SYSTRACE("sbreak(%08x)", a0);
        addr_t old_sbrk = current_sbrk;
        current_sbrk = (int)a0; // FIXME: check for ludicrous values (or negative..)
        SC_RET_VAL("%08x", 0); // FIXME: Check mem limit, return error (ENOMEM?)
}

void    rix_sc_lseek(ARMul_State *state)
{
        SC_3ARG;
        SYSTRACE("lseek(%d, %08x, %d)", a0, a1, a2);
        off_t r = lseek(a0, (rix_off_t)a1, a2);
        if (r < 0)
                SC_RET_ERROR(host_to_rix_errno(errno));
        else if (r > INT32_MAX)
                // The guest's signed 32-bit off_t cannot represent this position.
                SC_RET_ERROR(EFBIG);
        else
                SC_RET_VAL("%d", (rix_off_t)r);
}

void    rix_sc_getpid(ARMul_State *state)
{
        SYSTRACE("getpid()");
        ARMul_SetReg(state, state->Mode, 1, guest_ppid);
        SC_RET_VAL("%d", guest_pid);
}

void    rix_sc_getuid(ARMul_State *state)
{
        SYSTRACE("getuid()");
        ARMul_SetReg(state, state->Mode, 1, geteuid());
        SC_RET_VAL("%u", getuid());
}

void    rix_sc_getgid(ARMul_State *state)
{
        SYSTRACE("getgid()");
        ARMul_SetReg(state, state->Mode, 1, getegid());
        SC_RET_VAL("%u", getgid());
}

/* The non-root RISCiX shell tests supplementary groups before exec.
 * sys/types.h defines gid_t as unsigned short; host gid_t is not its ABI.
 */
void    rix_sc_getgroups(ARMul_State *state)
{
        SC_2ARG;
        SYSTRACE("getgroups(%d, %08x)", (int32_t)a0, a1);
        int n = getgroups(0, NULL);
        if (n < 0) {
                SC_RET_ERROR(host_to_rix_errno(errno));
                return;
        }
        if ((int32_t)a0 < 0 || (a0 && a0 < (unsigned int)n)) {
                SC_RET_ERROR(EINVAL);
                return;
        }
        if (!a0 || !n) {
                SC_RET_VAL("%d", n);
                return;
        }
        if (!a1 || !guest_range(a1, (size_t)n * 2)) {
                SC_RET_ERROR(EFAULT);
                return;
        }
        gid_t *groups = calloc(n, sizeof(*groups));
        if (!groups) {
                SC_RET_ERROR(ENOMEM);
                return;
        }
        int r = getgroups(n, groups);
        if (r >= 0) {
                for (int i = 0; i < r; i++)
                        if (groups[i] > UINT16_MAX) { r = -1; errno = EINVAL; break; }
                if (r >= 0) for (int i = 0; i < r; i++) {
                        uint16_t value = htole16((uint16_t)groups[i]);
                        memcpy(mem_base + a1 + i * 2, &value, 2);
                }
        }
        int error = errno;
        free(groups);
        if (r < 0) SC_RET_ERROR(host_to_rix_errno(error));
        else SC_RET_VAL("%d", r);
}

void    rix_sc_open(ARMul_State *state)
{
        SC_3ARG;
        char path[PATH_MAX];
        SYSTRACE("open(%08x, %08x, %08x)", a0, a1, a2);
        int r = guest_path(a0, path, 0);
        if (r == 0) {
                r = open(path, rix_to_host_openflags(a1), a2);
                if (r < 0) r = -errno;
        }
        if (r < 0)
                SC_RET_ERROR(host_to_rix_errno(-r));
        else
                SC_RET_VAL("%d", r);
}

void    rix_sc_access(ARMul_State *state)
{
        SC_2ARG;
        char path[PATH_MAX];
        SYSTRACE("access(%08x, %08x)", a0, a1);
        int r = guest_path(a0, path, 0);
        if (r == 0 && access(path, a1) < 0) r = -errno;
        if (r < 0)
                SC_RET_ERROR(host_to_rix_errno(-r));
        else
                SC_RET_VAL("%d", 0);
}

void    rix_sc_chmod(ARMul_State *state)
{
        SC_2ARG;
        char path[PATH_MAX];
        SYSTRACE("chmod(%08x, %08x)", a0, a1);
        int r = guest_path(a0, path, 0);
        if (r == 0 && chmod(path, a1) < 0) r = -errno;
        if (r < 0)
                SC_RET_ERROR(host_to_rix_errno(-r));
        else
                SC_RET_VAL("%d", 0);
}

void    rix_sc_stat(ARMul_State *state)
{
        SC_2ARG;
        char path[PATH_MAX];
        struct stat sb;
        SYSTRACE("stat(%08x, %08x)", a0, a1);
        if (!a1 || !guest_range(a1, sizeof(struct rix_stat))) {
                SC_RET_ERROR(EFAULT);
                return;
        }
        int r = guest_path(a0, path, 0);
        if (r == 0 && stat(path, &sb) < 0) r = -errno;
        if (r < 0)
                SC_RET_ERROR(host_to_rix_errno(-r));
        else if (sb.st_size > INT32_MAX)
                SC_RET_ERROR(EFBIG);
        else {
                host_to_rix_stat((struct rix_stat *)(mem_base + a1), &sb);
                SC_RET_VAL("%d", 0);
        }
}

void    rix_sc_ioctl(ARMul_State *state)
{
        SC_2ARG;
        SYSTRACE("ioctl(%d, %08x)", a0, a1);
        /* Terminal ioctl structures differ from the host ABI. Unsupported
         * queries must fail so isatty() does not report pipes as terminals.
         */
        if (fcntl(a0, F_GETFD) < 0)
                SC_RET_ERROR(host_to_rix_errno(errno));
        else
                SC_RET_ERROR(ENOTTY);
}

static int      exec_vector(addr_t p, char ***vector, int *count, size_t *space)
{
        char **v = calloc(RIX_ARG_MAX / 4, sizeof(*v));
        if (!v) return -ENOMEM;
        *vector = v;
        *count = 0;
        while (p) {
                if (!guest_range(p, 4)) return -EFAULT;
                addr_t arg = read32(p);
                if (!arg) break;
                char *s;
                int r = guest_string(arg, &s);
                if (r < 0) return r;
                size_t len = strlen(s) + 1 + 4;
                if (len > RIX_ARG_MAX - *space) return -E2BIG;
                *space += len;
                v[(*count)++] = s;
                p += 4;
        }
        return 0;
}

void    rix_sc_execve(ARMul_State *state)
{
        SC_3ARG;
        char path[PATH_MAX];
        char **argv = NULL, **envp = NULL;
        int argc, envc, r;
        struct stat sb;
        size_t space = 12;      /* argc and terminating argv/envp pointers */
        uint8_t *image = NULL, *old_memory = mem_base;
        ARMul_State next = initial_state;

        SYSTRACE("execve(%08x, %08x, %08x)", a0, a1, a2);
        r = guest_path(a0, path, 1);
        if (r < 0) goto out;
        if (stat(path, &sb) < 0) {
                r = -errno;
                goto out;
        }
        if (!S_ISREG(sb.st_mode)) {
                r = -EACCES;
                goto out;
        }
        if (access(path, X_OK) < 0) {
                r = -errno;
                goto out;
        }
        r = exec_vector(a1, &argv, &argc, &space);
        if (r < 0) goto out;
        r = exec_vector(a2, &envp, &envc, &space);
        if (r < 0) goto out;
        image = calloc(1, MEM_SIZE);
        if (!image) {
                r = -ENOMEM;
                goto out;
        }
        /* Arguments still point into old_memory. Nothing in the old image or
         * CPU state changes until the entire replacement has loaded.
         */
        mem_base = image;
        os_init(&next, sc_trace);
        r = load_zmagic_binary(&next, path, sc_trace, argc, argv, envc, envp);
        mem_base = old_memory;
        if (r == 0) {
                memcpy(mem_base, image, MEM_SIZE);
                *state = next;          /* SetPC in the loader flushes the pipeline */
                SYSTRACE_OUT(" -> loaded\n");
        }
out:
        free(image);
        free(argv);
        free(envp);
        if (r < 0) SC_RET_ERROR(host_to_rix_errno(-r));
}

void    rix_sc_fstat(ARMul_State *state)
{
        SC_2ARG;
        SYSTRACE("fstat(%d, %08x)", a0, a1);
        struct stat sb;
        int r = fstat(a0, &sb);
        if (r < 0) {
                SC_RET_ERROR(host_to_rix_errno(errno));
        } else if (sb.st_size > INT32_MAX) {
                // The guest's signed 32-bit off_t cannot represent this size.
                SC_RET_ERROR(EFBIG);
        } else {
                host_to_rix_stat((struct rix_stat *)(mem_base + a1), &sb);
                SC_RET_VAL("%d", r);
        }
}

void    rix_sc_getpagesize(ARMul_State *state)
{
        SYSTRACE("getpagesize()");
        SC_RET_VAL("%d", 32768);
}

void    rix_sc_fork(ARMul_State *state)
{
        int id;
        SYSTRACE("fork/vfork()");
        for (id = 1; id <= INT16_MAX; id++)
                if (id != guest_pid && id != guest_ppid && !child_pids[id])
                        break;
        if (id > INT16_MAX) {
                SC_RET_ERROR(host_to_rix_errno(EAGAIN));
                return;
        }
        fflush(NULL);
        pid_t r = fork();
        if (r < 0) {
                SC_RET_ERROR(host_to_rix_errno(errno));
                return;
        }
        if (r == 0) {
                guest_ppid = guest_pid;
                guest_pid = id;
                memset(child_pids, 0, sizeof(child_pids));
        } else {
                child_pids[id] = r;
        }
        /* fork's libc wrapper tests r1 bit zero; vfork returns r0 directly.
         * Both syscalls use host fork: guest memory is private in the child.
         */
        ARMul_SetReg(state, state->Mode, 1, r == 0);
        SC_RET_VAL("%d", r == 0 ? 0 : id);
}

void    rix_sc_getdtablesize(ARMul_State *state)
{
        // Dunno what this is on RISCiX.
        SYSTRACE("getdtablesize()");
        SC_RET_VAL("%d", 512);
}

void    rix_sc_gettimeofday(ARMul_State *state)
{
        SC_2ARG;
        SYSTRACE("gettimeofday(%08x, %08x)", a0, a1);

        // TZ is ignored!
        struct timeval tv;
        gettimeofday(&tv, NULL);
        // Do this by proxy to protect against 64b time_t.
        write32(a0, tv.tv_sec);
        write32(a0 + 4, tv.tv_usec);
        SC_RET_VAL("%d", 0);
}

void    rix_sc_getrusage(ARMul_State *state)
{
        SC_2ARG;
        SYSTRACE("getrusage(%d, %08x)", a0, a1);

        // Basically a NOP, but at least write to the output structure...
        bzero(mem_base + a1, 8 + // timeval
              8 + // timeval
              14*4);

        SC_RET_VAL("%d", 0);
}

void    rix_sc_ftruncate(ARMul_State *state)
{
        SC_2ARG;
        SYSTRACE("ftruncate(%d, %08x)", a0, a1);

        int r = ftruncate(a0, (rix_off_t)a1);

        if (r < 0)
                SC_RET_ERROR(host_to_rix_errno(errno));
        else
                SC_RET_VAL("%d", r);
}

void    rix_sc_NOP(ARMul_State *state, char *name)
{
        SC_4ARG;
        SYSTRACE("%s(%08x, %08x, %08x, %08x)",
                 name, a0, a1, a2, a3);
        SC_RET_VAL("%d", 0);
}


////////////////////////////////////////////////////////////////////////////////

// From GDB's sim/arm/armos.h:
#define FPESTART        0x2000L
#define FPEEND          0x8000L
#define FPEOLDVECT      FPESTART + 0x100L + 8L * 16L + 4L    /* stack + 8 regs + fpsr */
#define FPENEWVECT(addr) 0xea000000L + ((addr) >> 2) - 3L       /* branch from 4 to 0x2400 */

#include "armfpe.h"

void    os_init(ARMul_State *state, int verbose)
{
        sc_trace = verbose;

        // Install FPE (based on GDB's armulator's armos.c)
        int i;
        for (i = 0; i < fpesize; i += 4)      /* copy the code */
                write32(FPESTART + i, fpecode[i >> 2]);
        // Find entry ptr:
        for (i = FPESTART + fpesize;; i -= 4) {
                unsigned int j;
                if ((j = read32(i)) == 0xffffffff)
                        break;
        }
        /* install new vector */
        write32(4, FPENEWVECT(read32(i - 4)));

        // Set up SVC stack below FPE:
        ARMul_SetReg(state, SVC26MODE, 13, FPESTART - 4);

        // Change to user mode for the running program:
        SETABORT(0, USER26MODE);
        ARMul_CPSRAltered(state);
        initial_state = *state;
}

unsigned int    ARMul_OSHandleSWI(ARMul_State *state, ARMword number)
{
        /* printf("Got SWI 0x%x at PC %08x\n", number, ARMul_GetPC(state)); */
        unsigned int scnum = number & 0xfffff;

        switch(scnum) {
        case 1:         /* exit         */      rix_sc_exit(state);             break;
        case 2:         /* fork         */      rix_sc_fork(state);             break;
        case 3:         /* read         */      rix_sc_read(state);             break;
        case 4:         /* write        */      rix_sc_write(state);            break;
        case 6:         /* close        */      rix_sc_close(state);            break;
        case 7:         /* wait4        */      rix_sc_wait4(state);            break;
        case 8:         /* creat        */      rix_sc_creat(state);            break;
        case 9:         /* link         */      rix_sc_link(state);             break;
        case 10:        /* unlink       */      rix_sc_unlink(state);           break;
        case 11:        /* waitpid      */      rix_sc_waitpid(state);          break;
        case 15:        /* chmod        */      rix_sc_chmod(state);            break;
        case 16:        /* chown        */      rix_sc_NOP(state, "chown");     break;
        case 17:        /* sbreak       */      rix_sc_sbreak(state);           break;
        case 19:        /* lseek        */      rix_sc_lseek(state);            break;
        case 20:        /* getpid       */      rix_sc_getpid(state);           break;
        case 24:        /* getuid       */      rix_sc_getuid(state);           break;
        case 92: { /* fcntl: descriptor commands only */
            SC_3ARG;
            int commands[] = {F_DUPFD, F_GETFD, F_SETFD};
            if (a1 == 3) {
                int flags = fcntl((int)a0, F_GETFL);
                if (flags < 0) { SC_RET_ERROR(host_to_rix_errno(errno)); break; }
                int result = flags & O_ACCMODE;
                if (flags & O_APPEND) result |= 1 << 3;
                if (flags & O_NONBLOCK) result |= (1 << 2) | (1 << 14);
                if (flags & O_ASYNC) result |= 1 << 6;
                if (flags & O_SYNC) result |= 1 << 13;
                SC_RET_VAL("%d", result);
                break;
            }
            if (a1 > 2) { SC_RET_ERROR(22); break; }
            int result = fcntl((int)a0, commands[a1], (int)a2);
            if (result < 0) SC_RET_ERROR(host_to_rix_errno(errno));
            else SC_RET_VAL("%d", result);
            break;
        }
        case 28:        /* open         */      rix_sc_open(state);             break;
        case 34:        /* access       */      rix_sc_access(state);           break;
        case 39:        /* stat         */      rix_sc_stat(state);             break;
        case 47:        /* getgid       */      rix_sc_getgid(state);           break;
        case 54:        /* ioctl        */      rix_sc_ioctl(state);            break;
        case 59:        /* execve       */      rix_sc_execve(state);           break;
        case 60:        /* umask        */      rix_sc_NOP(state, "umask");     break;
        case 62:        /* fstat        */      rix_sc_fstat(state);            break;
        case 64:        /* getpagesize  */      rix_sc_getpagesize(state);      break;
        case 66:        /* vfork        */      rix_sc_fork(state);             break;
        case 77:        /* getgroups    */      rix_sc_getgroups(state);        break;
        case 84:        /* wait         */      rix_sc_wait(state);             break;
        case 89:        /* getdtablesize*/      rix_sc_getdtablesize(state);    break;
        case 108:       /* sigvec       */      rix_sc_NOP(state, "sigvec");    break;
        case 109: {
            SC_1ARG;
            uint32_t old = guest_sigmask;
            guest_sigmask |= a0;
            SC_RET_VAL("%u", old);
            break;
        }
        case 110: {
            SC_1ARG;
            uint32_t old = guest_sigmask;
            guest_sigmask = a0;
            SC_RET_VAL("%u", old);
            break;
        }
        case 112: {
            SC_2ARG;
            if ((a0 && !guest_range(a0, 8)) || (a1 && !guest_range(a1, 8))) {
                SC_RET_ERROR(14);
                break;
            }
            uint32_t next_sp = a0 ? read32(a0) : 0;
            uint32_t next_onstack = a0 ? read32(a0 + 4) : 0;
            if (a1) {
                write32(a1, guest_sigstack[0]);
                write32(a1 + 4, guest_sigstack[1]);
            }
            if (a0) {
                guest_sigstack[0] = next_sp;
                guest_sigstack[1] = next_onstack;
            }
            SC_RET_VAL("%d", 0);
            break;
        }
        case 116:       /* gettimeofday */      rix_sc_gettimeofday(state);     break;
        case 117:       /* getrusage    */      rix_sc_getrusage(state);        break;
        case 130:       /* ftruncate    */      rix_sc_ftruncate(state);        break;

        default:
                panic("*** Unhandled syscall %u at PC %08" PRIx32 "\n",
                      scnum, ARMul_GetPC(state));
        }
        return 1;
}

void dump_state(ARMul_State *state)
{
        for (int i = 0; i < 15; i++) {
                printf("R%02d %08x      ", i, ARMul_GetReg(state, state->Mode, i));
                if ((i & 3) == 3)
                        printf("\n");
        }
        printf("R15 %08x, mode %d\n", ARMul_GetPC(state), state->Mode);
}

unsigned int    ARMul_OSException(ARMul_State * state, ARMword vector,
                                  ARMword pc)
{
        if (vector == 0x4) {    /* Undefined, assume FPE! */
                if (state->verbose > 1) {
                        printf("*** UNK exception @PC%08x, calling vector\n", pc);
                        dump_state(state);
                }
                return 0;       // Go to exception vector
        } else {
                panic("Got exception (vector 0x%" PRIx32 "), PC %08" PRIx32 "\n",
                      vector, pc);
        }

        return 1; // Don't do exception vectors, etc.
}
