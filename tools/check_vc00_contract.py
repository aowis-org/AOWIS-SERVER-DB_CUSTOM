#!/usr/bin/env python3
"""Read-only VC-00 implementation gap probe; no database or project mutations."""
from pathlib import Path
import argparse
import re

ROOT = Path(__file__).resolve().parents[1]


def check(text: str, pattern: str) -> bool:
    return re.search(pattern, text, re.MULTILINE | re.DOTALL) is not None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--gui-root', type=Path, help='optional AOWIS-SERVER-GUI checkout')
    args = parser.parse_args()
    checks = [
        ('Native branch creation writes an initial revision',
         ROOT / 'src/native_store.cpp', r'RevisionId Repository::createBranch\b[\s\S]*?id = writeRevisionLocked\('),
        ('Native HEAD API returns mandatory revision',
         ROOT / 'include/aowis/dvmdb/native_store.h', r'RevisionId head\('),
        ('Service HEAD API returns mandatory revision',
         ROOT / 'include/aowis/dvmdb/project_service.h', r'RevisionId head\('),
        ('Project creation invokes root branch creation',
         ROOT / 'src/project_service.cpp', r'repository\.createBranch\(\*project, "main"\)'),
        ('REST graph serializes HEAD as a revision string',
         ROOT / 'src/dvmdb_http_routes.cpp', r'\{"revision", revisionIdString\(head\.revision\)\}'),
    ]
    if args.gui_root:
        gui = args.gui_root.resolve()
        checks += [
            ('GUI graph rejects empty HEAD', gui / 'src/projects/project_source_reader.cpp',
             r'!DvmdbIdentity::isUuidV7\(branch\.head\)'),
            ('GUI HEAD stored as string', gui / 'src/projects/project_source_reader.h',
             r'QString head;'),
        ]
    for label, path, pattern in checks:
        if not path.is_file():
            print(f'MISSING: {label} ({path})')
            return 2
        present = check(path.read_text(encoding='utf-8'), pattern)
        print(f'{"GAP PRESENT" if present else "GAP CLOSED / REVIEW"}: {label}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
