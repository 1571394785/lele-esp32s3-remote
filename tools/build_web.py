"""Build a self-contained Pages site with firmware from the successful CI build."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil


def build_site(root, build, output, info_path):
    info = json.loads(info_path.read_text())
    if info["target"] != "esp32s3":
        raise ValueError("Web installer only supports this ESP32-S3 project")
    args = json.loads((build / "flasher_args.json").read_text())
    expected = {"0x0": "bootloader/bootloader.bin", "0x8000": "partition_table/partition-table.bin",
                "0x10000": args["app"]["file"]}
    if args["flash_files"] != expected:
        raise ValueError("Unexpected flash layout; refusing to publish extra partitions")
    if output.resolve() in (root.resolve(), (root / "web").resolve()):
        raise ValueError("Output must not overwrite source")
    parts = []
    binaries = []
    for address, relative in expected.items():
        path = (build / relative).resolve()
        if not path.is_relative_to(build.resolve()):
            raise ValueError("Build file escapes its directory")
        data = path.read_bytes()
        digest = hashlib.sha256(data).hexdigest()
        if digest != info["firmware_sha256"][relative]:
            raise ValueError("Site firmware does not match the verified release build")
        parts.append({"name": Path(relative).name, "path": relative, "offset": int(address, 16),
                      "size": len(data), "sha256": digest})
        binaries.append((relative, data))
    shutil.copytree(root / "web", output, dirs_exist_ok=True)
    (output / ".nojekyll").touch()
    for relative, data in binaries:
        target = output / "firmware" / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
    manifest = {"name": "乐乐牌 ESP32-S3 遥控器", "chipFamily": "ESP32-S3",
                "version": info["version"], "commit": info["commit"],
                "builtAt": info["built_at_utc"], "parts": parts}
    (output / "firmware/manifest.json").write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return manifest


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path.cwd())
    parser.add_argument("--build-dir", type=Path, default=Path("build"))
    parser.add_argument("--output-dir", type=Path, default=Path("site"))
    parser.add_argument("--build-info", type=Path, default=Path("dist/BUILD_INFO.json"))
    options = parser.parse_args()
    print(json.dumps(build_site(options.root, options.build_dir, options.output_dir, options.build_info), indent=2))
