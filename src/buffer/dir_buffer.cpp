#include "dir_buffer.hpp"
#include "byte_constants.hpp"
#include "fs_path.hpp"
#include "utf8.hpp"

#include <algorithm>
#include <ctime>
#include <grp.h>
#include <pwd.h>
#include <sys/stat.h>

namespace {

/**
 * @brief Format a byte count with a K/M/G suffix.
 *
 * Suffixes are 1024-based but rendered without the 'i' to match the
 * long-standing POSIX `ls -h` convention.
 */
std::string formatSize(std::uintmax_t bytes) {
  if (bytes < 1024)
    return std::to_string(bytes);
  if (bytes < 1024 * 1024)
    return std::to_string(bytes / 1024) + "K";
  if (bytes < 1024 * 1024 * 1024)
    return std::to_string(bytes / (1024 * 1024)) + "M";
  return std::to_string(bytes / (1024 * 1024 * 1024)) + "G";
}

/** @brief Build an `ls`-style permission string (e.g. "drwxr-xr-x"). */
std::string formatPermissions(mode_t mode, bool isDir, bool isSymlink) {
  std::string s;
  s += isSymlink ? 'l' : isDir ? 'd' : '-';
  s += (mode & S_IRUSR) ? 'r' : '-';
  s += (mode & S_IWUSR) ? 'w' : '-';
  s += (mode & S_IXUSR) ? 'x' : '-';
  s += (mode & S_IRGRP) ? 'r' : '-';
  s += (mode & S_IWGRP) ? 'w' : '-';
  s += (mode & S_IXGRP) ? 'x' : '-';
  s += (mode & S_IROTH) ? 'r' : '-';
  s += (mode & S_IWOTH) ? 'w' : '-';
  s += (mode & S_IXOTH) ? 'x' : '-';
  return s;
}

/** @brief Format a modification time as "YYYY-MM-DD HH:MM:SS" (local time). */
std::string formatMtime(struct timespec const& ts) {
  std::tm tm{};
  localtime_r(&ts.tv_sec, &tm);
  char buf[20]; // "YYYY-MM-DD HH:MM:SS"
  std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
  return buf;
}

/**
 * @brief Truncate a (possibly ANSI-styled) display string to at most
 *        @p maxCols visual columns.
 *
 * Cuts only on whole character / escape-sequence boundaries so a
 * multi-byte char or CSI sequence is never split. The width walk is
 * CSI-aware (displayCharInfoAt) because display lines may carry
 * escapeNonPrintable styling for control-byte filenames.
 */
std::string truncateDisplayToWidth(std::string_view s, std::size_t maxCols) {
  std::size_t col = 0, i = 0;
  while (i < s.size()) {
    auto info = displayCharInfoAt(s.data() + i, s.size() - i);
    if (col + info.width > maxCols)
      break;
    col += info.width;
    i += info.len;
  }
  return std::string(s.substr(0, i));
}

} // namespace

namespace dir_buffer_detail {
// Owner and group names come from getpwuid / getgrgid, which on a
// non-trivial deployment (LDAP, NIS, SSSD) can include bytes a local
// user did not author. Replace C0 controls (incl. ESC, BEL), DEL, and
// the 8-bit C1 range with '?' so the dir listing — which writes its
// display lines straight to the terminal, not through the buffer
// renderer that styles control bytes — can't be hijacked by a hostile
// account name. Length-preserving substitution keeps the existing
// `size()`-based column alignment working without further changes.
std::string sanitizeNssField(std::string s) {
  for (auto& c : s) {
    auto b = static_cast<unsigned char>(c);
    if (b < 0x20 || b == 0x7F || (b >= 0x80 && b < 0xA0))
      c = '?';
  }
  return s;
}
} // namespace dir_buffer_detail

DirBuffer::DirBuffer(std::filesystem::path const& dir)
    : Buffer(canonicalPath(dir).string()), _dir(canonicalPath(dir)) {
  refresh();
}

ViewHint DirBuffer::navigate(std::filesystem::path const& dir) {
  _dir = canonicalPath(dir);
  _markedForDelete.clear();
  return refresh();
}

ViewHint DirBuffer::toggleHidden(BufferPoint currentPoint) {
  std::string prevName;
  if (currentPoint.line < _entries.size())
    prevName = _entries[currentPoint.line].name;

  _showHidden = !_showHidden;
  _markedForDelete.clear();
  return refresh(prevName);
}

void DirBuffer::toggleDeleteMark(BufferPoint p) {
  // Refuse to mark . and .. (always at indices 0 and 1).
  if (p.line < 2 || p.line >= _entries.size())
    return;
  if (_markedForDelete.contains(p.line))
    _markedForDelete.erase(p.line);
  else
    _markedForDelete.insert(p.line);
}

std::vector<std::filesystem::path> DirBuffer::markedPaths() const {
  std::vector<std::filesystem::path> result;
  for (auto idx : _markedForDelete) {
    if (idx < _entries.size())
      result.push_back(_entries[idx].path);
  }
  return result;
}

void DirBuffer::clearMarks() { _markedForDelete.clear(); }

DirBuffer::DeleteResult DirBuffer::executeDelete() {
  std::size_t count = 0;
  std::string errors;
  for (auto idx : _markedForDelete) {
    if (idx >= _entries.size())
      continue;
    std::error_code ec;
    // A symlink — even one pointing at a directory (isDir follows the
    // link) — is unlinked, never recursed into: recursing would delete
    // the target's contents, which live outside this listing. Only a
    // real directory takes the recursive remove_all path.
    if (_entries[idx].isDir && !_entries[idx].isSymlink)
      std::filesystem::remove_all(_entries[idx].path, ec);
    else
      std::filesystem::remove(_entries[idx].path, ec);
    if (ec) {
      if (!errors.empty())
        errors += "; ";
      errors += escapeNonPrintable(_entries[idx].name) + ": " + ec.message();
    } else {
      ++count;
    }
  }
  _markedForDelete.clear();
  auto hint = refresh();
  return {count, errors, hint};
}

DirBuffer::ChangeResult DirBuffer::renameEntry(BufferPoint p, std::string const& newName) {
  if (p.line >= _entries.size())
    return {false, "no entry selected", refresh()};
  auto const& entry = _entries[p.line];
  if (entry.name == "." || entry.name == "..")
    return {false, "cannot rename . or ..", refresh(entry.name)};
  if (newName.empty() || newName == "." || newName == "..")
    return {false, "invalid name", refresh(entry.name)};
  if (newName.find('/') != std::string::npos)
    return {false, "name may not contain '/'", refresh(entry.name)};
  auto target = _dir / newName;
  std::error_code ec;
  if (std::filesystem::exists(target, ec))
    return {false, "target already exists", refresh(entry.name)};
  std::filesystem::rename(entry.path, target, ec);
  if (ec)
    return {false, ec.message(), refresh(entry.name)};
  // Refresh and place cursor on the renamed entry.
  return {true, {}, refresh(newName)};
}

DirBuffer::ChangeResult DirBuffer::makeSubdir(std::string const& newName) {
  if (newName.empty() || newName == "." || newName == "..")
    return {false, "invalid name", refresh()};
  if (newName.find('/') != std::string::npos)
    return {false, "name may not contain '/'", refresh()};
  auto target = _dir / newName;
  std::error_code ec;
  if (std::filesystem::exists(target, ec))
    return {false, "target already exists", refresh()};
  if (!std::filesystem::create_directory(target, ec) || ec)
    return {false, ec ? ec.message() : "create_directory failed", refresh()};
  return {true, {}, refresh(newName)};
}

std::filesystem::path DirBuffer::selectedPath(BufferPoint p) const {
  if (p.line < _entries.size())
    return _entries[p.line].path;
  return _dir;
}

bool DirBuffer::selectedIsDir(BufferPoint p) const {
  if (p.line < _entries.size())
    return _entries[p.line].isDir;
  return false;
}

ViewHint DirBuffer::refresh(std::string const& prevName) {
  _entries.clear();
  _displayLines.clear();

  auto statEntry = [](std::filesystem::path const& p, bool isDir, bool isSymlink) -> Entry {
    Entry e;
    e.name = p.filename().string();
    e.path = p;
    e.isDir = isDir;
    e.isSymlink = isSymlink;

    struct stat st{};
    int rc = isSymlink ? lstat(p.c_str(), &st) : stat(p.c_str(), &st);
    if (rc == 0) {
      e.permissions = formatPermissions(st.st_mode, isDir, isSymlink);
      e.size = isDir ? 0 : static_cast<std::uintmax_t>(st.st_size);
      if (auto* pw = getpwuid(st.st_uid))
        e.owner = dir_buffer_detail::sanitizeNssField(pw->pw_name);
      else
        e.owner = std::to_string(st.st_uid);
      if (auto* gr = getgrgid(st.st_gid))
        e.group = dir_buffer_detail::sanitizeNssField(gr->gr_name);
      else
        e.group = std::to_string(st.st_gid);
      e.mtime = formatMtime(st.st_mtim);
    } else {
      e.permissions = "??????????";
      e.owner = "?";
      e.group = "?";
      e.mtime = "?";
      e.size = 0;
    }
    return e;
  };

  // . and .. always first.
  _entries.push_back(statEntry(_dir, true, false));
  _entries.back().name = ".";
  _entries.push_back(statEntry(_dir.parent_path(), true, false));
  _entries.back().name = "..";

  // Read directory.
  std::vector<Entry> dirs;
  std::vector<Entry> files;
  std::error_code ec;
  for (auto const& de : std::filesystem::directory_iterator(_dir, ec)) {
    std::error_code ec2;
    auto name = de.path().filename().string();
    if (!_showHidden && !name.empty() && name[0] == '.')
      continue;
    bool symlink = de.is_symlink(ec2);
    bool dir = de.is_directory(ec2);
    auto e = statEntry(de.path(), dir, symlink);
    if (dir)
      dirs.push_back(std::move(e));
    else
      files.push_back(std::move(e));
  }

  std::sort(dirs.begin(), dirs.end(),
            [](Entry const& a, Entry const& b) { return a.name < b.name; });
  std::sort(files.begin(), files.end(),
            [](Entry const& a, Entry const& b) { return a.name < b.name; });

  for (auto& e : dirs)
    _entries.push_back(std::move(e));
  for (auto& e : files)
    _entries.push_back(std::move(e));

  // Compute column widths for alignment.
  std::size_t maxOwner = 0;
  std::size_t maxGroup = 0;
  std::size_t maxSize = 0;
  for (auto& e : _entries) {
    // Owner/group can be multi-byte UTF-8 (e.g. an NSS name), so align by
    // visual width rather than byte length. They carry no ANSI styling
    // (sanitizeNssField strips control bytes), so visualWidth is exact.
    maxOwner = std::max(maxOwner, visualWidth(e.owner));
    maxGroup = std::max(maxGroup, visualWidth(e.group));
    maxSize = std::max(maxSize, e.isDir ? std::size_t{1} : formatSize(e.size).size());
  }

  // Build display lines.
  // Format: "    permissions  owner  group  size  mtime  name" (4-space prefix for DEL marker).
  for (auto& e : _entries) {
    std::string line;
    // 4 (prefix) + permissions(~10) + 2 + owner + 2 + group + 2 + size + 2
    // + mtime(~19) + 2 + name + optional '/'. Reserve a sane minimum so
    // the per-segment += never needs to grow the small-string allocation.
    // Escape control bytes so filenames with TAB/newline/DEL/etc. can't
    // silently corrupt column alignment or vanish from the listing. The
    // raw `e.name` is kept for selection / cursor restore — only the
    // display string is escaped.
    auto displayName = escapeNonPrintable(e.name);
    line.reserve(80 + displayName.size());
    // 4-space prefix (overwritten with "DEL " in render for marked items).
    line += "    ";
    line += e.permissions;
    line += "  ";
    line += e.owner;
    line.append(maxOwner - visualWidth(e.owner), ' ');
    line += "  ";
    line += e.group;
    line.append(maxGroup - visualWidth(e.group), ' ');
    line += "  ";
    auto sz = e.isDir ? "-" : formatSize(e.size);
    line.append(maxSize - sz.size(), ' ');
    line += sz;
    line += "  ";
    line += e.mtime;
    line += "  ";
    line += displayName;
    if (e.isDir && e.name != "." && e.name != "..")
      line += '/';
    _displayLines.push_back(std::move(line));
  }

  // Populate _lines for base-class cursor navigation.
  _lines.clear();
  _lines.reserve(_displayLines.size());
  for (auto& dl : _displayLines)
    _lines.push_back(makeLine(dl));
  if (_lines.empty())
    _lines.push_back(makeLine());

  // Restore cursor position.
  std::size_t newLine = 0;
  if (!prevName.empty()) {
    for (std::size_t i = 0; i < _entries.size(); ++i) {
      if (_entries[i].name == prevName) {
        newLine = i;
        break;
      }
    }
  }
  if (newLine >= _lines.size())
    newLine = _lines.size() > 0 ? _lines.size() - 1 : 0;
  return ViewHint{{newLine, 0}, {}};
}

std::tuple<RowIdx, ColIdx> DirBuffer::render(std::string& out, RowIdx rows, ColIdx cols,
                                             RenderParams const& params) const {
  constexpr auto kRedFg = "\x1b[31m";

  std::size_t curLine = params.point.line;
  std::size_t scrollLine = params.viewFrom.line.value;
  std::size_t rowOffset = params.rowOffset;
  std::size_t colOffset = params.colOffset;

  for (std::size_t r = 0; r < rows.value; ++r) {
    std::size_t lineIdx = scrollLine + r;
    ansi::appendCursorTo(out, rowOffset + r, colOffset);

    if (lineIdx < _displayLines.size()) {
      bool highlight = (lineIdx == curLine);
      bool marked = _markedForDelete.contains(lineIdx);

      if (highlight)
        out += ansi::ReverseVideo;
      if (marked)
        out += kRedFg;

      // Build the row text (marked rows show a "DEL " prefix in place of
      // the 4-space gutter) and clamp it to the panel width, reserving
      // the final column for the gutter indicator, so a long filename or
      // deep listing can't bleed past the panel into a sibling pane.
      std::string_view full = _displayLines[lineIdx];
      std::string content = marked ? "DEL " + std::string(full.substr(4)) : std::string(full);
      std::size_t budget = cols.value > 1 ? cols.value - 1 : 0;
      std::string clipped = truncateDisplayToWidth(content, budget);
      out += clipped;
      // A clamp may have cut between an escaped byte's colour-on and
      // colour-off; reset so styling never leaks into the cleared
      // remainder or the gutter cell.
      if (clipped.size() < content.size())
        out += ansi::Reset;

      if (marked || highlight)
        out += ansi::Reset;
      out += ansi::EraseLine;
    } else {
      out += ansi::EraseLine;
    }

    // Gutter column.
    ansi::appendCursorTo(out, rowOffset + r, colOffset + cols.value - 1);
    out += ansi::ReverseVideo;
    out += ' ';
    out += ansi::Reset;
  }

  std::size_t curScreenRow = curLine >= scrollLine ? curLine - scrollLine : 0;
  return {RowIdx{curScreenRow}, ColIdx{0}};
}
