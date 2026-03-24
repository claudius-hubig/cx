#include "subprocess.hpp"

#include <cerrno>
#include <cstring>

#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

// Read all available data from fd into out. Returns false on read error.
static bool readAll(int fd, std::string& out) {
  char buf[4096];
  for (;;) {
    auto n = read(fd, buf, sizeof(buf));
    if (n > 0)
      out.append(buf, static_cast<std::size_t>(n));
    else if (n == 0)
      return true;
    else if (errno == EINTR)
      continue;
    else
      return false;
  }
}

std::expected<SubprocessResult, std::string> runSubprocess(std::vector<std::string> const& args,
                                                           std::string_view input) {
  if (args.empty())
    return std::unexpected("empty command");

  // Three pipes: parent→child stdin, child→parent stdout, child→parent stderr.
  int pipeIn[2], pipeOut[2], pipeErr[2];
  if (pipe(pipeIn) != 0 || pipe(pipeOut) != 0 || pipe(pipeErr) != 0)
    return std::unexpected(std::string("pipe: ") + std::strerror(errno));

  pid_t pid = fork();
  if (pid < 0) {
    close(pipeIn[0]);
    close(pipeIn[1]);
    close(pipeOut[0]);
    close(pipeOut[1]);
    close(pipeErr[0]);
    close(pipeErr[1]);
    return std::unexpected(std::string("fork: ") + std::strerror(errno));
  }

  if (pid == 0) {
    // Child: wire up pipes and exec.
    dup2(pipeIn[0], STDIN_FILENO);
    dup2(pipeOut[1], STDOUT_FILENO);
    dup2(pipeErr[1], STDERR_FILENO);
    close(pipeIn[0]);
    close(pipeIn[1]);
    close(pipeOut[0]);
    close(pipeOut[1]);
    close(pipeErr[0]);
    close(pipeErr[1]);

    std::vector<char*> argv;
    for (auto& a : args)
      argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);

    execvp(argv[0], argv.data());
    // exec failed — write error to stderr and exit.
    auto msg = std::string("execvp: ") + std::strerror(errno) + "\n";
    write(STDERR_FILENO, msg.data(), msg.size());
    _exit(127);
  }

  // Parent: close child-side pipe ends.
  close(pipeIn[0]);
  close(pipeOut[1]);
  close(pipeErr[1]);

  // Use poll() to multiplex writing stdin and reading stdout/stderr,
  // avoiding deadlocks when the child's output fills the pipe buffer
  // before it has consumed all input.
  std::string stdoutData, stderrData;
  std::size_t written = 0;
  bool stdinOpen = true;

  for (;;) {
    std::vector<pollfd> fds;
    if (stdinOpen)
      fds.push_back({pipeIn[1], POLLOUT, 0});
    fds.push_back({pipeOut[0], POLLIN, 0});
    fds.push_back({pipeErr[0], POLLIN, 0});

    if (poll(fds.data(), fds.size(), -1) < 0) {
      if (errno == EINTR)
        continue;
      break;
    }

    for (auto& pfd : fds) {
      if (pfd.fd == pipeIn[1] && (pfd.revents & (POLLOUT | POLLERR))) {
        if (written < input.size()) {
          auto n = write(pipeIn[1], input.data() + written, input.size() - written);
          if (n > 0)
            written += static_cast<std::size_t>(n);
          else if (n < 0 && errno != EINTR) {
            close(pipeIn[1]);
            stdinOpen = false;
          }
        } else {
          close(pipeIn[1]);
          stdinOpen = false;
        }
      }
      if (pfd.fd == pipeOut[0] && (pfd.revents & (POLLIN | POLLHUP))) {
        char buf[4096];
        auto n = read(pipeOut[0], buf, sizeof(buf));
        if (n > 0)
          stdoutData.append(buf, static_cast<std::size_t>(n));
      }
      if (pfd.fd == pipeErr[0] && (pfd.revents & (POLLIN | POLLHUP))) {
        char buf[4096];
        auto n = read(pipeErr[0], buf, sizeof(buf));
        if (n > 0)
          stderrData.append(buf, static_cast<std::size_t>(n));
      }
    }

    // Check if both output pipes have closed (child exited).
    bool outDone = false, errDone = false;
    for (auto& pfd : fds) {
      if (pfd.fd == pipeOut[0] && (pfd.revents & POLLHUP) && !(pfd.revents & POLLIN))
        outDone = true;
      if (pfd.fd == pipeErr[0] && (pfd.revents & POLLHUP) && !(pfd.revents & POLLIN))
        errDone = true;
    }
    if (outDone && errDone && !stdinOpen)
      break;
  }

  if (stdinOpen)
    close(pipeIn[1]);

  // Drain any remaining data.
  readAll(pipeOut[0], stdoutData);
  readAll(pipeErr[0], stderrData);
  close(pipeOut[0]);
  close(pipeErr[0]);

  int status = 0;
  waitpid(pid, &status, 0);
  int exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);

  return SubprocessResult{exitCode, std::move(stdoutData), std::move(stderrData)};
}
