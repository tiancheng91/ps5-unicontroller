"use strict";
const fs = require("fs"),
  vm = require("vm"),
  assert = require("assert"),
  path = require("path");
const elements = new Map();
class Element {
  constructor(id) {
    this.id = id;
    this.value = id === "level" ? "info" : "";
    this.options = [];
    this.children = [];
    this.disabled = false;
    this.checked = false;
    this.classList = { toggle() {} };
    this.textContent = "";
  }
  append(child) {
    this.children.push(child);
    if (child.tag === "option") {
      this.options.push(child);
      if (!this.value) this.value = String(child.value);
    }
  }
  replaceChildren() {
    this.children = [];
    this.options = [];
    this.value = "";
  }
  querySelectorAll(tag) {
    return this.children.flatMap((c) => [
      ...(c.tag === tag ? [c] : []),
      ...c.querySelectorAll(tag),
    ]);
  }
}
function get(id) {
  if (!elements.has(id)) elements.set(id, new Element(id));
  return elements.get(id);
}
let now = 1000,
  bt = {
    state: "off",
    reason: "",
    user_id: 1,
    pairing_seconds: 0,
    devices: [],
    paired: [],
  };
const calls = [];
let releaseStart = null;
let failPair = false;
const context = {
  console,
  AbortController,
  Date: { now: () => now },
  setTimeout: () => 1,
  clearTimeout() {},
  setInterval() {},
  clearInterval() {},
  document: {
    hidden: false,
    getElementById: get,
    createElement(tag) {
      const e = new Element();
      e.tag = tag;
      return e;
    },
  },
  window: { confirm: () => false },
  fetch: async (url, options) => {
    const method = options.method,
      body = options.body ? JSON.parse(options.body) : undefined;
    calls.push({ url, method, body });
    let data = {};
    let ok = true,
      status = 200;
    if (url.endsWith("/session")) data = { token: "redacted" };
    else if (url.endsWith("/users")) data = { users: [{ user_id: 1, display_name: "Player" }] };
    else if (url.endsWith("/usb"))
      data = { state: "off", reason: "", user_id: 1, devices: [], pending: false, bound: false };
    else if (url.endsWith("/test-controller")) data = { enabled: false, session_id: null };
    else if (url.endsWith("/bluetooth/start")) {
      await new Promise((r) => (releaseStart = r));
      bt = { ...bt, state: "starting" };
      data = { pending: true };
    } else if (url.endsWith("/bluetooth/pair")) {
      if (failPair) {
        ok = false;
        status = 409;
        data = { error: "hardware_pair_failed" };
      } else bt = { ...bt, pairing_seconds: 60 };
    } else if (url.endsWith("/bluetooth/stop")) bt = { ...bt, state: "off" };
    else if (url.endsWith("/bluetooth")) data = JSON.parse(JSON.stringify(bt));
    return { ok, status, text: async () => JSON.stringify(data) };
  },
};
vm.createContext(context);
vm.runInContext(
  fs.readFileSync(path.join(__dirname, "../../web/control/control.js"), "utf8"),
  context,
);
const settle = () => new Promise((r) => setImmediate(r));
(async () => {
  await settle();
  assert(get("bt-start").disabled, "start must wait for stable users");
  now += 5001;
  await vm.runInContext("refresh()", context);
  assert(!get("bt-start").disabled);
  const first = get("bt-start").onclick();
  await settle();
  await get("bt-start").onclick();
  assert.equal(
    calls.filter((c) => c.url.endsWith("/bluetooth/start")).length,
    1,
    "duplicate start suppressed",
  );
  releaseStart();
  await first;
  assert(get("enable").disabled, "starting disables test output");
  bt = {
    ...bt,
    state: "on",
    devices: [
      {
        address: "<img src=x>",
        model: "<script>bad</script>",
        profile: "Generic HID",
        vid: 1,
        pid: 2,
        connected: true,
        input_frames: 20,
        battery: -1,
        bound: true,
      },
    ],
    paired: [
      {
        address: "E0:00:00:00:00:01",
        vid: 1,
        pid: 2,
        le: false,
        connected: true,
      },
    ],
  };
  await vm.runInContext("refresh()", context);
  assert.equal(get("bt-devices").children[0].children[0].textContent, "<script>bad</script>");
  failPair = true;
  await get("bt-pair").onclick();
  assert(get("bt-status").textContent.includes("hardware_pair_failed"), "real failure visible");
  failPair = false;
  await get("bt-pair").onclick();
  assert(get("bt-pair").disabled, "pair countdown disables repeat request");
  const pairCount = calls.filter((c) => c.url.endsWith("/bluetooth/pair")).length;
  await get("bt-pair").onclick();
  assert.equal(calls.filter((c) => c.url.endsWith("/bluetooth/pair")).length, pairCount);
  await get("bt-paired").querySelectorAll("button")[0].onclick();
  assert.equal(
    calls.find((c) => c.url.endsWith("/bluetooth/forget")).body.address,
    "E0:00:00:00:00:01",
  );
  await get("bt-stop").onclick();
  assert(!get("enable").disabled);
  console.log(
    "Bluetooth UI: stable-user gate, start serialization, conflict gating, safe DOM, error, pair lease, forget PASS",
  );
})().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
