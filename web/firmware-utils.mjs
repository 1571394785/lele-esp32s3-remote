export function validateManifest(manifest, baseUrl) {
  if (manifest.chipFamily !== "ESP32-S3" || !Array.isArray(manifest.parts) || manifest.parts.length !== 3) {
    throw new Error("本站固件信息不完整，请刷新页面后重试。");
  }
  const limits = new Map([[0, 0x8000], [0x8000, 0x1000], [0x10000, 0x300000]]);
  const seen = new Set();
  for (const part of manifest.parts) {
    const url = new URL(part.path, baseUrl);
    if (!limits.has(part.offset) || seen.has(part.offset) || !Number.isInteger(part.size) ||
        part.size <= 0 || part.size > limits.get(part.offset) || !/^[a-f0-9]{64}$/.test(part.sha256) ||
        url.origin !== new URL(baseUrl).origin || !url.pathname.startsWith(new URL("./", baseUrl).pathname)) {
      throw new Error("固件分区或校验信息不正确，已停止操作。");
    }
    seen.add(part.offset);
  }
  return manifest;
}

export function checkApplicationImage(bytes) {
  if (!(bytes instanceof Uint8Array) || bytes.length < 24 || bytes.length > 0x300000 ||
      bytes[0] !== 0xe9 || bytes[12] !== 9 || bytes[13] !== 0) {
    throw new Error("请选择 ESP32-S3 的应用固件 .bin，文件不能超过 3 MB。");
  }
  return bytes;
}

export async function checkedBytes(bytes, expectedSize, expectedHash, cryptoApi = globalThis.crypto) {
  if (bytes.length !== expectedSize) throw new Error("固件下载不完整，请重试。");
  const digest = await cryptoApi.subtle.digest("SHA-256", bytes);
  const actual = [...new Uint8Array(digest)].map(value => value.toString(16).padStart(2, "0")).join("");
  if (actual !== expectedHash) throw new Error("固件校验失败，已停止烧录。请刷新后重试。");
  return bytes;
}

export function progressPercent(sizes, fileIndex, written, total) {
  if (!sizes.length || fileIndex < 0 || fileIndex >= sizes.length || total <= 0) return 0;
  const done = sizes.slice(0, fileIndex).reduce((sum, size) => sum + size, 0);
  const current = sizes[fileIndex] * Math.min(1, Math.max(0, written / total));
  return Math.min(98, Math.floor(98 * (done + current) / sizes.reduce((sum, size) => sum + size, 0)));
}
