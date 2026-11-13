"""Validate a release tag and optionally extract its CHANGELOG section."""

import argparse
from pathlib import Path
import re

NUMBER = r"(?:0|[1-9][0-9]*)"
PRE_ID = r"(?:0|[1-9][0-9]*|[0-9]*[A-Za-z-][0-9A-Za-z-]*)"
TAG = re.compile(
    rf"v(?P<version>{NUMBER}\.{NUMBER}\.{NUMBER}"
    rf"(?:-(?P<pre>{PRE_ID}(?:\.{PRE_ID})*))?"
    r"(?:\+[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?)"
)


def metadata(tag):
    match = TAG.fullmatch(tag)
    if not match:
        raise ValueError("Expected a version tag such as v0.1.0 or v0.1.0-beta.1")
    return match["version"], match["pre"] is not None


def release_notes(changelog, version):
    heading = re.compile(rf"^## \[(?:v)?{re.escape(version)}\](?:\s+-\s+.+)?\s*$")
    sections = []
    current = None
    for line in changelog.splitlines():
        if line.startswith("## "):
            if current is not None:
                sections.append("\n".join(current).strip())
            current = [] if heading.fullmatch(line) else None
        elif current is not None:
            current.append(line)
    if current is not None:
        sections.append("\n".join(current).strip())
    if len(sections) > 1 or (sections and not sections[0]):
        raise ValueError("Release CHANGELOG section is duplicated or empty")
    return sections[0] if sections else None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tag", default="")
    parser.add_argument("--changelog", type=Path, default=Path("CHANGELOG.md"))
    parser.add_argument("--notes", type=Path)
    args = parser.parse_args()
    try:
        version, prerelease = metadata(args.tag) if args.tag else ("dev", False)
        notes = (
            release_notes(args.changelog.read_text(encoding="utf-8"), version)
            if args.tag
            else None
        )
    except (ValueError, OSError) as error:
        parser.error(str(error))
    if args.notes:
        args.notes.write_text(notes + "\n" if notes else "", encoding="utf-8")
    print(f"version={version}")
    print(f"prerelease={str(prerelease).lower()}")
    print(f"has_notes={str(notes is not None).lower()}")


if __name__ == "__main__":
    main()
