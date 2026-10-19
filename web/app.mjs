import { ESPLoader, Transport } from "./vendor/esptool-js-0.7.0.js";
import { validateManifest, checkedBytes, checkApplicationImage, progressPercent } from "./firmware-utils.mjs";

const $ = id => document.getElementById(id);
const supported = Boolean(navigator.serial && globalThis.isSecureContext);
const manifestUrl = new URL("./firmware/manifest.json", import.meta.url);
const state = { busy: false, mode: null, port: null, transport: null, loader: null,
  manifest: null, reader: null, readTask: null, chip: "等待连接" };

function log(message) {
  const box = $("log");
  const atBottom = box.scrollTop + box.clientHeight >= box.scrollHeight - 35;
  box.textContent = (box.textContent + String(message).replace(/\x1b\[[0-?]*[ -/]*[@-~]/g, "")).slice(-180000);
  if (atBottom) box.scrollTop = box.scrollHeight;
}
function line(message) { log(message + "\n"); }
function status(text) { $("status").textContent = text; }
function progress(value) { $("progress").value = value; $("percent").textContent = value + "%"; }
function render() {
  const connected = Boolean(state.mode);
  $("connection").textContent = connected ? "已连接" : "未连接";
  $("connection").classList.toggle("connected", connected);
  $("chip").textContent = state.chip;
  $("mode").textContent = state.mode === "flash" ? "固件烧录" : state.mode === "log" ? "运行日志" : "未连接";
  $("connect").disabled = !supported || state.busy || connected;
  $("install").disabled = state.busy || state.mode !== "flash" || !state.manifest;
  $("custom-install").disabled = state.busy || state.mode !== "flash" || !$("custom-file").files.length;
  $("custom-file").disabled = state.busy;
  $("baud").disabled = state.busy || connected;
  $("restart").disabled = state.busy || state.mode !== "flash";
  $("disconnect").disabled = state.busy || !connected;
  $("read-logs").disabled = !supported || state.busy || connected;
}
function failure(error) {
  const message = error?.name === "NotFoundError" ? "已取消选择设备。" : error?.message || String(error);
  status(message); line("提示：" + message);
}
async function releaseConnection() {
  const reader = state.reader, readTask = state.readTask;
  state.reader = null;
  if (reader) { try { await reader.cancel(); } catch {} }
  if (readTask) { try { await readTask; } catch {} }
  if (state.transport) { try { await state.transport.disconnect(); } catch {} }
  if (state.port?.readable || state.port?.writable) { try { await state.port.close(); } catch {} }
  state.port = null; state.transport = null; state.loader = null;
  state.readTask = null; state.mode = null; state.chip = "等待连接";
  render();
}

async function connect() {
  state.busy = true; render(); progress(0);
  try {
    // requestPort must run directly in the user's click handler.
    state.port = await navigator.serial.requestPort();
    status("正在识别设备，请稍候…");
    state.transport = new Transport(state.port, false);
    state.loader = new ESPLoader({ transport: state.transport, baudrate: Number($("baud").value),
      terminal: { clean() {}, write: log, writeLine: line }, debugLogging: false });
    const chip = await state.loader.main();
    if (!/^ESP32-S3(?:\b|$)/i.test(chip)) throw new Error("检测到 " + chip + "。此页面只为 ESP32-S3 遥控器提供固件。");
    state.chip = chip; state.mode = "flash";
    status("设备已连接，可以开始烧录。");
    line("设备识别成功：" + chip);
  } catch (error) { await releaseConnection(); failure(error); }
  finally { state.busy = false; render(); }
}

async function install(custom = false) {
  if (state.mode !== "flash" || state.busy) return;
  state.busy = true; render(); progress(0);
  try {
    let files;
    if (custom) {
      const file = $("custom-file").files[0];
      if (!file) throw new Error("请先选择应用固件。");
      if (file.size > 0x300000) throw new Error("应用固件不能超过 3 MB。");
      files = [{ address: 0x10000, data: checkApplicationImage(new Uint8Array(await file.arrayBuffer())) }];
      line("自选应用固件：" + file.name + "，写入 0x10000。");
    } else {
      status("正在下载并校验固件…");
      validateManifest(state.manifest, manifestUrl);
      files = [];
      for (const part of state.manifest.parts) {
        const response = await fetch(new URL(part.path, manifestUrl), { cache: "no-store" });
        if (!response.ok) throw new Error("固件下载失败，请稍后重试。");
        const data = await checkedBytes(new Uint8Array(await response.arrayBuffer()), part.size, part.sha256);
        files.push({ address: part.offset, data });
        line("下载校验通过：" + part.name);
      }
    }
    const sizes = files.map(file => file.data.length);
    if (!globalThis.SparkMD5?.ArrayBuffer) throw new Error("校验工具未加载，请刷新页面。");
    status("正在写入，请勿断开数据线…");
    await state.loader.writeFlash({ fileArray: files, flashMode: "dio", flashFreq: "80m", flashSize: "8MB",
      eraseAll: false, compress: true,
      calculateMD5Hash: image => globalThis.SparkMD5.ArrayBuffer.hash(image.buffer.slice(image.byteOffset, image.byteOffset + image.byteLength)),
      reportProgress: (index, written, total) => {
        progress(progressPercent(sizes, index, written, total));
        status(written === total ? "正在校验写入数据…" : "正在写入，请勿断开数据线…");
      } });
    progress(100);
    line("写入与设备端 MD5 校验完成。");
    status("烧录成功，正在重启设备…");
    let resetSent = false;
    try { await state.loader.after("hard_reset"); resetSent = true; }
    catch { line("自动重启未完成，可短按 RESET 或重新插拔 USB。"); }
    await releaseConnection();
    status(resetSent ? "烧录成功，已发送重启命令。可重新连接查看日志。" : "烧录成功，请短按 RESET 或重新插拔 USB 启动设备。");
  } catch (error) { failure(error); await releaseConnection(); }
  finally { state.busy = false; render(); }
}

async function readLogs() {
  state.busy = true; render();
  try {
    state.port = await navigator.serial.requestPort();
    await state.port.open({ baudRate: 115200 });
    state.mode = "log"; state.chip = "USB 串口（运行日志）";
    status("正在读取设备的新日志。点击断开连接可停止。");
    line("开始读取运行日志（115200）。历史日志不会从设备回放。");
    const reader = state.port.readable.getReader(); state.reader = reader;
    state.readTask = (async () => {
      const decoder = new TextDecoder();
      try {
        while (state.reader === reader) {
          const { value, done } = await reader.read();
          if (done) break;
          if (value) log(decoder.decode(value, { stream: true }));
        }
        log(decoder.decode());
      } catch (error) { if (state.reader === reader) failure(error); }
      finally {
        reader.releaseLock();
        if (state.reader === reader) queueMicrotask(async () => {
          await releaseConnection(); status("日志读取已结束，可以重新连接。");
        });
      }
    })();
  } catch (error) { await releaseConnection(); failure(error); }
  finally { state.busy = false; render(); }
}

$("connect").addEventListener("click", connect);
$("install").addEventListener("click", () => install(false));
$("custom-install").addEventListener("click", () => install(true));
$("custom-file").addEventListener("change", render);
$("read-logs").addEventListener("click", readLogs);
$("disconnect").addEventListener("click", async () => {
  state.busy = true; render();
  if (state.mode === "flash") {
    try { await state.loader.after("hard_reset"); }
    catch { line("未完成自动重启，可短按 RESET 返回正常运行。"); }
  }
  await releaseConnection(); state.busy = false;
  status("连接已断开。"); render();
});
$("restart").addEventListener("click", async () => {
  state.busy = true; render();
  try { await state.loader.after("hard_reset"); status("已发送重启命令，连接已释放。"); }
  catch (error) { failure(error); }
  finally { await releaseConnection(); state.busy = false; render(); }
});
$("clear-log").addEventListener("click", () => { $("log").textContent = ""; });
$("save-log").addEventListener("click", () => {
  const url = URL.createObjectURL(new Blob([$("log").textContent], { type: "text/plain;charset=utf-8" }));
  const link = document.createElement("a"); link.href = url; link.download = "remote-log-" + new Date().toISOString().replace(/[:.]/g, "-") + ".txt";
  link.click(); setTimeout(() => URL.revokeObjectURL(url), 1000);
});
if (navigator.serial) navigator.serial.addEventListener("disconnect", event => {
  if (event.target === state.port && !state.busy) releaseConnection().then(() => status("设备已断开，请重新连接。"));
});
$("browser-warning").hidden = supported;
status(supported ? "请先连接设备。" : "请使用电脑上的 Edge 或 Chrome 打开本页。");
render();

try {
  const response = await fetch(manifestUrl, { cache: "no-store" });
  if (!response.ok) throw new Error("尚未获取到网站固件，请稍后刷新。");
  state.manifest = validateManifest(await response.json(), manifestUrl);
  $("version").textContent = state.manifest.version;
  const date = new Date(state.manifest.builtAt).toLocaleString("zh-CN");
  $("build-detail").textContent = "构建于 " + date + " · 提交 " + state.manifest.commit.slice(0, 7) + " · ESP32-S3";
  $("release-link").href = "https://github.com/1571394785/lele-esp32s3-remote/releases/tag/" + encodeURIComponent(state.manifest.version);
  render();
} catch (error) {
  $("version").textContent = "固件暂时不可用";
  $("build-detail").textContent = error.message;
  line(error.message);
}
