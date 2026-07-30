#pragma once

enum class ChannelPhase {
  WaitingStartOfMessages,
  StartupDirectorySpin,
  StartupAdminSpin,
  SystemHours,
  MarketHours,
  PostMarketHours,
  PostSystemHours,
  EndOfMessages
};
