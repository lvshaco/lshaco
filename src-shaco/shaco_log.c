#include "shaco_log.h"
#include "shaco.h"
#include "shaco_timer.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <execinfo.h>
#include <stdarg.h>
#include <time.h>

static FILE *F;
static int LEVEL = LOG_INFO;

static const char* STR_LEVELS[LOG_MAX] = {
    "DEBUG", "TRACE", "INFO", "WARNING", "ERROR", "EXIT", "PANIC",
};

static inline const char*
_levelstr(int level) {
    if (level >= 0 && level < LOG_MAX)
        return STR_LEVELS[level];
    return "";
}

static int 
_levelid(const char* level) {
    int i;
    for (i=LOG_DEBUG; i<LOG_MAX; ++i) {
        if (strcasecmp(STR_LEVELS[i], level) == 0)
            return i;
    }
    return -1;
}

const char *
shaco_log_level() {
    return _levelstr(LEVEL);
}

int
shaco_log_setlevel(const char* level) {
    int id = _levelid(level);
    if (id == -1)
        return -1;
    else {
        LEVEL = id;
        return id;
    }
}

static inline void
_prefix(int level) {
    char tmp[64];
    uint64_t now = shaco_timer_now();
    time_t sec = now / 1000;
    uint32_t msec = now % 1000;
    strftime(tmp, sizeof(tmp), "%y%m%d-%H:%M:%S.", localtime(&sec));
    fprintf(F, "%d %s%03d %s: ", (int)getpid(), tmp, msec, _levelstr(level));
}

static inline void
_log(int level, const char *log) {
    _prefix(level);
    fprintf(F, "%s\n", log);
    fflush(F);
}

static inline void
_logv(int level, const char *fmt, va_list ap) {
    _prefix(level);
    vfprintf(F, fmt, ap);
    fprintf(F, "%s", "\n");
    fflush(F);
}

void
shaco_log(int level, const char *fmt, ...) {
    if (level < LEVEL)
        return;
    va_list ap;
    va_start(ap, fmt);
    _logv(level, fmt, ap);
    va_end(ap);
}

void
shaco_backtrace() {
    void* addrs[24];
    int i, n;
    char** symbols;
    n = backtrace(addrs, sizeof(addrs)/sizeof(addrs[0]));
    symbols = backtrace_symbols(addrs, n);
    if (symbols) {
        for (i=0; i<n; ++i) {
            _log(LOG_PANIC, symbols[i]);
        }
        free(symbols);
    }
}

void
shaco_exit(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    _logv(LOG_PANIC, fmt, ap);
    va_end(ap);
    exit(1);
}

void 
shaco_panic(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    _logv(LOG_PANIC, fmt, ap);
    va_end(ap);

    _log(LOG_PANIC, "Panic detected at:");
    shaco_backtrace();
    abort();
}

void
shaco_log_open(const char *filename) {
    if (filename && filename[0]) {
        F = fopen(filename, "a+");
        if (F==NULL) {
            fprintf(stderr, "log open `%s` fail: %s", filename, strerror(errno));
            exit(1);
        }
    } else {
        F = stdout;
    }
}

void
shaco_log_close() {
    if (F && F != stdout) {
        fclose(F);
        F=NULL;
    }
}