# Potential Improvements

Ideas for future work that would improve performance or architecture but
are not yet implemented.

## Dirty-region rendering

`render()` rebuilds the entire screen string on every keystroke via
`_renderBuf.clear()` followed by a full reconstruction. A double-buffer
or dirty-region approach could skip re-rendering unchanged lines, reducing
the per-frame cost from O(rows × cols) to O(changed lines × cols). This
would help on very large terminals or over slow SSH connections where the
full write can cause visible flicker.
