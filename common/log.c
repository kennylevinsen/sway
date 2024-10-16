#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <syslog.h>
#include "log.h"

static terminate_callback_t log_terminate = exit;

void _sway_abort(const char *format, ...) {
	va_list args;
	va_start(args, format);
	_sway_vlog(SWAY_ERROR, format, args);
	va_end(args);
	log_terminate(EXIT_FAILURE);
}

bool _sway_assert(bool condition, const char *format, ...) {
	if (condition) {
		return true;
	}

	va_list args;
	va_start(args, format);
	_sway_vlog(SWAY_ERROR, format, args);
	va_end(args);

#ifndef NDEBUG
	raise(SIGABRT);
#endif

	return false;
}

static sway_log_target_t log_target = SWAY_LOG_TARGET_STANDARD;
static sway_log_importance_t log_importance = SWAY_ERROR;
static struct timespec start_time = {-1, -1};

static const char *verbosity_colors[] = {
	[SWAY_SILENT] = "",
	[SWAY_ERROR ] = "\x1B[1;31m",
	[SWAY_INFO  ] = "\x1B[1;34m",
	[SWAY_DEBUG ] = "\x1B[1;90m",
};

static const char *verbosity_headers[] = {
	[SWAY_SILENT] = "",
	[SWAY_ERROR] = "[ERROR]",
	[SWAY_INFO] = "[INFO]",
	[SWAY_DEBUG] = "[DEBUG]",
};

static const int verbosity_syslog[] = {
	[SWAY_SILENT] = LOG_DEBUG,
	[SWAY_ERROR] = LOG_ERR,
	[SWAY_INFO] = LOG_INFO,
	[SWAY_DEBUG] = LOG_DEBUG,
};

static void timespec_sub(struct timespec *r, const struct timespec *a,
		const struct timespec *b) {
	const long NSEC_PER_SEC = 1000000000;
	r->tv_sec = a->tv_sec - b->tv_sec;
	r->tv_nsec = a->tv_nsec - b->tv_nsec;
	if (r->tv_nsec < 0) {
		r->tv_sec--;
		r->tv_nsec += NSEC_PER_SEC;
	}
}

static void init_start_time(void) {
	if (start_time.tv_sec >= 0) {
		return;
	}
	clock_gettime(CLOCK_MONOTONIC, &start_time);
}

static void sway_log_stderr(sway_log_importance_t verbosity, const char *fmt,
		va_list args) {
	struct timespec ts = {0};
	clock_gettime(CLOCK_MONOTONIC, &ts);
	timespec_sub(&ts, &ts, &start_time);

	fprintf(stderr, "%02d:%02d:%02d.%03ld ", (int)(ts.tv_sec / 60 / 60),
		(int)(ts.tv_sec / 60 % 60), (int)(ts.tv_sec % 60),
		ts.tv_nsec / 1000000);

	unsigned c = (verbosity < SWAY_LOG_IMPORTANCE_LAST) ? verbosity :
		SWAY_LOG_IMPORTANCE_LAST - 1;

	if (isatty(STDERR_FILENO)) {
		fprintf(stderr, "%s", verbosity_colors[c]);
	} else {
		fprintf(stderr, "%s ", verbosity_headers[c]);
	}

	vfprintf(stderr, fmt, args);

	if (isatty(STDERR_FILENO)) {
		fprintf(stderr, "\x1B[0m");
	}
	fprintf(stderr, "\n");
}

// Some tools like `logger(1)` support parsing syslog priority from a prefix
// that contains the numeric priority value, e.g. `<6>` for LOG_INFO.
static void sway_log_prio_prefix(sway_log_importance_t verbosity, const char *fmt,
		va_list args) {

	// We could do an independent print for each of the prefix, message and
	// trailing newline, but it's much nicer to just do a single write. musl's
	// syslog implementation uses a 1024-byte stack buffer, so that's probably
	// good enough.
	char buf[1024];
	size_t n1 = snprintf(buf, sizeof(buf) - 1, "<%d>", verbosity_syslog[verbosity]);
	size_t n2 = vsnprintf(buf + n1, sizeof(buf) - 1 - n1, fmt, args);


	if (n1 + n2 < sizeof(buf) - 1) {
		buf[n1 + n2] = '\n';
		fwrite(buf, n1 + n2 + 1, 1, stderr);
	} else {
		// The log message got cut off, insert ellipsis at the end so that the
		// user can see that an overflow had occurred.
		snprintf(buf + sizeof(buf) - 5, 5, "...\n");
		fwrite(buf, sizeof(buf), 1, stderr);
	}
}

static void sway_log_syslog(sway_log_importance_t verbosity, const char *fmt,
		va_list args) {
	vsyslog(verbosity_syslog[verbosity], fmt, args);
}

void sway_log_init(sway_log_importance_t verbosity, terminate_callback_t callback, sway_log_target_t target) {
	init_start_time();

	if (verbosity < SWAY_LOG_IMPORTANCE_LAST) {
		log_importance = verbosity;
	}
	if (callback) {
		log_terminate = callback;
	}
	log_target = target;
}

void _sway_vlog(sway_log_importance_t verbosity, const char *fmt, va_list args) {
	if (verbosity > log_importance) {
		return;
	}
	switch (log_target) {
	case SWAY_LOG_TARGET_PRIO_PREFIX:
		sway_log_prio_prefix(verbosity, fmt, args);
		break;
	case SWAY_LOG_TARGET_SYSLOG:
		sway_log_syslog(verbosity, fmt, args);
		break;
	default:
		sway_log_stderr(verbosity, fmt, args);
		break;
	}
}

void _sway_log(sway_log_importance_t verbosity, const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
	_sway_vlog(verbosity, fmt, args);
	va_end(args);
}
