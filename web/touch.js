"use strict";
const $ = (id) => document.getElementById(id);
let token = null;
async function api(path, method = "GET", body, keepalive = false) {
  if (method !== "GET" && !token) {
    const session = await api("/session");
    token = session.token;
    if (typeof token !== "string") throw Error("服务器未返回会话令牌");
  }
  const headers = {};
  if (method !== "GET") {
    headers["X-UC-Token"] = token;
    headers["Content-Type"] = "application/json";
  }
  const controller = keepalive ? null : new AbortController();
  const timeout = controller ? setTimeout(() => controller.abort(), 4000) : null;
  let res, text;
  try {
    res = await fetch("/api/v1" + path, {
      method,
      headers,
      body: body === undefined ? undefined : JSON.stringify(body),
      keepalive,
      signal: controller ? controller.signal : undefined,
    });
    text = await res.text();
  } finally {
    if (timeout !== null) clearTimeout(timeout);
  }
  let data;
  try {
    data = text ? JSON.parse(text) : {};
  } catch {
    throw Error("服务器返回无效 JSON（HTTP " + res.status + "）");
  }
  if (!res.ok) {
    const error = Error("HTTP " + res.status + " · " + (data.error || data.message || text));
    error.httpStatus = res.status;
    throw error;
  }
  return data;
}
let enabling = false;
let enableEpoch = 0;
let pendingId = null;
function removeOutput(id, keepalive = false) {
  if (!Number.isSafeInteger(id) || id <= 0) return Promise.resolve();
  return api("/test-controller", "DELETE", { session_id: id }, keepalive);
}
async function enableOutput(operation) {
  const started = Date.now();
  let id = null;
  try {
    while (operation === enableEpoch) {
      const body = id ? { user_id: 0, session_id: id } : { user_id: 0 };
      const result = await api("/test-controller", "POST", body);
      id = result.session_id;
      if (!Number.isSafeInteger(id) || id <= 0) throw Error("服务器会话编号无效");
      if (operation !== enableEpoch) break;
      if (!result.pending) return result;
      pendingId = id;
      if (Date.now() - started > 10000) throw Error("初始化仍未完成，请稍后重试");
      status("正在初始化虚拟输出…");
      await new Promise((resolve) => setTimeout(resolve, 300));
    }
    throw Error("启用已取消");
  } catch (error) {
    await removeOutput(id).catch(() => {});
    throw error;
  }
}
const touchMessages = {
  服务器未返回会话令牌: "No session token returned",
  "服务器返回无效 JSON（HTTP ": "Invalid JSON (HTTP ",
  "初始化仍未完成，请稍后重试": "Initialization not complete; retry later",
  "正在初始化虚拟输出…": "Initializing virtual output…",
  启用已取消: "Enable cancelled",
  "已连接 · 输入正常": "Connected · input received",
  "输入失败，已停止发送：": "Input failed; sending stopped: ",
  "未创建 · 创建后在 PS5 上选择使用者": "Ready · create a controller, then choose a user on PS5",
  "页面已隐藏，输出已关闭": "Page hidden; output disabled",
  服务器会话编号无效: "Invalid server session ID",
  "启用失败：": "Enable failed: ",
  输出已关闭: "Output disabled",
  "关闭失败，请重试：": "Disable failed; retry: ",
  已释放全部控制: "All controls released",
  "页面已隐藏 · 已释放，返回后请重新启用": "Page hidden · input released; enable again on return",
  "网络已断开 · 输入停止；服务器将在 500ms 内释放":
    "Network offline · input stopped; server releases within 500ms",
  "正在同步手柄状态…": "Synchronizing controller state…",
  "模拟手柄已启用 · 可停用后重新启用":
    "Test controller registered · disable it before enabling here",
  "模拟手柄正在初始化 · 可取消": "Test controller initializing · you can cancel",
  "蓝牙手柄使用中 · 请先关闭蓝牙": "Bluetooth input active · stop Bluetooth first",
  "USB 手柄使用中 · 请先关闭 USB": "USB input active · stop USB first",
  "状态同步失败：": "State synchronization failed: ",
  专注操控: "Focus mode",
  退出专注: "Exit focus mode",
};
function translateTouch(message) {
  if (!window.UCUI || UCUI.get().lang === "zh") return message;
  for (const key of Object.keys(touchMessages).sort((a, b) => b.length - a.length))
    message = message.split(key).join(touchMessages[key]);
  return message;
}
let lastStatus = "正在同步手柄状态…";
function status(message, error = false) {
  lastStatus = message;
  $("status").textContent = translateTouch(message);
  $("status").classList.toggle("error", error);
}
window.addEventListener("uc-language", () => {
  $("status").textContent = translateTouch(lastStatus);
  $("focus").textContent = translateTouch(
    document.body.classList.contains("focus-mode") ? "退出专注" : "专注操控",
  );
});
let stateReady = false,
  statePending = false,
  externalSource = null,
  syncing = false,
  disabling = false;
let session = null,
  sequence = 0,
  inflight = false,
  dirty = false,
  timer = null,
  active = false;
let axes = [0, 0, 0, 0];
const held = new Map();
const stickPointers = new Map();
let epoch = 0;
function frame() {
  let buttons = 0;
  for (const bit of held.values()) buttons = (buttons | bit) >>> 0;
  return {
    session_id: session,
    sequence: ++sequence,
    buttons,
    axes: [...axes],
    triggers: [buttons & 256 ? 65535 : 0, buttons & 512 ? 65535 : 0],
  };
}
function render() {
  for (const b of document.querySelectorAll(".padbutton")) {
    const pressed = [...held.values()].includes(Number(b.dataset.bit));
    b.classList.toggle("pressed", pressed);
    b.disabled = !active;
    b.setAttribute("aria-pressed", String(pressed));
  }
  for (const id of ["left", "right"]) $(id).setAttribute("aria-disabled", String(!active));
  $("telemetry").textContent = JSON.stringify(
    {
      active,
      session_id: session,
      buttons: [...held.values()].reduce((a, b) => (a | b) >>> 0, 0),
      axes,
    },
    null,
    2,
  );
  $("enable").disabled =
    !stateReady || !!externalSource || session !== null || statePending || enabling || disabling;
  $("disable").disabled =
    disabling || !!externalSource || (session === null && !statePending && !enabling);
  $("release").disabled = !active;
}
function reset() {
  held.clear();
  stickPointers.clear();
  axes = [0, 0, 0, 0];
  for (const e of document.querySelectorAll(".stick span")) e.style.transform = "translate(0,0)";
  render();
}
function stop() {
  active = false;
  clearInterval(timer);
  timer = null;
  reset();
}
async function pump() {
  if (!active || !session || inflight || document.hidden) return;
  inflight = true;
  dirty = false;
  const sentEpoch = epoch;
  const body = frame();
  try {
    await api("/test-controller/input", "POST", body);
    if (sentEpoch === epoch) status("已连接 · 输入正常");
  } catch (e) {
    if (sentEpoch === epoch) {
      stop();
      status("输入失败，已停止发送：" + e.message, true);
    }
  } finally {
    inflight = false;
    if (dirty && active && sentEpoch === epoch) pump();
  }
}
function changed() {
  dirty = true;
  render();
  pump();
}
function releaseAll(keepalive = false) {
  reset();
  if (session && token) {
    const body = frame();
    if (keepalive) {
      api("/test-controller/input", "POST", body, true).catch(() => {});
    } else {
      dirty = true;
      pump();
    }
  }
}
// Observe registration separately from this page's input pump. Reloading a page
// must not silently adopt another sender's sequence or leave its output invisible.
async function syncState() {
  if (syncing || enabling || disabling || document.hidden) return;
  syncing = true;
  const revision = enableEpoch;
  try {
    const data = await api("/test-controller");
    if (revision !== enableEpoch || enabling || disabling) return;
    if (
      typeof data.enabled !== "boolean" ||
      (data.enabled && !Number.isSafeInteger(data.session_id))
    )
      throw Error("服务器会话编号无效");
    stateReady = true;
    statePending = !!data.pending;
    externalSource = data.source === "bluetooth" || data.source === "usb" ? data.source : null;
    const serverSession = data.enabled ? data.session_id : null;
    if (active && serverSession !== session) {
      epoch++;
      stop();
    }
    session = serverSession;
    pendingId = statePending ? data.pending_id : null;
    if (!active) {
      status(
        externalSource === "bluetooth"
          ? "蓝牙手柄使用中 · 请先关闭蓝牙"
          : externalSource === "usb"
            ? "USB 手柄使用中 · 请先关闭 USB"
            : statePending
              ? "模拟手柄正在初始化 · 可取消"
              : session !== null
                ? "模拟手柄已启用 · 可停用后重新启用"
                : "未创建 · 创建后在 PS5 上选择使用者",
      );
    }
  } catch (e) {
    if (revision === enableEpoch && !enabling && !disabling) {
      stateReady = false;
      status("状态同步失败：" + e.message, true);
    }
  } finally {
    syncing = false;
    render();
  }
}
$("enable").onclick = async () => {
  if (enabling || disabling || !stateReady || externalSource || session !== null || statePending)
    return;
  enabling = true;
  const operation = ++enableEpoch;
  render();
  try {
    const data = await enableOutput(operation);
    if (operation !== enableEpoch) return;
    if (document.hidden) {
      await removeOutput(data.session_id);
      throw Error("页面已隐藏，输出已关闭");
    }
    if (!Number.isSafeInteger(data.session_id)) throw Error("服务器会话编号无效");
    pendingId = null;
    session = data.session_id;
    sequence = 0;
    epoch++;
    reset();
    active = true;
    timer = setInterval(pump, 100);
    render();
    pump();
  } catch (e) {
    if (operation === enableEpoch) status("启用失败：" + e.message, true);
  } finally {
    enabling = false;
    render();
  }
};
$("disable").onclick = async () => {
  if (disabling || externalSource) return;
  ++enableEpoch;
  disabling = true;
  epoch++;
  stop();
  try {
    await removeOutput(session || pendingId);
    pendingId = null;
    session = null;
    statePending = false;
    render();
    status("输出已关闭");
  } catch (e) {
    status("关闭失败，请重试：" + e.message, true);
  } finally {
    disabling = false;
    render();
  }
};
$("release").onclick = () => {
  releaseAll();
  status("已释放全部控制");
};
for (const b of document.querySelectorAll(".padbutton")) {
  b.addEventListener("pointerdown", (e) => {
    if (!active) return;
    e.preventDefault();
    b.setPointerCapture(e.pointerId);
    held.set(e.pointerId, Number(b.dataset.bit));
    changed();
  });
  const end = (e) => {
    if (held.delete(e.pointerId)) changed();
  };
  const keyboardId = "keyboard:" + b.dataset.bit;
  b.addEventListener("keydown", (e) => {
    if (!active || (e.key !== " " && e.key !== "Enter")) return;
    e.preventDefault();
    if (e.repeat || held.has(keyboardId)) return;
    held.set(keyboardId, Number(b.dataset.bit));
    changed();
  });
  const releaseKeyboard = () => {
    if (held.delete(keyboardId)) changed();
  };
  b.addEventListener("keyup", (e) => {
    if (e.key === " " || e.key === "Enter") {
      e.preventDefault();
      releaseKeyboard();
    }
  });
  b.addEventListener("blur", releaseKeyboard);
  for (const type of ["pointerup", "pointercancel", "lostpointercapture"])
    b.addEventListener(type, end);
}
for (const [id, offset] of [
  ["left", 0],
  ["right", 2],
]) {
  const stick = $(id);
  function move(e) {
    if (stickPointers.get(id) !== e.pointerId) return;
    const r = stick.getBoundingClientRect();
    let x = (e.clientX - r.left - r.width / 2) / (r.width * 0.35),
      y = (e.clientY - r.top - r.height / 2) / (r.height * 0.35);
    const len = Math.hypot(x, y);
    if (len > 1) {
      x /= len;
      y /= len;
    }
    axes[offset] = Math.round(x * 32767);
    axes[offset + 1] = Math.round(y * 32767);
    stick.firstElementChild.style.transform =
      "translate(" + x * r.width * 0.3 + "px," + y * r.height * 0.3 + "px)";
    changed();
  }
  stick.addEventListener("pointerdown", (e) => {
    if (!active || stickPointers.has(id)) return;
    e.preventDefault();
    stickPointers.set(id, e.pointerId);
    stick.setPointerCapture(e.pointerId);
    move(e);
  });
  stick.addEventListener("pointermove", move);
  const end = (e) => {
    if (stickPointers.get(id) !== e.pointerId) return;
    stickPointers.delete(id);
    axes[offset] = axes[offset + 1] = 0;
    stick.firstElementChild.style.transform = "translate(0,0)";
    changed();
  };
  for (const type of ["pointerup", "pointercancel", "lostpointercapture"])
    stick.addEventListener(type, end);
}
function leaveController() {
  const id = active ? session : enabling ? pendingId : null;
  ++enableEpoch;
  epoch++;
  if (active) releaseAll(true);
  stop();
  removeOutput(id, true).catch(() => {});
}
document.addEventListener("visibilitychange", () => {
  if (document.hidden) {
    leaveController();
    status("页面已隐藏 · 已释放，返回后请重新启用");
  } else {
    syncState();
  }
});
window.addEventListener("pagehide", leaveController);
window.addEventListener("blur", () => {
  if (active) releaseAll();
});
window.addEventListener("offline", () => {
  leaveController();
  status("网络已断开 · 输入停止；服务器将在 500ms 内释放", true);
});
syncState();
render();
setInterval(syncState, 2000);
window.addEventListener("focus", syncState);

// A CSS focus view works on mobile browsers without a Fullscreen API.
function focusMode(enabled) {
  releaseAll();
  document.body.classList.toggle("focus-mode", enabled);
  $("focus").setAttribute("aria-pressed", String(enabled));
  $("focus").textContent = translateTouch(enabled ? "退出专注" : "专注操控");
}
$("focus").onclick = () => focusMode(!document.body.classList.contains("focus-mode"));
document.addEventListener("keydown", (event) => {
  if (event.key === "Escape" && document.body.classList.contains("focus-mode")) focusMode(false);
});
