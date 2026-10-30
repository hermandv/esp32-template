/* devlink web UI (SPEC §8): Devices, Firmware and Monitor pages on top of the local server's /api. Live updates come
   from /api/events (Server-Sent Events); the Monitor page opens the board's WebSocket itself. */
"use strict";

const $ = (id) => document.getElementById(id);
const esc = (s) => String(s ?? "").replace(/[&<>"']/g, (c) => ({"&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;"}[c]));
const store = {
  get(k) { try { return localStorage.getItem(k); } catch (e) { return null; } },
  set(k, v) { try { localStorage.setItem(k, v); } catch (e) { /* private window */ } },
};

const S = {
  devices: [], selected: null, jobs: new Map(), settings: {},
  fw: {token: null, info: null, checked: new Set(), jobId: null, lastMacs: []},
  cfgJobId: null,
};

// ---------------------------------------------------------------- server calls
async function api(path, opts = {}) {
  const init = {method: opts.method || (opts.body !== undefined || opts.form ? "POST" : "GET"), headers: {"X-Devlink": "1"}};
  if (opts.form) init.body = opts.form;
  else if (opts.body !== undefined) { init.body = JSON.stringify(opts.body); init.headers["Content-Type"] = "application/json"; }
  const res = await fetch(path, init);
  if (opts.raw) return res;
  let data = null;
  try { data = await res.json(); } catch (e) { /* not JSON */ }
  if (!res.ok) throw new Error((data && data.error) || `${res.status} ${res.statusText}`);
  return data;
}

function toast(text, kind = "info") {
  const el = document.createElement("div");
  el.className = `alert alert-${kind} alert-dismissible shadow-sm mb-2`;
  el.style.minWidth = "320px";
  el.innerHTML = `<div>${esc(text)}</div><a class="btn-close" data-bs-dismiss="alert"></a>`;
  $("toasts").appendChild(el);
  setTimeout(() => el.remove(), kind === "danger" ? 9000 : 4500);
}

async function attempt(what, fn) {
  try { return await fn(); } catch (e) { toast(`${what}: ${e.message}`, "danger"); return undefined; }
}

const modal = (id) => window.tabler.Modal.getOrCreateInstance($(id));

/** Generic dialog: fields [{id,label,type,value,placeholder}], optional check {text}. Resolves {values, checked} or null. */
function ask({title, text = "", fields = [], check = null, ok = "OK", danger = false}) {
  $("askTitle").textContent = title;
  $("askText").textContent = text;
  $("askFields").innerHTML = fields.map((f) => `<div class="mb-2"><label class="form-label">${esc(f.label)}</label>
    <input class="form-control" id="ask_${f.id}" type="${f.type || "text"}" value="${esc(f.value || "")}"
    placeholder="${esc(f.placeholder || "")}" autocomplete="off"></div>`).join("");
  $("askCheckWrap").hidden = !check;
  $("askCheck").checked = false;
  $("askCheckText").textContent = check ? check.text : "";
  $("askOk").textContent = ok;
  $("askOk").className = `btn ${danger ? "btn-danger" : "btn-primary"}`;
  return new Promise((resolve) => {
    let result = null;
    const m = modal("askModal");
    const onOk = () => {
      result = {values: Object.fromEntries(fields.map((f) => [f.id, $(`ask_${f.id}`).value])), checked: $("askCheck").checked};
      m.hide();
    };
    const onKey = (e) => { if (e.key === "Enter") onOk(); };
    $("askOk").onclick = onOk;
    $("askFields").onkeydown = onKey;
    $("askModal").addEventListener("hidden.bs.modal", () => resolve(result), {once: true});
    $("askModal").addEventListener("shown.bs.modal", () => { const f = $("askFields").querySelector("input"); if (f) f.focus(); }, {once: true});
    m.show();
  });
}

// ---------------------------------------------------------------- tabs + theme
function showTab(name) {
  document.querySelectorAll("[data-page]").forEach((s) => { s.hidden = s.dataset.page !== name; });
  document.querySelectorAll("#tabs .nav-link").forEach((a) => a.classList.toggle("active", a.dataset.tab === name));
  store.set("devlink_tab", name);
  if (name === "monitor") Mon.resize();
}
document.querySelectorAll("#tabs .nav-link").forEach((a) => a.addEventListener("click", (e) => { e.preventDefault(); showTab(a.dataset.tab); }));

function setTheme(t) {
  document.documentElement.dataset.bsTheme = t;
  $("themeBtn").innerHTML = `<i class="ti ti-${t === "dark" ? "sun" : "moon"}"></i>`;
  store.set("devlink_theme", t);
  Mon.rebuild();
}
$("themeBtn").onclick = () => setTheme(document.documentElement.dataset.bsTheme === "dark" ? "light" : "dark");

// ---------------------------------------------------------------- events from the server
function connectEvents() {
  const es = new EventSource("/api/events");
  es.onopen = () => { $("conn").className = "badge bg-green-lt"; $("conn").textContent = "server connected"; loadState(); };
  es.onerror = () => { $("conn").className = "badge bg-red-lt"; $("conn").textContent = "server not reachable"; };
  es.addEventListener("devices", (e) => setDevices(JSON.parse(e.data)));
  es.addEventListener("note", (e) => addNote(JSON.parse(e.data)));
  es.addEventListener("job", (e) => onJob(JSON.parse(e.data)));
}

async function loadState() {
  const st = await attempt("load state", () => api("/api/state"));
  if (!st) return;
  S.settings = st.settings;
  $("scanAps").checked = !!st.settings.scan_for_unconfigured;
  $("notes").textContent = "";
  st.notes.forEach(addNote);
  st.jobs.forEach(onJob);
  setDevices(st.devices);
}

// ---------------------------------------------------------------- Devices
function addNote(line) {
  const box = $("notes");
  const atEnd = box.scrollTop + box.clientHeight >= box.scrollHeight - 4;
  box.textContent += line + "\n";
  if (box.textContent.length > 60000) box.textContent = box.textContent.slice(-40000);
  if (atEnd) box.scrollTop = box.scrollHeight;
}

function setDevices(list) {
  S.devices = list;
  const lan = list.filter((d) => d.on_lan);
  $("summary").textContent = `${lan.length} board(s) on the network, ${list.length - lan.length} to configure`;
  renderDevices();
  Fw.renderBoards();
  Mon.fillBoards();
}

function renderDevices() {
  const tb = $("devTable").tBodies[0];
  $("noDevices").hidden = S.devices.length > 0;
  tb.innerHTML = S.devices.map((d) => {
    const cls = [d.key === S.selected ? "selected" : "", d.warn ? "table-warning" : d.mode === "needs-config" ? "table-info" : ""].join(" ");
    const mode = d.mode === "recovery" ? `<span class="badge bg-orange-lt">recovery</span>`
      : d.mode === "needs-config" ? `<span class="badge bg-azure-lt">needs config</span>`
      : `<span class="badge bg-green-lt">${esc(d.mode || "?")}</span>`;
    const boot = d.warn ? `<span class="text-warning fw-bold" title="check this board">${esc(d.boot)}</span>` : esc(d.boot);
    return `<tr class="${cls}" data-key="${esc(d.key)}">
      <td class="fw-medium">${esc(d.label || (d.ap_ssid ? d.ap_ssid : "(no name)"))}</td>
      <td class="font-monospace small">${esc(d.mac || "-")}</td><td class="font-monospace small">${esc(d.id)}</td>
      <td>${d.ip ? `<a href="http://${esc(d.ip)}/" target="_blank" rel="noopener">${esc(d.ip)}</a>` : "-"}</td>
      <td>${mode}</td><td>${boot}</td><td>${esc(d.app)}</td><td>${esc(d.rec)}</td><td>${esc(d.rssi)}</td>
      <td class="text-secondary small">${d.age_s == null ? "" : d.age_s + " s ago"}</td></tr>`;
  }).join("");
  updateDeviceButtons();
}

function selectedDevice() { return S.devices.find((d) => d.key === S.selected) || null; }

function updateDeviceButtons() {
  const d = selectedDevice();
  $("selInfo").textContent = d ? `${d.label || d.ap_ssid || d.mac}` : "Select a board.";
  document.querySelectorAll("[data-need]").forEach((b) => {
    const ok = b.dataset.need === "config" ? !!(d && d.mode === "needs-config") : !!(d && d.on_lan);
    b.classList.toggle("disabled", !ok);
    b.disabled = !ok;
  });
  $("webBtn").href = d && d.on_lan ? `http://${d.ip}/` : "#";
}

$("devTable").addEventListener("click", (e) => {
  const tr = e.target.closest("tr[data-key]");
  if (!tr || e.target.closest("a")) return;
  S.selected = tr.dataset.key;
  renderDevices();
});
$("devTable").addEventListener("dblclick", (e) => {
  const d = selectedDevice();
  if (d && d.mode === "needs-config") openConfigure({mode: "ap", ap_ssid: d.ap_ssid, label: ""});
  else if (d && d.on_lan) window.open(`http://${d.ip}/`, "_blank", "noopener");
});

$("refreshBtn").onclick = () => attempt("refresh", () => api("/api/refresh", {body: {}}));
$("scanAps").onchange = () => attempt("settings", () => api("/api/settings", {body: {scan_for_unconfigured: $("scanAps").checked}}));

$("addIpBtn").onclick = async () => {
  const r = await ask({title: "Add a board by IP", fields: [{id: "ip", label: "Board IP address", placeholder: "10.0.0.42"}], ok: "Add"});
  if (r && r.values.ip.trim()) {
    const res = await attempt("add by IP", () => api("/api/devices/add_ip", {body: {ip: r.values.ip.trim()}}));
    if (res) toast(`Added ${res.mac}`, "success");
  }
};

function openConfigure(target) {
  S.cfgTarget = target;
  S.cfgJobId = null;
  $("cfgTitle").textContent = target.mode === "usb" ? `Configure ${target.port} via USB` : `Configure ${target.ap_ssid}`;
  $("cfgHint").textContent = target.mode === "usb"
    ? "Sends the settings over the USB cable (Improv serial)."
    : "This PC joins the board's access point for a few seconds (no internet meanwhile), sends the settings and returns to its current network.";
  $("cfgSsid").value = S.settings.bench_ssid || "";
  $("cfgPw").value = "";
  $("cfgLabel").value = target.label || "";
  $("cfgLog").hidden = true;
  $("cfgLog").textContent = "";
  $("cfgGo").disabled = false;
  modal("cfgModal").show();
  setTimeout(() => ($("cfgSsid").value ? $("cfgPw") : $("cfgSsid")).focus(), 300);
}

$("cfgGo").onclick = async () => {
  const t = S.cfgTarget;
  const body = {ssid: $("cfgSsid").value.trim(), password: $("cfgPw").value, label: $("cfgLabel").value.trim()};
  if (!body.ssid) { $("cfgSsid").focus(); return; }
  Object.assign(body, t.mode === "usb" ? {port: t.port} : {ap_ssid: t.ap_ssid});
  $("cfgGo").disabled = true;
  const job = await attempt("configure", () => api(`/api/configure/${t.mode}`, {body}));
  $("cfgPw").value = "";
  if (!job) { $("cfgGo").disabled = false; return; }
  S.settings.bench_ssid = body.ssid;
  S.cfgJobId = job.id;
  onJob(job);
};

$("cfgBtn").onclick = () => { const d = selectedDevice(); if (d) openConfigure({mode: "ap", ap_ssid: d.ap_ssid, label: ""}); };

$("usbBtn").onclick = async () => {
  $("usbList").innerHTML = "";
  $("usbMsg").hidden = false;
  $("usbMsg").innerHTML = `<span class="spinner-border spinner-border-sm me-2"></span>Looking for boards that answer Improv on USB…`;
  modal("usbModal").show();
  const boards = await attempt("USB", () => api("/api/usb/boards", {body: {}}));
  if (!boards) { modal("usbModal").hide(); return; }
  if (!boards.length) { $("usbMsg").textContent = "No board answers Improv on a USB port. Close anything that holds the port (serial monitor, test script)."; return; }
  $("usbMsg").hidden = true;
  $("usbList").innerHTML = boards.map((b, i) => `<a href="#" class="list-group-item list-group-item-action" data-i="${i}">
    <div class="fw-medium">${esc(b.port)} · ${esc(b.name || "(no name)")}</div>
    <div class="small text-secondary">${esc(b.firmware)} ${esc(b.version)} · ${esc(b.chip)} · ${esc(b.serial_number)}</div></a>`).join("");
  $("usbList").onclick = (e) => {
    const a = e.target.closest("[data-i]");
    if (!a) return;
    e.preventDefault();
    const b = boards[+a.dataset.i];
    modal("usbModal").hide();
    openConfigure({mode: "usb", port: b.port, label: b.label});
  };
};

// Program blank board (USB): pick port and builds -> check the board (nothing written) -> confirm -> program.
const Prog = {
  stage: "check",   // check -> confirm -> running
  jobId: null,
  builds: [],

  async open() {
    $("progLog").hidden = true; $("progLog").textContent = ""; $("progBoard").hidden = true;
    $("progPw").value = ""; $("progSsid").value = S.settings.bench_ssid || "";
    this.setStage("check");
    modal("progModal").show();
    await Promise.all([this.loadPorts(), this.loadBuilds("")]);
  },

  async loadPorts() {
    const ports = await attempt("USB ports", () => api("/api/usb/ports")) || [];
    $("progPort").innerHTML = ports.length ? ports.map((p) => `<option value="${esc(p.port)}">${esc(p.desc)}</option>`).join("")
      : `<option value="">no USB-serial port found</option>`;
    this.setStage("check");
  },

  async loadBuilds(folder) {
    const r = await attempt("builds", () => api("/api/usb/builds", {body: {folder}}));
    if (!r) return;
    $("progFolder").value = r.folder;
    this.builds = r.builds;
    const opts = (kind) => [...r.builds].sort((a, b) => (a.kind !== kind) - (b.kind !== kind) || a.name.localeCompare(b.name))
      .map((b) => `<option value="${esc(b.dir)}">${esc(b.label)}</option>`).join("");
    $("progRec").innerHTML = opts("recovery");
    $("progApp").innerHTML = opts("app");
    const app = r.builds.find((b) => b.name === "app") || r.builds.find((b) => b.kind === "app");
    if (app) $("progApp").value = app.dir;
    await this.checkFw();
  },

  async checkFw() {
    this.setStage("check");
    this.fw = null;
    if (!$("progRec").value || !$("progApp").value) {
      $("progFw").innerHTML = `<span class="text-danger">No builds in this folder (each needs flasher_args.json).</span>`;
      return;
    }
    const fw = await attempt("builds", () => api("/api/usb/firmware", {body: this.picks()}));
    if (!fw) return;
    this.fw = fw;
    $("progFw").innerHTML = esc(fw.summary) + fw.errors.map((e) => `<div class="text-danger">${esc(e)}</div>`).join("")
      + fw.warnings.map((w) => `<div class="text-warning">${esc(w)}</div>`).join("");
  },

  picks() { return {recovery: $("progRec").value, app: $("progApp").value, folder: $("progFolder").value.trim()}; },

  setStage(stage) {
    this.stage = stage;
    const go = $("progGo");
    go.disabled = stage === "running";
    go.className = `btn ${stage === "confirm" ? "btn-danger" : "btn-primary"}`;
    go.textContent = stage === "confirm" ? "Erase ALL flash and program" : stage === "running" ? "Programming…" : "Check board…";
    $("progClose").textContent = stage === "running" ? "Cancel" : "Close";
    if (stage === "check") $("progBoard").hidden = true;
  },

  async go() {
    const port = $("progPort").value;
    if (!port || this.stage === "running") return;
    if (!this.fw || this.fw.errors.length) { toast("Pick a recovery and an app build without errors first", "warning"); return; }
    $("progLog").hidden = false;
    if (this.stage === "check") {
      const job = await attempt("check", () => api("/api/usb/check", {body: {port, ...this.picks()}}));
      if (job) { this.jobId = job.id; this.setStage("running"); this.onJob(job); }
      return;
    }
    const body = {port, ...this.picks(), label: $("progLabel").value.trim(), ssid: $("progSsid").value.trim(), password: $("progPw").value};
    const job = await attempt("program", () => api("/api/usb/program", {body}));
    $("progPw").value = "";
    if (job) { this.jobId = job.id; this.setStage("running"); this.onJob(job); }
  },

  onJob(job) {
    $("progLog").textContent = job.log.join("\n");
    $("progLog").scrollTop = $("progLog").scrollHeight;
    if (job.state === "running" || job._handled) return;
    job._handled = true;
    if (job.kind === "usbcheck") {
      const bc = job.result;
      if (!bc) { this.setStage("check"); return; }
      const box = $("progBoard");
      box.className = `alert mt-3 mb-0 ${bc.errors.length ? "alert-danger" : "alert-warning"}`;
      box.innerHTML = `<div class="fw-medium">Board on ${esc(bc.port)}: ${esc(bc.summary)}</div>`
        + bc.errors.map((e) => `<div>Not possible: ${esc(e)}</div>`).join("")
        + bc.warnings.map((w) => `<div>${esc(w)}</div>`).join("")
        + (bc.errors.length ? "" : `<div class="mt-1">Erase ALL flash of this board and write ${esc(this.fw.summary)}?</div>`);
      this.setStage(bc.errors.length ? "check" : "confirm");
      box.hidden = false;  // after setStage: "check" hides it
    } else {
      this.setStage("check");
      const r = job.result;
      if (r && r.ok && r.wifi_error) toast(`Programmed ${r.label || r.mac}, but Wi-Fi was not set (${r.wifi_error}). Check the password and use Configure via USB.`, "warning");
      else if (r && r.ok) toast(`Programmed ${r.label || r.mac}${r.url ? " · " + r.url : ""}`, "success");
      else toast(`Program: ${job.state === "failed" ? (job.log[job.log.length - 1] || "failed") : "stopped"}`, "warning");
    }
  },

  async close() {
    if (this.stage !== "running") { modal("progModal").hide(); return; }
    const job = S.jobs.get(this.jobId);
    if (job && job.kind === "usbcheck") return;  // a few seconds; nothing written
    const r = await ask({title: "Cancel programming", text: "Stop programming? A board stopped while it is being written does not start until it is programmed again.", ok: "Stop", danger: true});
    if (r) await attempt("cancel", () => api(`/api/jobs/${this.jobId}/cancel`, {body: {}}));
  },
};

$("progBtn").onclick = () => Prog.open();
$("progGo").onclick = () => Prog.go();
$("progClose").onclick = () => Prog.close();
$("progX").onclick = () => Prog.close();
$("progPorts").onclick = () => Prog.loadPorts();
$("progScan").onclick = () => Prog.loadBuilds($("progFolder").value.trim());
$("progFolder").onkeydown = (e) => { if (e.key === "Enter") Prog.loadBuilds($("progFolder").value.trim()); };
$("progRec").onchange = () => Prog.checkFw();
$("progApp").onchange = () => Prog.checkFw();
$("progPort").onchange = () => Prog.setStage("check");

$("nameBtn").onclick = async () => {
  const d = selectedDevice();
  if (!d) return;
  const r = await ask({title: "Friendly name", fields: [{id: "label", label: "Name (max 32 bytes)", value: d.label}], ok: "Save"});
  if (r) await attempt("set name", () => api(`/api/dev/${d.mac}/label`, {body: {label: r.values.label}}));
};

$("wifiBtn").onclick = async () => {
  const d = selectedDevice();
  if (!d) return;
  const r = await ask({title: `Change Wi-Fi of ${d.label || d.mac}`,
    text: "Safe change: if the board cannot connect with the new settings within 45 s, it goes back to the old ones and stays reachable.",
    fields: [{id: "ssid", label: "SSID", value: S.settings.bench_ssid || ""}, {id: "password", label: "Password", type: "password"}], ok: "Change"});
  if (r && r.values.ssid) {
    const res = await attempt("change Wi-Fi", () => api(`/api/dev/${d.mac}/wifi`, {body: r.values}));
    if (res) toast("Wi-Fi change sent; the board switches in 1 s and reverts within 45 s if it can't connect.", "success");
  }
};

document.querySelectorAll("[data-reboot]").forEach((a) => a.addEventListener("click", async (e) => {
  e.preventDefault();
  const d = selectedDevice();
  if (!d) return;
  const target = a.dataset.reboot;
  if (target === "other" && !(await ask({title: "Boot other slot", text: "Boot the app in the other slot (only if it holds a valid app)?", ok: "Boot"}))) return;
  const res = await attempt("reboot", () => api(`/api/dev/${d.mac}/reboot`, {body: {target}}));
  if (res) toast(`Rebooting ${d.label || d.mac} (${target})`, "success");
}));

$("resetBtn").onclick = async () => {
  const d = selectedDevice();
  if (!d) return;
  const r = await ask({title: "Factory reset", text: `Erase the devlink config of ${d.label || d.mac} (Wi-Fi, name)? It goes back to AP mode.`,
    check: {text: "Also erase the test app's NVS"}, ok: "Erase", danger: true});
  if (r) await attempt("factory reset", () => api(`/api/dev/${d.mac}/factory_reset`, {body: {nvs: r.checked}}));
};

$("dumpBtn").onclick = async () => {
  const d = selectedDevice();
  if (!d) return;
  const res = await attempt("core dump", () => api(`/api/dev/${d.mac}/coredump`, {raw: true}));
  if (!res) return;
  if (!res.ok) { const j = await res.json().catch(() => ({})); toast(j.error || `core dump: ${res.status}`, res.status === 404 ? "info" : "danger"); return; }
  const name = (res.headers.get("Content-Disposition") || "").match(/filename="([^"]+)"/)?.[1] || "coredump.elf";
  saveBlob(await res.blob(), name);
};

function saveBlob(blob, name) {
  const a = document.createElement("a");
  a.href = URL.createObjectURL(blob);
  a.download = name;
  a.click();
  setTimeout(() => URL.revokeObjectURL(a.href), 5000);
}

// ---------------------------------------------------------------- jobs
function onJob(job) {
  S.jobs.set(job.id, job);
  if (job.id === S.cfgJobId) {
    $("cfgLog").hidden = false;
    $("cfgLog").textContent = job.log.join("\n");
    $("cfgLog").scrollTop = $("cfgLog").scrollHeight;
    if (job.state !== "running") {
      $("cfgGo").disabled = false;
      const ok = job.result && job.result.ok;
      toast(ok ? `Configured: ${job.result.message}` : `Configure: ${(job.result && job.result.message) || "failed"}`, ok ? "success" : "warning");
    }
  }
  if (job.kind === "ota" && (job.id === S.fw.jobId || S.fw.jobId === null)) Fw.showJob(job);
  if (job.id === Prog.jobId) Prog.onJob(job);
}

// ---------------------------------------------------------------- Firmware
const Fw = {
  target() { return $("fwTarget").value; },

  async loadRecent() {
    const r = await attempt("recent files", () => api("/api/firmware/recent"));
    if (!r) return;
    const list = r[this.target()] || [];
    $("fwRecent").innerHTML = list.length ? list.map((p) => `<option>${esc(p)}</option>`).join("") : `<option value="">(none yet)</option>`;
  },

  async inspect(opts) {
    let res;
    if (opts.file) {
      const form = new FormData();
      form.append("file", opts.file);
      form.append("target", this.target());
      res = await attempt("load file", () => api("/api/firmware/inspect", {form}));
    } else {
      res = await attempt("load file", () => api("/api/firmware/inspect", {body: {path: opts.path, target: this.target()}}));
    }
    if (!res) return null;
    this.show(res);
    this.loadRecent();
    return res;
  },

  show(res) {
    const i = res.info;
    S.fw.token = res.token;
    S.fw.info = i;
    const rev = (x) => (x / 100).toFixed(2);
    let h = `<div class="fw-bold fs-3 text-body">${esc(i.project || "?")} <span class="badge bg-blue-lt ms-1">${esc(i.version)}</span></div>
      <div>built ${esc(i.date)} ${esc(i.time)} · ${esc(i.idf)}</div>
      <div>chip ${esc(i.chip)}, revision ${rev(i.min_rev)}–${rev(i.max_rev)}, ${i.flash_mb} MB flash, ${i.size.toLocaleString()} bytes,
        devlink marker ${i.marker ? "API " + i.api_version : "<b>missing</b>"}</div>
      <div class="small font-monospace">${esc(res.path)} · SHA-256 ${esc(i.sha256.slice(0, 16))}…</div>`;
    h += i.errors.map((e) => `<div class="text-danger mt-1"><i class="ti ti-circle-x me-1"></i>${esc(e)}</div>`).join("");
    h += i.warnings.map((w) => `<div class="text-warning mt-1"><i class="ti ti-alert-triangle me-1"></i>${esc(w)}</div>`).join("");
    if (!i.errors.length && !i.warnings.length) h += `<div class="text-success mt-1"><i class="ti ti-circle-check me-1"></i>Ready to upload</div>`;
    $("fwDetails").innerHTML = h;
    $("fwForce").checked = false;
    $("fwForceWrap").hidden = !(i.warnings.length && !i.errors.length);
  },

  renderBoards() {
    const lan = S.devices.filter((d) => d.on_lan);
    $("fwNoBoards").hidden = lan.length > 0;
    $("fwBoards").innerHTML = lan.map((d) => `<label class="list-group-item d-flex gap-2 align-items-center">
      <input class="form-check-input m-0" type="checkbox" value="${esc(d.mac)}" ${S.fw.checked.has(d.mac) ? "checked" : ""}>
      <span class="flex-fill"><span class="fw-medium">${esc(d.label || "(no name)")}</span>
      <span class="small text-secondary ms-1">${esc(d.mac)} · ${esc(d.ip)}</span></span>
      <span class="badge ${d.mode === "recovery" ? "bg-orange-lt" : "bg-green-lt"}">${esc(d.mode)}</span>
      <span class="small text-secondary">${esc(d.app)}</span></label>`).join("");
  },

  async start(macs = null, forceOverride = null) {
    macs = macs || [...S.fw.checked];
    const i = S.fw.info;
    if (!S.fw.token || !i || !macs.length) { toast("Load a firmware file and tick at least one board.", "warning"); return; }
    if (i.errors.length) { toast("This file cannot be uploaded: " + i.errors.join("; "), "danger"); return; }
    const force = forceOverride ?? $("fwForce").checked;
    if (i.warnings.length && !force) { toast(i.warnings.join("; ") + " — tick 'Upload anyway' to override.", "warning"); return; }
    if (i.warnings.length && forceOverride === null && !(await ask({title: "Upload anyway?", text: i.warnings.join("\n") + "\n\nUpload this file anyway?", ok: "Upload", danger: true}))) return;
    if (this.target() === "recovery" && forceOverride === null && !(await ask({title: "Recovery update", danger: true, ok: "Replace recovery",
      text: "Replace the RECOVERY image (the safety net) on the selected board(s)? Only from a valid, running app. The board then boots the new recovery once as a self-test and returns to the app. If the self-test fails, the board has no working recovery until you re-flash a good one."}))) return;
    S.fw.lastMacs = macs;
    const job = await attempt("update", () => api("/api/firmware/update", {body: {token: S.fw.token, macs, force}}));
    if (job) { S.fw.jobId = job.id; this.showJob(job); }
  },

  showJob(job) {
    $("fwJobTitle").textContent = job.title + (job.state === "running" ? " — running" : job.state === "done" ? " — finished" : " — failed");
    $("fwStart").disabled = job.state === "running";
    const name = (mac) => { const d = S.devices.find((x) => x.mac === mac); return d && d.label ? `${d.label} (${mac})` : mac; };
    $("fwResults").tBodies[0].innerHTML = Object.entries(job.boards).map(([mac, b]) => {
      const bad = ["rolled_back", "selftest_failed", "rejected", "failed", "timeout"].includes(b.state);
      const good = b.state === "updated";
      const pct = b.pct == null ? 0 : b.pct;
      return `<tr class="${bad ? "table-danger" : good ? "table-success" : ""}"><td>${esc(name(mac))}</td><td>${esc(b.phase)}</td>
        <td><div class="progress progress-sm"><div class="progress-bar ${bad ? "bg-danger" : good ? "bg-success" : ""}" style="width:${pct}%"></div></div>
        <span class="small text-secondary">${b.pct == null ? "" : pct + " %"}</span></td><td>${esc(b.message)}</td></tr>`;
    }).join("");
    if (job.state !== "running" && job.id === S.fw.jobId && !job._handled) {
      job._handled = true;
      this.finished(job);
    }
  },

  async finished(job) {
    const r = job.result;
    if (!r) return;
    const ok = r.results.length && r.results.every((x) => x.state === "updated");
    toast(ok ? "Update finished: all boards updated." : "Update finished with problems; see Results.", ok ? "success" : "warning");
    if (r.selftest_failed && r.previous_recovery) {
      if (await ask({title: "Recovery self-test failed", danger: true, ok: "Re-flash previous",
        text: `The new recovery failed its self-test. Re-flash the previous recovery file?\n${r.previous_recovery}`})) {
        if (await this.inspect({path: r.previous_recovery})) this.start(S.fw.lastMacs, true);
      }
    } else if (r.selftest_failed) {
      toast("The new recovery failed its self-test. Load a known-good recovery file and update again.", "danger");
    }
  },
};

$("fwTarget").onchange = () => { S.fw.token = null; S.fw.info = null; $("fwDetails").textContent = Fw.target() === "recovery" ? "Load a recovery image (devlink_recovery*.bin)." : "No file loaded."; $("fwForceWrap").hidden = true; Fw.loadRecent(); };
$("fwRecentLoad").onclick = () => { if ($("fwRecent").value) Fw.inspect({path: $("fwRecent").value}); };
$("fwPathLoad").onclick = () => { if ($("fwPath").value.trim()) Fw.inspect({path: $("fwPath").value.trim()}); };
$("fwPath").onkeydown = (e) => { if (e.key === "Enter") $("fwPathLoad").click(); };
$("fwFile").onchange = () => { if ($("fwFile").files[0]) Fw.inspect({file: $("fwFile").files[0]}); $("fwFile").value = ""; };
$("fwBoards").onchange = (e) => { if (e.target.checked) S.fw.checked.add(e.target.value); else S.fw.checked.delete(e.target.value); };
$("fwStart").onclick = () => Fw.start();

// ---------------------------------------------------------------- Monitor
const LEVELS = ["V", "D", "I", "W", "E"];
const WINDOW_S = 30;
const MAX_POINTS = 20000;
const MAX_BLOCK_POINTS = 30000;  // per block-channel field: 30 s at 1 kHz

/** One binary WebSocket message of a block channel (SPEC §5.7; layout as dl_stream_core.h DL_BLOCK_*), or null. */
function decodeBlock(buf) {
  const d = new DataView(buf);
  if (buf.byteLength < 28 || d.getUint16(0, true) !== 0xB10C || d.getUint8(2) !== 1) return null;
  const type = d.getUint8(3), nf = d.getUint8(6), ns = d.getUint16(24, true), hlen = d.getUint16(26, true);
  const size = type === 0 ? 2 : 4;
  if (type > 2 || nf === 0 || hlen !== 28 + 4 * nf || buf.byteLength !== hlen + ns * nf * size) return null;
  const scale = [];
  for (let f = 0; f < nf; f++) scale.push(d.getFloat32(28 + 4 * f, true));
  const get = type === 0 ? (o) => d.getInt16(o, true) : type === 1 ? (o) => d.getInt32(o, true) : (o) => d.getFloat32(o, true);
  const values = [];  // values[field][sample]
  for (let f = 0; f < nf; f++) {
    const col = new Array(ns);
    for (let i = 0; i < ns; i++) col[i] = get(hlen + (i * nf + f) * size) * scale[f];
    values.push(col);
  }
  return {id: d.getUint16(4, true), flags: d.getUint8(7), seq: d.getUint32(8, true),
    t0_us: Number(d.getBigUint64(12, true)), period_ns: d.getUint32(20, true), values};
}

/** Field names of a block channel from /channels: a list, or a comma string (older boards). */
function blockFields(c) {
  const f = Array.isArray(c.fields) ? c.fields : String(c.fields || "").split(",").filter((x) => x);
  const n = c.n_fields || f.length || 1;
  return f.length === n ? f : Array.from({length: n}, (_, i) => `f${i}`);
}

const Mon = {
  sess: null,            // {mac, ip, ws, closed, state, offset}
  channels: [],
  subs: new Map(),       // channel -> rate
  points: new Map(),     // series key -> {t: [], v: []}
  values: new Map(),     // channel -> latest value
  bstat: new Map(),      // block channel -> {n: samples since the last rate update, rate, lost, last: seq}
  logLines: [],
  mode: "follow",        // follow / all / free
  t0: performance.now(),
  plot: null,
  plotKeys: "",
  dirty: false,

  fillBoards() {
    const sel = $("monBoard").value;
    const lan = S.devices.filter((d) => d.on_lan);
    $("monBoard").innerHTML = lan.length ? lan.map((d) => `<option value="${esc(d.mac)}">${esc(d.label || "(no name)")} · ${esc(d.mac)} · ${esc(d.ip)} (${esc(d.mode)})</option>`).join("")
      : `<option value="">no boards on the network</option>`;
    if (sel && lan.some((d) => d.mac === sel)) $("monBoard").value = sel;
    else if (this.sess) $("monBoard").value = this.sess.mac;
  },

  setState(text, kind = "secondary") {
    $("monState").className = `badge bg-${kind}-lt`;
    $("monState").textContent = text;
  },

  async connect() {
    if (this.sess) { this.disconnect(); return; }
    const mac = $("monBoard").value;
    const d = S.devices.find((x) => x.mac === mac);
    if (!d) return;
    this.setState("loading channels…");
    const ch = await attempt("channels", () => api(`/api/dev/${mac}/channels`));
    if (!ch) { this.setState("not connected"); return; }
    this.channels = (ch.channels || []).filter((c) => c.name !== "log");
    this.subs = new Map([["log", null]]);
    this.points.clear(); this.values.clear(); this.bstat.clear(); this.logLines = []; $("logBox").textContent = "";
    this.blockById = new Map(this.channels.filter((c) => c.type === "block").map((c) => [c.id, c]));
    this.t0 = performance.now();
    this.mode = "follow"; this.markMode();
    this.renderChannels();
    this.rebuild();
    this.sess = {mac, ip: d.ip, ws: null, closed: false, state: "connecting", offset: null, reconnects: 0};
    $("monConnect").innerHTML = `<i class="ti ti-plug-connected-x me-1"></i>Disconnect`;
    $("monConnect").className = "btn btn-outline-danger";
    this.open();
    this.statusTimer = setInterval(() => { this.pollStatus(); this.watchdog(); }, 2000);
    this.pollStatus();
  },

  disconnect() {
    const s = this.sess;
    this.sess = null;
    if (s) { s.closed = true; if (s.ws) s.ws.close(); }
    clearInterval(this.statusTimer);
    clearTimeout(this.retryTimer);
    $("monConnect").innerHTML = `<i class="ti ti-plug-connected me-1"></i>Connect`;
    $("monConnect").className = "btn btn-primary";
    this.setState("not connected");
    $("monStatus").textContent = "";
  },

  open() {
    const s = this.sess;
    if (!s || s.closed) return;
    let hello = false;
    const ws = new WebSocket(`ws://${s.ip}/ws`);
    ws.binaryType = "arraybuffer";  // block channels
    s.ws = ws;
    s.lastRx = performance.now();
    s.probeAt = null;
    ws.onmessage = (ev) => {
      s.lastRx = performance.now();
      s.probeAt = null;
      if (ev.data instanceof ArrayBuffer) { this.block(ev.data); return; }
      for (const line of String(ev.data).split("\n")) {
        if (!line.trim()) continue;
        let f;
        try { f = JSON.parse(line); } catch (e) { continue; }
        if (!hello) {
          hello = true;
          if (f.error === "too_many_clients") {
            if (!s.reconnects) { s.state = "refused"; this.setState("refused: the board already has 2 clients", "red"); ws.close(); return; }
            // reconnecting: the board may still hold the dead socket's slot until it notices; try again
            ws.close(); return;
          }
          s.state = "connected"; s.offset = null;
          for (const b of this.bstat.values()) b.last = null;  // a new connection starts every channel at seq 0
          this.setState(s.reconnects ? `connected (reconnected ${s.reconnects}×)` : "connected", "green");
          for (const [ch, hz] of this.subs) this.send({op: "sub", ch: [ch]}), hz != null && this.send({op: "rate", ch, hz});
        }
        if ("ch" in f) this.frame(f);
      }
    };
    ws.onclose = () => this.lost(ws);
  },

  /** The connection is gone (closed, or found dead by the watchdog): try again in 3 s. */
  lost(ws) {
    const s = this.sess;
    if (!s || s.closed || s.state === "refused" || s.ws !== ws) return;
    ws.onclose = null; ws.onmessage = null;
    try { ws.close(); } catch (e) { /* already closed */ }
    s.ws = null;
    s.state = "reconnecting"; s.reconnects++;
    this.setState(`reconnecting… (${s.reconnects})`, "yellow");
    clearTimeout(this.retryTimer);
    this.retryTimer = setTimeout(() => this.reopen(), 3000);
  },

  /** A board that resets or drops off Wi-Fi leaves the browser's socket "open" with no close event (browsers send no
   *  WebSocket pings). After 4 s of silence send an op the board always answers (an unknown op gets an error
   *  reply); no answer within 4 s more means the connection is dead. */
  watchdog() {
    const s = this.sess;
    if (!s || !s.ws || s.ws.readyState !== 1) return;
    const now = performance.now();
    if (s.probeAt !== null && now - s.probeAt > 4000) this.lost(s.ws);
    else if (s.probeAt === null && now - s.lastRx > 4000) { s.probeAt = now; this.send({op: "ping"}); }
  },

  async reopen() {
    const s = this.sess;
    if (!s || s.closed) return;
    try {  // the board may have a new IP after a reboot: the server keeps looking for it by MAC
      const list = await api("/api/devices");
      const d = list.find((x) => x.mac === s.mac && x.on_lan);
      if (d) s.ip = d.ip;
    } catch (e) { /* server busy; try the old address */ }
    this.open();
  },

  send(op) { const s = this.sess; if (s && s.ws && s.ws.readyState === 1) s.ws.send(JSON.stringify(op)); },

  subscribe(ch, on, hz) {
    if (on) { this.subs.set(ch, hz); this.send({op: "sub", ch: [ch]}); if (hz != null) this.send({op: "rate", ch, hz}); }
    else {
      this.subs.delete(ch); this.send({op: "unsub", ch: [ch]});
      for (const k of [...this.points.keys()]) if (k === ch || k.startsWith(ch + "[")) this.points.delete(k);
      this.bstat.delete(ch); this.values.delete(ch);
      this.dirty = true;
    }
  },

  renderChannels() {
    $("monNoChannels").hidden = this.channels.length > 0;
    $("monChannels").tBodies[0].innerHTML = this.channels.map((c) => {
      const blk = c.type === "block";
      return `<tr data-ch="${esc(c.name)}">
      <td class="fw-medium">${esc(c.name)}${blk ? ` <span class="badge bg-azure-lt" title="binary blocks: ${esc(blockFields(c).join(", "))}">block</span>` : ""}</td>
      <td class="text-secondary">${esc(c.unit || "")}</td>
      <td><input class="form-check-input m-0" type="checkbox" data-sub></td>
      <td><input class="form-control form-control-sm" type="number" min="${blk ? 1 : 0}" max="${c.max_hz || 1000}" value="${c.default_hz || (blk ? 1 : 0)}"
        title="${blk ? "sample rate: the board runs at the highest rate any client asks for" : "0 = every value"}" data-hz></td>
      <td class="val"></td></tr>`;
    }).join("");
  },

  /** A block of a block channel: one point per sample and field, on the board's sample clock. */
  block(buf) {
    const b = decodeBlock(buf);
    const c = b && this.blockById && this.blockById.get(b.id);
    if (!c || !this.subs.has(c.name) || !b.values[0].length) return;
    const s = this.sess;
    let st = this.bstat.get(c.name);
    if (!st) { st = {n: 0, rate: 0, lost: 0, last: null, since: performance.now()}; this.bstat.set(c.name, st); }
    if (st.last !== null) { const miss = (b.seq - st.last - 1) >>> 0; if (miss < 0x80000000) st.lost += miss; }
    st.last = b.seq;
    const arrive = (performance.now() - this.t0) / 1000;
    const t0 = b.t0_us / 1e6, dt = b.period_ns / 1e9, n = b.values[0].length;
    if (s.offset === null || Math.abs(t0 + s.offset - arrive) > 5) s.offset = arrive - (t0 + (n - 1) * dt);
    const names = blockFields(c);
    b.values.forEach((col, f) => {
      const k = `${c.name}[${names[f] || f}]`;
      let p = this.points.get(k);
      if (!p) { p = {t: [], v: []}; this.points.set(k, p); }
      for (let i = 0; i < n; i++) { p.t.push(t0 + i * dt + s.offset); p.v.push(col[i]); }
      if (p.t.length > MAX_BLOCK_POINTS + 5000) { p.t.splice(0, p.t.length - MAX_BLOCK_POINTS); p.v.splice(0, p.v.length - MAX_BLOCK_POINTS); }
    });
    st.n += n;
    const now = performance.now();
    if (now - st.since >= 1000) { st.rate = st.n * 1000 / (now - st.since); st.n = 0; st.since = now; }
    this.values.set(c.name, `${Math.round(st.rate)} samples/s · lost ${st.lost} blocks`);
    this.dirty = true;
  },

  frame(f) {
    if (f.ch === "log") { this.log(f); return; }
    const s = this.sess;
    const arrive = (performance.now() - this.t0) / 1000;
    let t = arrive;
    if (typeof f.ts === "number") {  // board time keeps the spacing of batched samples; re-anchored per connection
      if (s.offset === null || Math.abs(f.ts / 1e6 + s.offset - arrive) > 5) s.offset = arrive - f.ts / 1e6;
      t = f.ts / 1e6 + s.offset;
    }
    this.values.set(f.ch, f.v);
    const vals = typeof f.v === "number" ? [[f.ch, f.v]] : Array.isArray(f.v) ? f.v.map((x, i) => [`${f.ch}[${i}]`, x]).filter(([, x]) => typeof x === "number") : [];
    for (const [k, x] of vals) {
      let p = this.points.get(k);
      if (!p) { p = {t: [], v: []}; this.points.set(k, p); }
      p.t.push(t); p.v.push(x);
      if (p.t.length > MAX_POINTS + 2000) { p.t.splice(0, p.t.length - MAX_POINTS); p.v.splice(0, p.v.length - MAX_POINTS); }
    }
    this.dirty = true;
  },

  log(f) {
    const line = `${((f.ts || 0) / 1e6).toFixed(3).padStart(10)} ${f.lvl || "I"} ${f.tag || ""}: ${f.msg || ""}`;
    this.logLines.push([f.lvl || "I", line]);
    if (this.logLines.length > 20000) this.logLines.splice(0, 5000);
    if (!$("logPause").checked && this.logOk(f.lvl || "I", line)) this.appendLog([[f.lvl || "I", line]]);
  },

  logOk(lvl, line) {
    const min = LEVELS.indexOf($("logLevel").value);
    const txt = $("logFilter").value.trim().toLowerCase();
    return (LEVELS.includes(lvl) ? LEVELS.indexOf(lvl) : 2) >= min && (!txt || line.toLowerCase().includes(txt));
  },

  appendLog(items) {
    const box = $("logBox");
    const atEnd = box.scrollTop + box.clientHeight >= box.scrollHeight - 4;
    const frag = document.createDocumentFragment();
    for (const [lvl, line] of items) { const sp = document.createElement("span"); sp.className = `log-${lvl}`; sp.textContent = line + "\n"; frag.appendChild(sp); }
    box.appendChild(frag);
    while (box.childNodes.length > 5000) box.removeChild(box.firstChild);
    if (atEnd) box.scrollTop = box.scrollHeight;
  },

  rerenderLog() { $("logBox").textContent = ""; this.appendLog(this.logLines.filter(([l, s]) => this.logOk(l, s)).slice(-5000)); },

  async pollStatus() {
    const s = this.sess;
    if (!s) return;
    try {
      const st = await api(`/api/dev/${s.mac}/status`);
      if (s.uptime != null && st.uptime_ms < s.uptime && s.ws) this.lost(s.ws);  // the board restarted
      s.uptime = st.uptime_ms;
      const w = st.wifi || {};
      $("monStatus").textContent = `uptime ${Math.floor(st.uptime_ms / 1000)} s · heap ${Math.floor(st.heap_free / 1024)} KiB · ` +
        `RSSI ${w.sta_connected ? w.rssi : "AP"} · drops ${st.drops || 0} · temp ${st.temp} · error ${st.last_error || "-"}`;
    } catch (e) { $("monStatus").textContent = "status: no answer"; }
  },

  // ---- plot ----
  colors: ["#206bc4", "#d63939", "#2fb344", "#f76707", "#ae3ec9", "#17a2b8", "#f59f00", "#74b816", "#4263eb", "#d6336c"],

  rebuild() {
    if (this.plot) { this.plot.destroy(); this.plot = null; }
    this.plotKeys = "";
    this.dirty = true;
  },

  resize() { if (this.plot) this.plot.setSize({width: $("plot").clientWidth, height: 340}); },

  make(keys) {
    const fg = getComputedStyle(document.body).color;
    const grid = document.documentElement.dataset.bsTheme === "dark" ? "rgba(255,255,255,.08)" : "rgba(0,0,0,.07)";
    const axis = {stroke: fg, grid: {stroke: grid}, ticks: {stroke: grid}};
    const opts = {
      width: $("plot").clientWidth || 800, height: 340,
      scales: {x: {time: false}},
      axes: [{...axis, label: "s"}, {...axis}],
      cursor: {drag: {x: true, y: false}, points: {show: false}},  // own dots in tip()
      legend: {live: false},  // values under the mouse: see tip() (the joined data has gaps per series)
      series: [{label: "t (s)", value: (u, v) => v == null ? "-" : v.toFixed(2)},
        ...keys.map((k, i) => ({label: k, stroke: this.colors[i % this.colors.length], width: 1.5, spanGaps: true,
          value: (u, v) => v == null ? "-" : (+v.toPrecision(4)).toString()}))],
      hooks: {
        setSelect: [(u) => { if (u.select.width > 0) { this.mode = "free"; this.markMode(); } }],
        ready: [(u) => u.over.addEventListener("dblclick", () => { this.mode = "all"; this.markMode(); this.dirty = true; })],
        init: [(u) => {
          this.tipEl = document.createElement("div");
          this.tipEl.className = "plot-tip";
          this.tipEl.hidden = true;
          u.over.appendChild(this.tipEl);
          this.dots = keys.map((k, i) => {
            const d = document.createElement("div");
            d.className = "plot-dot";
            d.style.background = this.colors[i % this.colors.length];
            d.hidden = true;
            u.over.appendChild(d);
            return d;
          });
        }],
        setCursor: [(u) => this.tip(u)],
      },
    };
    $("plot").innerHTML = "";
    this.tipEl = null;
    this.plot = new uPlot(opts, [[], ...keys.map(() => [])], $("plot"));
    this.plotKeys = keys.join("|");
  },

  draw() {
    if (!this.dirty || $("plot").offsetParent === null) return;  // hidden tab: draw when shown
    this.dirty = false;
    for (const [ch, v] of this.values) {
      const td = document.querySelector(`#monChannels tr[data-ch="${CSS.escape(ch)}"] td.val`);
      if (td) td.textContent = typeof v === "number" ? (+v.toPrecision(5)).toString() : typeof v === "string" && this.bstat.has(ch) ? v : JSON.stringify(v);
    }
    const keys = [...this.points.keys()];
    if (keys.join("|") !== this.plotKeys || !this.plot) this.make(keys);
    if (!keys.length) return;
    const data = uPlot.join(keys.map((k) => [this.points.get(k).t, this.points.get(k).v]));
    const now = (performance.now() - this.t0) / 1000;
    if (this.mode === "follow") {
      this.plot.setData(data, false);
      this.plot.setScale("x", {min: Math.max(0, now - WINDOW_S), max: Math.max(WINDOW_S, now)});
    } else {
      this.plot.setData(data, this.mode === "all");
    }
    if (this.tipEl && !this.tipEl.hidden) this.tip(this.plot);  // values move under a still mouse
  },

  /** Tooltip at the mouse: the time under it and, per series, the nearest sample (value, and a dot on the curve).
   *  Looked up in each series' own points, because the aligned plot data has a gap for every other series. */
  tip(u) {
    const el = this.tipEl;
    if (!el) return;
    const {left, top} = u.cursor;
    const keys = this.plotKeys ? this.plotKeys.split("|") : [];
    if (left == null || left < 0 || !keys.length) { el.hidden = true; this.dots.forEach((d) => { d.hidden = true; }); return; }
    const t = u.posToVal(left, "x");
    const rows = [`<div class="text-secondary">t = ${t.toFixed(2)} s</div>`];
    keys.forEach((k, i) => {
      const p = this.points.get(k);
      const dot = this.dots[i];
      if (!p || !p.t.length) { if (dot) dot.hidden = true; return; }
      let lo = 0, hi = p.t.length - 1;
      while (lo < hi) { const mid = (lo + hi) >> 1; if (p.t[mid] < t) lo = mid + 1; else hi = mid; }
      if (lo > 0 && Math.abs(p.t[lo - 1] - t) < Math.abs(p.t[lo] - t)) lo--;
      const v = p.v[lo];
      const color = this.colors[i % this.colors.length];
      rows.push(`<div><span class="plot-swatch" style="background:${color}"></span>${esc(k)} <b>${(+v.toPrecision(5)).toString()}</b>` +
        (Math.abs(p.t[lo] - t) > 0.5 ? ` <span class="text-secondary">@ ${p.t[lo].toFixed(2)} s</span>` : "") + `</div>`);
      if (dot) {
        const x = u.valToPos(p.t[lo], "x"), y = u.valToPos(v, "y");
        dot.hidden = !(x >= 0 && x <= u.over.clientWidth && y >= 0 && y <= u.over.clientHeight);
        dot.style.left = `${x}px`; dot.style.top = `${y}px`;
      }
    });
    el.innerHTML = rows.join("");
    el.hidden = false;
    const w = el.offsetWidth, h = el.offsetHeight;  // keep it inside the plot: flip left / up near the edges
    el.style.left = `${left + 14 + w > u.over.clientWidth ? left - 14 - w : left + 14}px`;
    el.style.top = `${Math.max(0, Math.min(top + 14, u.over.clientHeight - h))}px`;
  },

  markMode() {
    $("plotFollow").classList.toggle("active", this.mode === "follow");
    $("plotAll").classList.toggle("active", this.mode === "all");
  },
};

$("monConnect").onclick = () => Mon.connect();
$("plotFollow").onclick = () => { Mon.mode = "follow"; Mon.markMode(); Mon.dirty = true; };
$("plotAll").onclick = () => { Mon.mode = "all"; Mon.markMode(); Mon.dirty = true; };
$("monChannels").addEventListener("change", (e) => {
  const tr = e.target.closest("tr[data-ch]");
  if (!tr) return;
  const ch = tr.dataset.ch;
  const hz = +tr.querySelector("[data-hz]").value;
  if (e.target.matches("[data-sub]")) Mon.subscribe(ch, e.target.checked, hz);
  else if (e.target.matches("[data-hz]") && Mon.subs.has(ch)) { Mon.subs.set(ch, hz); Mon.send({op: "rate", ch, hz}); }
});
$("logLevel").onchange = () => Mon.rerenderLog();
$("logFilter").oninput = () => Mon.rerenderLog();
$("logPause").onchange = () => { if (!$("logPause").checked) Mon.rerenderLog(); };
$("logClear").onclick = () => { Mon.logLines = []; $("logBox").textContent = ""; };
$("logSave").onclick = () => {
  const stamp = new Date().toISOString().replace(/[:T]/g, "-").slice(0, 19);
  saveBlob(new Blob([Mon.logLines.map(([, l]) => l).join("\n") + "\n"], {type: "text/plain"}), `devlink_log_${stamp}.txt`);
};
window.addEventListener("resize", () => Mon.resize());
setInterval(() => Mon.draw(), 200);

// ---------------------------------------------------------------- settings
$("settingsBtn").onclick = async () => {
  const s = await attempt("settings", () => api("/api/settings"));
  if (!s) return;
  $("setKey").value = s.api_key; $("setApPw").value = s.ap_password; $("setSsid").value = s.bench_ssid;
  $("setDisc").value = s.discovery_s; $("setAps").value = s.ap_scan_s; $("setDir").textContent = s.data_dir;
  modal("setModal").show();
};
$("setSave").onclick = async () => {
  const s = await attempt("save settings", () => api("/api/settings", {body: {
    api_key: $("setKey").value, ap_password: $("setApPw").value, bench_ssid: $("setSsid").value,
    discovery_s: +$("setDisc").value, ap_scan_s: +$("setAps").value}}));
  if (s) { S.settings = s; modal("setModal").hide(); toast("Settings saved", "success"); }
};

// ---------------------------------------------------------------- start
setTheme(document.documentElement.dataset.bsTheme === "dark" ? "dark" : "light");
showTab(["devices", "firmware", "monitor"].includes(store.get("devlink_tab")) ? store.get("devlink_tab") : "devices");
Fw.loadRecent();
connectEvents();
