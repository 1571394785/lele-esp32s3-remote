"""Check both real report maps: byte sizes and wheel-only multiplier scope."""
from pathlib import Path
import re
import json
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "main/ble_hid.c").read_text(encoding="utf-8")
constant_sources = source + "\n" + (root / "main/ble_hid.h").read_text(encoding="utf-8")
constants = {name: int(value) for name, value in re.findall(
    r"^#define\s+(\w+)\s+(\d+)\s*$", constant_sources, re.M)}
constants["AIR_MOUSE_WHEEL_RESOLUTION"] = 120
macro_lines = []
for line in source.split("#define BLE_HID_PRECISE_WHEEL_DESCRIPTOR", 1)[1].splitlines():
    macro_lines.append(line)
    if not line.rstrip().endswith("\\"):
        break
macro = "\n".join(macro_lines)
maps = []

for name in ("HID_REPORT_MAP_COMPAT", "HID_REPORT_MAP_FULL"):
    body = source.split(f"{name}[] = {{", 1)[1].split("};", 1)[0]
    body = body.replace("BLE_HID_PRECISE_WHEEL_DESCRIPTOR", macro)
    body = re.sub(r"//[^\n]*", "", body)
    tokens = re.findall(r"0x[0-9A-Fa-f]+|\b(?:BLE_HID_\w+|AIR_MOUSE_WHEEL_RESOLUTION)\b", body)
    data = bytes(int(token, 16) if token.startswith("0x") else constants[token] for token in tokens)
    legacy_body = body.replace(macro, "0x09, 0x38, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x01, 0x81, 0x06")
    # body has had comments stripped, while the macro contains no comments.
    legacy_tokens = re.findall(r"0x[0-9A-Fa-f]+|\b(?:BLE_HID_\w+|AIR_MOUSE_WHEEL_RESOLUTION)\b", legacy_body)
    legacy_data = bytes(int(token, 16) if token.startswith("0x") else constants[token] for token in legacy_tokens)
    maps.append((data, legacy_data))
    state = {"page": 0, "id": 0, "size": 0, "count": 0}
    usage = 0
    collections = []
    sizes = {}
    wheel_scope = multiplier_scope = None
    cursor_scopes = []
    physical_min = physical_max = None
    index = 0
    while index < len(data):
        prefix = data[index]
        length = (0, 1, 2, 4)[prefix & 3]
        tag = prefix & 0xFC
        value = int.from_bytes(data[index + 1:index + 1 + length], "little")
        index += 1 + length
        if tag == 0x04: state["page"] = value
        elif tag == 0x84: state["id"] = value
        elif tag == 0x74: state["size"] = value
        elif tag == 0x94: state["count"] = value
        elif tag == 0x34: physical_min = value
        elif tag == 0x44: physical_max = value
        elif tag == 0x08: usage = value
        elif tag == 0xA0:
            collections.append((index, value))
            usage = 0
        elif tag == 0xC0: collections.pop()
        elif tag in (0x80, 0x90, 0xB0):
            key = (state["id"], tag)
            sizes[key] = sizes.get(key, 0) + state["size"] * state["count"]
            scope = next((entry[0] for entry in reversed(collections) if entry[1] == 2), None)
            if state["page"] == 1 and usage == 0x38 and tag == 0x80: wheel_scope = scope
            if state["page"] == 1 and usage in (0x30, 0x31) and state["id"] == 2:
                cursor_scopes.append(scope)
            if state["page"] == 1 and usage == 0x48 and tag == 0xB0:
                multiplier_scope = scope
                assert physical_min == 1 and physical_max == 120
            usage = 0
    assert not collections
    assert sizes[(1, 0x80)] == 64
    assert sizes[(2, 0x80)] == 32
    assert sizes[(3, 0x80)] == 16
    assert sizes[(5, 0xB0)] == 8
    assert wheel_scope is not None and wheel_scope == multiplier_scope
    assert len(cursor_scopes) == 2 and wheel_scope not in cursor_scopes
    print(f"PASS: {name}: original input sizes, one-byte feature, wheel-only 120x scope")

# Exercise the actual ESP-IDF parser and the firmware's compatibility wrapper.
# The IDE configuration points to this project's installed ESP-IDF version.
settings = json.loads((root / ".vscode/settings.json").read_text(encoding="utf-8"))
idf = Path(os.environ.get("IDF_PATH", settings.get("idf.espIdfPathWin", "__idf_not_configured__")))
if not (idf / "components/esp_hid/src/esp_hid_common.c").exists():
    print("SKIP: ESP-IDF parser unavailable on this host")
else:
    with tempfile.TemporaryDirectory(prefix="hid_wheel_test_") as directory:
        temporary = Path(directory)
        (temporary / "esp_err.h").write_text("#pragma once\ntypedef int esp_err_t;\n")
        (temporary / "esp_log.h").write_text(
            "#pragma once\n#include <stdlib.h>\n"
            "#define ESP_LOGE(tag,...) ((void)(tag))\n"
            "#define ESP_LOGW(tag,...) ((void)(tag))\n"
            "#define ESP_LOGI(tag,...) ((void)(tag))\n"
            "#define ESP_LOGD(tag,...) ((void)(tag))\n")
        harness = "#include <assert.h>\n#include <stdlib.h>\n#include <stdio.h>\n#include <string.h>\n#include <esp_hid_common.h>\n"
        harness += "esp_hid_report_map_t *__real_esp_hid_parse_report_map(const uint8_t *,size_t);\n"
        for index, (current, legacy) in enumerate(maps):
            for label, array in (("current", current), ("legacy", legacy)):
                harness += f"static const uint8_t {label}_{index}[]={{" + ",".join(map(str, array)) + "};\n"
        harness += "int main(void) {\n"
        for index in range(len(maps)):
            harness += f"""{{
                esp_hid_report_map_t *old = __real_esp_hid_parse_report_map(legacy_{index},sizeof(legacy_{index}));
                esp_hid_report_map_t *updated = esp_hid_parse_report_map(current_{index},sizeof(current_{index}));
                assert(old && updated && updated->reports_len == old->reports_len + 1);
                assert(!memcmp(old->reports,updated->reports,old->reports_len * sizeof(*old->reports)));
                esp_hid_report_item_t *feature = &updated->reports[updated->reports_len - 1];
                assert(feature->report_id == 5 && feature->report_type == ESP_HID_REPORT_TYPE_FEATURE && feature->value_len == 1);
                free(old->reports); free(old); free(updated->reports); free(updated);
            }}\n"""
        harness += 'puts("PASS: actual ESP-IDF parser preserves every old GATT report before appending the new feature"); return 0;}\n'
        (temporary / "test.c").write_text(harness)
        executable = temporary / "test.exe"
        subprocess.run(["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
            "-I", str(temporary), "-I", str(root / "main"),
            "-I", str(idf / "components/esp_hid/include"),
            str(temporary / "test.c"), str(root / "main/hid_report_compat.c"),
            str(idf / "components/esp_hid/src/esp_hid_common.c"),
            "-Wl,--wrap=esp_hid_parse_report_map", "-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True)
