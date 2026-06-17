#pragma once

/**
 * @file clipboard.hpp
 * @brief X11 PRIMARY/CLIPBOARD integration via the xclip / xsel
 *        command-line tools.
 *
 * The project rule forbids linking libX11 (stdlib only), so all
 * interaction goes through one of those external programs. They are
 * detected lazily on first use and cached; if neither is on PATH, or
 * DISPLAY is unset, every method becomes a silent no-op so the editor
 * still works unchanged on a tty without a graphical session.
 *
 * API assumptions:
 * - Single-threaded; not safe to call from a signal handler.
 * - Writes (setPrimary / setClipboard) are best-effort and never block
 *   the caller for more than the cost of a single fork (~1 ms): a
 *   double-fork detach lets the grandchild xclip/xsel run independently
 *   and be reaped by init.
 * - Reads (getClipboard / getPrimary) ARE synchronous, since the caller
 *   wants the text immediately. Bounded by xclip/xsel startup (~30 ms).
 * - Errors are written to ~/.cx-errors via error_log and never surfaced
 *   to the user.
 */

#include <optional>
#include <string>
#include <string_view>

/** @brief Best-effort X11 clipboard adapter backed by xclip/xsel. */
class Clipboard {
public:
  Clipboard() = default;

  /**
   * @brief Best-effort write to the X11 PRIMARY selection (the mouse-drag
   *        / middle-click buffer).
   *
   * Empty text is ignored. Re-publishing text identical to the previous
   * successful call is a free no-op (no fork).
   */
  void setPrimary(std::string_view text);

  /**
   * @brief Best-effort write to the X11 CLIPBOARD selection
   *        (Ctrl-C/Ctrl-V).
   *
   * Empty text is ignored. Not deduped — cut/copy is rare enough that we
   * always republish.
   */
  void setClipboard(std::string_view text);

  /**
   * @brief Synchronously read the X11 CLIPBOARD selection.
   * @return The selection text, or nullopt when no backend is available,
   *         the read fails, or the selection is empty.
   */
  std::optional<std::string> getClipboard();

  /**
   * @brief Synchronously read the X11 PRIMARY selection (mouse-drag /
   *        middle-click buffer).
   *
   * Same nullopt contract as getClipboard(); used by the in-app
   * middle-click paste.
   */
  std::optional<std::string> getPrimary();

  /**
   * @brief Whether a backend was detected on first use.
   * @return True if xclip or xsel is available. Mostly for tests.
   */
  bool active();

private:
  enum class Backend { Unknown, None, Xclip, Xsel };

  /**
   * @brief Lazy probe: check DISPLAY and walk PATH for xclip then xsel.
   *
   * Idempotent; the result is cached in @c _backend.
   */
  void ensureDetected();

  /** @brief Spawn the backend with @p text on stdin via a double-fork detach. */
  void publishAsync(std::string_view text, char const* selection);

  /** @brief Spawn the backend and read its stdout to EOF synchronously. */
  std::optional<std::string> readSync(char const* selection);

  /**
   * @brief Shared body of getClipboard() / getPrimary().
   *
   * Detects a backend, reads @p selection synchronously, and maps an
   * absent backend or empty result to nullopt.
   * @param selection "clipboard" or "primary".
   */
  std::optional<std::string> getSelection(char const* selection);

  Backend _backend = Backend::Unknown;
  std::string _lastPrimary; /**< Dedup key for setPrimary. */
};

/** @brief Implementation helpers exposed for testing. */
namespace clipboard_detail {
/**
 * @brief Walk a colon-separated PATH-style string for an executable named
 *        @p name.
 *
 * Exposed so tests can drive the lookup with synthetic PATH strings.
 * @return The first hit, or an empty string if none is found.
 */
std::string findOnPath(std::string_view path, std::string_view name);
} // namespace clipboard_detail
