"""Resolve CI versions from the reviewed VERSION file, never from an old tag."""

import argparse
from pathlib import Path
import re


def read_version(path):
    value = path.read_text().strip()
    if not re.fullmatch(r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)", value):
        raise ValueError("VERSION must contain one stable X.Y.Z version without a v prefix")
    return value


def resolve(path, event, ref, pr, sha):
    base = read_version(path)
    if not re.fullmatch(r"[0-9a-f]{7,40}", sha):
        raise ValueError("Expected a tested commit SHA")
    if event == "push" and ref == "refs/heads/main":
        return base
    if event == "pull_request" and re.fullmatch(r"[1-9][0-9]*", pr):
        return f"{base}-pr-{pr}-{sha[:7]}"
    raise ValueError("CI only supports main pushes and pull requests")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--file", type=Path, default=Path("VERSION"))
    parser.add_argument("--event", required=True)
    parser.add_argument("--ref", required=True)
    parser.add_argument("--pr", default="")
    parser.add_argument("--sha", required=True)
    args = parser.parse_args()
    print(resolve(args.file, args.event, args.ref, args.pr, args.sha))
