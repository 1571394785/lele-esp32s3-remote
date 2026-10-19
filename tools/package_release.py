"""Package tracked source together with THIS build, never the archived binaries."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import subprocess
import zipfile


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def checksums(files):
    return "".join(f"{sha256(data)}  {name}\n" for name, data in sorted(files.items())).encode()


def safe_file(directory, name):
    path = (directory / name).resolve()
    if not path.is_relative_to(directory.resolve()):
        raise ValueError(f"File escapes build directory: {name}")
    return path.read_bytes()


def package(root, build, output, version, commit):
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]{0,99}", version):
        raise ValueError("Invalid release version")
    if not re.fullmatch(r"[0-9a-f]{40}", commit):
        raise ValueError("Expected a full Git commit SHA")
    description = json.loads((build / "project_description.json").read_text())
    if description["target"] != "esp32s3":
        raise ValueError("Expected an ESP32-S3 build")
    args = json.loads((build / "flasher_args.json").read_text())
    app = args["app"]["file"]
    expected = {"0x0": "bootloader/bootloader.bin",
                "0x8000": "partition_table/partition-table.bin", "0x10000": app}
    if args["flash_files"] != expected:
        raise ValueError("Unexpected flash layout; refusing to package additional partitions")
    firmware = {name: safe_file(build, name) for name in expected.values()}
    firmware["flash_args"] = safe_file(build, "flash_args")
    firmware["flasher_args.json"] = safe_file(build, "flasher_args.json")
    info = {"version": version, "commit": commit, "target": "esp32s3",
            "esp_idf": "v5.4.1", "built_at_utc": datetime.now(timezone.utc).isoformat(),
            "firmware_sha256": {name: sha256(data) for name, data in firmware.items()}}
    info_bytes = (json.dumps(info, ensure_ascii=False, indent=2) + "\n").encode()
    firmware["BUILD_INFO.json"] = info_bytes
    firmware["README.md"] = f"""# 自动编译固件 {version}

源提交：{commit}。目标 ESP32-S3，ESP-IDF v5.4.1，8 MB Flash 布局。
此目录中的二进制由本次 CI 编译生成，不是仓库初始固件快照。
自动编译与测试不代表已完成实机体验验证。

安装 Python 和 esptool 4.8.1，在本目录运行（COM3 改成实际串口）：

```powershell
python -m pip install esptool==4.8.1
python -m esptool --chip esp32s3 --port COM3 --baud 460800 --before default_reset --after hard_reset write_flash '@flash_args'
```

不执行整片擦除。包内不含 NVS、蓝牙密钥或 Wi-Fi 配置备份。
已有兼容分区布局下的设备配置会保留。第三方许可见 licenses/ 和 THIRD_PARTY_NOTICES.md。
""".encode()
    for name in ("LICENSE", "THIRD_PARTY_NOTICES.md"):
        firmware[name] = (root / name).read_bytes()
    for path in sorted((root / "licenses").rglob("*")):
        if path.is_file():
            firmware[path.relative_to(root).as_posix()] = path.read_bytes()
    firmware["SHA256SUMS"] = checksums(firmware)

    tracked = subprocess.check_output(
        ["git", "-c", f"safe.directory={root.resolve()}", "ls-files", "-z"], cwd=root
    ).decode().split("\0")
    source = {}
    for name in tracked:
        if not name or name == "SHA256SUMS" or name.startswith(("firmware/", "data/")):
            continue
        if name.endswith((".log", ".elf", ".map", ".pyc")) or name.startswith((".env", "build/", ".venv/")):
            continue
        source[name] = (root / name).read_bytes()
    if (root / "data/README.md").exists():
        source["data/README.md"] = (root / "data/README.md").read_bytes()
    source.update({f"firmware/{name}": data for name, data in firmware.items()})
    source["BUILD_INFO.json"] = info_bytes
    source["SHA256SUMS"] = checksums(source)

    output.mkdir(parents=True, exist_ok=True)
    assets = {}
    for kind, files in (("firmware", firmware), ("source-and-firmware", source)):
        filename = f"lele-esp32s3-remote-{version}-{kind}.zip"
        with zipfile.ZipFile(output / filename, "w", zipfile.ZIP_DEFLATED) as archive:
            for name, data in sorted(files.items()):
                archive.writestr(f"lele-esp32s3-remote-{version}/{name}", data)
        assets[filename] = (output / filename).read_bytes()
    assets["BUILD_INFO.json"] = info_bytes
    (output / "BUILD_INFO.json").write_bytes(info_bytes)
    (output / "SHA256SUMS").write_bytes(checksums(assets))
    return info


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", required=True)
    parser.add_argument("--commit", required=True)
    parser.add_argument("--root", type=Path, default=Path.cwd())
    parser.add_argument("--build-dir", type=Path, default=Path("build"))
    parser.add_argument("--output-dir", type=Path, default=Path("dist"))
    options = parser.parse_args()
    print(json.dumps(package(options.root, options.build_dir, options.output_dir,
                             options.version, options.commit), indent=2))
