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
async function enableOutput(user_id, operation) {
  const started = Date.now();
  try {
    while (operation === enableEpoch) {
      const result = await api("/test-controller", "POST", { user_id });
      if (operation !== enableEpoch) break;
      if (!result.pending) return result;
      if (Date.now() - started > 10000) throw Error("初始化仍未完成，请稍后重试");
      status("正在初始化虚拟输出…");
      await new Promise((resolve) => setTimeout(resolve, 300));
    }
  } catch (error) {
    // A timed-out request may still have created an output; cleanup after it settles.
    if (operation !== enableEpoch || error.httpStatus !== 409)
      await api("/test-controller", "DELETE").catch(() => {});
    throw error;
  }
  // A cancelled POST may succeed after the first DELETE; remove that late result.
  await api("/test-controller", "DELETE");
  throw Error("启用已取消");
}
function status(message, error = false) {
  $("status").textContent = message;
  $("status").classList.toggle("error", error);
}
function userList(data) {
  return Array.isArray(data) ? data : data.users || [];
}
function fillUsers(data) {
  $("user").replaceChildren();
  for (const u of userList(data)) {
    if (u.signed_in === false) continue;
    const id = u.user_id ?? u.id;
    const o = document.createElement("option");
    o.value = id;
    o.textContent = (u.display_name || u.name || "用户") + " · " + id;
    $("user").append(o);
  }
  if (!$("user").options.length) {
    const o = document.createElement("option");
    o.value = "";
    o.textContent = "没有已登录用户";
    $("user").append(o);
  }
}
let bluetooth = null;
let usb = null;
let usbBusy = false;
let usbEpoch = 0;
let sourcesReady = false;
let btBusy = false;
let btEpoch = 0;
let virtualSession = null;
let stableUsersKey = null;
let stableUsersSince = 0;
let usersReady = false;
function stableUser() {
  return usersReady && $("user").value !== "" && Date.now() - stableUsersSince >= 5000;
}
function btMessage(message, error = false) {
  $("bt-status").textContent = message;
  $("bt-status").classList.toggle("error", error);
}
function usbRunning() {
  return !!(usb && (["starting", "on"].includes(usb.state) || usb.pending || usb.bound));
}
function btRunning() {
  return !!(bluetooth && ["starting", "on"].includes(bluetooth.state));
}
function virtualRunning() {
  return !!(
    virtualSession &&
    (virtualSession.enabled || virtualSession.session_id || virtualSession.pending)
  );
}
function canStart(source) {
  if (!sourcesReady || !usb || !bluetooth || usbBusy || btBusy || enabling) return false;
  if (source === "test") return !usbRunning() && !btRunning();
  if (!stableUser() || virtualRunning()) return false;
  return source === "usb" ? !usbRunning() : !btRunning();
}
function updateActionButtons() {
  $("enable").disabled = !canStart("test");
  $("bt-start").disabled = !canStart("bluetooth");
  $("usb-start").disabled = !canStart("usb");
  $("bt-stop").disabled =
    btBusy || !bluetooth || !["starting", "on", "failed"].includes(bluetooth.state);
  $("usb-stop").disabled = usbBusy || !usb || !(usbRunning() || usb.state === "failed");
  $("bt-pair").disabled =
    btBusy || !bluetooth || bluetooth.state !== "on" || bluetooth.pairing_seconds > 0;
  const wait = Math.max(0, Math.ceil((5000 - (Date.now() - stableUsersSince)) / 1000));
  const gate =
    !usersReady || !$("user").value
      ? "等待可用的已登录账号。"
      : stableUser()
        ? "账号已稳定，可手动启动。"
        : "账号稳定检查：还需 " + wait + " 秒。";
  $("bt-user-gate").textContent = gate;
  $("usb-user-gate").textContent = gate;
}
function usbMessage(message, error = false) {
  $("usb-status").textContent = message;
  $("usb-status").classList.toggle("error", error);
}
function renderUsb(data) {
  if (
    !data ||
    !["off", "starting", "on", "failed"].includes(data.state) ||
    !Array.isArray(data.devices) ||
    typeof data.pending !== "boolean" ||
    typeof data.bound !== "boolean"
  )
    throw Error("USB 状态结构无效");
  usb = data;
  const labels = { off: "已关闭", starting: "正在启动", on: "运行中", failed: "USB 失败" };
  usbMessage(
    labels[data.state] +
      (data.pending ? " · 输出绑定中" : "") +
      (data.reason ? " · " + data.reason : "") +
      (data.state === "on" ? " · 账号 " + data.user_id : ""),
    data.state === "failed",
  );
  $("usb-devices").replaceChildren();
  for (const device of data.devices) {
    const card = addText($("usb-devices"), "article", "", "usb-device");
    addText(card, "strong", device.model || device.profile || "Generic HID");
    addText(card, "div", device.node, "mono");
    addText(card, "div", "VID/PID " + device.vid + "/" + device.pid);
    addText(card, "div", device.connected ? "已连接" : "已断开", "tag");
    addText(
      card,
      "div",
      "输入帧 " + device.input_frames + " · " + (device.bound ? "已绑定账号" : "未绑定"),
    );
    addAssignment(card, device, false);
  }
  if (!data.devices.length) addText($("usb-devices"), "p", "当前没有 USB 手柄。");
  updateActionButtons();
}
async function usbAction(path, body) {
  if (usbBusy || (path === "/start" && !canStart("usb"))) return;
  usbBusy = true;
  const operation = ++usbEpoch;
  updateActionButtons();
  try {
    await api(
      "/usb" + path,
      "POST",
      path === "/start" ? { user_id: Number($("user").value) } : body,
    );
    const latest = await api("/usb");
    if (operation === usbEpoch) renderUsb(latest);
  } catch (error) {
    if (operation === usbEpoch) {
      sourcesReady = false;
      usbMessage("USB 操作未确认：" + error.message, true);
    }
  } finally {
    if (operation === usbEpoch) {
      usbBusy = false;
      updateActionButtons();
    }
  }
}
$("usb-start").onclick = () => usbAction("/start");
$("usb-stop").onclick = () => usbAction("/stop");
function addText(parent, tag, text, className) {
  const node = document.createElement(tag);
  node.textContent = String(text);
  if (className) node.className = className;
  parent.append(node);
  return node;
}
function addAssignment(card, device, wireless) {
  addText(
    card,
    "div",
    "账号 " +
      (device.user_id > 0 ? device.user_id : "待选择") +
      (device.pending ? " · 绑定中" : ""),
  );
  const button = addText(card, "button", "绑定到上方所选账号");
  button.disabled = !stableUser() || btBusy || usbBusy;
  button.onclick = () => {
    const body = { slot: device.slot, user_id: Number($("user").value) };
    body[wireless ? "address" : "node"] = wireless ? device.address : device.node;
    return wireless ? bluetoothAction("/assign", body) : usbAction("/assign", body);
  };
}
function renderBluetooth(data) {
  if (
    !data ||
    !["off", "starting", "on", "failed"].includes(data.state) ||
    !Array.isArray(data.devices) ||
    !Array.isArray(data.paired)
  )
    throw Error("蓝牙状态结构无效");
  bluetooth = data;
  const labels = {
    off: "已关闭",
    starting: "正在启动",
    on: "运行中",
    failed: "启动失败",
  };
  btMessage(
    labels[data.state] +
      (data.reason ? " · " + data.reason : "") +
      (data.state === "on" ? " · 账号 " + data.user_id : ""),
    data.state === "failed",
  );
  $("bt-pairing").textContent =
    data.pairing_seconds > 0 ? "可发现 / 配对剩余 " + data.pairing_seconds + " 秒" : "配对未开启";
  $("bt-devices").replaceChildren();
  for (const device of data.devices) {
    const card = addText($("bt-devices"), "article", "", "bt-device");
    addText(card, "strong", device.model || device.profile || "Generic HID");
    addText(card, "div", device.connected ? "已连接" : "已断开", "tag");
    addText(card, "div", device.address, "mono");
    addText(
      card,
      "div",
      "VID/PID " + device.vid + "/" + device.pid + " · " + (device.profile || "Generic HID"),
    );
    addText(
      card,
      "div",
      "输入帧 " + device.input_frames + " · " + (device.bound ? "已绑定账号" : "未绑定"),
    );
    addAssignment(card, device, true);
    addText(card, "div", Number(device.battery) >= 0 ? "电量 " + device.battery + "%" : "电量未知");
  }
  if (!data.devices.length) addText($("bt-devices"), "p", "当前没有连接的蓝牙手柄。");
  $("bt-paired").replaceChildren();
  for (const device of data.paired) {
    const card = addText($("bt-paired"), "article", "", "bt-device");
    addText(card, "div", device.address, "mono");
    addText(
      card,
      "div",
      "VID/PID " +
        device.vid +
        "/" +
        device.pid +
        " · " +
        (device.le ? "LE" : "Classic") +
        " · " +
        (device.connected ? "已连接" : "未连接"),
    );
    const forget = addText(card, "button", "忘记设备", "danger");
    forget.disabled = btBusy;
    forget.onclick = () => bluetoothAction("/forget", { address: device.address });
  }
  if (!data.paired.length) addText($("bt-paired"), "p", "尚无已配对设备。");
  updateActionButtons();
}
async function bluetoothAction(path, body) {
  if (btBusy) return;
  if (path === "/start" && !canStart("bluetooth")) return;
  if (path === "/pair" && (!bluetooth || bluetooth.state !== "on" || bluetooth.pairing_seconds > 0))
    return;
  btBusy = true;
  const operation = ++btEpoch;
  updateActionButtons();
  for (const button of $("bt-paired").querySelectorAll("button")) button.disabled = true;
  let commandConfirmed = false;
  try {
    await api("/bluetooth" + path, "POST", body);
    commandConfirmed = true;
    if (operation !== btEpoch) return;
    const latest = await api("/bluetooth");
    if (operation === btEpoch) renderBluetooth(latest);
  } catch (error) {
    if (operation === btEpoch) {
      if (commandConfirmed || path === "/start" || path === "/stop") sourcesReady = false;
      btMessage("蓝牙操作未确认：" + error.message, true);
    }
  } finally {
    if (operation === btEpoch) {
      btBusy = false;
      updateActionButtons();
      for (const button of $("bt-paired").querySelectorAll("button")) button.disabled = false;
    }
  }
}
$("bt-start").onclick = () => bluetoothAction("/start", { user_id: Number($("user").value) });
$("bt-stop").onclick = () => bluetoothAction("/stop");
$("bt-pair").onclick = () => bluetoothAction("/pair", { seconds: 60 });
$("user").onchange = () => {
  stableUsersKey = null;
  usersReady = false;
  updateActionButtons();
  refresh();
};
let refreshing = false;
async function refresh() {
  if (refreshing) return;
  refreshing = true;
  const bluetoothOperation = btEpoch;
  const usbOperation = usbEpoch;
  const outputOperation = enableEpoch;
  try {
    const [system, users, session, bt, wired] = await Promise.all([
      api("/system"),
      api("/users"),
      api("/test-controller"),
      api("/bluetooth"),
      api("/usb"),
    ]);
    $("system").textContent = JSON.stringify(system, null, 2);
    const selected = $("user").value;
    fillUsers(users);
    if ([...$("user").options].some((o) => o.value === selected)) $("user").value = selected;
    const usersKey = JSON.stringify([
      $("user").value,
      userList(users)
        .filter((u) => u.signed_in !== false)
        .map((u) => u.user_id ?? u.id)
        .sort(),
    ]);
    if (usersKey !== stableUsersKey) {
      stableUsersKey = usersKey;
      stableUsersSince = Date.now();
    }
    usersReady = true;
    const currentSources =
      bluetoothOperation === btEpoch &&
      usbOperation === usbEpoch &&
      outputOperation === enableEpoch;
    if (currentSources && !btBusy && !usbBusy && !enabling) {
      virtualSession = session;
      renderBluetooth(bt);
      renderUsb(wired);
      sourcesReady = true;
    }
    updateActionButtons();
    $("session").textContent = JSON.stringify(session, null, 2);
    status("服务已连接 · 状态已更新");
    if (!$("pause").checked) {
      const logs = await api("/logs?level=" + encodeURIComponent($("level").value));
      $("logs").textContent = JSON.stringify(logs, null, 2);
      $("logs").scrollTop = $("logs").scrollHeight;
    }
  } catch (e) {
    usersReady = false;
    stableUsersKey = null;
    sourcesReady = false;
    if (usbOperation === usbEpoch && !usbBusy) {
      usb = null;
      usbMessage("USB 状态不可用：" + e.message, true);
    }
    if (bluetoothOperation === btEpoch && !btBusy) {
      bluetooth = null;
      btMessage("蓝牙状态不可用：" + e.message, true);
    }
    updateActionButtons();
    status("连接或硬件错误：" + e.message, true);
  } finally {
    refreshing = false;
  }
}
$("enable").onclick = async () => {
  if (!canStart("test")) return;
  if (!$("user").value) return status("请选择已登录用户", true);
  enabling = true;
  const operation = ++enableEpoch;
  $("enable").disabled = true;
  try {
    await enableOutput(Number($("user").value), operation);
    if (operation === enableEpoch) await refresh();
  } catch (e) {
    if (operation === enableEpoch) status(e.message, true);
  } finally {
    enabling = false;
    sourcesReady = false;
    updateActionButtons();
    refresh();
  }
};
$("disable").onclick = async () => {
  ++enableEpoch;
  try {
    await api("/test-controller", "DELETE");
    await refresh();
  } catch (e) {
    status(e.message, true);
  }
};
$("refresh").onclick = refresh;
$("level").onchange = refresh;
$("copy").onclick = async () => {
  try {
    await navigator.clipboard.writeText($("logs").textContent);
    status("日志已复制");
  } catch (e) {
    status("复制失败：" + e.message, true);
  }
};
$("export").onclick = () => {
  const url = URL.createObjectURL(new Blob([$("logs").textContent], { type: "text/plain" }));
  const a = document.createElement("a");
  a.href = url;
  a.download = "unicontroller-logs.txt";
  a.click();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
};
refresh();
setInterval(() => {
  if (!document.hidden) refresh();
}, 2000);

$("stop").onclick = async () => {
  if (!window.confirm("停止 UniController 并移除测试手柄？重新使用需要再次加载 payload。")) return;
  $("stop").disabled = true;
  try {
    await api("/stop", "POST");
    status("UniController 已收到停止请求，请关闭此页面。");
  } catch (error) {
    status("停止未确认：" + error.message, true);
    $("stop").disabled = false;
  }
};
