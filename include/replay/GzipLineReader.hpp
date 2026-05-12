#pragma once

#include <array>
#include <cstdint>
#include <string>

#include <zlib.h>

class GzipLineReader {
public:
  GzipLineReader() = default;
  explicit GzipLineReader(const std::string &path);
  ~GzipLineReader();

  GzipLineReader(const GzipLineReader &) = delete;
  GzipLineReader &operator=(const GzipLineReader &) = delete;

  GzipLineReader(GzipLineReader &&other) noexcept;
  GzipLineReader &operator=(GzipLineReader &&other) noexcept;

  bool open(const std::string &path);
  void close();
  bool getline(std::string &line);

  bool isOpen() const;
  uint64_t lineNumber() const;
  const std::string &lastError() const;

private:
  gzFile file_{nullptr};
  uint64_t line_number_{0};
  std::string last_error_;
  std::array<char, 64 * 1024> buffer_{};
};
