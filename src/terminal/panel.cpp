#include "panel.hpp"

Panel::Panel(int id, Buffer& buf, PanelRect rect)
    : _id(id), _view(std::make_unique<BufferView>(BufferView::fromSavedState(buf))), _rect(rect) {}

RowIdx Panel::contentRows() const { return RowIdx{_rect.rows > 1 ? _rect.rows - 1 : 0}; }

ColIdx Panel::totalCols() const { return ColIdx{_rect.cols}; }

void Panel::setBuffer(Buffer& buf, bool restoreState) {
  if (restoreState)
    _view = std::make_unique<BufferView>(BufferView::fromSavedState(buf));
  else
    _view = std::make_unique<BufferView>(buf);
}

void Panel::saveViewState() { _view->saveViewState(); }
