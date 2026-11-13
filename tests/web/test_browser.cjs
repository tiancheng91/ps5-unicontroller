"use strict";
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const { spawn } = require("node:child_process");
const { chromium } = require("playwright-core");
const root = path.resolve(__dirname, "../..");
(async () => {
  const server = spawn(
    process.env.PYTHON || "python3",
    ["tools/preview_server.py", "--port", "0"],
    { cwd: root, stdio: ["ignore", "pipe", "pipe"] },
  );
  let browser;
  try {
    const port = await new Promise((resolve, reject) => {
      let output = "";
      const timer = setTimeout(() => reject(Error("Preview server timeout")), 10000);
      server.on("error", reject);
      server.on("exit", (code) => {
        clearTimeout(timer);
        reject(Error("Preview server exited: " + code));
      });
      server.stdout.on("data", (chunk) => {
        output += chunk;
        const match = output.match(/127\.0\.0\.1:(\d+)/);
        if (match) {
          clearTimeout(timer);
          resolve(Number(match[1]));
        }
      });
    });
    const executablePath =
      process.env.CHROME_PATH ||
      [
        "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
        "/usr/bin/google-chrome",
        "/usr/bin/chromium",
      ].find((p) => fs.existsSync(p));
    if (!executablePath) throw Error("Set CHROME_PATH to a Chromium/Chrome executable");
    browser = await chromium.launch({ executablePath, headless: true });
    const context = await browser.newContext({
      locale: "en-US",
      viewport: { width: 1280, height: 900 },
    });
    const page = await context.newPage(),
      errors = [],
      apiRequests = [];
    page.on("pageerror", (e) => errors.push(e.message));
    page.on("console", (m) => {
      if (m.type() === "warning" && m.text().includes("Alpine")) errors.push(m.text());
    });
    page.on("request", (r) => {
      if (new URL(r.url()).pathname.startsWith("/api/v1/")) apiRequests.push(r.url());
    });
    const url = "http://127.0.0.1:" + port;
    await page.goto(url);
    await page.waitForFunction(
      () => document.querySelector("#devices h3")?.textContent === "Sample Switch Pro",
    );
    assert.equal(await page.locator("html").getAttribute("lang"), "en");
    assert((await page.locator(".preview-notice").innerText()).includes("Demo preview"));
    await page.evaluate(() => {
      window.commands = [];
      const request = UCAPI.request;
      UCAPI.request = (...args) => {
        if (args[1] && args[1] !== "GET") window.commands.push(args);
        return request(...args);
      };
    });
    // Device details are a modal drawer; browsing must never start hardware.
    const details = page.locator("#devices button").first();
    await details.click();
    await page.locator("[data-drawer]").waitFor({ state: "visible" });
    assert(
      await page.locator("[data-close-drawer]").evaluate((el) => el === document.activeElement),
    );
    await page.keyboard.press("Shift+Tab");
    assert(await page.evaluate(() => !!document.activeElement.closest("[data-drawer]")));
    await page.keyboard.press("Escape");
    await page.waitForFunction(() => document.activeElement?.matches("#devices button"));
    assert.deepEqual(await page.evaluate(() => window.commands), []);
    await details.focus();
    await page.evaluate(() => {
      window.savedButton = document.activeElement;
    });
    await page.waitForTimeout(2300);
    assert(
      await page.evaluate(() => document.activeElement === window.savedButton),
      "keyed refresh preserves focus",
    );
    await details.click();
    await page.locator('summary[x-label="advancedAssignment"]').click();
    await page.locator("#user").selectOption("2");
    await page.waitForTimeout(5200);
    await page.locator("#assign-device").click();
    assert(
      await page.evaluate(() =>
        window.commands.some((c) => c[0] === "/bluetooth/assign" && c[2].user_id === 2),
      ),
    );
    await page.keyboard.press("Escape");
    // Text from the backend is never interpreted as HTML.
    await page.evaluate(() => {
      const request = UCAPI.request;
      UCAPI.request = async (...args) => {
        const data = await request(...args);
        if (args[0] === "/bluetooth" && data.devices.length)
          data.devices[0].model = '<img src=x onerror="alert(1)">';
        return data;
      };
    });
    await page.waitForFunction(() =>
      document.querySelector("#devices h3")?.textContent.includes("<img"),
    );
    assert.equal(await page.locator("#devices img").count(), 0);
    await page.locator('nav a[data-page="logs"]').click();
    await page.locator("#logs-view").waitFor({ state: "visible" });
    assert(!(await page.locator("#controllers-view").isVisible()));
    await page.locator('nav a[data-page="settings"]').click();
    await page.locator("#settings-view").waitFor({ state: "visible" });
    const beforeAppearance = await page.evaluate(() => window.commands.length);
    await page.locator("#ui-language").selectOption("zh");
    await page.locator("#ui-theme").selectOption("blue");
    await page.locator("#ui-size").selectOption("tv");
    assert.equal(await page.locator("html").getAttribute("lang"), "zh-CN");
    assert.equal(await page.locator("html").getAttribute("data-theme"), "blue");
    assert.equal(
      await page.evaluate(() => window.commands.length),
      beforeAppearance,
      "appearance has no hardware writes",
    );
    await page.locator("#ui-size").selectOption("normal");
    await page.locator("#bt-stop").click();
    await page.waitForFunction(() => !document.querySelector("#save").disabled);
    await page.locator('summary[x-label="mapping"]').click();
    const mapping = page.locator('select[x-field="field"]');
    assert.equal(await mapping.nth(14).inputValue(), "14");
    await mapping.first().selectOption("11");
    const rumble = page.locator('input[data-form="rumble"]');
    await rumble.setChecked(false);
    await page.locator("#swap").check();
    await page.locator("#save").click();
    await page.waitForFunction(() =>
      document.querySelector("#settings-status").textContent.includes("已保存"),
    );
    await page.locator("#load").click();
    assert.equal(await rumble.isChecked(), false);
    assert.equal(await mapping.first().inputValue(), "11");
    await page.locator("#ui-navigation").selectOption("arrows");
    await page.locator("#ui-theme").focus();
    await page.keyboard.press("ArrowDown");
    assert(
      await page.locator("#ui-theme").evaluate((el) => el === document.activeElement),
      "native select keeps arrows",
    );
    await page.keyboard.press("Escape");
    await page.locator('nav a[data-page="settings"]').focus();
    await page.keyboard.press("ArrowDown");
    assert.notEqual(
      await page.evaluate(() => document.activeElement?.closest("#controllers-view")?.id),
      "controllers-view",
    );
    // Confirm is edge-triggered; holding a gamepad button cannot toggle twice.
    await rumble.focus();
    await page.evaluate(() => {
      window.fakePad = {
        index: 0,
        mapping: "standard",
        buttons: Array.from({ length: 16 }, () => ({ pressed: false })),
      };
      navigator.getGamepads = () => [window.fakePad];
      document.hasFocus = () => true;
    });
    await page.waitForTimeout(400);
    await page.evaluate(() => {
      window.fakePad.buttons[0].pressed = true;
    });
    await page.waitForFunction(() => document.querySelector('input[data-form="rumble"]').checked);
    await page.waitForTimeout(500);
    assert.equal(await rumble.isChecked(), true);
    await page.evaluate(() => {
      navigator.getGamepads = () => [];
    });
    // An off service is enabled before pairing; merely opening the drawer is read-only.
    await page.locator('nav a[data-page="controllers"]').click();
    await page.locator("#add-controller").click();
    await page.locator("#bt-scan").click();
    await page.locator(".discovered-device").first().waitFor({ state: "visible" });
    const firstSeconds = await page.evaluate(
      () => Alpine.$data(document.querySelector("#management")).scanRemaining,
    );
    await page.waitForTimeout(2300);
    const nextSeconds = await page.evaluate(
      () => Alpine.$data(document.querySelector("#management")).scanRemaining,
    );
    assert(nextSeconds < firstSeconds, "scan countdown must tick instead of displaying fixed 60");
    let writes = await page.evaluate(() => window.commands.map((c) => c[0]));
    assert(writes.indexOf("/bluetooth/start") < writes.lastIndexOf("/bluetooth/scan"));
    assert(!writes.includes("/bluetooth/pair"), "discovering a device must not pair it");
    await page.locator(".discovered-device button").first().click();
    await page.waitForFunction(() => window.commands.some((c) => c[0] === "/bluetooth/pair"));
    const selected = await page.evaluate(
      () => window.commands.find((c) => c[0] === "/bluetooth/pair")[2],
    );
    assert(Number.isInteger(selected.discovery_id) && !selected.seconds);
    await page.waitForFunction(() => document.querySelector(".discovered-device button")?.disabled);
    await page.locator("#bt-scan-stop").click();
    await page.waitForFunction(() => !Alpine.$data(document.querySelector("#management")).scanning);
    assert.equal(
      await page.evaluate(() => Alpine.$data(document.querySelector("#management")).bt.state),
      "on",
    );
    await page.keyboard.press("Escape");
    await page.locator("[data-drawer]").waitFor({ state: "hidden" });
    await page.goto(url + "/settings");
    await page.locator("#settings-view").waitFor({ state: "visible" });
    assert.equal(await page.locator("html").getAttribute("data-theme"), "blue");
    assert.equal(await page.locator("#ui-language").inputValue(), "zh");
    await page.goto(url + "/controller");
    await page.waitForFunction(() =>
      document.querySelector("#status").textContent.includes("选择已登录"),
    );
    await page.evaluate(() => {
      window.inputs = [];
      const request = window.fetch;
      window.fetch = (resource, options) => {
        if (String(resource).endsWith("/test-controller/input"))
          window.inputs.push(JSON.parse(options.body));
        return request(resource, options);
      };
    });
    await page.locator("#enable").click();
    const ps = page.locator('.padbutton[data-bit="65536"]');
    await page.waitForFunction(
      () => !document.querySelector('.padbutton[data-bit="65536"]').disabled,
    );
    await ps.focus();
    await page.keyboard.down("Enter");
    await page.waitForFunction(() => window.inputs.some((f) => f.buttons === 65536));
    await page.keyboard.up("Enter");
    await page.waitForFunction(
      () => window.inputs.length && window.inputs[window.inputs.length - 1].buttons === 0,
    );
    await page.locator("#disable").click();
    assert(await ps.isDisabled());
    assert.deepEqual(apiRequests, [], "isolated preview never sends real API requests");
    // Fresh Chinese locale defaults to Chinese. Blocked storage still permits UI use.
    const chinese = await browser.newContext({ locale: "zh-CN" });
    const cp = await chinese.newPage();
    await cp.addInitScript(() => {
      Object.defineProperty(window, "localStorage", {
        get() {
          throw Error("blocked");
        },
      });
    });
    await cp.goto(url + "/#settings");
    await cp.waitForFunction(() => document.querySelector("#ui-language").value === "zh");
    await cp.locator("#ui-theme").selectOption("green");
    assert.equal(await cp.locator("html").getAttribute("data-theme"), "green");
    const narrow = await browser.newContext({
      viewport: { width: 390, height: 844 },
      locale: "en-US",
    });
    const np = await narrow.newPage();
    await np.goto(url + "/#settings");
    await np.waitForFunction(() => document.querySelector("#ui-language").value === "en");
    assert(
      await np.evaluate(() => document.documentElement.scrollWidth <= innerWidth),
      "mobile page must not overflow horizontally",
    );
    assert.deepEqual(errors, [], "no CSP or Alpine runtime errors");
    console.log(
      "Browser: skeleton layout, CSP, drawers, search ordering, keyed focus, safe text, locale, settings, gamepad, PS input and preview isolation PASS",
    );
  } finally {
    if (browser) await browser.close();
    server.kill("SIGTERM");
  }
})().catch((e) => {
  console.error(e);
  process.exitCode = 1;
});
