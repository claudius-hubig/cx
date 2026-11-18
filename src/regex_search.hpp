#pragma once

/**
 * @file regex_search.hpp
 * @brief Per-line regex search/replace helpers used by Esc-X / Esc-Y.
 *
 * Kept separate from the smart-find helpers in search.hpp because regex
 * matching uses anchors, character classes and groups that smart-find
 * deliberately avoids, and regex replace honours group substitution ($1,
 * $&, …) so a plain replacement loop is the wrong primitive. A pattern
 * that cannot match a line break is matched line by line; one that can
 * (`\n`, `\s`, …; Regex::spansLines()) is matched over the buffer's lines
 * as one text. The engine is Regex (regex.hpp), which takes time linear in
 * the length of the text whatever the pattern.
 */

#include "buffer/buffer.hpp"
#include "regex.hpp"
#include "replace.hpp"
#include "types.hpp"
#include "utf8.hpp"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

/**
 * @brief Compile @p pattern (see docs/regex-dialect.md).
 * @param pattern The pattern source.
 * @param[out] error Set to a human-readable message on failure.
 * @return The compiled regex, or nullopt for an empty or invalid pattern.
 */
inline std::optional<Regex> compileRegex(std::string const& pattern, std::string& error) {
  auto re = Regex::compile(pattern);
  if (!re) {
    error = re.error();
    return std::nullopt;
  }
  return std::move(*re);
}

/** @brief The lines of @p buf as a Regex::LineSource (valid while @p buf is
 *         not edited). */
inline Regex::LineSource bufferLines(Buffer const& buf) {
  return {[&buf](std::size_t i) { return std::string_view(buf.lineContent(i)); }, buf.lineCount()};
}

/**
 * @brief Counts the text a scan has gone through and asks its ScanCancel
 *        once per kScanCheckBytes of it.
 */
class ScanBudget {
public:
  static constexpr std::size_t kScanCheckBytes = std::size_t{1} << 20;

  explicit ScanBudget(ScanCancel const& cancel) : _cancel(cancel) {}

  /** @brief Account for @p bytes scanned. @return False once cancelled. */
  bool charge(std::size_t bytes) {
    if (_cancelled)
      return false;
    _bytes += bytes;
    if (_bytes >= kScanCheckBytes) {
      _bytes = 0;
      _cancelled = _cancel && _cancel();
    }
    return !_cancelled;
  }
  bool cancelled() const { return _cancelled; }

private:
  ScanCancel const& _cancel;
  std::size_t _bytes = 0;
  bool _cancelled = false;
};

/**
 * @brief Scan a line for the first regex match starting at or after
 *        @p from.
 *
 * The text before @p from is context, not a line start.
 * @return {matchStart, matchEnd} byte offsets, or nullopt.
 */
inline std::optional<std::pair<std::size_t, std::size_t>>
findFirstRegex(std::string_view line, std::size_t from, Regex const& re) {
  auto m = re.search(line, from);
  if (!m)
    return std::nullopt;
  return std::pair{m->begin(), m->end()};
}

/**
 * @brief Scan a line for the last regex match within [from, limit).
 *
 * Used by the backward-search path.
 * @return {matchStart, matchEnd} byte offsets, or nullopt.
 */
inline std::optional<std::pair<std::size_t, std::size_t>>
findLastRegex(std::string_view line, std::size_t from, std::size_t limit, Regex const& re) {
  std::optional<std::pair<std::size_t, std::size_t>> last;
  std::size_t pos = from;
  while (pos <= limit) {
    auto m = findFirstRegex(line, pos, re);
    if (!m || m->first >= limit)
      break;
    last = m;
    // Step past an empty match by one code point, or the scan stalls.
    if (m->second > m->first)
      pos = m->second;
    else if (m->second < line.size())
      pos = m->second + utf8SeqLen(static_cast<unsigned char>(line[m->second]));
    else
      break;
  }
  return last;
}

/** @brief What findNextRegexInBuffer() found. */
struct RegexScan {
  std::optional<std::pair<BufferPoint, BufferPoint>> match; ///< nullopt: none (or cancelled)
  bool cancelled = false;                                   ///< the ScanCancel stopped the scan
};

/** @brief findNextRegexInBuffer() for a pattern that can match a line break. */
inline RegexScan findNextRegexOverLines(Buffer const& buf, BufferPoint start, Regex const& re,
                                        ScanCancel const& cancel) {
  bool cancelled = false;
  auto m = re.searchLines(bufferLines(buf), start, &cancel, &cancelled);
  if (!m)
    return {std::nullopt, cancelled};
  return {std::pair{m->begin(), m->end()}};
}

/**
 * @brief Find the next regex match in the buffer at or after @p start.
 *
 * Scans to the end of the buffer and never wraps; a match spans lines only
 * if the pattern can match a line break. @p cancel is asked once per
 * megabyte scanned.
 */
inline RegexScan findNextRegexInBuffer(Buffer const& buf, BufferPoint start, Regex const& re,
                                       ScanCancel const& cancel = {}) {
  if (re.spansLines())
    return findNextRegexOverLines(buf, start, re, cancel);
  ScanBudget budget(cancel);
  std::size_t lineCount = buf.lineCount();
  for (std::size_t li = start.line; li < lineCount; ++li) {
    auto const& line = buf.lineContent(li);
    std::size_t from = (li == start.line) ? start.offset : 0;
    if (auto m = findFirstRegex(line, from, re))
      return {std::pair{BufferPoint{li, m->first}, BufferPoint{li, m->second}}};
    if (!budget.charge(line.size() + 1))
      return {std::nullopt, true};
  }
  return {};
}

/**
 * @brief The position just past @p p for resuming a scan after an empty
 *        (zero-width) match at @p p.
 *
 * Steps one code point forward, or to the start of the next line from a
 * line end, so a scan that resumes there cannot find the same empty match
 * again.
 * @return The next position, or nullopt at the end of the buffer.
 */
inline std::optional<BufferPoint> stepPastEmptyMatch(Buffer const& buf, BufferPoint p) {
  auto const& line = buf.lineContent(p.line);
  if (p.offset < line.size()) {
    auto len = utf8SeqLen(static_cast<unsigned char>(line[p.offset]));
    return BufferPoint{p.line, std::min(p.offset + len, line.size())};
  }
  if (p.line + 1 < buf.lineCount())
    return BufferPoint{p.line + 1, 0};
  return std::nullopt;
}

/**
 * @brief The replacement for the match [@p from, @p to) found in @p buf, with
 *        its group references and escapes substituted (Regex::expand()).
 *
 * Matches again at @p from so groups and context-dependent parts (\b, ^,
 * $) resolve; falls back to @p replacement as it is if that does not give
 * the same match (it should).
 */
inline std::string substituteCapturesInBuffer(Buffer const& buf, BufferPoint from, BufferPoint to,
                                              Regex const& re, std::string_view replacement) {
  if (re.spansLines()) {
    auto lines = bufferLines(buf);
    auto m = re.matchAtLines(lines, from);
    if (!m || m->end() != to)
      return std::string(replacement);
    return Regex::expandLines(replacement, lines, *m);
  }
  auto const& line = buf.lineContent(from.line);
  auto m = re.matchAt(line, from.offset);
  if (!m || to.line != from.line || m->end() != to.offset)
    return std::string(replacement);
  return Regex::expand(replacement, line, *m);
}

/**
 * @brief Substitute a single match's region with @p replacement,
 *        honouring group references ($0..$9, $&, $$, $`, $') and escapes.
 *
 * The caller has already located the match's [from, to) and verified the
 * line.
 */
inline std::string substituteCaptures(std::string_view line, std::size_t from, std::size_t to,
                                      Regex const& re, std::string_view replacement) {
  // Re-run the match anchored at `from` with the whole line as context,
  // so matches that depend on surrounding text (\b, ^, $) reproduce and
  // $1.. resolve correctly.
  auto m = re.matchAt(line, from);
  if (!m || m->end() != to)
    return std::string(replacement); // shouldn't happen, but fail safe
  return Regex::expand(replacement, line, *m);
}

/** @brief What replaceAllRegexForward() did. */
struct ReplaceAllResult {
  std::size_t count = 0;  ///< replacements made
  bool cancelled = false; ///< the ScanCancel stopped it (what was replaced stays)
};

/**
 * @brief replaceAllRegexForward() for a pattern that can match a line
 *        break: the matches are collected over the original text, then
 *        rewritten together (applyRewrites()).
 */
inline ReplaceAllResult replaceAllRegexForwardLines(Buffer& buf, BufferPoint start, Regex const& re,
                                                    std::string const& replacement,
                                                    ScanCancel const& cancel) {
  ReplaceAllResult result;
  auto lines = bufferLines(buf);
  std::vector<Rewrite> rewrites;
  for (BufferPoint at = start;;) {
    bool cancelled = false;
    auto m = re.searchLines(lines, at, &cancel, &cancelled);
    if (cancelled)
      result.cancelled = true;
    if (!m)
      break;
    rewrites.push_back({m->begin(), m->end(), Regex::expandLines(replacement, lines, *m)});
    at = m->end();
    if (m->begin() == m->end()) {
      auto next = stepPastEmptyMatch(buf, at);
      if (!next)
        break;
      at = *next;
    }
  }
  result.count = rewrites.size();
  if (!rewrites.empty()) {
    applyRewrites(buf, rewrites);
    buf.recordVersion(start);
  }
  return result;
}

/**
 * @brief Replace every forward regex match of @p re with the result of
 *        applying @p replacement (a template with $&/$N references) to
 *        each match.
 *
 * Every line is matched once, left to right, against its original text
 * and rewritten in one edit. Matches therefore never see replaced text:
 * a replacement that itself matches does not loop, `^a` replaces only
 * the `a` that started the line (not the one that moves there once it
 * is removed), and a line costs time linear in its length however many
 * matches it has. An empty match is replaced and the scan steps one code
 * point on, so zero-width patterns (`x*`, `^`, `\b`, …) terminate. (Unlike
 * std::regex_replace, no non-empty match is retried at the position of an
 * empty one, which only matters for patterns such as `|a`.) A no-op on a
 * read-only buffer.
 *
 * @p cancel is asked once per megabyte scanned and stops the replacement
 * between lines; the lines already rewritten stay rewritten.
 *
 * The edits bypass BufferView, so this records the history version itself:
 * one version, with the cursor at @p start, when anything was replaced.
 */
inline ReplaceAllResult replaceAllRegexForward(Buffer& buf, BufferPoint start, Regex const& re,
                                               std::string const& replacement,
                                               ScanCancel const& cancel = {}) {
  ReplaceAllResult result;
  if (buf.readonly())
    return result;
  if (re.spansLines())
    return replaceAllRegexForwardLines(buf, start, re, replacement, cancel);
  ScanBudget budget(cancel);
  for (std::size_t li = start.line; li < buf.lineCount(); ++li) {
    std::string_view const view = buf.lineContent(li);
    std::size_t pos = li == start.line ? std::min(start.offset, view.size()) : 0;
    auto first = re.search(view, pos);
    if (!first) {
      if (!budget.charge(view.size() + 1)) {
        result.cancelled = true;
        break;
      }
      continue;
    }
    std::string const line(view); // a copy: the buffer's line is replaced below
    std::string out(line, 0, pos);
    std::size_t before = result.count;
    for (auto m = first; m; m = re.search(line, pos)) {
      bool empty = m->begin() == m->end();
      out.append(line, pos, m->begin() - pos);
      out += Regex::expand(replacement, line, *m);
      ++result.count;
      pos = m->end();
      if (empty) {
        if (pos >= line.size())
          break;
        auto len = std::min(utf8SeqLen(static_cast<unsigned char>(line[pos])), line.size() - pos);
        out.append(line, pos, len);
        pos += len;
      }
    }
    if (result.count != before) {
      out.append(line, pos);
      buf.deleteRegion({li, 0}, {li, line.size()});
      li = buf.insertText(out, {li, 0}).line; // a replacement may contain newlines
    }
    if (!budget.charge(line.size() + 1)) {
      result.cancelled = true;
      break;
    }
  }
  if (result.count > 0)
    buf.recordVersion(start);
  return result;
}
