// LD_PRELOAD guard for fuzzing cx.
//  - Mutating filesystem calls outside $FUZZ_ROOT (except /dev/) fail with EACCES
//    and are logged to $FUZZ_GUARDLOG.
//  - exec is restricted to a whitelist; `/bin/sh -c CMD` runs only whitelisted
//    CMDs, anything else is replaced by `cat`.
//  - Fatal signals print a backtrace to $FUZZ_CRASHLOG.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <execinfo.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char root[PATH_MAX];
static size_t rootLen;

static void glog(const char* what, const char* path) {
  const char* lp = getenv("FUZZ_GUARDLOG");
  if (!lp)
    return;
  int (*ropen)(const char*, int, ...) = dlsym(RTLD_NEXT, "open");
  int fd = ropen(lp, O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (fd < 0)
    return;
  dprintf(fd, "DENY %s %s\n", what, path ? path : "(null)");
  close(fd);
}

// Resolve dirfd/path to an absolute path whose parent is canonicalised.
static int inside(int dirfd, const char* path) {
  if (!path)
    return 0;
  // Device files are fine to write (/dev/null), except the terminals by
  // name: a buffer saved to /dev/pts/N (the directory browser goes there)
  // would write raw text into the fuzzer's terminal stream. /dev/tty stays
  // open to cx, which reads keys from it when stdin is a pipe.
  if (!strncmp(path, "/dev/pts/", 9))
    return 0;
  if (!strncmp(path, "/dev/", 5))
    return 1;
  if (!rootLen)
    return 0;
  char full[PATH_MAX * 2];
  if (path[0] == '/') {
    snprintf(full, sizeof full, "%s", path);
  } else {
    char base[PATH_MAX];
    if (dirfd == AT_FDCWD) {
      if (!getcwd(base, sizeof base))
        return 0;
    } else {
      char link[64];
      snprintf(link, sizeof link, "/proc/self/fd/%d", dirfd);
      ssize_t n = readlink(link, base, sizeof base - 1);
      if (n < 0)
        return 0;
      base[n] = 0;
    }
    snprintf(full, sizeof full, "%s/%s", base, path);
  }
  // Canonicalise the parent directory, keep the final component.
  char tmp1[PATH_MAX * 2], tmp2[PATH_MAX * 2];
  snprintf(tmp1, sizeof tmp1, "%s", full);
  snprintf(tmp2, sizeof tmp2, "%s", full);
  char* dir = dirname(tmp1);
  char* leaf = basename(tmp2);
  char rdir[PATH_MAX];
  if (!realpath(dir, rdir))
    // A parent that does not exist (or is no directory) makes the call
    // fail on its own: nothing can be created or changed there.
    return errno == ENOENT || errno == ENOTDIR;
  if (!strcmp(leaf, ".."))
    return 0;
  char res[PATH_MAX * 2];
  snprintf(res, sizeof res, "%s/%s", rdir, leaf);
  return !strncmp(res, root, rootLen) && res[rootLen] == '/'; // root itself is off-limits
}

static int writes(int flags) { return flags & (O_WRONLY | O_RDWR | O_CREAT | O_TRUNC | O_APPEND); }

#define REAL(name, ret, ...) static ret (*real_##name)(__VA_ARGS__); if (!real_##name) real_##name = dlsym(RTLD_NEXT, #name)
#define DENY(what, p) do { glog(what, p); errno = EACCES; return -1; } while (0)

int open(const char* p, int flags, ...) {
  REAL(open, int, const char*, int, ...);
  mode_t m = 0;
  if (flags & (O_CREAT | O_TMPFILE)) { va_list a; va_start(a, flags); m = va_arg(a, mode_t); va_end(a); }
  if (writes(flags) && !inside(AT_FDCWD, p)) DENY("open", p);
  return real_open(p, flags, m);
}
int open64(const char* p, int flags, ...) {
  REAL(open64, int, const char*, int, ...);
  mode_t m = 0;
  if (flags & (O_CREAT | O_TMPFILE)) { va_list a; va_start(a, flags); m = va_arg(a, mode_t); va_end(a); }
  if (writes(flags) && !inside(AT_FDCWD, p)) DENY("open64", p);
  return real_open64(p, flags, m);
}
int openat(int d, const char* p, int flags, ...) {
  REAL(openat, int, int, const char*, int, ...);
  mode_t m = 0;
  if (flags & (O_CREAT | O_TMPFILE)) { va_list a; va_start(a, flags); m = va_arg(a, mode_t); va_end(a); }
  if (writes(flags) && !inside(d, p)) DENY("openat", p);
  return real_openat(d, p, flags, m);
}
int openat64(int d, const char* p, int flags, ...) {
  REAL(openat64, int, int, const char*, int, ...);
  mode_t m = 0;
  if (flags & (O_CREAT | O_TMPFILE)) { va_list a; va_start(a, flags); m = va_arg(a, mode_t); va_end(a); }
  if (writes(flags) && !inside(d, p)) DENY("openat64", p);
  return real_openat64(d, p, flags, m);
}
int creat(const char* p, mode_t m) { REAL(creat, int, const char*, mode_t); if (!inside(AT_FDCWD, p)) DENY("creat", p); return real_creat(p, m); }
int creat64(const char* p, mode_t m) { REAL(creat64, int, const char*, mode_t); if (!inside(AT_FDCWD, p)) DENY("creat64", p); return real_creat64(p, m); }
static int fmodeWrites(const char* m) { return strpbrk(m, "wa+") != NULL; }
FILE* fopen(const char* p, const char* m) {
  REAL(fopen, FILE*, const char*, const char*);
  if (fmodeWrites(m) && !inside(AT_FDCWD, p)) { glog("fopen", p); errno = EACCES; return NULL; }
  return real_fopen(p, m);
}
FILE* fopen64(const char* p, const char* m) {
  REAL(fopen64, FILE*, const char*, const char*);
  if (fmodeWrites(m) && !inside(AT_FDCWD, p)) { glog("fopen64", p); errno = EACCES; return NULL; }
  return real_fopen64(p, m);
}
int unlink(const char* p) { REAL(unlink, int, const char*); if (!inside(AT_FDCWD, p)) DENY("unlink", p); return real_unlink(p); }
int unlinkat(int d, const char* p, int f) { REAL(unlinkat, int, int, const char*, int); if (!inside(d, p)) DENY("unlinkat", p); return real_unlinkat(d, p, f); }
int rmdir(const char* p) { REAL(rmdir, int, const char*); if (!inside(AT_FDCWD, p)) DENY("rmdir", p); return real_rmdir(p); }
int remove(const char* p) { REAL(remove, int, const char*); if (!inside(AT_FDCWD, p)) DENY("remove", p); return real_remove(p); }
int rename(const char* a, const char* b) { REAL(rename, int, const char*, const char*); if (!inside(AT_FDCWD, a) || !inside(AT_FDCWD, b)) DENY("rename", a); return real_rename(a, b); }
int renameat(int da, const char* a, int db, const char* b) { REAL(renameat, int, int, const char*, int, const char*); if (!inside(da, a) || !inside(db, b)) DENY("renameat", a); return real_renameat(da, a, db, b); }
int renameat2(int da, const char* a, int db, const char* b, unsigned f) { REAL(renameat2, int, int, const char*, int, const char*, unsigned); if (!inside(da, a) || !inside(db, b)) DENY("renameat2", a); return real_renameat2(da, a, db, b, f); }
int mkdir(const char* p, mode_t m) { REAL(mkdir, int, const char*, mode_t); if (!inside(AT_FDCWD, p)) DENY("mkdir", p); return real_mkdir(p, m); }
int mkdirat(int d, const char* p, mode_t m) { REAL(mkdirat, int, int, const char*, mode_t); if (!inside(d, p)) DENY("mkdirat", p); return real_mkdirat(d, p, m); }
int truncate(const char* p, off_t l) { REAL(truncate, int, const char*, off_t); if (!inside(AT_FDCWD, p)) DENY("truncate", p); return real_truncate(p, l); }
int symlink(const char* a, const char* b) { REAL(symlink, int, const char*, const char*); if (!inside(AT_FDCWD, b)) DENY("symlink", b); return real_symlink(a, b); }
int link(const char* a, const char* b) { REAL(link, int, const char*, const char*); if (!inside(AT_FDCWD, b)) DENY("link", b); return real_link(a, b); }
int chmod(const char* p, mode_t m) { REAL(chmod, int, const char*, mode_t); if (!inside(AT_FDCWD, p)) DENY("chmod", p); return real_chmod(p, m); }

// ---- exec whitelist ----
static const char* okShell[] = {
    "sort", "tr a-z A-Z", "cat", "false", "true", "rev", "head -c 0",
    "yes | head -c 3000000", "printf '\\033[2J\\033]0;x\\007\\r\\n'", "printf 'a\\0b\\377\\n'",
    "sleep 3", "echo err >&2", "echo err >&2; exit 1", "head -c 200000 /dev/urandom", NULL};
static const char* okProg[] = {"grep", "git", "clang-format", "xclip", "xsel", "cat", "sort", "tr", "rev", "head", "yes", "sleep", "printf", "false", "true", NULL};

static int listed(const char* s, const char** l) {
  for (; *l; ++l)
    if (!strcmp(s, *l))
      return 1;
  return 0;
}

static char* const catArgv[] = {"cat", NULL};

// Returns argv to run, or NULL to refuse.
static char* const* vet(const char* file, char* const argv[]) {
  if (!argv || !argv[0])
    return NULL;
  char tmp[PATH_MAX];
  snprintf(tmp, sizeof tmp, "%s", file);
  const char* b = basename(tmp);
  if (!strcmp(b, "sh") && argv[1] && !strcmp(argv[1], "-c") && argv[2]) {
    if (listed(argv[2], okShell))
      return argv;
    glog("sh-replaced-by-cat", argv[2]);
    return catArgv;
  }
  if (listed(b, okProg)) {
    if (!strcmp(b, "git") && !(argv[1] && (!strcmp(argv[1], "rev-parse") || !strcmp(argv[1], "grep"))))
      return NULL;
    return argv;
  }
  glog("exec-refused", file);
  return NULL;
}

int execvp(const char* file, char* const argv[]) {
  REAL(execvp, int, const char*, char* const*);
  char* const* a = vet(file, argv);
  if (!a) { errno = ENOENT; return -1; }
  return real_execvp(a == catArgv ? "cat" : file, a);
}
int execve(const char* file, char* const argv[], char* const envp[]) {
  REAL(execve, int, const char*, char* const*, char* const*);
  char* const* a = vet(file, argv);
  if (!a) { errno = ENOENT; return -1; }
  return real_execve(a == catArgv ? "/usr/bin/cat" : file, a, envp);
}
int execv(const char* file, char* const argv[]) {
  REAL(execv, int, const char*, char* const*);
  char* const* a = vet(file, argv);
  if (!a) { errno = ENOENT; return -1; }
  return real_execv(a == catArgv ? "/usr/bin/cat" : file, a);
}

// ---- crash backtraces ----
static void onFatal(int sig) {
  const char* lp = getenv("FUZZ_CRASHLOG");
  int (*ropen)(const char*, int, ...) = dlsym(RTLD_NEXT, "open");
  int fd = lp ? ropen(lp, O_WRONLY | O_CREAT | O_APPEND, 0644) : 2;
  if (fd >= 0) {
    dprintf(fd, "SIGNAL %d\n", sig);
    void* bt[128];
    int n = backtrace(bt, 128);
    backtrace_symbols_fd(bt, n, fd);
  }
  signal(sig, SIG_DFL);
  raise(sig);
}

__attribute__((constructor)) static void init(void) {
  const char* r = getenv("FUZZ_ROOT");
  if (r && realpath(r, root))
    rootLen = strlen(root);
  void* bt[4];
  backtrace(bt, 4); // load libgcc now, not inside the handler
  int sigs[] = {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT};
  for (unsigned i = 0; i < sizeof sigs / sizeof *sigs; ++i)
    signal(sigs[i], onFatal);
}
