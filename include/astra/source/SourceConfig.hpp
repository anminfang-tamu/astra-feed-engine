#pragma once

#include <cstdint>
#include <string>

struct SourceConfig {
  enum class Type { Udp, Dpdk, Replay };

  Type type{Type::Udp};
  std::string ip;
  uint16_t port{0};
  std::string replay_file;
};