#include "help_buffer.hpp"

#include <algorithm>
#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>

static constexpr std::array kCommandLines = {
    std::string_view{"Commands:"},
    std::string_view{"  Ctrl-b      Switch/open buffer"},
    std::string_view{"  Ctrl-c      Exit"},
    std::string_view{"  Ctrl-h      Show this help"},
    std::string_view{"  Ctrl-k      Kill buffer"},
    std::string_view{"  Ctrl-l      Go to line"},
    std::string_view{"  Ctrl-n      Buffer list"},
    std::string_view{"  Ctrl-s      Save file"},
    std::string_view{"  Ctrl-w      Toggle whitespace"},
};

static constexpr std::array kMovementLines = {
    std::string_view{"Movement:"},
    std::string_view{"  Arrows      Move cursor"},
    std::string_view{"  Home        Start of line"},
    std::string_view{"  End         End of line"},
    std::string_view{"  Bksp        Delete before cursor"},
    std::string_view{"  Delete      Delete at cursor"},
    std::string_view{"  Enter       Insert newline"},
};

static constexpr std::string_view kFooterText = "Press 'q', Esc, or Ctrl-h to return to editing.";
static constexpr std::string_view kTitle = "cx";
static constexpr std::size_t kGap = 6;

// Data layout (one line per entry), derived from array sizes:
//   0..2             header (title, separator, blank)
//   kCmdStart..      command lines (kCommandLines)
//   kCmdEnd          blank
//   kMovStart..      movement lines (kMovementLines)
//   kMovEnd          blank
//   kFooter          footer
//
// In stacked mode the visual rows are 1:1 with data lines.
// In side-by-side mode the movement block occupies the right column
// alongside the command block.
static constexpr std::size_t kHeaderLines = 3;
static constexpr std::size_t kCmdStart  = kHeaderLines;
static constexpr std::size_t kCmdEnd    = kCmdStart + kCommandLines.size();
static constexpr std::size_t kMovStart  = kCmdEnd + 1;   // +1 for blank
static constexpr std::size_t kMovEnd    = kMovStart + kMovementLines.size();
static constexpr std::size_t kFooter    = kMovEnd + 1;   // +1 for blank

static std::size_t maxWidth(std::span<const std::string_view> lines) {
    std::size_t w = 0;
    for (auto& l : lines)
        w = std::max(w, l.size());
    return w;
}

static std::size_t countNewlinesBefore(std::vector<char> const& data, std::size_t pos) {
    std::size_t n = 0;
    for (std::size_t i = 0; i < pos && i < data.size(); ++i)
        if (data[i] == '\n') ++n;
    return n;
}

static bool isSideBySide(std::size_t cols) {
    return maxWidth(kCommandLines) + kGap + maxWidth(kMovementLines) <= cols;
}

// Map a data-line index to an absolute visual row.
static std::size_t dataLineToVisualRow(std::size_t dataLine, std::size_t cols) {
    if (!isSideBySide(cols))
        return dataLine; // stacked: 1:1

    // Side-by-side: movement block (data lines 14-20) merges into
    // the same visual rows as the command block (data lines 3-13).
    if (dataLine < kMovStart)
        return dataLine;
    if (dataLine < kMovEnd) {
        // Movement lines share rows with command lines.
        return kCmdStart + (dataLine - kMovStart);
    }
    // Lines after movement block: collapse the movement range.
    // Data lines kMovStart..kMovEnd-1 (7 lines) share rows with
    // kCmdStart..kCmdEnd-1 (10 cmd entries). The merged block has
    // max(10,7) = 10 rows. After that: blank + footer.
    std::size_t cmdCount = kCmdEnd - kCmdStart;
    std::size_t movCount = kMovEnd - kMovStart;
    std::size_t mergedRows = std::max(cmdCount, movCount);
    // Data line kMovEnd (21, blank) → visual row kCmdStart + mergedRows
    // Data line kFooter (22) → visual row kCmdStart + mergedRows + 1
    return kCmdStart + mergedRows + (dataLine - kMovEnd);
}

// Build all visual lines for a given column width.
struct VisualLine {
    std::string content;
    std::size_t len = 0;
};

static std::vector<VisualLine> buildVisualLines(std::size_t cols) {
    std::vector<VisualLine> vlines;
    std::size_t c = cols;

    // Row 0: centered title.
    {
        std::size_t pad = (c > kTitle.size()) ? (c - kTitle.size()) / 2 : 0;
        std::string s(pad, ' ');
        s += kTitle;
        vlines.push_back({s, pad + kTitle.size()});
    }

    // Row 1: separator.
    vlines.push_back({std::string(c, '='), c});

    // Row 2: blank.
    vlines.push_back({"", 0});

    std::size_t leftW = maxWidth(kCommandLines);
    std::size_t rightW = maxWidth(kMovementLines);
    std::size_t totalW = leftW + kGap + rightW;
    bool sbs = isSideBySide(c);
    std::size_t cmdCount = kCommandLines.size();
    std::size_t movCount = kMovementLines.size();

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
                line += kCommandLines[i];
                std::size_t cur = line.size() - leftPad;
                if (cur < leftW)
                    line.append(leftW - cur, ' ');
            } else {
                line.append(leftW, ' ');
            }
            line.append(kGap, ' ');
            if (i < movCount)
                line += kMovementLines[i];
            vlines.push_back({line, line.size()});
        }
    } else {
        for (std::size_t i = 0; i < cmdCount; ++i) {
            std::string line(leftPad, ' ');
            line += kCommandLines[i];
            vlines.push_back({line, line.size()});
        }
        vlines.push_back({"", 0}); // blank between blocks
        for (std::size_t i = 0; i < movCount; ++i) {
            std::string line(leftPad, ' ');
            line += kMovementLines[i];
            vlines.push_back({line, line.size()});
        }
    }

    // Blank before footer.
    vlines.push_back({"", 0});

    // Footer centered.
    {
        std::size_t fpad = (c > kFooterText.size()) ? (c - kFooterText.size()) / 2 : 0;
        std::string s(fpad, ' ');
        s += kFooterText;
        vlines.push_back({s, fpad + kFooterText.size()});
    }

    return vlines;
}

HelpBuffer::HelpBuffer() : Buffer("*help*") {
    // _data holds the full content in stacked order so that base-class
    // navigation (moveUp/Down, scrollUp/Down) and _viewFrom work correctly.
    std::string text;
    text += kTitle;               text += '\n'; // 0  title
    text += '\n';                               // 1  separator (rendered as '=')
    text += '\n';                               // 2  blank
    for (auto& l : kCommandLines) { text += l; text += '\n'; } // 3-12
    text += '\n';                               // 13 blank
    for (auto& l : kMovementLines) { text += l; text += '\n'; } // 14-20
    text += '\n';                               // 21 blank
    text += kFooterText;          text += '\n'; // 22 footer
    _data.assign(text.begin(), text.end());
}

std::size_t HelpBuffer::screenRow(ColIdx cols) const {
    std::size_t pointLine = countNewlinesBefore(_data, _point);
    std::size_t scrollLine = countNewlinesBefore(_data, _viewFrom);
    std::size_t pointVis = dataLineToVisualRow(pointLine, cols.value);
    std::size_t scrollVis = dataLineToVisualRow(scrollLine, cols.value);
    return pointVis >= scrollVis ? pointVis - scrollVis : 0;
}

std::tuple<RowIdx, ColIdx> HelpBuffer::render(
        std::string& out, RowIdx rows, ColIdx cols, bool /*showWhitespace*/) const {

    std::size_t c = cols.value;

    // Cursor data-line and column within that line.
    std::size_t dataLine = 0, dataCol = 0;
    for (std::size_t i = 0; i < _point && i < _data.size(); ++i) {
        if (_data[i] == '\n') { ++dataLine; dataCol = 0; }
        else ++dataCol;
    }

    // Scroll offset: visual row corresponding to _viewFrom.
    std::size_t scrollLine = countNewlinesBefore(_data, _viewFrom);
    std::size_t scrollVis = dataLineToVisualRow(scrollLine, c);

    // Cursor visual row, relative to scroll.
    std::size_t curVis = dataLineToVisualRow(dataLine, c);
    std::size_t curScreenRow = curVis >= scrollVis ? curVis - scrollVis : 0;

    // Cursor column with centering.
    std::size_t leftW = maxWidth(kCommandLines);
    std::size_t rightW = maxWidth(kMovementLines);
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
    } else if (dataLine >= kCmdStart && dataLine < kCmdEnd) {
        curCol = leftPad + dataCol;
    } else if (dataLine >= kMovStart && dataLine < kMovEnd) {
        if (sbs)
            curCol = leftPad + leftW + kGap + dataCol;
        else
            curCol = leftPad + dataCol;
    } else if (dataLine == kFooter) {
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
    }

    return {RowIdx{curScreenRow}, ColIdx{curCol}};
}
