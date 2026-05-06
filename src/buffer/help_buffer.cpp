#include "help_buffer.hpp"
#include "byte_constants.hpp"
#include "shortcut.hpp"
#include "utf8.hpp"

#include <algorithm>
#include <array>
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
    for (auto a :
         {Action::Help, Action::Exit, Action::SwitchBuffer, Action::BufferList, Action::DirBrowser,
          Action::Save, Action::KillBuffer, Action::Revert, Action::VersionHistory})
      v.push_back(formatHelpLine(a));
    v.emplace_back("");
    v.emplace_back("Regions & Edit:");
    for (auto a : {Action::ToggleMark, Action::Cut, Action::Copy, Action::Paste, Action::PasteCycle,
                   Action::PasteRing, Action::Format, Action::PipeReplace, Action::ToggleComment})
      v.push_back(formatHelpLine(a));
    v.emplace_back("");
    v.emplace_back("Bracket matching:");
    v.push_back(formatRawLine("?", "Jump to matching bracket (all modes)"));
    v.emplace_back("");
    v.emplace_back("C mode (Esc-m to set):");
    v.push_back(formatRawLine("Tab", "Format line"));
    v.emplace_back("");
    v.emplace_back("Directory browser:");
    v.push_back(formatRawLine("Enter", "Open file / enter dir"));
    v.push_back(formatRawLine("h", "Toggle hidden files"));
    v.push_back(formatRawLine("d", "Mark / unmark for deletion"));
    v.push_back(formatRawLine("x", "Delete marked items"));
    v.push_back(formatRawLine("r", "Rename selected entry"));
    v.push_back(formatRawLine("m", "Make subdirectory"));
    v.emplace_back("");
    // History-mode keys are dispatched inline in handleNormalInput rather
    // than through the central shortcut table, so they are listed here
    // as raw entries so users can still discover them via Esc-?.
    v.emplace_back("History mode:");
    v.push_back(formatHelpLine(Action::VersionHistory));
    v.push_back(formatRawLine("[", "Step to older version"));
    v.push_back(formatRawLine("]", "Step to newer version"));
    v.push_back(formatRawLine("Enter", "Commit selected version"));
    v.push_back(formatRawLine("Esc-Esc", "Cancel and return to latest"));
    return v;
  }();
  return lines;
}

static std::vector<std::string> const& rightColumn() {
  static std::vector<std::string> lines = [] {
    std::vector<std::string> v;
    v.emplace_back("Search & Movement:");
    for (auto a : {Action::Find, Action::FindBackward, Action::Replace})
      v.push_back(formatHelpLine(a));
    v.push_back(
        formatRawLine("\xe2\x86\x90/\xe2\x86\x91/\xe2\x86\x92/\xe2\x86\x93", "Move cursor"));
    v.push_back(formatRawLine("Ctrl-\xe2\x86\x90/\xe2\x86\x92", "Move by word"));
    v.push_back(formatRawLine("Ctrl-\xe2\x86\x91/\xe2\x86\x93", "Move by paragraph"));
    for (auto a : {Action::GoToLine, Action::NextBlock, Action::PrevBlock})
      v.push_back(formatHelpLine(a));
    v.push_back(formatRawLine("PgUp/PgDn", "Page up/down (half screen)"));
    v.push_back(formatRawLine("Home/End", "Start/end of line"));
    v.push_back(formatRawLine("Ctrl-Home/End", "Start/end of file"));
    v.emplace_back("");
    v.emplace_back("Display:");
    for (auto a : {Action::ToggleWhitespace, Action::ToggleWrap, Action::SetMode})
      v.push_back(formatHelpLine(a));
    v.emplace_back("");
    v.emplace_back("Deletion:");
    v.push_back(formatRawLine("Bksp", "Delete before cursor"));
    v.push_back(formatHelpLine(Action::DeleteChunkBefore));
    v.push_back(formatRawLine("Delete", "Delete at cursor"));
    v.push_back(formatRawLine("Ctrl-Del", "Delete chunk at cursor"));
    v.push_back(formatHelpLine(Action::DeleteLine));
    v.emplace_back("");
    v.emplace_back("Panels:");
    for (auto a : {Action::SplitVertical, Action::SplitHorizontal, Action::ClosePanel,
                   Action::FocusMRUPanel})
      v.push_back(formatHelpLine(a));
    v.push_back(formatRawLine("C-x 1..6", "Focus panel 1..6"));
    return v;
  }();
  return lines;
}

static constexpr auto kFooterLines = std::to_array<std::string_view>({
    "Press 'q', Esc-Esc, or Esc-? to return to editing.",
    "",
    "cx text editor",
    "Copyright (C) 2026 Claudius",
    "",
    "This program is free software: you can redistribute it and/or modify it",
    "under the terms of the GNU General Public License as published by the",
    "Free Software Foundation, either version 3 of the License, or (at your",
    "option) any later version.",
    "",
    "This program is distributed in the hope that it will be useful, but",
    "WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY",
    "or FITNESS FOR A PARTICULAR PURPOSE.",
    "See the GNU General Public License for more details.",
});
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

// Counts mirror the widths above. dataLineToVisualRow runs per cursor
// movement and was calling .size() on the function-static vector each
// time; cache the length so the hot path is purely arithmetic.
static std::size_t cachedLeftCount() {
  static std::size_t const n = leftColumn().size();
  return n;
}

static std::size_t cachedRightCount() {
  static std::size_t const n = rightColumn().size();
  return n;
}

static bool isSideBySide(std::size_t cols) {
  return cachedLeftWidth() + kGap + cachedRightWidth() <= cols;
}

// Map a data-line index to an absolute visual row.
static std::size_t dataLineToVisualRow(std::size_t dataLine, std::size_t cols) {
  auto leftCount = cachedLeftCount();
  auto rightCount = cachedRightCount();
  std::size_t leftEnd = kHeaderLines + leftCount;
  std::size_t rightStart = leftEnd + 1;
  std::size_t rightEnd = rightStart + rightCount;

  if (!isSideBySide(cols))
    return dataLine; // stacked: 1:1

  // Side-by-side: left and right columns share visual rows
  // [kHeaderLines .. kHeaderLines + max(leftCount, rightCount)).
  // The data-line layout still has a blank between blocks at index
  // leftEnd; map that blank to the slot just past the merged block
  // (the same row we use as "blank before footer") so a cursor on it
  // does not land on a row that still has right-column content.
  std::size_t mergedRows = std::max(leftCount, rightCount);
  if (dataLine < leftEnd)
    return dataLine;
  if (dataLine == leftEnd)
    return kHeaderLines + mergedRows; // blank slot
  if (dataLine < rightEnd)
    return kHeaderLines + (dataLine - rightStart);
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

  // Footer: center each line individually.
  for (auto fl : kFooterLines) {
    auto flw = visualWidth(fl);
    std::size_t fpad = (c > flw) ? (c - flw) / 2 : 0;
    std::string s(fpad, ' ');
    s += fl;
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
  auto addLine = [&](std::string_view sv) { _lines.push_back(makeLine(sv)); };
  addLine(kTitle);
  addLine({});
  addLine({});
  for (auto& l : leftColumn())
    addLine(l);
  addLine({});
  for (auto& l : rightColumn())
    addLine(l);
  addLine({});
  for (auto fl : kFooterLines)
    addLine(fl);
}

// Help layout assumes one data line per visual row. The `wrap` parameter
// is intentionally ignored: the help text is composed in-process and is
// laid out from `kTitle`, `leftColumn()`, `rightColumn()` and
// `kFooterLines`, all of which are short. If the terminal is narrower
// than any of those entries the visual count diverges from
// dataLineToVisualRow's output and cursor placement is approximate;
// sub-30-column terminals are unsupported by design.
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

  // Hoist references to the function-static column vectors once per
  // render() so the per-row paths below (and isSectionHeader checks)
  // do not re-call the wrappers.
  auto const& left = leftColumn();
  auto const& right = rightColumn();

  // Cursor data-line and column within that line.
  std::size_t dataLine = params.point.line;
  std::size_t dataCol = params.point.offset;

  // Layout indices computed from generated vectors.
  std::size_t leftEnd = kHeaderLines + left.size();
  std::size_t rightStart = leftEnd + 1;
  std::size_t rightEnd = rightStart + right.size();
  std::size_t footerStart = rightEnd + 1;

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
  } else if (dataLine >= footerStart && dataLine < footerStart + kFooterLines.size()) {
    auto fl = kFooterLines[dataLine - footerStart];
    auto flw = visualWidth(fl);
    std::size_t fpad = (c > flw) ? (c - flw) / 2 : 0;
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
    ansi::appendCursorTo(out, params.rowOffset + r, params.colOffset);
    if (vi < vlines.size()) {
      auto& content = vlines[vi].content;
      // Check if any section header appears in this visual line.
      // In side-by-side mode, a line can contain headers from both columns.
      bool hasDim = false;
      if (sbs && vi >= kHeaderLines) {
        std::size_t idx = vi - kHeaderLines;
        bool leftIsHeader = idx < left.size() && isSectionHeader(left[idx]);
        bool rightIsHeader = idx < right.size() && isSectionHeader(right[idx]);
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
    ansi::appendCursorTo(out, params.rowOffset + r, params.colOffset + cols.value - 1);
    out += ansi::ReverseVideo;
    out += ' ';
    out += ansi::Reset;
  }

  return {RowIdx{curScreenRow}, ColIdx{curCol}};
}
