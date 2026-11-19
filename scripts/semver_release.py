#!/usr/bin/env python3
"""Compute the next semver from conventional commits and optionally apply release files.

Usage:
  scripts/semver_release.py                 # print JSON summary (no writes)
  scripts/semver_release.py --apply         # patch CMakeLists / man / Release-Notes
  scripts/semver_release.py --github-output # also write GITHUB_OUTPUT keys

Exit 0 always when analysis succeeds; should_release is in the payload.
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import re
import subprocess
import sys
from collections import defaultdict
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
CMAKE = REPO_ROOT / "CMakeLists.txt"
MAN = REPO_ROOT / "man" / "mktorrent.1"
NOTES = REPO_ROOT / "docs" / "Release-Notes.md"

RELEASE_SUBJECT = re.compile(r"^chore\(release\):\s*v?\d+\.\d+\.\d+", re.I)
CONV = re.compile(
    r"^(?P<type>feat|fix|docs|style|refactor|perf|test|build|ci|chore|i18n|revert)"
    r"(?:\([^)]*\))?(?P<breaking>!)?:\s*(?P<summary>.+)$",
    re.I,
)
BREAKING_FOOTER = re.compile(r"^BREAKING CHANGE:", re.I | re.M)
VERSION_LINE = re.compile(
    r"(project\(mktorrent\s*\n\s*VERSION\s+)(\d+\.\d+\.\d+)",
    re.M,
)
MAN_VERSION = re.compile(r'(MKTorrent )(\d+\.\d+\.\d+)')
TAG_RE = re.compile(r"^v?(\d+)\.(\d+)\.(\d+)$")

SECTION_FOR_TYPE = {
    "feat": "Added",
    "fix": "Fixed",
    "perf": "Changed",
    "refactor": "Changed",
    "docs": "Changed",
    "style": "Changed",
    "test": "Changed",
    "build": "Changed",
    "ci": "Changed",
    "chore": "Changed",
    "i18n": "Changed",
    "revert": "Fixed",
}


def run_git(*args: str) -> str:
    return subprocess.check_output(
        ["git", *args],
        cwd=REPO_ROOT,
        text=True,
        stderr=subprocess.DEVNULL,
    ).strip()


def latest_tag() -> str | None:
    try:
        tags = run_git("tag", "-l", "v*").splitlines()
    except subprocess.CalledProcessError:
        return None
    best = None
    best_key = (-1, -1, -1)
    for tag in tags:
        m = TAG_RE.match(tag.strip())
        if not m:
            continue
        key = tuple(int(x) for x in m.groups())
        if key > best_key:
            best_key = key  # type: ignore[assignment]
            best = tag.strip()
    return best


def parse_version(tag_or_ver: str) -> tuple[int, int, int]:
    m = TAG_RE.match(tag_or_ver.lstrip("v"))
    if not m:
        raise ValueError(f"invalid version: {tag_or_ver}")
    return int(m.group(1)), int(m.group(2)), int(m.group(3))


def read_cmake_version() -> str:
    text = CMAKE.read_text(encoding="utf-8")
    m = VERSION_LINE.search(text)
    if not m:
        raise SystemExit("VERSION not found in CMakeLists.txt")
    return m.group(2)


def commit_messages_since(tag: str | None) -> list[str]:
    rng = f"{tag}..HEAD" if tag else "HEAD"
    try:
        raw = run_git("log", rng, "--format=%B%x1e")
    except subprocess.CalledProcessError:
        return []
    if not raw:
        return []
    return [c.strip() for c in raw.split("\x1e") if c.strip()]


def classify(messages: list[str]) -> tuple[str, list[tuple[str, str]]]:
    """Return bump kind (major|minor|patch|none) and list of (section, bullet)."""
    entries: list[tuple[str, str]] = []
    bump = "none"
    useful = 0

    for msg in messages:
        subject = msg.splitlines()[0].strip() if msg else ""
        if RELEASE_SUBJECT.match(subject):
            continue
        useful += 1
        body = "\n".join(msg.splitlines()[1:])
        breaking = bool(BREAKING_FOOTER.search(body))
        m = CONV.match(subject)
        if m:
            ctype = m.group("type").lower()
            if m.group("breaking"):
                breaking = True
            summary = m.group("summary").strip()
            section = SECTION_FOR_TYPE.get(ctype, "Changed")
            if breaking:
                section = "Changed"
                summary = f"**BREAKING** {summary}"
            entries.append((section, summary))
            if breaking:
                bump = "major"
            elif ctype == "feat" and bump != "major":
                bump = "minor"
            elif bump == "none":
                bump = "patch"
        else:
            entries.append(("Changed", subject))
            if bump == "none":
                bump = "patch"

    if useful == 0:
        return "none", []
    return bump, entries


def bump_version(base: tuple[int, int, int], kind: str) -> tuple[int, int, int]:
    major, minor, patch = base
    if kind == "major":
        return major + 1, 0, 0
    if kind == "minor":
        return major, minor + 1, 0
    if kind == "patch":
        return major, minor, patch + 1
    return base


def format_ver(v: tuple[int, int, int]) -> str:
    return f"{v[0]}.{v[1]}.{v[2]}"


def render_changelog_section(version: str, entries: list[tuple[str, str]], day: str) -> str:
    grouped: dict[str, list[str]] = defaultdict(list)
    for section, bullet in entries:
        if bullet not in grouped[section]:
            grouped[section].append(bullet)
    order = ["Added", "Fixed", "Changed", "Deprecated", "Removed", "Security"]
    lines = [f"## [v{version}] - {day}", ""]
    for section in order:
        bullets = grouped.get(section)
        if not bullets:
            continue
        lines.append(f"### {section}")
        for b in bullets:
            lines.append(f"- {b}")
        lines.append("")
    return "\n".join(lines).rstrip() + "\n"


def apply_files(version: str, section_md: str) -> None:
    cmake = CMAKE.read_text(encoding="utf-8")
    cmake_new, n = VERSION_LINE.subn(rf"\g<1>{version}", cmake, count=1)
    if n != 1:
        raise SystemExit("failed to patch CMakeLists.txt VERSION")
    CMAKE.write_text(cmake_new, encoding="utf-8")

    man = MAN.read_text(encoding="utf-8")
    man_new, n = MAN_VERSION.subn(rf"\g<1>{version}", man, count=1)
    if n != 1:
        raise SystemExit("failed to patch man/mktorrent.1 version")
    MAN.write_text(man_new, encoding="utf-8")

    notes = NOTES.read_text(encoding="utf-8")
    # Drop a premature same-version section if present, then insert after Unreleased.
    notes = re.sub(
        rf"\n## \[v{re.escape(version)}\] - [0-9-]+\n.*?(?=\n## \[|\Z)",
        "\n",
        notes,
        count=1,
        flags=re.S,
    )
    marker = "## [Unreleased]"
    idx = notes.find(marker)
    if idx == -1:
        raise SystemExit("## [Unreleased] not found in Release-Notes.md")
    after = idx + len(marker)
    # Clear any body under Unreleased until next ## heading.
    rest = notes[after:]
    next_h = re.search(r"\n## \[", rest)
    unreleased_tail = rest[next_h.start() :] if next_h else "\n"
    notes_new = notes[:after] + "\n\n" + section_md + unreleased_tail
    NOTES.write_text(notes_new, encoding="utf-8")


def write_github_output(data: dict) -> None:
    path = os.environ.get("GITHUB_OUTPUT")
    if not path:
        return
    with open(path, "a", encoding="utf-8") as fh:
        for key in ("should_release", "version", "tag", "bump"):
            fh.write(f"{key}={data[key]}\n")
        fh.write("changelog<<EOF\n")
        fh.write(data.get("changelog", ""))
        fh.write("\nEOF\n")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--apply", action="store_true", help="patch version files in-tree")
    parser.add_argument(
        "--github-output",
        action="store_true",
        help="append results to $GITHUB_OUTPUT",
    )
    parser.add_argument(
        "--base-tag",
        default=None,
        help="override latest tag (default: highest vX.Y.Z)",
    )
    args = parser.parse_args()

    tag = args.base_tag or latest_tag()
    if tag:
        base = parse_version(tag)
    else:
        base = parse_version(read_cmake_version())

    messages = commit_messages_since(tag)
    kind, entries = classify(messages)
    cmake_ver = read_cmake_version()

    if kind == "none":
        # Already released at HEAD, or only bot release commits since tag.
        should = False
        next_ver = cmake_ver
        # Exception: CMake ahead of last tag and tag missing → finish release.
        if tag and parse_version(cmake_ver) > base:
            remote_missing = True
            try:
                run_git("rev-parse", "-q", "--verify", f"refs/tags/v{cmake_ver}")
                remote_missing = False
            except subprocess.CalledProcessError:
                remote_missing = True
            if remote_missing:
                should = True
                next_ver = cmake_ver
                entries = [("Changed", f"Release v{cmake_ver}")]
                kind = "patch"
    else:
        next_tuple = bump_version(base, kind)
        next_ver = format_ver(next_tuple)
        should = True
        # If tag for next_ver already exists locally, do not release again.
        try:
            run_git("rev-parse", "-q", "--verify", f"refs/tags/v{next_ver}")
            should = False
        except subprocess.CalledProcessError:
            pass

    day = dt.date.today().isoformat()
    changelog = (
        render_changelog_section(next_ver, entries, day) if should and entries else ""
    )

    payload = {
        "should_release": "true" if should else "false",
        "version": next_ver,
        "tag": f"v{next_ver}",
        "bump": kind,
        "base_tag": tag or "",
        "cmake_version": cmake_ver,
        "commits_considered": len(messages),
        "changelog": changelog,
    }

    if args.apply and should:
        apply_files(next_ver, changelog if changelog else render_changelog_section(next_ver, entries or [("Changed", "Automated release")], day))

    if args.github_output:
        write_github_output(payload)

    json.dump(payload, sys.stdout, indent=2)
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
