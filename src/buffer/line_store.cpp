#include "line_store.hpp"

#include <algorithm>
#include <iterator>

LineStore::LineStore(std::size_t chunk) : _target(std::max<std::size_t>(chunk, 4)) {}

LineStore::LineStore(std::span<LinePtr const> lines, std::size_t chunk) : LineStore(chunk) {
  insert(0, lines);
}

std::pair<std::size_t, std::size_t> LineStore::locateSlow(std::size_t i) const {
  auto c = _hint + 1; // sequential access: the next chunk
  if (!(c + 1 < _starts.size() && _starts[c] <= i && i < _starts[c + 1]))
    c = static_cast<std::size_t>(std::ranges::upper_bound(_starts, i) - _starts.begin()) - 1;
  _hint = c;
  return {c, i - _starts[c]};
}

std::pair<std::size_t, std::size_t> LineStore::insertionPoint(std::size_t pos) const {
  if (pos == size())
    return {_chunks.size() - 1, _chunks.back()->size()};
  return locate(pos);
}

LineStore::Chunk& LineStore::mut(std::size_t c) {
  if (_chunks[c].use_count() > 1)
    _chunks[c] = std::make_shared<Chunk>(*_chunks[c]);
  return *_chunks[c];
}

void LineStore::reindex(std::size_t from) {
  // The chunks before `from` are unchanged, so _starts[from] still holds.
  from = std::min(from, _chunks.size());
  _starts.resize(_chunks.size() + 1);
  for (auto c = from; c < _chunks.size(); ++c)
    _starts[c + 1] = _starts[c] + _chunks[c]->size();
}

void LineStore::rechunk(std::size_t c, Chunk&& lines) {
  auto n = lines.size();
  // n / T pieces of [T, 2T) lines each, or one piece when it fits.
  auto pieces = n <= 2 * _target ? 1 : n / _target;
  std::vector<std::shared_ptr<Chunk>> made;
  made.reserve(pieces);
  auto it = std::make_move_iterator(lines.begin());
  for (std::size_t p = 0; p < pieces; ++p) {
    auto len = static_cast<std::ptrdiff_t>(n / pieces + (p < n % pieces ? 1 : 0));
    made.push_back(std::make_shared<Chunk>(it, it + len));
    it += len;
  }
  _chunks.erase(_chunks.begin() + static_cast<std::ptrdiff_t>(c));
  _chunks.insert(_chunks.begin() + static_cast<std::ptrdiff_t>(c), made.begin(), made.end());
  reindex(c);
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
  auto take = [&](std::size_t k) -> Chunk {
    return _chunks[k].use_count() == 1 ? std::move(*_chunks[k]) : *_chunks[k];
  };
  if (n > 2 * _target) {
    rechunk(c, take(c));
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
  rechunk(lo, std::move(merged));
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
  insert(pos, std::span<LinePtr const>(&line, 1));
}

void LineStore::insert(std::size_t pos, std::span<LinePtr const> lines) {
  if (lines.empty())
    return;
  if (_chunks.empty()) {
    _chunks.push_back(std::make_shared<Chunk>());
    reindex(0);
  }
  auto [c, off] = insertionPoint(pos);
  auto at = static_cast<std::ptrdiff_t>(off);
  if (lines.size() <= _target) {
    auto& ch = mut(c);
    ch.insert(ch.begin() + at, lines.begin(), lines.end());
    reindex(c);
    rebalance(c);
    return;
  }
  // Large insert: build whole chunks from the old chunk's head, the new
  // lines and its tail.
  Chunk const& old = *_chunks[c];
  Chunk all;
  all.reserve(old.size() + lines.size());
  all.insert(all.end(), old.begin(), old.begin() + at);
  all.insert(all.end(), lines.begin(), lines.end());
  all.insert(all.end(), old.begin() + at, old.end());
  rechunk(c, std::move(all));
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
    _starts.push_back(_starts.back());
  }
  mut(_chunks.size() - 1).push_back(std::move(line));
  ++_starts.back();
}

void LineStore::clear() {
  _chunks.clear();
  _starts.assign(1, 0);
  _hint = 0;
}

bool LineStore::invariantsHold() const {
  if (_starts.size() != _chunks.size() + 1 || _starts[0] != 0)
    return false;
  for (std::size_t c = 0; c < _chunks.size(); ++c) {
    auto n = _chunks[c]->size();
    if (n == 0 || n > 2 * _target || _starts[c + 1] != _starts[c] + n)
      return false;
    // The last chunk may be small: push_back starts a fresh one when the
    // previous is full.
    if (c + 1 < _chunks.size() && n < _target / 4)
      return false;
  }
  return true;
}
