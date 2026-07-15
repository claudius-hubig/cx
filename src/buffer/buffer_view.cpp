#include "buffer_view.hpp"

#include "modes/c/syntax.hpp"
#include "modes/jsn/syntax.hpp"
#include "types.hpp"
#include "utf8.hpp"

#include <algorithm>
#include <cassert>
#include <variant>

BufferView::BufferView(Buffer& buf) : _buf(buf), _docVersion(buf.docVersion()) {}

BufferView BufferView::fromSavedState(Buffer& buf) {
  BufferView v(buf);
  if (auto saved = buf.savedView()) {
    v._point = saved->point;
    v._viewFrom = saved->viewFrom;
    v._mark = saved->mark;
    if (saved->desiredCol)
      v._desiredCol = ColIdx{*saved->desiredCol};
    v._wrap = saved->wrapMode;
    v._wrapMode = saved->wrapMode;
    v._showWhitespace = saved->showWhitespace;
    v._showLineNumbers = saved->showLineNumbers;
    v._highlightCurrentLine = saved->highlightCurrentLine;
    buf.clearSavedView();
  }
  v._docVersion = buf.docVersion();
  return v;
}

void BufferView::clampToBuffer() {
  // Buffer guarantees at least one line; assert here so any subclass
  // regression that drops the invariant fails loudly instead of
  // underflowing the lineCount() - 1 expressions below.
  assert(_buf.lineCount() > 0);

  _docVersion = _buf.docVersion();
  // Clamp point.
  if (_point.line >= _buf.lineCount())
    _point.line = _buf.lineCount() - 1;
  if (_point.offset > _buf.lineContent(_point.line).size())
    _point.offset = _buf.lineContent(_point.line).size();
  // Clamp mark.
  if (_mark) {
    if (_mark->line >= _buf.lineCount())
      _mark->line = _buf.lineCount() - 1;
    if (_mark->offset > _buf.lineContent(_mark->line).size())
      _mark->offset = _buf.lineContent(_mark->line).size();
  }
  // Clamp viewport.
  if (_viewFrom.line.value >= _buf.lineCount())
    _viewFrom.line = RowIdx{_buf.lineCount() - 1};
  // In wrap mode the viewport's sub-line ByteOffset can land mid-codepoint
  // or past the new line size after another view shortens this line.
  // VisualCol is purely a horizontal column (not a byte index) so it
  // does not need clamping here.
  if (auto* off = std::get_if<ByteOffset>(&_viewFrom.col)) {
    std::size_t maxOff = _buf.lineContent(_viewFrom.line.value).size();
    if (off->value > maxOff)
      *off = ByteOffset{maxOff};
  }
  // Invalidate per-view caches and the desired-column memory: the line
  // it referred to may have changed shape.
  _pointColCache.line = SIZE_MAX;
  _rowBoundsCache.lineIdx = SIZE_MAX;
  _desiredCol.reset();
}

void BufferView::saveViewState() {
  std::optional<std::size_t> dc;
  if (_desiredCol)
    dc = _desiredCol->value;
  _buf.setSavedView({.point = _point,
                     .viewFrom = _viewFrom,
                     .mark = _mark,
                     .desiredCol = dc,
                     .wrapMode = _wrap,
                     .showWhitespace = _showWhitespace,
                     .showLineNumbers = _showLineNumbers,
                     .highlightCurrentLine = _highlightCurrentLine});
}

std::size_t BufferView::lineNumberGutterWidth() const {
  if (!_showLineNumbers || !_buf.supportsLineNumberGutter())
    return 0;
  // 1-based line numbers, plus a single trailing space separator.
  std::size_t lc = _buf.lineCount();
  std::size_t digits = 1;
  while (lc >= 10) {
    lc /= 10;
    ++digits;
  }
  return digits + 1;
}

void BufferView::setWrap(bool w) {
  if (w == _wrap)
    return;
  _wrap = w;
  // Reset the horizontal/sub-line scroll state because the variant
  // alternative changes when wrap mode flips. ensureVisible() also
  // does this on its next call, but updating eagerly avoids a stale
  // viewport between toggle and the next render.
  _viewFrom.resetCol(w);
}

// --- Private helpers ---

std::size_t BufferView::contentCols(ColIdx cols) const {
  std::size_t cw = contentWidth(cols);
  std::size_t lng = lineNumberGutterWidth();
  return cw > lng ? cw - lng : 1;
}

std::size_t BufferView::pointVisualCol() const {
  if (_pointColCache.line == _point.line && _pointColCache.offset == _point.offset)
    return _pointColCache.visualCol;
  // Compute both visual and codepoint columns in a single scan.
  _pointColCache.line = _point.line;
  _pointColCache.offset = _point.offset;
  _pointColCache.visualCol = 0;
  _pointColCache.codepointCol = 0;
  auto const& line = _buf.lineContent(_point.line);
  for (std::size_t i = 0; i < _point.offset && i < line.size();) {
    auto info = charInfoAt(line.data() + i, line.size() - i);
    _pointColCache.visualCol += info.width;
    i += info.len;
    ++_pointColCache.codepointCol;
  }
  return _pointColCache.visualCol;
}

std::vector<std::size_t> const& BufferView::ensureRowBounds(std::size_t lineIdx,
                                                            std::size_t cols) const {
  if (_rowBoundsCache.lineIdx == lineIdx && _rowBoundsCache.cols == cols)
    return _rowBoundsCache.rowStarts;

  _rowBoundsCache.lineIdx = lineIdx;
  _rowBoundsCache.cols = cols;
  _rowBoundsCache.rowStarts.clear();
  _rowBoundsCache.rowStarts.push_back(0);

  auto const& line = _buf.lineContent(lineIdx);
  std::size_t col = 0;
  for (std::size_t i = 0; i < line.size();) {
    auto info = charInfoAt(line.data() + i, line.size() - i);
    if (col + info.width > cols) {
      _rowBoundsCache.rowStarts.push_back(i);
      col = 0;
    }
    col += info.width;
    if (col >= cols) {
      _rowBoundsCache.rowStarts.push_back(i + info.len);
      col = 0;
    }
    i += info.len;
  }
  return _rowBoundsCache.rowStarts;
}

std::size_t BufferView::viewFromLinesBeforeEnd(std::size_t lines) const {
  if (lines >= _buf.lineCount())
    return 0;
  return _buf.lineCount() - lines;
}

bool BufferView::atEnd() const {
  return _point.line == _buf.lineCount() - 1 &&
         _point.offset == _buf.lineContent(_point.line).size();
}

// --- View state accessors ---

BufferPoint BufferView::viewFrom() const {
  if (_docVersion != _buf.docVersion())
    const_cast<BufferView*>(this)->clampToBuffer();
  std::size_t off = std::holds_alternative<ByteOffset>(_viewFrom.col) ? _viewFrom.byteOffset() : 0;
  return {_viewFrom.line.value, off};
}

void BufferView::setPoint(std::size_t line, std::size_t offset) {
  if (_docVersion != _buf.docVersion())
    clampToBuffer();
  _point.line = std::min(line, _buf.lineCount() - 1);
  _point.offset = std::min(offset, _buf.lineContent(_point.line).size());
  _desiredCol.reset();
}

void BufferView::toggleMark() {
  if (_mark)
    _mark.reset();
  else
    _mark = _point;
}

std::tuple<std::size_t, std::size_t> BufferView::lineCol() const {
  if (_docVersion != _buf.docVersion())
    const_cast<BufferView*>(this)->clampToBuffer();
  // Ensure the cache is populated (also computes codepointCol).
  pointVisualCol();
  return {_point.line + 1, _pointColCache.codepointCol + 1};
}

// --- Movement ---

void BufferView::moveLeft() {
  if (_docVersion != _buf.docVersion())
    clampToBuffer();
  if (_point.offset > 0) {
    auto const& line = _buf.lineContent(_point.line);
    --_point.offset;
    while (_point.offset > 0 && isUtf8Continuation(static_cast<unsigned char>(line[_point.offset])))
      --_point.offset;
  } else if (_point.line > 0) {
    --_point.line;
    _point.offset = _buf.lineContent(_point.line).size();
  }
  _desiredCol.reset();
}

void BufferView::moveRight() {
  if (_docVersion != _buf.docVersion())
    clampToBuffer();
  auto const& line = _buf.lineContent(_point.line);
  if (_point.offset < line.size()) {
    auto len = utf8SeqLen(static_cast<unsigned char>(line[_point.offset]));
    _point.offset = std::min(_point.offset + len, line.size());
  } else if (_point.line + 1 < _buf.lineCount()) {
    ++_point.line;
    _point.offset = 0;
  }
  _desiredCol.reset();
}

void BufferView::moveUp(ColIdx cols) {
  if (_docVersion != _buf.docVersion())
    clampToBuffer();
  auto const& line = _buf.lineContent(_point.line);
  // Content width excludes both gutters (right wrap-indicator + left
  // line-number).
  std::size_t cw = contentCols(cols);
  auto& bounds = ensureRowBounds(_point.line, cw);
  auto vr = findVisualRowCached(bounds, _point.offset, line.size());

  // Screen column within the current visual row.
  std::size_t vcol = visualColRange(line, vr.curRowStart, _point.offset);
  std::size_t goal = _desiredCol ? _desiredCol->value : vcol;

  std::size_t targetRowStart, targetRowEnd;
  if (vr.curRowStart > 0) {
    // Move to previous visual row of the same physical line.
    targetRowStart = vr.prevRowStart;
    targetRowEnd = vr.curRowStart;
  } else if (_point.line > 0) {
    // Move to last visual row of previous physical line.
    --_point.line;
    auto& prevBounds = ensureRowBounds(_point.line, cw);
    targetRowStart = prevBounds.back();
    targetRowEnd = _buf.lineContent(_point.line).size();
  } else {
    _point.offset = 0;
    _desiredCol.reset();
    return;
  }

  _point.offset =
      offsetForVisualCol(_buf.lineContent(_point.line), targetRowStart, targetRowEnd, goal);

  std::size_t actual = visualColRange(_buf.lineContent(_point.line), targetRowStart, _point.offset);
  if (actual == goal)
    _desiredCol.reset();
  else if (!_desiredCol)
    _desiredCol = ColIdx{goal};
}

void BufferView::moveDown(ColIdx cols) {
  if (_docVersion != _buf.docVersion())
    clampToBuffer();
  auto const& line = _buf.lineContent(_point.line);
  // Content width excludes both gutters.
  std::size_t cw = contentCols(cols);
  auto& bounds = ensureRowBounds(_point.line, cw);
  auto vr = findVisualRowCached(bounds, _point.offset, line.size());

  // Screen column within the current visual row.
  std::size_t vcol = visualColRange(line, vr.curRowStart, _point.offset);
  std::size_t goal = _desiredCol ? _desiredCol->value : vcol;

  std::size_t targetRowStart, targetRowEnd;
  if (vr.nextRowStart < line.size()) {
    // Move to next visual row of the same physical line.
    targetRowStart = vr.nextRowStart;
    auto nextVr = findVisualRowCached(bounds, vr.nextRowStart, line.size());
    targetRowEnd = nextVr.nextRowStart;
  } else if (_point.line + 1 < _buf.lineCount()) {
    // Move to first visual row of next physical line.
    ++_point.line;
    auto const& next = _buf.lineContent(_point.line);
    targetRowStart = 0;
    if (next.empty()) {
      targetRowEnd = 0;
    } else {
      auto& nextBounds = ensureRowBounds(_point.line, cw);
      targetRowEnd = nextBounds.size() > 1 ? nextBounds[1] : next.size();
    }
  } else {
    _point.offset = line.size();
    _desiredCol.reset();
    return;
  }

  _point.offset =
      offsetForVisualCol(_buf.lineContent(_point.line), targetRowStart, targetRowEnd, goal);

  std::size_t actual = visualColRange(_buf.lineContent(_point.line), targetRowStart, _point.offset);
  if (actual == goal)
    _desiredCol.reset();
  else if (!_desiredCol)
    _desiredCol = ColIdx{goal};
}

void BufferView::movePhysicalUp() {
  if (_docVersion != _buf.docVersion())
    clampToBuffer();
  std::size_t vcol = pointVisualCol();
  std::size_t goal = _desiredCol ? _desiredCol->value : vcol;

  if (_point.line == 0) {
    _point.offset = 0;
    _desiredCol.reset();
    return;
  }

  --_point.line;
  _point.offset = offsetForVisualCol(_buf.lineContent(_point.line), 0,
                                     _buf.lineContent(_point.line).size(), goal);

  std::size_t actual = pointVisualCol();
  if (actual == goal)
    _desiredCol.reset();
  else if (!_desiredCol)
    _desiredCol = ColIdx{goal};
}

void BufferView::movePhysicalDown() {
  if (_docVersion != _buf.docVersion())
    clampToBuffer();
  std::size_t vcol = pointVisualCol();
  std::size_t goal = _desiredCol ? _desiredCol->value : vcol;

  if (_point.line + 1 >= _buf.lineCount()) {
    _point.offset = _buf.lineContent(_point.line).size();
    _desiredCol.reset();
    return;
  }

  ++_point.line;
  _point.offset = offsetForVisualCol(_buf.lineContent(_point.line), 0,
                                     _buf.lineContent(_point.line).size(), goal);

  std::size_t actual = pointVisualCol();
  if (actual == goal)
    _desiredCol.reset();
  else if (!_desiredCol)
    _desiredCol = ColIdx{goal};
}

void BufferView::moveHome() {
  if (_docVersion != _buf.docVersion())
    clampToBuffer();
  _point.offset = 0;
  _desiredCol.reset();
}

void BufferView::moveEnd() {
  if (_docVersion != _buf.docVersion())
    clampToBuffer();
  _point.offset = _buf.lineContent(_point.line).size();
  _desiredCol.reset();
}

void BufferView::moveWordRight() {
  if (_docVersion != _buf.docVersion())
    clampToBuffer();
  auto const& line = _buf.lineContent(_point.line);
  // At very end of buffer?
  if (_point.line == _buf.lineCount() - 1 && _point.offset >= line.size()) {
    _desiredCol.reset();
    return;
  }

  auto isSep = isWhitespace;

  // Step forward one code point (or cross to next line).
  if (_point.offset < line.size()) {
    auto len = utf8SeqLen(static_cast<unsigned char>(line[_point.offset]));
    _point.offset = std::min(_point.offset + len, line.size());
  }
  // If at end of line, cross to next.
  if (_point.offset >= _buf.lineContent(_point.line).size() && _point.line + 1 < _buf.lineCount()) {
    ++_point.line;
    _point.offset = 0;
  }

  // Skip separators and line boundaries forward.
  for (;;) {
    auto const& cur = _buf.lineContent(_point.line);
    while (_point.offset < cur.size() && isSep(cur[_point.offset]))
      ++_point.offset;
    if (_point.offset < cur.size())
      break; // found non-sep
    // At end of line - cross to next.
    if (_point.line + 1 < _buf.lineCount()) {
      ++_point.line;
      _point.offset = 0;
    } else {
      break;
    }
  }

  // Advance past the last char of this word so typing immediately
  // extends the word. moveWordLeft already lands on the first char of
  // its target word, so this is the symmetric convention.
  {
    auto const& cur = _buf.lineContent(_point.line);
    while (_point.offset < cur.size() && !isSep(cur[_point.offset])) {
      auto len = utf8SeqLen(static_cast<unsigned char>(cur[_point.offset]));
      _point.offset = std::min(_point.offset + len, cur.size());
    }
  }
  _desiredCol.reset();
}

void BufferView::moveWordLeft() {
  if (_docVersion != _buf.docVersion())
    clampToBuffer();
  if (_point.line == 0 && _point.offset == 0) {
    _desiredCol.reset();
    return;
  }

  auto isSep = isWhitespace;

  // Step back one code point (or cross to previous line).
  if (_point.offset > 0) {
    auto const& line = _buf.lineContent(_point.line);
    --_point.offset;
    while (_point.offset > 0 && isUtf8Continuation(static_cast<unsigned char>(line[_point.offset])))
      --_point.offset;
  } else if (_point.line > 0) {
    --_point.line;
    _point.offset = _buf.lineContent(_point.line).size();
    // We're now at the "newline" position. Step back once more if possible.
    if (_point.offset > 0) {
      auto const& line = _buf.lineContent(_point.line);
      --_point.offset;
      while (_point.offset > 0 &&
             isUtf8Continuation(static_cast<unsigned char>(line[_point.offset])))
        --_point.offset;
    }
  }

  // Skip separators and line boundaries backward.
  for (;;) {
    auto const& cur = _buf.lineContent(_point.line);
    if (_point.offset < cur.size() && !isSep(cur[_point.offset]))
      break; // on a non-sep char
    // Skip seps backward within line.
    bool foundNonSep = false;
    while (_point.offset > 0) {
      std::size_t prev = _point.offset - 1;
      while (prev > 0 && isUtf8Continuation(static_cast<unsigned char>(cur[prev])))
        --prev;
      if (!isSep(cur[prev])) {
        _point.offset = prev;
        foundNonSep = true;
        break;
      }
      _point.offset = prev;
    }
    if (foundNonSep)
      break;
    // Check position 0.
    if (!cur.empty() && !isSep(cur[0])) {
      _point.offset = 0;
      break;
    }
    // Cross to previous line.
    if (_point.line > 0) {
      --_point.line;
      auto const& prev = _buf.lineContent(_point.line);
      _point.offset = prev.size();
      if (_point.offset > 0) {
        --_point.offset;
        while (_point.offset > 0 &&
               isUtf8Continuation(static_cast<unsigned char>(prev[_point.offset])))
          --_point.offset;
      }
    } else {
      break;
    }
  }

  // If currently on a separator, we're done.
  {
    auto const& cur = _buf.lineContent(_point.line);
    if (_point.offset < cur.size() && isSep(cur[_point.offset])) {
      _desiredCol.reset();
      return;
    }
  }

  // Walk backward to the first char of this word.
  {
    auto const& cur = _buf.lineContent(_point.line);
    while (_point.offset > 0) {
      std::size_t prev = _point.offset - 1;
      while (prev > 0 && isUtf8Continuation(static_cast<unsigned char>(cur[prev])))
        --prev;
      if (isSep(cur[prev]))
        break;
      _point.offset = prev;
    }
  }
  _desiredCol.reset();
}

void BufferView::moveParagraphUp() {
  if (_docVersion != _buf.docVersion())
    clampToBuffer();
  if (_point.line == 0) {
    _point = {0, 0};
    _desiredCol.reset();
    return;
  }

  // Go to the line above.
  std::size_t pos = _point.line - 1;

  // Skip blank lines upward.
  while (pos > 0 && _buf.isBlankLine(pos))
    --pos;

  // After the loop, pos is either 0 or pointing at a non-blank line.
  // If we walked to the top without finding content, jump to {0, 0}.
  if (pos == 0 && _buf.isBlankLine(0)) {
    _point = {0, 0};
    _desiredCol.reset();
    return;
  }

  // On a non-blank line. Find the start of this paragraph.
  while (pos > 0 && !_buf.isBlankLine(pos - 1))
    --pos;

  _point = {pos, 0};
  _desiredCol.reset();
}

void BufferView::moveParagraphDown() {
  if (_docVersion != _buf.docVersion())
    clampToBuffer();
  bool onBlank = _buf.isBlankLine(_point.line);
  std::size_t pos = _point.line + 1;

  // If current line was non-blank, skip non-blank lines (rest of paragraph).
  if (!onBlank) {
    while (pos < _buf.lineCount() && !_buf.isBlankLine(pos))
      ++pos;
  }

  // Skip blank lines.
  while (pos < _buf.lineCount() && _buf.isBlankLine(pos))
    ++pos;

  if (pos >= _buf.lineCount())
    pos = _buf.lineCount() - 1;
  _point = {pos, 0};
  _desiredCol.reset();
}

void BufferView::pageUp(RowIdx screenRows, ColIdx cols, bool wrap) {
  if (_docVersion != _buf.docVersion())
    clampToBuffer();
  std::size_t half = screenRows.value / 2;
  if (half == 0)
    half = 1;
  // Each step calls scrollUp/scrollDown which redundantly checks
  // docVersion (we already clamped above), but the cost is negligible
  // compared to the visual-row scan they perform internally. No
  // explicit upper clamp here: scrollUp self-bounds at line 0 via its
  // `_viewFrom.line.value > 0` guard, unlike scrollDown which advances
  // unconditionally and is clamped by pageDown via viewFromLinesBeforeEnd.
  for (std::size_t i = 0; i < half; ++i) {
    if (wrap)
      moveUp(cols);
    else
      movePhysicalUp();
    scrollUp(wrap);
  }
}

void BufferView::pageDown(RowIdx screenRows, ColIdx cols, bool wrap) {
  if (_docVersion != _buf.docVersion())
    clampToBuffer();
  std::size_t half = screenRows.value / 2;
  if (half == 0)
    half = 1;
  for (std::size_t i = 0; i < half; ++i) {
    if (wrap)
      moveDown(cols);
    else
      movePhysicalDown();
    scrollDown();
  }
  // Clamp viewport so at least half a screen of content is visible.
  std::size_t limit = viewFromLinesBeforeEnd(half);
  if (_viewFrom.line.value > limit) {
    _viewFrom.line = RowIdx{limit};
    // Reset wrap-mode sub-line scroll; preserve horizontal scroll in no-wrap.
    if (std::holds_alternative<ByteOffset>(_viewFrom.col))
      _viewFrom.col = ByteOffset{0};
  }
}

BufferPoint BufferView::pointForScreenRowCol(std::size_t screenRow, std::size_t screenCol,
                                             ColIdx cols, bool wrap) const {
  std::size_t cw = contentCols(cols);
  std::size_t lc = _buf.lineCount();
  if (lc == 0)
    return {0, 0};
  // Translate screen column to content column by stripping the
  // line-number gutter. Clicks inside the gutter map to column 0 of
  // the row's physical line.
  std::size_t lng = lineNumberGutterWidth();
  if (screenCol < lng)
    screenCol = 0;
  else
    screenCol -= lng;

  if (!wrap) {
    std::size_t hScroll =
        std::holds_alternative<VisualCol>(_viewFrom.col) ? _viewFrom.hScroll() : 0;
    std::size_t li = _viewFrom.line.value + screenRow;
    if (li >= lc)
      li = lc - 1;
    auto const& line = _buf.lineContent(li);
    std::size_t goal = hScroll + screenCol;
    auto off = offsetForVisualCol(line, 0, line.size(), goal);
    return {li, off};
  }

  // Wrap mode: walk visible lines forward, accumulating wrap rows, until
  // we hit `screenRow`.
  std::size_t startByte =
      std::holds_alternative<ByteOffset>(_viewFrom.col) ? _viewFrom.byteOffset() : 0;
  std::size_t row = 0;
  std::size_t li = _viewFrom.line.value;
  while (li < lc) {
    auto const& line = _buf.lineContent(li);
    std::size_t firstByte = (li == _viewFrom.line.value) ? startByte : 0;
    auto bounds = computeRowBounds(line, cw);
    // Drop any row-starts strictly before the viewport start.
    std::size_t startRowIdx = 0;
    while (startRowIdx + 1 < bounds.size() && bounds[startRowIdx + 1] <= firstByte)
      ++startRowIdx;
    std::size_t numRows = bounds.size() - startRowIdx;
    if (line.empty())
      numRows = 1;
    if (row + numRows > screenRow) {
      // Click lands on this physical line.
      std::size_t subRow = screenRow - row;
      std::size_t rowStart = bounds[startRowIdx + subRow];
      std::size_t rowEnd = (startRowIdx + subRow + 1 < bounds.size())
                               ? bounds[startRowIdx + subRow + 1]
                               : line.size();
      auto off = offsetForVisualCol(line, rowStart, rowEnd, screenCol);
      return {li, off};
    }
    row += numRows;
    ++li;
  }
  // Past the end: clamp to the last line, last byte.
  std::size_t lastLi = lc - 1;
  return {lastLi, _buf.lineContent(lastLi).size()};
}

bool BufferView::gotoMatchingBracket() {
  if (_docVersion != _buf.docVersion())
    clampToBuffer();
  auto getTokens = [this](std::size_t i) { return _buf.cachedLineTokens(i); };
  auto match = (_buf.mode() == BufferMode::Jsn)
                   ? findMatchingBracketJson(_buf.lines(), _point, getTokens)
                   : findMatchingBracket(_buf.lines(), _point, getTokens);
  if (!match)
    return false;
  _point = *match;
  _desiredCol.reset();
  return true;
}

void BufferView::gotoLine(std::size_t line, RowIdx screenRows) {
  if (_docVersion != _buf.docVersion())
    clampToBuffer();
  if (line == 0)
    line = 1;
  // Convert to 0-based, clamp to last line.
  std::size_t idx = line - 1;
  if (idx >= _buf.lineCount())
    idx = _buf.lineCount() - 1;

  _point = {idx, 0};

  // Center _viewFrom so the target line is in the middle of the screen.
  std::size_t half = screenRows.value / 2;
  _viewFrom.line = RowIdx{idx >= half ? idx - half : 0};
  // Preserve the current variant alternative (ByteOffset for wrap, VisualCol
  // for no-wrap) and just zero it. Unconditionally writing ByteOffset would
  // clobber a no-wrap saved state and any subsequent hScroll() access would
  // throw bad_variant_access. ensureVisible() re-syncs the variant against
  // the active wrap mode on its next call.
  if (std::holds_alternative<ByteOffset>(_viewFrom.col))
    _viewFrom.col = ByteOffset{0};
  else
    _viewFrom.col = VisualCol{0};
  // Drop the desired-column memory: the user is explicitly relocating and
  // any prior column intent is stale.
  _desiredCol.reset();
}

void BufferView::scrollDown() {
  if (_docVersion != _buf.docVersion())
    clampToBuffer();
  if (_viewFrom.line.value + 1 < _buf.lineCount()) {
    _viewFrom.line = RowIdx{_viewFrom.line.value + 1};
    // Reset wrap-mode sub-line scroll at the start of the new physical line;
    // preserve the horizontal scroll position in no-wrap mode.
    if (std::holds_alternative<ByteOffset>(_viewFrom.col))
      _viewFrom.col = ByteOffset{0};
  }
}

void BufferView::scrollUp(bool wrap) {
  if (_docVersion != _buf.docVersion())
    clampToBuffer();
  if (wrap && std::holds_alternative<ByteOffset>(_viewFrom.col) && _viewFrom.byteOffset() > 0) {
    _viewFrom.col = ByteOffset{0};
  } else if (_viewFrom.line.value > 0) {
    _viewFrom.line = RowIdx{_viewFrom.line.value - 1};
    // Reset wrap-mode sub-line scroll; preserve horizontal scroll in no-wrap.
    if (std::holds_alternative<ByteOffset>(_viewFrom.col))
      _viewFrom.col = ByteOffset{0};
  }
}

void BufferView::recenter(RowIdx screenRows, std::size_t position, ColIdx cols, bool wrap) {
  if (_docVersion != _buf.docVersion())
    clampToBuffer();
  if (screenRows.value == 0)
    return;
  if (position >= screenRows.value)
    position = screenRows.value - 1;

  // Walk `position` visual rows back from the cursor's physical line to
  // find the new viewport start. In no-wrap mode each physical line is
  // exactly one visual row; in wrap mode we count wrapped subrows.
  if (!wrap) {
    std::size_t startLine = (_point.line >= position) ? _point.line - position : 0;
    _viewFrom.line = RowIdx{startLine};
    if (std::holds_alternative<ByteOffset>(_viewFrom.col))
      _viewFrom.col = ByteOffset{0};
    return;
  }

  std::size_t cw = contentCols(cols);
  std::size_t remaining = position;
  std::size_t li = _point.line;
  std::size_t startByte = 0;

  // Visual rows on the cursor's own line *before* the cursor.
  auto bounds = computeRowBounds(_buf.lineContent(li), cw);
  std::size_t cursorSubRow = 0;
  for (std::size_t i = 1; i < bounds.size(); ++i) {
    if (bounds[i] > _point.offset)
      break;
    cursorSubRow = i;
  }
  if (cursorSubRow >= remaining) {
    startByte = bounds[cursorSubRow - remaining];
    _viewFrom.line = RowIdx{li};
    _viewFrom.col = ByteOffset{startByte};
    return;
  }
  remaining -= cursorSubRow;

  // Walk earlier physical lines, subtracting their wrapped row counts.
  while (li > 0 && remaining > 0) {
    --li;
    auto const& line = _buf.lineContent(li);
    auto b = computeRowBounds(line, cw);
    std::size_t numRows = line.empty() ? 1 : b.size();
    // Cost of crossing the line break itself.
    if (remaining == 0)
      break;
    --remaining;
    if (numRows > remaining) {
      startByte = b[numRows - 1 - remaining];
      _viewFrom.line = RowIdx{li};
      _viewFrom.col = ByteOffset{startByte};
      return;
    }
    remaining -= numRows - 1;
  }
  _viewFrom.line = RowIdx{0};
  _viewFrom.col = ByteOffset{0};
}

void BufferView::scrollViewNearEnd(RowIdx screenRows) {
  if (_docVersion != _buf.docVersion())
    clampToBuffer();
  std::size_t half = screenRows.value / 2;
  if (half == 0)
    half = 1;
  std::size_t limit = viewFromLinesBeforeEnd(half);
  if (_viewFrom.line.value < limit)
    scrollDown();
}

// --- Editing ---
// These methods call the position-explicit editing API on Buffer and
// adjust the view's point and mark locally.

void BufferView::insert(uint32_t cp) {
  if (_buf.readonly())
    return;
  if (_docVersion != _buf.docVersion())
    clampToBuffer();

  auto insertLine = _point.line;
  auto insertOff = _point.offset;
  auto lineCountBefore = _buf.lineCount();

  // In overwrite mode: delete character under cursor before inserting (unless at end of line)
  // Exception: newline characters are always inserted (don't overwrite)
  bool didOverwriteDelete = false;
  if (_buf.overwriteMode() && cp != '\n') {
    auto const& lineContent = _buf.lineContent(insertLine);
    if (insertOff < lineContent.size()) {
      // Delete the character(s) under the cursor
      _point = _buf.deleteAt(_point);
      _docVersion = _buf.docVersion();
      _rowBoundsCache.lineIdx = SIZE_MAX;
      _pointColCache.line = SIZE_MAX;
      didOverwriteDelete = true;
      // Update line/offset after potential line join
      insertLine = _point.line;
      insertOff = _point.offset;
      lineCountBefore = _buf.lineCount();
    }
  }

  _point = _buf.insertAt(cp, _point);
  _docVersion = _buf.docVersion();
  _rowBoundsCache.lineIdx = SIZE_MAX;
  _pointColCache.line = SIZE_MAX;

  // Compute auto-indent amount for mark adjustment.
  std::size_t autoIndent = 0;
  if (cp == '\n' && (_buf.mode() == BufferMode::Cpp || _buf.mode() == BufferMode::Jsn))
    autoIndent = _point.offset;

  // Adjust mark.
  if (_mark) {
    auto& m = *_mark;
    if (cp == '\n') {
      if (m.line == insertLine && m.offset >= insertOff) {
        m.line = insertLine + 1;
        m.offset = m.offset - insertOff + autoIndent;
      } else if (m.line > insertLine) {
        m.line++;
      }
    } else {
      auto [n, enc] = encodeUtf8(cp);
      // In overwrite mode with a delete, the mark was affected by both delete and insert.
      // Net effect: if mark was at or after the cursor on the same line, it moved back 1
      // from the delete, then forward n from the insert, so net change is n-1.
      if (didOverwriteDelete && m.line == insertLine && m.offset >= insertOff) {
        m.offset += n - 1;
      } else if (m.line == insertLine && m.offset >= insertOff) {
        // Normal insert mode: just add n
        m.offset += n;
      }
    }
  }

  if (_desiredCol) {
    std::size_t vcol = pointVisualCol();
    if (vcol >= _desiredCol->value)
      _desiredCol.reset();
  }

  int shift = static_cast<int>(_buf.lineCount()) - static_cast<int>(lineCountBefore);
  _buf.recordVersion(_point, insertLine, shift);
}

void BufferView::deleteAtPoint() {
  if (_buf.readonly())
    return;
  if (_docVersion != _buf.docVersion())
    clampToBuffer();

  auto delLine = _point.line;
  auto delOff = _point.offset;
  auto const& lineContent = _buf.lineContent(delLine);
  bool withinLine = delOff < lineContent.size();
  std::size_t lineLen = lineContent.size();
  std::size_t delBytes = 0;
  if (withinLine) {
    auto len = utf8SeqLen(static_cast<unsigned char>(lineContent[delOff]));
    delBytes = std::min(delOff + len, lineLen) - delOff;
  }

  auto lineCountBefore = _buf.lineCount();
  auto oldVer = _buf.docVersion();
  _point = _buf.deleteAt(_point);
  if (_buf.docVersion() == oldVer)
    return; // end-of-buffer: nothing changed
  _docVersion = _buf.docVersion();
  _rowBoundsCache.lineIdx = SIZE_MAX;
  _pointColCache.line = SIZE_MAX;

  // Adjust mark.
  if (_mark) {
    if (withinLine) {
      if (_mark->line == delLine && _mark->offset > delOff) {
        if (_mark->offset >= delOff + delBytes)
          _mark->offset -= delBytes;
        else
          _mark->offset = delOff;
      }
    } else {
      // Line join occurred: delLine+1 was merged into delLine.
      if (_mark->line == delLine + 1) {
        _mark->line = delLine;
        _mark->offset += lineLen;
      } else if (_mark->line > delLine + 1) {
        _mark->line--;
      }
    }
  }

  int shift = static_cast<int>(_buf.lineCount()) - static_cast<int>(lineCountBefore);
  _buf.recordVersion(_point, delLine, shift);
}

void BufferView::deleteBeforePoint() {
  if (_buf.readonly())
    return;
  if (_docVersion != _buf.docVersion())
    clampToBuffer();

  auto beforeLine = _point.line;
  auto beforeOff = _point.offset;
  auto lineCountBefore = _buf.lineCount();

  auto oldVer = _buf.docVersion();
  _point = _buf.deleteBefore(_point);
  if (_buf.docVersion() == oldVer)
    return; // start-of-buffer: nothing changed
  _docVersion = _buf.docVersion();
  _rowBoundsCache.lineIdx = SIZE_MAX;
  _pointColCache.line = SIZE_MAX;

  // Adjust mark.
  if (_mark) {
    if (beforeOff > 0) {
      auto delBytes = beforeOff - _point.offset;
      if (_mark->line == beforeLine && _mark->offset > _point.offset) {
        if (_mark->offset >= _point.offset + delBytes)
          _mark->offset -= delBytes;
        else
          _mark->offset = _point.offset;
      }
    } else if (beforeLine > 0) {
      if (_mark->line == beforeLine) {
        _mark->line = _point.line;
        _mark->offset += _point.offset;
      } else if (_mark->line > beforeLine) {
        _mark->line--;
      }
    }
  }

  int shift = static_cast<int>(_buf.lineCount()) - static_cast<int>(lineCountBefore);
  _buf.recordVersion(_point, _point.line, shift);
}

void BufferView::deleteChunkAtPoint() {
  if (_buf.readonly())
    return;
  if (_docVersion != _buf.docVersion())
    clampToBuffer();
  if (atEnd())
    return;

  auto curIsBlank = [&]() -> bool {
    auto const& line = _buf.lineContent(_point.line);
    if (_point.offset < line.size())
      return isWhitespace(line[_point.offset]);
    return true;
  };

  bool startBlank = curIsBlank();
  do {
    deleteAtPoint();
  } while (!atEnd() && curIsBlank() == startBlank);
}

void BufferView::deleteChunkBeforePoint() {
  if (_buf.readonly())
    return;
  if (_docVersion != _buf.docVersion())
    clampToBuffer();
  if (_point.line == 0 && _point.offset == 0)
    return;

  auto prevIsBlank = [&]() -> bool {
    if (_point.offset > 0) {
      auto const& line = _buf.lineContent(_point.line);
      std::size_t pos = _point.offset - 1;
      while (pos > 0 && isUtf8Continuation(static_cast<unsigned char>(line[pos])))
        --pos;
      return isWhitespace(line[pos]);
    }
    return true;
  };

  bool startBlank = prevIsBlank();
  do {
    deleteBeforePoint();
  } while ((_point.line > 0 || _point.offset > 0) && prevIsBlank() == startBlank);
}

void BufferView::deleteLine() {
  if (_buf.readonly())
    return;
  if (_docVersion != _buf.docVersion())
    clampToBuffer();

  std::size_t lc = _buf.lineCount();
  std::size_t li = _point.line;
  // Delete the entire physical line, including its trailing newline.
  // The single-line case (only line in the buffer) collapses to an empty
  // line by deleting just the line's content.
  BufferPoint from{li, 0};
  BufferPoint to;
  if (li + 1 < lc) {
    to = {li + 1, 0};
  } else if (li > 0) {
    // Last line, but not the only line: also remove the preceding newline
    // by extending the deletion back from the end of the previous line.
    from = {li - 1, _buf.lineContent(li - 1).size()};
    to = {li, _buf.lineContent(li).size()};
  } else {
    // Only line in the buffer: just clear its content.
    to = {li, _buf.lineContent(li).size()};
  }
  deleteRegion(from, to);
}

void BufferView::toggleComment(std::string_view prefix) {
  if (_buf.readonly() || prefix.empty())
    return;
  if (_docVersion != _buf.docVersion())
    clampToBuffer();

  // Determine line range from the mark/point region. The region is half-
  // open in column terms but we operate on whole lines: if the user
  // selected (5,0)..(7,0), we comment only lines 5..6 (the trailing line
  // is just where the cursor sits, not part of the selection).
  std::size_t fromLine = _point.line;
  std::size_t toLine = _point.line;
  if (_mark) {
    auto a = std::min(*_mark, _point);
    auto b = std::max(*_mark, _point);
    fromLine = a.line;
    toLine = b.line;
    if (toLine > fromLine && b.offset == 0)
      --toLine;
  }
  toLine = std::min(toLine, _buf.lineCount() - 1);

  auto leadingWs = [](std::string const& l) -> std::size_t {
    std::size_t i = 0;
    while (i < l.size() && (l[i] == ' ' || l[i] == '\t'))
      ++i;
    return i;
  };

  // Check whether every non-blank line in the range already starts with
  // `prefix` (after leading whitespace). If yes, this is an "uncomment"
  // operation; otherwise it's a "comment" operation.
  bool allCommented = true;
  bool anyNonBlank = false;
  for (std::size_t li = fromLine; li <= toLine; ++li) {
    auto const& line = _buf.lineContent(li);
    auto ws = leadingWs(line);
    if (ws == line.size())
      continue; // blank line, ignore
    anyNonBlank = true;
    if (line.compare(ws, prefix.size(), prefix) != 0) {
      allCommented = false;
      break;
    }
  }
  if (!anyNonBlank)
    return;

  for (std::size_t li = fromLine; li <= toLine; ++li) {
    auto const& line = _buf.lineContent(li);
    auto ws = leadingWs(line);
    if (ws == line.size())
      continue;
    if (allCommented) {
      // Remove the prefix at the first non-whitespace position.
      _buf.deleteRegion({li, ws}, {li, ws + prefix.size()});
    } else {
      // Insert at the first non-whitespace position so the indent is
      // preserved and the prefix sits flush with the code.
      _buf.insertText(prefix, {li, ws});
    }
  }
  // Operating on whole lines may have moved the column-relative cursor;
  // pick it up by clamping. Mark stays valid relative to the new content.
  _docVersion = _buf.docVersion();
  if (_point.line >= _buf.lineCount())
    _point.line = _buf.lineCount() - 1;
  if (_point.offset > _buf.lineContent(_point.line).size())
    _point.offset = _buf.lineContent(_point.line).size();
  _rowBoundsCache.lineIdx = SIZE_MAX;
  _pointColCache.line = SIZE_MAX;
  _buf.recordVersion(_point, fromLine, 0);
}

namespace {

// A "word" byte for case operations: ASCII alphanumerics plus any
// non-ASCII byte (>= 0x80), so multi-byte UTF-8 sequences stay within a
// word and are never split. ASCII whitespace and punctuation act as word
// boundaries.
bool isCaseWordByte(unsigned char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c >= 0x80;
}

// ASCII-only case transform. Bytes outside A-Z/a-z (including every UTF-8
// lead/continuation byte and all whitespace/newlines) are left untouched,
// so the result is byte-length identical to the input.
std::string applyCaseOp(BufferView::CaseOp op, std::string_view text) {
  auto upper = [](char& ch) {
    if (ch >= 'a' && ch <= 'z')
      ch -= 'a' - 'A';
  };
  auto lower = [](char& ch) {
    if (ch >= 'A' && ch <= 'Z')
      ch += 'a' - 'A';
  };
  std::string r(text);
  switch (op) {
  case BufferView::CaseOp::Upper:
    for (auto& ch : r)
      upper(ch);
    break;
  case BufferView::CaseOp::Lower:
    for (auto& ch : r)
      lower(ch);
    break;
  case BufferView::CaseOp::Capitalize: {
    bool inWord = false;
    for (auto& ch : r) {
      bool alpha = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
      if (alpha) {
        inWord ? lower(ch) : upper(ch);
        inWord = true;
      } else {
        inWord = false;
      }
    }
    break;
  }
  }
  return r;
}

} // namespace

void BufferView::convertCase(CaseOp op) {
  if (_buf.readonly())
    return;
  if (_docVersion != _buf.docVersion())
    clampToBuffer();

  bool hadMark = _mark.has_value();
  BufferPoint from, to;
  if (hadMark) {
    from = std::min(*_mark, _point);
    to = std::max(*_mark, _point);
  } else {
    // No mark: operate on the word at, or the next word to the right of,
    // the cursor on the current line.
    auto const& line = _buf.lineContent(_point.line);
    std::size_t off = _point.offset;
    while (off < line.size() && !isCaseWordByte(static_cast<unsigned char>(line[off])))
      ++off;
    if (off >= line.size())
      return; // no word at or after the cursor on this line
    std::size_t start = off;
    while (start > 0 && isCaseWordByte(static_cast<unsigned char>(line[start - 1])))
      --start;
    std::size_t end = off;
    while (end < line.size() && isCaseWordByte(static_cast<unsigned char>(line[end])))
      ++end;
    from = {_point.line, start};
    to = {_point.line, end};
  }

  std::string text = _buf.extractRegion(from, to);
  std::string converted = applyCaseOp(op, text);

  if (converted != text) {
    // Rewrite as a single edit so undo restores in one step. The
    // replacement is byte-length identical, so mark/point offsets and the
    // line count are unchanged.
    _buf.deleteRegion(from, to);
    _buf.insertText(converted, from);
    _docVersion = _buf.docVersion();
    _rowBoundsCache.lineIdx = SIZE_MAX;
    _pointColCache.line = SIZE_MAX;
    _buf.recordVersion(hadMark ? _point : to, from.line, 0);
  }

  // A word operation advances the cursor past the word (even when the
  // text was already in the target case) so repeated presses march on.
  if (!hadMark) {
    _point = to;
    _desiredCol.reset();
  }
}

void BufferView::reflowParagraph(std::size_t targetWidth) {
  if (_buf.readonly() || targetWidth == 0)
    return;
  if (_docVersion != _buf.docVersion())
    clampToBuffer();

  // Determine line range. With an active mark, use the lines spanned by
  // the region (whole lines, matching toggleComment's convention). Else
  // grow outward from the cursor to the surrounding non-blank block.
  std::size_t fromLine, toLine;
  if (_mark) {
    auto a = std::min(*_mark, _point);
    auto b = std::max(*_mark, _point);
    fromLine = a.line;
    toLine = b.line;
    if (toLine > fromLine && b.offset == 0)
      --toLine;
  } else {
    if (_buf.isBlankLine(_point.line))
      return;
    fromLine = _point.line;
    while (fromLine > 0 && !_buf.isBlankLine(fromLine - 1))
      --fromLine;
    toLine = _point.line;
    while (toLine + 1 < _buf.lineCount() && !_buf.isBlankLine(toLine + 1))
      ++toLine;
  }
  if (fromLine > toLine)
    return;

  // Collect non-blank lines in the range. Blank lines inside an
  // explicit selection are preserved as paragraph breaks: we reflow
  // each contiguous non-blank run independently and re-emit blanks
  // between them. For the no-mark case there are no internal blanks
  // by construction.
  struct Block {
    std::size_t firstLine;
    std::vector<std::string> lines;
  };
  std::vector<Block> blocks;
  std::size_t i = fromLine;
  while (i <= toLine) {
    while (i <= toLine && _buf.isBlankLine(i))
      ++i;
    if (i > toLine)
      break;
    Block b{i, {}};
    while (i <= toLine && !_buf.isBlankLine(i)) {
      b.lines.emplace_back(_buf.lineContent(i));
      ++i;
    }
    blocks.push_back(std::move(b));
  }
  if (blocks.empty())
    return;

  auto leadingWs = [](std::string const& s) -> std::size_t {
    std::size_t k = 0;
    while (k < s.size() && (s[k] == ' ' || s[k] == '\t'))
      ++k;
    return k;
  };

  // Detect the longest leading whitespace common to every line in a
  // block. Used as the per-line prefix for that block's reflow output.
  auto commonLeadingWs = [&](Block const& b) -> std::string {
    auto& first = b.lines.front();
    std::size_t maxLen = leadingWs(first);
    std::string candidate = first.substr(0, maxLen);
    for (std::size_t j = 1; j < b.lines.size(); ++j) {
      auto& l = b.lines[j];
      std::size_t k = 0;
      while (k < candidate.size() && k < l.size() && l[k] == candidate[k])
        ++k;
      candidate.resize(k);
      if (candidate.empty())
        break;
    }
    return candidate;
  };

  // Detect a list bullet at offset `off` of `line`. Returns the bullet
  // length (text + trailing whitespace) on match, 0 otherwise.
  auto bulletLen = [](std::string const& line, std::size_t off) -> std::size_t {
    if (off >= line.size())
      return 0;
    char c = line[off];
    if (c == '-' || c == '*' || c == '+') {
      if (off + 1 < line.size() && (line[off + 1] == ' ' || line[off + 1] == '\t')) {
        std::size_t k = off + 2;
        while (k < line.size() && (line[k] == ' ' || line[k] == '\t'))
          ++k;
        return k - off;
      }
      return 0;
    }
    if (c >= '0' && c <= '9') {
      std::size_t k = off;
      while (k < line.size() && line[k] >= '0' && line[k] <= '9')
        ++k;
      if (k < line.size() && (line[k] == '.' || line[k] == ')')) {
        ++k;
        if (k < line.size() && (line[k] == ' ' || line[k] == '\t')) {
          while (k < line.size() && (line[k] == ' ' || line[k] == '\t'))
            ++k;
          return k - off;
        }
      }
    }
    return 0;
  };

  // Reflow one block into a vector of output lines. Caller stitches
  // them together with '\n' and the inter-block blank rows.
  auto reflowBlock = [&](Block const& b) -> std::vector<std::string> {
    std::string commonPrefix = commonLeadingWs(b);
    std::string firstPrefix = commonPrefix;
    std::string contPrefix = commonPrefix;
    auto const& first = b.lines.front();
    std::size_t bl = bulletLen(first, commonPrefix.size());
    std::string firstWordsStart = first.substr(commonPrefix.size());
    if (bl > 0) {
      // Preserve the bullet as the literal start of the first output
      // line; subsequent lines get the same width filled with spaces so
      // wrapped text aligns under the bullet's first non-bullet column.
      firstPrefix = commonPrefix + first.substr(commonPrefix.size(), bl);
      contPrefix = commonPrefix + std::string(bl, ' ');
      firstWordsStart = first.substr(commonPrefix.size() + bl);
    }

    // Tokenise into words. Whitespace runs (ASCII) are separators; UTF-8
    // continuation bytes are part of the preceding word.
    std::vector<std::string> words;
    auto pushWord = [&](std::string_view src) {
      std::size_t k = 0;
      while (k < src.size()) {
        while (k < src.size() && (src[k] == ' ' || src[k] == '\t'))
          ++k;
        if (k == src.size())
          break;
        std::size_t start = k;
        while (k < src.size() && src[k] != ' ' && src[k] != '\t')
          ++k;
        words.emplace_back(src.substr(start, k - start));
      }
    };
    pushWord(firstWordsStart);
    for (std::size_t j = 1; j < b.lines.size(); ++j) {
      auto const& l = b.lines[j];
      // Strip the line's own leading whitespace, not commonPrefix —
      // commonPrefix may be shorter than this line's actual indent.
      pushWord(std::string_view(l).substr(leadingWs(l)));
    }
    if (words.empty())
      return {firstPrefix};

    std::vector<std::string> out;
    auto width = [](std::string_view s) { return visualWidth(s); };

    bool firstLine = true;
    std::string current = firstPrefix;
    std::size_t curWidth = width(current);
    std::size_t budget = targetWidth;
    for (auto& w : words) {
      std::size_t ww = width(w);
      bool isFirstWordOnLine =
          (firstLine && current == firstPrefix) || (!firstLine && current == contPrefix);
      std::size_t addedWidth = isFirstWordOnLine ? ww : 1 + ww;
      if (!isFirstWordOnLine && curWidth + addedWidth > budget) {
        out.push_back(std::move(current));
        firstLine = false;
        current = contPrefix;
        curWidth = width(current);
        addedWidth = ww;
        isFirstWordOnLine = true;
      }
      if (!isFirstWordOnLine)
        current += ' ';
      current += w;
      curWidth += addedWidth;
    }
    if (!current.empty())
      out.push_back(std::move(current));
    return out;
  };

  // Inside a paragraph (a "block"), split on lines that start with a
  // bullet so each list item reflows independently. Without this a
  // four-line list would be merged into one long flow because the
  // outer reflow detects the bullet only on the very first input
  // line. Continuation lines under a bullet (no bullet of their own)
  // stay attached to that bullet's sub-block.
  auto splitListItems = [&](Block const& b) -> std::vector<Block> {
    std::vector<Block> subs;
    Block current{b.firstLine, {}};
    for (std::size_t j = 0; j < b.lines.size(); ++j) {
      auto const& l = b.lines[j];
      auto ws = leadingWs(l);
      bool isBullet = bulletLen(l, ws) > 0;
      if (isBullet && !current.lines.empty()) {
        subs.push_back(std::move(current));
        current = Block{b.firstLine + j, {}};
      }
      current.lines.push_back(l);
    }
    if (!current.lines.empty())
      subs.push_back(std::move(current));
    return subs;
  };

  // Build the replacement text for [fromLine, toLine] inclusive.
  // Blank lines separate paragraph-level blocks; list items inside a
  // block are emitted back-to-back with no blank between them.
  std::string replacement;
  for (std::size_t bi = 0; bi < blocks.size(); ++bi) {
    if (bi > 0)
      replacement += '\n';
    auto subs = splitListItems(blocks[bi]);
    for (std::size_t si = 0; si < subs.size(); ++si) {
      if (si > 0)
        replacement += '\n';
      auto outLines = reflowBlock(subs[si]);
      for (std::size_t k = 0; k < outLines.size(); ++k) {
        if (k > 0)
          replacement += '\n';
        replacement += outLines[k];
      }
    }
  }

  // Apply the rewrite as a single edit so undo restores in one step.
  BufferPoint regionStart{fromLine, 0};
  BufferPoint regionEnd{toLine, _buf.lineContent(toLine).size()};
  auto lineCountBefore = _buf.lineCount();
  _buf.deleteRegion(regionStart, regionEnd);
  auto after = _buf.insertText(replacement, regionStart);
  _docVersion = _buf.docVersion();
  _point = after;
  _mark.reset();
  _rowBoundsCache.lineIdx = SIZE_MAX;
  _pointColCache.line = SIZE_MAX;
  _desiredCol.reset();
  int shift = static_cast<int>(_buf.lineCount()) - static_cast<int>(lineCountBefore);
  _buf.recordVersion(_point, fromLine, shift);
}

void BufferView::adjustCursorToIndent() {
  if (_docVersion != _buf.docVersion())
    clampToBuffer();
  auto const& content = _buf.lineContent(_point.line);
  std::size_t firstNonSpace = 0;
  while (firstNonSpace < content.size() && content[firstNonSpace] == ' ')
    ++firstNonSpace;
  if (_point.offset < firstNonSpace)
    setPoint(_point.line, firstNonSpace);
}

std::string BufferView::extractRegion(BufferPoint from, BufferPoint to) const {
  return _buf.extractRegion(from, to);
}

void BufferView::deleteRegion(BufferPoint from, BufferPoint to) {
  if (_buf.readonly())
    return;
  if (_docVersion != _buf.docVersion())
    clampToBuffer();

  auto lineCountBefore = _buf.lineCount();
  auto regionStart = (to < from) ? to : from;

  auto oldVersion = _buf.docVersion();
  _buf.deleteRegion(from, to);
  if (_buf.docVersion() == oldVersion)
    return; // No-op: don't move point or clear mark.

  _point = regionStart;
  if (_point.line >= _buf.lineCount())
    _point.line = _buf.lineCount() - 1;
  if (_point.offset > _buf.lineContent(_point.line).size())
    _point.offset = _buf.lineContent(_point.line).size();
  _mark.reset();
  _docVersion = _buf.docVersion();
  _rowBoundsCache.lineIdx = SIZE_MAX;
  _pointColCache.line = SIZE_MAX;
  _desiredCol.reset();

  int shift = static_cast<int>(_buf.lineCount()) - static_cast<int>(lineCountBefore);
  _buf.recordVersion(_point, regionStart.line, shift);
}

BufferPoint BufferView::insertText(std::string_view text) {
  if (_buf.readonly())
    return _point;
  if (_docVersion != _buf.docVersion())
    clampToBuffer();

  auto startLine = _point.line;
  auto startOff = _point.offset;
  auto lineCountBefore = _buf.lineCount();
  _point = _buf.insertText(text, _point);
  _docVersion = _buf.docVersion();
  _rowBoundsCache.lineIdx = SIZE_MAX;
  _pointColCache.line = SIZE_MAX;

  // Adjust mark.
  if (_mark) {
    auto linesAdded = _point.line - startLine;
    if (_mark->line == startLine && _mark->offset >= startOff) {
      _mark->line += linesAdded;
      if (linesAdded > 0)
        _mark->offset = _mark->offset - startOff + _point.offset;
      else
        _mark->offset += _point.offset - startOff;
    } else if (_mark->line > startLine) {
      _mark->line += linesAdded;
    }
  }

  int shift = static_cast<int>(_buf.lineCount()) - static_cast<int>(lineCountBefore);
  _buf.recordVersion(_point, startLine, shift);
  return _point;
}

// --- History mode ---

void BufferView::enterHistoryMode() {
  auto info = _buf.versionHistoryInfo();
  if (info.empty)
    return;
  _buf.setBrowsingHistory(true);
  _historyIndex = info.size - 1;
  // Match stepHistory(): the mark cannot meaningfully refer to a position
  // in a snapshot that may differ from the current contents.
  _mark.reset();
  _desiredCol.reset();
  _rowBoundsCache.lineIdx = SIZE_MAX;
  _pointColCache.line = SIZE_MAX;
}

void BufferView::stepHistory(int delta) {
  if (!_historyIndex)
    return;
  auto info = _buf.versionHistoryInfo();
  long long ni = static_cast<long long>(*_historyIndex) + delta;
  if (ni < 0)
    ni = 0;
  if (ni >= static_cast<long long>(info.size))
    ni = static_cast<long long>(info.size) - 1;
  auto newIdx = static_cast<std::size_t>(ni);
  if (newIdx == *_historyIndex)
    return;
  _historyIndex = newIdx;
  _buf.applyVersionAt(newIdx, _point);
  _docVersion = _buf.docVersion();
  if (_point.line >= _buf.lineCount())
    _point.line = _buf.lineCount() - 1;
  if (_point.offset > _buf.lineContent(_point.line).size())
    _point.offset = _buf.lineContent(_point.line).size();
  _mark.reset();
  _desiredCol.reset();
  _rowBoundsCache.lineIdx = SIZE_MAX;
  _pointColCache.line = SIZE_MAX;
}

void BufferView::exitHistoryModeCommit() {
  if (!_historyIndex)
    return;
  _buf.truncateHistoryAfter(*_historyIndex);
  _buf.setBrowsingHistory(false);
  _historyIndex.reset();
}

// --- Rendering ---

void BufferView::ensureVisible(RowIdx rows, ColIdx cols, bool wrap) {
  if (_docVersion != _buf.docVersion())
    clampToBuffer();

  // Nothing to do (and avoid a modulo-by-zero in the ring buffer below) when
  // the panel has no content rows. This happens on narrow terminal splits.
  if (rows.value == 0)
    return;

  // Reset col offset when switching wrap mode, to avoid stale scroll state.
  if (_wrapMode != wrap) {
    _wrapMode = wrap;
    _viewFrom.resetCol(wrap);
  }
  // Ensure the variant holds the correct type for the current mode.
  if (wrap && !std::holds_alternative<ByteOffset>(_viewFrom.col))
    _viewFrom.col = ByteOffset{0};
  else if (!wrap && !std::holds_alternative<VisualCol>(_viewFrom.col))
    _viewFrom.col = VisualCol{0};

  // Content width excludes both gutters.
  std::size_t cw = contentCols(cols);

  if (!wrap) {
    auto& hs = _viewFrom.hScroll();

    // Vertical: simple line-based scrolling.
    if (_point.line < _viewFrom.line.value)
      _viewFrom.line = RowIdx{_point.line};
    if (_point.line >= _viewFrom.line.value + rows.value)
      _viewFrom.line = RowIdx{_point.line - rows.value + 1};

    // Horizontal: keep cursor within visible columns.
    std::size_t vcol = pointVisualCol();
    if (vcol < hs)
      hs = vcol;
    if (vcol >= hs + cw)
      hs = vcol - cw + 1;
    return;
  }

  auto& vfo = _viewFrom.byteOffset();

  // Handle point above the viewport (including sub-line offset).
  if (_point < viewFrom()) {
    _viewFrom.line = RowIdx{_point.line};
    vfo = 0;
  }

  // Handle point far below the viewport (line-level jump). Strict `>`
  // rather than `>=`: when point.line equals viewFrom.line + rows
  // (cursor exactly one visual row below the viewport, the natural
  // outcome of a single Down keypress when content above is unwrapped),
  // we want a smooth one-row scroll, not a jump-to-centre. The forward
  // ring-buffer scan below handles that case correctly.
  if (_point.line > _viewFrom.line.value + rows.value) {
    std::size_t half = rows.value / 2;
    _viewFrom.line = RowIdx{_point.line >= half ? _point.line - half : 0};
    vfo = 0;
  }

  // Single forward scan from the viewport to the point, counting visual
  // rows. We maintain a ring buffer of the last `rows` viewport-start
  // candidates {line, byteOffset}. When the scan finishes, the oldest
  // entry in the ring is the viewport position that places the cursor on
  // the last visible screen row.
  //
  // This replaces the previous while-loop that called screenRow() on each
  // iteration, avoiding the O(N^2) cost of re-scanning from the viewport
  // on every line advance.

  std::size_t ringSize = rows.value;
  // Reuse the scratch member; assign rather than reallocate to keep
  // capacity stable across calls (so cursor moves don't touch the heap).
  _ensureVisibleRing.assign(ringSize, {_viewFrom.line.value, vfo});
  auto& ring = _ensureVisibleRing;
  std::size_t ringPos = 0;
  std::size_t visRow = 0;

  auto pushRing = [&](std::size_t line, std::size_t off) {
    ringPos = (ringPos + 1) % ringSize;
    ring[ringPos] = {line, off};
  };

  for (std::size_t lineIdx = _viewFrom.line.value;
       lineIdx <= _point.line && lineIdx < _buf.lineCount(); ++lineIdx) {
    auto const& line = _buf.lineContent(lineIdx);
    std::size_t end = (lineIdx == _point.line) ? _point.offset : line.size();
    std::size_t start = (lineIdx == _viewFrom.line.value) ? vfo : std::size_t{0};
    std::size_t col = 0;

    for (std::size_t i = start; i < end && i < line.size();) {
      auto info = charInfoAt(line.data() + i, line.size() - i);
      if (col + info.width > cw) {
        ++visRow;
        pushRing(lineIdx, i);
        col = 0;
      }
      col += info.width;
      if (col >= cw) {
        ++visRow;
        pushRing(lineIdx, i + info.len);
        col = 0;
      }
      i += info.len;
    }
    if (lineIdx < _point.line) {
      ++visRow;
      pushRing(lineIdx + 1, 0);
    }
  }

  if (visRow < rows.value)
    return; // point is already on screen

  std::size_t readIdx = (ringPos + 1) % ringSize;
  _viewFrom.line = RowIdx{ring[readIdx].line};
  vfo = ring[readIdx].byteOff;
}

std::tuple<RowIdx, ColIdx> BufferView::render(std::string& out, RowIdx rows, ColIdx cols,
                                              BufferRenderArgs const& args) const {
  if (_docVersion != _buf.docVersion())
    const_cast<BufferView*>(this)->clampToBuffer();
  RenderParams params;
  params.point = _point;
  params.viewFrom = _viewFrom;
  params.mark = _mark;
  params.highlight = _highlight;
  params.showWhitespace = _showWhitespace;
  params.wrap = _wrap;
  params.highlightCurrentLine = _highlightCurrentLine;
  params.leftGutter = lineNumberGutterWidth();
  params.focused = args.focused;
  params.rowOffset = args.rowOffset;
  params.colOffset = args.colOffset;
  return _buf.render(out, rows, cols, params);
}

std::size_t BufferView::screenRow(ColIdx cols, bool wrap) const {
  // If the line-number gutter is on, the wrap math inside Buffer::screenRow
  // needs to know about the narrower content area. Pass a reduced cols so
  // its `contentWidth(cols)` matches our own contentCols(cols).
  std::size_t lng = lineNumberGutterWidth();
  ColIdx adjustedCols{cols.value > lng ? cols.value - lng : cols.value};
  // Fall through to the buffer's screenRow with the (possibly) reduced
  // cols so its contentWidth() matches our content area.
  if (_docVersion != _buf.docVersion())
    const_cast<BufferView*>(this)->clampToBuffer();
  return _buf.screenRow(adjustedCols, _point, _viewFrom, wrap);
}

bool BufferView::currentLineWraps(ColIdx cols) const {
  if (_docVersion != _buf.docVersion())
    const_cast<BufferView*>(this)->clampToBuffer();
  std::size_t cw = contentCols(cols);
  // Reuse the row-bounds cache when it covers the current line at the
  // current width: a single row means the line fits in one visual row;
  // any more means it wraps. Falls back to a full visual-width scan
  // when the cache holds bounds for a different line or width.
  if (_rowBoundsCache.lineIdx == _point.line && _rowBoundsCache.cols == cw)
    return _rowBoundsCache.rowStarts.size() > 1;
  return visualColRange(_buf.lineContent(_point.line), 0, _buf.lineContent(_point.line).size()) >
         cw;
}
