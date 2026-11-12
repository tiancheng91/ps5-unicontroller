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
let usb = { state: "off", reason: "", user_id: 1, devices: [], pending: false, bound: false };
let failUsb = false,
  failQuery = false,
  virtualEnabled = false;
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
    if (url.endsWith("/usb/start")) {
      if (failUsb) {
        ok = false;
        status = 409;
        data = { error: "usb_busy" };
      } else {
        await new Promise((r) => (releaseStart = r));
        usb = { ...usb, state: "on", pending: true };
        data = { pending: true };
      }
    } else if (url.endsWith("/usb/stop"))
      usb = { ...usb, state: "off", pending: false, bound: false };
    else if (url.endsWith("/bluetooth") && failQuery) throw Error("source_unavailable");
    else if (url.endsWith("/session")) data = { token: "redacted" };
    else if (url.endsWith("/users")) data = { users: [{ user_id: 1, display_name: "Player" }] };
    else if (url.endsWith("/usb")) data = usb;
    else if (url.endsWith("/test-controller"))
      data = { enabled: virtualEnabled, session_id: virtualEnabled ? 9 : null };
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
  assert.equal(calls.filter((c) => c.method === "POST").length, 0, "page must not auto-start USB");
  assert(get("usb-start").disabled, "account must stabilize");
  now += 5001;
  await vm.runInContext("refresh()", context);
  assert(!get("usb-start").disabled);
  bt.state = "on";
  await vm.runInContext("refresh()", context);
  assert(!get("usb-start").disabled, "Bluetooth permits concurrent USB");
  bt.state = "off";
  virtualEnabled = true;
  await vm.runInContext("refresh()", context);
  assert(get("usb-start").disabled);
  virtualEnabled = false;
  await vm.runInContext("refresh()", context);
  const starting = get("usb-start").onclick();
  await settle();
  assert(
    get("bt-start").disabled && get("enable").disabled,
    "awaiting USB POST excludes other sources",
  );
  await get("usb-start").onclick();
  assert.equal(calls.filter((c) => c.url.endsWith("/usb/start")).length, 1);
  releaseStart();
  await starting;
  assert(
    !get("bt-start").disabled && get("enable").disabled && get("usb-start").disabled,
    "physical transports coexist while phone output remains exclusive",
  );
  usb = {
    ...usb,
    pending: false,
    bound: true,
    devices: [
      {
        slot: 0,
        user_id: 1,
        node: "<img src=x>",
        vid: 123,
        pid: 456,
        profile: "<script>unsafe</script>",
        connected: true,
        input_frames: 27,
        bound: true,
      },
    ],
  };
  await vm.runInContext("refresh()", context);
  assert.equal(get("usb-devices").children[0].children[0].textContent, "<script>unsafe</script>");
  assert.equal(get("usb-devices").children[0].children[1].textContent, "<img src=x>");
  usb.devices[0].model = "GameSir Nova 2 Lite Receiver";
  await vm.runInContext("refresh()", context);
  assert.equal(
    get("usb-devices").children[0].children[0].textContent,
    "GameSir Nova 2 Lite Receiver",
  );
  await get("usb-devices").querySelectorAll("button")[0].onclick();
  const assignment = calls.find((c) => c.url.endsWith("/usb/assign"));
  assert.deepEqual(assignment.body, { slot: 0, user_id: 1, node: "<img src=x>" });
  await get("usb-stop").onclick();
  assert(!get("bt-start").disabled && !get("enable").disabled);
  failUsb = true;
  await get("usb-start").onclick();
  assert(get("usb-status").textContent.includes("usb_busy"));
  assert(
    get("usb-start").disabled && get("bt-start").disabled && get("enable").disabled,
    "failed operation requires fresh source status",
  );
  failUsb = false;
  await vm.runInContext("refresh()", context);
  assert(!get("usb-start").disabled);
  failQuery = true;
  await vm.runInContext("refresh()", context);
  assert(
    get("usb-start").disabled && get("bt-start").disabled && get("enable").disabled,
    "any source query failure must fail closed",
  );
  console.log(
    "USB UI: no auto-start, stable account, source conflicts, duplicate/pending start, safe DOM, errors and fail-closed polling PASS",
  );
})().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
