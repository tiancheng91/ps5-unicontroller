"use strict";
(() => {
  const key = "unicontroller.appearance.v1";
  const choices = {
    lang: ["zh", "en"],
    theme: ["orange", "blue", "green", "red"],
    size: ["normal", "tv"],
    navigation: ["native", "arrows"],
  };
  let saved = {};
  try {
    saved = JSON.parse(localStorage.getItem(key) || "{}");
  } catch {
    /* Storage is optional. */
  }
  const preferred = (navigator.languages && navigator.languages[0]) || navigator.language || "en";
  const state = {
    lang: /^zh/i.test(preferred) ? "zh" : "en",
    theme: "orange",
    size: "normal",
    navigation: "native",
    page: "controllers",
    get controllersPage() {
      return this.page === "controllers";
    },
    get logsPage() {
      return this.page === "logs";
    },
    get settingsPage() {
      return this.page === "settings";
    },
    get labels() {
      return window.UCMessages[this.lang];
    },
    t(name, values = {}) {
      return (this.labels[name] || name).replace(/\{(\w+)\}/g, (_, k) => String(values[k] ?? ""));
    },
  };
  for (const name of Object.keys(choices))
    if (choices[name].includes(saved?.[name])) state[name] = saved[name];
  function apply(ui) {
    const root = document.documentElement;
    root.lang = ui.lang === "zh" ? "zh-CN" : "en";
    root.dataset.theme = ui.theme;
    root.dataset.size = ui.size;
    root.dataset.navigation = ui.navigation;
    try {
      localStorage.setItem(
        key,
        JSON.stringify(Object.fromEntries(Object.keys(choices).map((k) => [k, ui[k]]))),
      );
    } catch {
      /* Keep preferences in memory. */
    }
    window.dispatchEvent(new CustomEvent("uc-language", { detail: ui.lang }));
  }
  // Apply appearance before Alpine or body parsing to avoid a theme flash.
  document.documentElement.lang = state.lang === "zh" ? "zh-CN" : "en";
  document.documentElement.dataset.theme = state.theme;
  document.documentElement.dataset.size = state.size;
  document.addEventListener("alpine:init", () => {
    Alpine.store("ui", state);
    Alpine.directive("label", (el, { expression }, { effect }) => {
      effect(() => {
        el.textContent = Alpine.store("ui").labels[expression] || expression;
      });
    });
    Alpine.directive("field", (el, { expression }, { evaluate, effect, cleanup }) => {
      effect(() => {
        const field = evaluate(expression),
          value = field.value;
        Alpine.nextTick(() => {
          if (el.isConnected) el.value = value;
        });
      });
      const change = () => {
        evaluate(expression).value = el.value;
      };
      el.addEventListener("change", change);
      cleanup(() => el.removeEventListener("change", change));
    });
    Alpine.data("shell", () => ({}));
    Alpine.effect(() => apply(Alpine.store("ui")));
    const route = () => {
      const ui = Alpine.store("ui");
      const hash = location.hash.slice(1);
      const next = ["controllers", "settings", "logs"].includes(hash)
        ? hash
        : /^\/settings\/?$/.test(location.pathname)
          ? "settings"
          : "controllers";
      const changed = ui.page !== next;
      if (changed) window.dispatchEvent(new CustomEvent("uc-close-drawer"));
      ui.page = next;
      document.querySelectorAll("nav a[data-page]").forEach((el) => {
        if (el.dataset.page === (document.querySelector(".controller-stage") ? "test" : next))
          el.setAttribute("aria-current", "page");
        else el.removeAttribute("aria-current");
      });
      if (changed)
        Alpine.nextTick(() => {
          const heading = document.querySelector("#" + next + "-view h1");
          if (heading) {
            heading.tabIndex = -1;
            heading.focus();
          }
        });
    };
    route();
    window.addEventListener("hashchange", route);
  });
  window.UCUI = {
    get: () => window.Alpine?.store("ui") || state,
    set(name, value) {
      if (choices[name]?.includes(value)) this.get()[name] = value;
    },
  };
  document.addEventListener("DOMContentLoaded", () => apply(window.UCUI.get()));
})();
