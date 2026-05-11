#include "astra/source/ISnapshotSource.hpp"

class VendorSnapshotSource final : public ISnapshotSource {
public:
  bool requestSnapshot(uint32_t symbol_id, uint64_t min_seq_num) override {
    // Implement logic to request snapshot from the vendor
    return true; // Return true if the request was successful
  }

  bool next(PacketView &packet) override {
    // Implement logic to retrieve the next packet from the vendor
    return true; // Return true if a packet was successfully retrieved
  }
};