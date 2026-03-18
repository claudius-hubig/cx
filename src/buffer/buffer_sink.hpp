#pragma once

class Buffer;

/// Abstract sink that receives the "active buffer" from BufferManager.
/// Decouples BufferManager from any concrete display layer so that
/// tests can supply a trivial stub instead of a real terminal.
class BufferSink {
public:
  virtual ~BufferSink() = default;

  BufferSink(BufferSink const&) = delete;
  BufferSink& operator=(BufferSink const&) = delete;

  /// Called by BufferManager when the active buffer changes.
  virtual void setBuffer(Buffer& buf) = 0;

protected:
  BufferSink() = default;
};
