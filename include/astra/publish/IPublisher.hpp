#pragma once

class IPublisher {
public:
  virtual ~IPublisher() = default;
  virtual void publish() = 0;
};