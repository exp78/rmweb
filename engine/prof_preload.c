/* LD_PRELOAD profiler: on SIGUSR2, capture the receiving thread's userspace backtrace and hand it to a
 * helper thread that symbolizes it to stderr. The signal handler itself is async-signal-safe (only
 * backtrace() [primed at startup] + sem_post); backtrace_symbols_fd (malloc/dladdr) runs OUTSIDE signal
 * context in the helper thread -> no crash. Used to find what the WPE compositor thread spins on. */
#define _GNU_SOURCE
#include <signal.h>
#include <execinfo.h>
#include <unistd.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/prctl.h>

static sem_t g_sem;
static void *g_bt[64];
static volatile int g_n;
static char g_name[20];

static void handler(int sig)
{
    (void)sig;
    prctl(PR_GET_NAME, g_name);           /* async-signal-safe (bare syscall) */
    g_n = backtrace(g_bt, 64);            /* primed in init() so it won't allocate here */
    sem_post(&g_sem);                     /* async-signal-safe */
}

static void *printer(void *arg)
{
    (void)arg;
    for (;;) {
        sem_wait(&g_sem);
        write(2, "\n[PROF] thread=", 15);
        int l = 0; while (g_name[l]) l++;
        write(2, g_name, (size_t)l);
        write(2, "\n", 1);
        backtrace_symbols_fd(g_bt, g_n, 2);   /* safe: ordinary thread context */
    }
    return 0;
}

__attribute__((constructor))
static void init(void)
{
    void *tmp[4];
    backtrace(tmp, 4);                     /* prime the libgcc unwinder before any signal */
    sem_init(&g_sem, 0, 0);
    pthread_t t;
    pthread_create(&t, 0, printer, 0);
    struct sigaction sa;
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGUSR2, &sa, 0);
}
