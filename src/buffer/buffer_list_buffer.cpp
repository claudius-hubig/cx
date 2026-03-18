#include "buffer_list_buffer.hpp"

#include <map>
#include <string>

namespace {

struct DirNode {
  std::map<std::string, DirNode> subdirs;
  // (filename, full_path) pairs, kept in insertion (sorted) order.
  std::vector<std::pair<std::string, std::string>> files;
};

void insertPath(DirNode& root, std::string const& fullPath) {
  // Split on '/', skipping the leading '/'.
  std::vector<std::string> parts;
  std::size_t start = 1;
  while (start < fullPath.size()) {
    auto end = fullPath.find('/', start);
    if (end == std::string::npos)
      end = fullPath.size();
    parts.push_back(fullPath.substr(start, end - start));
    start = end + 1;
  }
  if (parts.empty())
    return;

  DirNode* node = &root;
  for (std::size_t i = 0; i + 1 < parts.size(); ++i)
    node = &node->subdirs[parts[i]];
  node->files.push_back({parts.back(), fullPath});
}

// Render a directory node's contents into lines/names.
void renderNode(DirNode const& node, std::string const& prefix, std::size_t indent, bool showHeader,
                std::vector<std::string>& lines, std::vector<std::string>& names) {
  std::size_t total = node.files.size() + node.subdirs.size();
  if (total == 0)
    return;

  if (node.files.empty() && node.subdirs.size() == 1) {
    auto& [name, child] = *node.subdirs.begin();
    renderNode(child, prefix + name + "/", indent, true, lines, names);
    return;
  }

  if (node.files.size() == 1 && node.subdirs.empty()) {
    lines.push_back(std::string(indent, ' ') + prefix + node.files[0].first);
    names.push_back(node.files[0].second);
    return;
  }

  std::string childPrefix;
  if (showHeader) {
    lines.push_back(std::string(indent, ' ') + prefix);
    names.push_back({});
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

BufferListBuffer::BufferListBuffer(std::vector<std::string> names, std::string const& startName)
    : Buffer("*buffers*") {
  refresh(std::move(names), startName);
}

void BufferListBuffer::refresh(std::vector<std::string> names, std::string const& startName) {
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

  // Render path trie.
  renderNode(root, "/", 0, false, _displayLines, _lineToName);

  // Build _lines from display lines (for base-class cursor navigation).
  _lines.clear();
  for (auto& dl : _displayLines)
    _lines.emplace_back(dl.begin(), dl.end());
  if (_lines.empty())
    _lines.emplace_back(); // always at least one line

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
  _point = {startLine, 0};
  _viewFromLine = 0;
  _viewOffset = WrapOffset{0};
  _desiredCol.reset();
}

std::string BufferListBuffer::selectedName() const {
  if (_point.line < _lineToName.size())
    return _lineToName[_point.line];
  return {};
}

std::tuple<RowIdx, ColIdx> BufferListBuffer::render(std::string& out, RowIdx rows, ColIdx cols,
                                                    bool /*showWhitespace*/, bool /*wrap*/) const {

  static constexpr auto kHighlight = "\x1b[7m"; // reverse video
  static constexpr auto kGutter = "\x1b[7m";
  static constexpr auto kReset = "\x1b[0m";

  std::size_t curLine = _point.line;
  std::size_t scrollLine = _viewFromLine;

  for (std::size_t r = 0; r < rows.value; ++r) {
    std::size_t lineIdx = scrollLine + r;
    out += "\x1b[";
    out += std::to_string(r + 1);
    out += ";1H";

    if (lineIdx < _displayLines.size()) {
      bool highlight = (lineIdx == curLine);
      if (highlight)
        out += kHighlight;
      out += _displayLines[lineIdx];
      if (highlight)
        out += kReset;
      out += "\x1b[K";
    } else {
      out += "\x1b[K";
    }
    // Gutter column.
    out += "\x1b[";
    out += std::to_string(r + 1);
    out += ";";
    out += std::to_string(cols.value);
    out += "H";
    out += kGutter;
    out += ' ';
    out += kReset;
  }

  std::size_t curScreenRow = curLine >= scrollLine ? curLine - scrollLine : 0;
  return {RowIdx{curScreenRow}, ColIdx{0}};
}
