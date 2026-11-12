"""Check the manually organized Makefiles without rewriting recipe tabs."""

from pathlib import Path
import sys

errors = []
for filename in sys.argv[1:]:
    text = Path(filename).read_text()
    if not text.endswith("\n"):
        errors.append(f"{filename}: missing final newline")
    for line_number, line in enumerate(text.splitlines(), 1):
        if line.rstrip() != line:
            errors.append(f"{filename}:{line_number}: trailing whitespace")
        if line.startswith(" "):
            errors.append(
                f"{filename}:{line_number}: use tabs for recipes/continuations"
            )
        if len(line.expandtabs(4)) > 120:
            errors.append(f"{filename}:{line_number}: exceeds 120 columns")
if errors:
    raise SystemExit("\n".join(errors))
print("Makefile style checks passed")
