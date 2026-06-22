// SPDX-License-Identifier: MIT
// Copyright 2026 Aaron Perkins

"use strict";

// ---- GATT contract (must match firmware/main/bt_config.cpp) --------------------------------
const SVC        = "5f1d0000-7c5a-4e2a-9b6e-2a8f3c9d1e00";
const CH_INFO    = "5f1d0001-7c5a-4e2a-9b6e-2a8f3c9d1e00"; // read + notify : JSON device/info+settings
const CH_CMD     = "5f1d0002-7c5a-4e2a-9b6e-2a8f3c9d1e00"; // write         : JSON config command
const CH_OTACTL  = "5f1d0003-7c5a-4e2a-9b6e-2a8f3c9d1e00"; // write + notify: OTA control / status
const CH_OTADATA = "5f1d0004-7c5a-4e2a-9b6e-2a8f3c9d1e00"; // write-no-resp : OTA payload chunks

// OTA control opcodes (client -> device, first byte)
const OP_BEGIN = 0x01, OP_END = 0x02, OP_ABORT = 0x03;
// OTA status codes (device -> client, first byte)
const ST_READY = 0x10, ST_ACK = 0x11, ST_DONE = 0x12, ST_ERROR = 0x1f;

// ---- DOM ----------------------------------------------------------------------------------
const $ = (id) => document.getElementById(id);
const els = {
  connect: $("connectBtn"), disconnect: $("disconnectBtn"),
  connDot: $("connDot"), connState: $("connState"), availWarn: $("availWarn"),
  deviceCard: $("deviceCard"), settingsCard: $("settingsCard"), otaCard: $("otaCard"),
  d_name: $("d_name"), d_fw: $("d_fw"), d_build: $("d_build"), d_slot: $("d_slot"),
  d_batt: $("d_batt"), d_ident: $("d_ident"),
  s_transport: $("s_transport"), s_profile: $("s_profile"), s_dirmode: $("s_dirmode"),
  apply: $("applyBtn"), forget: $("forgetBtn"), reboot: $("rebootBtn"),
  otaFile: $("otaFile"), otaMeta: $("otaMeta"), otaBar: $("otaBar"),
  otaStatus: $("otaStatus"), otaRate: $("otaRate"),
  otaStart: $("otaStartBtn"), otaAbort: $("otaAbortBtn"),
  log: $("log"),
};

// ---- logging ------------------------------------------------------------------------------
function log(msg, cls) {
  const t = new Date().toLocaleTimeString();
  const line = document.createElement("div");
  line.innerHTML = `<span class="t">${t}</span> `;
  const s = document.createElement("span");
  if (cls) s.className = cls;
  s.textContent = msg;
  line.appendChild(s);
  els.log.appendChild(line);
  els.log.scrollTop = els.log.scrollHeight;
  console.log(`[${t}] ${msg}`);
}

// ---- BLE state ----------------------------------------------------------------------------
let device = null, server = null;
let chInfo = null, chCmd = null, chOtaCtl = null, chOtaData = null;
let info = null;             // last-parsed device info JSON
let otaImage = null;         // ArrayBuffer of selected firmware

// ---- availability check (no user gesture needed) ------------------------------------------
async function checkAvailability() {
  if (!navigator.bluetooth) {
    els.availWarn.classList.remove("hide");
    els.availWarn.innerHTML = "Web Bluetooth is not available in this browser. " +
      "Use Chrome/Edge. On Linux you may need <code>chrome://flags/#enable-experimental-web-platform-features</code>.";
    els.connect.disabled = true;
    log("navigator.bluetooth missing", "e");
    return false;
  }
  try {
    const avail = await navigator.bluetooth.getAvailability();
    log(`Web Bluetooth available: adapter ${avail ? "present" : "NOT present"}`, avail ? "ok" : "w");
    if (!avail) {
      els.availWarn.classList.remove("hide");
      els.availWarn.textContent = "No Bluetooth adapter detected (or it is powered off).";
    }
    return avail;
  } catch (e) {
    log("getAvailability() failed: " + e, "e");
    return false;
  }
}

// ---- connect / disconnect -----------------------------------------------------------------
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function connect() {
  try {
    log("Requesting device (choose 'NES Advantage Config')...");
    device = await navigator.bluetooth.requestDevice({
      filters: [{ services: [SVC] }],
      optionalServices: [SVC],
    });
    device.addEventListener("gattserverdisconnected", onDisconnected);

    // The first GATT connect on Chrome/Linux (BlueZ) often throws a transient NetworkError; the
    // device is already chosen, so we can retry the connect+discovery without another user gesture.
    const ATTEMPTS = 4;
    for (let i = 1; i <= ATTEMPTS; i++) {
      try {
        log(`Connecting to ${device.name || "device"} (attempt ${i}/${ATTEMPTS})...`);
        await connectOnce();
        setConnected(true);
        log("Connected.", "ok");
        return;
      } catch (e) {
        log(`Attempt ${i} failed: ${e}`, "w");
        try { if (device.gatt.connected) device.gatt.disconnect(); } catch (_) {}
        if (i < ATTEMPTS) await sleep(600);
      }
    }
    log("Could not connect after retries, click Connect to try again.", "e");
    setConnected(false);
  } catch (e) {
    log("Connect cancelled/failed: " + e, "e");   // e.g. user dismissed the chooser
    setConnected(false);
  }
}

async function connectOnce() {
  server = await device.gatt.connect();
  const svc = await server.getPrimaryService(SVC);
  chInfo    = await svc.getCharacteristic(CH_INFO);
  chCmd     = await svc.getCharacteristic(CH_CMD);
  chOtaCtl  = await svc.getCharacteristic(CH_OTACTL);
  chOtaData = await svc.getCharacteristic(CH_OTADATA);

  await chInfo.startNotifications();
  chInfo.addEventListener("characteristicvaluechanged", (e) => onInfo(e.target.value));
  await chOtaCtl.startNotifications();
  chOtaCtl.addEventListener("characteristicvaluechanged", (e) => onOtaStatus(e.target.value));

  onInfo(await chInfo.readValue());
}

function disconnect() {
  if (device && device.gatt.connected) device.gatt.disconnect();
}

function onDisconnected() {
  log("Disconnected.", "w");
  setConnected(false);
}

function setConnected(on) {
  els.connDot.className = "dot " + (on ? "on" : "off");
  els.connState.textContent = on ? "Connected" : "Disconnected";
  els.connect.disabled = on;
  els.disconnect.disabled = !on;
  els.deviceCard.classList.toggle("hide", !on);
  els.settingsCard.classList.toggle("hide", !on);
  els.otaCard.classList.toggle("hide", !on);
  if (!on) { chInfo = chCmd = chOtaCtl = chOtaData = null; info = null; }
}

// ---- device info / settings ---------------------------------------------------------------
function onInfo(dataView) {
  const json = new TextDecoder().decode(dataView);
  try { info = JSON.parse(json); } catch (e) { log("Bad INFO JSON: " + json, "e"); return; }
  els.d_name.textContent  = info.name ?? "-";
  els.d_fw.textContent    = info.fw ?? "-";
  els.d_build.textContent = info.build ?? "-";
  els.d_slot.textContent  = info.slot ?? "-";
  els.d_batt.textContent  = (info.batt == null || info.batt < 0) ? "absent" : info.batt + "%";
  els.d_ident.textContent = info.ident ?? "-";

  els.s_transport.value = String(info.transport ?? 0);
  fillProfileLists();
  log("Device info updated.");
}

function fillProfileLists() {
  if (!info) return;
  const t = parseInt(els.s_transport.value, 10);
  const profs = (info.profiles && info.profiles[t]) || [];
  const dirs  = (info.dirmodes && info.dirmodes[t]) || [];
  fillSelect(els.s_profile, profs, t === info.transport ? info.profile : 0);
  fillSelect(els.s_dirmode, dirs, t === info.transport ? info.dirmode : 0);
}

function fillSelect(sel, items, selected) {
  sel.innerHTML = "";
  items.forEach((name, i) => {
    const o = document.createElement("option");
    o.value = String(i); o.textContent = `${i + 1}. ${name}`;
    if (i === selected) o.selected = true;
    sel.appendChild(o);
  });
}

async function sendCmd(obj) {
  const bytes = new TextEncoder().encode(JSON.stringify(obj));
  await chCmd.writeValue(bytes);
  log("sent cmd " + JSON.stringify(obj));
}

async function applySettings() {
  try {
    await sendCmd({ transport: parseInt(els.s_transport.value, 10) });
    await sendCmd({ profile:   parseInt(els.s_profile.value, 10) });
    await sendCmd({ dirmode:   parseInt(els.s_dirmode.value, 10) });
    log("Settings written; rebooting device...", "ok");
    await sendCmd({ action: "reboot" });
  } catch (e) { log("Apply failed: " + e, "e"); }
}

// ---- OTA ----------------------------------------------------------------------------------
let ota = null;  // active transfer state

function crc32(buf) {
  let c, crc = 0xffffffff;
  const u8 = new Uint8Array(buf);
  for (let n = 0; n < u8.length; n++) {
    c = (crc ^ u8[n]) & 0xff;
    for (let k = 0; k < 8; k++) c = (c & 1) ? (0xedb88320 ^ (c >>> 1)) : (c >>> 1);
    crc = (crc >>> 8) ^ c;
  }
  return (crc ^ 0xffffffff) >>> 0;
}

function onFile(file) {
  if (!file) return;
  const r = new FileReader();
  r.onload = () => {
    otaImage = r.result;
    const size = otaImage.byteLength;
    const crc = crc32(otaImage);
    els.otaMeta.textContent = `${file.name}, ${size.toLocaleString()} bytes, crc32 0x${crc.toString(16).padStart(8,"0")}`;
    els.otaStart.disabled = false;
    els.otaStatus.textContent = "Ready to flash.";
    log(`Loaded ${file.name}: ${size} bytes, crc32 0x${crc.toString(16)}`);
  };
  r.readAsArrayBuffer(file);
}

function u32le(n) { const b = new Uint8Array(4); new DataView(b.buffer).setUint32(0, n >>> 0, true); return b; }
function rdU32le(dv, off) { return dv.getUint32(off, true); }

async function startOta() {
  if (!otaImage || !chOtaCtl || !chOtaData) return;
  const size = otaImage.byteLength;
  const crc = crc32(otaImage);
  ota = { size, crc, sent: 0, acked: 0, chunk: 240, window: 8192, t0: performance.now(),
          waiters: [], done: false, error: null };
  setOtaActive(true);
  log(`OTA begin: ${size} bytes`, "ok");
  els.otaStatus.textContent = "Negotiating...";

  // BEGIN: [op][u32 size][u32 crc]
  const begin = new Uint8Array(9);
  begin[0] = OP_BEGIN; begin.set(u32le(size), 1); begin.set(u32le(crc), 5);
  await chOtaCtl.writeValue(begin);
  // device replies with ST_READY via notification -> onOtaStatus kicks off the pump
}

function setOtaActive(active) {
  els.otaStart.disabled = active || !otaImage;
  els.otaAbort.disabled = !active;
  els.otaFile.disabled = active;
}

function onOtaStatus(dv) {
  const code = dv.getUint8(0);
  if (code === ST_READY) {
    if (dv.byteLength >= 9) { ota.chunk = rdU32le(dv, 1); ota.window = rdU32le(dv, 5); }
    log(`Device READY, chunk ${ota.chunk}B, window ${ota.window}B`);
    pumpOta();
  } else if (code === ST_ACK) {
    ota.acked = rdU32le(dv, 1);
    const w = ota.waiters; ota.waiters = []; w.forEach((fn) => fn());
    updateOtaProgress();
  } else if (code === ST_DONE) {
    ota.done = true;
    els.otaBar.style.width = "100%";
    els.otaStatus.textContent = "Flashed & verified, device is rebooting into the new firmware.";
    log("OTA DONE, device rebooting.", "ok");
    setOtaActive(false);
  } else if (code === ST_ERROR) {
    const err = dv.byteLength > 1 ? dv.getUint8(1) : 0;
    const msg = dv.byteLength > 2 ? new TextDecoder().decode(dv.buffer.slice(2)) : "";
    ota.error = `err ${err} ${msg}`;
    els.otaStatus.textContent = "OTA error: " + ota.error;
    log("OTA ERROR: " + ota.error, "e");
    setOtaActive(false);
  }
}

function updateOtaProgress() {
  const pct = Math.floor((ota.acked / ota.size) * 100);
  els.otaBar.style.width = pct + "%";
  const dt = (performance.now() - ota.t0) / 1000;
  const rate = dt > 0 ? (ota.acked / dt / 1024) : 0;
  els.otaStatus.textContent = `Flashing... ${pct}% (${ota.acked.toLocaleString()} / ${ota.size.toLocaleString()} B)`;
  els.otaRate.textContent = `${rate.toFixed(1)} KiB/s`;
}

function waitForAck() {
  return new Promise((resolve) => ota.waiters.push(resolve));
}

async function pumpOta() {
  const u8 = new Uint8Array(otaImage);
  try {
    while (ota.sent < ota.size && !ota.error) {
      // flow control: keep at most `window` bytes unacked
      while (ota.sent - ota.acked >= ota.window && !ota.error) await waitForAck();
      const end = Math.min(ota.sent + ota.chunk, ota.size);
      await chOtaData.writeValueWithoutResponse(u8.subarray(ota.sent, end));
      ota.sent = end;
      if (ota.sent % (ota.chunk * 20) < ota.chunk) updateOtaProgress();
    }
    if (ota.error) return;
    // wait for the device to ack everything before END
    while (ota.acked < ota.size && !ota.error) await waitForAck();
    log("All data sent; finalizing...");
    await chOtaCtl.writeValue(new Uint8Array([OP_END]));
  } catch (e) {
    log("OTA pump failed: " + e, "e");
    els.otaStatus.textContent = "Transfer failed: " + e;
    setOtaActive(false);
  }
}

async function abortOta() {
  try { if (chOtaCtl) await chOtaCtl.writeValue(new Uint8Array([OP_ABORT])); } catch (e) {}
  if (ota) ota.error = "aborted";
  els.otaStatus.textContent = "Aborted.";
  setOtaActive(false);
  log("OTA aborted by user.", "w");
}

// ---- wire up ------------------------------------------------------------------------------
els.connect.addEventListener("click", connect);
els.disconnect.addEventListener("click", disconnect);
els.apply.addEventListener("click", applySettings);
els.forget.addEventListener("click", () => sendCmd({ action: "forget" }).catch((e) => log(e, "e")));
els.reboot.addEventListener("click", () => sendCmd({ action: "reboot" }).catch((e) => log(e, "e")));
els.s_transport.addEventListener("change", fillProfileLists);
els.otaFile.addEventListener("change", (e) => onFile(e.target.files[0]));
els.otaStart.addEventListener("click", startOta);
els.otaAbort.addEventListener("click", abortOta);

// Drag & drop a .bin onto the OTA card (nicer than the file picker, and scriptable for testing).
["dragenter", "dragover"].forEach((ev) =>
  els.otaCard.addEventListener(ev, (e) => { e.preventDefault(); els.otaCard.classList.add("drop"); }));
["dragleave", "drop"].forEach((ev) =>
  els.otaCard.addEventListener(ev, (e) => { e.preventDefault(); els.otaCard.classList.remove("drop"); }));
els.otaCard.addEventListener("drop", (e) => {
  const f = e.dataTransfer && e.dataTransfer.files[0];
  if (f) onFile(f);
});

log("Page loaded.");
checkAvailability();