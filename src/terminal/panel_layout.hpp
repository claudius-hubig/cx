#pragma once

#include "panel.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <variant>
#include <vector>

class Buffer;

enum class SplitDir { Vertical, Horizontal };

/// Binary split tree managing a set of panels.
///
/// Each leaf owns a Panel. Internal nodes record a split direction and
/// ratio. The tree is recomputed on resize/split/close to assign each
/// panel an absolute PanelRect within the terminal.
class PanelLayout {
public:
  PanelLayout(Buffer& initialBuf, std::size_t termRows, std::size_t termCols);

  /// The currently focused panel.
  Panel& focused();
  Panel const& focused() const;

  /// Look up a panel by its id. Returns nullptr if not found.
  Panel* byId(int id);

  /// Visit every panel in tree order.
  void forEachPanel(std::function<void(Panel&)> const& fn);
  void forEachPanel(std::function<void(Panel const&)> const& fn) const;

  /// Total number of panels.
  std::size_t count() const;

  /// Split the focused panel, placing a view of `buf` in the new panel.
  /// Returns the new panel. The new panel does NOT become focused.
  /// Returns nullptr if the split would make panels too small.
  Panel* splitFocused(SplitDir dir, Buffer& buf);

  /// Close the focused panel, merging its space with a neighbor.
  /// Returns false (no-op) if this is the last panel.
  bool closeFocused();

  /// Switch focus to the panel with the given id.
  /// Returns false if no panel has that id.
  bool focusPanel(int id);

  /// Recompute all panel rects after a terminal resize.
  void resize(std::size_t termRows, std::size_t termCols);

  /// Count how many panels currently show the given buffer.
  std::size_t viewCountFor(Buffer const& buf) const;

private:
  struct SplitNode;
  struct LeafNode {
    std::unique_ptr<Panel> panel;
  };
  using Node = std::variant<std::unique_ptr<SplitNode>, std::unique_ptr<LeafNode>>;

  struct SplitNode {
    SplitDir dir;
    Node left;
    Node right;
  };

  Node _root;
  int _focusedId = 1;
  int _nextId = 2;
  std::size_t _termRows = 0;
  std::size_t _termCols = 0;

  // MRU stack of previously-focused panel ids (back = most recently
  // focused before the current panel). The currently focused panel is
  // NOT in this list. Used by closeFocused to restore focus to the
  // panel the user was at before the one being closed.
  std::vector<int> _focusHistory;

  // Minimum panel dimensions (including modeline + gutter).
  static constexpr std::size_t MinPanelCols = 10;
  static constexpr std::size_t MinPanelRows = 3;

  void computeRects(Node& node, PanelRect available);

  // Find the leaf containing the focused panel and return a pointer
  // to the Node slot that holds it. Returns nullptr if not found.
  static Panel* findPanel(Node& node, int id);
  static Panel const* findPanel(Node const& node, int id);
  static void forEachImpl(Node& node, std::function<void(Panel&)> const& fn);
  static void forEachImpl(Node const& node, std::function<void(Panel const&)> const& fn);
  static std::size_t countImpl(Node const& node);
  static std::size_t viewCountImpl(Node const& node, Buffer const& buf);

  // Remove the leaf with the given id from the subtree, promoting its
  // sibling in place. Returns true if found and removed.
  static bool removeLeaf(Node& node, int id);
};
