"""Verify fresh-build packaging and reject unsafe or incomplete release assets."""
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def load(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / "tools" / (name + ".py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


packager = load("package_release")
publisher = load("publish_release")
web_builder = load("build_web")


class ReleaseTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="remote_release_test_")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        for name, value in {"LICENSE": b"0BSD", "THIRD_PARTY_NOTICES.md": b"notices",
                            "main.c": b"source", "firmware/caiji.bin": b"OLD ARCHIVE",
                            "data/1/raw.csv": b"private recordings", ".env": b"private"}.items():
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(value)
        (self.root / "licenses").mkdir()
        subprocess.run(["git", "init", "-q", str(self.root)], check=True)
        subprocess.run(["git", "add", "--all"], cwd=self.root, check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.build = self.root / "build"
        self.build.mkdir()
        self.args = {"app": {"file": "caiji.bin"}, "flash_files": {
            "0x0": "bootloader/bootloader.bin", "0x8000": "partition_table/partition-table.bin",
            "0x10000": "caiji.bin"}}
        for name in self.args["flash_files"].values():
            path = self.build / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"NEW BUILD " + name.encode())
        (self.build / "flasher_args.json").write_text(json.dumps(self.args))
        (self.build / "project_description.json").write_text('{"target":"esp32s3"}')
        (self.build / "flash_args").write_text("0x10000 caiji.bin\n")
        self.version = "build-1-1-aaaaaaa"
        self.commit = "a" * 40
        self.dist = self.root / "dist"

    def package(self):
        return packager.package(self.root, self.build, self.dist, self.version, self.commit)

    def test_new_build_not_archived_binary_and_valid_manifests(self):
        self.package()
        assets = publisher.verify_assets(self.dist, self.version, self.commit)
        self.assertEqual(len(assets), 4)
        for archive in self.dist.glob("*.zip"):
            with zipfile.ZipFile(archive) as package:
                prefix = f"lele-esp32s3-remote-{self.version}/"
                binary = "firmware/caiji.bin" if "source-and-firmware" in archive.name else "caiji.bin"
                self.assertEqual(package.read(prefix + binary), b"NEW BUILD caiji.bin")
                self.assertFalse(any("raw.csv" in name or name.endswith("/.env") for name in package.namelist()))
                for line in package.read(prefix + "SHA256SUMS").decode().splitlines():
                    digest, name = line.split("  ", 1)
                    self.assertEqual(packager.sha256(package.read(prefix + name)), digest)

    def test_corrupt_asset_cannot_be_published(self):
        self.package()
        archive = next(self.dist.glob("*.zip"))
        archive.write_bytes(archive.read_bytes() + b"tampered")
        with self.assertRaisesRegex(ValueError, "checksum mismatch"):
            publisher.verify_assets(self.dist, self.version, self.commit)

    def test_wrong_commit_cannot_be_published(self):
        self.package()
        with self.assertRaisesRegex(ValueError, "does not match"):
            publisher.verify_assets(self.dist, self.version, "b" * 40)

    def test_extra_flash_partition_is_rejected(self):
        self.args["flash_files"]["0x9000"] = "nvs.bin"
        (self.build / "flasher_args.json").write_text(json.dumps(self.args))
        with self.assertRaisesRegex(ValueError, "Unexpected flash layout"):
            self.package()

    def test_build_path_escape_is_rejected(self):
        self.args["app"]["file"] = "../LICENSE"
        self.args["flash_files"]["0x10000"] = "../LICENSE"
        (self.build / "flasher_args.json").write_text(json.dumps(self.args))
        with self.assertRaisesRegex(ValueError, "escapes build directory"):
            self.package()

    def test_web_manifest_uses_verified_build_and_preserves_nvs_region(self):
        self.package()
        (self.root / "web").mkdir()
        (self.root / "web/index.html").write_text("web flasher")
        output = self.root / "site"
        manifest = web_builder.build_site(self.root, self.build, output, self.dist / "BUILD_INFO.json")
        self.assertEqual({part["offset"] for part in manifest["parts"]}, {0, 0x8000, 0x10000})
        self.assertEqual(manifest["commit"], self.commit)
        self.assertEqual((output / "firmware/caiji.bin").read_bytes(), b"NEW BUILD caiji.bin")
        self.assertEqual((output / "index.html").read_text(), "web flasher")
        for part in manifest["parts"]:
            data = (output / "firmware" / part["path"]).read_bytes()
            self.assertEqual(len(data), part["size"])
            self.assertEqual(packager.sha256(data), part["sha256"])

    def test_web_refuses_firmware_changed_after_release_packaging(self):
        self.package()
        (self.build / "caiji.bin").write_bytes(b"different build")
        with self.assertRaisesRegex(ValueError, "does not match"):
            web_builder.build_site(self.root, self.build, self.root / "site", self.dist / "BUILD_INFO.json")


if __name__ == "__main__":
    unittest.main()
