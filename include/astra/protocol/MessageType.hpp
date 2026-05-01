#include <cstdint>

enum class MessageType : uint8_t {
  AddOrder = 1,
  ModifyOrder = 2,
  DeleteOrder = 3,
  Trade = 4,
  Quote = 5,
  Heartbeat = 6,
  SnapshotBegin = 7,
  SnapshotEnd = 8,
};