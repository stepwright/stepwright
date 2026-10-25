#!/usr/bin/env python3
"""Extract one dated Keep a Changelog version section, never Unreleased."""

import argparse
import pathlib
import re


def extract(document, version):
    """Return the requested section body or fail before publishing bad notes."""
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", version):
        raise ValueError("release version must be MAJOR.MINOR.PATCH")
    heading = re.compile(r"^## \[" + re.escape(version) + r"\] - \d{4}-\d{2}-\d{2}\s*$", re.M)
    matches = list(heading.finditer(document))
    if len(matches) != 1:
        raise ValueError(f"expected one dated CHANGELOG section for {version}")
    rest = document[matches[0].end():]
    body = re.split(r"^## ", rest, maxsplit=1, flags=re.M)[0].strip()
    if not body:
        raise ValueError("release notes must not be empty")
    return body + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("version", help="version without the v tag prefix")
    parser.add_argument("--changelog", type=pathlib.Path, default=pathlib.Path("CHANGELOG.md"))
    args = parser.parse_args()
    try:
        print(extract(args.changelog.read_text(encoding="utf-8"), args.version), end="")
    except ValueError as error:
        parser.error(str(error))


if __name__ == "__main__":
    main()
