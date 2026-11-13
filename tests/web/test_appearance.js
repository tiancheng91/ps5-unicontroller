"use strict";
const assert = require("assert");
const fs = require("fs");
const vm = require("vm");
const path = require("path");
function appearance(userAgent, saved = {}) {
  const listeners = {},
    root = { dataset: {} };
  let persisted;
  const context = {
    navigator: { userAgent, language: "zh-CN" },
    document: {
      documentElement: root,
      addEventListener: (name, fn) => {
        listeners[name] = fn;
      },
    },
    localStorage: {
      getItem: () => JSON.stringify(saved),
      setItem: (_, value) => {
        persisted = JSON.parse(value);
      },
    },
    window: { dispatchEvent() {} },
    CustomEvent: function () {},
  };
  vm.createContext(context);
  vm.runInContext(fs.readFileSync(path.join(__dirname, "../../web/ui.js"), "utf8"), context);
  listeners.DOMContentLoaded();
  return { ui: context.window.UCUI, root, persisted };
}
for (const version of ["2.26", "12.40", "13.60"]) {
  const h = appearance(`Mozilla/5.0 (PlayStation; PlayStation 5/${version}) AppleWebKit/605.1.15`, {
    size: "normal",
    navigation: "native",
    theme: "blue",
  });
  assert.equal(h.root.dataset.size, "tv");
  assert.equal(h.root.dataset.navigation, "arrows");
  assert.equal(h.root.dataset.theme, "blue");
  h.ui.set("size", "normal");
  h.ui.set("navigation", "native");
  assert.equal(h.ui.get().size, "tv");
  assert.equal(h.ui.get().navigation, "arrows");
  assert.deepEqual(h.persisted, { lang: "zh", theme: "blue" });
}
for (const agent of [
  undefined,
  "Mozilla/5.0 Macintosh",
  "Mozilla/5.0 iPhone",
  "PlayStation 4/12.02",
  "PlayStation 50/1.0",
]) {
  const h = appearance(agent, { size: "tv", navigation: "arrows" });
  assert.equal(h.root.dataset.size, "normal");
  assert.equal(h.root.dataset.navigation, "native");
}
console.log(
  "Appearance: PS5 UA detection, desktop/mobile defaults and obsolete preference cleanup PASS",
);
