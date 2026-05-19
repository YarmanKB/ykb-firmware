#!/usr/bin/env python3

import argparse
from pathlib import Path


ARRAY_SECTIONS = ("thresholds", "minimums", "maximums", "layer1", "layer2", "layer3")
FN_SHORTCUTS_SECTION = "fn_shortcuts"
MOUSEEMU_KEYS = {
    "enabled",
    "direction",
    "move_x",
    "move_y",
    "scroll",
}
MOUSEEMU_ARRAY_KEYS = {
    "move_keys",
    "scroll_keys",
    "button_keys",
    "move_keys_deadzones",
    "scroll_keys_deadzones",
}

FN_ACTIONS = {
    "MODE_NORMAL": "KB_FN_ACTION_MODE_NORMAL",
    "MODE_RACE": "KB_FN_ACTION_MODE_RACE",
    "MODE_MOUSESIM": "KB_FN_ACTION_MODE_MOUSESIM",
    "TRANSPORT_USB": "KB_FN_ACTION_TRANSPORT_USB",
    "TRANSPORT_BT": "KB_FN_ACTION_TRANSPORT_BT",
    "BL_TOGGLE": "KB_FN_ACTION_BL_TOGGLE",
    "BL_NEXT_SCRIPT": "KB_FN_ACTION_BL_NEXT_SCRIPT",
    "BL_PREV_SCRIPT": "KB_FN_ACTION_BL_PREV_SCRIPT",
    "BL_BRIGHTNESS_UP": "KB_FN_ACTION_BL_BRIGHTNESS_UP",
    "BL_BRIGHTNESS_DOWN": "KB_FN_ACTION_BL_BRIGHTNESS_DOWN",
    "BL_SPEED_UP": "KB_FN_ACTION_BL_SPEED_UP",
    "BL_SPEED_DOWN": "KB_FN_ACTION_BL_SPEED_DOWN",
}

FN_PARAM_ACTIONS = {
    "BL_SET_SCRIPT": ("KB_FN_ACTION_BL_SET_SCRIPT", "int"),
    "BL_SET_BRIGHTNESS": ("KB_FN_ACTION_BL_SET_BRIGHTNESS", "percent"),
    "BL_SET_SPEED": ("KB_FN_ACTION_BL_SET_SPEED", "speed"),
}


def parse_layout(path: Path):
    data = {section: [] for section in ARRAY_SECTIONS}
    data["mouseemu"] = {}
    data[FN_SHORTCUTS_SECTION] = []
    for key in MOUSEEMU_ARRAY_KEYS:
        data[key] = []
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
                    f"expected a known layout section"
                )
            continue

        if current is None:
            raise ValueError(f"{path}:{lineno}: content found before any section")

        if current == "mouseemu":
            if "=" not in line:
                raise ValueError(
                    f"{path}:{lineno}: mouseemu entries should use key = value"
                )
            key, value = [part.strip().lower() for part in line.split("=", 1)]
            if key not in MOUSEEMU_KEYS:
                raise ValueError(
                    f"{path}:{lineno}: unknown mouseemu key '{key}'"
                )
            data[current][key] = value
            continue

        if current == FN_SHORTCUTS_SECTION:
            data[current].append((lineno, line))
            continue

        if current in ARRAY_SECTIONS:
            parts = [part.strip() for part in line.split("|")]
            if len(parts) > 2:
                raise ValueError(
                    f"{path}:{lineno}: section '{current}' should contain at most one '|' separator per row"
                )

            left_tokens = parts[0].split() if parts[0] else []
            right_tokens = parts[1].split() if len(parts) == 2 and parts[1] else []

            row_store = data[current]
            if not row_store or not isinstance(row_store[0], tuple):
                data[current] = []
                row_store = data[current]

            row_store.append((left_tokens, right_tokens))
            continue

        for token in line.split():
            if token != "|":
                data[current].append(token)

    return data


def parse_u16_levels(tokens, path: Path, section_name: str):
    values = []
    for idx, token in enumerate(tokens):
        try:
            value = int(token, 10)
        except ValueError as exc:
            raise ValueError(
                f"{path}: {section_name} token #{idx} '{token}' is not an integer"
            ) from exc

        if value < 1 or value > 1023:
            raise ValueError(
                f"{path}: {section_name} token #{idx} value {value} is outside 1..1023"
            )
        values.append(value)

    return values


def normalize_key_token(token: str):
    return token if token.startswith("KEY_") else f"KEY_{token}"


def parse_fn_shortcuts(entries, path: Path):
    shortcuts = []

    for lineno, line in entries:
        if "=" not in line:
            raise ValueError(
                f"{path}:{lineno}: fn shortcut entries should use FN+KEY = ACTION"
            )

        lhs, rhs = [part.strip() for part in line.split("=", 1)]
        if "+" not in lhs:
            raise ValueError(
                f"{path}:{lineno}: fn shortcut lhs should be in FN+KEY form"
            )

        prefix, key_token = [part.strip() for part in lhs.split("+", 1)]
        if prefix.upper() != "FN":
            raise ValueError(
                f"{path}:{lineno}: fn shortcut lhs should start with FN+"
            )

        rhs = rhs.strip()
        if rhs.endswith(")") and "(" in rhs:
            action_name, arg = rhs[:-1].split("(", 1)
            action_token = action_name.strip().upper()
            arg = arg.strip()

            if action_token not in FN_PARAM_ACTIONS:
                raise ValueError(
                    f"{path}:{lineno}: unknown parameterized fn action '{action_name}'"
                )

            action_symbol, arg_kind = FN_PARAM_ACTIONS[action_token]

            if arg_kind == "int":
                try:
                    param = int(arg, 10)
                except ValueError as exc:
                    raise ValueError(
                        f"{path}:{lineno}: action '{action_name}' expects integer argument"
                    ) from exc
            elif arg_kind == "percent":
                try:
                    param = int(arg, 10)
                except ValueError as exc:
                    raise ValueError(
                        f"{path}:{lineno}: action '{action_name}' expects integer percent"
                    ) from exc
                if param < 0 or param > 100:
                    raise ValueError(
                        f"{path}:{lineno}: brightness percent should be in 0..100"
                    )
            elif arg_kind == "speed":
                try:
                    speed = float(arg)
                except ValueError as exc:
                    raise ValueError(
                        f"{path}:{lineno}: action '{action_name}' expects numeric speed"
                    ) from exc
                if speed < 0.1 or speed > 4.0:
                    raise ValueError(
                        f"{path}:{lineno}: speed should be in 0.1..4.0"
                    )
                param = int(round(speed * 100.0))
            else:
                raise ValueError(
                    f"{path}:{lineno}: unsupported argument kind '{arg_kind}'"
                )

            shortcuts.append((normalize_key_token(key_token), action_symbol, param))
            continue

        action_token = rhs.upper()
        if action_token not in FN_ACTIONS:
            raise ValueError(
                f"{path}:{lineno}: unknown fn shortcut action '{rhs}'"
            )

        shortcuts.append((normalize_key_token(key_token), FN_ACTIONS[action_token], 0))

    return shortcuts


def parse_bool(value: str, path: Path, key: str):
    lowered = value.lower()
    if lowered in ("true", "1", "yes", "on"):
        return "true"
    if lowered in ("false", "0", "no", "off"):
        return "false"
    raise ValueError(f"{path}: mouseemu {key} should be true/false")


def parse_ratio(value: str, path: Path, key: str):
    if "/" not in value:
        raise ValueError(f"{path}: mouseemu {key} should be in NUM/DEN form")
    num_str, den_str = [part.strip() for part in value.split("/", 1)]
    try:
        num = int(num_str, 10)
        den = int(den_str, 10)
    except ValueError as exc:
        raise ValueError(f"{path}: mouseemu {key} ratio should be integers") from exc
    if den == 0:
        raise ValueError(f"{path}: mouseemu {key} denominator should not be zero")
    return num, den


def parse_mouseemu_indices(tokens, path: Path, key: str, max_count: int):
    if len(tokens) > max_count:
        raise ValueError(
            f"{path}: mouseemu {key} has {len(tokens)} entries, max is {max_count}"
        )
    values = []
    for idx, token in enumerate(tokens):
        try:
            values.append(int(token, 10))
        except ValueError as exc:
            raise ValueError(
                f"{path}: mouseemu {key} token #{idx} '{token}' is not an integer"
            ) from exc
    return values


def format_c_array(values, wrap=8):
    lines = []
    for start in range(0, len(values), wrap):
        chunk = values[start : start + wrap]
        lines.append("    " + ", ".join(chunk) + ",")
    return "\n".join(lines) if lines else ""


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--layout", required=True)
    parser.add_argument("--out-c", required=True)
    parser.add_argument("--out-h", required=True)
    args = parser.parse_args()

    layout_path = Path(args.layout)
    out_c = Path(args.out_c)
    out_h = Path(args.out_h)

    sections = parse_layout(layout_path)

    def flatten_half_rows(rows, section_name):
        left = []
        right = []
        right_seen = False

        for left_tokens, right_tokens in rows:
            left.extend(left_tokens)
            right.extend(right_tokens)
            if right_tokens:
                right_seen = True

        if right_seen:
            return left + right
        return left

    thresholds = parse_u16_levels(
        flatten_half_rows(sections["thresholds"], "thresholds"), layout_path
        , "thresholds"
    )
    minimums_tokens = flatten_half_rows(sections["minimums"], "minimums")
    minimums = (
        parse_u16_levels(minimums_tokens, layout_path, "minimums")
        if minimums_tokens
        else list(thresholds)
    )
    maximums_tokens = flatten_half_rows(sections["maximums"], "maximums")
    maximums = (
        parse_u16_levels(maximums_tokens, layout_path, "maximums")
        if maximums_tokens
        else [1023] * len(thresholds)
    )
    layer1 = [
        normalize_key_token(token)
        for token in flatten_half_rows(sections["layer1"], "layer1")
    ]
    layer2 = [
        normalize_key_token(token)
        for token in flatten_half_rows(sections["layer2"], "layer2")
    ]
    layer3 = [
        normalize_key_token(token)
        for token in flatten_half_rows(sections["layer3"], "layer3")
    ]
    fn_shortcuts = parse_fn_shortcuts(sections[FN_SHORTCUTS_SECTION], layout_path)

    key_count = len(thresholds)
    if key_count == 0:
        raise ValueError(f"{layout_path}: thresholds section is empty")
    if len(minimums) not in (0, key_count):
        raise ValueError(
            f"{layout_path}: minimums has {len(minimums)} entries, expected {key_count}"
        )
    if len(maximums) not in (0, key_count):
        raise ValueError(
            f"{layout_path}: maximums has {len(maximums)} entries, expected {key_count}"
        )
    for idx, (minimum, maximum) in enumerate(zip(minimums, maximums)):
        if minimum > maximum:
            raise ValueError(
                f"{layout_path}: minimums[{idx}]={minimum} is greater than maximums[{idx}]={maximum}"
            )

    for name, layer in (("layer1", layer1), ("layer2", layer2), ("layer3", layer3)):
        if len(layer) not in (0, key_count):
            raise ValueError(
                f"{layout_path}: {name} has {len(layer)} entries, expected {key_count}"
            )

    if not layer1:
        raise ValueError(f"{layout_path}: layer1 section is empty")
    if not layer2:
        raise ValueError(f"{layout_path}: layer2 section is empty")
    if not layer3:
        layer3 = ["KEY_NOKEY"] * key_count

    mouseemu_cfg = sections["mouseemu"]
    mouseemu_enabled = parse_bool(mouseemu_cfg.get("enabled", "false"), layout_path,
                                  "enabled")
    mouseemu_direction = mouseemu_cfg.get("direction", "4way")
    if mouseemu_direction == "4way":
        mouseemu_direction = "KB_MOUSEEMU_DIRECTION_4_WAY"
    elif mouseemu_direction == "8way":
        mouseemu_direction = "KB_MOUSEEMU_DIRECTION_8_WAY"
    else:
        raise ValueError(f"{layout_path}: mouseemu direction should be 4way or 8way")

    move_x_num, move_x_den = parse_ratio(
        mouseemu_cfg.get("move_x", "1/1"), layout_path, "move_x"
    )
    move_y_num, move_y_den = parse_ratio(
        mouseemu_cfg.get("move_y", "1/1"), layout_path, "move_y"
    )
    scroll_num, scroll_den = parse_ratio(
        mouseemu_cfg.get("scroll", "1/1"), layout_path, "scroll"
    )

    move_keys = parse_mouseemu_indices(
        sections["move_keys"], layout_path, "move_keys", 8
    )
    scroll_keys = parse_mouseemu_indices(
        sections["scroll_keys"], layout_path, "scroll_keys", 2
    )
    button_keys = parse_mouseemu_indices(
        sections["button_keys"], layout_path, "button_keys", 3
    )
    move_keys_deadzones = parse_mouseemu_indices(
        sections["move_keys_deadzones"], layout_path, "move_keys_deadzones", 8
    )
    scroll_keys_deadzones = parse_mouseemu_indices(
        sections["scroll_keys_deadzones"],
        layout_path,
        "scroll_keys_deadzones",
        2,
    )

    while len(move_keys) < 8:
        move_keys.append(0)
    while len(scroll_keys) < 2:
        scroll_keys.append(0)
    while len(button_keys) < 3:
        button_keys.append(0)
    while len(move_keys_deadzones) < 8:
        move_keys_deadzones.append(0)
    while len(scroll_keys_deadzones) < 2:
        scroll_keys_deadzones.append(0)

    header = f"""#ifndef GENERATED_KB_HANDLER_LAYOUT_H
#define GENERATED_KB_HANDLER_LAYOUT_H

#include <subsys/kb_settings.h>
#include <zephyr/sys/util.h>
#include <stdint.h>

#define GENERATED_KB_HANDLER_KEY_COUNT {key_count}U

extern const uint16_t
    generated_kb_handler_default_thresholds[GENERATED_KB_HANDLER_KEY_COUNT];
extern const uint16_t
    generated_kb_handler_default_minimums[GENERATED_KB_HANDLER_KEY_COUNT];
extern const uint16_t
    generated_kb_handler_default_maximums[GENERATED_KB_HANDLER_KEY_COUNT];
extern const uint8_t
    generated_kb_handler_default_keymap_layer1[GENERATED_KB_HANDLER_KEY_COUNT];
extern const uint8_t
    generated_kb_handler_default_keymap_layer2[GENERATED_KB_HANDLER_KEY_COUNT];
extern const uint8_t
    generated_kb_handler_default_keymap_layer3[GENERATED_KB_HANDLER_KEY_COUNT];
extern const kb_mouseemu_settings_t generated_kb_handler_default_mouseemu;
extern const kb_fn_shortcut_t generated_kb_handler_default_fn_shortcuts[{max(1, len(fn_shortcuts))}U];
extern const uint16_t generated_kb_handler_default_fn_shortcuts_count;

#endif // GENERATED_KB_HANDLER_LAYOUT_H
"""

    source = f"""#include "generated_kb_handler_layout.h"

#include <dt-bindings/kb-handler/kb-key-codes.h>

const uint16_t generated_kb_handler_default_thresholds[GENERATED_KB_HANDLER_KEY_COUNT] = {{
{format_c_array([str(value) for value in thresholds])}
}};

const uint16_t generated_kb_handler_default_minimums[GENERATED_KB_HANDLER_KEY_COUNT] = {{
{format_c_array([str(value) for value in minimums])}
}};

const uint16_t generated_kb_handler_default_maximums[GENERATED_KB_HANDLER_KEY_COUNT] = {{
{format_c_array([str(value) for value in maximums])}
}};

const uint8_t generated_kb_handler_default_keymap_layer1[GENERATED_KB_HANDLER_KEY_COUNT] = {{
{format_c_array(layer1)}
}};

const uint8_t generated_kb_handler_default_keymap_layer2[GENERATED_KB_HANDLER_KEY_COUNT] = {{
{format_c_array(layer2)}
}};

const uint8_t generated_kb_handler_default_keymap_layer3[GENERATED_KB_HANDLER_KEY_COUNT] = {{
{format_c_array(layer3)}
}};

const kb_mouseemu_settings_t generated_kb_handler_default_mouseemu = {{
    .enabled = {mouseemu_enabled},
    .direction_mode = {mouseemu_direction},
    .move_keys_count = {len(sections["move_keys"])}U,
    .move_keys = {{{", ".join(str(value) for value in move_keys)}}},
    .scroll_keys_count = {len(sections["scroll_keys"])}U,
    .scroll_keys = {{{", ".join(str(value) for value in scroll_keys)}}},
    .button_keys_count = {len(sections["button_keys"])}U,
    .button_keys = {{{", ".join(str(value) for value in button_keys)}}},
    .move_x_k = (double){move_x_num} / (double){move_x_den},
    .move_y_k = (double){move_y_num} / (double){move_y_den},
    .scroll_k = (double){scroll_num} / (double){scroll_den},
    .move_keys_deadzones = {{{", ".join(str(value) for value in move_keys_deadzones)}}},
    .scroll_keys_deadzones = {{{", ".join(str(value) for value in scroll_keys_deadzones)}}},
}};

BUILD_ASSERT({len(fn_shortcuts)}U <= CONFIG_KB_SETTINGS_FN_SHORTCUTS_MAX,
             "fn shortcut default count exceeds CONFIG_KB_SETTINGS_FN_SHORTCUTS_MAX");

const kb_fn_shortcut_t generated_kb_handler_default_fn_shortcuts[{max(1, len(fn_shortcuts))}U] = {{
{format_c_array([f"{{.key = {key}, .action = {action}, .param = {param}}}" for key, action, param in fn_shortcuts], wrap=2)}
}};

const uint16_t generated_kb_handler_default_fn_shortcuts_count = {len(fn_shortcuts)}U;
"""

    out_h.write_text(header)
    out_c.write_text(source)


if __name__ == "__main__":
    main()
