#pragma once

/**
 * @file buffer_sink.hpp
 * @brief BufferSink — the abstract interface that decouples BufferManager
 *        from the concrete display layer (Screen).
 */

class Buffer;

/**
 * @brief Abstract sink that receives the "active buffer" from
 *        BufferManager.
 *
 * Decouples BufferManager from any concrete display layer so that tests
 * can supply a trivial stub instead of a real terminal.
 */
class BufferSink {
public:
  virtual ~BufferSink() = default;

  BufferSink(BufferSink const&) = delete;
  BufferSink& operator=(BufferSink const&) = delete;

  /**
   * @brief Called by BufferManager when the active buffer changes.
   * @param buf The buffer that should become active.
   */
  virtual void setBuffer(Buffer& buf) = 0;

protected:
  BufferSink() = default;
};
