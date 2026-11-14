"use strict";
const assert = require("assert"),
  { harness, settle } = require("./model_harness");
(async () => {
  const h = harness(),
    m = h.model;
  await m.refresh();
  assert(m.logs.includes("[bluetooth] sample connected"));
  m.paused = true;
  const reads = h.calls.filter((c) => c.route.startsWith("/logs")).length;
  await m.refresh();
  assert.equal(h.calls.filter((c) => c.route.startsWith("/logs")).length, reads);
  assert(m.logs.includes("[bluetooth] sample connected"));
  m.paused = false;
  h.responder((route, method, body, previous) => {
    if (route === "/usb") throw Error("usb_status_unavailable");
    return previous(route, method, body);
  });
  await m.refresh();
  assert(
    !m.ready && m.logs.includes("[bluetooth] sample connected"),
    "Runtime logs remain readable when a status endpoint fails",
  );
  const bundle = await m.diagnosticText();
  assert(
    bundle.includes("# /bluetooth") &&
      bundle.includes("# /usb") &&
      bundle.includes("# /usb/diagnostics") &&
      bundle.includes("# /bluetooth/diagnostics") &&
      bundle.includes("# runtime.log") &&
      bundle.includes("# /core/events?level=trace"),
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
      usb_auto_start: false,
      usb_auto_user: 1,
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
  s.model.form.usb_auto_start = true;
  s.model.form.bluetooth_auto_start = true;
  s.model.form.bluetooth_auto_trigger = "resume";
  assert(s.model.autoConflict && s.model.saveDisabled);
  const saveCount = s.calls.filter((c) => c.method === "POST").length;
  await s.model.save();
  assert.equal(s.calls.filter((c) => c.method === "POST").length, saveCount);
  s.model.form.bluetooth_auto_trigger = "login";
  assert(!s.model.autoConflict && !s.model.saveDisabled);
  await s.model.save();
  assert(s.calls.filter((c) => c.method === "POST").at(-1).body.usb_auto_start);
  assert.equal(s.calls.filter((c) => c.method === "POST").at(-1).body.usb_auto_user, 1);

  const lastSaved = s.model.current;
  s.responder(() => {
    throw Error("disk_full");
  });
  await s.model.save();
  assert(s.model.statusText.includes("disk_full") && s.model.current === lastSaved);
  s.ui.lang = "en";
  assert(s.model.statusText.startsWith("Operation not confirmed"));
  console.log(
    "Management models: diagnostic export, settings snapshots, mapping, failure retention and locale PASS",
  );
})().catch((e) => {
  console.error(e);
  process.exitCode = 1;
});
