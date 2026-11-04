#!/usr/bin/env python3
"""Gate for a change made by the crash-fixing agent (docs/CRASH_FIXES.md). Exit 0: fit for a DRAFT pull request.

    python tools/crash/check_patch.py --repo . --briefing context/crash_context.md

The agent only reads and edits files; this ordinary script decides whether what it left behind may go public:
  * only source, tools, tests, server and docs paths; never workflows, packaging, the build files, dependencies,
    the crash inbox address or binaries;
  * small (files and lines), so a human can read all of it;
  * nothing from the crash report in the added lines: no long line of the log or the player's note, no address,
    user path, e-mail, Steam id or secret-looking text. The report is private, the pull request is public.
"""
import argparse, re, subprocess, sys
from pathlib import Path

ALLOW = ('src/', 'tools/', 'server/', 'docs/', 'data/mod_compat.json')
DENY = ('.github/', 'packaging/', 'extern/', 'patches/', 'tools/crash/', 'data/crash_inbox.json', 'CMakeLists.txt', 'CMakePresets.json',
        'server/crash-inbox/wrangler.toml')
BINARY = ('.exe', '.dll', '.pdb', '.zip', '.7z', '.png', '.jpg', '.bin', '.arc', '.szp', '.iso', '.rvz', '.aw', '.mdl')
MAX_FILES, MAX_LINES = 8, 400

PATTERNS = [
    (r'\b(?:\d{1,3}\.){3}\d{1,3}\b', 'an IP address'),
    (r'[A-Za-z]:\\Users\\', 'a Windows user path'),
    (r'/home/\w|/Users/\w', 'a home folder path'),
    (r'[\w.+-]+@[\w-]+\.[\w.-]+', 'an e-mail address'),
    (r'\b7656119\d{10}\b', 'a Steam id'),
    (r'(?i)(password|passwd|secret|token|api[_-]?key)\s*[=:]\s*\S{4,}', 'a secret-looking value'),
    (r'\bgh[pousr]_[A-Za-z0-9]{20,}|sk-[A-Za-z0-9_-]{20,}', 'a token'),
]

def sh(repo, *args):
    return subprocess.run(['git', '-C', repo, *args], capture_output=True, text=True).stdout

def private_lines(briefing):
    if not (briefing and Path(briefing).exists()):
        return set()
    text = Path(briefing).read_text(encoding='utf-8', errors='replace').split('UNTRUSTED DATA', 1)[-1]
    return {l.strip() for l in text.splitlines() if len(l.strip()) >= 24}

def scan_text(path, briefing):
    """A pull request title or body is public: no report content in it."""
    text = Path(path).read_text(encoding='utf-8', errors='replace')
    problems = []
    for line in text.splitlines():
        for pat, what in PATTERNS:
            if re.search(pat, line):
                problems.append(f'the text contains {what}: {line.strip()[:80]!r}')
                break
    for p in private_lines(briefing):
        if p in text:
            problems.append(f'the text repeats a line of the crash report: {p[:80]!r}')
    if problems:
        print('REFUSED:')
        for p in dict.fromkeys(problems):
            print(' -', p)
        return 1
    print('ok: the text is clean')
    return 0

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--repo')
    ap.add_argument('--briefing')
    ap.add_argument('--scan-text', help='check a pull request text instead of a change: exit 1 when it leaks report content')
    a = ap.parse_args()
    problems = []

    if a.scan_text:
        return scan_text(a.scan_text, a.briefing)
    if not a.repo:
        ap.error('--repo is required')

    changed = [l[3:].strip().strip('"') for l in sh(a.repo, 'status', '--porcelain', '--untracked-files=all').splitlines() if l.strip()]
    if not changed:
        print('no change: nothing to check')
        return 2
    for f in changed:
        if f.startswith(DENY) or not f.startswith(ALLOW):
            problems.append(f'{f}: this path is not allowed for an automatic change')
        if f.lower().endswith(BINARY):
            problems.append(f'{f}: binary or game data')
    if len(changed) > MAX_FILES:
        problems.append(f'{len(changed)} files changed (limit {MAX_FILES})')

    # the whole change, added lines only (untracked files count in full)
    added = []
    for line in sh(a.repo, 'diff', '--unified=0', 'HEAD').splitlines():
        if line.startswith('+') and not line.startswith('+++'):
            added.append(line[1:])
    for f in changed:
        p = Path(a.repo) / f
        if f not in sh(a.repo, 'ls-files').split('\n') and p.is_file():
            try:
                added += p.read_text(encoding='utf-8', errors='replace').splitlines()
            except OSError:
                pass
    if len(added) > MAX_LINES:
        problems.append(f'{len(added)} added lines (limit {MAX_LINES})')

    for text in added:
        for pat, what in PATTERNS:
            if re.search(pat, text):
                problems.append(f'an added line contains {what}: {text.strip()[:80]!r}')
                break

    # lines of the private report must not reappear in the public change
    private = private_lines(a.briefing)
    if private:
        for text in added:
            if any(p in text for p in private):  # anywhere in the line, so "// <report line>" is caught too
                problems.append(f'an added line is copied from the crash report: {text.strip()[:80]!r}')

    if problems:
        print('REFUSED:')
        for p in dict.fromkeys(problems):
            print(' -', p)
        return 1
    print(f'ok: {len(changed)} file(s), {len(added)} added line(s)')
    return 0

if __name__ == '__main__':
    sys.exit(main())
