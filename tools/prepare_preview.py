"""Map application URLs to a static python -m http.server document root.

Symlinks keep edits live without copying or changing production HTML URLs.
Only the explicitly generated preview links are replaced.
"""

from pathlib import Path

root = Path(__file__).resolve().parent.parent
preview = root / "build" / "preview"
links = {
    "index.html": root / "web/control/index.html",
    "controller/index.html": root / "web/controller/index.html",
    "settings/index.html": root / "web/control/settings.html",
    "assets": root / "web",
}
for relative, source in links.items():
    destination = preview / relative
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.is_symlink():
        destination.unlink()
    elif destination.exists():
        raise SystemExit(f"Refusing to replace non-generated path: {destination}")
    destination.symlink_to(source, target_is_directory=source.is_dir())
print("Static preview prepared; APIs are unavailable in this preview.")
