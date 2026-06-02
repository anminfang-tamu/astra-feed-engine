#pragma once

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>

namespace astra::trace {

inline bool enabled() noexcept {
  static const bool enabled = [] {
    const char *value = std::getenv("ASTRA_TRACE");
    return value != nullptr && value[0] != '\0' && value[0] != '0';
  }();
  return enabled;
}

inline int outputFd() noexcept {
  static const int fd = [] {
    const char *path = std::getenv("ASTRA_TRACE_FILE");
    if (path == nullptr || path[0] == '\0') return STDERR_FILENO;
    const int opened = ::open(path, O_CREAT | O_WRONLY | O_APPEND, 0644);
    return opened >= 0 ? opened : STDERR_FILENO;
  }();
  return fd;
}

inline void writeAll(int fd, const char *data, int len) noexcept {
  int written = 0;
  while (written < len) {
    const ssize_t n = ::write(fd, data + written,
                              static_cast<size_t>(len - written));
    if (n <= 0) return;
    written += static_cast<int>(n);
  }
}

inline void log(const char *file, int line, const char *fmt, ...) noexcept {
  if (!enabled()) return;

  char buf[1024];
  int n = std::snprintf(buf, sizeof(buf), "[astra-trace] %s:%d ", file, line);
  if (n < 0) return;
  if (n >= static_cast<int>(sizeof(buf))) n = static_cast<int>(sizeof(buf)) - 1;

  va_list args;
  va_start(args, fmt);
  int m = std::vsnprintf(buf + n, sizeof(buf) - static_cast<size_t>(n), fmt,
                         args);
  va_end(args);
  if (m < 0) return;

  int total = n + m;
  if (total >= static_cast<int>(sizeof(buf)))
    total = static_cast<int>(sizeof(buf)) - 1;
  buf[total++] = '\n';
  writeAll(outputFd(), buf, total);
}

} // namespace astra::trace

#ifdef ASTRA_ENABLE_TRACE
#define ASTRA_TRACE(...) ::astra::trace::log(__FILE__, __LINE__, __VA_ARGS__)
#else
#define ASTRA_TRACE(...) do { } while (false)
#endif
