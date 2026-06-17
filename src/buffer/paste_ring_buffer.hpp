#pragma once

/**
 * @file paste_ring_buffer.hpp
 * @brief PasteRingBuffer — a read-only buffer displaying the contents of
 *        the paste ring.
 */

#include "buffer.hpp"
#include "paste_ring.hpp"

/**
 * @brief Read-only buffer displaying the paste-ring contents.
 *
 * Modelled after BufferListBuffer.
 */
class PasteRingBuffer : public Buffer {
public:
  PasteRingBuffer();

  bool readonly() const override { return true; }
  bool supportsLineNumberGutter() const override { return false; }

  /**
   * @brief Rebuild the lines from @p pr.
   * @return A ViewHint positioning the cursor at the top.
   */
  ViewHint refresh(PasteRing const& pr);
};
