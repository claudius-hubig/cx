#include "line_store.hpp"

#include <algorithm>
#include <iterator>

LineStore::LineStore(std::size_t chunk) : _target(std::max<std::size_t>(chunk, 4)) {}

LineStore::LineStore(std::span<LinePtr const> lines, std::size_t chunk) : LineStore(chunk) {
  insert(0, lines);
}

LineStore::LineStore(std::vector<LinePtr>&& lines, std::size_t chunk) : LineStore(chunk) {
  if (lines.empty())
    return;
  _chunks.push_back(std::make_shared<Chunk>());
  reindex(0);
  rechunk(0, {lines});
}

LineStore::LineStore(LineStore&& o) noexcept
    : _chunks(std::move(o._chunks)), _starts(std::move(o._starts)),
      _size(std::exchange(o._size, 0)), _target(o._target), _hintData(o._hintData),
      _hintFirst(o._hintFirst), _hintCount(std::exchange(o._hintCount, 0)),
      _hintChunk(o._hintChunk) {
  o._chunks.clear();
  o._starts.clear();
}

LineStore& LineStore::operator=(LineStore&& o) noexcept {
  if (this != &o) {
    _chunks = std::move(o._chunks);
    _starts = std::move(o._starts);
    _size = std::exchange(o._size, 0);
    _target = o._target;
    _hintData = o._hintData;
    _hintFirst = o._hintFirst;
    _hintCount = std::exchange(o._hintCount, 0);
    _hintChunk = o._hintChunk;
    o._chunks.clear();
    o._starts.clear();
  }
  return *this;
}

LinePtr const& LineStore::lookupSlow(std::size_t i) const {
  auto holds = [&](std::size_t c) {
    return c < _starts.size() && _starts[c] <= i && i < chunkEnd(c);
  };
  // The cached chunk (after a mutation dropped it) or the next one
  // (sequential access); otherwise a binary search.
  auto c = _hintChunk;
  if (!holds(c) && !holds(++c))
    c = static_cast<std::size_t>(std::ranges::upper_bound(_starts, i) - _starts.begin()) - 1;
  _hintChunk = c;
  _hintData = _chunks[c]->data();
  _hintFirst = _starts[c];
  _hintCount = _chunks[c]->size();
  return _hintData[i - _hintFirst];
}

std::pair<std::size_t, std::size_t> LineStore::insertionPoint(std::size_t pos) const {
  if (pos == size())
    return {_chunks.size() - 1, _chunks.back()->size()};
  return locate(pos);
}

LineStore::Chunk& LineStore::mut(std::size_t c) {
  dropHint(); // the caller may reallocate the chunk's array
  if (_chunks[c].use_count() > 1)
    _chunks[c] = std::make_shared<Chunk>(*_chunks[c]);
  return *_chunks[c];
}

void LineStore::reindex(std::size_t from) {
  dropHint();
  // The chunks before `from` are unchanged, so their starts still hold.
  from = std::min(from, _chunks.size());
  _starts.resize(_chunks.size());
  for (auto c = from; c < _chunks.size(); ++c)
    _starts[c] = c == 0 ? 0 : _starts[c - 1] + _chunks[c - 1]->size();
  _size = _chunks.empty() ? 0 : _starts.back() + _chunks.back()->size();
}

void LineStore::rechunk(std::size_t c, std::initializer_list<std::span<LinePtr>> parts) {
  std::size_t n = 0;
  for (auto const& part : parts)
    n += part.size();
  // n / T pieces of [T, 2T) lines each, or one piece when it fits.
  auto pieces = n <= 2 * _target ? 1 : n / _target;
  std::vector<std::shared_ptr<Chunk>> made;
  made.reserve(pieces);
  auto part = parts.begin();
  auto it = part->begin();
  for (std::size_t p = 0; p < pieces; ++p) {
    auto len = n / pieces + (p < n % pieces ? 1 : 0);
    auto chunk = std::make_shared<Chunk>();
    chunk->reserve(len);
    while (chunk->size() < len) {
      while (it == part->end())
        it = (++part)->begin();
      auto k =
          std::min<std::size_t>(len - chunk->size(), static_cast<std::size_t>(part->end() - it));
      auto end = it + static_cast<std::ptrdiff_t>(k);
      chunk->insert(chunk->end(), std::make_move_iterator(it), std::make_move_iterator(end));
      it = end;
    }
    made.push_back(std::move(chunk));
  }
  _chunks.erase(_chunks.begin() + static_cast<std::ptrdiff_t>(c));
  _chunks.insert(_chunks.begin() + static_cast<std::ptrdiff_t>(c), made.begin(), made.end());
  reindex(c);
}

LineStore::Chunk LineStore::take(std::size_t c) {
  return _chunks[c].use_count() == 1 ? std::move(*_chunks[c]) : *_chunks[c];
}

void LineStore::rebalance(std::size_t c) {
  if (c >= _chunks.size())
    return;
  auto n = _chunks[c]->size();
  if (n == 0) {
    _chunks.erase(_chunks.begin() + static_cast<std::ptrdiff_t>(c));
    reindex(c);
    return;
  }
  if (n > 2 * _target) {
    auto all = take(c);
    rechunk(c, {all});
    return;
  }
  if (n >= _target / 4 || _chunks.size() == 1)
    return;
  // Too small: merge with the smaller neighbour (re-split if that overflows).
  auto last = _chunks.size() - 1;
  auto nb = c == 0                                             ? 1
            : c == last                                        ? c - 1
            : _chunks[c - 1]->size() <= _chunks[c + 1]->size() ? c - 1
                                                               : c + 1;
  auto lo = std::min(c, nb);
  Chunk merged = take(lo);
  Chunk const& hi = *_chunks[lo + 1];
  merged.insert(merged.end(), hi.begin(), hi.end());
  _chunks.erase(_chunks.begin() + static_cast<std::ptrdiff_t>(lo + 1));
  rechunk(lo, {merged});
}

void LineStore::replace(std::size_t i, LinePtr line) {
  auto [c, off] = locate(i);
  mut(c)[off] = std::move(line);
}

std::string& LineStore::mutableLine(std::size_t i) {
  auto [c, off] = locate(i);
  // Detach the chunk first: while it is shared, the LinePtrs in it look
  // uniquely held although a snapshot can still reach them.
  auto& p = mut(c)[off];
  if (p.use_count() > 1)
    p = makeLine(std::string_view(*p));
  // Safe: makeLine() created the string non-const.
  return const_cast<std::string&>(*p);
}

void LineStore::insert(std::size_t pos, LinePtr line) {
  if (_chunks.empty()) {
    push_back(std::move(line));
    return;
  }
  auto [c, off] = insertionPoint(pos);
  auto& ch = mut(c);
  ch.insert(ch.begin() + static_cast<std::ptrdiff_t>(off), std::move(line));
  reindex(c);
  rebalance(c);
}

void LineStore::insert(std::size_t pos, std::span<LinePtr const> lines) {
  insert(pos, std::vector<LinePtr>(lines.begin(), lines.end()));
}

void LineStore::insert(std::size_t pos, std::vector<LinePtr>&& lines) {
  if (lines.empty())
    return;
  if (_chunks.empty()) {
    _chunks.push_back(std::make_shared<Chunk>());
    reindex(0);
  }
  auto [c, off] = insertionPoint(pos);
  auto at = static_cast<std::ptrdiff_t>(off);
  auto moved = [&](auto it) { return std::make_move_iterator(it); };
  if (lines.size() <= _target) {
    auto& ch = mut(c);
    ch.insert(ch.begin() + at, moved(lines.begin()), moved(lines.end()));
    reindex(c);
    rebalance(c);
    return;
  }
  // Large insert: build whole chunks straight from the old chunk's head,
  // the new lines and its tail.
  auto old = take(c);
  std::span<LinePtr> all(old);
  rechunk(c, {all.first(off), lines, all.subspan(off)});
}

void LineStore::erase(std::size_t first, std::size_t last) {
  if (first >= last)
    return;
  auto [c1, o1] = locate(first);
  auto [c2, o2] = locate(last - 1);
  if (c1 == c2) {
    auto& ch = mut(c1);
    ch.erase(ch.begin() + static_cast<std::ptrdiff_t>(o1),
             ch.begin() + static_cast<std::ptrdiff_t>(o2 + 1));
    reindex(c1);
    rebalance(c1);
    return;
  }
  // Trim the two end chunks and drop the whole chunks between them.
  {
    auto& tail = mut(c2);
    tail.erase(tail.begin(), tail.begin() + static_cast<std::ptrdiff_t>(o2 + 1));
  }
  {
    auto& head = mut(c1);
    head.erase(head.begin() + static_cast<std::ptrdiff_t>(o1), head.end());
  }
  _chunks.erase(_chunks.begin() + static_cast<std::ptrdiff_t>(c1 + 1),
                _chunks.begin() + static_cast<std::ptrdiff_t>(c2));
  reindex(c1);
  // Either end may now be too small or empty; the right one first, since
  // merging it may touch c1 but not the other way round.
  rebalance(c1 + 1);
  rebalance(c1);
}

void LineStore::push_back(LinePtr line) {
  if (_chunks.empty() || _chunks.back()->size() >= _target) {
    _chunks.push_back(std::make_shared<Chunk>());
    _chunks.back()->reserve(_target);
    _starts.push_back(_size);
  }
  mut(_chunks.size() - 1).push_back(std::move(line));
  ++_size;
}

void LineStore::clear() {
  _chunks.clear();
  _starts.clear();
  _size = 0;
  dropHint();
}

bool LineStore::invariantsHold() const {
  if (_starts.size() != _chunks.size() || (!_starts.empty() && _starts[0] != 0))
    return false;
  if (_hintCount > 0 &&
      (_hintChunk >= _chunks.size() || _hintData != _chunks[_hintChunk]->data() ||
       _hintFirst != _starts[_hintChunk] || _hintCount != _chunks[_hintChunk]->size()))
    return false;
  std::size_t total = 0;
  for (std::size_t c = 0; c < _chunks.size(); ++c) {
    auto n = _chunks[c]->size();
    if (n == 0 || n > 2 * _target || _starts[c] != total)
      return false;
    total += n;
    // The last chunk may be small: push_back starts a fresh one when the
    // previous is full.
    if (c + 1 < _chunks.size() && n < _target / 4)
      return false;
  }
  return total == _size;
}
