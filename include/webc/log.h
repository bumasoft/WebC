// SPDX-License-Identifier: Apache-2.0
// WebC — structured logging and metrics (SPEC §6.6).
#ifndef WEBC_LOG_H
#define WEBC_LOG_H

#include <webc/core.h>

typedef enum LogLevel : u8 { LOG_DEBUG, LOG_INFO, LOG_WARN, LOG_ERROR, LOG_OFF } LogLevel;

typedef struct LogField {
  const char *key;
  str value;
} LogField;

// Installed by wtask_init()/webc_serve(): a fault prints a backtrace and every
// live task before the process dies. Idempotent.
void     wc_crash_init(void);
void     wc_crash_thread_init(void); // per worker thread: its own sigaltstack

void     log_set_level(LogLevel lvl);
LogLevel log_level(void);
void     log_set_json(bool json);
bool     log_is_json(void);

void log_write(LogLevel lvl, const char *msg, const LogField *fields, i32 n);

// log_info("listening", { "addr", str_lit(":8080") }, { "cpus", ... });
#define WC_LOG_F_(...) (LogField[]) { __VA_OPT__(__VA_ARGS__) }
#define WC_LOG_N_(...) ((i32)(sizeof(WC_LOG_F_(__VA_ARGS__)) / sizeof(LogField)))
#define log_debug(msg_, ...) log_write(LOG_DEBUG, (msg_), WC_LOG_F_(__VA_ARGS__), WC_LOG_N_(__VA_ARGS__))
#define log_info(msg_, ...)  log_write(LOG_INFO,  (msg_), WC_LOG_F_(__VA_ARGS__), WC_LOG_N_(__VA_ARGS__))
#define log_warn(msg_, ...)  log_write(LOG_WARN,  (msg_), WC_LOG_F_(__VA_ARGS__), WC_LOG_N_(__VA_ARGS__))
#define log_error(msg_, ...) log_write(LOG_ERROR, (msg_), WC_LOG_F_(__VA_ARGS__), WC_LOG_N_(__VA_ARGS__))

// ---------------------------------------------------------------- metrics

typedef struct MetricLabel { str name, value; } MetricLabel;

void metrics_counter_add(const char *name, i64 delta, const char *help);
void metrics_counter_inc(const char *name);
void metrics_gauge_set(const char *name, f64 v, const char *help);
void metrics_observe(const char *name, f64 v, const char *help);
void metrics_observe_http(str method, i32 status, u64 duration_ms);
void metrics_render(Arena *a, Buf *out);     // Prometheus text exposition
void metrics_reset(void);
u64  metrics_request_count(void);

// ---------------------------------------------------------------- profiling
//
// A CPU sampler for `/debug/cpu`: while a window is open the kernel interrupts
// the process on a CPU-time timer and records where it was. Samples are grouped
// by symbol when the window closes. `webc_profile_report()` returns a flat
// profile (arena-allocated, so pass the request arena).
bool webc_profile_start(i64 duration_ms, i32 *err);
void webc_profile_stop(void);
bool webc_profile_active(void);
i32  webc_profile_samples(void);
i32  webc_profile_rounds(void); // sampler loop iterations: a health check for the sampler
str  webc_profile_report(Arena *a);

#endif // WEBC_LOG_H
