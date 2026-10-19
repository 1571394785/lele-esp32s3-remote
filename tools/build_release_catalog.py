"""Mirror every published firmware release into the same-origin web installer."""
import argparse
import hashlib
import io
import json
from pathlib import Path
import re
import urllib.request
import zipfile

API = "https://api.github.com/repos/1571394785/lele-esp32s3-remote/releases"


def download(url):
    request = urllib.request.Request(url, headers={"User-Agent": "lele-web-installer"})
    with urllib.request.urlopen(request, timeout=90) as response:
        return response.read()


def import_release(site, release, archive_bytes):
    tag = release["tag_name"]
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]{0,99}", tag):
        raise ValueError("Unsupported release tag")
    with zipfile.ZipFile(io.BytesIO(archive_bytes)) as archive:
        candidates = [name for name in archive.namelist() if name.endswith("/flasher_args.json")]
        if len(candidates) != 1:
            raise ValueError("Expected exactly one flash layout")
        prefix = candidates[0].removesuffix("flasher_args.json")
        args = json.loads(archive.read(candidates[0]))
        expected = {"0x0": "bootloader/bootloader.bin", "0x8000": "partition_table/partition-table.bin",
                    "0x10000": "caiji.bin"}
        if args["flash_files"] != expected:
            raise ValueError("Unexpected release flash layout")
        checksum_name = prefix + "SHA256SUMS"
        if checksum_name not in archive.namelist():
            checksum_name = prefix.removesuffix("firmware/") + "SHA256SUMS"
        sums = dict(line.split("  ", 1)[::-1] for line in archive.read(checksum_name).decode().splitlines() if line.strip())
        checksum_prefix = checksum_name.removesuffix("SHA256SUMS")
        parts, binaries = [], []
        for offset, relative in expected.items():
            name = prefix + relative
            data = archive.read(name)
            digest = hashlib.sha256(data).hexdigest()
            if sums.get(name.removeprefix(checksum_prefix)) != digest:
                raise ValueError("Release firmware checksum mismatch: " + relative)
            limit = {"0x0": 0x8000, "0x8000": 0x1000, "0x10000": 0x300000}[offset]
            if not 0 < len(data) <= limit:
                raise ValueError("Release firmware exceeds partition")
            binaries.append((relative, data))
            parts.append({"name": Path(relative).name, "path": relative, "offset": int(offset, 16),
                          "size": len(data), "sha256": digest})
        info_name = prefix + "BUILD_INFO.json"
        info = json.loads(archive.read(info_name)) if info_name in archive.namelist() else {}
    directory = site / "firmware/releases" / tag
    for relative, data in binaries:
        target = directory / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
    manifest = {"name": "乐乐牌 ESP32-S3 遥控器", "chipFamily": "ESP32-S3", "version": tag,
                "commit": info.get("commit", ""), "builtAt": info.get("built_at_utc", release["published_at"]),
                "parts": parts}
    (directory / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return {"version": tag, "name": release.get("name") or tag, "prerelease": release.get("prerelease", False),
            "manifest": "releases/" + tag + "/manifest.json"}


def build_catalog(site, get=download):
    current = json.loads((site / "firmware/manifest.json").read_text(encoding="utf-8"))
    entries = [{"version": current["version"], "name": current["version"], "manifest": "manifest.json"}]
    page = 1
    while True:
        releases = json.loads(get(API + f"?per_page=100&page={page}"))
        for release in releases:
            if release["draft"] or release["tag_name"] == current["version"]:
                continue
            tag = release["tag_name"]
            assets = release["assets"]
            asset = next((a for a in assets if a["name"].endswith("-firmware.zip")), None)
            if asset is None:
                asset = next((a for a in assets if a["name"] == f"lele-esp32s3-remote-{tag}.zip"), None)
            if asset is None:
                raise ValueError("Published release has no firmware package: " + tag)
            entries.append(import_release(site, release, get(asset["browser_download_url"])))
        if len(releases) < 100:
            break
        page += 1
    (site / "firmware/releases.json").write_text(json.dumps(entries, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return entries


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--site", type=Path, default=Path("site"))
    print(json.dumps(build_catalog(parser.parse_args().site), ensure_ascii=False, indent=2))
