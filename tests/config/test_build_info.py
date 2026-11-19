"""Build labels must describe the checkout without executing or corrupting refs."""

import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "build_info", ROOT / "tools/build_info.py"
)
build = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(build)


class BuildInfoTests(unittest.TestCase):
    def test_release_and_branch_labels(self):
        sha = "abcdef123456" + "a" * 28
        self.assertEqual(
            build.identity("0.1.2", "master", sha)["build_id"], "v0.1.2 + abcdef123456"
        )
        self.assertEqual(
            build.identity("dev", "feat/test", sha)["build_id"],
            "feat/test + abcdef123456",
        )
        self.assertEqual(build.identity("0.1.2-rc.1", "tag", sha)["branch"], "")
        for bad in ("abc", "g" * 40, "a" * 40 + "\n"):
            with self.assertRaises(ValueError):
                build.identity("dev", "master", bad)
        with self.assertRaises(ValueError):
            build.identity("dev", "bad\nref", sha)

    def test_header_and_http_json_preserve_untrusted_label(self):
        compiler = shutil.which("cc")
        if not compiler:
            self.skipTest("C compiler unavailable")
        info = build.identity("dev", 'feat/"quote"/$(`text`)/??/中文', "a" * 40)
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            (directory / "build_info.h").write_text(build.header(info))
            source = directory / "test.c"
            source.write_text(
                '#include <stdio.h>\n#include "build_info.h"\n'
                'int main(void) { puts("{" UC_BUILD_METADATA_JSON "}"); return 0; }\n'
            )
            binary = directory / "test"
            subprocess.run(
                [
                    compiler,
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    str(source),
                    "-o",
                    str(binary),
                ],
                check=True,
            )
            result = subprocess.check_output([str(binary)], text=True)
            self.assertEqual(json.loads(result), info)


if __name__ == "__main__":
    unittest.main()
