#include "replay/GzipLineReader.hpp"

#include <utility>

GzipLineReader::GzipLineReader(const std::string &path) { open(path); }

GzipLineReader::~GzipLineReader() { close(); }

GzipLineReader::GzipLineReader(GzipLineReader &&other) noexcept
    : file_(std::exchange(other.file_, nullptr)),
      line_number_(std::exchange(other.line_number_, 0)),
      last_error_(std::move(other.last_error_)) {}

GzipLineReader &GzipLineReader::operator=(GzipLineReader &&other) noexcept {
  if (this != &other) {
    close();
    file_ = std::exchange(other.file_, nullptr);
    line_number_ = std::exchange(other.line_number_, 0);
    last_error_ = std::move(other.last_error_);
  }
  return *this;
}

bool GzipLineReader::open(const std::string &path) {
  close();
  file_ = gzopen(path.c_str(), "rb");
  line_number_ = 0;
  if (file_ == nullptr) {
    last_error_ = "failed to open replay file: " + path;
    return false;
  }
  last_error_.clear();
  return true;
}

void GzipLineReader::close() {
  if (file_ != nullptr) {
    gzclose(file_);
    file_ = nullptr;
  }
}

bool GzipLineReader::getline(std::string &line) {
  line.clear();
  if (file_ == nullptr) {
    last_error_ = "replay file is not open";
    return false;
  }

  while (true) {
    char *chunk = gzgets(file_, buffer_.data(),
                         static_cast<int>(buffer_.size()));
    if (chunk == nullptr) {
      if (!line.empty()) {
        ++line_number_;
        return true;
      }
      return false;
    }

    line.append(chunk);
    if (!line.empty() && line.back() == '\n') {
      line.pop_back();
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      ++line_number_;
      return true;
    }
  }
}

bool GzipLineReader::isOpen() const { return file_ != nullptr; }

uint64_t GzipLineReader::lineNumber() const { return line_number_; }

const std::string &GzipLineReader::lastError() const { return last_error_; }
