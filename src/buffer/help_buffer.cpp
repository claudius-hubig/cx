#include "help_buffer.hpp"
#include "shortcut.hpp"

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
  while (line.size() < 14)
    line += ' ';
  line += desc;
  return line;
}

static std::vector<std::string> const& commandLines() {
  static std::vector<std::string> lines = [] {
    std::vector<std::string> v;
    v.emplace_back("Commands:");
    for (auto a : {Action::Help, Action::Exit, Action::SwitchBuffer, Action::BufferList,
                   Action::Save, Action::KillBuffer, Action::Revert, Action::Find,
                   Action::ToggleWhitespace, Action::ToggleWrap, Action::ToggleMark, Action::Cut,
                   Action::Copy, Action::Paste, Action::PasteCycle, Action::PasteRing})
      v.push_back(formatHelpLine(a));
    return v;
  }();
  return lines;
}

static std::vector<std::string> const& movementLines() {
  static std::vector<std::string> lines = [] {
    std::vector<std::string> v;
    v.emplace_back("Movement:");
    v.emplace_back("  \xe2\x86\x90/\xe2\x86\x91/\xe2\x86\x92/\xe2\x86\x93     Move cursor");
    v.emplace_back("  Ctrl-\xe2\x86\x90/\xe2\x86\x92    Move by word");
    v.emplace_back("  Ctrl-\xe2\x86\x91/\xe2\x86\x93    Move by paragraph");
    v.push_back(formatHelpLine(Action::GoToLine));
    v.emplace_back("  PgUp        Page up (half screen)");
    v.emplace_back("  PgDn        Page down (half screen)");
    v.emplace_back("  Home        Start of line");
    v.emplace_back("  End         End of line");
    v.emplace_back("  Bksp        Delete before cursor");
    v.push_back(formatHelpLine(Action::DeleteChunkBefore));
    v.emplace_back("  Delete      Delete at cursor");
    v.emplace_back("  Ctrl-Del    Delete chunk at cursor");
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
    w = std::max(w, l.size());
  return w;
}

static bool isSideBySide(std::size_t cols) {
  return maxWidth(commandLines()) + kGap + maxWidth(movementLines()) <= cols;
}

// Map a data-line index to an absolute visual row.
static std::size_t dataLineToVisualRow(std::size_t dataLine, std::size_t cols) {
  auto cmdCount = commandLines().size();
  auto movCount = movementLines().size();
  std::size_t cmdEnd = kHeaderLines + cmdCount;
  std::size_t movStart = cmdEnd + 1;
  std::size_t movEnd = movStart + movCount;

  if (!isSideBySide(cols))
    return dataLine; // stacked: 1:1

  if (dataLine < movStart)
    return dataLine;
  if (dataLine < movEnd)
    return kHeaderLines + (dataLine - movStart);
  std::size_t mergedRows = std::max(cmdCount, movCount);
  return kHeaderLines + mergedRows + (dataLine - movEnd);
}

// Build all visual lines for a given column width.
struct VisualLine {
  std::string content;
};

static std::vector<VisualLine> buildVisualLines(std::size_t cols) {
  auto const& cmdLines = commandLines();
  auto const& movLines = movementLines();

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

  std::size_t leftW = maxWidth(cmdLines);
  std::size_t rightW = maxWidth(movLines);
  std::size_t totalW = leftW + kGap + rightW;
  bool sbs = isSideBySide(c);
  std::size_t cmdCount = cmdLines.size();
  std::size_t movCount = movLines.size();

  std::size_t leftPad = 0;
  if (sbs)
    leftPad = (c > totalW) ? (c - totalW) / 2 : 0;
  else {
    std::size_t blockW = std::max(leftW, rightW);
    leftPad = (c > blockW) ? (c - blockW) / 2 : 0;
  }

  if (sbs) {
    std::size_t blockRows = std::max(cmdCount, movCount);
    for (std::size_t i = 0; i < blockRows; ++i) {
      std::string line(leftPad, ' ');
      if (i < cmdCount) {
        line += cmdLines[i];
        std::size_t cur = line.size() - leftPad;
        if (cur < leftW)
          line.append(leftW - cur, ' ');
      } else {
        line.append(leftW, ' ');
      }
      line.append(kGap, ' ');
      if (i < movCount)
        line += movLines[i];
      vlines.push_back({line});
    }
  } else {
    for (std::size_t i = 0; i < cmdCount; ++i) {
      std::string line(leftPad, ' ');
      line += cmdLines[i];
      vlines.push_back({line});
    }
    vlines.push_back({""}); // blank between blocks
    for (std::size_t i = 0; i < movCount; ++i) {
      std::string line(leftPad, ' ');
      line += movLines[i];
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

HelpBuffer::HelpBuffer() : Buffer("*help*") {
  _lines.clear();
  auto addLine = [&](std::string_view sv) { _lines.emplace_back(sv.begin(), sv.end()); };
  addLine(kTitle);
  addLine({});
  addLine({});
  for (auto& l : commandLines())
    addLine(l);
  addLine({});
  for (auto& l : movementLines())
    addLine(l);
  addLine({});
  addLine(kFooterText);
}

std::size_t HelpBuffer::screenRow(ColIdx cols, bool /*wrap*/) const {
  std::size_t pointVis = dataLineToVisualRow(_point.line, cols.value);
  std::size_t scrollVis = dataLineToVisualRow(_viewFrom.line.value, cols.value);
  return pointVis >= scrollVis ? pointVis - scrollVis : 0;
}

std::tuple<RowIdx, ColIdx> HelpBuffer::render(std::string& out, RowIdx rows, ColIdx cols,
                                              bool /*showWhitespace*/, bool /*wrap*/) const {

  // Content width excludes the gutter column.
  std::size_t c = cols.value > 1 ? cols.value - 1 : 1;

  static constexpr auto kGutter = "\x1b[7m";
  static constexpr auto kReset = "\x1b[0m";

  // Cursor data-line and column within that line.
  std::size_t dataLine = _point.line;
  std::size_t dataCol = _point.offset;

  // Layout indices computed from generated vectors.
  std::size_t cmdEnd = kHeaderLines + commandLines().size();
  std::size_t movStart = cmdEnd + 1;
  std::size_t movEnd = movStart + movementLines().size();
  std::size_t footer = movEnd + 1;

  // Scroll offset: visual row corresponding to _viewFrom.line.value.
  std::size_t scrollVis = dataLineToVisualRow(_viewFrom.line.value, c);

  // Cursor visual row, relative to scroll.
  std::size_t curVis = dataLineToVisualRow(dataLine, c);
  std::size_t curScreenRow = curVis >= scrollVis ? curVis - scrollVis : 0;

  // Cursor column with centering.
  std::size_t leftW = maxWidth(commandLines());
  std::size_t rightW = maxWidth(movementLines());
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
  } else if (dataLine >= kHeaderLines && dataLine < cmdEnd) {
    curCol = leftPad + dataCol;
  } else if (dataLine >= movStart && dataLine < movEnd) {
    if (sbs)
      curCol = leftPad + leftW + kGap + dataCol;
    else
      curCol = leftPad + dataCol;
  } else if (dataLine == footer) {
    std::size_t fpad = (c > kFooterText.size()) ? (c - kFooterText.size()) / 2 : 0;
    curCol = fpad + dataCol;
  }

  // Build visual lines and emit from scroll offset.
  auto vlines = buildVisualLines(c);

  for (std::size_t r = 0; r < rows.value; ++r) {
    std::size_t vi = scrollVis + r;
    out += "\x1b[";
    out += std::to_string(r + 1);
    out += ";1H";
    if (vi < vlines.size())
      out += vlines[vi].content;
    out += "\x1b[K";
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

  return {RowIdx{curScreenRow}, ColIdx{curCol}};
}
