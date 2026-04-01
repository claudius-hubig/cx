#include "dir_buffer.hpp"
#include "byte_constants.hpp"

#include <algorithm>
#include <ctime>
#include <grp.h>
#include <pwd.h>
#include <sys/stat.h>

namespace {

std::string formatSize(std::uintmax_t bytes) {
  if (bytes < 1024)
    return std::to_string(bytes);
  if (bytes < 1024 * 1024)
    return std::to_string(bytes / 1024) + "K";
  if (bytes < 1024 * 1024 * 1024)
    return std::to_string(bytes / (1024 * 1024)) + "M";
  return std::to_string(bytes / (1024 * 1024 * 1024)) + "G";
}

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

std::string formatMtime(struct timespec const& ts) {
  std::tm tm{};
  localtime_r(&ts.tv_sec, &tm);
  char buf[20]; // "YYYY-MM-DD HH:MM:SS"
  std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
  return buf;
}

} // namespace

DirBuffer::DirBuffer(std::filesystem::path dir)
    : Buffer(std::filesystem::weakly_canonical(std::filesystem::absolute(dir)).string()),
      _dir(std::filesystem::weakly_canonical(std::filesystem::absolute(std::move(dir)))) {
  refresh();
}

void DirBuffer::navigate(std::filesystem::path dir) {
  _dir = std::filesystem::weakly_canonical(std::filesystem::absolute(std::move(dir)));
  _markedForDelete.clear();
  refresh();
}

void DirBuffer::toggleHidden() {
  std::string prevName;
  if (_point.line < _entries.size())
    prevName = _entries[_point.line].name;

  _showHidden = !_showHidden;
  _markedForDelete.clear();
  refresh();

  // Try to restore cursor on the same entry.
  if (!prevName.empty()) {
    for (std::size_t i = 0; i < _entries.size(); ++i) {
      if (_entries[i].name == prevName) {
        _point = {i, 0};
        break;
      }
    }
  }
}

void DirBuffer::toggleDeleteMark() {
  // Refuse to mark . and .. (always at indices 0 and 1).
  if (_point.line < 2 || _point.line >= _entries.size())
    return;
  if (_markedForDelete.count(_point.line))
    _markedForDelete.erase(_point.line);
  else
    _markedForDelete.insert(_point.line);
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

std::pair<std::size_t, std::string> DirBuffer::executeDelete() {
  std::size_t count = 0;
  std::string errors;
  for (auto idx : _markedForDelete) {
    if (idx >= _entries.size())
      continue;
    std::error_code ec;
    if (_entries[idx].isDir)
      std::filesystem::remove_all(_entries[idx].path, ec);
    else
      std::filesystem::remove(_entries[idx].path, ec);
    if (ec) {
      if (!errors.empty())
        errors += "; ";
      errors += _entries[idx].name + ": " + ec.message();
    } else {
      ++count;
    }
  }
  _markedForDelete.clear();
  refresh();
  return {count, errors};
}

std::filesystem::path DirBuffer::selectedPath() const {
  if (_point.line < _entries.size())
    return _entries[_point.line].path;
  return _dir;
}

bool DirBuffer::selectedIsDir() const {
  if (_point.line < _entries.size())
    return _entries[_point.line].isDir;
  return false;
}

void DirBuffer::refresh() {
  // Remember current entry name for cursor restoration.
  std::string prevName;
  if (_point.line < _entries.size())
    prevName = _entries[_point.line].name;

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
        e.owner = pw->pw_name;
      else
        e.owner = std::to_string(st.st_uid);
      if (auto* gr = getgrgid(st.st_gid))
        e.group = gr->gr_name;
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
    maxOwner = std::max(maxOwner, e.owner.size());
    maxGroup = std::max(maxGroup, e.group.size());
    maxSize = std::max(maxSize, e.isDir ? std::size_t{1} : formatSize(e.size).size());
  }

  // Build display lines.
  // Format: "    permissions  owner  group  size  mtime  name" (4-space prefix for DEL marker).
  for (auto& e : _entries) {
    std::string line;
    // 4-space prefix (overwritten with "DEL " in render for marked items).
    line += "    ";
    line += e.permissions;
    line += "  ";
    line += e.owner;
    line.append(maxOwner - e.owner.size(), ' ');
    line += "  ";
    line += e.group;
    line.append(maxGroup - e.group.size(), ' ');
    line += "  ";
    auto sz = e.isDir ? "-" : formatSize(e.size);
    line.append(maxSize - sz.size(), ' ');
    line += sz;
    line += "  ";
    line += e.mtime;
    line += "  ";
    line += e.name;
    if (e.isDir && e.name != "." && e.name != "..")
      line += '/';
    _displayLines.push_back(std::move(line));
  }

  // Populate _lines for base-class cursor navigation.
  _lines.clear();
  for (auto& dl : _displayLines)
    _lines.emplace_back(dl.begin(), dl.end());
  if (_lines.empty())
    _lines.emplace_back();

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
  _point = {newLine, 0};
  _viewFrom = ViewFrom{};
  _desiredCol.reset();
}

std::tuple<RowIdx, ColIdx> DirBuffer::render(std::string& out, RowIdx rows, ColIdx cols, bool, bool,
                                             std::size_t rowOffset, std::size_t colOffset) const {
  constexpr auto kRedFg = "\x1b[31m";

  std::size_t curLine = _point.line;
  std::size_t scrollLine = _viewFrom.line.value;

  for (std::size_t r = 0; r < rows.value; ++r) {
    std::size_t lineIdx = scrollLine + r;
    out += ansi::cursorTo(rowOffset + r, colOffset);

    if (lineIdx < _displayLines.size()) {
      bool highlight = (lineIdx == curLine);
      bool marked = _markedForDelete.count(lineIdx) > 0;

      if (highlight)
        out += ansi::ReverseVideo;
      if (marked)
        out += kRedFg;

      if (marked) {
        // Replace the 4-space prefix with "DEL ".
        out += "DEL ";
        out += std::string_view(_displayLines[lineIdx]).substr(4);
      } else {
        out += _displayLines[lineIdx];
      }

      if (marked || highlight)
        out += ansi::Reset;
      out += ansi::EraseLine;
    } else {
      out += ansi::EraseLine;
    }

    // Gutter column.
    out += ansi::cursorTo(rowOffset + r, colOffset + cols.value - 1);
    out += ansi::ReverseVideo;
    out += ' ';
    out += ansi::Reset;
  }

  std::size_t curScreenRow = curLine >= scrollLine ? curLine - scrollLine : 0;
  return {RowIdx{curScreenRow}, ColIdx{0}};
}
