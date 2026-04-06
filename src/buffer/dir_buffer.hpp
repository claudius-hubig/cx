#pragma once

#include "buffer.hpp"

#include <cstdint>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

/// Readonly buffer that displays a directory listing with permissions,
/// owner, group, and size. Supports navigation into subdirectories,
/// toggling hidden files, and marking entries for deletion.
class DirBuffer : public Buffer {
public:
  explicit DirBuffer(std::filesystem::path dir);

  bool readonly() const override { return true; }

  std::tuple<RowIdx, ColIdx> render(std::string& out, RowIdx rows, ColIdx cols,
                                    RenderParams const& params) const override;

  /// Return the full path for the entry at the given cursor position.
  std::filesystem::path selectedPath(BufferPoint p) const;

  /// True if the entry at the given position is a directory.
  bool selectedIsDir(BufferPoint p) const;

  /// Navigate to a new directory and rebuild the listing.
  /// Returns a ViewHint for the caller to position the cursor.
  ViewHint navigate(std::filesystem::path dir);

  /// Toggle visibility of dotfiles. Returns a ViewHint that tries
  /// to restore the cursor on the same entry.
  ViewHint toggleHidden(BufferPoint currentPoint);

  /// Toggle deletion mark at the given line. No-op on . and .. entries.
  void toggleDeleteMark(BufferPoint p);

  /// Return the set of paths currently marked for deletion.
  std::vector<std::filesystem::path> markedPaths() const;

  /// Clear all deletion marks.
  void clearMarks();

  /// Remove all marked entries from disk. Returns count, error, and a
  /// ViewHint for cursor positioning after the listing is refreshed.
  struct DeleteResult {
    std::size_t count;
    std::string error;
    ViewHint hint;
  };
  DeleteResult executeDelete();

  /// The directory currently being listed.
  std::filesystem::path const& directory() const { return _dir; }

  /// Whether hidden files are currently shown.
  bool showingHidden() const { return _showHidden; }

  struct Entry {
    std::string name;
    std::filesystem::path path;
    bool isDir = false;
    bool isSymlink = false;
    std::uintmax_t size = 0;
    std::string owner;
    std::string group;
    std::string permissions; // e.g. "drwxr-xr-x"
    std::string mtime;       // e.g. "2026-03-27 14:05:12"
  };

  /// Access entries (for testing).
  std::vector<Entry> const& entries() const { return _entries; }

  /// Check if an index is marked for deletion (for testing).
  bool isMarked(std::size_t idx) const { return _markedForDelete.count(idx) > 0; }

private:
  /// Rebuild the entry list and display lines. If `prevName` is non-empty,
  /// tries to place the returned cursor on the entry with that name.
  ViewHint refresh(std::string const& prevName = {});

  std::filesystem::path _dir;
  bool _showHidden = false;
  std::vector<Entry> _entries;
  std::vector<std::string> _displayLines;
  std::set<std::size_t> _markedForDelete;
};
