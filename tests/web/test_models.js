"use strict";
const assert = require("assert"),
  { harness, settle } = require("./model_harness");
(async () => {
  const h = harness(),
    m = h.model;
  await m.refresh();
  const formatted = m.formatLogs({
    entries: [
      {
        timestamp_ms: 1234,
        level: "error",
        code: "create_failed\nforged",
        generation: 1,
        session_id: 2,
      },
    ],
    dropped: 4,
  });
  assert(
    formatted.includes("[mono 1.234s] [ERROR] core create_failed forged generation=1 session=2"),
  );
  assert.equal(formatted.split("\n").length, 2);
  assert(formatted.includes("overwritten=4"));
  const bundle = await m.diagnosticText();
  assert(
    bundle.includes("# /bluetooth") &&
      bundle.includes("# /usb") &&
      bundle.includes("# /usb/diagnostics") &&
      bundle.includes("# runtime.log"),
  );
  h.responder((route, method, body, previous) => {
    if (route === "/diagnostics") throw Error("runtime_log_unavailable");
    return previous(route, method, body);
  });
  const partial = await m.diagnosticText();
  assert(
    partial.includes("# /usb") &&
      partial.includes("runtime.log unavailable: runtime_log_unavailable"),
  );
  const search = harness();
  await search.model.refresh();
  search.advance(5001);
  await search.model.refresh();
  await search.model.beginSearch();
  const commands = search.calls.filter((c) => c.method === "POST").map((c) => c.route);
  assert.deepEqual(commands, ["/bluetooth/start", "/bluetooth/scan"]);
  assert(search.model.searchDisabled, "active pairing window cannot restart");
  assert.equal(search.model.scanRemaining, 60);
  search.advance(1100);
  search.model.acceptBluetooth(search.state.bt); // Even a stale response cannot reset the clock.
  assert.equal(search.model.scanRemaining, 59);
  assert(!search.calls.some((c) => c.route === "/bluetooth/pair"));
  await search.model.pairDevice({ currentTarget: { dataset: { discoveryId: "7" } } });
  assert.deepEqual(search.calls.find((c) => c.route === "/bluetooth/pair").body, {
    discovery_id: 7,
  });
  const pairCount = search.calls.filter((c) => c.route === "/bluetooth/pair").length;
  await search.model.pairDevice({ currentTarget: { dataset: { discoveryId: "7" } } });
  assert.equal(search.calls.filter((c) => c.route === "/bluetooth/pair").length, pairCount);
  await search.model.stopSearch();
  assert.equal(search.model.scanRemaining, 0);
  assert.equal(search.model.bt.state, "on", "stop discovery must not disable Bluetooth");
  const cancel = harness();
  await cancel.model.refresh();
  cancel.advance(5001);
  await cancel.model.refresh();
  let finish;
  cancel.responder(async (route, method, body, previous) => {
    if (route === "/bluetooth/start")
      await new Promise((resolve) => {
        finish = resolve;
      });
    return previous(route, method, body);
  });
  const pendingSearch = cancel.model.beginSearch();
  await settle();
  ++cancel.model.searchEpoch; // Closing the drawer cancels a not-yet-started pairing phase.
  finish();
  await pendingSearch;
  assert(!cancel.calls.some((c) => c.route === "/bluetooth/scan"));
  assert(!cancel.model.searchBusy);
  const s = harness("settings"),
    settings = {
      schema_version: 1,
      recording_level: "info",
      page_level: "info",
      rumble: true,
      swap_ab: false,
      bluetooth_auto_start: false,
      bluetooth_auto_trigger: "login",
      bluetooth_auto_user: 2,
      button_mapping: Array.from({ length: 32 }, (_, i) => i),
    };
  s.responder((route, method, body) =>
    route === "/settings" ? (method === "POST" ? body : settings) : { state: "unsupported" },
  );
  await s.model.load();
  assert(!s.model.saveDisabled);
  s.model.form.swap_ab = true;
  s.model.fields[14].value = "11";
  await s.model.save();
  const saved = s.calls.find((c) => c.method === "POST").body;
  assert(saved.swap_ab && saved.bluetooth_auto_user === 2 && saved.button_mapping[14] === 11);
  s.responder(() => {
    throw Error("disk_full");
  });
  await s.model.save();
  assert(s.model.statusText.includes("disk_full") && s.model.current === saved);
  s.ui.lang = "en";
  assert(s.model.statusText.startsWith("Operation not confirmed"));
  console.log(
    "Management models: diagnostic export, settings snapshots, mapping, failure retention and locale PASS",
  );
})().catch((e) => {
  console.error(e);
  process.exitCode = 1;
});
