#pragma once

/**
 * @file panel_layout.hpp
 * @brief PanelLayout — the binary split tree that arranges panels and
 *        tracks focus.
 */

#include "panel.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <variant>
#include <vector>

class Buffer;

/** @brief Direction of a panel split. */
enum class SplitDir { Vertical, Horizontal };

/**
 * @brief Binary split tree managing a set of panels.
 *
 * Each leaf owns a Panel; internal nodes record a split direction. The
 * tree is recomputed on resize/split/close to assign each panel an
 * absolute PanelRect within the terminal.
 */
class PanelLayout {
public:
  PanelLayout(Buffer& initialBuf, std::size_t termRows, std::size_t termCols);

  /** @brief The currently focused panel. */
  Panel& focused();
  /** @brief The currently focused panel (const). */
  Panel const& focused() const;

  /** @brief Look up a panel by id, or nullptr if not found. */
  Panel* byId(int id);

  /** @brief Visit every panel in tree order. */
  void forEachPanel(std::function<void(Panel&)> const& fn);
  /** @brief Visit every panel in tree order (const). */
  void forEachPanel(std::function<void(Panel const&)> const& fn) const;

  /** @brief Total number of panels. */
  std::size_t count() const;

  /**
   * @brief Split the focused panel, placing a view of @p buf in the new
   *        panel.
   *
   * The original panel stays left/top and the new panel right/bottom; the
   * new panel does NOT become focused.
   * @return The new panel, or nullptr if the split would make panels
   *         smaller than the minimum dimensions.
   */
  Panel* splitFocused(SplitDir dir, Buffer& buf);

  /**
   * @brief Close the focused panel, promoting its sibling into the freed
   *        space.
   * @return False (a no-op) if this is the last panel.
   */
  bool closeFocused();

  /**
   * @brief Switch focus to the panel with @p id.
   * @return False if no panel has that id.
   */
  bool focusPanel(int id);

  /**
   * @brief Switch focus to the most recently used panel.
   *
   * Falls back to the first neighbour in tree order when no MRU history
   * exists.
   * @return False if there is only one panel.
   */
  bool focusMRU();

  /** @brief Recompute all panel rects after a terminal resize. */
  void resize(std::size_t termRows, std::size_t termCols);

  /** @brief Count how many panels currently show @p buf. */
  std::size_t viewCountFor(Buffer const& buf) const;

private:
  struct SplitNode;
  /** @brief Tree leaf owning a single Panel. */
  struct LeafNode {
    std::unique_ptr<Panel> panel;
  };
  using Node = std::variant<std::unique_ptr<SplitNode>, std::unique_ptr<LeafNode>>;

  /** @brief Internal tree node holding a split direction and two children. */
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

  /**
   * @brief MRU stack of previously-focused panel ids (back = most recently
   *        focused before the current panel).
   *
   * The currently focused panel is NOT in this list. Used by
   * closeFocused() to restore focus to the panel the user was at before
   * the one being closed.
   */
  std::vector<int> _focusHistory;

  /** @brief Minimum panel width (including modeline + gutter). */
  static constexpr std::size_t MinPanelCols = 10;
  /** @brief Minimum panel height (including modeline). */
  static constexpr std::size_t MinPanelRows = 3;

  /** @brief Assign absolute rects to every panel under @p node. */
  void computeRects(Node& node, PanelRect available);

  /** @brief Find the panel with @p id under @p node, or nullptr. */
  static Panel* findPanel(Node& node, int id);
  /** @brief Find the panel with @p id under @p node, or nullptr (const). */
  static Panel const* findPanel(Node const& node, int id);
  static void forEachImpl(Node& node, std::function<void(Panel&)> const& fn);
  static void forEachImpl(Node const& node, std::function<void(Panel const&)> const& fn);
  static std::size_t countImpl(Node const& node);
  static std::size_t viewCountImpl(Node const& node, Buffer const& buf);

  /**
   * @brief Remove the leaf with @p id from the subtree, promoting its
   *        sibling in place.
   * @return True if found and removed.
   */
  static bool removeLeaf(Node& node, int id);
};
