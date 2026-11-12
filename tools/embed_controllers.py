#!/usr/bin/env python3
"""Validate the build-time controller registry and emit fixed decoder references.

Importing this module performs no I/O. No registry field is executable code.
"""

import argparse
import json
import os
import re
import tempfile
from pathlib import Path

MAX_BYTES = 256 * 1024
IMPLEMENTATIONS = {
    "ds4": ("ds4_parse", "ds4_output", "ds4_init", "NULL"),
    "ds5": ("ds5_parse", "ds5_output", "ds5_init", "NULL"),
    "xbox": ("xbox_parse", "xbox_output", "NULL", "xbox_setup"),
    "switch": ("switch_parse", "switch_output", "switch_init", "NULL"),
    "joycon-left": ("joycon_left_parse", "switch_output", "switch_init", "NULL"),
    "joycon-right": ("joycon_right_parse", "switch_output", "switch_init", "NULL"),
}
BUTTONS = frozenset(
    "cross circle square triangle l1 r1 l2 r2 l3 r3 create share options ps "
    "touchpad up down left right none".split()
)
ROLES = frozenset("lx ly rx ry l2 r2".split())
AXES = frozenset("x y z rx ry rz slider dial brake accelerator accel none".split())
IDENTIFIER = re.compile(r"[a-z][a-z0-9_-]{0,31}\Z")
HEX_ID = re.compile(r"[0-9a-fA-F]{4}\Z")
BUTTON_KEY = re.compile(r"button([1-9]|[12][0-9]|3[0-2])\Z")


class RegistryError(ValueError):
    """Invalid declarative registry; no output should be written."""


def require(condition, message):
    if not condition:
        raise RegistryError(message)


def object_fields(value, allowed, required, where):
    require(type(value) is dict, f"{where}: expected object")
    require(not (set(value) - allowed), f"{where}: unknown fields")
    require(required <= set(value), f"{where}: missing required fields")


def name(value, where):
    require(
        type(value) is str
        and 1 <= len(value) <= 63
        and all(32 <= ord(character) <= 126 for character in value),
        f"{where}: expected 1..63 printable ASCII characters",
    )


def c_string(value):
    # JSON's ASCII string escapes are C-compatible for these validated values.
    # Escape '?' too, preventing C11 trigraphs from changing quoted contents.
    return json.dumps(value, ensure_ascii=True).replace("?", r"\?")


def no_duplicates(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, f"duplicate JSON key: {key}")
        result[key] = value
    return result


def reject_constant(value):
    raise RegistryError(f"non-JSON constant: {value}")


def load_config(path):
    with Path(path).open("rb") as stream:
        raw = stream.read(MAX_BYTES + 1)
    require(len(raw) <= MAX_BYTES, "registry exceeds 256 KiB")
    try:
        return json.loads(
            raw.decode("utf-8"),
            object_pairs_hook=no_duplicates,
            parse_constant=reject_constant,
        )
    except (UnicodeError, json.JSONDecodeError, RecursionError) as error:
        raise RegistryError(f"invalid JSON: {error}") from error


def mapping(layout, where):
    object_fields(layout, {"bindings"}, {"bindings"}, where)
    bindings = layout["bindings"]
    require(type(bindings) is dict, f"{where}: bindings must be an object")
    lines = []
    for key, target in bindings.items():
        require(type(key) is str and type(target) is str, f"{where}: invalid binding")
        if BUTTON_KEY.fullmatch(key):
            require(target in BUTTONS, f"{where}: unknown button target")
        else:
            require(key in ROLES and target in AXES, f"{where}: invalid axis binding")
        lines.append(f"{key}={'accel' if target == 'accelerator' else target}\n")
    result = "".join(lines)
    require(
        len(result.encode("ascii")) < 1024, f"{where}: mapping must be under 1024 bytes"
    )
    return result


def generate(config):
    """Validate a parsed registry; return deterministic (profiles, models) C text."""
    object_fields(
        config,
        {"schema_version", "profiles", "layouts", "devices"},
        {"schema_version", "profiles", "layouts", "devices"},
        "registry",
    )
    require(
        type(config["schema_version"]) is int and config["schema_version"] == 1,
        "schema_version must be 1",
    )
    profiles, layouts, devices = (
        config[key] for key in ("profiles", "layouts", "devices")
    )
    require(
        type(profiles) is dict and 1 <= len(profiles) <= 32, "expected 1..32 profiles"
    )
    require(type(layouts) is dict and len(layouts) <= 32, "expected at most 32 layouts")
    require(
        type(devices) is list and 1 <= len(devices) <= 256, "expected 1..256 devices"
    )
    for key, profile in profiles.items():
        require(
            type(key) is str and IDENTIFIER.fullmatch(key), "invalid profile identifier"
        )
        object_fields(
            profile,
            {"name", "implementation", "output", "needs_full", "rumble_refresh_ms"},
            {"name", "implementation", "output", "needs_full", "rumble_refresh_ms"},
            f"profile {key}",
        )
        name(profile["name"], f"profile {key} name")
        require(
            type(profile["implementation"]) is str
            and profile["implementation"] in IMPLEMENTATIONS,
            "unknown decoder implementation",
        )
        require(
            type(profile["output"]) is bool and type(profile["needs_full"]) is bool,
            "profile flags must be booleans",
        )
        refresh = profile["rumble_refresh_ms"]
        require(
            type(refresh) is int and 0 <= refresh <= 1000,
            "invalid rumble refresh interval",
        )
        require(
            profile["output"] or refresh == 0,
            "disabled output cannot have rumble refresh",
        )
        require(
            not profile["needs_full"]
            or IMPLEMENTATIONS[profile["implementation"]][2] != "NULL",
            "decoder has no full-report initialization",
        )
    maps = {}
    for key, layout in layouts.items():
        require(
            type(key) is str and IDENTIFIER.fullmatch(key), "invalid layout identifier"
        )
        maps[key] = mapping(layout, f"layout {key}")
    identities = set()
    grouped = {key: [] for key in profiles}
    models = []
    for index, device in enumerate(devices):
        where = f"device {index}"
        object_fields(
            device,
            {
                "vid",
                "pid",
                "name",
                "bluetooth_profile",
                "hid_layout",
                "usb",
                "identity_kind",
                "rumble",
                "ble",
            },
            {"vid", "pid"},
            where,
        )
        for field in ("vid", "pid"):
            require(
                type(device[field]) is str and HEX_ID.fullmatch(device[field]),
                f"{where}: {field} must be four hexadecimal digits",
            )
        identity = tuple(int(device[field], 16) for field in ("vid", "pid"))
        require(identity not in identities, f"{where}: duplicate VID/PID")
        identities.add(identity)
        if "name" in device:
            name(device["name"], f"{where} name")
        if "identity_kind" in device:
            require(
                type(device["identity_kind"]) is str
                and device["identity_kind"] in {"reported-model", "protocol"},
                f"{where}: invalid identity_kind",
            )
        if "ble" in device:
            require(type(device["ble"]) is bool, f"{where}: ble must be boolean")
        if "rumble" in device:
            require(type(device["rumble"]) is bool, f"{where}: rumble must be boolean")
            require("bluetooth_profile" in device, f"{where}: rumble requires profile")
        if "bluetooth_profile" in device:
            reference = device["bluetooth_profile"]
            require(
                type(reference) is str and reference in profiles,
                f"{where}: unknown profile reference",
            )
            grouped[reference].append(identity)
        if "hid_layout" in device:
            require(
                type(device["hid_layout"]) is str and device["hid_layout"] in maps,
                f"{where}: unknown layout reference",
            )
        flags = 0
        if "usb" in device:
            usb = device["usb"]
            object_fields(
                usb, {"xinput_activation", "input_watchdog"}, set(), f"{where} USB"
            )
            for flag, bit in (("xinput_activation", 1), ("input_watchdog", 2)):
                if flag in usb:
                    require(
                        type(usb[flag]) is bool, f"{where}: USB flags must be booleans"
                    )
                    if usb[flag]:
                        flags |= bit
        models.append((device, identity, flags))
    prefix = "/* Generated by tools/embed_controllers.py; do not edit. */\n"
    profile_text = [prefix]
    for index, key in enumerate(profiles):
        if grouped[key]:
            profile_text.append(
                f"static const uint16_t uc_profile_ids_{index}[][2] = {{\n"
            )
            profile_text.extend(
                f"    {{0x{vid:04x}, 0x{pid:04x}}},\n" for vid, pid in grouped[key]
            )
            profile_text.append("};\n")
    profile_text.append("static const pad_profile k_profiles[] = {\n")
    for index, (key, profile) in enumerate(profiles.items()):
        parser, output, init, setup = IMPLEMENTATIONS[profile["implementation"]]
        ids = f"uc_profile_ids_{index}" if grouped[key] else "NULL"
        profile_text.append(
            f"    {{{c_string(profile['name'])}, {ids}, {len(grouped[key])}, {parser}, "
            f"{output if profile['output'] else 'NULL'}, {init}, {int(profile['needs_full'])}, "
            f"{profile['rumble_refresh_ms']}, {setup}}},\n"
        )
    profile_text.append("};\n")
    profile_text.append("static const char *const k_profile_keys[] = {\n")
    profile_text.extend(f"    {c_string(key)},\n" for key in profiles)
    profile_text.append("};\n")
    # profiles.c intentionally contains the small fixed decoder implementations.
    # A custom subset must still compile with -Wunused-function -Werror. These
    # unreachable metadata anchors mark omitted callbacks used, without adding
    # them to k_profiles or changing any VID/PID lookup/transport permissions.
    used_callbacks = set()
    for profile in profiles.values():
        parser, output, init, setup = IMPLEMENTATIONS[profile["implementation"]]
        used_callbacks.update((parser, init, setup))
        if profile["output"]:
            used_callbacks.add(output)
    missing = []
    for implementation, callbacks in IMPLEMENTATIONS.items():
        if any(
            callback != "NULL" and callback not in used_callbacks
            for callback in callbacks
        ):
            missing.append((implementation, callbacks))
            used_callbacks.update(callbacks)
    if missing:
        profile_text.append(
            "static const pad_profile uc_decoder_callback_anchors[] __attribute__((unused)) = {\n"
        )
        for implementation, (parser, output, init, setup) in missing:
            profile_text.append(
                f"    {{{c_string(implementation)}, NULL, 0, {parser}, {output}, {init}, 0, 0, {setup}}},\n"
            )
        profile_text.append("};\n")
    model_text = [prefix]
    map_symbols = {key: f"uc_hid_map_{index}" for index, key in enumerate(maps)}
    # Only referenced maps are emitted: unused declarative layouts remain legal
    # without causing unused-const-variable warnings in strict C builds.
    used_maps = {device["hid_layout"] for device in devices if "hid_layout" in device}
    for key, text in maps.items():
        if key in used_maps:
            model_text.append(
                f"static const char {map_symbols[key]}[] = {c_string(text)};\n"
            )
    model_text.append(
        "static const struct { uint16_t vid, pid; const char *name; const char *map; unsigned usb_flags; int ble; } k_models[] = {\n"
    )
    for device, (vid, pid), flags in models:
        display = c_string(device["name"]) if "name" in device else "NULL"
        layout = map_symbols[device["hid_layout"]] if "hid_layout" in device else "NULL"
        model_text.append(
            f"    {{0x{vid:04x}, 0x{pid:04x}, {display}, {layout}, {flags}u, {int(device.get('ble', False))}}},\n"
        )
    model_text.append("};\n")
    # Immutable capability views reuse the same decoder and initialization.
    model_text.append(
        "static const struct { uint16_t vid, pid; pad_profile profile; } k_no_rumble[] = {\n"
    )
    for device, (vid, pid), flags in models:
        if device.get("rumble") is not False:
            continue
        profile = profiles[device["bluetooth_profile"]]
        parser, output, init, setup = IMPLEMENTATIONS[profile["implementation"]]
        model_text.append(
            f"    {{0x{vid:04x}, 0x{pid:04x}, {{{c_string(profile['name'])}, NULL, 0, "
            f"{parser}, NULL, {init}, {int(profile['needs_full'])}, 0, {setup}}}}},\n"
        )
    model_text.append("    {0, 0, {0}},\n};\n")
    return "".join(profile_text), "".join(model_text)


def write_outputs(directory, contents):
    # One atomic artifact keeps decoder bindings and device metadata consistent.
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    descriptor, temporary = tempfile.mkstemp(
        prefix=".controller_registry.", dir=directory
    )
    try:
        with os.fdopen(descriptor, "w", encoding="ascii", newline="\n") as stream:
            stream.write("".join(contents))
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, directory / "controller_registry.inc")
    finally:
        try:
            os.unlink(temporary)
        except FileNotFoundError:
            pass


def main(argv=None):
    root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--config", type=Path, default=root / "src/drivers/protocols/controllers.json"
    )
    parser.add_argument("--output-dir", type=Path, default=root / "build")
    args = parser.parse_args(argv)
    try:
        contents = generate(load_config(args.config))
        write_outputs(args.output_dir, contents)
    except (RegistryError, OSError) as error:
        parser.exit(1, f"controller registry: {error}\n")


if __name__ == "__main__":
    main()
