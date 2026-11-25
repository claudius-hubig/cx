# Potential improvements

These ideas would improve performance or architecture. cx does not implement them yet.

## Dirty-region rendering

`render()` rebuilds the whole screen string on every keystroke. It calls `_renderBuf.clear()` and then builds the
string again from the start. A double buffer or a dirty-region approach could skip the lines that did not change.
That would cut the cost of a frame from O(rows × cols) to O(changed lines × cols). It would help on very large
terminals, and over slow SSH connections where the full write can cause visible flicker.
