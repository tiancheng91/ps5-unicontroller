"use strict";
const assert = require("assert");
const { harness, settle } = require("./model_harness");
(async () => {
  const h = harness(),
    m = h.model;
  h.state.usb.state = "on";
  h.state.usb.devices = [0, 1].map((slot) => ({
    slot,
    connected: true,
    node: "/dev/ugen1." + (slot + 2),
    vid: "3537",
    pid: "1040",
    profile: "xinput-usb",
    session_id: 100 + slot,
    settings_key: "usb:/dev/ugen1." + (slot + 2) + ":3537:1040",
    known_device: slot === 0,
    rumble: slot === 0,
    swap_ab: false,
    rumble_available: true,
  }));
  await m.refresh();
  m.selectedDeviceKey = m.usbDevices[0].key;
  assert(!m.deviceOptionsDisabled);
  let release;
  h.responder(async (route, method, body, previous) => {
    if (route !== "/device-settings") return previous(route, method, body);
    await new Promise((r) => {
      release = r;
    });
    const d = h.state.usb.devices.find((d) => d.settings_key === body.key);
    d.rumble = body.rumble;
    d.swap_ab = body.swap_ab;
    return { saved: true };
  });
  const input = { dataset: { option: "swap_ab" }, checked: true };
  const save = m.changeDeviceOption({ currentTarget: input });
  await settle();
  assert(m.deviceBusy && m.deviceOptionsDisabled);
  await m.changeDeviceOption({ currentTarget: input });
  assert.equal(h.calls.filter((c) => c.route === "/device-settings").length, 1);
  release();
  await save;
  assert.equal(m.detailDevice.swap_ab, true);
  assert.equal(h.state.usb.devices[1].swap_ab, false);
  assert.equal(h.state.usb.devices[1].rumble, false);
  assert.deepEqual(
    JSON.parse(JSON.stringify(h.calls.find((c) => c.route === "/device-settings").body)),
    {
      key: "usb:/dev/ugen1.2:3537:1040",
      session_id: 100,
      rumble: true,
      swap_ab: true,
    },
  );
  h.responder((route, method, body, previous) => {
    if (route === "/device-settings") throw Error("device_changed");
    return previous(route, method, body);
  });
  const failed = { dataset: { option: "rumble" }, checked: false };
  await m.changeDeviceOption({ currentTarget: failed });
  assert(failed.checked && m.error && !m.deviceBusy);
  m.selectedDeviceKey = m.usbDevices[1].key;
  assert(m.deviceRumbleHint.includes("未知设备"));
  m.selectedDeviceKey = m.usbDevices[0].key;
  h.state.usb.devices[0].rumble_available = true;
  h.state.usb.devices[0].rumble_enabled = true;
  await m.refresh();
  h.responder((route, method, body, previous) => {
    if (route === "/rumble-test") return { queued: true, duration_ms: 500 };
    return previous(route, method, body);
  });
  await m.testDeviceRumble();
  assert.equal(h.calls.filter((c) => c.route === "/rumble-test").length, 1);
  assert.equal(h.calls.find((c) => c.route === "/rumble-test").body.session_id, 100);
  await m.testDeviceRumble();
  assert.equal(h.calls.filter((c) => c.route === "/rumble-test").length, 1);
  h.advance(1001);
  await m.refresh();
  assert(!m.rumbleTestDisabled && m.message === "rumbleTestQueued");
  h.state.usb.devices[0].rumble_enabled = false;
  await m.refresh();
  assert(m.rumbleTestDisabled && m.rumbleTestHint.includes("总开关"));
  h.state.usb.devices[0].rumble_available = false;
  await m.refresh();
  assert(m.rumbleTestDisabled && m.rumbleTestHint.includes("没有可用"));
  h.state.usb.devices = [];
  await m.refresh();
  assert(m.deviceOptionsDisabled);
  console.log(
    "Device options UI: independent switches, experimental label, request identity, busy and failure PASS",
  );
})().catch((e) => {
  console.error(e);
  process.exitCode = 1;
});
