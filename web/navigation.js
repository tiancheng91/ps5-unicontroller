"use strict";
(() => {
  const selector =
    "a[href],button:not(:disabled),select:not(:disabled),input:not(:disabled),summary";
  function targets() {
    const drawer = [...document.querySelectorAll("[data-drawer]")].find(
      (el) => el.getClientRects().length,
    );
    return [...(drawer || document).querySelectorAll(selector)].filter(
      (el) => !el.closest("[hidden],[inert],.padbutton,.stick") && el.getClientRects().length,
    );
  }
  function focus(el) {
    if (el) {
      el.focus();
      el.scrollIntoView({ block: "nearest", inline: "nearest" });
    }
  }
  function move(direction) {
    const all = targets(),
      current = document.activeElement;
    if (!all.includes(current)) return focus(all[0]);
    const r = current.getBoundingClientRect(),
      x = r.left + r.width / 2,
      y = r.top + r.height / 2;
    const horizontal = direction === "ArrowLeft" || direction === "ArrowRight";
    const sign = direction === "ArrowLeft" || direction === "ArrowUp" ? -1 : 1;
    let best,
      score = Infinity;
    for (const el of all) {
      if (el === current) continue;
      const b = el.getBoundingClientRect(),
        dx = b.left + b.width / 2 - x,
        dy = b.top + b.height / 2 - y;
      const forward = (horizontal ? dx : dy) * sign,
        sideways = Math.abs(horizontal ? dy : dx);
      if (forward <= 1) continue;
      const distance = forward + sideways * 3;
      if (distance < score) {
        best = el;
        score = distance;
      }
    }
    focus(best);
  }
  function back() {
    if ([...document.querySelectorAll("[data-drawer]")].some((el) => el.getClientRects().length)) {
      window.dispatchEvent(new CustomEvent("uc-close-drawer"));
      return;
    }
    const el = document.activeElement,
      details = el?.closest("details[open]");
    if (details) {
      details.open = false;
      return focus(details.querySelector("summary"));
    }
    focus(document.querySelector('nav a[aria-current="page"]'));
  }
  let keyboardAt = 0;
  document.addEventListener("keydown", (e) => {
    keyboardAt = performance.now();
    if (
      window.UCUI.get().navigation !== "arrows" ||
      e.defaultPrevented ||
      e.altKey ||
      e.ctrlKey ||
      e.metaKey
    )
      return;
    const el = document.activeElement;
    if (
      el?.matches('select,textarea,input:not([type="checkbox"]),[contenteditable="true"]') ||
      el?.closest(".padbutton,.stick")
    )
      return;
    if (e.key.startsWith("Arrow")) {
      e.preventDefault();
      move(e.key);
    } else if (e.key === "Escape") {
      e.preventDefault();
      back();
    } else if (e.repeat && (e.key === "Enter" || e.key === " ")) e.preventDefault();
  });
  // Keyed lists keep nodes stable; recover focus only when its node disappears/gets disabled.
  let lastFocus, lastRegion;
  document.addEventListener("focusin", (e) => {
    lastFocus = e.target;
    lastRegion = e.target.closest("section,details,header,.row");
  });
  document.addEventListener("DOMContentLoaded", () => {
    new MutationObserver(() => {
      if (!lastFocus || (lastFocus.isConnected && !lastFocus.matches(":disabled"))) return;
      if (document.activeElement !== document.body && document.activeElement !== lastFocus) return;
      const region = lastRegion?.isConnected ? lastRegion : document;
      focus(
        [...region.querySelectorAll(selector)].find((el) => el.getClientRects().length) ||
          document.querySelector('nav a[aria-current="page"]'),
      );
    }).observe(document.body, {
      childList: true,
      subtree: true,
      attributes: true,
      attributeFilter: ["disabled"],
    });
  });
  let previous = null,
    padIndex = null,
    repeatAt = 0,
    repeating = "";
  function poll(now) {
    // Touch input remains native; management navigation never feeds virtual input.
    if (
      window.UCUI.get().navigation !== "arrows" ||
      document.hidden ||
      !document.hasFocus() ||
      document.querySelector(".controller-stage")
    ) {
      previous = null;
      padIndex = null;
    } else {
      let pads = [];
      try {
        pads = navigator.getGamepads ? [...navigator.getGamepads()] : [];
      } catch {
        /* WebView may not expose this API. */
      }
      const pad = pads.find((p) => p && p.mapping === "standard");
      if (pad) {
        const buttons = pad.buttons.map((b) => b.pressed);
        if (previous && padIndex === pad.index && now - keyboardAt > 250) {
          const arrows = [
            [12, "ArrowUp"],
            [13, "ArrowDown"],
            [14, "ArrowLeft"],
            [15, "ArrowRight"],
          ];
          const direction = arrows.find(([i]) => buttons[i]);
          if (
            direction &&
            (!previous[direction[0]] || (repeating === direction[1] && now >= repeatAt))
          ) {
            const el = document.activeElement;
            if (el?.tagName === "SELECT") {
              const step = direction[1] === "ArrowUp" || direction[1] === "ArrowLeft" ? -1 : 1;
              el.selectedIndex = Math.max(
                0,
                Math.min(el.options.length - 1, el.selectedIndex + step),
              );
              el.dispatchEvent(new Event("change", { bubbles: true }));
            } else move(direction[1]);
            repeatAt = now + (previous[direction[0]] ? 140 : 400);
            repeating = direction[1];
          }
          if (!direction) repeating = "";
          if (buttons[0] && !previous[0]) {
            const el = document.activeElement;
            if (el && targets().includes(el) && el.tagName !== "SELECT") el.click();
          }
          if (buttons[1] && !previous[1]) back();
        }
        previous = buttons;
        padIndex = pad.index;
      } else {
        previous = null;
        padIndex = null;
      }
    }
    requestAnimationFrame(poll);
  }
  requestAnimationFrame(poll);
})();
