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
                                    bool showWhitespace = false, bool wrap = true) const override;

  /// Return the full path for the entry on the cursor line.
  std::filesystem::path selectedPath() const;

  /// True if the selected entry is a directory.
  bool selectedIsDir() const;

  /// Navigate to a new directory and rebuild the listing.
  void navigate(std::filesystem::path dir);

  /// Toggle visibility of dotfiles.
  void toggleHidden();

  /// Toggle deletion mark on the current line. No-op on . and .. entries.
  void toggleDeleteMark();

  /// Return the set of paths currently marked for deletion.
  std::vector<std::filesystem::path> markedPaths() const;

  /// Clear all deletion marks.
  void clearMarks();

  /// Remove all marked entries from disk. Returns the number of items
  /// deleted and an error string (empty on full success).
  std::pair<std::size_t, std::string> executeDelete();

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
  void refresh();

  std::filesystem::path _dir;
  bool _showHidden = false;
  std::vector<Entry> _entries;
  std::vector<std::string> _displayLines;
  std::set<std::size_t> _markedForDelete;
};
