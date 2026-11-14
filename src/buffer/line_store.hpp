#pragma once

/**
 * @file line_store.hpp
 * @brief LineStore — a buffer's lines as a sequence of shared,
 *        copy-on-write chunks, so copying the whole store (a history
 *        snapshot) costs one pointer per chunk instead of one per line.
 */

#include "types.hpp"

#include <cstddef>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/**
 * @brief A new line for a LineStore.
 *
 * The string is created non-const behind the `string const` pointer, which
 * is what lets LineStore::mutableLine() edit a uniquely held line in place
 * without undefined behaviour. Lines that may be edited in place must come
 * from here.
 */
inline LinePtr makeLine(std::string s = {}) { return std::make_shared<std::string>(std::move(s)); }
/** @copydoc makeLine(std::string) */
inline LinePtr makeLine(std::string_view s) { return std::make_shared<std::string>(s); }
/** @copydoc makeLine(std::string) */
inline LinePtr makeLine(char const* s) { return makeLine(std::string_view(s)); }

/**
 * @brief An indexed sequence of lines stored in shared copy-on-write chunks.
 *
 * The lines are kept in chunks of up to 2×T LinePtrs (T = the target chunk
 * size; every chunk but a lone one holds at least T/4). Copying a LineStore
 * copies the chunk pointers only — O(N/T) — and both copies then share
 * every chunk; the first edit of a shared chunk clones it (O(T) pointer
 * copies) and mutableLine() then also detaches a shared line string. So a
 * snapshot taken after every edit costs O(N/T), and unchanged chunks and
 * lines stay shared between all snapshots.
 *
 * Line @p i is found by binary search over the chunks' first line numbers.
 * The last chunk looked up is cached (its line range and array), so
 * sequential and nearby reads cost one comparison and one load, as for a
 * vector. Edits are O(T + N/T).
 *
 * Assumptions:
 * - **Single-threaded.** Reads update the lookup cache, and chunk sharing
 *   is decided by `use_count()`; neither is safe across threads.
 * - **References are short-lived.** A reference returned by operator[],
 *   back() or mutableLine(), and every iterator, is valid only until the
 *   next mutation of this store (as for `std::vector`).
 * - Lines passed in that may later be edited in place were made by
 *   makeLine() (see there).
 * - Bulk loads should build a `std::vector<LinePtr>` and move it in: the
 *   chunk arrays are then allocated after the lines, which keeps the line
 *   strings contiguous. (Allocating an 8 KB chunk array between every 512
 *   lines, as push_back() must, was measured to halve the speed of later
 *   whole-buffer scans of those lines.)
 */
class LineStore {
public:
  /** @brief Default target chunk size T. */
  static constexpr std::size_t kDefaultChunk = 512;

  /** @brief An empty store; @p chunk is the target chunk size T (>= 4). */
  explicit LineStore(std::size_t chunk = kDefaultChunk);
  /** @brief A store holding copies of @p lines, in order. */
  explicit LineStore(std::span<LinePtr const> lines, std::size_t chunk = kDefaultChunk);
  /** @brief A store holding @p lines, in order (moved in; see the class notes). */
  explicit LineStore(std::vector<LinePtr>&& lines, std::size_t chunk = kDefaultChunk);

  LineStore(LineStore const&) = default;
  LineStore& operator=(LineStore const&) = default;
  /** @brief Moves leave @p o a valid empty store. */
  LineStore(LineStore&& o) noexcept;
  LineStore& operator=(LineStore&& o) noexcept;

  /** @name Reading */
  /** @{ */
  std::size_t size() const { return _size; }
  bool empty() const { return _size == 0; }
  /** @pre @p i < size(). */
  LinePtr const& operator[](std::size_t i) const {
    // Unsigned: one comparison covers both ends of the cached range.
    if (i - _hintFirst < _hintCount)
      return _hintData[i - _hintFirst];
    return lookupSlow(i);
  }
  /** @pre !empty(). */
  LinePtr const& back() const { return _chunks.back()->back(); }

  /**
   * @brief Forward iterator over the lines (as `LinePtr const&`).
   *
   * Holds the current chunk's element range, so dereferencing is one load
   * and stepping one comparison (the chunk is re-entered every T lines).
   */
  class const_iterator {
  public:
    using value_type = LinePtr;
    using difference_type = std::ptrdiff_t;
    using reference = LinePtr const&;
    using pointer = LinePtr const*;
    using iterator_category = std::forward_iterator_tag;

    const_iterator() = default;
    reference operator*() const { return *_cur; }
    pointer operator->() const { return _cur; }
    const_iterator& operator++() {
      if (++_cur == _end) {
        ++_chunk;
        enter();
      }
      return *this;
    }
    const_iterator operator++(int) {
      auto old = *this;
      ++*this;
      return old;
    }
    /** @brief Positions are distinct elements (end() holds none). */
    bool operator==(const_iterator const& o) const { return _cur == o._cur; }

  private:
    friend class LineStore;
    using Chunks = std::vector<std::shared_ptr<std::vector<LinePtr>>>;
    const_iterator(Chunks const* chunks, std::size_t chunk, std::size_t off = 0)
        : _chunks(chunks), _chunk(chunk) {
      enter();
      _cur += off;
    }
    /** @brief Point at the start of chunk @c _chunk (nothing past the last). */
    void enter() {
      if (_chunk < _chunks->size()) {
        auto const& v = *(*_chunks)[_chunk];
        _cur = v.data();
        _end = _cur + v.size();
      } else {
        _cur = _end = nullptr;
      }
    }
    Chunks const* _chunks = nullptr;
    std::size_t _chunk = 0;
    LinePtr const* _cur = nullptr;
    LinePtr const* _end = nullptr;
  };
  const_iterator begin() const { return {&_chunks, 0}; }
  const_iterator end() const { return {&_chunks, _chunks.size()}; }
  /**
   * @brief Iterator at line @p i (end() for @p i == size()); O(log N/T).
   *        Sequential loops over a range are cheaper this way than indexing.
   */
  const_iterator iteratorAt(std::size_t i) const {
    if (i >= _size)
      return end();
    auto [c, off] = locate(i);
    return {&_chunks, c, off};
  }
  /** @} */

  /** @name Editing */
  /** @{ */
  /** @brief Replace line @p i. @pre @p i < size(). */
  void replace(std::size_t i, LinePtr line);
  /**
   * @brief Line @p i as a mutable string, detached from every other holder
   *        (snapshots, other stores) first.
   * @pre @p i < size(), and the line was made by makeLine() if it is held
   *      only here (see the class assumptions).
   */
  std::string& mutableLine(std::size_t i);
  /** @brief Insert @p line before position @p pos. @pre @p pos <= size(). */
  void insert(std::size_t pos, LinePtr line);
  /** @brief Insert @p lines, in order, before position @p pos. @pre @p pos <= size(). */
  void insert(std::size_t pos, std::span<LinePtr const> lines);
  /** @brief As above, moving the lines in (no reference-count traffic). */
  void insert(std::size_t pos, std::vector<LinePtr>&& lines);
  /** @brief Remove line @p pos. @pre @p pos < size(). */
  void erase(std::size_t pos) { erase(pos, pos + 1); }
  /** @brief Remove lines [@p first, @p last). @pre @p first <= @p last <= size(). */
  void erase(std::size_t first, std::size_t last);
  /** @brief Append @p line (filling chunks to T, which leaves room for edits). */
  void push_back(LinePtr line);
  /** @brief Remove every line. */
  void clear();
  /** @} */

  /** @name Introspection (tests and diagnostics) */
  /** @{ */
  std::size_t chunkCount() const { return _chunks.size(); }
  /** @brief Identity of the chunk holding line @p i, for sharing checks. */
  void const* chunkIdentity(std::size_t i) const { return _chunks[locate(i).first].get(); }
  /**
   * @brief True if the chunk sizes, the index and the lookup cache are
   *        consistent and every chunk is within its size bounds.
   */
  bool invariantsHold() const;
  /** @} */

private:
  using Chunk = std::vector<LinePtr>;

  /** @brief {chunk, offset} of line @p i (and caches that chunk). */
  std::pair<std::size_t, std::size_t> locate(std::size_t i) const {
    (void)(*this)[i];
    return {_hintChunk, i - _hintFirst};
  }
  /** @brief operator[] for a line outside the cached chunk: finds and caches its chunk. */
  LinePtr const& lookupSlow(std::size_t i) const;
  /** @brief One past the last line of chunk @p c. */
  std::size_t chunkEnd(std::size_t c) const {
    return c + 1 < _starts.size() ? _starts[c + 1] : _size;
  }
  /** @brief Forget the cached chunk's range and array (keeps its index as a search hint). */
  void dropHint() { _hintCount = 0; }
  /** @brief {chunk, offset} at which to insert before position @p pos. */
  std::pair<std::size_t, std::size_t> insertionPoint(std::size_t pos) const;
  /** @brief Chunk @p c, cloned first if it is shared. Drops the cached chunk. */
  Chunk& mut(std::size_t c);
  /** @brief Rebuild _starts and _size from chunk @p from on. Drops the cached chunk. */
  void reindex(std::size_t from);
  /**
   * @brief Restore the size bounds around chunk @p c after an edit:
   *        split it if too large, merge it with a neighbour if too small,
   *        drop it if empty. Leaves _starts consistent.
   */
  void rebalance(std::size_t c);
  /**
   * @brief Replace chunk @p c by pieces of [T, 2T) lines (or one piece)
   *        holding the concatenation of @p parts, which are moved from.
   */
  void rechunk(std::size_t c, std::initializer_list<std::span<LinePtr>> parts);
  /** @brief Chunk @p c's lines, moved out if it is unshared, copied otherwise. */
  Chunk take(std::size_t c);

  std::vector<std::shared_ptr<Chunk>> _chunks;
  /** @brief First line number of each chunk: chunk c holds [_starts[c], chunkEnd(c)). */
  std::vector<std::size_t> _starts;
  std::size_t _size = 0;
  std::size_t _target;

  /**
   * @name Lookup cache: the chunk of the last lookup.
   *
   * Valid while @c _hintCount > 0; every mutation drops it (mut(),
   * reindex()), since a chunk's array may move. A copy may keep it: the
   * chunk it points into is then shared, and so neither store edits it in
   * place.
   */
  /** @{ */
  mutable LinePtr const* _hintData = nullptr; /**< The chunk's array. */
  mutable std::size_t _hintFirst = 0;         /**< Its first line number. */
  mutable std::size_t _hintCount = 0;         /**< Its line count (0: no cache). */
  mutable std::size_t _hintChunk = 0;         /**< Its index, kept as a search hint. */
  /** @} */
};
