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
let current = null;
const levels = ["off", "error", "warn", "info", "debug", "trace"];
for (const id of ["recording", "page"])
  for (const v of levels) {
    const o = document.createElement("option");
    o.value = v;
    o.textContent = v.toUpperCase();
    $(id).append(o);
  }
const names = {
  0: "Create",
  1: "L3",
  2: "R3",
  3: "Options",
  4: "↑",
  5: "→",
  6: "↓",
  7: "←",
  8: "L2",
  9: "R2",
  10: "L1",
  11: "R1",
  12: "Triangle",
  13: "Circle",
  14: "Cross",
  15: "Square",
  16: "PS",
  20: "Touchpad",
};
for (let i = 0; i < 32; i++) {
  const label = document.createElement("label");
  label.textContent = "位 " + i + " " + (names[i] || "保留");
  const select = document.createElement("select");
  select.id = "map" + i;
  select.setAttribute("aria-label", "来源位 " + i + " 的目标位");
  for (let j = 0; j < 32; j++) {
    const o = document.createElement("option");
    o.value = j;
    o.textContent = j + " · " + (names[j] || "保留");
    select.append(o);
  }
  label.append(select);
  $("mapping").append(label);
}
function valid(s) {
  return (
    s &&
    s.schema_version === 1 &&
    levels.includes(s.recording_level) &&
    levels.includes(s.page_level) &&
    typeof s.rumble === "boolean" &&
    typeof s.swap_ab === "boolean" &&
    Array.isArray(s.button_mapping) &&
    s.button_mapping.length === 32 &&
    s.button_mapping.every((v) => Number.isInteger(v) && v >= 0 && v < 32)
  );
}
async function load() {
  try {
    const s = await api("/settings");
    if (!valid(s)) throw Error("设置结构无效，未启用保存");
    current = s;
    $("recording").value = s.recording_level;
    $("page").value = s.page_level;
    $("rumble").checked = s.rumble;
    $("swap").checked = s.swap_ab;
    s.button_mapping.forEach((v, i) => ($("map" + i).value = v));
    $("save").disabled = false;
    status("已读取服务器配置");
  } catch (e) {
    current = null;
    $("save").disabled = true;
    status(e.message, true);
  }
}
$("identity").onclick = () => {
  for (let i = 0; i < 32; i++) $("map" + i).value = i;
};
$("load").onclick = load;
$("save").onclick = async () => {
  if (!current) return;
  $("save").disabled = true;
  try {
    const s = {
      ...current,
      recording_level: $("recording").value,
      page_level: $("page").value,
      rumble: $("rumble").checked,
      swap_ab: $("swap").checked,
      button_mapping: Array.from({ length: 32 }, (_, i) => Number($("map" + i).value)),
    };
    await api("/settings", "POST", s);
    current = s;
    status("设置已保存");
  } catch (e) {
    status("保存失败：" + e.message, true);
  } finally {
    $("save").disabled = !current;
  }
};
load();
