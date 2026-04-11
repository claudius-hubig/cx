#include "help_buffer.hpp"
#include "byte_constants.hpp"
#include "shortcut.hpp"
#include "utf8.hpp"

#include <algorithm>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Generate a help line: "  <shortcut>  <description>" with consistent padding.
static std::string formatHelpLine(Action a) {
  auto s = shortcutForAction(a);
  auto label = formatShortcut(s);
  auto desc = descriptionForAction(a);
  std::string line = "  ";
  line += label;
  while (visualWidth(line) < 16)
    line += ' ';
  line += desc;
  return line;
}

// A hardcoded line with the same padding convention.
static std::string formatRawLine(std::string_view key, std::string_view desc) {
  std::string line = "  ";
  line += key;
  while (visualWidth(line) < 16)
    line += ' ';
  line += desc;
  return line;
}

static std::vector<std::string> const& leftColumn() {
  static std::vector<std::string> lines = [] {
    std::vector<std::string> v;
    v.emplace_back("Files & Buffers:");
    for (auto a : {Action::Help, Action::Exit, Action::SwitchBuffer, Action::BufferList,
                   Action::DirBrowser, Action::Save, Action::KillBuffer, Action::Revert})
      v.push_back(formatHelpLine(a));
    v.emplace_back("");
    v.emplace_back("Search & Edit:");
    v.push_back(formatHelpLine(Action::ToggleMark));
    for (auto a : {Action::Cut, Action::Copy, Action::Paste, Action::PasteCycle, Action::PasteRing})
      v.push_back(formatHelpLine(a));
    v.push_back(formatHelpLine(Action::Format));
    v.push_back(formatHelpLine(Action::PipeReplace));
    v.emplace_back("");
    v.emplace_back("C mode (Esc-m to set):");
    v.push_back(formatRawLine("Tab", "Format line"));
    v.push_back(formatRawLine("?", "Jump to matching bracket"));
    v.emplace_back("");
    v.emplace_back("JSON mode (Esc-m to set):");
    v.push_back(formatRawLine("?", "Jump to matching bracket"));
    v.emplace_back("");
    v.emplace_back("Directory browser:");
    v.push_back(formatRawLine("Enter", "Open file / enter dir"));
    v.push_back(formatRawLine("h", "Toggle hidden files"));
    v.push_back(formatRawLine("d", "Mark / unmark for deletion"));
    v.push_back(formatRawLine("x", "Delete marked items"));
    return v;
  }();
  return lines;
}

static std::vector<std::string> const& rightColumn() {
  static std::vector<std::string> lines = [] {
    std::vector<std::string> v;
    v.emplace_back("Movement:");
    v.push_back(formatHelpLine(Action::Find));
    v.push_back(formatHelpLine(Action::FindBackward));
    v.push_back(
        formatRawLine("\xe2\x86\x90/\xe2\x86\x91/\xe2\x86\x92/\xe2\x86\x93", "Move cursor"));
    v.push_back(formatRawLine("Ctrl-\xe2\x86\x90/\xe2\x86\x92", "Move by word"));
    v.push_back(formatRawLine("Ctrl-\xe2\x86\x91/\xe2\x86\x93", "Move by paragraph"));
    v.push_back(formatHelpLine(Action::GoToLine));
    v.push_back(formatRawLine("PgUp/PgDn", "Page up/down (half screen)"));
    v.push_back(formatRawLine("Home/End", "Start/end of line"));
    v.push_back(formatRawLine("Ctrl-Home/End", "Start/end of file"));
    v.emplace_back("");
    v.emplace_back("Display:");
    v.push_back(formatHelpLine(Action::ToggleWhitespace));
    v.push_back(formatHelpLine(Action::ToggleWrap));
    v.push_back(formatHelpLine(Action::SetMode));
    v.emplace_back("");
    v.emplace_back("Deletion:");
    v.push_back(formatRawLine("Bksp", "Delete before cursor"));
    v.push_back(formatHelpLine(Action::DeleteChunkBefore));
    v.push_back(formatRawLine("Delete", "Delete at cursor"));
    v.push_back(formatRawLine("Ctrl-Del", "Delete chunk at cursor"));
    v.emplace_back("");
    v.emplace_back("Panels:");
    v.push_back(formatHelpLine(Action::SplitVertical));
    v.push_back(formatHelpLine(Action::SplitHorizontal));
    v.push_back(formatHelpLine(Action::ClosePanel));
    v.push_back(formatRawLine("Esc-1..6", "Focus panel 1..6"));
    return v;
  }();
  return lines;
}

static constexpr std::string_view kFooterText =
    "Press 'q', Esc-Esc, or Esc-? to return to editing.";
static constexpr std::string_view kTitle = "cx";
static constexpr std::size_t kGap = 6;
static constexpr std::size_t kHeaderLines = 3;

static std::size_t maxWidth(std::span<std::string const> lines) {
  std::size_t w = 0;
  for (auto& l : lines)
    w = std::max(w, visualWidth(l));
  return w;
}

// Cached maximum visual widths of the left/right columns. The column
// content is static (computed once via lambdas in leftColumn/rightColumn),
// so the widths never change — compute each once and reuse.
static std::size_t cachedLeftWidth() {
  static std::size_t const w = maxWidth(leftColumn());
  return w;
}

static std::size_t cachedRightWidth() {
  static std::size_t const w = maxWidth(rightColumn());
  return w;
}

static bool isSideBySide(std::size_t cols) {
  return cachedLeftWidth() + kGap + cachedRightWidth() <= cols;
}

// Map a data-line index to an absolute visual row.
static std::size_t dataLineToVisualRow(std::size_t dataLine, std::size_t cols) {
  auto leftCount = leftColumn().size();
  auto rightCount = rightColumn().size();
  std::size_t leftEnd = kHeaderLines + leftCount;
  std::size_t rightStart = leftEnd + 1;
  std::size_t rightEnd = rightStart + rightCount;

  if (!isSideBySide(cols))
    return dataLine; // stacked: 1:1

  if (dataLine < rightStart)
    return dataLine;
  if (dataLine < rightEnd)
    return kHeaderLines + (dataLine - rightStart);
  std::size_t mergedRows = std::max(leftCount, rightCount);
  return kHeaderLines + mergedRows + (dataLine - rightEnd);
}

// Build all visual lines for a given column width.
struct VisualLine {
  std::string content;
};

static std::vector<VisualLine> buildVisualLines(std::size_t cols) {
  auto const& leftLines = leftColumn();
  auto const& rightLines = rightColumn();

  std::vector<VisualLine> vlines;
  std::size_t c = cols;

  // Row 0: centered title.
  {
    std::size_t pad = (c > kTitle.size()) ? (c - kTitle.size()) / 2 : 0;
    std::string s(pad, ' ');
    s += kTitle;
    vlines.push_back({s});
  }

  // Row 1: separator.
  vlines.push_back({std::string(c, '=')});

  // Row 2: blank.
  vlines.push_back({""});

  std::size_t leftW = cachedLeftWidth();
  std::size_t rightW = cachedRightWidth();
  std::size_t totalW = leftW + kGap + rightW;
  bool sbs = isSideBySide(c);
  std::size_t leftCount = leftLines.size();
  std::size_t rightCount = rightLines.size();

  std::size_t leftPad = 0;
  if (sbs)
    leftPad = (c > totalW) ? (c - totalW) / 2 : 0;
  else {
    std::size_t blockW = std::max(leftW, rightW);
    leftPad = (c > blockW) ? (c - blockW) / 2 : 0;
  }

  if (sbs) {
    std::size_t blockRows = std::max(leftCount, rightCount);
    for (std::size_t i = 0; i < blockRows; ++i) {
      std::string line(leftPad, ' ');
      if (i < leftCount) {
        line += leftLines[i];
        std::size_t cur = visualWidth(leftLines[i]);
        if (cur < leftW)
          line.append(leftW - cur, ' ');
      } else {
        line.append(leftW, ' ');
      }
      line.append(kGap, ' ');
      if (i < rightCount)
        line += rightLines[i];
      vlines.push_back({line});
    }
  } else {
    for (std::size_t i = 0; i < leftCount; ++i) {
      std::string line(leftPad, ' ');
      line += leftLines[i];
      vlines.push_back({line});
    }
    vlines.push_back({""}); // blank between blocks
    for (std::size_t i = 0; i < rightCount; ++i) {
      std::string line(leftPad, ' ');
      line += rightLines[i];
      vlines.push_back({line});
    }
  }

  // Blank before footer.
  vlines.push_back({""});

  // Footer centered.
  {
    std::size_t fpad = (c > kFooterText.size()) ? (c - kFooterText.size()) / 2 : 0;
    std::string s(fpad, ' ');
    s += kFooterText;
    vlines.push_back({s});
  }

  return vlines;
}

// Return a cached vector of visual lines for the given content width.
// The help content is static, so we only need to rebuild when the
// column count changes (e.g. on terminal resize). Single-threaded
// editor, so a simple function-level static is safe.
static std::vector<VisualLine> const& cachedVisualLines(std::size_t cols) {
  static std::vector<VisualLine> cache;
  static std::size_t cachedCols = 0;
  static bool cacheValid = false;
  if (!cacheValid || cachedCols != cols) {
    cache = buildVisualLines(cols);
    cachedCols = cols;
    cacheValid = true;
  }
  return cache;
}

HelpBuffer::HelpBuffer() : Buffer("*help*") {
  _lines.clear();
  auto addLine = [&](std::string_view sv) { _lines.emplace_back(sv.begin(), sv.end()); };
  addLine(kTitle);
  addLine({});
  addLine({});
  for (auto& l : leftColumn())
    addLine(l);
  addLine({});
  for (auto& l : rightColumn())
    addLine(l);
  addLine({});
  addLine(kFooterText);
}

std::size_t HelpBuffer::screenRow(ColIdx cols, BufferPoint point, ViewFrom viewFrom,
                                  bool /*wrap*/) const {
  std::size_t pointVis = dataLineToVisualRow(point.line, cols.value);
  std::size_t scrollVis = dataLineToVisualRow(viewFrom.line.value, cols.value);
  return pointVis >= scrollVis ? pointVis - scrollVis : 0;
}

std::tuple<RowIdx, ColIdx> HelpBuffer::render(std::string& out, RowIdx rows, ColIdx cols,
                                              RenderParams const& params) const {

  // Content width excludes the gutter column.
  std::size_t c = contentWidth(cols);

  // Cursor data-line and column within that line.
  std::size_t dataLine = params.point.line;
  std::size_t dataCol = params.point.offset;

  // Layout indices computed from generated vectors.
  std::size_t leftEnd = kHeaderLines + leftColumn().size();
  std::size_t rightStart = leftEnd + 1;
  std::size_t rightEnd = rightStart + rightColumn().size();
  std::size_t footer = rightEnd + 1;

  // Scroll offset: visual row corresponding to viewFrom.
  std::size_t scrollVis = dataLineToVisualRow(params.viewFrom.line.value, c);

  // Cursor visual row, relative to scroll.
  std::size_t curVis = dataLineToVisualRow(dataLine, c);
  std::size_t curScreenRow = curVis >= scrollVis ? curVis - scrollVis : 0;

  // Cursor column with centering.
  std::size_t leftW = cachedLeftWidth();
  std::size_t rightW = cachedRightWidth();
  std::size_t totalW = leftW + kGap + rightW;
  bool sbs = isSideBySide(c);

  std::size_t leftPad = 0;
  if (sbs)
    leftPad = (c > totalW) ? (c - totalW) / 2 : 0;
  else {
    std::size_t blockW = std::max(leftW, rightW);
    leftPad = (c > blockW) ? (c - blockW) / 2 : 0;
  }

  std::size_t curCol = dataCol; // default for header/footer (no extra pad)
  if (dataLine == 0) {
    // Title: centered.
    std::size_t tpad = (c > kTitle.size()) ? (c - kTitle.size()) / 2 : 0;
    curCol = tpad + dataCol;
  } else if (dataLine >= kHeaderLines && dataLine < leftEnd) {
    curCol = leftPad + dataCol;
  } else if (dataLine >= rightStart && dataLine < rightEnd) {
    if (sbs)
      curCol = leftPad + leftW + kGap + dataCol;
    else
      curCol = leftPad + dataCol;
  } else if (dataLine == footer) {
    std::size_t fpad = (c > kFooterText.size()) ? (c - kFooterText.size()) / 2 : 0;
    curCol = fpad + dataCol;
  }

  // Build visual lines and emit from scroll offset (cached — rebuilt
  // only when the column count changes).
  auto const& vlines = cachedVisualLines(c);

  // Section headers that should be rendered dimmed.
  auto isSectionHeader = [](std::string_view line) {
    return !line.empty() && line.back() == ':' && line.front() != ' ';
  };

  for (std::size_t r = 0; r < rows.value; ++r) {
    std::size_t vi = scrollVis + r;
    out += ansi::cursorTo(params.rowOffset + r, params.colOffset);
    if (vi < vlines.size()) {
      auto& content = vlines[vi].content;
      // Check if any section header appears in this visual line.
      // In side-by-side mode, a line can contain headers from both columns.
      bool hasDim = false;
      if (sbs && vi >= kHeaderLines) {
        std::size_t idx = vi - kHeaderLines;
        bool leftIsHeader = idx < leftColumn().size() && isSectionHeader(leftColumn()[idx]);
        bool rightIsHeader = idx < rightColumn().size() && isSectionHeader(rightColumn()[idx]);
        if (leftIsHeader || rightIsHeader) {
          // Emit with dim applied to header portions.
          // The line is: leftPad + leftContent + gap + rightContent.
          // We'll emit it in parts.
          std::size_t pos = 0;
          // Left pad.
          if (leftPad > 0 && leftPad <= content.size()) {
            out.append(content, 0, leftPad);
            pos = leftPad;
          }
          // Left content.
          std::size_t leftContentEnd = leftPad + leftW;
          if (leftContentEnd > content.size())
            leftContentEnd = content.size();
          if (leftIsHeader && pos < leftContentEnd) {
            out += ansi::Dim;
            out.append(content, pos, leftContentEnd - pos);
            out += ansi::Reset;
          } else if (pos < leftContentEnd) {
            out.append(content, pos, leftContentEnd - pos);
          }
          pos = leftContentEnd;
          // Gap.
          std::size_t gapEnd = leftPad + leftW + kGap;
          if (gapEnd > content.size())
            gapEnd = content.size();
          if (pos < gapEnd) {
            out.append(content, pos, gapEnd - pos);
            pos = gapEnd;
          }
          // Right content.
          if (rightIsHeader && pos < content.size()) {
            out += ansi::Dim;
            out.append(content, pos, content.size() - pos);
            out += ansi::Reset;
          } else if (pos < content.size()) {
            out.append(content, pos, content.size() - pos);
          }
          hasDim = true;
        }
      } else if (!sbs && vi >= kHeaderLines) {
        // Stacked mode: check if this line is a section header.
        // The content is leftPad + section text.
        std::size_t dataIdx = vi - kHeaderLines; // approximate
        // For stacked, the visual line maps 1:1 to data lines.
        // Trim leading pad and check.
        auto trimmed = content;
        auto firstNonSpace = trimmed.find_first_not_of(' ');
        if (firstNonSpace != std::string::npos) {
          auto inner = std::string_view(trimmed).substr(firstNonSpace);
          if (isSectionHeader(inner)) {
            out += ansi::Dim;
            out += content;
            out += ansi::Reset;
            hasDim = true;
          }
        }
      }
      if (!hasDim)
        out += content;
    }
    out += ansi::EraseLine;
    // Gutter column.
    out += ansi::cursorTo(params.rowOffset + r, params.colOffset + cols.value - 1);
    out += ansi::ReverseVideo;
    out += ' ';
    out += ansi::Reset;
  }

  return {RowIdx{curScreenRow}, ColIdx{curCol}};
}
