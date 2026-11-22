"use strict";
const assert = require("assert"),
  { harness, settle } = require("./model_harness");
(async () => {
  const h = harness(),
    m = h.model;
  h.state.usb.rejected = [
    { node: "/dev/ugen2.7", vid: "3537", pid: "1040", reason: "claim_interface", errno: 16 },
    {
      node: "/dev/ugen0.1",
      vid: "0000",
      pid: "0000",
      reason: "control_open",
      device_descriptor_ok: false,
      errno: 6,
    },
    {
      node: "/dev/ugen2.1",
      vid: "0000",
      pid: "0000",
      reason: "device_descriptor",
      device_descriptor_ok: false,
      errno: 25,
    },
    { node: "/dev/ugen2.2", vid: "054c", pid: "0ce6", reason: "system_device_excluded", errno: 0 },
  ];
  await m.refresh();
  assert(m.usbRejected.length === 1 && m.usbRejected[0].detail.includes("claim_interface"));
  assert(m.usbRejected[0].detail.includes("errno=16"));
  assert(m.usbDevices.length === 0);
  assert(m.usb.rejected.length === 4, "Full scan evidence remains available to diagnostics");
  m.usb.devices = [{ node: "/dev/ugen2.7", connected: true }];
  assert.equal(m.usbRejected.length, 0, "An active connection hides stale failures on its node");
  m.usb.devices = [];
  m.usb.rejected.push({
    node: "/dev/ugen3.2",
    vid: "0000",
    pid: "0000",
    device_descriptor_ok: true,
    reason: "hid_first_input_timeout",
  });
  assert.equal(m.usbRejected.length, 2, "A descriptor-verified zero-ID HID failure stays visible");
  m.usb.rejected.pop();
  assert(!m.usbStartDisabled, "USB can start immediately after loading signed-in users");
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
  const choiceDevice = {
    slot: 0,
    node: "/dev/ugen1.2",
    vid: "3537",
    pid: "1040",
    connected: true,
    input_frames: 1,
    bound: true,
    user_id: 1,
  };
  h.state.usb.devices = [choiceDevice];
  await m.refresh();
  m.selectedDeviceKey = m.usbDevices[0].key;
  assert.equal(m.detailAssignmentDisabled, false);
  const chosen = m.selectedUser;
  await m.reselectDevice({
    currentTarget: {
      dataset: {
        source: "usb",
        slot: "0",
        identity: "/dev/ugen1.2",
      },
    },
  });
  assert.deepEqual(h.calls.find((c) => c.route === "/usb/reselect-user").body, {
    slot: 0,
    node: "/dev/ugen1.2",
  });
  assert.equal(m.selectedUser, chosen);
  assert(m.statusText.includes("PS"));
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
  const input = harness(),
    waiting = input.model.deviceView(
      {
        node: "/dev/ugen1.2",
        slot: 0,
        input_frames: 0,
        waiting_for_input: true,
        first_input_remaining_ms: 29999,
      },
      false,
    );
  assert.equal(waiting.inputStatus, input.model.t("usbHidWaiting", { seconds: 30 }));
  const parsed = input.model.deviceView(
    { node: "/dev/ugen1.2", slot: 0, input_frames: 1, waiting_for_input: false },
    false,
  );
  assert.equal(parsed.inputStatus, input.model.t("inputParsed"));
  input.model.usb.rejected = [
    { node: "/dev/ugen1.2", vid: "3537", pid: "1041", reason: "hid_first_input_timeout", errno: 0 },
  ];
  assert(input.model.usbInputTimedOut);
  assert(input.model.usbRejected[0].detail.includes(input.model.t("usbHidInputTimeout")));

  console.log(
    "USB model: coexistence, account gate, assignment identity, failures and stale polling PASS",
  );
})().catch((e) => {
  console.error(e);
  process.exitCode = 1;
});
