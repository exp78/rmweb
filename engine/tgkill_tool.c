/* tgkill_tool <pid> <tid> <signum> -- send a signal to a SPECIFIC thread (BusyBox kill can't).
 * Used to deliver SIGUSR2 to the WPE compositor thread so prof_preload.so dumps ITS backtrace. */
#define _GNU_SOURCE
#include <sys/syscall.h>
#include <unistd.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    if (argc < 4) return 2;
    return syscall(SYS_tgkill, atoi(argv[1]), atoi(argv[2]), atoi(argv[3])) == 0 ? 0 : 1;
}
