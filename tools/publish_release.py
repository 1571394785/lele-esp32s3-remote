"""Publish verified CI packages, exposing the release only after every upload."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import urllib.error
import urllib.parse
import urllib.request


def verify_assets(directory, version, commit):
    info = json.loads((directory / "BUILD_INFO.json").read_text())
    if info["version"] != version or info["commit"] != commit:
        raise ValueError("Build metadata does not match this workflow commit/version")
    expected = {"BUILD_INFO.json", f"lele-esp32s3-remote-{version}-firmware.zip",
                f"lele-esp32s3-remote-{version}-source-and-firmware.zip"}
    listed = set()
    for line in (directory / "SHA256SUMS").read_text().splitlines():
        digest, name = line.split("  ", 1)
        if name not in expected or name in listed:
            raise ValueError("Unexpected or duplicate release asset")
        if hashlib.sha256((directory / name).read_bytes()).hexdigest() != digest:
            raise ValueError(f"Asset checksum mismatch: {name}")
        listed.add(name)
    if listed != expected:
        raise ValueError("Missing release asset")
    return [directory / name for name in sorted(expected | {"SHA256SUMS"})]


def publish(directory, version):
    repository = os.environ["GITHUB_REPOSITORY"]
    commit = os.environ["GITHUB_SHA"]
    if not re.fullmatch(r"[\w.-]+/[\w.-]+", repository) or not re.fullmatch(r"[0-9a-f]{40}", commit):
        raise ValueError("Invalid workflow repository/commit")
    if not re.fullmatch(r"build-[0-9]+-[0-9]+-[0-9a-f]{7}", version):
        raise ValueError("Invalid automatic release version")
    files = verify_assets(directory, version, commit)
    headers = {"Authorization": "Bearer " + os.environ["GITHUB_TOKEN"],
               "Accept": "application/vnd.github+json", "User-Agent": "lele-remote-ci",
               "X-GitHub-Api-Version": "2022-11-28"}

    def request(url, method="GET", payload=None, binary=None):
        data = binary if binary is not None else None if payload is None else json.dumps(payload).encode()
        content_type = "application/octet-stream" if binary is not None else "application/json"
        req = urllib.request.Request(url, data=data, method=method,
                                     headers={**headers, "Content-Type": content_type})
        with urllib.request.urlopen(req, timeout=90) as response:
            body = response.read()
            return json.loads(body) if body else None

    base = "https://api.github.com/repos/" + repository
    release = request(base + "/releases", "POST", {
        "tag_name": version, "target_commitish": commit,
        "name": f"自动编译 {version}", "draft": True, "prerelease": False,
        "body": f"源提交：{commit}\n\nESP32-S3 / ESP-IDF v5.4.1。编译与回归测试已通过，"
                "不代表已完成实机验证。\n\nfirmware.zip 为可直接烧录的固件包；"
                "source-and-firmware.zip 为源码与本次新编译固件整包。"
                "包内附烧录说明、构建信息和 SHA-256 校验；不包含 NVS 或设备配置备份。"})
    for path in files:
        data = path.read_bytes()
        url = release["upload_url"].split("{", 1)[0] + "?name=" + urllib.parse.quote(path.name)
        asset = request(url, "POST", binary=data)
        if asset["state"] != "uploaded" or asset["size"] != len(data):
            raise RuntimeError(f"Incomplete upload: {path.name}")
        if asset.get("digest") and asset["digest"] != "sha256:" + hashlib.sha256(data).hexdigest():
            raise RuntimeError(f"Remote checksum mismatch: {path.name}")
    # An older parallel build must not displace the current main commit as Latest.
    head = request(base + "/commits/main")["sha"]
    published = request(base + "/releases/" + str(release["id"]), "PATCH",
                        {"draft": False, "make_latest": "true" if head == commit else "false"})
    print(published["html_url"])
    if os.environ.get("GITHUB_STEP_SUMMARY"):
        with open(os.environ["GITHUB_STEP_SUMMARY"], "a", encoding="utf-8") as summary:
            summary.write(f"## 自动发行版\n\n[{version}]({published['html_url']})\n\n源提交：`{commit}`\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", required=True)
    parser.add_argument("--dist", type=Path, default=Path("dist"))
    options = parser.parse_args()
    publish(options.dist, options.version)
