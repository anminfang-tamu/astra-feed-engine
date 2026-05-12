#pragma once

enum class MdEventType {
  Add,
  Modify,
  Replace,
  Delete,
  Execution,
  Trade,
  Imbalance,
  Status,
  Summary,
  Unknown
};