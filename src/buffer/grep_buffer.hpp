#pragma once

/**
 * @file grep_buffer.hpp
 * @brief GrepBuffer — a read-only listing of find-in-files results, plus
 *        the parser for the NUL-delimited output of the search backends.
 */

#include "buffer.hpp"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/** @brief A file a find-in-files search matched; shared by all of its hits. */
struct GrepFile {
  std::filesystem::path path; /**< Absolute path, resolved against the search root. */
  std::string displayPath;    /**< Path exactly as the backend emitted it (root-relative). */
};

/**
 * @brief One match produced by a find-in-files search.
 *
 * The file is shared because one search can return millions of hits in a
 * few files, and a std::filesystem::path with its components costs several
 * hundred bytes: a copy per hit once took 3.4 GB for 4M hits.
 */
struct GrepHit {
  std::shared_ptr<GrepFile const> file; /**< The matched file; never null. */
  std::size_t line = 0;                 /**< 1-based line number of the match. */
  std::string text; /**< Content of the matching line, leading whitespace trimmed. */

  std::filesystem::path const& path() const { return file->path; }
  std::string const& displayPath() const { return file->displayPath; }
};

/**
 * @brief Parse the NUL-delimited output of `git grep -z` / `grep -Z`.
 *
 * Both backends emit one record per line, terminated by '\\n', and both
 * write a NUL directly after the filename. They then differ in how the
 * line number is terminated:
 * - `git grep -z -n` produces `path NUL line NUL text`
 * - `grep -Z -n`     produces `path NUL line ':' text`
 *
 * Accepting either separator byte after the digits handles both without
 * the caller having to say which backend ran. The NUL after the filename
 * is what makes this unambiguous for paths that themselves contain ':'
 * or even '\\n': the path is everything up to the NUL, and only the text
 * after the line number ends at '\\n'.
 *
 * Records whose line number is malformed (non-numeric, empty, zero) are
 * skipped up to the next '\\n', and trailing output with no NUL is
 * ignored: an unexpected diagnostic line on stdout (neither backend writes
 * one with -I) costs at most the record it runs into, not the listing.
 *
 * @param out   Raw stdout of the search command.
 * @param root  Directory the command ran in; relative paths resolve
 *              against it.
 * @return The parsed hits, in backend order. Consecutive hits in the same
 *         file share one GrepFile (both backends list a file's hits together).
 */
std::vector<GrepHit> parseGrepOutput(std::string_view out, std::filesystem::path const& root);

/**
 * @brief Read-only buffer listing find-in-files matches, one per line,
 *        with a header describing the search.
 *
 * Enter on a result line opens the corresponding file at the matching
 * line; Screen looks the target up with selectedHit().
 */
class GrepBuffer : public Buffer {
public:
  GrepBuffer();

  bool readonly() const override { return true; }
  bool supportsLineNumberGutter() const override { return false; }

  std::tuple<RowIdx, ColIdx> render(std::string& out, RowIdx rows, ColIdx cols,
                                    RenderParams const& params) const override;

  /**
   * @brief Replace the listing with a new result set.
   * @return A ViewHint placing the cursor on the first result.
   */
  ViewHint refresh(std::string pattern, std::filesystem::path root, std::vector<GrepHit> hits);

  /**
   * @brief The hit at cursor position @p p.
   * @return nullopt on the header line, or when there are no results.
   */
  std::optional<GrepHit> selectedHit(BufferPoint p) const;

  /** @brief The parsed hits (for testing). */
  std::vector<GrepHit> const& hits() const { return _hits; }

  /** @brief Number of leading non-result lines (for testing). */
  std::size_t headerLines() const { return kHeaderLines; }

private:
  /** @brief Rebuild the display lines (the base-class _lines). */
  ViewHint rebuild();

  /** @brief Lines occupied by the header before the first result. */
  static constexpr std::size_t kHeaderLines = 1;

  std::string _pattern;
  std::filesystem::path _root;
  std::vector<GrepHit> _hits;
  /** @brief Byte length of the `path:line:` prefix of each line of _lines. */
  std::vector<std::size_t> _prefixLens;
};
