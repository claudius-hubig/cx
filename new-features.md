# Useful general-purpose features for `cx`

A wishlist of features that would make `cx` more competitive as a
general-purpose terminal editor. Excludes additional language modes
(per the project guideline that mode work is a separate axis).

Features are grouped by theme and ranked roughly by **value-for-effort**
within each group. Each entry notes:

- **What** the feature does from a user's point of view.
- **Why** it is worth adding — what gap it fills.
- **Impl notes** — sketch of implementation surface and stdlib-only
  considerations.
- **Cost** — rough effort estimate (S = a day, M = a week, L = a month-scale).
- **Discoverability** — proposed shortcut / surface.

The whole list respects cx's hard constraints: stdlib + POSIX only, no
configuration file, simple "no surprises" defaults.

---

## A. Selection & region operations

These are wins because cx already has a strong mark/region model and
clean BufferView editing primitives. Each item is a small, self-contained
operation on the active region.

### A8. Reflow / wrap paragraph at column N (Esc-Q) · M

- **What:** rewrap the current paragraph (or region) to fit within N
  columns, preserving leading whitespace / list bullets.
- **Why:** primary value-add for Markdown and plain-text writing — the
  one feature whose absence is most felt vs. e.g. Emacs `M-q`.
- **Impl notes:** tricky around wide-character widths and list prefixes;
  reuse existing `visualWidth()` + UTF-8 helpers. Default column = 80.
- **Cost:** mid-effort because of wide-character / Markdown-prefix edge
  cases.

---

## B. Search & navigation

cx already has Find, Replace and smart-find skipping. The gaps are around
finding within larger contexts (multi-file, regex) and quick navigation.

### B2. Regex search and replace (Esc-X / Esc-Y) · M-L

- **What:** parallel command bound to a different key that uses
  `<regex>` (`std::regex`) for the pattern, with capture-group
  back-references in the replacement (`$1`, `$&`, etc.).
- **Why:** the single biggest power gap. Smart-find covers ~80% of
  ad-hoc lookups but cannot do anchored, whole-word, or class-based
  matches.
- **Impl notes:** `std::regex` is heavyweight, but it is in the
  standard library and meets cx's "stdlib only" rule. Compile per
  prompt; cache last pattern. Cost is mostly UX (clear prompt label,
  escape rules, error handling for invalid patterns).
- **Cost:** medium for search; large if combined with the same y/n/!/q
  loop as plain Replace and with capture-group substitution.

---

## C. Display & ergonomics

Small render-side changes that improve readability without altering
editing semantics. cx's renderer is full-redraw, so most of these are
straightforward additions.

### C1. Line-number gutter (Esc-#) · S-M

- **What:** toggleable column on the left of each panel showing 1-based
  line numbers, right-aligned, dim.
- **Why:** universal expected feature. Even with `Ctrl-g` for goto-line,
  having line numbers visible is valuable for diff reading and code
  review.
- **Impl notes:** widens the gutter from 1 to N columns; adjust
  `contentWidth()`, `pointForScreenRowCol()`, mouse-click hit testing,
  and `ensureVisible`. Cost is wider than it sounds because the 1-col
  gutter assumption is hard-coded in many places.

### C4. Highlight current line (Esc-_) · S

- **What:** subtle background tint on the row containing the cursor.
- **Why:** in split panels with multiple cursors, finding "where am I
  again?" is a real problem.
- **Impl notes:** trivial — set a background colour for cursor's visual
  row. Needs to coexist with diff-line backgrounds.

### C5. Centre cursor on screen (Ctrl-l) · S

- **What:** scroll so the cursor sits at vertical mid-screen.
- **Why:** classic Emacs/vim move; useful after long Find / Goto-line
  jumps.
- **Impl notes:** `BufferView::centreOnPoint(rows)`. Consecutive presses
  could cycle through top / middle / bottom (Emacs C-l style).

### C7. Per-buffer wrap / whitespace state · S

- **What:** today `_wrap` and `_showWhitespace` live on `Screen`. Move
  them per `BufferView` so different files can have different settings.
- **Why:** mixing a wrapped Markdown buffer and an unwrapped C++ buffer
  is the common case; current global setting forces a compromise.

---

## D. Persistence, safety and recovery

cx is "no configuration", but a small amount of state-on-disk would
recover huge amounts of user friction.

---

## E. Productivity

Larger features that change the editor's shape. Higher cost; bigger
return on power-user workflows.

---

## F. Smaller polish

Items too small to deserve their own category but worth a one-liner each.

| Idea                                    | Why                                                                                                  |
|-----------------------------------------|------------------------------------------------------------------------------------------------------|
| **`Esc-x` execute named command**       | Drops the need to memorise every shortcut; surfaces actions by name with completion. Show shortcut after execution in status line                |

