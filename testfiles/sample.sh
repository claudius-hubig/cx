#!/usr/bin/env bash
# sample.sh — shell mode manual highlight check

set -euo pipefail

NAME="world"
single='no $expansion here'
double="hello, $NAME — status $? pid $$"
braced="${NAME:-default} and ${#NAME} chars"
sub=`date +%Y`
nested="${HOME}/bin"

# keywords, brackets, operators
if [ -n "$NAME" ] && [ "${#NAME}" -gt 0 ]; then
  for i in 1 2 3; do
    echo "iteration $i of $NAME" | tr a-z A-Z > /dev/null
  done
else
  while read -r line; do
    printf '%s\n' "$line"
  done
fi

function greet() {
  local who=$1
  echo "hi ${who:?name required}"
  return 0
}

# heredoc (expands), indented heredoc (<<-), and quoted (literal)
cat <<EOF
Plain heredoc body: $NAME is expanded here.
Not a terminator: EOFX
EOF

cat <<-INDENT
	tab-indented body, closing tag may be tab-indented
	INDENT

cat <<'LITERAL'
Literal heredoc: $NAME stays as text, `no cmd sub`.
LITERAL

# here-string (<<<) is NOT a heredoc
grep foo <<< "search within this string"

greet "$NAME"
