#!/usr/bin/env python3
"""End-to-end PTY fuzzer for cx.

Each run: fresh dir (FUZZ_ROOT + HOME), random input file(s), random terminal
size, random key-macro sequence with random pacing and resizes. Detects:
crash signals, UBSan / libstdc++-debug aborts, uncaught exceptions
(~/.cx-errors), hangs (sustained CPU after input stops -> SIGABRT for a
backtrace), runaway memory, and guard denials. Saves a replayable JSON case
for every finding.

usage: e2e_fuzz.py CX_BINARY OUTDIR SECONDS [SEED]
       e2e_fuzz.py --replay CASE.json CX_BINARY OUTDIR
"""
import fcntl, json, os, random, select, shutil, signal, struct, sys, termios, time, resource

HERE = os.path.dirname(os.path.abspath(__file__))
GUARD = os.path.abspath(os.environ.get("FUZZ_GUARD") or os.path.join(HERE, "..", "..", "build", "fuzz", "bin", "guard.so"))
ASAN = os.environ.get("FUZZ_ASAN") == "1"
ASAN_LIB = "/usr/lib/gcc/x86_64-linux-gnu/16/libasan.so"
RSS_LIMIT_MB = 3500
BIG = os.environ.get("FUZZ_BIG") == "1"  # favour large / long-line / many-line files
BIG_WEIGHTS = [10, 5, 15, 20, 0, 0, 5, 5, 15, 15, 10]

ESC = "\x1b"
CSI = ESC + "["

# ---------------------------------------------------------------- content --
UTF8_BITS = ["\u0085", "\u202e", "\u0301", "\u0301\u0302\u0303", "\U0001F1E9\U0001F1EA", "👨\u200d👩\u200d👧", "\u1100\u1161", "\u00ad", "\u009b31m", "é", "ß", "中文", "日本", "😀", "👍🏽", "e\u0301", "\u200d", "\u200b", "\ufeff",
             "\u0000", "\t", "　", "Ａ", "\U0010ffff", "\u2028", "ﷺ", "𝕏"]
BAD_UTF8 = [b"\xff", b"\xc3", b"\xe2\x82", b"\xf0\x9f\x98", b"\xc0\xaf", b"\xed\xa0\x80",
            b"\xf4\x90\x80\x80", b"\xf8\x88\x80\x80\x80", b"\x80\x80", b"\xfe"]

SNIPPETS = {
    ".cpp": ["#include <x>\n", "int main() {\n", "}\n", "/* block\n", "*/\n", 'R"d(raw\n', ')d"\n',
             "template <typename T> struct S<T>;\n", "#define X \\\n", "  a < b > c;\n",
             "// comment\n", 'auto s = "str\\"ing";\n', "char c = '\\'';\n", "if (a) { [x](){}; }\n",
             "0x1F'00ull 1e-5 .5f\n", "<<<<<<< HEAD\n", "=======\n", ">>>>>>> b\n"],
    ".md": ["# Head\n", "**bold** text\n", "[link](http://x)\n", "[multi\n", "line](url\n", ")\n",
            "```\ncode\n```\n", "- item\n", "> quote\n", "***\n", "[](\n",
            "| a | b | c |\n", "|---|:-:|--:|\n", "| 1 | two words here and more words to wrap around | 3 |\n",
            "|  | continuation text | |\n", "| | | |\n", "> | q | r |\n", "> |---|---|\n", "> | 中文 | `a|b` |\n",
            "| x \\| y | [link text](http://u/|) |\n", "a | b\n", "-|-\n", "~~~\n", "    ```\n", "|\n", "||\n", "|-|\n",
            "| e\u0301 | 😀😀 | \u0085 |\n"],
    ".json": ['{"a": [1, 2, {"b": null}],\n', '"s": "x\\"y",\n', "true, false, -1.5e3\n", "]}\n",
              "{{{{[[[[\n", '"unterminated\n'],
    ".diff": ["--- a/x\n", "+++ b/x\n", "@@ -1,3 +1,4 @@\n", "+added\n", "-removed\n", " ctx\n",
              "@@ garbage\n", "\\ No newline at end of file\n"],
    ".sh": ["#!/bin/sh\n", "cat <<EOF\n", "body $X\n", "EOF\n", "cat <<-'T'\n", "\tT\n", "x=${y:-z}\n",
            "if [ -f a ]; then echo 'q'; fi\n", "echo `date` $? $$\n", "cat <<<here\n", "<<\n"],
    ".yml": ["key: value\n", "list:\n", "  - a\n", "  - b: c\n", "text: |\n", "  block\n", "\n",
             "folded: >-\n", "   more\n", "anchor: &a x\n", "ref: *a\n", "--- \n", "...\n", "%YAML 1.2\n",
             "colors: [#fff, #000]\n", 'q: "x"#y\n', "{k: v}#c\n", "? complex\n", "- - - x\n", "|\n"],
    ".toml": ["[table]\n", "[[arr]]\n", 'key = "v"\n', "a.b.'c' = 1\n", 'ml = """\n', "text\n",
              '"""\n', "lit = '''\n", "'''\n", "arr = [\n", "  [1, 2],\n", "]\n",
              "dt = 1979-05-27 07:32:00Z\n", "inl = { x = 1, y = [ {z = 2} ] }\n", "[\n", "{\n",
              "x = 0xDEAD_BEEF\n", 'k = """a""""\n'],
    ".py": ["def f(x):\n", "    return x\n", '"""doc\n', 'more"""\n', "r'''raw\\'\n", "'''\n",
            "@decorator\n", "class C:\n", "    pass\n", "x = 0x1_0 + 1.5j\n", 'f"{x!r}"\n',
            "# comment\n", "lambda: None\n"],
    ".txt": ["hello world\n", "\n", "   \n", "\t\tindented\n", "a" * 300 + "\n", "<<<<<<< ours\n",
             "=======\n", ">>>>>>> theirs\n", "(([[{{\n", "}}]]))\n", "word, word; word.\n"],
}
EXTS = list(SNIPPETS) + [".h", ".yaml", ".patch", ".bash", ".pyi", ""]


def gen_content(rng, ext):
    key = {".h": ".cpp", ".yaml": ".yml", ".patch": ".diff", ".bash": ".sh", ".pyi": ".py", "": ".txt"}.get(ext, ext)
    kind = rng.choices(["snip", "binary", "long", "many", "empty", "nl", "mixed", "crlf", "huge_line", "bigfile", "bigtable"],
                       BIG_WEIGHTS if BIG else [50, 5, 5, 4, 4, 3, 15, 8, 2, 2, 3])[0]
    if kind == "bigfile":
        unit = "".join(rng.choice(SNIPPETS[key]) for _ in range(50)).encode()
        return unit * (rng.randrange(20000, 80000) if BIG else rng.randrange(2000, 12000))
    if kind == "bigtable":
        ncol = rng.randrange(1, 12)
        rows = ["|" + "|".join(rng.choice(["a", "", " ", "long cell text " * rng.randrange(1, 8), "中文", "`c|d`", "x\\|y", "e\u0301"]) for _ in range(rng.randrange(max(1, ncol - 2), ncol + 2))) + rng.choice(["|", ""]) for _ in range(rng.randrange(1, 3000))]
        rows.insert(1, "|" + "|".join(rng.choice(["---", ":-:", "-", "--:", ":--"]) for _ in range(ncol)) + "|")
        return (rng.choice(["", "> ", "  ", "\ufeff"]) + "\n".join(rows) + rng.choice(["", "\n", "\n\ntext\n"])).encode()
    if kind == "empty":
        return b""
    if kind == "nl":
        return rng.choice([b"\n", b"\n\n\n", b"\r\n", b"\r", b"x"])
    if kind == "binary":
        return bytes(rng.randrange(256) for _ in range(rng.randrange(1, 4000)))
    if kind == "long":
        return ("x" * rng.randrange(1000, 20000) + "\n").encode() * rng.randrange(1, 4)
    if kind == "huge_line":
        return (rng.choice("ab 中😀\t") * rng.randrange(50000, 200000)).encode()
    if kind == "many":
        n = rng.randrange(2000, 60000)
        return "".join(rng.choice(SNIPPETS[key]) for _ in range(n)).encode()
    parts = []
    for _ in range(rng.randrange(1, 200)):
        r = rng.random()
        if r < 0.7:
            parts.append(rng.choice(SNIPPETS[key]).encode())
        elif r < 0.85:
            parts.append(rng.choice(UTF8_BITS).encode())
        elif r < 0.95:
            parts.append(rng.choice(BAD_UTF8))
        else:
            parts.append(rng.choice(SNIPPETS[rng.choice(list(SNIPPETS))]).encode())
    data = b"".join(parts)
    if kind == "crlf":
        data = data.replace(b"\n", rng.choice([b"\r\n", b"\r"]))
    if rng.random() < 0.3:
        data = data.rstrip(b"\n")
    return data


# ------------------------------------------------------------------- keys --
def ctrl(c):
    return chr(ord(c) & 0x1F)


SEARCH_TERMS = ["a", "x", "the", "", " ", "中", "😀", "\\", ".", "e\u0301", "aaaa", "(", "#", "\t", "Ab",
                "ab" * 50, "\x7f", "é", "[", "\"\"\"", "key"]
REGEXES = ["a", ".", ".*", "^", "$", "^$", "(a*)*b", "(x+x+)+y", "[", "(", "\\", "a{1000}", "\\b",
           "(?=a)", "\\1", "(a)|b", "[^\\n]", "", "x*", "\\w+", ".{0}", "(.)\\1", "[[:alpha:]]", "a|",
           "*", "+?", "(?:", "\\d{2,}"]
REGEX_REPLACE = ["a", ".", "(a)|b", "[^\\n]", "\\w+", "(.)\\1", "[[:alpha:]]", "\\d{2,}", "(\\w)(\\w)", "é", "[", "("]
REGEX_FIND_SAFE = [r for r in REGEXES if r not in ("(a*)*b", "(x+x+)+y", "a{1000}")]
REPLACEMENTS = ["", "b", "$1", "\\1", "&", "$&", "$0", "$$", "$9", "\\n", "\n", "中文😀", "x" * 200, "$`",
                "$'", "\\", "$"]
GOTO = ["1", "0", "-1", "99999999999999999999999", "abc", "", "5", "100000", "1:5", " 3", "+2", "2x", "18446744073709551616"]
MODES = ["txt", "c", "cpp", "md", "jsn", "json", "dif", "sh", "yml", "tml", "toml", "py", "zz", "", "C"]
SHELL = ["sort", "tr a-z A-Z", "cat", "false", "rev", "yes | head -c 3000000", "printf '\\033[2J\\033]0;x\\007\\r\\n'",
         "printf 'a\\0b\\377\\n'", "sleep 3", "echo err >&2", "echo err >&2; exit 1", "head -c 200000 /dev/urandom",
         "head -c 0", "notwhitelisted"]
COMMANDS = ["Paste", "Save file", "Toggle line wrap", "Reflow paragraph (80 cols)", "Format buffer/region",
            "Toggle comment", "Uppercase region/word", "Add cursor on line below", "Add cursor on line above",
            "Cursor at every occurrence", "Split panel vertical", "Close panel", "Undo / version history",
            "Toggle insert/overwrite mode", "Directory browser", "Buffer list", "Paste ring",
            "Highlight current line", "Toggle line-number gutter", "Next block (hunk/conflict)",
            "Delete current line", "Kill buffer", "nonsense", "", "S"]
FILES = ["f", "new.txt", ".", "..", "sub", "sub/", "sub/inner.md", "", " ", "nonexist/x", "f:3", "~",
         "中文.py", "a\x01b", "/dev/null", "/dev/zero", "/proc/self/status", "-", "sub/../f"]


def rand_text(rng, n=None):
    n = n if n is not None else rng.randrange(1, 12)
    out = []
    for _ in range(n):
        r = rng.random()
        if r < 0.75:
            out.append(rng.choice("abcdefghijklmnopqrstuvwxyz ABC012(){}[]<>\"'#;:.,-_=+*/\\$&|!?%^~`@"))
        elif r < 0.9:
            out.append(rng.choice(UTF8_BITS).replace("\u0000", ""))
        else:
            out.append(rng.choice(["\t", " ", "  "]))
    return "".join(out)


def prompt_end(rng):
    return rng.choices(["\r", ESC, ctrl("g"), "\r\r", ""], [70, 10, 8, 5, 7])[0]


def prompt_edit(rng):
    """Random line-editing keys inside a prompt."""
    keys = [CSI + "D", CSI + "C", CSI + "H", CSI + "F", "\x7f", ctrl("a"), ctrl("e"), ctrl("k"), ctrl("w"),
            ctrl("y"), ctrl(" "), ESC + "w", CSI + "1;5D", CSI + "1;5C", CSI + "3~", CSI + "A", CSI + "B",
            "\t", ctrl("u"), ESC + "b", ESC + "f", ESC + "\x7f", ctrl("h"), ctrl("d")]
    return "".join(rng.choice(keys) for _ in range(rng.randrange(0, 4)))


def mouse(rng, rows, cols):
    b = rng.choice([0, 1, 2, 3, 32, 33, 34, 35, 64, 65, 66, 67, 4, 8, 16, 96, 128, 255, 999999])
    x = rng.choice([1, cols, cols + 1, rng.randrange(1, max(2, cols + 5)), 0, 99999, 2**31])
    y = rng.choice([1, rows, rows + 1, rng.randrange(1, max(2, rows + 5)), 0, 99999, 2**32 + 1])
    tail = rng.choice(["M", "m", "M", "", "X"])
    s = f"{CSI}<{b};{x};{y}{tail}"
    if rng.random() < 0.05:
        s = CSI + "<" + rng.choice(["", ";", ";;", "a;b;cM", "1;2", "1;2;3;4M", "-1;-1;-1M", "1" * 50 + ";1;1M"])
    return s


def macro(rng, rows, cols):
    """Return one chunk of input (a 'key' or meaningful sequence)."""
    m = rng.choices([
        "text", "nav", "edit", "ctrl", "esc", "cx", "find", "rfind", "replace", "regex", "rreplace",
        "goto", "mode", "pipe", "cmd", "open", "dir", "grep", "history", "mouse", "garbage_csi",
        "paste_burst", "region", "multicursor", "bookmark", "split", "quit", "savas", "kill", "random_bytes", "table", "undo",
    ], [30, 25, 12, 6, 6, 5, 5, 2, 4, 3, 3, 3, 2, 1, 2, 2, 2, 1, 2, 4, 2, 1, 5, 3, 1, 3, 0.4, 1, 0.05, 2, 6, 4])[0]
    r = rng
    if m == "table":
        return r.choice(["", "|", "| a | b |\r|---|---|\r| ", "\r|x|y|\r|-|-|\r||" + rand_text(r, 30) + "|\r", ctrl(" ") + CSI + "B" * r.randrange(1, 9)]) + \
            "".join(r.choice([CSI + "A", CSI + "B", CSI + "C" * r.randrange(1, 30), CSI + "1;5C", ESC + "q", ESC + "q", ESC + "f"]) for _ in range(r.randrange(1, 8))) + ESC + "q"
    if m == "undo":
        return ctrl("u") + "[" * r.randrange(0, 12) + "]" * r.randrange(0, 5) + r.choice([ctrl("u"), "\r", "q", ESC, ""])
    r = rng
    if m == "text":
        return rand_text(r)
    if m == "nav":
        return r.choice([CSI + "A", CSI + "B", CSI + "C", CSI + "D", CSI + "H", CSI + "F", CSI + "5~", CSI + "6~",
                         CSI + "1;5A", CSI + "1;5B", CSI + "1;5C", CSI + "1;5D", CSI + "1;5H", CSI + "1;5F",
                         ctrl("a"), ctrl("e"), ctrl("l"), ctrl("n"), ctrl("b"), ESC + "<", ESC + ">", ESC + "v",
                         ctrl("v")]) * r.choice([1, 1, 1, 3, 20])
    if m == "edit":
        return r.choice(["\r", "\t", "\x7f", CSI + "3~", CSI + "3;5~", ctrl("h"), ESC + "\x7f", ctrl("k"), ctrl("d"),
                         CSI + "2~", ESC + "U", ESC + "L", ESC + "C", ESC + ";", ESC + "q", ESC + "f"]) * r.choice([1, 1, 4])
    if m == "ctrl":
        return chr(r.choice([c for c in range(0, 32) if c not in (0x1b,)]))
    if m == "esc":
        return ESC + r.choice("?bcrfgslwyp%;ULCmdx_qXY#*" + "aeiknotuvzABDEFGHIJKMNOPQRSTVWZ0123456789!@$^&()-=+[]{}|\\:'\",<.>/`~ " + ESC)
    if m == "cx":
        return ctrl("x") + r.choice("vh.o123456bjm" + "aczq9" + ctrl("x") + ESC + ctrl("c") + ctrl("s") + ctrl("f"))
    if m == "find":
        return (ctrl("f") + prompt_edit(r) + r.choice(SEARCH_TERMS) + prompt_edit(r) +
                r.choice(["", ctrl("f"), ctrl("f") * 5, ctrl("r"), ctrl("r") * 3]) + prompt_end(r))
    if m == "rfind":
        return ctrl("r") + r.choice(SEARCH_TERMS) + r.choice(["", ctrl("r"), ctrl("f")]) + prompt_end(r)
    if m == "replace":
        conf = "".join(r.choice("yn!q yx\r" + ESC) for _ in range(r.randrange(0, 8)))
        return ESC + "%" + r.choice(SEARCH_TERMS) + prompt_end(r) + r.choice(REPLACEMENTS + SEARCH_TERMS) + prompt_end(r) + conf
    if m == "regex":
        return ESC + "X" + r.choice(REGEX_FIND_SAFE) + prompt_end(r) + r.choice(["", ESC + "X\r", ctrl("f")])
    if m == "rreplace":
        conf = "".join(r.choice("yn!q") for _ in range(r.randrange(0, 8)))
        return ESC + "Y" + r.choice(REGEX_REPLACE) + prompt_end(r) + r.choice(REPLACEMENTS) + prompt_end(r) + conf
    if m == "goto":
        return ctrl("g") + r.choice(GOTO) + prompt_end(r)
    if m == "mode":
        return ESC + "m" + r.choice(MODES) + prompt_end(r)
    if m == "pipe":
        return ctrl("p") + r.choice(SHELL) + "\r" + r.choice(["", "", "y", "n", ESC])
    if m == "cmd":
        return ESC + "x" + r.choice(COMMANDS) + r.choice(["", "\t", "\t\t"]) + prompt_end(r)
    if m == "open":
        return ctrl("o") + r.choice(FILES) + r.choice(["", "\t", "\t\t", CSI + "A"]) + prompt_end(r)
    if m == "dir":
        keys = "".join(r.choice(["d", "x", "y", "\r", "h", "r", "m", "q", CSI + "A", CSI + "B", "n", ESC])
                       for _ in range(r.randrange(1, 10)))
        return ESC + "d" + keys + r.choice(["", rand_text(r, 3) + "\r"])
    if m == "grep":
        return ESC + "g" + r.choice(SEARCH_TERMS + ["-e", "--help", "[", "\\("]) + "\r" + r.choice(["", "\r", CSI + "B\r", "q"])
    if m == "history":
        return ctrl("u") + "".join(r.choice("[]q\r" + ESC) for _ in range(r.randrange(1, 10)))
    if m == "mouse":
        return mouse(r, rows, cols)
    if m == "garbage_csi":
        return r.choice([CSI, CSI + "999999999999~", CSI + "1;", CSI + "1;5", CSI + "3;", CSI + "200~", CSI + "201~",
                         CSI + "?1049h", ESC + "O", ESC + "OA", CSI + "1;9Z", CSI + "M   ", CSI + "<", CSI + "2;5~",
                         CSI + "1;6A", CSI + "1;6B", CSI + "1;2A", CSI + "15~", CSI + "\x7f", ESC + "]0;t\x07",
                         CSI + "".join(r.choice("0123456789;") for _ in range(30)) + "~"])
    if m == "paste_burst":
        return rand_text(r, r.randrange(200, 5000)).replace("\t", "\r")
    if m == "region":
        moves = "".join(r.choice([CSI + "B", CSI + "C", CSI + "1;5C", CSI + "6~"]) for _ in range(r.randrange(0, 6)))
        return ctrl(" ") + moves + r.choice([ctrl("w"), ESC + "w", "\x7f", ESC + "U", ESC + ";", ESC + "f", ESC + "q",
                                             ctrl("p") + r.choice(SHELL) + "\r", ESC + "*", ctrl(" ")])
    if m == "multicursor":
        return r.choice([CSI + "1;6A", CSI + "1;6B", ESC + "*"]) * r.randrange(1, 6) + rand_text(r, 3) + r.choice(["", "\x7f\x7f", ESC, ctrl("y"), "\r"])
    if m == "bookmark":
        return ctrl("x") + r.choice("bj") + r.choice("0a9z\r" + ESC + "!中")
    if m == "split":
        return ctrl("x") + r.choice("vh.o123456")
    if m == "quit":
        return ESC + "c" + r.choice(["n", "y", ESC, "\r", ""])
    if m == "savas":
        return ctrl("s") + r.choice(["", "saved.txt\r", "sub\r", "\r", "f\ry", "f\rn", ESC])
    if m == "kill":
        return ctrl("x") + "k" + r.choice(["", "y", "n", ESC, "\r"])
    if m == "random_bytes":
        return bytes(r.randrange(256) for _ in range(r.randrange(1, 20))).decode("utf-8", "surrogateescape")
    return ""


# ----------------------------------------------------------------- runner --
import re
_ALLOWED = re.compile(rb"\x1b\[[0-?]*[ -/]*[@-~]|\x1b\]0;[^\x07\x1b]*\x07")
_BAD = re.compile(rb"[\x00-\x1f\x7f]|\xc2[\x80-\x9f]|\xe2\x80[\xa8-\xae]|\xe2\x81[\xa6-\xa9]")


def term_leak(stream):
    """Return a snippet if the terminal stream holds a control byte that is
    not part of a CSI sequence or the title OSC (content must never reach
    the terminal raw)."""
    body = bytes(stream[:-64]) if len(stream) > 64 else b""
    end = body.find(b"\x1b[?1049l")  # cx left the alternate screen: the rest is tty echo
    if end >= 0:
        body = body[:end]
    cut = body.rfind(b"\x1b")  # the cut may fall inside a sequence
    if cut >= 0 and cut >= len(body) - 48:
        body = body[:cut]
    pos = 0
    for m in _ALLOWED.finditer(body):
        b = _BAD.search(body, pos, m.start())
        if b:
            return repr(body[max(0, b.start() - 60):b.start() + 40])
        pos = m.end()
    b = _BAD.search(body, pos)
    return repr(body[max(0, b.start() - 60):b.start() + 40]) if b else None


def set_winsize(fd, rows, cols):
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))


def proc_cpu(pid):
    try:
        with open(f"/proc/{pid}/stat") as f:
            parts = f.read().rsplit(")", 1)[1].split()
        return (int(parts[11]) + int(parts[12])) / os.sysconf("SC_CLK_TCK"), None
    except (OSError, IndexError):
        return None, None


def rss_mb(pid):
    try:
        with open(f"/proc/{pid}/statm") as f:
            return int(f.read().split()[1]) * 4096 / 2**20
    except OSError:
        return 0.0


def children_cpu(pid):
    """Total CPU of pid's direct children (pipe commands), to excuse their spin."""
    total = 0.0
    try:
        for t in os.listdir(f"/proc/{pid}/task"):
            with open(f"/proc/{pid}/task/{t}/children") as f:
                for c in f.read().split():
                    cpu, _ = proc_cpu(int(c))
                    total += cpu or 0
    except OSError:
        pass
    return total


def make_case(rng, run_seed):
    ext = rng.choice(EXTS)
    files = {"f" + ext: gen_content(rng, ext)}
    if rng.random() < 0.3:
        e2 = rng.choice(EXTS)
        files["g" + e2] = gen_content(rng, e2)
    rows = rng.choice([24, 24, 40, 10, 5, 3, 2, 1, 80, 200])
    cols = rng.choice([80, 80, 120, 40, 20, 10, 5, 2, 1, 300])
    argv_kind = rng.choices(["file", "line", "multi", "none", "dir", "stdin", "missing"], [55, 10, 8, 5, 5, 7, 5])[0]
    first = next(iter(files))
    args = {"file": [first], "line": [first + ":" + rng.choice(["0", "1", "5", "999999", "-1", "", "abc", "99999999999999999999999"])],
            "multi": list(files), "none": [], "dir": ["."], "stdin": [], "missing": ["nope/" + first]}[argv_kind]
    steps = []
    for _ in range(rng.randrange(5, 120)):
        if rng.random() < 0.04:
            steps.append({"resize": [rng.choice([1, 2, 3, 5, 24, 50, 200]), rng.choice([1, 2, 5, 20, 80, 300])]})
        elif rng.random() < 0.03:
            steps.append({"fs": rng.choice(["truncate", "delete", "append", "rewrite", "todir", "chmod000", "chmod644", "huge"]),
                          "target": rng.choice(list(files) + ["sub", "saved.txt"])})
        else:
            delay = rng.choices([0, 0.002, 0.07], [60, 30, 10])[0]
            steps.append({"keys": macro(rng, rows, cols), "delay": delay})
    return {"seed": run_seed, "files": {k: v.decode("latin-1") for k, v in files.items()}, "rows": rows, "cols": cols,
            "args": args, "stdin": argv_kind == "stdin", "steps": steps, "weird_names": rng.random() < 0.3}


def fs_mutate(workdir, op, target):
    p = os.path.join(workdir, target)
    try:
        if op == "truncate":
            open(p, "wb").close()
        elif op == "delete":
            if os.path.isdir(p):
                shutil.rmtree(p)
            elif os.path.exists(p):
                os.unlink(p)
        elif op == "append":
            with open(p, "ab") as f:
                f.write(b"appended \xe4\xb8\xad\n")
        elif op == "rewrite":
            with open(p, "wb") as f:
                f.write(b"rewritten\r\nline2\n")
        elif op == "todir":
            if os.path.isfile(p):
                os.unlink(p)
            os.makedirs(p, exist_ok=True)
        elif op == "chmod000":
            os.chmod(p, 0)
        elif op == "chmod644":
            os.chmod(p, 0o644)
        elif op == "huge":
            with open(p, "wb") as f:
                f.write(b"x" * 3000000 + b"\n" + b"y\n" * 200000)
    except OSError:
        pass


def run_case(cx, case, workdir, hang_cpu=1.0, keep=False, quiet_after=0.3):
    """Execute one case. Returns (verdict, details dict)."""
    workdir = os.path.abspath(workdir)
    if os.path.exists(workdir):
        for root_, dirs_, files_ in os.walk(workdir):
            for n_ in dirs_ + files_:
                try:
                    os.chmod(os.path.join(root_, n_), 0o755)
                except OSError:
                    pass
        shutil.rmtree(workdir, ignore_errors=True)
    os.makedirs(os.path.join(workdir, "sub"))
    with open(os.path.join(workdir, "sub", "inner.md"), "w") as f:
        f.write("# inner\n")
    if case.get("weird_names"):
        for nm in ["a:1:x.txt", "nl\nname.txt", "-dash.txt", "sp ace.txt", "\u4e2d.txt", "tab\tname"]:
            try:
                with open(os.path.join(workdir, "sub", nm), "w") as f:
                    f.write("the key a\nx:2: the\n")
            except OSError:
                pass
        try:
            os.symlink("loop", os.path.join(workdir, "sub", "loop"))
        except OSError:
            pass
    for name, data in case["files"].items():
        with open(os.path.join(workdir, name), "wb") as f:
            f.write(data.encode("latin-1"))
    crashlog = os.path.join(workdir, ".crash.log")
    guardlog = os.path.join(workdir, ".guard.log")
    ubsanlog = os.path.join(workdir, ".ubsan")
    env = {
        "PATH": "/usr/bin:/bin", "HOME": workdir, "TERM": "xterm-256color", "LANG": "C.UTF-8",
        "FUZZ_ROOT": workdir, "FUZZ_CRASHLOG": crashlog, "FUZZ_GUARDLOG": guardlog,
        "LD_PRELOAD": (ASAN_LIB + " " if ASAN else "") + GUARD,
        "ASAN_OPTIONS": f"abort_on_error=1:detect_leaks=0:log_path={workdir}/.asan:allocator_may_return_null=1:detect_stack_use_after_return=1:strict_string_checks=1:max_malloc_fill_size=256:malloc_fill_byte=165",
        "UBSAN_OPTIONS": f"halt_on_error=1:abort_on_error=1:print_stacktrace=1:log_path={ubsanlog}",
        "DISPLAY": "", "GLIBC_TUNABLES": "glibc.malloc.perturb=165",
    }
    cx = os.path.abspath(cx)  # the child changes directory before exec
    stdin_r = None
    if case.get("stdin"):
        stdin_r, stdin_w = os.pipe()
    pid, fd = os.forkpty()
    if pid == 0:
        try:
            os.chdir(workdir)
            if not ASAN:
                resource.setrlimit(resource.RLIMIT_AS, (3 << 30, 3 << 30))
            resource.setrlimit(resource.RLIMIT_FSIZE, (512 << 20, 512 << 20))
            resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
            if stdin_r is not None:
                os.dup2(stdin_r, 0)
            os.execve(cx, [cx] + case["args"], env)
        finally:
            os._exit(127)
    set_winsize(fd, case["rows"], case["cols"])
    if stdin_r is not None:
        os.close(stdin_r)
        os.write(stdin_w, case["files"][next(iter(case["files"]))].encode("latin-1")[:1 << 20])
        os.close(stdin_w)
    out_tail = bytearray()
    out_all = bytearray()
    status = None
    peak_rss = 0.0

    def drain(timeout):
        nonlocal out_tail, out_all
        end = time.time() + timeout
        while True:
            left = end - time.time()
            r, _, _ = select.select([fd], [], [], max(0, left))
            if not r:
                return
            try:
                data = os.read(fd, 65536)
            except OSError:
                return
            if not data:
                return
            out_tail += data
            if len(out_all) < (8 << 20):
                out_all += data
            if len(out_tail) > 200000:
                del out_tail[:-100000]

    def alive():
        nonlocal status
        if status is not None:
            return False
        p, st = os.waitpid(pid, os.WNOHANG)
        if p == pid:
            status = st
            return False
        return True

    def send(s):
        b = s.encode("utf-8", "surrogateescape")
        for i in range(0, len(b), 1024):
            if not alive():
                return
            # wait until writable, but never block forever (editor may be hung)
            _, w, _ = select.select([], [fd], [], 2.0)
            if not w:
                return
            try:
                os.write(fd, b[i:i + 1024])
            except OSError:
                return
            drain(0)

    drain(0.15)
    hung = False
    step_idx = -1
    for step_idx, st in enumerate(case["steps"]):
        if not alive():
            break
        if "resize" in st:
            set_winsize(fd, *st["resize"])
            drain(0.01)
            continue
        if "fs" in st:
            fs_mutate(workdir, st["fs"], st["target"])
            drain(0.01)
            continue
        send(st["keys"])
        drain(st["delay"])
        peak_rss = max(peak_rss, rss_mb(pid))
        if peak_rss > RSS_LIMIT_MB:
            break
    # quiesce, then check for spin
    drain(quiet_after)
    cpu_spin = None
    if alive():
        c0, _ = proc_cpu(pid)
        k0 = children_cpu(pid)
        t0 = time.time()
        drain(hang_cpu)
        c1, _ = proc_cpu(pid)
        k1 = children_cpu(pid)
        if c0 is not None and c1 is not None:
            cpu_spin = (c1 - c0) / (time.time() - t0)
            if cpu_spin > 0.7 and alive():
                # Confirm: still spinning after another window
                drain(3.0)
                c2, _ = proc_cpu(pid)
                if c2 is not None and (c2 - c1) / 3.0 > 0.7:
                    hung = True
                    os.kill(pid, signal.SIGABRT)  # guard prints backtrace
                    for _ in range(50):
                        drain(0.1)
                        if not alive():
                            break
        peak_rss = max(peak_rss, rss_mb(pid))
    # SIGTERM is cx's exit command: with modified buffers it asks about
    # them, and a second SIGTERM while it asks writes them to copies and
    # exits. Only an editor that survives both is unkillable.
    term_sent = 0
    for _ in range(2):
        if not alive():
            break
        os.kill(pid, signal.SIGTERM)
        term_sent += 1
        for _ in range(60):
            drain(0.05)
            if not alive():
                break
    if alive():
        os.kill(pid, signal.SIGKILL)
        os.waitpid(pid, 0)
        status = status if status is not None else -9
    os.close(fd)

    det = {"steps_executed": step_idx + 1, "peak_rss_mb": round(peak_rss, 1), "cpu_spin": cpu_spin}
    verdict = "ok"
    if status is not None and status != -9:
        if os.WIFSIGNALED(status):
            sig = os.WTERMSIG(status)
            det["signal"] = sig
            if sig != signal.SIGTERM:
                verdict = "hang" if hung else "crash"
        elif os.WIFEXITED(status):
            det["exit"] = os.WEXITSTATUS(status)
            # After the second SIGTERM, 1 means an emergency copy could not
            # be written, which the tree changing behind cx's back allows.
            if det["exit"] not in ((0, 1) if term_sent == 2 else (0,)):
                verdict = "exit_nonzero"
    elif status == -9:
        verdict = "unkillable"
    for name, path in [("crashlog", crashlog), ("guardlog", guardlog)]:
        if os.path.exists(path):
            det[name] = open(path, errors="replace").read()[-8000:]
    for fn in os.listdir(workdir):
        if fn.startswith(".ubsan"):
            det["ubsan"] = open(os.path.join(workdir, fn), errors="replace").read()[-8000:]
            verdict = "ubsan"
        if fn.startswith(".asan"):
            det["ubsan"] = open(os.path.join(workdir, fn), errors="replace").read()[:8000]
            verdict = "asan"
    errf = os.path.join(workdir, ".cx-errors")
    if os.path.exists(errf):
        det["cx_errors"] = open(errf, errors="replace").read()[-4000:]
        # An emergency copy that cannot be written next to its file (one
        # in /proc, a directory the fuzzer removed) is logged, as it should
        # be; it is not a finding.
        expected = re.compile(r"(cannot create|write error for) .*\.unsaved-\d{8}T\d{6}Z$")
        unexpected = [l for l in open(errf, errors="replace").read().splitlines()
                      if l.strip() and not expected.search(l)]
        if unexpected and verdict == "ok":
            verdict = "cx_error"
    if peak_rss > (2500 if ASAN else 1500) and verdict == "ok":
        verdict = "memory"
    if "guardlog" in det:
        interesting = [l for l in det["guardlog"].splitlines() if "sh-replaced" not in l and "exec-refused" not in l]
        if interesting and verdict == "ok":
            verdict = "guard"
    if verdict == "ok":
        leak = term_leak(out_all)
        if leak:
            verdict = "termleak"
            det["leak"] = leak
    det["screen_tail"] = bytes(out_tail[-3000:]).decode("utf-8", "replace")
    if not keep:
        shutil.rmtree(workdir, ignore_errors=True)
    return verdict, det


def signature(verdict, det):
    """Rough dedup key: verdict + top in-project frames / first sanitizer line."""
    text = det.get("ubsan") or det.get("crashlog") or det.get("cx_errors") or ""
    key = []
    for line in text.splitlines():
        if "runtime error" in line or "Error:" in line or "error:" in line:
            key.append(line.split("runtime error")[-1][:120])
            break
    for line in text.splitlines():
        if "/cx(" in line or "/cx " in line or "(+0x" in line:
            key.append(line.split("(")[-1][:60])
            if len(key) > 3:
                break
    if verdict == "termleak":
        key = [det.get("leak", "")[-45:]]
    if verdict == "cx_error":
        key = [det.get("cx_errors", "").split("] ")[-1].strip()[:120]]
    return verdict + "|" + "|".join(key)


def main():
    if sys.argv[1] == "--replay":
        case = json.load(open(sys.argv[2]))
        cx, out = sys.argv[3], sys.argv[4]
        v, d = run_case(cx, case, os.path.join(out, "replay"), keep=True)
        print(v)
        print(json.dumps({k: v for k, v in d.items() if k != "screen_tail"}, indent=1)[:6000])
        return
    cx, out, secs = sys.argv[1], sys.argv[2], float(sys.argv[3])
    seed = int(sys.argv[4]) if len(sys.argv) > 4 else int(time.time())
    os.makedirs(os.path.join(out, "findings"), exist_ok=True)
    master = random.Random(seed)
    seen = {}
    n = 0
    t_end = time.time() + secs
    stats = {}
    while time.time() < t_end:
        run_seed = master.randrange(1 << 30)
        case = make_case(random.Random(run_seed), run_seed)
        v, d = run_case(cx, case, os.path.join(out, "work"))
        n += 1
        stats[v] = stats.get(v, 0) + 1
        if v != "ok":
            sig = signature(v, d)
            seen.setdefault(sig, 0)
            seen[sig] += 1
            if seen[sig] <= 3:
                base = os.path.join(out, "findings", f"{v}-{run_seed}")
                json.dump(case, open(base + ".json", "w"))
                json.dump(d, open(base + ".det.json", "w"), indent=1)
                print(f"[{n}] {v} seed={run_seed} sig={sig[:150]}", flush=True)
        if n % 25 == 0:
            print(f"[{n}] stats={stats} unique={len(seen)}", flush=True)
    print(f"done runs={n} stats={stats}")
    json.dump(seen, open(os.path.join(out, "signatures.json"), "w"), indent=1)


if __name__ == "__main__":
    main()
