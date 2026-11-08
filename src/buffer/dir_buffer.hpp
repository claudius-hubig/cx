#pragma once

/**
 * @file dir_buffer.hpp
 * @brief DirBuffer — a read-only directory browser supporting navigation,
 *        deletion, rename, and subdirectory creation.
 */

#include "buffer.hpp"

#include <cstdint>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

/**
 * @brief Read-only buffer that displays a directory listing with
 *        permissions, owner, group, size, and modification time.
 *
 * Supports navigating into subdirectories, toggling hidden files, marking
 * entries for deletion, renaming entries, and creating subdirectories.
 */
class DirBuffer : public Buffer {
public:
  explicit DirBuffer(std::filesystem::path const& dir);

  bool readonly() const override { return true; }
  bool supportsLineNumberGutter() const override { return false; }

  std::tuple<RowIdx, ColIdx> render(std::string& out, RowIdx rows, ColIdx cols,
                                    RenderParams const& params) const override;

  /** @brief Full path for the entry at cursor position @p p. */
  std::filesystem::path selectedPath(BufferPoint p) const;

  /** @brief True if the entry at @p p is a directory. */
  bool selectedIsDir(BufferPoint p) const;

  /**
   * @brief Navigate to @p dir and rebuild the listing.
   * @return A ViewHint for the caller to position the cursor.
   */
  ViewHint navigate(std::filesystem::path const& dir);

  /**
   * @brief Toggle visibility of dotfiles.
   * @return A ViewHint that tries to keep the cursor on the same entry.
   */
  ViewHint toggleHidden(BufferPoint currentPoint);

  /** @brief Toggle the deletion mark at line @p p. A no-op on `.` and `..`. */
  void toggleDeleteMark(BufferPoint p);

  /** @brief Paths currently marked for deletion. */
  std::vector<std::filesystem::path> markedPaths() const;

  /** @brief Clear all deletion marks. */
  void clearMarks();

  /** @brief Outcome of executeDelete(). */
  struct DeleteResult {
    std::size_t count; /**< Number of entries removed. */
    std::string error; /**< Error message, empty on success. */
    ViewHint hint;     /**< Cursor placement after the refresh. */
  };
  /**
   * @brief Remove all marked entries from disk and refresh the listing.
   * @return The count removed, any error, and a cursor hint.
   */
  DeleteResult executeDelete();

  /** @brief Outcome of renameEntry() / makeSubdir(). */
  struct ChangeResult {
    bool ok;           /**< True on success. */
    std::string error; /**< Error message, populated when @c ok is false. */
    ViewHint hint;     /**< Cursor placement after the refresh. */
  };

  /**
   * @brief Rename the entry at @p p to @p newName (relative to the current
   *        directory).
   *
   * Refuses on `.` / `..`, empty or separator-bearing names, and when the
   * target already exists. Refreshes the listing on success and tries to
   * leave the cursor on the renamed entry.
   */
  ChangeResult renameEntry(BufferPoint p, std::string const& newName);

  /**
   * @brief Create a new subdirectory @p newName inside the current
   *        directory.
   *
   * Refuses on empty or separator-bearing names. Refreshes the listing and
   * returns a hint that lands the cursor on the new entry.
   */
  ChangeResult makeSubdir(std::string const& newName);

  /** @brief The directory currently being listed. */
  std::filesystem::path const& directory() const { return _dir; }

  /** @brief Whether hidden files are currently shown. */
  bool showingHidden() const { return _showHidden; }

  /** @brief Metadata for one listed filesystem entry. */
  struct Entry {
    std::string name;
    std::filesystem::path path;
    bool isDir = false;
    bool isSymlink = false;
    std::uintmax_t size = 0;
    std::string owner;
    std::string group;
    std::string permissions; /**< e.g. "drwxr-xr-x". */
    std::string mtime;       /**< e.g. "2026-03-27 14:05:12". */
  };

  /** @brief Access the entry list (for testing). */
  std::vector<Entry> const& entries() const { return _entries; }

  /** @brief Whether index @p idx is marked for deletion (for testing). */
  bool isMarked(std::size_t idx) const { return _markedForDelete.contains(idx); }

private:
  /**
   * @brief Rebuild the entry list and display lines.
   * @param prevName When non-empty, tries to place the returned cursor on
   *        the entry with that name.
   */
  ViewHint refresh(std::string const& prevName = {});

  std::filesystem::path _dir;
  bool _showHidden = false;
  std::vector<Entry> _entries;
  std::vector<std::string> _displayLines;
  std::set<std::size_t> _markedForDelete;
};

/** @brief DirBuffer implementation helpers exposed for testing. */
namespace dir_buffer_detail {
/**
 * @brief Length-preserving sanitiser for NSS-sourced fields (owner / group
 *        from getpwuid / getgrgid).
 *
 * Replaces C0 controls, DEL, and 8-bit C1 controls with '?' so they cannot
 * drive a terminal escape when the listing is written direct-to-terminal.
 * Exposed for tests; not intended for general (context-specific) use.
 */
std::string sanitizeNssField(std::string s);
} // namespace dir_buffer_detail
