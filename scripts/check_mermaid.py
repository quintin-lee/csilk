#!/usr/bin/env python3
"""
Mermaid diagram syntax validator for csilk documentation.

Extracts ```mermaid ... ``` code blocks from Markdown files and performs
basic structural validation:
  - Balanced block-delimiter pairs (subgraph/end, alt/end, loop/end, etc.)
  - No bare placeholder tokens like "..."
  - Proper bracket matching outside quoted labels
  - No obviously invalid syntax patterns

Exit code: 0 if all diagrams pass, 1 if any issues found.
"""

import os
import re
import sys


# Mermaid keywords that open a block closed by 'end'
BLOCK_OPENERS = {"subgraph", "alt", "loop", "opt", "par", "critical"}


def find_mermaid_blocks(filepath):
    """Yield (start_line, block_number, lines) for each mermaid block."""
    with open(filepath, "r", encoding="utf-8") as f:
        text = f.read()
    lines = text.split("\n")

    in_block = False
    block_start = 0
    block_lines = []
    block_count = 0

    for i, line in enumerate(lines, 1):
        stripped = line.strip()
        if stripped == "```mermaid":
            in_block = True
            block_start = i
            block_lines = []
        elif stripped == "```" and in_block:
            block_count += 1
            yield block_start, block_count, block_lines
            in_block = False
        elif in_block:
            block_lines.append((i, line.rstrip()))

    if in_block:
        print(f"WARNING: {filepath}:{block_start}: Mermaid block not closed",
              file=sys.stderr)


def line_is_comment(stripped):
    return stripped.startswith("%%")


def normalize_line(line):
    """Strip whitespace and comments from a line."""
    stripped = line.strip()
    if line_is_comment(stripped):
        return ""
    return stripped


def validate_blocks(filepath, start_line, block_num, lines):
    """Validate a single mermaid block. Returns list of error strings."""
    errors = []

    # --- Empty block ---
    content = "".join(line for _, line in lines)
    if not content.strip():
        return errors

    # --- Bare placeholder tokens ---
    for lineno, line in lines:
        stripped = line.strip()
        if stripped in ("...", "***", "---") and not line_is_comment(stripped):
            errors.append(
                f"{filepath}:{lineno}:{block_num}: Invalid placeholder '{stripped}'"
            )

    # --- Balanced block delimiters (subgraph/end, alt/end, loop/end, etc.) ---
    depth = 0
    for lineno, line in lines:
        stripped = normalize_line(line)
        if not stripped:
            continue
        first_word = stripped.split()[0] if stripped.split() else ""
        if first_word in BLOCK_OPENERS:
            depth += 1
        elif first_word == "end":
            depth -= 1
            if depth < 0:
                errors.append(
                    f"{filepath}:{lineno}:{block_num}: Dangling 'end' "
                    f"without matching block opener"
                )
    if depth > 0:
        errors.append(
            f"{filepath}:{start_line}:{block_num}: {depth} unclosed block(s)"
        )

    # --- Bracket matching inside quoted labels only ---
    # Mermaid uses [], (), and {} in node shape syntax (stadium, subroutine,
    # database shapes), so we only check for unmatched brackets that appear
    # inside double-quoted label strings.
    for lineno, line in lines:
        stripped = line.strip()
        if line_is_comment(stripped) or not stripped:
            continue
        # Extract all quoted strings and check bracket balance within each
        for match in re.finditer(r'"([^"\\]*(?:\\.[^"\\]*)*)"', stripped):
            label = match.group(1)
            for kind, open_c, close_c in [("()", "(", ")"),
                                           ("[]", "[", "]"),
                                           ("{}", "{", "}")]:
                depth = 0
                for ch in label:
                    if ch == open_c:
                        depth += 1
                    elif ch == close_c:
                        depth -= 1
                if depth != 0:
                    errors.append(
                        f"{filepath}:{lineno}:{block_num}: Unmatched '{kind}' "
                        f"in label (depth {depth})"
                    )

    return errors


def find_markdown_files(root_dir):
    """Find all .md files under root_dir."""
    files = []
    for entry in sorted(os.listdir(root_dir)):
        if entry.endswith(".md"):
            files.append(os.path.join(root_dir, entry))
    docs_dir = os.path.join(root_dir, "docs")
    if os.path.isdir(docs_dir):
        for dirpath, _, filenames in os.walk(docs_dir):
            for f in sorted(filenames):
                if f.endswith(".md"):
                    files.append(os.path.join(dirpath, f))
    return files


def main():
    root_dir = sys.argv[1] if len(sys.argv) > 1 else \
        os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    files = find_markdown_files(root_dir)

    all_errors = []
    blocks_found = 0

    for filepath in files:
        for start_line, block_num, lines in find_mermaid_blocks(filepath):
            blocks_found += 1
            errors = validate_blocks(filepath, start_line, block_num, lines)
            all_errors.extend(errors)

    if all_errors:
        print(f"ERROR: {len(all_errors)} Mermaid validation issue(s) in "
              f"{blocks_found} diagram(s):", file=sys.stderr)
        for err in sorted(all_errors):
            print(f"  {err}", file=sys.stderr)
        return 1

    msg = f"OK: all {blocks_found} Mermaid diagram(s) pass validation."
    print(msg)
    return 0


if __name__ == "__main__":
    sys.exit(main())
