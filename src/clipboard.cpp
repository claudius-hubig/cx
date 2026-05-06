#include "clipboard.hpp"

#include "error_log.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>

namespace {

// Walk a PATH-style colon-separated list looking for an executable
// `name`. Returns the first hit, or empty string if none. Pulled out
// of the class so tests can drive it with synthetic PATH strings
// without touching the filesystem ordering rules. (We re-expose it
// in the clipboard_detail namespace below.)
std::string findOnPathImpl(std::string_view path, std::string_view name) {
  if (path.empty() || name.empty())
    return {};
  std::size_t i = 0;
  while (i <= path.size()) {
    auto colon = path.find(':', i);
    auto end = (colon == std::string_view::npos) ? path.size() : colon;
    auto dir = path.substr(i, end - i);
    if (!dir.empty()) {
      std::string candidate(dir);
      if (candidate.back() != '/')
        candidate += '/';
      candidate.append(name);
      if (::access(candidate.c_str(), X_OK) == 0)
        return candidate;
    }
    if (end == path.size())
      break;
    i = end + 1;
  }
  return {};
}

// Redirect /dev/null onto fd; best-effort, ignores failure.
void redirectNull(int fd, int flags) {
  int devnull = ::open("/dev/null", flags);
  if (devnull < 0)
    return;
  ::dup2(devnull, fd);
  if (devnull != fd)
    ::close(devnull);
}

} // namespace

namespace clipboard_detail {

// Re-export the PATH-walk for tests.
std::string findOnPath(std::string_view path, std::string_view name) {
  return findOnPathImpl(path, name);
}

} // namespace clipboard_detail

// ---------------------------------------------------------------------------

void Clipboard::ensureDetected() {
  if (_backend != Backend::Unknown)
    return;

  char const* display = std::getenv("DISPLAY");
  if (!display || display[0] == '\0') {
    _backend = Backend::None;
    return;
  }

  char const* path = std::getenv("PATH");
  std::string_view pathView = path ? std::string_view(path) : std::string_view{};
  if (!findOnPathImpl(pathView, "xclip").empty()) {
    _backend = Backend::Xclip;
    return;
  }
  if (!findOnPathImpl(pathView, "xsel").empty()) {
    _backend = Backend::Xsel;
    return;
  }
  _backend = Backend::None;
}

bool Clipboard::active() {
  ensureDetected();
  return _backend == Backend::Xclip || _backend == Backend::Xsel;
}

void Clipboard::setPrimary(std::string_view text) {
  if (text.empty())
    return;
  ensureDetected();
  if (_backend == Backend::None)
    return;
  if (text == _lastPrimary)
    return;
  _lastPrimary.assign(text);
  publishAsync(text, "primary");
}

void Clipboard::setClipboard(std::string_view text) {
  if (text.empty())
    return;
  ensureDetected();
  if (_backend == Backend::None)
    return;
  publishAsync(text, "clipboard");
}

std::optional<std::string> Clipboard::getClipboard() {
  ensureDetected();
  if (_backend == Backend::None)
    return std::nullopt;
  auto out = readSync("clipboard");
  if (!out || out->empty())
    return std::nullopt;
  return out;
}

// Spawn the chosen backend with `text` on stdin and detach it via a
// double-fork so the parent never blocks on xclip/xsel startup or on
// their post-read daemonization. Errors are logged but never surfaced.
void Clipboard::publishAsync(std::string_view text, char const* selection) {
  // Resolve argv from (backend, selection). We hold the pointers as
  // const because execvp takes char* const argv[]; we cast away const
  // at call time, which is the standard idiom (POSIX guarantees
  // execvp doesn't write through them).
  char const* argv[6] = {nullptr};
  if (_backend == Backend::Xclip) {
    argv[0] = "xclip";
    argv[1] = "-selection";
    argv[2] = selection; // "primary" or "clipboard"
    argv[3] = "-i";
    argv[4] = nullptr;
  } else if (_backend == Backend::Xsel) {
    argv[0] = "xsel";
    argv[1] = (std::strcmp(selection, "primary") == 0) ? "--primary" : "--clipboard";
    argv[2] = "--input";
    argv[3] = nullptr;
  } else {
    return;
  }

  int p[2];
  if (::pipe(p) != 0) {
    logError(std::string("clipboard pipe: ") + std::strerror(errno));
    return;
  }

  pid_t mid = ::fork();
  if (mid < 0) {
    logError(std::string("clipboard fork: ") + std::strerror(errno));
    ::close(p[0]);
    ::close(p[1]);
    return;
  }

  if (mid == 0) {
    // Intermediate child: fork again so the grandchild is reparented
    // to init when we exit, and init takes care of reaping it.
    pid_t grand = ::fork();
    if (grand < 0)
      ::_exit(127);
    if (grand == 0) {
      // Grandchild: connect stdin to the pipe's read end, silence
      // stdout/stderr (we don't want xclip writing to the terminal),
      // then exec.
      if (::dup2(p[0], STDIN_FILENO) < 0)
        ::_exit(127);
      ::close(p[0]);
      ::close(p[1]);
      redirectNull(STDOUT_FILENO, O_WRONLY);
      redirectNull(STDERR_FILENO, O_WRONLY);
      ::execvp(argv[0], const_cast<char* const*>(argv));
      ::_exit(127);
    }
    // Intermediate child immediately exits, orphaning the grandchild.
    ::_exit(0);
  }

  // Original parent: send `text` to the grandchild via p[1] and reap
  // the intermediate child (not the grandchild — init does that).
  ::close(p[0]);

  std::size_t written = 0;
  while (written < text.size()) {
    ssize_t n = ::write(p[1], text.data() + written, text.size() - written);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      // EPIPE means the grandchild exec failed (or xclip exited
      // before reading everything). Either way we can't recover.
      if (errno != EPIPE)
        logError(std::string("clipboard write: ") + std::strerror(errno));
      break;
    }
    if (n == 0)
      break;
    written += static_cast<std::size_t>(n);
  }

  ::close(p[1]);

  // Reap the intermediate child. It has already _exit(0)'d, so this
  // returns essentially instantly.
  int status = 0;
  while (::waitpid(mid, &status, 0) < 0) {
    if (errno != EINTR)
      break;
  }
}

// Synchronous read of a selection. Used only on Paste, so the cost of
// blocking the caller for one xclip startup is acceptable.
std::optional<std::string> Clipboard::readSync(char const* selection) {
  char const* argv[6] = {nullptr};
  if (_backend == Backend::Xclip) {
    argv[0] = "xclip";
    argv[1] = "-selection";
    argv[2] = selection;
    argv[3] = "-o";
    argv[4] = nullptr;
  } else if (_backend == Backend::Xsel) {
    argv[0] = "xsel";
    argv[1] = (std::strcmp(selection, "primary") == 0) ? "--primary" : "--clipboard";
    argv[2] = "--output";
    argv[3] = nullptr;
  } else {
    return std::nullopt;
  }

  int p[2];
  if (::pipe(p) != 0) {
    logError(std::string("clipboard pipe: ") + std::strerror(errno));
    return std::nullopt;
  }

  pid_t pid = ::fork();
  if (pid < 0) {
    logError(std::string("clipboard fork: ") + std::strerror(errno));
    ::close(p[0]);
    ::close(p[1]);
    return std::nullopt;
  }

  if (pid == 0) {
    // Child: stdout -> pipe's write end, silence stderr, close
    // unused stdin (leave the inherited terminal stdin alone — xclip
    // -o ignores it).
    if (::dup2(p[1], STDOUT_FILENO) < 0)
      ::_exit(127);
    ::close(p[0]);
    ::close(p[1]);
    redirectNull(STDERR_FILENO, O_WRONLY);
    ::execvp(argv[0], const_cast<char* const*>(argv));
    ::_exit(127);
  }

  ::close(p[1]);

  std::string out;
  char buf[4096];
  while (true) {
    ssize_t n = ::read(p[0], buf, sizeof(buf));
    if (n < 0) {
      if (errno == EINTR)
        continue;
      logError(std::string("clipboard read: ") + std::strerror(errno));
      out.clear();
      break;
    }
    if (n == 0)
      break;
    out.append(buf, static_cast<std::size_t>(n));
  }
  ::close(p[0]);

  int status = 0;
  while (::waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR)
      break;
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
    return std::nullopt;

  return out;
}
