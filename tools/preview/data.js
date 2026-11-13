"use strict";
// Injected by the local preview server only. Not packaged in the PS5 payload.
(() => {
  const realFetch = window.fetch.bind(window);
  const device = {
    slot: 0,
    address: "02:00:00:00:00:01",
    vid: "057E",
    pid: "2009",
    model: "Sample Switch Pro",
    profile: "Switch Pro",
    connected: true,
    bound: true,
    user_id: 2,
    session_id: 101,
    pending: false,
    input_frames: 42,
    battery: 75,
    charging: false,
  };
  const bt = {
    state: "on",
    reason: "",
    user_id: 2,
    running: true,
    devices: [device],
    paired: [
      { address: device.address, vid: device.vid, pid: device.pid, le: false, connected: true },
      { address: "02:00:00:00:00:02", vid: "057E", pid: "2009", le: false, connected: false },
    ],
    scan_seconds: 0,
    scan_id: 0,
    discovered: [],
  };
  if (location.pathname.startsWith("/controller")) {
    bt.state = "off";
    bt.running = false;
    bt.devices = [];
    bt.paired[0].connected = false;
  }
  const usbDevice = {
    slot: 0,
    node: "/dev/ugen1.4",
    vid: "045E",
    pid: "028E",
    model: "Sample Xbox USB",
    profile: "XInput",
    connected: true,
    bound: true,
    user_id: 1,
    input_frames: 10,
    battery: -1,
  };
  const usbHidDiagnostic = {
    node: "/dev/ugen1.5",
    vid: "3537",
    pid: "1041",
    reason: "generic_hid_not_implemented",
    errno: 0,
    protocol_hint: "generic-hid-gamepad",
    interface_summary: "IF0 alt0 03/00/00",
    hid_report_captured: 7,
    hid_report_declared: 7,
    hid_report_valid: true,
    report_descriptor_hex: "05010905a101c0",
    config_descriptor_hex: "",
    preview: true,
  };
  const usb = {
    state: "off",
    reason: "",
    user_id: 2,
    running: false,
    devices: [],
    pending: false,
    bound: false,
  };
  let session = { enabled: false, session_id: null, pending: false },
    next = 200;
  let settings = {
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
  const entries = [
    {
      level: "info",
      code: "preview_only",
      timestamp_ms: 0,
      generation: 0,
      session_id: 0,
      message: "Sample data. No PS5 device operations.",
    },
  ];
  let scanUntil = 0,
    nextDiscovery = 1;
  const candidates = [
    {
      address: "02:00:00:00:00:03",
      name: "Sample GameSir Controller",
      transport: "classic",
      address_type: 0,
      rssi: -42,
    },
    {
      address: "02:00:00:00:00:04",
      name: "Sample Xbox Bluetooth",
      transport: "ble",
      address_type: 1,
      rssi: -55,
    },
  ];
  window.fetch = async (resource, options = {}) => {
    const url = new URL(typeof resource === "string" ? resource : resource.url, location.href);
    if (url.origin !== location.origin || !url.pathname.startsWith("/api/v1/"))
      return realFetch(resource, options);
    const path = url.pathname.slice(7),
      method = options.method || "GET";
    let body = {};
    try {
      if (options.body) body = JSON.parse(options.body);
    } catch {
      return Response.json({ error: "invalid_preview_json" }, { status: 400 });
    }
    let data = {},
      status = 200;
    if (path === "/usb/diagnostics")
      data = { schema_version: 1, snapshot: true, devices: usb.rejected || [], preview: true };
    else if (path === "/session") data = { token: "preview-only-token" };
    else if (path === "/users")
      data = {
        users: [
          { user_id: 1, display_name: "Sample Player 1" },
          { user_id: 2, display_name: "Sample Player 2" },
        ],
      };
    else if (path === "/system")
      data = { mode: "DEMO PREVIEW", backend: "simulated", hardware: false };
    else if (path === "/settings") {
      if (method === "POST") settings = JSON.parse(JSON.stringify(body));
      data = settings;
    } else if (path === "/launcher")
      data = { state: "unsupported", title_id: "UNIC00001", port: Number(location.port) };
    else if (path === "/logs") data = { entries, dropped: 0 };
    else if (path === "/diagnostics")
      data = { preview: true, runtime: "No hardware. Changes reset on reload." };
    else if (path === "/test-controller") {
      if (method === "DELETE") session = { enabled: false, session_id: null, pending: false };
      else if (method === "POST") {
        if (bt.state === "on" || usb.state === "on") {
          status = 409;
          data = { error: "stop_physical_sources_first" };
        } else
          session = {
            enabled: true,
            session_id: session.session_id || ++next,
            pending: false,
            user_id: body.user_id,
          };
      }
      if (status === 200) data = session;
    } else if (path === "/test-controller/input") data = { accepted: true };
    else if (path === "/bluetooth") {
      if (bt.state === "on")
        bt.devices.forEach((d) => {
          d.input_frames++;
        });
      bt.scan_seconds = Math.max(0, Math.ceil((scanUntil - performance.now()) / 1000));
      data = bt;
    } else if (path === "/usb") {
      if (usb.state === "on") usbDevice.input_frames++;
      data = usb;
    } else if (path === "/bluetooth/scan/stop" && method === "POST") {
      scanUntil = 0;
      bt.scan_seconds = 0;
      entries.push({
        level: "info",
        code: "preview_scan_stop",
        timestamp_ms: Math.floor(performance.now()),
        generation: 0,
        session_id: 0,
      });
    } else if (/^\/(bluetooth|usb)\/(start|stop|scan|pair|assign|forget)$/.test(path)) {
      const [, source, action] = path.split("/"),
        state = source === "bluetooth" ? bt : usb;
      if (method !== "POST") {
        status = 405;
        data = { error: "method_not_allowed" };
      } else if (action === "start") {
        state.state = "on";
        state.running = true;
        state.user_id = body.user_id;
        if (source === "bluetooth") {
          state.devices = bt.paired.some((bond) => bond.address === device.address) ? [device] : [];
          bt.paired.forEach((bond) => {
            bond.connected = state.devices.some((d) => d.address === bond.address);
          });
        } else {
          state.devices = [usbDevice];
          state.rejected = [usbHidDiagnostic];
        }
      } else if (action === "stop") {
        state.state = "off";
        state.running = false;
        state.devices = [];
        if (source === "bluetooth") {
          scanUntil = 0;
          state.scan_seconds = 0;
          state.discovered = [];
        }
        if (source === "bluetooth")
          bt.paired.forEach((bond) => {
            bond.connected = false;
          });
      } else if (action === "scan") {
        if (source !== "bluetooth" || bt.state !== "on") {
          status = 409;
          data = { error: "bluetooth_not_ready" };
        } else {
          bt.scan_id++;
          scanUntil = performance.now() + body.seconds * 1000;
          bt.scan_seconds = body.seconds;
          bt.discovered = candidates
            .filter((candidate) => !bt.paired.some((bond) => bond.address === candidate.address))
            .map((d) => ({
              ...d,
              id: nextDiscovery++,
              state: "discovered",
              reason: "",
            }));
        }
      } else if (action === "pair") {
        const found = bt.discovered.find((d) => d.id === body.discovery_id);
        if (!found || found.state !== "discovered" || bt.state !== "on") {
          status = 409;
          data = { error: "device_not_available_or_pairing" };
        } else {
          found.state = "pairing";
          setTimeout(() => {
            if (bt.state !== "on" || !bt.discovered.includes(found)) return;
            found.state = "connected";
            bt.devices.push({
              ...device,
              slot: bt.devices.length,
              address: found.address,
              model: found.name,
              vid: found.transport === "ble" ? "045E" : "057E",
              pid: found.transport === "ble" ? "0B13" : "2009",
              transport: found.transport,
              profile: found.transport === "ble" ? "Xbox" : "Switch Pro",
              bound: false,
              user_id: -1,
            });
            bt.paired.push({
              address: found.address,
              vid: found.transport === "ble" ? "045E" : "057E",
              pid: found.transport === "ble" ? "0B13" : "2009",
              le: found.transport === "ble",
              connected: true,
            });
          }, 400);
        }
      } else if (action === "assign") {
        const target = state.devices.find(
          (d) =>
            d.slot === body.slot &&
            (source === "bluetooth" ? d.address === body.address : d.node === body.node),
        );
        if (target) {
          target.user_id = body.user_id;
          target.bound = true;
        } else {
          status = 409;
          data = { error: "device_not_available" };
        }
      } else if (action === "forget") {
        bt.paired = bt.paired.filter((bond) => bond.address !== body.address);
        bt.devices = bt.devices.filter((d) => d.address !== body.address);
      }
      entries.push({
        level: "info",
        code: "preview_" + action,
        source,
        timestamp_ms: Math.floor(performance.now()),
        generation: 0,
        session_id: 0,
      });
      if (entries.length > 30) entries.shift();
    } else if (path === "/stop") {
      bt.state = usb.state = "off";
      bt.devices = usb.devices = [];
      session.enabled = false;
      data = { stopped: true, preview: true };
    } else {
      status = 404;
      data = { error: "unknown_preview_route" };
    }
    return new Response(JSON.stringify(data), {
      status,
      headers: { "Content-Type": "application/json" },
    });
  };
  document.addEventListener("DOMContentLoaded", () => {
    const banner = document.createElement("div");
    banner.className = "preview-notice";
    banner.setAttribute("role", "status");
    const update = () => {
      banner.textContent = window.UCUI
        ? UCUI.get().labels.preview
        : "DEMO PREVIEW · No hardware operations";
    };
    update();
    document.body.prepend(banner);
    window.addEventListener("uc-language", update);
  });
})();
