#pragma once

#include <optional>
#include <string>
#include <string_view>

// X11 clipboard integration via the xclip / xsel command-line tools.
//
// We can't link libX11 (project rule: stdlib only), so all interaction
// goes through one of those external programs. They are detected lazily
// on first use and cached; if neither is on PATH, or DISPLAY is unset,
// every method becomes a silent no-op so the editor still works
// unchanged on a tty without a graphical session.
//
// API assumptions:
//  - Single-threaded. Not safe to call from a signal handler.
//  - Writes (setPrimary / setClipboard) are best-effort and never block
//    the caller for more than the cost of a single fork (~1 ms): a
//    double-fork detach lets the grandchild xclip/xsel run independently
//    and be reaped by init.
//  - Reads (getClipboard) ARE synchronous, since the caller wants the
//    text immediately. Bounded by xclip/xsel startup (~30 ms typical).
//  - Errors are written to ~/.cx-errors via the existing error_log
//    module and never surfaced to the user.
class Clipboard {
public:
  Clipboard() = default;

  // Best-effort write to the X11 PRIMARY selection (the mouse-drag /
  // middle-click buffer). Empty text is ignored. Re-publishing the same
  // text as the previous successful call is a free no-op (no fork).
  void setPrimary(std::string_view text);

  // Best-effort write to the X11 CLIPBOARD selection (Ctrl-C/Ctrl-V).
  // Empty text is ignored. Not deduped — cut/copy is rare enough that
  // we always republish.
  void setClipboard(std::string_view text);

  // Synchronously read the X11 CLIPBOARD selection. Returns nullopt
  // when no backend is available, the read fails, or the selection is
  // empty.
  std::optional<std::string> getClipboard();

  // Mostly for tests: true iff a backend was detected on first use.
  bool active();

private:
  enum class Backend { Unknown, None, Xclip, Xsel };

  // Lazy probe: checks DISPLAY and walks PATH looking for xclip then
  // xsel. Idempotent and cached in `_backend`.
  void ensureDetected();

  // Spawn `argv` with `text` on stdin via a double-fork detach.
  void publishAsync(std::string_view text, char const* selection);

  // Spawn `argv` and read its stdout to EOF synchronously.
  std::optional<std::string> readSync(char const* selection);

  Backend _backend = Backend::Unknown;
  std::string _lastPrimary; // dedup key for setPrimary
};

namespace clipboard_detail {
// Walk a colon-separated PATH-style string for an executable named
// `name`. Returns the first hit, or empty if none. Exposed so tests
// can drive the lookup with synthetic PATH strings.
std::string findOnPath(std::string_view path, std::string_view name);
} // namespace clipboard_detail
