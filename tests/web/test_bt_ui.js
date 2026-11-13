"use strict";
const assert = require("assert"),
  { harness, settle } = require("./model_harness");
(async () => {
  const h = harness(),
    m = h.model;
  await m.refresh();
  assert(m.btStartDisabled);
  assert(!h.calls.some((c) => c.method === "POST"));
  h.advance(5001);
  await m.refresh();
  assert(!m.btStartDisabled);
  let release;
  h.responder(async (route, method, body, previous) => {
    if (route === "/bluetooth/start")
      await new Promise((r) => {
        release = r;
      });
    return previous(route, method, body);
  });
  const starting = m.startBT();
  await settle();
  await m.startBT();
  assert.equal(h.calls.filter((c) => c.route.endsWith("/start")).length, 1);
  release();
  await starting;
  assert(m.btRunning);
  h.responder((route, method, body, previous) => {
    if (route.endsWith("/scan")) throw Error("hardware_pair_failed");
    return previous(route, method, body);
  });
  await m.beginSearch();
  assert(m.statusText.includes("hardware_pair_failed"));
  assert(!m.ready);
  h.responder((route, method, body, previous) =>
    route.endsWith("/scan") ? ((h.state.bt.scan_seconds = 60), {}) : previous(route, method, body),
  );
  await m.refresh();
  await m.beginSearch();
  assert(m.searchDisabled);
  const count = h.calls.length;
  await m.beginSearch();
  assert.equal(h.calls.length, count);
  h.state.bt.system_assignment = true;
  await m.refresh();
  assert(!m.appAssignment);
  const before = h.calls.length;
  await m.assignBT({ currentTarget: { dataset: { slot: "0", identity: "A" } } });
  assert.equal(h.calls.length, before);
  await m.stopBT();
  assert(!m.btRunning);
  // Keep option identities untouched when the server's user list has not changed.
  const users = m.users;
  await m.refresh();
  assert.strictEqual(m.users, users);
  m.selectedUser = "2";
  m.userChanged();
  assert(!m.stableUser);
  h.advance(5001);
  await m.refresh();
  assert(m.stableUser && m.selectedUser === "2");
  const clock = harness();
  clock.model.acceptBluetooth({ ...clock.state.bt, scan_id: 1, scan_seconds: 60 });
  assert.equal(clock.model.scanRemaining, 60);
  clock.model.now += 4000;
  assert.equal(
    clock.model.scanRemaining,
    56,
    "wall clock advances countdown when performance clock stalls",
  );
  clock.model.acceptBluetooth({ ...clock.state.bt, scan_id: 1, scan_seconds: 55 });
  assert(clock.model.scanRemaining <= 55, "polling cannot extend the same scan");
  clock.model.acceptBluetooth({ ...clock.state.bt, scan_id: 2, scan_seconds: 60 });
  assert.equal(clock.model.scanRemaining, 60, "new scans get a fresh countdown");
  clock.model.acceptBluetooth({ ...clock.state.bt, scan_id: 2, scan_seconds: 0 });
  assert.equal(clock.model.scanRemaining, 0);
  const preferred = harness();
  preferred.state.users.preferred_user_id = 2;
  await preferred.model.refresh();
  assert.equal(
    preferred.model.selectedUser,
    "2",
    "prefer backend initial/foreground account policy",
  );
  console.log(
    "Bluetooth model: stable users, serialization, pairing lease, failures and experimental isolation PASS",
  );
})().catch((e) => {
  console.error(e);
  process.exitCode = 1;
});
