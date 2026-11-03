/* Optional stress test, compiled and linked with the original RISCiX tools.
 * Usage: norcroft-fork object crt0.o -L/path/to/usr/lib
 * Each child links the supplied object into ./linked.
 */
#define _BSD_C 1
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>

extern int vfork(void);

int main(int argc, char **argv)
{
        int i;
        union wait status;
        volatile int sentinel = 42;
        char *args[8];
        char *env[] = { "PATH=/usr/bin:/usr/sbin", 0 };

        if (argc != 4) return 2;
        args[0] = "ld";
        args[1] = argv[1];
        args[2] = argv[2];
        args[3] = argv[3];
        args[4] = "-lc";
        args[5] = "-o";
        args[6] = "linked";
        args[7] = 0;

        for (i = 0; i < 32; i++) {
                pid_t child = (i & 1) ? vfork() : fork();
                if (child < 0) {
                        perror("fork");
                        return 1;
                }
                if (child == 0) {
                        sentinel = 0;
                        execve("/does-not-exist", args, env);
                        if (errno != ENOENT) _exit(101);
                        execve("/usr/bin/ld", args, env);
                        perror("execve ld");
                        _exit(102);
                }
                if (waitpid(child, &status, 0) != child || status.w_status || sentinel != 42) {
                        fprintf(stderr, "child %d failed\n", i);
                        return 1;
                }
                if (waitpid(child, &status, 0) != -1 || errno != ECHILD) return 1;
        }
        printf("32 linker children passed\n");
        return 0;
}
