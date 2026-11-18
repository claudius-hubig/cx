#pragma once

/**
 * @file regex.hpp
 * @brief Regex — the regex engine behind Esc-X (regex find) and Esc-Y
 *        (regex replace).
 *
 * A Pike VM (a Thompson NFA simulation that carries capture positions):
 * matching a line of n code points against a pattern compiled to m
 * instructions takes O(n·m) time and O(m) memory whatever the pattern, and
 * nothing recurses on the text. std::regex backtracks, which froze the
 * editor for minutes on patterns such as `(a*)*b`, or `.*x` on a long line.
 *
 * The dialect is the subset of ECMAScript that needs no backtracking; see
 * docs/regex-dialect.md. Back-references in the pattern and lookaround are
 * refused when compiling. Matching works on code points: an invalid UTF-8
 * byte is a unit of its own that only `.` and negated classes match. A
 * pattern that can match a line break (`\n`, `\s`, a class listing one;
 * never `.` or a negated class) is matched over lines with searchLines().
 * Like
 * ECMAScript (and std::regex), the leftmost match wins, and among the
 * matches starting there the first by priority: alternatives left to
 * right, greedy quantifiers preferring more repetitions, lazy ones fewer.
 */

#include "types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/**
 * @brief Asked now and then by a scan over many lines; true stops it.
 *
 * Matching is linear, but tens of megabytes still take a while, and the
 * editor is single-threaded: this is how a search can be cancelled.
 */
using ScanCancel = std::function<bool()>;

class Regex {
public:
  /** @brief Groups reported in a Match: 0 (the whole match) to 9, all a
   *         replacement template can refer to. */
  static constexpr std::size_t kReportedGroups = 10;
  /** @brief Largest count in `{n,m}`. */
  static constexpr std::size_t kMaxRepeat = 1000;
  /** @brief Largest compiled program, in instructions (`{n,m}` copies). */
  static constexpr std::size_t kMaxProgram = 20000;
  /** @brief Deepest nesting of groups. */
  static constexpr std::size_t kMaxNesting = 100;

  static constexpr std::size_t npos = std::string_view::npos;

  /** @brief A group's byte range in the line; npos for both if the group
   *         did not take part in the match. */
  struct Span {
    std::size_t begin = npos;
    std::size_t end = npos;
    bool matched() const { return begin != npos; }
  };

  /** @brief Byte ranges of groups 0..9 (group 0 is the whole match). */
  struct Match {
    std::array<Span, kReportedGroups> groups;
    std::size_t begin() const { return groups[0].begin; }
    std::size_t end() const { return groups[0].end; }
  };

  /**
   * @brief Compile @p pattern.
   * @return The regex, or a message for the status line naming the
   *         problem and its 1-based character position, e.g.
   *         "back-references are not supported (at 4)".
   */
  static std::expected<Regex, std::string> compile(std::string_view pattern);

  /**
   * @brief The first match in @p line that starts at byte @p from or later.
   *
   * The text before @p from is context, not a line start: `^` matches only
   * at byte 0 and `\b` sees the character before @p from.
   * @pre @p from is a code-point boundary of @p line (or its end).
   */
  std::optional<Match> search(std::string_view line, std::size_t from) const;

  /** @brief The match that starts exactly at byte @p at, if any; context as
   *         for search(). */
  std::optional<Match> matchAt(std::string_view line, std::size_t at) const;

  /**
   * @brief The replacement for @p match in @p line: @p replacement with
   *        `$&` / `$0` (the match), `$1`..`$9` (groups; empty if the group
   *        did not take part), `$$` (a `$`), `` $` `` / `$'` (the line before /
   *        after the match) substituted, and the escapes `\n`, `\t`, `\r`
   *        and `\\` (newline, tab, carriage return, backslash), as in a
   *        pattern. Any other `$` or `\` is literal, and one digit is read at
   *        most: `$10` is group 1 and a `0`.
   */
  static std::string expand(std::string_view replacement, std::string_view line,
                            Match const& match);

  /** @brief Number of capturing groups in the pattern (any number; only
   *         1..9 are reported). */
  std::size_t groupCount() const { return _groups; }

  /**
   * @brief True if a match can contain a line break: the pattern has `\n`
   *        (or a literal newline), `\s` or a class that lists a line break.
   *        Such a pattern must be matched with searchLines() to find
   *        matches that span lines.
   */
  bool spansLines() const { return _spansLines; }

  /** @brief Lines to match across, a line break between each two. */
  struct LineSource {
    std::function<std::string_view(std::size_t)> line; ///< line i, without its break
    std::size_t count = 0;                             ///< at least one
  };

  /** @brief Groups 0..9 of a match over lines, as [from, to) points. */
  struct LinesMatch {
    std::array<std::optional<std::pair<BufferPoint, BufferPoint>>, kReportedGroups> groups;
    BufferPoint begin() const { return groups[0]->first; }
    BufferPoint end() const { return groups[0]->second; }
  };

  /**
   * @brief The first match at or after @p from in the lines of @p src,
   *        which may span lines (a line break matches `\n`).
   *
   * `^` and `$` match at every line start and end. The text before @p from
   * is context. Scans to the end of the lines; @p cancel, if given, is
   * asked once per megabyte and stops the scan, which then reports no
   * match and sets @p *cancelled.
   */
  std::optional<LinesMatch> searchLines(LineSource const& src, BufferPoint from,
                                        ScanCancel const* cancel = nullptr,
                                        bool* cancelled = nullptr) const;

  /** @brief The match over lines that starts exactly at @p at, if any. */
  std::optional<LinesMatch> matchAtLines(LineSource const& src, BufferPoint at) const;

  /** @brief expand() for a match over lines: groups may span lines; `` $` ``
   *         and `$'` are the text of the first line before the match and of
   *         the last line after it. */
  static std::string expandLines(std::string_view replacement, LineSource const& src,
                                 LinesMatch const& match);

  // Program representation, public for the compiler in regex.cpp.
  /** @brief Instruction opcodes of the Pike VM. */
  enum class Op : std::uint8_t {
    Char,     ///< match unit x
    Any,      ///< match any unit
    Class,    ///< match a unit in class x
    Split,    ///< continue at x (preferred) and at y
    Jmp,      ///< continue at x
    Save,     ///< record the position in capture slot x
    Bol,      ///< assert line start
    Eol,      ///< assert line end
    WordB,    ///< assert a word boundary
    NotWordB, ///< assert no word boundary
    Match,    ///< a match
  };
  struct Inst {
    Op op;
    std::uint32_t x = 0;
    std::uint32_t y = 0;
  };
  /** @brief A set of code points as sorted, disjoint, non-adjacent ranges. */
  struct CharClass {
    std::vector<std::pair<std::uint32_t, std::uint32_t>> ranges;
    bool negated = false;
    /** @brief True if the class matches @p unit (see unitAt() in regex.cpp:
     *         an invalid byte only matches a negated class). */
    bool contains(std::uint32_t unit) const;
  };

private:
  /** @brief The VM over a text (one line, or lines; see regex.cpp). */
  /// @p start, if not npos, is where the first match can start at the
  /// earliest (the caller found _prefix there).
  template <class Text>
  std::optional<Match> run(Text text, std::size_t from, bool anchored,
                           std::size_t start = npos) const;

  std::vector<Inst> _prog;
  std::vector<CharClass> _classes;
  std::string _prefix;     ///< UTF-8 literal every match starts with, for skipping ahead
  std::string _prefixHead; ///< _prefix up to its first line break
  std::string _required;   ///< longer UTF-8 literal every match contains, to reject a line
  bool _spansLines = false;
  std::size_t _slots = 2; ///< capture slots tracked: 2 per reported group
  std::size_t _groups = 0;

  /**
   * @brief Working memory of run(), kept between calls so a search over
   *        many lines does not allocate per line. Makes a Regex unsafe to
   *        use from two threads at once (cx has one).
   */
  struct Scratch {
    struct List {
      std::vector<std::uint32_t> pcs;
      std::vector<std::size_t> caps; // pcs.size() * _slots
    };
    List a, b;
    std::vector<std::uint32_t> mark; // per pc: generation that last visited it
    std::uint32_t gen = 0;
    struct Frame {
      std::uint32_t pc;
      std::int32_t slot; // -1: visit pc; else restore capture slot to old
      std::size_t old;
    };
    std::vector<Frame> stack;
    std::vector<std::size_t> cur;
  };
  mutable Scratch _scratch;

  friend struct RegexBuilder;
};
