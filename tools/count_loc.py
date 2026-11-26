#!/usr/bin/env python3
"""Count lines of code in a directory tree.

Walks a folder recursively, classifies files by source language via file
extension, and reports total, code, pure-code (no comment), comment, and
blank line counts per language with a grand total.

Usage:
    tools/count_loc.py [root] [--exclude DIR ...] [--by-file]

Notes:
    - Only major programming languages are counted. Markup, JSON/YAML,
      build files, and text docs are ignored.
    - The .m extension is treated as Objective-C (not MATLAB).
    - Python triple-quoted strings are counted as comments only when they
      begin a logical line (matching docstring convention).
    - Escape handling and raw-string edge cases are best-effort, not a full
      lexer. Numbers should match cloc within a small margin.
"""

import argparse
import os
import sys
from collections import defaultdict

C_STYLE_LINE = ("//",)
C_STYLE_BLOCK = (("/*", "*/"),)

# (language name, extensions, line-comment starts, block-comment (start,end) pairs, is_python)
LANGUAGES = [
    ("C/C++",       ("c", "h", "cpp", "cc", "cxx", "c++", "hpp", "hh", "hxx", "h++",
                     "ipp", "tpp", "inl", "cu", "cuh"),
                    C_STYLE_LINE, C_STYLE_BLOCK, False),
    ("C#",          ("cs",),                         C_STYLE_LINE, C_STYLE_BLOCK, False),
    ("Objective-C", ("m", "mm"),                     C_STYLE_LINE, C_STYLE_BLOCK, False),
    ("Java",        ("java",),                       C_STYLE_LINE, C_STYLE_BLOCK, False),
    ("Kotlin",      ("kt", "kts"),                   C_STYLE_LINE, C_STYLE_BLOCK, False),
    ("Scala",       ("scala", "sc"),                 C_STYLE_LINE, C_STYLE_BLOCK, False),
    ("Swift",       ("swift",),                      C_STYLE_LINE, C_STYLE_BLOCK, False),
    ("Go",          ("go",),                         C_STYLE_LINE, C_STYLE_BLOCK, False),
    ("Rust",        ("rs",),                         C_STYLE_LINE, C_STYLE_BLOCK, False),
    ("JavaScript",  ("js", "mjs", "cjs", "jsx"),     C_STYLE_LINE, C_STYLE_BLOCK, False),
    ("TypeScript",  ("ts", "tsx"),                   C_STYLE_LINE, C_STYLE_BLOCK, False),
    ("PHP",         ("php", "phtml"),                ("//", "#"), C_STYLE_BLOCK, False),
    ("Dart",        ("dart",),                       C_STYLE_LINE, C_STYLE_BLOCK, False),
    ("Shader",      ("glsl", "vert", "frag", "comp", "geom", "tesc", "tese",
                     "hlsl", "fx"),
                    C_STYLE_LINE, C_STYLE_BLOCK, False),
    ("CSS/SCSS",    ("css", "scss", "sass", "less"), ("//",),     C_STYLE_BLOCK, False),
    ("SQL",         ("sql",),                        ("--",),     C_STYLE_BLOCK, False),
    ("Python",      ("py", "pyw", "pyi"),            ("#",),      (('"""', '"""'), ("'''", "'''")), True),
    ("Ruby",        ("rb",),                         ("#",),      (("=begin", "=end"),), False),
    ("Perl",        ("pl", "pm"),                    ("#",),      (("=pod", "=cut"),), False),
    ("Shell",       ("sh", "bash", "zsh", "ksh", "fish"),
                    ("#",),      (),                 False),
    ("PowerShell",  ("ps1", "psm1"),                 ("#",),      (("<#", "#>"),), False),
    ("R",           ("r",),                          ("#",),      (),                 False),
    ("Lua",         ("lua",),                        ("--",),     (("--[[", "]]"),), False),
    ("Haskell",     ("hs", "lhs"),                   ("--",),     (("{-", "-}"),), False),
    ("OCaml",       ("ml", "mli"),                   (),          (("(*", "*)"),), False),
    ("Erlang",      ("erl", "hrl"),                  ("%",),      (),                 False),
]


def build_ext_map():
    m = {}
    for lang in LANGUAGES:
        _, exts, *_ = lang
        for ext in exts:
            m.setdefault(ext, lang)
    return m


EXT_MAP = build_ext_map()

DEFAULT_EXCLUDES = {
    ".git", ".hg", ".svn",
    "__pycache__", "node_modules",
    ".venv", "venv", "env",
    ".idea", ".vscode",
    "dist",
}

MAX_FILE_BYTES = 20 * 1024 * 1024  # 20 MB safety cap


def classify_lines(text, line_comments, block_comments, is_python):
    """Return (total, code, pure_code, comment, blank) line counts.

    - code: lines containing at least one non-comment, non-whitespace token
    - pure_code: code lines that contain NO comment content
    - comment: lines that are entirely comment (no code)
    - blank: everything else
    """
    if not text:
        return 0, 0, 0, 0, 0

    total = 0
    code_lines = 0
    pure_code_lines = 0
    comment_lines = 0

    in_block = None     # end-marker string when inside a block comment / docstring
    in_string = None    # delimiter string when inside a regular string literal

    # Check longer markers first so '--[[' wins over '--', etc.
    block_starts = tuple(sorted(block_comments, key=lambda p: -len(p[0])))
    line_starts = tuple(sorted(line_comments, key=lambda s: -len(s)))

    n = len(text)
    i = 0
    line_start = 0
    has_code = False
    has_comment = False

    while i < n:
        c = text[i]

        # Line boundary
        if c == '\n':
            total += 1
            if has_code:
                code_lines += 1
                if not has_comment:
                    pure_code_lines += 1
            elif has_comment or in_block is not None:
                comment_lines += 1
            has_code = False
            has_comment = False
            i += 1
            line_start = i
            # Single-quote strings don't span lines in C-style languages
            if in_string is not None and len(in_string) == 1:
                in_string = None
            # A continuation of a block comment on the next line should be
            # counted as comment even if it contains no characters
            if in_block is not None:
                has_comment = True
            continue

        # Inside a block comment / docstring
        if in_block is not None:
            if text.startswith(in_block, i):
                has_comment = True
                i += len(in_block)
                in_block = None
            else:
                if not c.isspace():
                    has_comment = True
                i += 1
            continue

        # Inside a string literal
        if in_string is not None:
            if c == '\\' and i + 1 < n and text[i + 1] != '\n':
                i += 2
                has_code = True
                continue
            if text.startswith(in_string, i):
                i += len(in_string)
                in_string = None
                has_code = True
                continue
            has_code = True
            i += 1
            continue

        # Block comment start (before line comment so '--[[' beats '--')
        matched = False
        for start, end in block_starts:
            if text.startswith(start, i):
                if is_python and start in ('"""', "'''"):
                    prefix = text[line_start:i]
                    if prefix.strip() == "":
                        in_block = end
                        has_comment = True
                    else:
                        in_string = end
                        has_code = True
                else:
                    in_block = end
                    has_comment = True
                i += len(start)
                matched = True
                break
        if matched:
            continue

        # Line comment
        for lc in line_starts:
            if text.startswith(lc, i):
                has_comment = True
                nl = text.find('\n', i)
                i = nl if nl >= 0 else n
                matched = True
                break
        if matched:
            continue

        # Regular string start
        if c == '"' or c == "'":
            in_string = c
            has_code = True
            i += 1
            continue

        if not c.isspace():
            has_code = True
        i += 1

    # Trailing line without a terminating newline
    if has_code or has_comment or i > line_start:
        total += 1
        if has_code:
            code_lines += 1
            if not has_comment:
                pure_code_lines += 1
        elif has_comment or in_block is not None:
            comment_lines += 1

    blank = total - code_lines - comment_lines
    return total, code_lines, pure_code_lines, comment_lines, blank


def count_file(path, lang):
    _, _, line_c, block_c, is_py = lang
    try:
        with open(path, 'r', encoding='utf-8', errors='replace') as f:
            text = f.read()
    except OSError as e:
        print(f"warning: failed to read {path}: {e}", file=sys.stderr)
        return None
    return classify_lines(text, line_c, block_c, is_py)


def walk_source_files(root, excludes):
    for dirpath, dirnames, filenames in os.walk(root, followlinks=False):
        # Prune excluded and hidden directories in place
        dirnames[:] = [d for d in dirnames
                       if d not in excludes and not d.startswith('.')]
        for fn in filenames:
            ext = os.path.splitext(fn)[1].lower().lstrip('.')
            if not ext:
                continue
            lang = EXT_MAP.get(ext)
            if lang is None:
                continue
            path = os.path.join(dirpath, fn)
            try:
                if os.path.getsize(path) > MAX_FILE_BYTES:
                    print(f"warning: skipping large file {path}", file=sys.stderr)
                    continue
            except OSError:
                continue
            yield path, lang


def format_row(cells, widths):
    out = []
    for idx, (c, w) in enumerate(zip(cells, widths)):
        s = str(c)
        if idx == 0:
            out.append(s.ljust(w))
        else:
            out.append(s.rjust(w))
    return "  ".join(out)


def main():
    ap = argparse.ArgumentParser(
        description="Count lines of code across a directory tree.")
    ap.add_argument("root", nargs="?", default=".",
                    help="root directory to scan (default: current directory)")
    ap.add_argument("--exclude", action="append", default=[],
                    metavar="DIR",
                    help="directory name to skip (repeatable)")
    ap.add_argument("--by-file", action="store_true",
                    help="also print a per-file breakdown sorted by line count")
    args = ap.parse_args()

    if not os.path.isdir(args.root):
        print(f"error: not a directory: {args.root}", file=sys.stderr)
        return 1

    excludes = set(DEFAULT_EXCLUDES) | set(args.exclude)

    # stats: [files, total, code, pure, comment, blank]
    per_lang = defaultdict(lambda: [0, 0, 0, 0, 0, 0])
    per_file_rows = []

    for path, lang in walk_source_files(args.root, excludes):
        result = count_file(path, lang)
        if result is None:
            continue
        total, code, pure, comment, blank = result
        name = lang[0]
        s = per_lang[name]
        s[0] += 1
        s[1] += total
        s[2] += code
        s[3] += pure
        s[4] += comment
        s[5] += blank
        if args.by_file:
            per_file_rows.append((path, name, total, code, pure, comment, blank))

    if not per_lang:
        print("No source files found.")
        return 0

    if args.by_file:
        per_file_rows.sort(key=lambda r: -r[2])
        fwidths = (60, 12, 8, 8, 8, 7, 7)
        headers = ("File", "Lang", "Total", "Code", "Pure", "Cmt", "Blank")
        print(format_row(headers, fwidths))
        print("  ".join("-" * w for w in fwidths))
        for row in per_file_rows:
            path = row[0]
            disp = path if len(path) <= fwidths[0] else "..." + path[-(fwidths[0] - 3):]
            print(format_row((disp,) + row[1:], fwidths))
        print()

    headers = ("Language", "Files", "Total", "Code", "Pure Code", "Comment", "Blank")
    widths = (14, 7, 10, 10, 10, 10, 10)
    rows = sorted(per_lang.items(), key=lambda kv: -kv[1][1])
    print(format_row(headers, widths))
    print("  ".join("-" * w for w in widths))
    grand = [0, 0, 0, 0, 0, 0]
    for name, s in rows:
        print(format_row((name, s[0], s[1], s[2], s[3], s[4], s[5]), widths))
        for k in range(6):
            grand[k] += s[k]
    print("  ".join("-" * w for w in widths))
    print(format_row(("TOTAL", *grand), widths))
    print()
    print(f"Summary: {grand[0]} source files, {grand[1]} total lines, "
          f"{grand[3]} pure code lines (no comments).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
