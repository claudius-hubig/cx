#pragma once

#include "buffer.hpp"
#include "paste_ring.hpp"

/// Readonly buffer displaying paste ring contents. Modeled after
/// BufferListBuffer.
class PasteRingBuffer : public Buffer {
public:
  PasteRingBuffer();

  bool readonly() const override { return true; }
  bool supportsLineNumberGutter() const override { return false; }

  /// Rebuild _lines from paste ring contents. Returns a ViewHint
  /// positioning the cursor at the top.
  ViewHint refresh(PasteRing const& pr);
};
