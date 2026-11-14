#pragma once

/**
 * @file line_store.hpp
 * @brief LineStore — a buffer's lines as a sequence of shared,
 *        copy-on-write chunks, so copying the whole store (a history
 *        snapshot) costs one pointer per chunk instead of one per line.
 */

#include "types.hpp"

#include <cstddef>
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
 * Line @p i is found by binary search over the chunks' first line numbers;
 * a cached "last chunk" makes sequential and nearby access O(1). Edits are
 * O(T + N/T).
 *
 * Assumptions:
 * - **Single-threaded.** Reads update the lookup cache, and chunk sharing
 *   is decided by `use_count()`; neither is safe across threads.
 * - **References are short-lived.** A reference returned by operator[],
 *   back() or mutableLine(), and every iterator, is valid only until the
 *   next mutation of this store (as for `std::vector`).
 * - Lines passed in that may later be edited in place were made by
 *   makeLine() (see there).
 */
class LineStore {
public:
  /** @brief Default target chunk size T. */
  static constexpr std::size_t kDefaultChunk = 512;

  /** @brief An empty store; @p chunk is the target chunk size T (>= 4). */
  explicit LineStore(std::size_t chunk = kDefaultChunk);
  /** @brief A store holding @p lines, in order. */
  explicit LineStore(std::span<LinePtr const> lines, std::size_t chunk = kDefaultChunk);

  /** @name Reading */
  /** @{ */
  std::size_t size() const { return _starts.back(); }
  bool empty() const { return size() == 0; }
  /** @pre @p i < size(). */
  LinePtr const& operator[](std::size_t i) const {
    auto [c, off] = locate(i);
    return (*_chunks[c])[off];
  }
  /** @pre !empty(). */
  LinePtr const& back() const { return _chunks.back()->back(); }

  /** @brief Forward iterator over the lines (as `LinePtr const&`). */
  class const_iterator {
  public:
    using value_type = LinePtr;
    using difference_type = std::ptrdiff_t;
    using reference = LinePtr const&;
    using pointer = LinePtr const*;
    using iterator_category = std::forward_iterator_tag;

    const_iterator() = default;
    reference operator*() const { return (*(*_chunks)[_chunk])[_off]; }
    pointer operator->() const { return &**this; }
    const_iterator& operator++() {
      if (++_off == (*_chunks)[_chunk]->size()) {
        ++_chunk;
        _off = 0;
      }
      return *this;
    }
    const_iterator operator++(int) {
      auto old = *this;
      ++*this;
      return old;
    }
    bool operator==(const_iterator const& o) const { return _chunk == o._chunk && _off == o._off; }

  private:
    friend class LineStore;
    using Chunks = std::vector<std::shared_ptr<std::vector<LinePtr>>>;
    const_iterator(Chunks const* chunks, std::size_t chunk) : _chunks(chunks), _chunk(chunk) {}
    Chunks const* _chunks = nullptr;
    std::size_t _chunk = 0;
    std::size_t _off = 0;
  };
  const_iterator begin() const { return {&_chunks, 0}; }
  const_iterator end() const { return {&_chunks, _chunks.size()}; }
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

  /** @brief {chunk, offset} of line @p i, via the cached last chunk. */
  std::pair<std::size_t, std::size_t> locate(std::size_t i) const {
    auto c = _hint;
    if (c + 1 < _starts.size() && _starts[c] <= i && i < _starts[c + 1])
      return {c, i - _starts[c]};
    return locateSlow(i);
  }
  /** @brief locate() when line @p i is not in the cached chunk. */
  std::pair<std::size_t, std::size_t> locateSlow(std::size_t i) const;
  /** @brief {chunk, offset} at which to insert before position @p pos. */
  std::pair<std::size_t, std::size_t> insertionPoint(std::size_t pos) const;
  /** @brief Chunk @p c, cloned first if it is shared. */
  Chunk& mut(std::size_t c);
  /** @brief Rebuild _starts from chunk @p from on. */
  void reindex(std::size_t from);
  /**
   * @brief Restore the size bounds around chunk @p c after an edit:
   *        split it if too large, merge it with a neighbour if too small,
   *        drop it if empty. Leaves _starts consistent.
   */
  void rebalance(std::size_t c);
  /** @brief Replace chunk @p c by pieces of [T, 2T] lines holding @p lines. */
  void rechunk(std::size_t c, Chunk&& lines);

  std::vector<std::shared_ptr<Chunk>> _chunks;
  /**
   * @brief First line number of each chunk, plus size() at the end:
   *        chunk c holds lines [_starts[c], _starts[c + 1]).
   */
  std::vector<std::size_t> _starts{0};
  std::size_t _target;
  mutable std::size_t _hint = 0; /**< Chunk of the last lookup. */
};
