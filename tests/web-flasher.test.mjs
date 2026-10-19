import test from "node:test";
import assert from "node:assert/strict";
import { webcrypto } from "node:crypto";
import { readFile } from "node:fs/promises";
import vm from "node:vm";
import { validateManifest, checkedBytes, checkApplicationImage, progressPercent } from "../web/firmware-utils.mjs";

const base = new URL("https://example.com/remote/firmware/manifest.json");
function manifest() {
  return { chipFamily: "ESP32-S3", parts: [0, 0x8000, 0x10000].map((offset, i) => ({
    offset, path: ["bootloader/bootloader.bin", "partition_table/partition-table.bin", "caiji.bin"][i],
    size: 100, sha256: "a".repeat(64) })) };
}

test("only the three expected ESP32-S3 regions can be written", () => {
  assert.equal(validateManifest(manifest(), base).parts.length, 3);
  for (const offset of [0x9000, 0xf000, 0x400000]) {
    const value = manifest(); value.parts[1].offset = offset;
    assert.throws(() => validateManifest(value, base));
  }
  const duplicate = manifest(); duplicate.parts[1].offset = 0;
  assert.throws(() => validateManifest(duplicate, base));
  const overlap = manifest(); overlap.parts[1].size = 4097;
  assert.throws(() => validateManifest(overlap, base));
});

test("external and parent-directory firmware URLs are refused", () => {
  for (const path of ["https://evil.example/firmware.bin", "../../other.bin", "//evil.example/app.bin"]) {
    const value = manifest(); value.parts[0].path = path;
    assert.throws(() => validateManifest(value, base));
  }
});

test("SHA-256 and length must match before any flash call", async () => {
  const bytes = new TextEncoder().encode("abc");
  const hash = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
  assert.equal(await checkedBytes(bytes, 3, hash, webcrypto), bytes);
  await assert.rejects(checkedBytes(bytes, 4, hash, webcrypto));
  await assert.rejects(checkedBytes(bytes, 3, "0".repeat(64), webcrypto));
});

test("local application must identify ESP32-S3 and fit its partition", () => {
  const app = new Uint8Array(24); app[0] = 0xe9; app[12] = 9;
  assert.equal(checkApplicationImage(app), app);
  app[12] = 0; assert.throws(() => checkApplicationImage(app));
  assert.throws(() => checkApplicationImage(new Uint8Array(0x300001)));
  assert.throws(() => checkApplicationImage(new Uint8Array(23)));
});

test("progress accounts for all parts and never claims verification is finished", () => {
  assert.equal(progressPercent([100, 100, 800], 2, 500, 1000), 58);
  assert.equal(progressPercent([100, 100, 800], 2, 1000, 1000), 98);
  assert.equal(progressPercent([100], 0, -10, 100), 0);
  assert.equal(progressPercent([100], 0, 1000, 100), 98);
  assert.equal(progressPercent([], 0, 0, 0), 0);
});

async function appFixture(options = {}) {
  const elements = new Map();
  const element = id => {
    if (!elements.has(id)) elements.set(id, { textContent: "", value: id === "baud" ? "460800" : 0,
      disabled: true, hidden: false, files: [], scrollTop: 0, clientHeight: 100, scrollHeight: 100,
      classList: { toggle() {} }, events: {}, addEventListener(name, callback) { this.events[name] = callback; } });
    return elements.get(id);
  };
  const firmware = new Uint8Array([1, 2, 3]);
  const digest = [...new Uint8Array(await webcrypto.subtle.digest("SHA-256", firmware))]
    .map(byte => byte.toString(16).padStart(2, "0")).join("");
  const value = manifest(); value.version = "build-1-1-aaaaaaa"; value.commit = "a".repeat(40);
  value.builtAt = "2026-10-19T00:00:00Z";
  value.parts.forEach(part => { part.size = firmware.length; part.sha256 = options.badHash ? "0".repeat(64) : digest; });
  const calls = { writes: [], resets: 0, closes: 0, requests: 0 };
  let pendingRead, emitted = false;
  const reader = {
    read() {
      if (!emitted) { emitted = true; return Promise.resolve({ value: new TextEncoder().encode("<script>not executable</script>\n"), done: false }); }
      return new Promise(resolve => { pendingRead = resolve; });
    },
    async cancel() { pendingRead?.({ done: true }); }, releaseLock() {},
  };
  const port = { readable: { getReader: () => reader }, writable: {}, async open() {}, async close() { calls.closes++; } };
  const context = vm.createContext({
    document: { getElementById: element },
    navigator: options.unsupported ? {} : { serial: {
      async requestPort() {
        calls.requests++;
        if (options.cancel) throw new DOMException("cancelled", "NotFoundError");
        return port;
      }, addEventListener() {},
    } },
    isSecureContext: true, URL, Uint8Array, ArrayBuffer, TextDecoder, TextEncoder, Blob,
    queueMicrotask, setTimeout,
    Transport: class { async disconnect() {} },
    ESPLoader: class {
      async main() { return options.wrongChip ? "ESP32" : "ESP32-S3"; }
      async after() { calls.resets++; if (options.resetFails) throw Error("reset unavailable"); }
      async writeFlash(settings) {
        calls.writes.push(settings);
        settings.reportProgress(2, 10, 10);
        assert.equal(element("progress").value, 98);
        if (options.writeFails) throw Error("MD5 verification failed");
        assert.equal(settings.calculateMD5Hash(new Uint8Array([97, 98, 99])), "900150983cd24fb0d6963f7d28e17f72");
      }
    },
    validateManifest, checkApplicationImage, progressPercent,
    checkedBytes: (bytes, size, hash) => checkedBytes(bytes, size, hash, webcrypto),
    async fetch(url) {
      if (String(url).endsWith("manifest.json")) return { ok: true, json: async () => value };
      return { ok: true, arrayBuffer: async () => firmware.buffer };
    },
  });
  context.window = context;
  vm.runInContext(await readFile(new URL("../web/vendor/spark-md5-3.0.2.min.js", import.meta.url), "utf8"), context);
  const source = (await readFile(new URL("../web/app.mjs", import.meta.url), "utf8"))
    .replace(/^import .*;\r?$/gm, "")
    .replaceAll("import.meta.url", JSON.stringify(base.href));
  await vm.runInContext("(async () => {" + source + "\n})()", context);
  return { element, calls, click: id => element(id).events.click() };
}

test("flashing stays disabled until connected and writes only verified parts", async () => {
  const app = await appFixture();
  assert.equal(app.element("install").disabled, true);
  await app.click("connect");
  assert.equal(app.element("install").disabled, false);
  await app.click("install");
  assert.equal(app.calls.writes.length, 1);
  assert.deepEqual(Array.from(app.calls.writes[0].fileArray, file => file.address), [0, 0x8000, 0x10000]);
  assert.equal(app.calls.writes[0].eraseAll, false);
  assert.equal(app.element("progress").value, 100);
  assert.equal(app.element("connect").disabled, false);
  assert.match(app.element("status").textContent, /烧录成功/);
});

test("cancelled connection and wrong chip leave the UI usable without writing", async () => {
  for (const option of [{ cancel: true }, { wrongChip: true }]) {
    const app = await appFixture(option);
    await app.click("connect");
    assert.equal(app.calls.writes.length, 0);
    assert.equal(app.element("connect").disabled, false);
    assert.equal(app.element("install").disabled, true);
  }
});

test("download corruption prevents flashing, and MD5 failure never shows success", async () => {
  for (const options of [{ badHash: true }, { writeFails: true }]) {
    const app = await appFixture(options); await app.click("connect"); await app.click("install");
    assert.equal(app.calls.writes.length, options.badHash ? 0 : 1);
    assert.notEqual(app.element("progress").value, 100);
    assert.doesNotMatch(app.element("status").textContent, /烧录成功/);
    assert.equal(app.element("connect").disabled, false);
  }
});

test("a failed reset does not falsely claim the device has rebooted", async () => {
  const app = await appFixture({ resetFails: true });
  await app.click("connect"); await app.click("install");
  assert.equal(app.element("progress").value, 100);
  assert.match(app.element("status").textContent, /请短按 RESET/);
});

test("unsupported browsers cannot connect or write", async () => {
  const app = await appFixture({ unsupported: true });
  assert.equal(app.element("browser-warning").hidden, false);
  assert.equal(app.element("connect").disabled, true);
  assert.equal(app.element("install").disabled, true);
});

test("log mode renders serial data as text and releases the reader on disconnect", async () => {
  const app = await appFixture();
  await app.click("read-logs");
  await new Promise(resolve => setImmediate(resolve));
  assert.match(app.element("log").textContent, /<script>not executable<\/script>/);
  assert.equal(app.element("install").disabled, true);
  assert.equal(app.calls.writes.length, 0);
  await app.click("disconnect");
  assert.equal(app.element("connect").disabled, false);
  assert.equal(app.calls.closes, 1);
});
