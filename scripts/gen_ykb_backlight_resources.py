#!/usr/bin/env python3

import argparse
import os
import subprocess
from pathlib import Path

ARRAY_SECTIONS = ("led_map", "x_coordinates", "y_coordinates")


def c_ident(name: str) -> str:
    out = []
    for ch in name:
        if ch.isalnum():
            out.append(ch)
        else:
            out.append("_")
    return "".join(out)


def parse_layout(path: Path):
    data = {section: [] for section in ARRAY_SECTIONS}
    current = None

    for lineno, raw_line in enumerate(path.read_text().splitlines(), start=1):
        line = raw_line.split("#", 1)[0].strip()
        if not line:
            continue

        if line.startswith("[") and line.endswith("]"):
            current = line[1:-1].strip().lower()
            if current not in data:
                raise ValueError(
                    f"{path}:{lineno}: unknown section '{current}', "
                    f"expected one of {', '.join(ARRAY_SECTIONS)}")
            continue

        if current is None:
            raise ValueError(
                f"{path}:{lineno}: content found before any section")

        parts = [part.strip() for part in line.split("|")]
        if len(parts) > 2:
            raise ValueError(
                f"{path}:{lineno}: section '{current}' should contain at most one '|' separator per row"
            )

        left_tokens = parts[0].split() if parts[0] else []
        right_tokens = parts[1].split() if len(parts) == 2 and parts[1] else []
        data[current].append((left_tokens, right_tokens))

    return data


def flatten_split_rows(rows):
    left_values = []
    right_values = []

    for left_tokens, right_tokens in rows:
        left_values.extend(left_tokens)
        right_values.extend(right_tokens)

    return left_values + right_values


def parse_u16_array(tokens, path: Path, section: str):
    values = []
    for idx, token in enumerate(tokens):
        try:
            value = int(token, 10)
        except ValueError as exc:
            raise ValueError(
                f"{path}: {section} token #{idx} '{token}' is not an integer"
            ) from exc

        if value < 0 or value > 65535:
            raise ValueError(
                f"{path}: {section} token #{idx} value {value} is outside 0..65535"
            )

        values.append(value)

    return values


def format_c_array(values, wrap=8, indent="    "):
    lines = []
    for start in range(0, len(values), wrap):
        chunk = values[start:start + wrap]
        lines.append(indent + ", ".join(values for values in chunk) + ",")
    return "\n".join(lines) if lines else ""


def run_command(args):
    env = os.environ.copy()
    env["CCACHE_DISABLE"] = "1"
    subprocess.run([str(arg) for arg in args], check=True, env=env)


def ensure_lumic(lumiscript_dir: Path, build_dir: Path) -> Path:
    lumic = build_dir / "lumic"
    build_file = build_dir / "build.ninja"

    if not (lumiscript_dir / "meson.build").is_file():
        raise ValueError(f"{lumiscript_dir}: lumiscript meson.build not found")

    if not build_file.is_file():
        build_dir.mkdir(parents=True, exist_ok=True)
        run_command([
            "meson",
            "setup",
            build_dir,
            lumiscript_dir,
            "--buildtype=release",
        ])

    run_command(["meson", "compile", "-C", build_dir, "lumic"])

    if not lumic.is_file():
        raise ValueError(f"{lumic}: lumic build did not produce expected binary")

    return lumic


def compile_lumiscript(lumic: Path, source: Path, output: Path,
                       optimization: str):
    output.parent.mkdir(parents=True, exist_ok=True)
    run_command([lumic, optimization, source, "-o", output])


def resolve_resource_files(files, lumiscript_dir, lumiscript_build_dir,
                           compiled_dir, optimization):
    resolved = []
    lumic = None

    for file_path in files:
        suffix = file_path.suffix.lower()
        if suffix == ".lbc":
            resolved.append((file_path.stem, file_path))
            continue

        if suffix != ".lumi":
            raise ValueError(
                f"{file_path}: expected .lumi source or .lbc bytecode")

        if (lumiscript_dir is None or lumiscript_build_dir is None or
                compiled_dir is None):
            raise ValueError(
                f"{file_path}: .lumi inputs require --lumiscript-dir, "
                "--lumiscript-build-dir and --compiled-dir")

        if lumic is None:
            lumic = ensure_lumic(lumiscript_dir, lumiscript_build_dir)

        compiled_path = compiled_dir / f"{file_path.stem}.lbc"
        compile_lumiscript(lumic, file_path, compiled_path, optimization)
        resolved.append((file_path.stem, compiled_path))

    return resolved


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--out-c", required=True)
    parser.add_argument("--out-h", required=True)
    parser.add_argument("--layout", required=True)
    parser.add_argument("--symbol", default="default_backlight_settings")
    parser.add_argument("--lumiscript-dir")
    parser.add_argument("--lumiscript-build-dir")
    parser.add_argument("--compiled-dir")
    parser.add_argument("--lumic-optimization", default="-O3")
    parser.add_argument("resources", nargs="+")
    args = parser.parse_args()

    input_files = [Path(p) for p in args.resources]
    files = resolve_resource_files(
        input_files,
        Path(args.lumiscript_dir) if args.lumiscript_dir else None,
        Path(args.lumiscript_build_dir) if args.lumiscript_build_dir else None,
        Path(args.compiled_dir) if args.compiled_dir else None,
        args.lumic_optimization,
    )
    layout_path = Path(args.layout)
    layout_sections = parse_layout(layout_path)
    led_map = parse_u16_array(flatten_split_rows(layout_sections["led_map"]), layout_path,
                              "led_map")
    x_coordinates = parse_u16_array(flatten_split_rows(layout_sections["x_coordinates"]),
                                    layout_path, "x_coordinates")
    y_coordinates = parse_u16_array(flatten_split_rows(layout_sections["y_coordinates"]),
                                    layout_path, "y_coordinates")

    key_count = len(led_map)
    if key_count == 0:
        raise ValueError(f"{layout_path}: led_map section is empty")
    if len(x_coordinates) != key_count:
        raise ValueError(
            f"{layout_path}: x_coordinates has {len(x_coordinates)} entries, "
            f"expected {key_count}")
    if len(y_coordinates) != key_count:
        raise ValueError(
            f"{layout_path}: y_coordinates has {len(y_coordinates)} entries, "
            f"expected {key_count}")

    out_c = Path(args.out_c)
    out_h = Path(args.out_h)

    out_h.write_text(f"""\
#pragma once

#include <subsys/kb_settings.h>
#include <subsys/ykb_backlight.h>

#define GENERATED_BACKLIGHT_LAYOUT_KEY_COUNT {key_count}U

extern const ykb_backlight_script_slots_t {args.symbol};
""")

    slot_entries = []
    for index, (name, file_path) in enumerate(files):
        data = file_path.read_bytes()
        bytecode_init = format_c_array([str(b) for b in data],
                                       wrap=16,
                                       indent="                ")
        slot_entries.append(f"""\
        [{index}] = {{
            .occupied = true,
            .size = {len(data)},
            .name = "{name}",
            .bytecode = {{
{bytecode_init}
            }},
        }},""")

    slots_init = "\n".join(slot_entries)

    out_c.write_text(f"""\
#include "generated_backlight_resources.h"

#include <zephyr/sys/util.h>

static const uint16_t generated_backlight_led_map[] = {{
{format_c_array([str(value) for value in led_map])}
}};

static const uint16_t generated_backlight_x_coordinates[] = {{
{format_c_array([str(value) for value in x_coordinates])}
}};

static const uint16_t generated_backlight_y_coordinates[] = {{
{format_c_array([str(value) for value in y_coordinates])}
}};

#if CONFIG_KB_HANDLER_SPLITLINK_SLAVE
#define GENERATED_BACKLIGHT_LAYOUT_OFFSET CONFIG_KB_SETTINGS_KEY_COUNT
#define GENERATED_BACKLIGHT_LAYOUT_LOCAL_KEY_COUNT CONFIG_KB_SETTINGS_KEY_COUNT_SLAVE
#else
#define GENERATED_BACKLIGHT_LAYOUT_OFFSET 0U
#define GENERATED_BACKLIGHT_LAYOUT_LOCAL_KEY_COUNT CONFIG_KB_SETTINGS_KEY_COUNT
#endif

BUILD_ASSERT(GENERATED_BACKLIGHT_LAYOUT_KEY_COUNT == TOTAL_KEY_COUNT,
             "generated backlight layout should match TOTAL_KEY_COUNT");
BUILD_ASSERT({len(files)} <= CONFIG_YKB_BL_SCRIPT_SLOT_COUNT,
             "default backlight scripts exceed configured slot count");

static const ykb_backlight_layout_t generated_backlight_layout = {{
    .key_count = GENERATED_BACKLIGHT_LAYOUT_LOCAL_KEY_COUNT,
    .led_map = &generated_backlight_led_map[GENERATED_BACKLIGHT_LAYOUT_OFFSET],
    .x_coordinates =
        &generated_backlight_x_coordinates[GENERATED_BACKLIGHT_LAYOUT_OFFSET],
    .y_coordinates =
        &generated_backlight_y_coordinates[GENERATED_BACKLIGHT_LAYOUT_OFFSET],
}};

const ykb_backlight_layout_t *ykb_backlight_get_layout(void) {{
    return &generated_backlight_layout;
}}

const ykb_backlight_script_slots_t {args.symbol} =
{{
    .slots = {{
{slots_init}
    }},
}};
""")


if __name__ == "__main__":
    main()
