#!/usr/bin/env python3
"""Recolor the fixed logo geometry; optionally render using rsvg-convert."""
import argparse
import re
import shutil
import subprocess
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--background", default="#FF8C00")
    parser.add_argument("--foreground", default="#0D0E11")
    parser.add_argument("--svg-only", action="store_true")
    args = parser.parse_args()
    for value in (args.background, args.foreground):
        if not re.fullmatch(r"#[0-9a-fA-F]{6}", value):
            parser.error("colors must be #RRGGBB")
    renderer = shutil.which("rsvg-convert")
    if not args.svg_only and not renderer:
        parser.error(
            "install librsvg (rsvg-convert), or use --svg-only with another SVG renderer"
        )
    root = Path(__file__).resolve().parent.parent / "assets/branding"
    svg = (root / "icon.template.svg").read_text()
    svg = svg.replace("{{background}}", args.background.upper()).replace(
        "{{foreground}}", args.foreground.upper()
    )
    (root / "icon.svg").write_text(svg)
    if not args.svg_only:
        subprocess.run(
            [
                renderer,
                "-w",
                "512",
                "-h",
                "512",
                "-o",
                str(root / "icon0.png"),
                str(root / "icon.svg"),
            ],
            check=True,
        )


if __name__ == "__main__":
    main()
