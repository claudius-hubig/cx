#include "buffer_list_buffer.hpp"
#include "byte_constants.hpp"

#include <map>
#include <string>

namespace {

struct DirNode {
  std::map<std::string, DirNode> subdirs;
  // (filename, full_path) pairs, kept in insertion (sorted) order.
  std::vector<std::pair<std::string, std::string>> files;
  // Non-empty if an open DirBuffer corresponds to this directory.
  std::string dirBufferName;
};

// Split an absolute path on '/', skipping the leading '/'. A trailing
// '/' produces an empty final component (matching the prior behavior
// of insertPath/insertDirBuffer); callers either ignore that or
// inspect parts.empty() before walking the result.
std::vector<std::string> splitPath(std::string const& fullPath) {
  std::vector<std::string> parts;
  std::size_t start = 1;
  while (start < fullPath.size()) {
    auto end = fullPath.find('/', start);
    if (end == std::string::npos)
      end = fullPath.size();
    parts.push_back(fullPath.substr(start, end - start));
    start = end + 1;
  }
  return parts;
}

void insertPath(DirNode& root, std::string const& fullPath) {
  auto parts = splitPath(fullPath);
  if (parts.empty())
    return;

  DirNode* node = &root;
  for (std::size_t i = 0; i + 1 < parts.size(); ++i)
    node = &node->subdirs[parts[i]];
  node->files.push_back({parts.back(), fullPath});
}

// Insert a DirBuffer name into the trie, marking the corresponding
// node so it renders as a selectable directory header. Returns true
// if the path was placed on an existing or newly created node.
bool insertDirBuffer(DirNode& root, std::string const& fullPath) {
  auto parts = splitPath(fullPath);
  if (parts.empty())
    return false;

  DirNode* node = &root;
  for (auto& part : parts)
    node = &node->subdirs[part];
  node->dirBufferName = fullPath;
  return true;
}

// Render a directory node's contents into lines/names.
void renderNode(DirNode const& node, std::string const& prefix, std::size_t indent, bool showHeader,
                std::vector<std::string>& lines, std::vector<std::string>& names) {
  bool hasContent = !node.files.empty() || !node.subdirs.empty();
  bool hasDirBuffer = !node.dirBufferName.empty();

  if (!hasContent && !hasDirBuffer)
    return;

  // Collapse single-child subdirectories, but not if this node has a
  // DirBuffer marker (it needs its own selectable header line).
  if (!hasDirBuffer && node.files.empty() && node.subdirs.size() == 1) {
    auto& [name, child] = *node.subdirs.begin();
    renderNode(child, prefix + name + "/", indent, true, lines, names);
    return;
  }

  // Single file leaf with no subdirs and no DirBuffer.
  if (!hasDirBuffer && node.files.size() == 1 && node.subdirs.empty()) {
    lines.push_back(std::string(indent, ' ') + prefix + node.files[0].first);
    names.push_back(node.files[0].second);
    return;
  }

  std::string childPrefix;
  if (showHeader) {
    lines.push_back(std::string(indent, ' ') + prefix);
    // If a DirBuffer is open for this directory, make the header selectable.
    names.push_back(hasDirBuffer ? node.dirBufferName : std::string{});
    indent += 2;
  } else {
    childPrefix = prefix;
  }

  auto fi = node.files.begin();
  auto di = node.subdirs.begin();
  while (fi != node.files.end() || di != node.subdirs.end()) {
    bool useFile = fi != node.files.end() && (di == node.subdirs.end() || fi->first < di->first);
    if (useFile) {
      lines.push_back(std::string(indent, ' ') + childPrefix + fi->first);
      names.push_back(fi->second);
      ++fi;
    } else {
      renderNode(di->second, childPrefix + di->first + "/", indent, true, lines, names);
      ++di;
    }
  }
}

} // namespace

BufferListBuffer::BufferListBuffer(std::vector<std::string> names,
                                   std::vector<std::string> dirNames, std::string const& startName)
    : Buffer("*buffers*") {
  auto hint = refresh(std::move(names), std::move(dirNames), startName);
  setSavedView({.point = hint.point, .viewFrom = hint.viewFrom});
}

ViewHint BufferListBuffer::refresh(std::vector<std::string> names,
                                   std::vector<std::string> dirNames,
                                   std::string const& startName) {
  _displayLines.clear();
  _lineToName.clear();

  // Separate non-path names from absolute paths.
  DirNode root;
  for (auto& name : names) {
    if (!name.empty() && name[0] == '/') {
      insertPath(root, name);
    } else {
      _displayLines.push_back(name);
      _lineToName.push_back(name);
    }
  }

  // Merge directory buffers into the trie as selectable headers.
  for (auto& dn : dirNames)
    insertDirBuffer(root, dn);

  // Render path trie.
  renderNode(root, "/", 0, false, _displayLines, _lineToName);

  // Build _lines from display lines (for base-class cursor navigation).
  _lines.clear();
  _lines.reserve(_displayLines.size());
  for (auto& dl : _displayLines)
    _lines.push_back(makeLine(dl));
  if (_lines.empty())
    _lines.push_back(makeLine()); // always at least one line

  // Place point on the line matching startName.
  std::size_t startLine = 0;
  if (!startName.empty()) {
    for (std::size_t i = 0; i < _lineToName.size(); ++i) {
      if (_lineToName[i] == startName) {
        startLine = i;
        break;
      }
    }
  }
  return ViewHint{{startLine, 0}, {}};
}

std::string BufferListBuffer::selectedName(BufferPoint p) const {
  if (p.line < _lineToName.size())
    return _lineToName[p.line];
  return {};
}

std::tuple<RowIdx, ColIdx> BufferListBuffer::render(std::string& out, RowIdx rows, ColIdx cols,
                                                    RenderParams const& params) const {

  std::size_t curLine = params.point.line;
  std::size_t scrollLine = params.viewFrom.line.value;

  for (std::size_t r = 0; r < rows.value; ++r) {
    std::size_t lineIdx = scrollLine + r;
    ansi::appendCursorTo(out, params.rowOffset + r, params.colOffset);

    if (lineIdx < _displayLines.size()) {
      bool highlight = (lineIdx == curLine);
      if (highlight)
        out += ansi::ReverseVideo;
      out += _displayLines[lineIdx];
      if (highlight)
        out += ansi::Reset;
      out += ansi::EraseLine;
    } else {
      out += ansi::EraseLine;
    }
    // Gutter column.
    ansi::appendCursorTo(out, params.rowOffset + r, params.colOffset + cols.value - 1);
    out += ansi::ReverseVideo;
    out += ' ';
    out += ansi::Reset;
  }

  std::size_t curScreenRow = curLine >= scrollLine ? curLine - scrollLine : 0;
  return {RowIdx{curScreenRow}, ColIdx{0}};
}
