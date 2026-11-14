"use strict";
const assert = require("assert"),
  { harness, settle } = require("./model_harness");
(async () => {
  const h = harness(),
    m = h.model;
  h.state.usb.rejected = [
    { node: "/dev/ugen2.7", vid: "3537", pid: "1040", reason: "claim_interface", errno: 16 },
  ];
  await m.refresh();
  assert(m.usbRejected.length === 1 && m.usbRejected[0].detail.includes("claim_interface"));
  assert(m.usbRejected[0].detail.includes("errno=16"));
  assert(m.usbDevices.length === 0);
  assert(m.usbStartDisabled);
  h.advance(5001);
  await m.refresh();
  assert(!m.usbStartDisabled);
  h.state.bt.state = "on";
  await m.refresh();
  assert(!m.usbStartDisabled);
  h.state.session.enabled = true;
  await m.refresh();
  assert(m.usbStartDisabled);
  h.state.session.enabled = false;
  await m.refresh();
  let release;
  h.responder(async (route, method, body, previous) => {
    if (route === "/usb/start")
      await new Promise((r) => {
        release = r;
      });
    return previous(route, method, body);
  });
  const starting = m.startUSB();
  await settle();
  assert(m.btStartDisabled && m.usbBusy);
  await m.startUSB();
  release();
  await starting;
  assert(m.usbRunning);
  const event = { currentTarget: { dataset: { slot: "0", identity: "/dev/ugen1.2" } } };
  await m.assignUSB(event);
  assert.deepEqual(h.calls.find((c) => c.route === "/usb/assign").body, {
    slot: 0,
    user_id: 1,
    node: "/dev/ugen1.2",
  });
  await m.stopUSB();
  h.responder((route, method, body, previous) => {
    if (route === "/usb") throw Error("query_failed");
    return previous(route, method, body);
  });
  await m.refresh();
  assert(!m.ready && m.btStartDisabled && m.usbStartDisabled);
  const malformed = harness();
  malformed.state.bt.state = "unknown";
  await malformed.model.refresh();
  assert(
    !malformed.model.ready && malformed.model.btStartDisabled && malformed.model.usbStartDisabled,
    "unknown source state must fail closed",
  );
  // An in-flight GET cannot roll back a later operation's result.
  const a = harness(),
    n = a.model;
  await n.refresh();
  a.advance(6000);
  await n.refresh();
  let finish;
  a.responder(async (route, method, body, previous) => {
    if (route === "/usb" && method === "GET" && !finish)
      return new Promise((r) => {
        finish = () => r({ state: "off", devices: [], bound: false, pending: false });
      });
    return previous(route, method, body);
  });
  const refreshing = n.refresh();
  await settle();
  await n.startUSB();
  finish();
  await refreshing;
  assert.equal(n.usb.state, "on");
  // Reconnecting the same node in another slot leaves a diagnostic history entry.
  // Show only live connections and never collapse two genuine pads by model name.
  a.state.usb.devices = [
    { slot: 0, node: "/dev/ugen1.2", model: "GameSir Nova Lite", connected: false },
    { slot: 1, node: "/dev/ugen1.2", model: "GameSir Nova Lite", connected: true },
    { slot: 2, node: "/dev/ugen1.3", model: "GameSir Nova Lite", connected: true },
  ];
  await n.refresh();
  assert.equal(n.usbDevices.length, 2);
  assert.deepEqual(
    n.usbDevices.map((d) => d.identity),
    ["/dev/ugen1.2", "/dev/ugen1.3"],
  );
  a.state.usb.devices.forEach((d) => {
    d.connected = false;
  });
  await n.refresh();
  assert.equal(n.usbDevices.length, 0);
  console.log(
    "USB model: coexistence, account gate, assignment identity, failures and stale polling PASS",
  );
})().catch((e) => {
  console.error(e);
  process.exitCode = 1;
});
