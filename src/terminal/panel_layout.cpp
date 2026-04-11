#include "panel_layout.hpp"

#include <algorithm>

PanelLayout::PanelLayout(Buffer& initialBuf, std::size_t termRows, std::size_t termCols)
    : _termRows(termRows), _termCols(termCols) {
  auto leaf = std::make_unique<LeafNode>();
  leaf->panel = std::make_unique<Panel>(1, initialBuf, PanelRect{0, 0, termRows, termCols});
  _root = std::move(leaf);
}

Panel& PanelLayout::focused() {
  auto* p = findPanel(_root, _focusedId);
  return *p;
}

Panel const& PanelLayout::focused() const {
  auto* p = findPanel(_root, _focusedId);
  return *p;
}

Panel* PanelLayout::byId(int id) { return findPanel(_root, id); }

void PanelLayout::forEachPanel(std::function<void(Panel&)> const& fn) { forEachImpl(_root, fn); }

void PanelLayout::forEachPanel(std::function<void(Panel const&)> const& fn) const {
  forEachImpl(_root, fn);
}

std::size_t PanelLayout::count() const { return countImpl(_root); }

Panel* PanelLayout::splitFocused(SplitDir dir, Buffer& buf) {
  // Check that the split won't produce panels that are too small.
  auto& fp = focused();
  auto rect = fp.rect();
  if (dir == SplitDir::Vertical) {
    if (rect.cols < MinPanelCols * 2)
      return nullptr;
  } else {
    if (rect.rows < MinPanelRows * 2)
      return nullptr;
  }

  int newId = _nextId++;

  // We need to find the leaf node holding the focused panel, replace it
  // with a split node containing the old leaf + a new leaf.
  // Walk the tree to find the parent slot.
  struct Finder {
    int targetId;
    Node* result = nullptr;
    void walk(Node& node) {
      if (auto* leaf = std::get_if<std::unique_ptr<LeafNode>>(&node)) {
        if ((*leaf)->panel->id() == targetId)
          result = &node;
      } else {
        auto& split = std::get<std::unique_ptr<SplitNode>>(node);
        walk(split->left);
        if (!result)
          walk(split->right);
      }
    }
  };

  Finder finder{_focusedId};
  finder.walk(_root);
  if (!finder.result)
    return nullptr;

  // Extract the existing leaf from the slot.
  auto existingLeaf = std::move(std::get<std::unique_ptr<LeafNode>>(*finder.result));

  // Create new leaf.
  auto newLeaf = std::make_unique<LeafNode>();
  newLeaf->panel = std::make_unique<Panel>(newId, buf, PanelRect{});

  // Create split node.
  auto splitNode = std::make_unique<SplitNode>();
  splitNode->dir = dir;
  splitNode->left = std::move(existingLeaf);
  splitNode->right = std::move(newLeaf);

  // Place the split node into the slot.
  *finder.result = std::move(splitNode);

  // Recompute rects.
  computeRects(_root, PanelRect{0, 0, _termRows, _termCols});

  return findPanel(_root, newId);
}

bool PanelLayout::closeFocused() {
  if (countImpl(_root) <= 1)
    return false;

  int closedId = _focusedId;

  // Remove the focused leaf, promoting its sibling.
  removeLeaf(_root, closedId);

  // Drop the closed panel from history if it somehow appears there
  // (shouldn't, since the focused panel is never in history, but guard
  // against id reuse).
  _focusHistory.erase(std::remove(_focusHistory.begin(), _focusHistory.end(), closedId),
                      _focusHistory.end());

  // Walk the MRU history from the back, picking the most recently
  // focused panel that still exists.
  int newFocus = 0;
  while (!_focusHistory.empty()) {
    int candidate = _focusHistory.back();
    _focusHistory.pop_back();
    if (findPanel(_root, candidate)) {
      newFocus = candidate;
      break;
    }
  }

  // Fallback: if history is empty or stale, focus the first panel in
  // tree order.
  if (newFocus == 0) {
    forEachPanel([&](Panel& p) {
      if (newFocus == 0)
        newFocus = p.id();
    });
  }
  _focusedId = newFocus;

  computeRects(_root, PanelRect{0, 0, _termRows, _termCols});
  return true;
}

bool PanelLayout::focusPanel(int id) {
  if (!findPanel(_root, id))
    return false;
  if (id == _focusedId)
    return true;
  // Push the previously focused id to the back of the MRU history,
  // removing any earlier occurrence first.
  _focusHistory.erase(std::remove(_focusHistory.begin(), _focusHistory.end(), _focusedId),
                      _focusHistory.end());
  _focusHistory.push_back(_focusedId);
  _focusedId = id;
  return true;
}

void PanelLayout::resize(std::size_t termRows, std::size_t termCols) {
  _termRows = termRows;
  _termCols = termCols;
  computeRects(_root, PanelRect{0, 0, termRows, termCols});
}

std::size_t PanelLayout::viewCountFor(Buffer const& buf) const { return viewCountImpl(_root, buf); }

// --- Tree traversal helpers ---

void PanelLayout::computeRects(Node& node, PanelRect available) {
  if (auto* leaf = std::get_if<std::unique_ptr<LeafNode>>(&node)) {
    (*leaf)->panel->setRect(available);
    return;
  }
  auto& split = std::get<std::unique_ptr<SplitNode>>(node);
  if (split->dir == SplitDir::Vertical) {
    std::size_t leftCols = available.cols / 2;
    std::size_t rightCols = available.cols - leftCols;
    computeRects(split->left, {available.row, available.col, available.rows, leftCols});
    computeRects(split->right,
                 {available.row, available.col + leftCols, available.rows, rightCols});
  } else {
    std::size_t topRows = available.rows / 2;
    std::size_t bottomRows = available.rows - topRows;
    computeRects(split->left, {available.row, available.col, topRows, available.cols});
    computeRects(split->right,
                 {available.row + topRows, available.col, bottomRows, available.cols});
  }
}

Panel* PanelLayout::findPanel(Node& node, int id) {
  if (auto* leaf = std::get_if<std::unique_ptr<LeafNode>>(&node))
    return (*leaf)->panel->id() == id ? (*leaf)->panel.get() : nullptr;
  auto& split = std::get<std::unique_ptr<SplitNode>>(node);
  if (auto* p = findPanel(split->left, id))
    return p;
  return findPanel(split->right, id);
}

Panel const* PanelLayout::findPanel(Node const& node, int id) {
  if (auto* leaf = std::get_if<std::unique_ptr<LeafNode>>(&node))
    return (*leaf)->panel->id() == id ? (*leaf)->panel.get() : nullptr;
  auto& split = std::get<std::unique_ptr<SplitNode>>(node);
  if (auto* p = findPanel(split->left, id))
    return p;
  return findPanel(split->right, id);
}

void PanelLayout::forEachImpl(Node& node, std::function<void(Panel&)> const& fn) {
  if (auto* leaf = std::get_if<std::unique_ptr<LeafNode>>(&node)) {
    fn(*(*leaf)->panel);
    return;
  }
  auto& split = std::get<std::unique_ptr<SplitNode>>(node);
  forEachImpl(split->left, fn);
  forEachImpl(split->right, fn);
}

void PanelLayout::forEachImpl(Node const& node, std::function<void(Panel const&)> const& fn) {
  if (auto* leaf = std::get_if<std::unique_ptr<LeafNode>>(&node)) {
    fn(*(*leaf)->panel);
    return;
  }
  auto& split = std::get<std::unique_ptr<SplitNode>>(node);
  Node const& left = split->left;
  Node const& right = split->right;
  forEachImpl(left, fn);
  forEachImpl(right, fn);
}

std::size_t PanelLayout::countImpl(Node const& node) {
  if (std::holds_alternative<std::unique_ptr<LeafNode>>(node))
    return 1;
  auto& split = std::get<std::unique_ptr<SplitNode>>(node);
  return countImpl(split->left) + countImpl(split->right);
}

std::size_t PanelLayout::viewCountImpl(Node const& node, Buffer const& buf) {
  if (auto* leaf = std::get_if<std::unique_ptr<LeafNode>>(&node))
    return (&(*leaf)->panel->buffer() == &buf) ? 1 : 0;
  auto& split = std::get<std::unique_ptr<SplitNode>>(node);
  return viewCountImpl(split->left, buf) + viewCountImpl(split->right, buf);
}

bool PanelLayout::removeLeaf(Node& node, int id) {
  auto* split = std::get_if<std::unique_ptr<SplitNode>>(&node);
  if (!split)
    return false; // leaf node — can't remove from here

  // Check if left child is the target leaf.
  if (auto* leftLeaf = std::get_if<std::unique_ptr<LeafNode>>(&(*split)->left)) {
    if ((*leftLeaf)->panel->id() == id) {
      // Extract sibling before assigning to node (which destroys the SplitNode).
      Node promoted = std::move((*split)->right);
      node = std::move(promoted);
      return true;
    }
  }
  // Check if right child is the target leaf.
  if (auto* rightLeaf = std::get_if<std::unique_ptr<LeafNode>>(&(*split)->right)) {
    if ((*rightLeaf)->panel->id() == id) {
      Node promoted = std::move((*split)->left);
      node = std::move(promoted);
      return true;
    }
  }

  // Recurse into children.
  return removeLeaf((*split)->left, id) || removeLeaf((*split)->right, id);
}
