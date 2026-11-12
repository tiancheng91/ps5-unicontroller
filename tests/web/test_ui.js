"use strict";
const fs = require("fs");
const vm = require("vm");
const assert = require("assert");
const path = require("path");
function harness(relative) {
  const elements = new Map();
  function element(id) {
    if (!elements.has(id))
      elements.set(id, {
        value: id === "user" ? "1" : "",
        disabled: false,
        checked: true,
        options: [],
        classList: { toggle() {} },
        setAttribute() {},
        querySelectorAll() {
          return [];
        },
        removeAttribute() {},
        replaceChildren() {
          this.options = [];
        },
        append(v) {
          this.options.push(v);
        },
        addEventListener() {},
        firstElementChild: { style: {} },
        style: {},
      });
    return elements.get(id);
  }
  const events = {},
    timers = [],
    calls = [];
  const context = {
    console,
    AbortController,
    JSON,
    Number,
    Date,
    Map,
    Promise,
    Error,
    clearTimeout() {},
    clearInterval() {},
    setInterval() {
      return 1;
    },
    setTimeout(fn, ms) {
      if (ms === 300) timers.push(fn);
      return 1;
    },
    document: {
      hidden: false,
      getElementById: element,
      querySelectorAll() {
        return [];
      },
      createElement() {
        return {};
      },
      addEventListener(name, fn) {
        events[name] = fn;
      },
    },
    window: {
      addEventListener(name, fn) {
        events[name] = fn;
      },
    },
    navigator: {},
    fetch: async () => {
      throw Error("unexpected fetch");
    },
  };
  vm.createContext(context);
  let source = fs.readFileSync(path.join(__dirname, "../../web", relative), "utf8");
  // Suppress startup networking; test real handlers and enableOutput against deferred requests.
  source = source.replace(/\ninit\(\);\nrender\(\);\s*$/, "\nrender();");
  source = source.replace(/\nrefresh\(\);\nsetInterval\([\s\S]*$/, "");
  vm.runInContext(source, context);
  vm.runInContext("refresh = async () => {};", context);
  const queue = [];
  context.request = (route, method) => {
    calls.push({ route, method });
    if (method === "POST") return new Promise((resolve, reject) => queue.push({ resolve, reject }));
    return Promise.resolve({});
  };
  vm.runInContext("api = request;", context);
  if (relative === "control/control.js")
    vm.runInContext(
      'bluetooth = {state:"off",devices:[],paired:[],pairing_seconds:0}; usb = {state:"off",devices:[],pending:false,bound:false}; sourcesReady = true; refresh = async () => { sourcesReady = true; updateActionButtons(); };',
      context,
    );
  return { context, events, element, calls, queue, timers };
}
async function flush() {
  for (let i = 0; i < 10; i++) await Promise.resolve();
}
(async () => {
  for (const file of ["control/control.js", "controller/controller.js"]) {
    let h = harness(file);
    const enabling = h.element("enable").onclick();
    assert.equal(h.queue.length, 1);
    await h.element("enable").onclick();
    assert.equal(h.queue.length, 1, "duplicate Enable must not send another POST");
    assert.equal(h.element("disable").disabled, false, "pending enable must be cancellable");
    await h.element("disable").onclick();
    h.queue.shift().resolve({ enabled: true, session_id: 1 });
    await enabling;
    assert.equal(h.calls.filter((c) => c.method === "POST").length, 1);
    assert.equal(
      h.calls.filter((c) => c.method === "DELETE").length,
      2,
      "late success must trigger cleanup after initial DELETE",
    );
    h = harness(file);
    const waiting = h.element("enable").onclick();
    h.queue.shift().resolve({ pending: true });
    await flush();
    await h.element("disable").onclick();
    h.timers.shift()();
    await waiting;
    assert.equal(
      h.calls.filter((c) => c.method === "POST").length,
      1,
      "cancelled pending loop must not retry",
    );
    assert.equal(h.element("enable").disabled, false);
  }
  const h = harness("controller/controller.js");
  const waiting = h.element("enable").onclick();
  h.context.document.hidden = true;
  h.events.visibilitychange();
  await flush();
  h.queue.shift().resolve({ enabled: true, session_id: 7 });
  await waiting;
  assert.equal(
    h.calls.filter((c) => c.method === "DELETE").length,
    2,
    "hidden pending enable must clean late result",
  );
  assert.equal(vm.runInContext("active", h.context), false);
  console.log("UI cancellation checks passed");
})().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
