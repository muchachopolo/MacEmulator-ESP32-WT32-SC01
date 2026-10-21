/* newlib system-call stubs. Circle provides malloc/free and the memory/string
 * functions; newlib's stdio (vsnprintf etc.) only needs a small private heap
 * (_sbrk) and output for stray fprintf(stderr) diagnostics (_write -> log). */
#include <errno.h>
#include <stddef.h>
#include <sys/stat.h>
#include "pi_platform.h"

static char newlib_heap[1 << 20];
static size_t newlib_heap_used;

void *_sbrk(ptrdiff_t incr)
{
    if (incr < 0 || newlib_heap_used + (size_t)incr > sizeof newlib_heap) {
        errno = ENOMEM;
        return (void *)-1;
    }
    void *p = &newlib_heap[newlib_heap_used];
    newlib_heap_used += (size_t)incr;
    return p;
}

int _write(int fd, const char *buf, int len)
{
    static char line[256];
    static int n;
    (void)fd;
    for (int i = 0; i < len; i++) {
        char c = buf[i];
        if (c == '\n' || n == (int)sizeof line - 1) {
            line[n] = 0;
            pi_log(3, "stdio", "%s", line);
            n = 0;
            if (c == '\n') continue;
        }
        line[n++] = c;
    }
    return len;
}

int _read(int fd, char *buf, int len) { (void)fd; (void)buf; (void)len; return 0; }
int _close(int fd) { (void)fd; return -1; }
int _open(const char *path, int flags, int mode) { (void)path; (void)flags; (void)mode; errno = ENOENT; return -1; }
int _lseek(int fd, int off, int whence) { (void)fd; (void)off; (void)whence; return 0; }
int _fstat(int fd, struct stat *st) { (void)fd; st->st_mode = S_IFCHR; return 0; }
int _isatty(int fd) { (void)fd; return 1; }
int _getpid(void) { return 1; }
int _kill(int pid, int sig) { (void)pid; (void)sig; errno = EINVAL; return -1; }
void _exit(int status) { (void)status; pi_restart(); for (;;) { } }
void _init(void) { }   /* newlib init/fini arrays: Circle runs constructors itself */
void _fini(void) { }

/* Musashi's only sscanf is m68kfpu.c's packed-decimal load: sscanf(str, "%le",
 * &d). newlib's sscanf would drag in strtoul/strtoull, which clash with
 * Circle's, so support just that format. */
#include <stdarg.h>
#include <stdlib.h>
int sscanf(const char *str, const char *fmt, ...)
{
    if (fmt[0] != '%' || fmt[1] != 'l' || fmt[2] != 'e' || fmt[3] != 0)
        return 0;
    va_list ap;
    va_start(ap, fmt);
    *va_arg(ap, double *) = strtod(str, NULL);
    va_end(ap);
    return 1;
}
