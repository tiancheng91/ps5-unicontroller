"use strict";
const fs = require("fs"),
  vm = require("vm"),
  path = require("path");
function harness(name = "control") {
  let factory,
    now = 1000;
  const calls = [],
    timers = [],
    listeners = {};
  const state = {
    users: {
      users: [
        { user_id: 1, display_name: "Player 1" },
        { user_id: 2, display_name: "Player 2" },
      ],
    },
    bt: { state: "off", devices: [], paired: [], discovered: [], scan_seconds: 0, scan_id: 0 },
    usb: { state: "off", devices: [], pending: false, bound: false },
    session: {},
  };
  let respond = async (route, method, body) => {
    if (route === "/users") return state.users;
    if (route === "/bluetooth") return state.bt;
    if (route === "/usb") return state.usb;
    if (route === "/test-controller") return state.session;
    if (route === "/system") return { mode: "host" };
    if (route.startsWith("/logs")) return { entries: [], dropped: 0 };
    if (/\/(bluetooth|usb)\/start$/.test(route)) {
      const s = route.includes("bluetooth") ? state.bt : state.usb;
      s.state = "on";
      return {};
    }
    if (/\/(bluetooth|usb)\/stop$/.test(route)) {
      const s = route.includes("bluetooth") ? state.bt : state.usb;
      s.state = "off";
      return {};
    }
    if (route === "/bluetooth/scan") {
      state.bt.scan_id++;
      state.bt.scan_seconds = body.seconds;
      state.bt.discovered = [
        {
          id: 7,
          name: "Example pad",
          address: "02:00:00:00:00:01",
          transport: "classic",
          state: "discovered",
          rssi: -40,
          reason: "",
        },
      ];
      return {};
    }
    if (route === "/bluetooth/scan/stop") {
      state.bt.scan_seconds = 0;
      return {};
    }
    if (route === "/bluetooth/pair") {
      const found = state.bt.discovered.find((d) => d.id === body.discovery_id);
      if (found) found.state = "pairing";
      return {};
    }
    return {};
  };
  const context = {
    window: { confirm: () => true },
    document: {
      hidden: false,
      addEventListener(k, fn) {
        listeners[k] = fn;
      },
    },
    Alpine: {
      data(k, fn) {
        if (k === name) factory = fn;
      },
      store() {
        return ui;
      },
    },
    performance: { now: () => now },
    Date: class extends Date {
      static now() {
        return now;
      }
    },
    console,
    setInterval() {
      return 1;
    },
    clearInterval() {},
    setTimeout(fn, ms) {
      if (ms === 300) timers.push(fn);
      return 1;
    },
    UCAPI: {
      async request(route, method = "GET", body) {
        calls.push({ route, method, body });
        return respond(route, method, body);
      },
    },
  };
  vm.createContext(context);
  vm.runInContext(fs.readFileSync(path.join(__dirname, "../../web/messages.js"), "utf8"), context);
  const ui = {
    lang: "zh",
    t(k, values = {}) {
      return (
        context.window.UCMessages[this.lang][k]?.replace(/\{(\w+)\}/g, (_, v) =>
          String(values[v] ?? ""),
        ) || k
      );
    },
  };
  vm.runInContext(
    fs.readFileSync(
      path.join(__dirname, "../../web/" + (name === "control" ? "app" : name) + ".js"),
      "utf8",
    ),
    context,
  );
  listeners["alpine:init"]();
  return {
    model: factory(),
    state,
    calls,
    timers,
    ui,
    advance(ms) {
      now += ms;
    },
    responder(fn) {
      const previous = respond;
      respond = (route, method, body) => fn(route, method, body, previous);
    },
  };
}
module.exports = { harness, settle: () => new Promise((r) => setImmediate(r)) };
