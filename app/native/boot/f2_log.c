/* F2 logging: logcat (tag "F2") + a log file in the app's external files dir.
 * Static PC test builds (-DF2_NO_LOGCAT) write to stderr instead of logcat. */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <pthread.h>
#include <sys/syscall.h>
#include "../f2_app.h"
#ifndef F2_NO_LOGCAT
#include <android/log.h>
#endif

static int s_fd = -1;
static int s_tried;
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;

int f2_log_fd(void) { return s_fd; }

/* (re)open the log file; called by the boot layer as soon as the files dir is known */
void f2_log_open(const char *path)
{
    pthread_mutex_lock(&s_lock);
    if (s_fd >= 0) close(s_fd);
    s_fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    s_tried = 1;
    pthread_mutex_unlock(&s_lock);
}

static void emit(const char *line, int n)
{
#ifndef F2_NO_LOGCAT
    __android_log_write(ANDROID_LOG_INFO, "F2", line);
#else
    (void)!write(2, line, n);
    (void)!write(2, "\n", 1);
#endif
    if (s_fd >= 0) {
        char pfx[48];
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        int m = snprintf(pfx, sizeof pfx, "%6ld.%03ld %5d ", (long)ts.tv_sec, ts.tv_nsec / 1000000, (int)syscall(SYS_gettid));
        struct iovec { const void *b; size_t l; };
        pthread_mutex_lock(&s_lock);
        (void)!write(s_fd, pfx, m);
        (void)!write(s_fd, line, n);
        (void)!write(s_fd, "\n", 1);
        pthread_mutex_unlock(&s_lock);
    }
}

void f2_logv(const char *fmt, va_list ap)
{
    char buf[1024];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    if (n < 0) return;
    if (n > (int)sizeof(buf) - 1) n = sizeof(buf) - 1;
    while (n > 0 && buf[n - 1] == '\n') buf[--n] = 0;
    emit(buf, n);
}

void f2_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    f2_logv(fmt, ap);
    va_end(ap);
}

void f2_milestone(int id, const char *fmt, ...)
{
    char buf[900];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    f2_log("F2: [M%02d] %s", id, buf);
}

/* async-signal-safe raw line (crash handler) */
void f2_log_raw(const char *line)
{
    size_t n = strlen(line);
#ifndef F2_NO_LOGCAT
    __android_log_write(ANDROID_LOG_FATAL, "F2", line);
#else
    (void)!write(2, line, n);
    (void)!write(2, "\n", 1);
#endif
    if (s_fd >= 0) { (void)!write(s_fd, line, n); (void)!write(s_fd, "\n", 1); }
}

/* file only (the engine's own logcat lines, copied by the import hook) */
void f2_log_file_only(const char *prefix, const char *text)
{
    if (s_fd < 0) return;
    char pfx[48];
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int m = snprintf(pfx, sizeof pfx, "%6ld.%03ld %5d ", (long)ts.tv_sec, ts.tv_nsec / 1000000, (int)syscall(SYS_gettid));
    size_t n = strlen(text);
    while (n > 0 && text[n - 1] == '\n') n--;
    pthread_mutex_lock(&s_lock);
    (void)!write(s_fd, pfx, m);
    (void)!write(s_fd, prefix, strlen(prefix));
    (void)!write(s_fd, text, n);
    (void)!write(s_fd, "\n", 1);
    pthread_mutex_unlock(&s_lock);
}
