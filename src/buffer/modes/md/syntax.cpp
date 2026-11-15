#include "syntax.hpp"

void tokenizeMarkdownLine(std::string const& line, MdSyntaxState& state,
                          std::vector<SyntaxToken>& tokens) {
  tokens.assign(line.size(), SyntaxToken::Normal);
  std::size_t sz = line.size();
  std::size_t i = 0;

  // Fenced code is not Markdown: without this a "# comment" in a shell
  // snippet was a heading and an "arr[i" opened a link. The block also
  // ends whatever link was open before it.
  if (state.fence.advance(line)) {
    state.mdLink = MdLink::None;
    return;
  }

  // A blank line ends the paragraph, and with it a link that is still
  // open: link text cannot span paragraphs. Otherwise an unmatched '[' in
  // prose, as in "the interval [0, 1)", would colour everything up to the
  // next ']' anywhere in the file as link text.
  if (line.find_first_not_of(" \t") == std::string::npos) {
    state.mdLink = MdLink::None;
    return;
  }

  // Continuation of a link that began on an earlier line. A link
  // spanning the boundary cannot also be a heading, so this is
  // resolved before the heading check.
  if (state.mdLink == MdLink::Label) {
    std::size_t scan = 0;
    while (scan < sz && line[scan] != ']')
      ++scan;
    if (scan == sz) {
      // Still no ']': the whole line is link-label text.
      for (std::size_t j = 0; j < sz; ++j)
        tokens[j] = SyntaxToken::Link;
      return;
    }
    if (scan + 1 < sz && line[scan + 1] == '(') {
      // `](` — the destination begins here.
      std::size_t urlScan = scan + 2;
      while (urlScan < sz && line[urlScan] != ')')
        ++urlScan;
      if (urlScan < sz) {
        ++urlScan; // include ')'
        for (std::size_t j = 0; j < urlScan; ++j)
          tokens[j] = SyntaxToken::Link;
        state.mdLink = MdLink::None;
        i = urlScan; // resume normal scan after the link
      } else {
        for (std::size_t j = 0; j < sz; ++j)
          tokens[j] = SyntaxToken::Link;
        state.mdLink = MdLink::Url;
        return;
      }
    } else {
      // `]` not followed by `(`: the construct ends at `]`.
      std::size_t end = scan + 1; // include ']'
      for (std::size_t j = 0; j < end; ++j)
        tokens[j] = SyntaxToken::Link;
      state.mdLink = MdLink::None;
      i = end;
    }
  } else if (state.mdLink == MdLink::Url) {
    std::size_t scan = 0;
    while (scan < sz && line[scan] != ')')
      ++scan;
    if (scan == sz) {
      for (std::size_t j = 0; j < sz; ++j)
        tokens[j] = SyntaxToken::Link;
      return;
    }
    ++scan; // include ')'
    for (std::size_t j = 0; j < scan; ++j)
      tokens[j] = SyntaxToken::Link;
    state.mdLink = MdLink::None;
    i = scan;
  } else {
    // No carry-over: a leading run of '#' may be a heading.
    if (!line.empty() && line[0] == '#') {
      std::size_t h = 0;
      while (h < sz && line[h] == '#')
        ++h;
      if (h == sz || line[h] == ' ') {
        for (std::size_t j = 0; j < sz; ++j)
          tokens[j] = SyntaxToken::Heading;
        return;
      }
    }
  }

  // Inline scan for bold (**...**) and links ([text](url)), starting
  // at i (0, or just past a link that closed mid-line).
  for (; i < sz;) {
    // Inline code span: `...`. Markdown markup is not processed inside a
    // code span, so a stray '[' between backticks must not open a link —
    // otherwise an unbalanced bracket in code (e.g. `arr[i`) would set
    // MdLink state and paint every following line as link text until a
    // ']' appears. Skip to the matching closing backtick on this line.
    // A backtick with no partner is a literal character (CommonMark):
    // advance one byte and keep scanning so later markup still resolves.
    if (line[i] == '`') {
      std::size_t close = i + 1;
      while (close < sz && line[close] != '`')
        ++close;
      if (close < sz) {
        i = close + 1; // skip the whole `...` span, markup-free
        continue;
      }
      ++i;
      continue;
    }

    // Bold: **...** (single-line only).
    if (i + 1 < sz && line[i] == '*' && line[i + 1] == '*') {
      std::size_t start = i;
      bool closed = false;
      std::size_t scan = i + 2;
      while (scan + 1 < sz) {
        if (line[scan] == '*' && line[scan + 1] == '*') {
          scan += 2;
          for (std::size_t j = start; j < scan; ++j)
            tokens[j] = SyntaxToken::Bold;
          i = scan;
          closed = true;
          break;
        }
        ++scan;
      }
      if (!closed) {
        // Unclosed: leave the `**` as Normal and continue scanning past
        // it so any subsequent links/markup still highlight.
        i = start + 2;
      }
      continue;
    }

    // Link: [text](url). The label and/or destination may run off the
    // end of the line; carry the open part into MdSyntaxState so it
    // resumes on the next line. Disallow nested '[' inside the label
    // so a mismatched outer '[' does not greedily consume a real link
    // further along the line.
    if (line[i] == '[') {
      std::size_t start = i;
      std::size_t scan = i + 1;
      while (scan < sz && line[scan] != ']' && line[scan] != '[')
        ++scan;
      if (scan == sz) {
        // Label runs off the end of the line — continues next line.
        for (std::size_t j = start; j < sz; ++j)
          tokens[j] = SyntaxToken::Link;
        state.mdLink = MdLink::Label;
        return;
      }
      if (line[scan] == ']' && scan + 1 < sz && line[scan + 1] == '(') {
        std::size_t urlScan = scan + 2;
        while (urlScan < sz && line[urlScan] != ')')
          ++urlScan;
        if (urlScan < sz) {
          ++urlScan; // include ')'
          for (std::size_t j = start; j < urlScan; ++j)
            tokens[j] = SyntaxToken::Link;
          i = urlScan;
          continue;
        }
        // `](` present but `)` missing — destination continues.
        for (std::size_t j = start; j < sz; ++j)
          tokens[j] = SyntaxToken::Link;
        state.mdLink = MdLink::Url;
        return;
      }
      // `]` not followed by `(`, or a nested `[`: not a link here.
      i = start + 1;
      continue;
    }

    ++i;
  }
}
