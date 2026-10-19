"""Run the firmware's actual coordinate transform on the host; clean all build files."""
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="air_mouse_test_") as directory:
    temporary = Path(directory)
    (temporary / "esp_err.h").write_text("#pragma once\ntypedef int esp_err_t;\n")
    executable = temporary / "air_mouse_test.exe"
    subprocess.run([
        "gcc", "-std=c11", "-Wall", "-Wextra", "-Werror",
        "-I", str(temporary), "-I", str(root / "main"),
        str(root / "tests/test_air_mouse_frame.c"),
        str(root / "main/air_mouse_frame.c"), "-lm", "-o", str(executable),
    ], check=True)
    subprocess.run([str(executable)], check=True)
    subprocess.run([
        "gcc", "-std=c11", "-Wall", "-Wextra", "-Werror",
        "-I", str(root / "main"),
        str(root / "tests/test_air_mouse_scroll.c"),
        str(root / "main/air_mouse_scroll.c"), "-lm", "-o", str(executable),
    ], check=True)
    subprocess.run([str(executable)], check=True)

subprocess.run([sys.executable, str(root / "tests/test_hid_wheel_descriptor.py")], check=True)
subprocess.run([sys.executable, str(root / "tests/test_hid_peer_state.py")], check=True)

subprocess.run([sys.executable, str(root / "tests/test_ble_device_slots.py")], check=True)

if sys.platform == "win32":
    subprocess.run([sys.executable, str(root / "tests/test_ble_peer_identity.py")], check=True)

subprocess.run([sys.executable, str(root / "tests/test_device_names.py")], check=True)

subprocess.run([sys.executable, str(root / "tests/test_oled_pixel_scroll.py")], check=True)

subprocess.run([sys.executable, str(root / "tests/test_buttons_scanner.py")], check=True)

subprocess.run([sys.executable, str(root / "tests/test_bmi220_startup_wait.py")], check=True)
