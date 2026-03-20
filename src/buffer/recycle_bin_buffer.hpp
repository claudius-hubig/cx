#pragma once

#include "buffer.hpp"
#include "recycle_bin.hpp"

/// Readonly buffer displaying recycle bin contents. Modeled after
/// BufferListBuffer.
class RecycleBinBuffer : public Buffer {
public:
  RecycleBinBuffer();

  bool readonly() const override { return true; }

  /// Rebuild _lines from recycle bin contents.
  void refresh(RecycleBin const& rb);
};
