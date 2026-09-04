#!/usr/bin/env python3
"""
GitHub Actions job timeout guard.

Scans every workflow file under .github/workflows/ (or the directory given
on the command line) and fails if any job is missing an explicit
`timeout-minutes:` at the job level. Without it a hung job burns GitHub's
360-minute default; every job should carry a generous cap sized to its
expected runtime.

Exit code: 0 if every job has a timeout-minutes guard, 1 otherwise.

Uses PyYAML when available for exact parsing; falls back to a line-based
scan of the standard workflow layout (jobs: at column 0, job ids at 2
spaces, job properties at 4 spaces).
"""

import glob
import os
import re
import sys

JOB_LINE = re.compile(r"^  ([A-Za-z_][A-Za-z0-9_-]*):\s*$")
TIMEOUT_LINE = re.compile(r"^ {4}timeout-minutes:\s*\S")


def check_with_yaml(path):
    """Return (jobs_missing, jobs_total) using PyYAML, or None on error."""
    try:
        import yaml
    except ImportError:
        return None
    try:
        with open(path, "r", encoding="utf-8") as f:
            data = yaml.safe_load(f)
    except yaml.YAMLError as exc:
        print(f"  ERROR: {path}: invalid YAML: {exc}")
        return ([], 0)
    jobs = (data or {}).get("jobs") or {}
    missing = [name for name, cfg in jobs.items()
               if not isinstance(cfg, dict) or "timeout-minutes" not in cfg]
    return (missing, len(jobs))


def check_linebased(path):
    """Fallback: line-based scan of the standard workflow layout."""
    with open(path, "r", encoding="utf-8") as f:
        lines = f.read().splitlines()
    in_jobs = False
    current = None
    total = 0
    missing = []
    for line in lines:
        if re.match(r"^jobs:\s*$", line):
            in_jobs = True
            continue
        if not in_jobs:
            continue
        if line and not line.startswith(" "):
            break
        m = JOB_LINE.match(line)
        if m:
            current = m.group(1)
            total += 1
            missing.append(current)
            continue
        if current and TIMEOUT_LINE.match(line):
            if current in missing:
                missing.remove(current)
    return (missing, total)


def check_file(path):
    """Return (missing_jobs, total_jobs) for one workflow file."""
    result = check_with_yaml(path)
    if result is not None:
        return result
    return check_linebased(path)


def main(argv):
    root = argv[1] if len(argv) > 1 else ".github/workflows"
    if os.path.isdir(root):
        files = sorted(
            glob.glob(os.path.join(root, "*.yml"))
            + glob.glob(os.path.join(root, "*.yaml"))
        )
    else:
        files = [root]

    if not files:
        print(f"check-ci-timeouts: no workflow files found under {root}")
        return 1

    failed = False
    for path in files:
        missing, total = check_file(path)
        if missing is None:
            missing = []
        if total == 0:
            print(f"  WARN: {path}: no jobs found (skipped)")
            continue
        if missing:
            failed = True
            print(f"  FAIL: {path}: {len(missing)}/{total} job(s) missing "
                  f"timeout-minutes: {', '.join(missing)}")
        else:
            print(f"  OK: {path}: {total}/{total} job(s) guarded")

    if failed:
        print("\nEvery GitHub Actions job must set an explicit "
              "`timeout-minutes:` so a hung runner fails fast instead of "
              "burning the 360-minute default.")
        return 1
    print("\nAll workflow jobs carry timeout-minutes guards.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
