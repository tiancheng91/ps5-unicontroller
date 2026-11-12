"""Registry validation/build tests; mappings below are synthetic test layouts."""

import copy
import importlib.util
import json
import subprocess
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "embed_controllers", ROOT / "tools/embed_controllers.py"
)
registry = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(registry)


def example():
    return {
        "schema_version": 1,
        "profiles": {
            "wired-pad": {
                "name": "Existing Xbox decoder",
                "implementation": "xbox",
                "output": True,
                "needs_full": False,
                "rumble_refresh_ms": 0,
            }
        },
        "layouts": {
            "test-layout": {
                "bindings": {"button1": "cross", "rx": "z"},
            }
        },
        "devices": [
            {
                "vid": "1234",
                "pid": "abcd",
                "name": "Synthetic controller",
                "bluetooth_profile": "wired-pad",
                "hid_layout": "test-layout",
            }
        ],
    }


class ControllerRegistryTests(unittest.TestCase):
    def invalid(self, mutate):
        config = example()
        mutate(config)
        with self.assertRaises(registry.RegistryError):
            registry.generate(config)

    def test_duplicate_keys_utf8_size_and_non_json_constants(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "config.json"
            for raw in (
                b'{"schema_version":1,"schema_version":1}',
                b'{"nested":{"vid":"1234","vid":"abcd"}}',
                b'{"value":NaN}',
                b"\xff",
                b" " * (registry.MAX_BYTES + 1),
            ):
                path.write_bytes(raw)
                with self.assertRaises(registry.RegistryError):
                    registry.load_config(path)

    def test_schema_unknown_fields_and_decoder_whitelist(self):
        self.invalid(lambda c: c.update(schema_version=True))
        self.invalid(lambda c: c.update(schema_version=2))
        self.invalid(lambda c: c.update(script="print('no')"))
        self.invalid(
            lambda c: c["profiles"]["wired-pad"].update(
                implementation="custom_function"
            )
        )
        self.invalid(lambda c: c["profiles"]["wired-pad"].update(command=[0x80, 2]))
        self.invalid(lambda c: c["profiles"]["wired-pad"].update(needs_full=True))
        self.invalid(
            lambda c: c["profiles"]["wired-pad"].update(
                output=False, rumble_refresh_ms=1
            )
        )
        self.invalid(lambda c: c["profiles"]["wired-pad"].update(output=1))
        self.invalid(
            lambda c: c["profiles"]["wired-pad"].update(rumble_refresh_ms=True)
        )
        self.invalid(
            lambda c: c["profiles"]["wired-pad"].update(rumble_refresh_ms=1001)
        )
        self.invalid(lambda c: c["devices"][0].update(path="/tmp/controller"))

    def test_ids_references_and_flags(self):
        for value in (1234, "123", "12345", "0x12", "gggg"):
            self.invalid(lambda c, value=value: c["devices"][0].update(vid=value))
        self.invalid(lambda c: c["devices"].append({"vid": "1234", "pid": "ABCD"}))
        self.invalid(lambda c: c["devices"][0].update(bluetooth_profile="missing"))
        self.invalid(lambda c: c["devices"][0].update(hid_layout="missing"))
        self.invalid(lambda c: c["devices"][0].update(usb={"xinput_activation": 1}))
        self.invalid(lambda c: c["devices"][0].update(usb={"reset_device": True}))
        self.invalid(lambda c: c["devices"][0].update(identity_kind="authenticated"))
        config = example()
        config["devices"][0]["usb"] = {
            "xinput_activation": True,
            "input_watchdog": True,
        }
        config["devices"][0]["identity_kind"] = "protocol"
        self.assertIn("3u, 0}", registry.generate(config)[1])

    def test_layout_grammar_and_bounds(self):
        for bindings in (
            {"button0": "cross"},
            {"button33": "cross"},
            {"button01": "cross"},
            {"button1junk": "cross"},
            {"button1": "not-a-button"},
            {"lx": "system()"},
            {"axis": "x"},
            {"lx": "x\nbutton1=ps"},
        ):
            self.invalid(
                lambda c, bindings=bindings: c["layouts"]["test-layout"].update(
                    bindings=bindings
                )
            )
        self.invalid(lambda c: c["layouts"]["test-layout"].update(file="external.map"))
        config = example()
        layout = config["layouts"]["test-layout"]
        layout["bindings"] = {f"button{i}": "touchpad" for i in range(1, 33)}
        layout["bindings"].update({role: "accelerator" for role in registry.ROLES})
        text = registry.mapping(layout, "test")
        self.assertLess(len(text.encode()), 1024)
        self.assertIn("l2=accel\n", text)
        self.assertIn("button32=touchpad\n", text)

    def test_counts_and_names(self):
        self.invalid(lambda c: c.update(devices=c["devices"] * 257))
        self.invalid(
            lambda c: c.update(
                profiles={
                    f"profile-{i}": copy.deepcopy(c["profiles"]["wired-pad"])
                    for i in range(33)
                }
            )
        )
        self.invalid(
            lambda c: c.update(
                layouts={f"layout-{i}": {"bindings": {}} for i in range(33)}
            )
        )
        for value in ("", "a" * 64, "controller\nsecond line", "控制器"):
            self.invalid(lambda c, value=value: c["devices"][0].update(name=value))

    def test_escaping_and_order_are_deterministic(self):
        config = example()
        config["devices"][0]["name"] = 'Quoted "controller" \\ ??/'
        before = copy.deepcopy(config)
        first = registry.generate(config)
        self.assertEqual(first, registry.generate(config))
        self.assertEqual(config, before)
        self.assertIn(r"\"controller\"", first[1])
        self.assertIn(r"\\", first[1])
        self.assertIn(r"\?\?/", first[1])
        self.assertNotIn("??/", first[1])
        self.assertIn(r"button1=cross\nrx=z\n", first[1])

    def test_custom_profile_reuses_existing_decoder_and_layout(self):
        config = example()
        custom = copy.deepcopy(config["profiles"]["wired-pad"])
        custom["name"] = "Custom model using Xbox"
        config["profiles"]["custom"] = custom
        config["devices"].append(
            {
                "vid": "beef",
                "pid": "0001",
                "bluetooth_profile": "custom",
                "hid_layout": "test-layout",
                "usb": {"input_watchdog": True},
            }
        )
        profiles, models = registry.generate(config)
        self.assertIn("{0xbeef, 0x0001}", profiles)
        self.assertIn(
            '"Custom model using Xbox", uc_profile_ids_1, 1, xbox_parse, xbox_output, NULL, 0, 0, xbox_setup',
            profiles,
        )
        self.assertIn("{0xbeef, 0x0001, NULL, uc_hid_map_0, 2u, 0}", models)
        config["profiles"]["unused"] = custom
        self.assertIn(
            '"Custom model using Xbox", NULL, 0, xbox_parse',
            registry.generate(config)[0],
        )

    def test_cli_writes_only_after_success_and_uses_atomic_replacement(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            config = directory / "registry.json"
            outputs = directory / "generated"
            config.write_text(json.dumps(example()))
            command = [
                sys.executable,
                str(ROOT / "tools/embed_controllers.py"),
                "--config",
                str(config),
                "--output-dir",
                str(outputs),
            ]
            subprocess.run(command, check=True, capture_output=True)
            old = {path.name: path.read_bytes() for path in outputs.iterdir()}
            self.assertEqual(set(old), {"controller_registry.inc"})
            subprocess.run(command, check=True, capture_output=True)
            self.assertEqual(
                old, {path.name: path.read_bytes() for path in outputs.iterdir()}
            )
            config.write_text('{"schema_version":1,"schema_version":2}')
            result = subprocess.run(command, capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(
                old, {path.name: path.read_bytes() for path in outputs.iterdir()}
            )

    def test_decoder_subsets_compile_with_strict_unused_checks(self):
        compiler = shutil.which("clang") or shutil.which("cc")
        if not compiler:
            self.skipTest("C compiler unavailable")
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            for implementation in registry.IMPLEMENTATIONS:
                config = example()
                profile = config["profiles"]["wired-pad"]
                profile["implementation"] = implementation
                profile["needs_full"] = False
                profile["output"] = False
                profiles, models = registry.generate(config)
                self.assertIn("uc_decoder_callback_anchors", profiles)
                (directory / "controller_registry.inc").write_text(profiles + models)
                subprocess.run(
                    [
                        compiler,
                        "-std=c11",
                        "-Wall",
                        "-Wextra",
                        "-Werror",
                        "-Wunused-function",
                        "-fsyntax-only",
                        "-I" + str(directory),
                        "-I" + str(ROOT / "src/core"),
                        "-I" + str(ROOT / "src/drivers/common"),
                        str(ROOT / "src/drivers/protocols/profiles.c"),
                    ],
                    check=True,
                    capture_output=True,
                )
        baseline = registry.generate(
            registry.load_config(ROOT / "src/drivers/protocols/controllers.json")
        )
        self.assertNotIn("uc_decoder_callback_anchors", baseline[0])

    def test_cli_default_uses_source_builtin_registry(self):
        with tempfile.TemporaryDirectory() as directory:
            subprocess.run(
                [
                    sys.executable,
                    str(ROOT / "tools/embed_controllers.py"),
                    "--output-dir",
                    directory,
                ],
                check=True,
                capture_output=True,
            )
            generated = (Path(directory) / "controller_registry.inc").read_text()
            self.assertIn("0x3537, 0x1040", generated)
            self.assertIn("static const pad_profile k_profiles[]", generated)

    def test_repository_registry_builds(self):
        contents = registry.generate(
            registry.load_config(ROOT / "src/drivers/protocols/controllers.json")
        )
        self.assertIn("static const pad_profile k_profiles[]", contents[0])
        self.assertIn("k_models[]", contents[1])
        self.assertIn("0x3537, 0x1040", contents[1])


if __name__ == "__main__":
    unittest.main()
