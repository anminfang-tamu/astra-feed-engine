#pragma once

#include "astra/publish/IPublisher.hpp"

class NullPublisher : public IPublisher {
public:
  void publish() override {

  };
};