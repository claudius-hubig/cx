#pragma once

/**
 * @file fifo.hpp
 * @brief Read a FIFO whole, without blocking: a named pipe, or a pipe passed as /dev/fd/N.
 *
 * A FIFO has no size and can be read only once. So cx does not open it as a file. It
 * reads all of it into a scratch buffer: bash's `cx <(cmd)` on the command line, or a
 * path typed into Ctrl-o.
 *
 * The caller waits in poll() on fd(). This relies on Linux poll() behaviour, which POSIX
 * leaves open:
 * - A named FIFO opened with O_NONBLOCK reports nothing until a writer connects.
 * - It reports POLLHUP when all writers have closed it.
 * - An anonymous pipe (as /dev/fd/N) reports POLLHUP at once if it has no writers left.
 */

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <expected>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>

/** @brief True if @p path is a FIFO. Symlinks are followed, so /dev/fd/N of a pipe counts. */
inline bool isFifo(std::filesystem::path const& path) {
  std::error_code ec;
  return std::filesystem::is_fifo(path, ec);
}

/** @brief An open FIFO and the text read from it so far. Owns the descriptor. */
class FifoReader {
public:
  /** @brief Open @p path for reading. Never waits for a writer (O_NONBLOCK). */
  static std::expected<FifoReader, std::error_code> open(std::filesystem::path const& path) {
    int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
      return std::unexpected(std::error_code(errno, std::generic_category()));
    return FifoReader(fd);
  }

  FifoReader(FifoReader&& other) noexcept
      : _fd(std::exchange(other._fd, -1)), _text(std::move(other._text)) {}
  FifoReader& operator=(FifoReader&&) = delete;
  ~FifoReader() {
    if (_fd >= 0)
      ::close(_fd);
  }

  /** @brief The descriptor to poll() for POLLIN. */
  int fd() const { return _fd; }

  /**
   * @brief Read all the data that is there now. Returns true at the end of the input.
   *
   * @pre poll() reported POLLIN, POLLHUP or POLLERR on fd(). Before a writer connects,
   *      read() returns 0, and this would take that for the end.
   *
   * The input ends when the last writer closes the FIFO, or on a read error.
   */
  bool readAvailable() {
    char buf[65536];
    for (;;) {
      ssize_t n = ::read(_fd, buf, sizeof(buf));
      if (n > 0)
        _text.append(buf, static_cast<std::size_t>(n));
      else if (n == 0)
        return true;
      else if (errno != EINTR)
        return errno != EAGAIN; // EAGAIN: a writer is still connected
    }
  }

  /** @brief The text read so far. */
  std::string const& text() const { return _text; }
  /** @brief Move the text out. */
  std::string takeText() { return std::move(_text); }

private:
  explicit FifoReader(int fd) : _fd(fd) {}

  int _fd;
  std::string _text;
};
