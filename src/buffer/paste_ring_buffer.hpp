#pragma once

#include "buffer.hpp"
#include "paste_ring.hpp"

/// Readonly buffer displaying paste ring contents. Modeled after
/// BufferListBuffer.
class PasteRingBuffer : public Buffer {
public:
  PasteRingBuffer();

  bool readonly() const override { return true; }

  /// Rebuild _lines from paste ring contents.
  void refresh(PasteRing const& pr);
};
