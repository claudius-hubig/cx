#pragma once

/**
 * @file replace.hpp
 * @brief Exact-match find-and-replace helpers used by
 *        Screen::openReplacePrompt.
 *
 * Replace deliberately uses byte-exact matching (not the smart-find rules
 * in search.hpp): fuzzy skipping is helpful for locating text you can
 * already see, but dangerous when the editor is about to rewrite bytes
 * you may not have intended to touch.
 *
 * A pattern with a newline in it matches across lines: split at its
 * newlines, its first piece must end a line, every middle piece must be a
 * whole line and its last piece must start the line after. Find uses the
 * same matching for such a pattern (see Screen::findNext()).
 */

#include "buffer/buffer.hpp"
#include "types.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/** @brief @p pattern split at its newlines: "a\nb" gives {"a", "b"}. */
inline std::vector<std::string_view> patternLines(std::string_view pattern) {
  std::vector<std::string_view> pieces;
  for (std::size_t start = 0;;) {
    auto nl = pattern.find('\n', start);
    pieces.push_back(pattern.substr(start, nl == std::string_view::npos ? nl : nl - start));
    if (nl == std::string_view::npos)
      return pieces;
    start = nl + 1;
  }
}

/**
 * @brief The end of an exact match of the pattern split into @p pieces
 *        (see patternLines()) that starts on line @p li, or nullopt.
 *
 * A pattern of more than one piece can only start where its first piece
 * ends the line, so a line has at most one candidate start; a one-piece
 * pattern is tried at @p off.
 * @return {start, end} of the match.
 */
inline std::optional<std::pair<BufferPoint, BufferPoint>>
exactMatchOnLine(Buffer const& buf, std::size_t li, std::size_t off,
                 std::vector<std::string_view> const& pieces) {
  std::string_view line = buf.lineContent(li);
  std::size_t k = pieces.size() - 1;
  if (k == 0) {
    if (line.substr(std::min(off, line.size())).starts_with(pieces[0]))
      return std::pair{BufferPoint{li, off}, BufferPoint{li, off + pieces[0].size()}};
    return std::nullopt;
  }
  if (li + k >= buf.lineCount() || line.size() < pieces[0].size() || !line.ends_with(pieces[0]))
    return std::nullopt;
  for (std::size_t i = 1; i < k; ++i)
    if (std::string_view(buf.lineContent(li + i)) != pieces[i])
      return std::nullopt;
  if (!std::string_view(buf.lineContent(li + k)).starts_with(pieces[k]))
    return std::nullopt;
  return std::pair{BufferPoint{li, line.size() - pieces[0].size()},
                   BufferPoint{li + k, pieces[k].size()}};
}

/**
 * @brief The last exact match of @p pattern (newlines allowed) that ends
 *        before @p limit, or nullopt. Used by Find backward for a pattern
 *        with a newline.
 */
inline std::optional<std::pair<BufferPoint, BufferPoint>>
findPrevMatchInBuffer(Buffer const& buf, BufferPoint limit, std::string const& pattern) {
  if (pattern.empty() || buf.lineCount() == 0)
    return std::nullopt;
  auto pieces = patternLines(pattern);
  std::size_t li = std::min(limit.line, buf.lineCount() - 1);
  for (;; --li) {
    if (pieces.size() == 1) {
      std::string_view line = buf.lineContent(li);
      for (auto pos = line.rfind(pieces[0]); pos != std::string_view::npos;
           pos = pos ? line.rfind(pieces[0], pos - 1) : std::string_view::npos) {
        BufferPoint end{li, pos + pieces[0].size()};
        if (end < limit)
          return std::pair{BufferPoint{li, pos}, end};
        if (pos == 0)
          break;
      }
    } else if (auto m = exactMatchOnLine(buf, li, 0, pieces); m && m->second < limit) {
      return m;
    }
    if (li == 0)
      return std::nullopt;
  }
}

/** @brief One match to rewrite: the region [from, to) and its new text. */
struct Rewrite {
  BufferPoint from;
  BufferPoint to;
  std::string text;
};

/**
 * @brief Apply @p rewrites, which are sorted and do not overlap, to @p buf.
 *
 * Rewrites on the same or adjacent lines are applied together, as one
 * delete and one insert over the lines they span; groups are applied from
 * the last to the first, so the points of the earlier ones stay valid.
 * The lines between groups keep their storage (and with it the memory
 * they share with earlier history versions). Records no version.
 */
inline void applyRewrites(Buffer& buf, std::vector<Rewrite> const& rewrites) {
  // Append the text of [from, to) to out, a newline between each two lines.
  auto appendRange = [&](std::string& out, BufferPoint from, BufferPoint to) {
    for (std::size_t li = from.line; li <= to.line; ++li) {
      std::string_view line = buf.lineContent(li);
      std::size_t b = li == from.line ? from.offset : 0;
      std::size_t e = li == to.line ? to.offset : line.size();
      out.append(line.substr(b, e - b));
      if (li != to.line)
        out += '\n';
    }
  };
  std::size_t end = rewrites.size();
  while (end > 0) {
    // The group [begin, end): rewrites whose lines touch.
    std::size_t begin = end - 1;
    while (begin > 0 && rewrites[begin - 1].to.line + 1 >= rewrites[begin].from.line)
      --begin;
    std::size_t first = rewrites[begin].from.line, last = rewrites[end - 1].to.line;
    std::string out;
    BufferPoint at{first, 0};
    for (std::size_t i = begin; i < end; ++i) {
      appendRange(out, at, rewrites[i].from);
      out += rewrites[i].text;
      at = rewrites[i].to;
    }
    BufferPoint lineEnd{last, buf.lineContent(last).size()};
    appendRange(out, at, lineEnd);
    buf.deleteRegion({first, 0}, lineEnd);
    buf.insertText(out, {first, 0});
    end = begin;
  }
}

/**
 * @brief Find the next exact substring match of @p pattern at or after
 *        @p start in @p buf.
 *
 * Scans line by line and never wraps past end-of-buffer; a match spans
 * lines only where @p pattern has a newline.
 *
 * @return The matched region as {from, to}, or nullopt when no further
 *         match exists.
 */
inline std::optional<std::pair<BufferPoint, BufferPoint>>
findNextMatchInBuffer(Buffer const& buf, BufferPoint start, std::string const& pattern) {
  if (pattern.empty())
    return std::nullopt;
  std::size_t lineCount = buf.lineCount();
  if (pattern.find('\n') != std::string::npos) {
    auto pieces = patternLines(pattern);
    for (std::size_t li = start.line; li < lineCount; ++li)
      if (auto m = exactMatchOnLine(buf, li, 0, pieces); m && !(m->first < start))
        return m;
    return std::nullopt;
  }
  for (std::size_t li = start.line; li < lineCount; ++li) {
    std::size_t from = (li == start.line) ? start.offset : 0;
    auto const& line = buf.lineContent(li);
    auto pos = line.find(pattern, from);
    if (pos != std::string::npos)
      return std::pair{BufferPoint{li, pos}, BufferPoint{li, pos + pattern.size()}};
  }
  return std::nullopt;
}

/**
 * @brief replaceAllForward() for a @p pattern with a newline: the matches
 *        are collected on the original text first, so replacements are
 *        never matched again, then applied together (applyRewrites()).
 */
inline std::size_t replaceAllForwardLines(Buffer& buf, BufferPoint start,
                                          std::string const& pattern,
                                          std::string const& replacement) {
  std::vector<Rewrite> rewrites;
  for (BufferPoint at = start;;) {
    auto m = findNextMatchInBuffer(buf, at, pattern);
    if (!m)
      break;
    rewrites.push_back({m->first, m->second, replacement});
    at = m->second;
  }
  if (rewrites.empty())
    return 0;
  applyRewrites(buf, rewrites);
  buf.recordVersion(start);
  return rewrites.size();
}

/**
 * @brief Replace every forward exact-match occurrence of @p pattern with
 *        @p replacement from @p start to end-of-buffer.
 *
 * Every line is scanned once, left to right, against its original text
 * and rewritten in one edit, so inserted text is never re-scanned (a
 * @p replacement that contains @p pattern terminates) and a line costs
 * time linear in its length however many matches it has. A no-op on a
 * read-only buffer or an empty pattern.
 *
 * The edits bypass BufferView, so this records the history version itself:
 * one version, with the cursor at @p start, when anything was replaced.
 *
 * @return The number of replacements performed.
 */
inline std::size_t replaceAllForward(Buffer& buf, BufferPoint start, std::string const& pattern,
                                     std::string const& replacement) {
  if (pattern.empty() || buf.readonly())
    return 0;
  if (pattern.find('\n') != std::string::npos)
    return replaceAllForwardLines(buf, start, pattern, replacement);
  std::size_t count = 0;
  for (std::size_t li = start.line; li < buf.lineCount(); ++li) {
    std::string_view const view = buf.lineContent(li);
    std::size_t pos = li == start.line ? std::min(start.offset, view.size()) : 0;
    std::size_t hit = view.find(pattern, pos);
    if (hit == std::string::npos)
      continue;
    std::string const line(view); // a copy: the buffer's line is replaced below
    std::string out(line, 0, pos);
    for (; hit != std::string::npos; hit = line.find(pattern, pos)) {
      out.append(line, pos, hit - pos);
      out += replacement;
      pos = hit + pattern.size();
      ++count;
    }
    out.append(line, pos);
    buf.deleteRegion({li, 0}, {li, line.size()});
    li = buf.insertText(out, {li, 0}).line; // a replacement may contain newlines
  }
  if (count > 0)
    buf.recordVersion(start);
  return count;
}
