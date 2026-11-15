#include "syntax.hpp"

void tokenizeDiffLine(std::string const& line, std::vector<SyntaxToken>& out) {
  if (line.empty()) {
    out.clear();
    return;
  }

  SyntaxToken tok = SyntaxToken::Normal;
  if (line.size() >= 2 && line[0] == '@' && line[1] == '@')
    tok = SyntaxToken::DiffHunk;
  else if (line.size() >= 3 && line[0] == '+' && line[1] == '+' && line[2] == '+')
    tok = SyntaxToken::DiffMeta;
  else if (line.size() >= 3 && line[0] == '-' && line[1] == '-' && line[2] == '-')
    tok = SyntaxToken::DiffMeta;
  else if (line[0] == '+')
    tok = SyntaxToken::DiffAdd;
  else if (line[0] == '-')
    tok = SyntaxToken::DiffRemove;

  out.assign(line.size(), tok);
}
