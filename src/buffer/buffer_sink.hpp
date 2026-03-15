#pragma once

class Buffer;

/// Abstract sink that receives the "active buffer" from BufferManager.
/// Decouples BufferManager from any concrete display layer so that
/// tests can supply a trivial stub instead of a real terminal.
class BufferSink {
public:
    virtual ~BufferSink() = default;

    /// Called by BufferManager when the active buffer changes.
    virtual void setBuffer(Buffer& buf) = 0;
};
