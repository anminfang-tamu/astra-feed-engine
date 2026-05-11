#include "astra/source/IReplaySource.hpp"

class VendorReplaySource final : public IReplaySource {
public:
  bool requestGapFill(const GapEvent &gap) override {
    // Implement logic to request gap fill from the vendor
    return true; // Return true if the request was successful
  }

  bool next(PacketView &packet) override {
    // Implement logic to retrieve the next packet from the vendor
    return true; // Return true if a packet was successfully retrieved
  }
};