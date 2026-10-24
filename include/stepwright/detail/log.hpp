#pragma once

// Optional logging facade: each STEPWRIGHT_LOG_<level> forwards to spdlog with that
// level's compile-time filter when STEPWRIGHT_ENABLE_LOGGING is defined. Otherwise
// the macros do not evaluate their arguments. Enabled calls inherit spdlog's
// thread-safety; no argument references are retained by this facade.
#ifdef STEPWRIGHT_ENABLE_LOGGING
#include <spdlog/spdlog.h>
#define STEPWRIGHT_LOG_TRACE(...) SPDLOG_TRACE(__VA_ARGS__)
#define STEPWRIGHT_LOG_DEBUG(...) SPDLOG_DEBUG(__VA_ARGS__)
#define STEPWRIGHT_LOG_INFO(...) SPDLOG_INFO(__VA_ARGS__)
#define STEPWRIGHT_LOG_WARN(...) SPDLOG_WARN(__VA_ARGS__)
#define STEPWRIGHT_LOG_ERROR(...) SPDLOG_ERROR(__VA_ARGS__)
#else
#define STEPWRIGHT_LOG_TRACE(...) static_cast<void>(0)
#define STEPWRIGHT_LOG_DEBUG(...) static_cast<void>(0)
#define STEPWRIGHT_LOG_INFO(...) static_cast<void>(0)
#define STEPWRIGHT_LOG_WARN(...) static_cast<void>(0)
#define STEPWRIGHT_LOG_ERROR(...) static_cast<void>(0)
#endif
