#include "buffer_view.hpp"

#include "modes/c/syntax.hpp"
#include "modes/jsn/syntax.hpp"
#include "types.hpp"
#include "utf8.hpp"

#include <algorithm>
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
    v._wrapMode = saved->wrapMode;
    buf.clearSavedView();
  }
  v._docVersion = buf.docVersion();
  return v;
}

void BufferView::clampToBuffer() {
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
  // Invalidate per-view caches.
  _pointColCache.line = SIZE_MAX;
  _rowBoundsCache.lineIdx = SIZE_MAX;
}

void BufferView::saveViewState() {
  std::optional<std::size_t> dc;
  if (_desiredCol)
    dc = _desiredCol->value;
  _buf.setSavedView({_point, _viewFrom, _mark, dc, _wrapMode});
}

// --- Private helpers ---

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
  // Content width excludes the gutter column.
  std::size_t cw = contentWidth(cols);
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
  // Content width excludes the gutter column.
  std::size_t cw = contentWidth(cols);
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

  // Advance to the last char of this word.
  {
    auto const& cur = _buf.lineContent(_point.line);
    while (_point.offset < cur.size()) {
      auto len = utf8SeqLen(static_cast<unsigned char>(cur[_point.offset]));
      auto next = std::min(_point.offset + len, cur.size());
      // If next position is end of line or a separator, stop here.
      if (next >= cur.size() || isSep(cur[next]))
        break;
      _point.offset = next;
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

  // If at top and still blank, go to start.
  if (_buf.isBlankLine(pos)) {
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
    _viewFrom.col = ByteOffset{0};
  }
}

bool BufferView::gotoMatchingBracket() {
  if (_docVersion != _buf.docVersion())
    clampToBuffer();
  auto match = (_buf.mode() == BufferMode::Jsn) ? findMatchingBracketJson(_buf.lines(), _point)
                                                : findMatchingBracket(_buf.lines(), _point);
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
  _viewFrom.col = ByteOffset{0};
}

void BufferView::scrollDown() {
  if (_docVersion != _buf.docVersion())
    clampToBuffer();
  if (_viewFrom.line.value + 1 < _buf.lineCount()) {
    _viewFrom.line = RowIdx{_viewFrom.line.value + 1};
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
    _viewFrom.col = ByteOffset{0};
  }
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
      if (m.line == insertLine && m.offset >= insertOff)
        m.offset += n;
    }
  }

  if (_desiredCol) {
    std::size_t vcol = pointVisualCol();
    if (vcol >= _desiredCol->value)
      _desiredCol.reset();
  }
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

  _point = _buf.deleteAt(_point);
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
}

void BufferView::deleteBeforePoint() {
  if (_buf.readonly())
    return;
  if (_docVersion != _buf.docVersion())
    clampToBuffer();

  auto beforeLine = _point.line;
  auto beforeOff = _point.offset;

  _point = _buf.deleteBefore(_point);
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

  auto oldVersion = _buf.docVersion();
  _buf.deleteRegion(from, to);
  if (_buf.docVersion() == oldVersion)
    return; // No-op: don't move point or clear mark.

  _point = (to < from) ? to : from;
  if (_point.line >= _buf.lineCount())
    _point.line = _buf.lineCount() - 1;
  if (_point.offset > _buf.lineContent(_point.line).size())
    _point.offset = _buf.lineContent(_point.line).size();
  _mark.reset();
  _docVersion = _buf.docVersion();
  _rowBoundsCache.lineIdx = SIZE_MAX;
  _pointColCache.line = SIZE_MAX;
}

BufferPoint BufferView::insertText(std::string_view text) {
  if (_buf.readonly())
    return _point;
  if (_docVersion != _buf.docVersion())
    clampToBuffer();

  auto startLine = _point.line;
  auto startOff = _point.offset;
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

  return _point;
}

// --- Rendering ---

void BufferView::ensureVisible(RowIdx rows, ColIdx cols, bool wrap) {
  if (_docVersion != _buf.docVersion())
    clampToBuffer();

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

  // Content width excludes the gutter column.
  std::size_t cw = contentWidth(cols);

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

  // Handle point far below the viewport (line-level jump).
  if (_point.line >= _viewFrom.line.value + rows.value) {
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

  struct ViewPos {
    std::size_t line;
    std::size_t byteOff;
  };
  std::size_t ringSize = rows.value;
  std::vector<ViewPos> ring(ringSize, {_viewFrom.line.value, vfo});
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
                                              bool showWhitespace, bool wrap, std::size_t rowOffset,
                                              std::size_t colOffset) const {
  if (_docVersion != _buf.docVersion())
    const_cast<BufferView*>(this)->clampToBuffer();
  RenderParams params;
  params.point = _point;
  params.viewFrom = _viewFrom;
  params.mark = _mark;
  params.highlight = _highlight;
  params.showWhitespace = showWhitespace;
  params.wrap = wrap;
  params.rowOffset = rowOffset;
  params.colOffset = colOffset;
  return _buf.render(out, rows, cols, params);
}

std::size_t BufferView::screenRow(ColIdx cols, bool wrap) const {
  if (_docVersion != _buf.docVersion())
    const_cast<BufferView*>(this)->clampToBuffer();
  return _buf.screenRow(cols, _point, _viewFrom, wrap);
}

bool BufferView::currentLineWraps(ColIdx cols) const {
  if (_docVersion != _buf.docVersion())
    const_cast<BufferView*>(this)->clampToBuffer();
  std::size_t cw = contentWidth(cols);
  return visualColRange(_buf.lineContent(_point.line), 0, _buf.lineContent(_point.line).size()) >
         cw;
}
