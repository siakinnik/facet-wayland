#!/usr/bin/env python3
"""Fails if non-ASCII letters (e.g. Cyrillic) appear in source code outside
i18n/ directories. Keeps comments English and every user-visible string
translatable.

    scripts/check_i18n.py [project-dir ...]   (default: this repository)
"""
import pathlib
import re
import sys

SOURCE_EXT = {".c", ".cc", ".cpp", ".h", ".hpp", ".py", ".sh", ".cmake", ".txt", ".service", ".script"}
SKIP_DIRS = {"i18n", "build", ".git", ".dev-data", "shots"}
# Letters of non-Latin scripts. Typographic punctuation (…, —, «») is fine.
_RANGES = [(0x0370, 0x03FF), (0x0400, 0x052F), (0x0590, 0x06FF), (0x3040, 0x30FF), (0x4E00, 0x9FFF)]
FOREIGN = re.compile("[" + "".join(f"{chr(a)}-{chr(b)}" for a, b in _RANGES) + "]")


def scan(root: pathlib.Path) -> int:
    bad = 0
    for path in root.rglob("*"):
        if not path.is_file() or path.suffix not in SOURCE_EXT and path.name != "CMakeLists.txt":
            continue
        if SKIP_DIRS.intersection(path.relative_to(root).parts[:-1]):
            continue
        for no, line in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
            if FOREIGN.search(line):
                print(f"{path}:{no}: {line.strip()}")
                bad += 1
    return bad


def main() -> int:
    roots = [pathlib.Path(a) for a in sys.argv[1:]] or [pathlib.Path(__file__).resolve().parent.parent]
    bad = sum(scan(r) for r in roots)
    if bad:
        print(f"\n{bad} line(s) with non-English text outside i18n/. Move strings to i18n tables.")
        return 1
    print("ok: no non-English text outside i18n/")
    return 0


if __name__ == "__main__":
    sys.exit(main())
