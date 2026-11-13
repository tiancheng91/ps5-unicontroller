"use strict";
// Render editable SVG masters to the checked-in PNG sizes consumed by PS5/web.
// Uses the existing Playwright dev dependency; no network or image service.
const fs = require("node:fs");
const path = require("node:path");
const { chromium } = require("playwright-core");
const root = path.resolve(__dirname, "..");
const outputs = [
  ["assets/branding/icon.svg", "assets/branding/icon0.png", 512, 512],
  ["assets/branding/icon.svg", "web/icon.png", 128, 128],
  ["assets/branding/banner.svg", "assets/branding/banner.png", 2048, 640],
];
(async () => {
  const executablePath =
    process.env.CHROME_PATH ||
    [
      "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
      "/usr/bin/google-chrome",
      "/usr/bin/chromium",
    ].find((file) => fs.existsSync(file));
  if (!executablePath) throw Error("Set CHROME_PATH to a Chrome/Chromium executable");
  const browser = await chromium.launch({ executablePath, headless: true });
  try {
    for (const [source, target, width, height] of outputs) {
      const page = await browser.newPage({ viewport: { width, height }, deviceScaleFactor: 1 });
      await page.setContent(
        "<!doctype html><style>html,body{margin:0;overflow:hidden}svg{display:block;width:100vw;height:100vh}</style>" +
          fs.readFileSync(path.join(root, source), "utf8"),
      );
      await page.evaluate(() => document.fonts.ready);
      await page.screenshot({ path: path.join(root, target), animations: "disabled" });
      await page.close();
      console.log(`${source} -> ${target} (${width}x${height})`);
    }
  } finally {
    await browser.close();
  }
})().catch((error) => {
  console.error(error.message);
  process.exitCode = 1;
});
