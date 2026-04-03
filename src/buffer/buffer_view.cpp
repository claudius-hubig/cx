#include "buffer_view.hpp"

#include <algorithm>

BufferView::BufferView(Buffer& buf) : _buf(buf), _docVersion(buf._docVersion) {}

BufferView BufferView::fromBufferState(Buffer& buf) {
  BufferView v(buf);
  v._point = buf._point;
  v._mark = buf._mark;
  v._viewFrom = buf._viewFrom;
  v._wrapMode = buf._wrapMode;
  v._desiredCol = buf._desiredCol;
  v._highlight = buf._highlight;
  v._pointColCache = buf._pointColCache;
  v._rowBoundsCache.lineIdx = buf._rowBoundsCache.lineIdx;
  v._rowBoundsCache.cols = buf._rowBoundsCache.cols;
  v._rowBoundsCache.rowStarts = buf._rowBoundsCache.rowStarts;
  v._docVersion = buf._docVersion;
  return v;
}

void BufferView::syncTo() const {
  // If the document changed under us, clamp first.
  if (_docVersion != _buf._docVersion)
    const_cast<BufferView*>(this)->clampToBuffer();

  _buf._point = _point;
  _buf._mark = _mark;
  _buf._viewFrom = _viewFrom;
  _buf._wrapMode = _wrapMode;
  _buf._desiredCol = _desiredCol;
  _buf._highlight = _highlight;
  _buf._pointColCache = _pointColCache;
  _buf._rowBoundsCache.lineIdx = _rowBoundsCache.lineIdx;
  _buf._rowBoundsCache.cols = _rowBoundsCache.cols;
  _buf._rowBoundsCache.rowStarts = std::move(_rowBoundsCache.rowStarts);
}

void BufferView::syncFrom() {
  _point = _buf._point;
  _mark = _buf._mark;
  _viewFrom = _buf._viewFrom;
  _wrapMode = _buf._wrapMode;
  _desiredCol = _buf._desiredCol;
  _highlight = _buf._highlight;
  _pointColCache = _buf._pointColCache;
  _rowBoundsCache.lineIdx = _buf._rowBoundsCache.lineIdx;
  _rowBoundsCache.cols = _buf._rowBoundsCache.cols;
  _rowBoundsCache.rowStarts = std::move(_buf._rowBoundsCache.rowStarts);
  _docVersion = _buf._docVersion;
}

void BufferView::clampToBuffer() {
  _docVersion = _buf._docVersion;
  // Clamp point.
  if (_point.line >= _buf._lines.size())
    _point.line = _buf._lines.size() - 1;
  if (_point.offset > _buf._lines[_point.line].size())
    _point.offset = _buf._lines[_point.line].size();
  // Clamp mark.
  if (_mark) {
    if (_mark->line >= _buf._lines.size())
      _mark->line = _buf._lines.size() - 1;
    if (_mark->offset > _buf._lines[_mark->line].size())
      _mark->offset = _buf._lines[_mark->line].size();
  }
  // Clamp viewport.
  if (_viewFrom.line.value >= _buf._lines.size())
    _viewFrom.line = RowIdx{_buf._lines.size() - 1};
  // Invalidate per-view caches.
  _pointColCache.line = SIZE_MAX;
  _rowBoundsCache.lineIdx = SIZE_MAX;
}

void BufferView::saveToBuffer() {
  syncTo();
  // syncTo already wrote everything to Buffer's fields. The buffer's
  // _rowBoundsCache was moved, so restore our copy from the buffer
  // (which now has the moved data... actually syncTo moved it out).
  // Just sync back to keep both sides consistent.
  syncFrom();
}

void BufferView::loadFromBuffer() { syncFrom(); }

// --- View state accessors ---

BufferPoint BufferView::viewFrom() const {
  syncTo();
  auto result = _buf.viewFrom();
  const_cast<BufferView*>(this)->syncFrom();
  return result;
}

void BufferView::setPoint(std::size_t line, std::size_t offset) {
  syncTo();
  _buf.setPoint(line, offset);
  syncFrom();
}

void BufferView::toggleMark() {
  if (_mark)
    _mark.reset();
  else
    _mark = _point;
}

std::tuple<std::size_t, std::size_t> BufferView::lineCol() const {
  syncTo();
  auto result = _buf.lineCol();
  const_cast<BufferView*>(this)->syncFrom();
  return result;
}

// --- Movement ---

void BufferView::moveLeft() {
  syncTo();
  _buf.moveLeft();
  syncFrom();
}

void BufferView::moveRight() {
  syncTo();
  _buf.moveRight();
  syncFrom();
}

void BufferView::moveUp(ColIdx cols) {
  syncTo();
  _buf.moveUp(cols);
  syncFrom();
}

void BufferView::moveDown(ColIdx cols) {
  syncTo();
  _buf.moveDown(cols);
  syncFrom();
}

void BufferView::movePhysicalUp() {
  syncTo();
  _buf.movePhysicalUp();
  syncFrom();
}

void BufferView::movePhysicalDown() {
  syncTo();
  _buf.movePhysicalDown();
  syncFrom();
}

void BufferView::moveHome() {
  syncTo();
  _buf.moveHome();
  syncFrom();
}

void BufferView::moveEnd() {
  syncTo();
  _buf.moveEnd();
  syncFrom();
}

void BufferView::moveWordRight() {
  syncTo();
  _buf.moveWordRight();
  syncFrom();
}

void BufferView::moveWordLeft() {
  syncTo();
  _buf.moveWordLeft();
  syncFrom();
}

void BufferView::moveParagraphUp() {
  syncTo();
  _buf.moveParagraphUp();
  syncFrom();
}

void BufferView::moveParagraphDown() {
  syncTo();
  _buf.moveParagraphDown();
  syncFrom();
}

void BufferView::pageUp(RowIdx screenRows, ColIdx cols, bool wrap) {
  syncTo();
  _buf.pageUp(screenRows, cols, wrap);
  syncFrom();
}

void BufferView::pageDown(RowIdx screenRows, ColIdx cols, bool wrap) {
  syncTo();
  _buf.pageDown(screenRows, cols, wrap);
  syncFrom();
}

bool BufferView::gotoMatchingBracket() {
  syncTo();
  bool result = _buf.gotoMatchingBracket();
  syncFrom();
  return result;
}

void BufferView::gotoLine(std::size_t line, RowIdx screenRows) {
  syncTo();
  _buf.gotoLine(line, screenRows);
  syncFrom();
}

void BufferView::scrollDown() {
  syncTo();
  _buf.scrollDown();
  syncFrom();
}

void BufferView::scrollUp(bool wrap) {
  syncTo();
  _buf.scrollUp(wrap);
  syncFrom();
}

void BufferView::scrollViewNearEnd(RowIdx screenRows) {
  syncTo();
  _buf.scrollViewNearEnd(screenRows);
  syncFrom();
}

// --- Editing ---

void BufferView::insert(uint32_t cp) {
  syncTo();
  _buf.insert(cp);
  syncFrom();
}

void BufferView::deleteAtPoint() {
  syncTo();
  _buf.deleteAtPoint();
  syncFrom();
}

void BufferView::deleteBeforePoint() {
  syncTo();
  _buf.deleteBeforePoint();
  syncFrom();
}

void BufferView::deleteChunkAtPoint() {
  syncTo();
  _buf.deleteChunkAtPoint();
  syncFrom();
}

void BufferView::deleteChunkBeforePoint() {
  syncTo();
  _buf.deleteChunkBeforePoint();
  syncFrom();
}

void BufferView::adjustCursorToIndent() {
  syncTo();
  _buf.adjustCursorToIndent();
  syncFrom();
}

std::string BufferView::extractRegion(BufferPoint from, BufferPoint to) const {
  // extractRegion is const and only reads _lines — no sync needed.
  return _buf.extractRegion(from, to);
}

void BufferView::deleteRegion(BufferPoint from, BufferPoint to) {
  syncTo();
  _buf.deleteRegion(from, to);
  syncFrom();
}

BufferPoint BufferView::insertText(std::string_view text) {
  syncTo();
  auto result = _buf.insertText(text);
  syncFrom();
  return result;
}

// --- Rendering ---

void BufferView::ensureVisible(RowIdx rows, ColIdx cols, bool wrap) {
  syncTo();
  _buf.ensureVisible(rows, cols, wrap);
  syncFrom();
}

std::tuple<RowIdx, ColIdx> BufferView::render(std::string& out, RowIdx rows, ColIdx cols,
                                              bool showWhitespace, bool wrap, std::size_t rowOffset,
                                              std::size_t colOffset) const {
  syncTo();
  auto result = _buf.render(out, rows, cols, showWhitespace, wrap, rowOffset, colOffset);
  const_cast<BufferView*>(this)->syncFrom();
  return result;
}

std::size_t BufferView::screenRow(ColIdx cols, bool wrap) const {
  syncTo();
  auto result = _buf.screenRow(cols, wrap);
  const_cast<BufferView*>(this)->syncFrom();
  return result;
}

bool BufferView::currentLineWraps(ColIdx cols) const {
  syncTo();
  auto result = _buf.currentLineWraps(cols);
  const_cast<BufferView*>(this)->syncFrom();
  return result;
}
