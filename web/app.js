"use strict";
document.addEventListener("alpine:init", () => {
  function validSource(data, wireless) {
    return (
      !!data &&
      ["off", "starting", "on", "failed"].includes(data.state) &&
      Array.isArray(data.devices) &&
      (wireless
        ? Array.isArray(data.paired) &&
          Array.isArray(data.discovered) &&
          Number.isInteger(data.scan_seconds) &&
          data.scan_seconds >= 0 &&
          data.scan_seconds <= 120 &&
          Number.isInteger(data.scan_id) &&
          data.scan_id >= 0
        : typeof data.pending === "boolean" && typeof data.bound === "boolean")
    );
  }
  Alpine.data("control", () => ({
    users: [],
    selectedUser: "",
    usersKey: "",
    stableSince: 0,
    now: Date.now(),
    bt: { state: "off", devices: [], paired: [], discovered: [], scan_seconds: 0, scan_id: 0 },
    usb: { state: "off", devices: [], pending: false, bound: false },
    session: {},
    system: {},
    logs: "",
    level: "info",
    paused: false,
    ready: false,
    deviceBusy: false,
    rumbleTestCooldown: 0,
    rumbleNoticeUntil: 0,
    btBusy: false,
    usbBusy: false,
    stopping: false,
    refreshing: false,
    btEpoch: 0,
    usbEpoch: 0,
    timer: null,
    clockTimer: null,
    scanDeadline: 0,
    scanWallDeadline: 0,
    scanNow: performance.now(),
    message: "waiting",
    messageValues: {},
    error: false,
    init() {
      this.refresh();
      this.clockTimer = setInterval(() => {
        this.now = Date.now();
        this.scanNow = performance.now();
      }, 1000);
      this.timer = setInterval(() => {
        if (!document.hidden) this.refresh();
      }, 2000);
    },
    destroy() {
      clearInterval(this.timer);
      clearInterval(this.clockTimer);
    },
    t(key, values) {
      return Alpine.store("ui").t(key, values);
    },
    get errorClass() {
      return this.error ? "error" : "";
    },
    get statusText() {
      return this.t(this.message, this.messageValues);
    },
    notice(key, values = {}, error = false) {
      this.message = key;
      this.messageValues = values;
      this.error = error;
    },
    get stableUser() {
      return !!this.selectedUser && this.now - this.stableSince >= 5000;
    },
    get btRunning() {
      return ["on", "starting"].includes(this.bt.state);
    },
    get usbRunning() {
      return ["on", "starting"].includes(this.usb.state) || this.usb.pending || this.usb.bound;
    },
    get virtualRunning() {
      return !!(this.session.enabled || this.session.session_id || this.session.pending);
    },
    get busy() {
      return this.btBusy || this.usbBusy || this.stopping || this.deviceBusy;
    },
    get btStartDisabled() {
      return !this.ready || !this.stableUser || this.busy || this.btRunning || this.virtualRunning;
    },
    get usbStartDisabled() {
      return (
        !!this.bt.system_assignment ||
        !this.ready ||
        !this.stableUser ||
        this.busy ||
        this.usbRunning ||
        this.virtualRunning
      );
    },
    get btStopDisabled() {
      return this.btBusy || !["on", "starting", "failed"].includes(this.bt.state);
    },
    get usbStopDisabled() {
      return this.usbBusy || !["on", "starting", "failed"].includes(this.usb.state);
    },
    get pairDisabled() {
      return (
        !this.ready ||
        this.busy ||
        this.bt.state !== "on" ||
        this.bt.discovered.some((d) => d.state === "pairing")
      );
    },
    get scanRemaining() {
      // Let either clock advance the display if the other stalls.
      // Status polling can only shorten the current scan.
      return Math.max(
        0,
        Math.ceil(
          Math.min(this.scanDeadline - this.scanNow, this.scanWallDeadline - this.now) / 1000,
        ),
      );
    },
    get scanning() {
      return this.scanRemaining > 0;
    },
    acceptBluetooth(latest) {
      const now = performance.now();
      this.now = Date.now();
      this.scanNow = now;
      if (latest.scan_seconds > 0) {
        const deadline = now + latest.scan_seconds * 1000;
        const wallDeadline = this.now + latest.scan_seconds * 1000;
        this.scanWallDeadline =
          latest.scan_id !== this.bt.scan_id || !this.scanWallDeadline
            ? wallDeadline
            : Math.min(this.scanWallDeadline, wallDeadline);
        this.scanDeadline =
          latest.scan_id !== this.bt.scan_id || !this.scanDeadline
            ? deadline
            : Math.min(this.scanDeadline, deadline);
      } else {
        this.scanDeadline = 0;
        this.scanWallDeadline = 0;
      }
      this.bt = latest;
    },
    get discovered() {
      return this.bt.discovered.map((d) => ({
        ...d,
        key: String(d.id),
        name: d.name || this.t("unnamedController"),
        transport: d.transport === "ble" ? "Bluetooth LE" : "Bluetooth Classic",
        stateText: this.t("discovery_" + d.state),
        reasonText: d.reason || "",
        signal: Number.isFinite(d.rssi) ? d.rssi + " dBm" : "—",
        disabled: this.pairDisabled || !["discovered", "failed"].includes(d.state),
        buttonText: this.t(
          d.state === "failed"
            ? "retryPair"
            : d.state === "pairing"
              ? "discovery_pairing"
              : d.state === "paired"
                ? "discovery_paired"
                : d.state === "connected"
                  ? "connected"
                  : "pairSelected",
        ),
      }));
    },
    get noDiscovered() {
      return this.bt.discovered.length === 0;
    },
    get assignmentDisabled() {
      return !this.ready || !this.stableUser || this.busy;
    },
    get btStateText() {
      return this.t(this.bt.state);
    },
    get usbStateText() {
      return this.t(this.usb.state);
    },
    get btServiceNote() {
      return [this.bt.system_assignment ? this.t("systemChoice") : "", this.bt.reason || ""]
        .filter(Boolean)
        .join(" · ");
    },
    get usbServiceNote() {
      return this.usb.reason || "";
    },
    get showServiceGate() {
      return !this.stableUser;
    },
    get btStatus() {
      return (
        this.t(this.bt.state) +
        (this.bt.system_assignment ? " · " + this.t("systemChoice") : "") +
        (this.bt.reason ? " · " + this.bt.reason : "")
      );
    },
    get usbStatus() {
      return this.t(this.usb.state) + (this.usb.reason ? " · " + this.usb.reason : "");
    },
    get scanText() {
      return this.scanning
        ? this.t("scanCountdown", { seconds: this.scanRemaining })
        : this.t("scanFinished");
    },
    get devices() {
      return [...this.btDevices, ...this.usbDevices];
    },
    get noDevices() {
      return !this.devices.length;
    },
    assignDevice(event) {
      return event.currentTarget.dataset.source === "bluetooth"
        ? this.assignBT(event)
        : this.assignUSB(event);
    },
    get btDevices() {
      return this.bt.devices.map((d) => this.deviceView(d, true));
    },
    get usbRejected() {
      return (this.usb.rejected || []).map((d) => ({
        ...d,
        detail: d.vid + ":" + d.pid + " · " + d.reason + " · errno=" + d.errno,
        diagnostic:
          (d.protocol_hint || "unknown") +
          " · " +
          (d.interface_summary || "-") +
          " · HID " +
          (d.hid_report_captured || 0) +
          "/" +
          (d.hid_report_declared || 0),
      }));
    },
    get usbDevices() {
      return this.usb.devices.map((d) => this.deviceView(d, false));
    },
    get bonds() {
      return this.bt.paired.map((d) => ({
        ...d,
        key: (d.le ? "ble:" : "classic:") + d.address,
        detail:
          d.vid +
          ":" +
          d.pid +
          " · " +
          (d.le ? "BLE" : "Classic") +
          " · " +
          this.t(d.connected ? "connected" : "disconnected"),
      }));
    },
    deviceView(d, wireless) {
      return {
        ...d,
        key: (wireless ? "bt:" : "usb:") + (wireless ? d.address : d.node) + ":" + d.slot,
        source: wireless ? "bluetooth" : "usb",
        transport: wireless ? (d.transport === "ble" ? "Bluetooth LE" : "Bluetooth") : "USB",
        assignable: !wireless || !this.bt.system_assignment,
        wireless,
        system_assignment: !!d.system_assignment,
        known_device: !!d.known_device,
        rumble: !!d.rumble,
        rumble_available: !!d.rumble_available,
        rumble_enabled: !!d.rumble_enabled,
        swap_ab: !!d.swap_ab,
        settings_key: d.settings_key || "",
        name: d.model || d.profile || "HID",
        identity: wireless ? d.address : d.node,
        detail: d.vid + ":" + d.pid + " · " + (d.profile || "HID"),
        input: this.t("frames", { count: d.input_frames }),
        inputStatus: this.t(Number(d.input_frames) > 0 ? "inputParsed" : "inputWaiting"),
        accountText: d.user_id > 0 ? this.t("account", { id: d.user_id }) : this.t("unbound"),
        stateText: d.system_assignment
          ? this.t(d.output_active ? "outputActive" : "outputWaiting")
          : this.t(d.bound ? "bound" : "unbound"),
        batteryText:
          Number(d.battery) >= 0
            ? this.t("battery", { percent: d.battery })
            : this.t(wireless ? "batteryUnknown" : "wired"),
      };
    },
    get appAssignment() {
      return !this.bt.system_assignment;
    },
    get systemJSON() {
      return JSON.stringify(this.system, null, 2);
    },
    get sessionJSON() {
      return JSON.stringify(this.session, null, 2);
    },
    drawer: "",
    selectedDeviceKey: "",
    searchBusy: false,
    searchEpoch: 0,
    drawerOpener: null,
    get drawerOpen() {
      return !!this.drawer;
    },
    get addingDevice() {
      return this.drawer === "add";
    },
    get viewingDevice() {
      return this.drawer === "device";
    },
    get drawerTitle() {
      return this.t(this.addingDevice ? "addBluetooth" : "deviceSettings");
    },
    get hasDevices() {
      return !!this.devices.length;
    },
    get deviceCount() {
      return this.t("deviceCount", { count: this.devices.length });
    },
    get offlineBonds() {
      return this.bonds.filter(
        (b) => !b.connected && !this.bt.devices.some((d) => d.address === b.address),
      );
    },
    get noOfflineBonds() {
      return !this.offlineBonds.length;
    },
    get transportJSON() {
      return JSON.stringify({ bluetooth: this.bt, usb: this.usb }, null, 2);
    },
    get detailDevice() {
      return (
        this.devices.find((d) => d.key === this.selectedDeviceKey) || {
          source: "",
          slot: -1,
          name: this.t("deviceGone"),
          transport: "—",
          identity: "—",
          detail: "—",
          input: "—",
          stateText: "—",
          batteryText: "—",
          assignable: false,
          system_assignment: false,
          known_device: false,
          rumble: false,
          rumble_available: false,
          rumble_enabled: false,
          swap_ab: false,
          settings_key: "",
        }
      );
    },
    get detailMissing() {
      return !this.devices.some((d) => d.key === this.selectedDeviceKey);
    },
    get deviceOptionsDisabled() {
      return this.busy || this.detailMissing || !this.detailDevice.settings_key;
    },
    get deviceRumbleExperimental() {
      return !this.detailMissing && !this.detailDevice.known_device;
    },
    get deviceRumbleHint() {
      if (!this.detailDevice.known_device) return this.t("experimentalRumbleHint");
      return this.t(
        this.detailDevice.rumble_available ? "deviceRumbleHint" : "rumbleUnavailableHint",
      );
    },
    get rumbleTestUnavailable() {
      return (
        this.detailMissing ||
        !this.detailDevice.settings_key ||
        this.detailDevice.connected === false ||
        !this.detailDevice.rumble_available ||
        !this.detailDevice.rumble_enabled
      );
    },
    get rumbleTestDisabled() {
      return this.busy || this.rumbleTestUnavailable || this.now < this.rumbleTestCooldown;
    },
    get rumbleTestHint() {
      if (this.detailMissing || this.detailDevice.connected === false) return this.t("deviceGone");
      if (!this.detailDevice.rumble_available) return this.t("rumbleTestUnsupported");
      if (!this.detailDevice.rumble_enabled) return this.t("rumbleTestEnableHint");
      return this.t("rumbleTestHint");
    },
    async testDeviceRumble() {
      if (this.rumbleTestDisabled) return;
      const d = this.detailDevice;
      this.deviceBusy = true;
      try {
        await UCAPI.request("/rumble-test", "POST", {
          key: d.settings_key,
          session_id: d.session_id || 0,
        });
        this.now = Date.now();
        this.rumbleTestCooldown = this.now + 1000;
        this.rumbleNoticeUntil = this.now + 3000;
        this.notice("rumbleTestQueued", { name: d.name });
      } catch (e) {
        const reasons = {
          rumble_disabled: "rumbleTestEnableHint",
          rumble_test_unavailable: "rumbleTestUnsupported",
          rumble_test_busy: "rumbleTestBusy",
          device_changed: "deviceGone",
        };
        if (reasons[e.message]) this.notice(reasons[e.message], {}, true);
        else this.notice("operationFailed", { error: e.message }, true);
      } finally {
        this.deviceBusy = false;
      }
    },
    async changeDeviceOption(event) {
      const d = this.detailDevice;
      const field = event.currentTarget.dataset.option;
      if (this.deviceOptionsDisabled || !["rumble", "swap_ab"].includes(field)) return;
      const input = event.currentTarget;
      const body = {
        key: d.settings_key,
        session_id: d.session_id || 0,
        rumble: !!d.rumble,
        swap_ab: !!d.swap_ab,
      };
      body[field] = input.checked;
      this.deviceBusy = true;
      ++this.btEpoch;
      ++this.usbEpoch;
      try {
        await UCAPI.request("/device-settings", "POST", body);
        this.notice("deviceSettingsSaved");
      } catch (e) {
        input.checked = !!d[field];
        this.notice("operationFailed", { error: e.message }, true);
      } finally {
        this.deviceBusy = false;
        await this.refresh();
      }
    },
    get detailAssignmentDisabled() {
      return this.assignmentDisabled || this.detailMissing;
    },
    openAdd(event) {
      this.openDrawer("add", event);
    },
    openDevice(event) {
      this.selectedDeviceKey = event.currentTarget.dataset.key;
      this.openDrawer("device", event);
    },
    openDrawer(name, event) {
      this.drawerOpener = event.currentTarget;
      this.drawer = name;
      Alpine.nextTick(() => document.querySelector("[data-close-drawer]")?.focus());
    },
    closeDrawer() {
      if (!this.drawer) return;
      ++this.searchEpoch;
      this.drawer = "";
      const opener = this.drawerOpener;
      this.drawerOpener = null;
      Alpine.nextTick(() => {
        const target =
          opener?.isConnected && !opener.disabled
            ? opener
            : document.querySelector('nav a[aria-current="page"]');
        target?.focus();
      });
    },
    closeBackdrop(event) {
      if (event.target === event.currentTarget) this.closeDrawer();
    },
    drawerKeys(event) {
      if (event.key === "Escape") {
        event.preventDefault();
        event.stopPropagation();
        this.closeDrawer();
      }
      if (event.key !== "Tab") return;
      const focusable = [
        ...document.querySelectorAll(
          "[data-drawer] button:not(:disabled),[data-drawer] select:not(:disabled),[data-drawer] summary,[data-drawer] input:not(:disabled)",
        ),
      ].filter((el) => el.getClientRects().length);
      const index = focusable.indexOf(document.activeElement);
      if ((event.shiftKey && index <= 0) || (!event.shiftKey && index === focusable.length - 1)) {
        event.preventDefault();
        (event.shiftKey ? focusable[focusable.length - 1] : focusable[0])?.focus();
      }
    },
    get searchDisabled() {
      return (
        this.searchBusy ||
        this.btBusy ||
        this.scanning ||
        this.bt.state === "starting" ||
        (this.bt.state === "on" ? this.pairDisabled : this.btStartDisabled)
      );
    },
    get searchNeedsReady() {
      return this.bt.state !== "on" && this.btStartDisabled;
    },
    get searchTitle() {
      return this.scanning
        ? this.scanText
        : this.t(this.searchBusy || this.bt.state === "starting" ? "starting" : "searchTitle");
    },
    get searchButton() {
      return this.t(this.bt.state === "on" ? "startSearch" : "enableAndSearch");
    },
    get stopSearchDisabled() {
      return this.btBusy || !this.scanning;
    },
    stopSearch() {
      return this.physical("bluetooth", "scan/stop", {});
    },
    async beginSearch() {
      if (this.searchDisabled) return;
      this.searchBusy = true;
      const operation = ++this.searchEpoch;
      try {
        if (this.bt.state !== "on") {
          const started = await this.startBT();
          if (!started) return;
          const deadline = Date.now() + 15000;
          while (operation === this.searchEpoch && this.bt.state === "starting") {
            if (Date.now() >= deadline) throw Error(this.t("timeout"));
            await new Promise((resolve) => setTimeout(resolve, 300));
            if (operation !== this.searchEpoch) return;
            const revision = this.btEpoch;
            const latest = await UCAPI.request("/bluetooth");
            if (revision !== this.btEpoch) return;
            if (!validSource(latest, true)) throw Error("Invalid device response");
            this.acceptBluetooth(latest);
          }
        }
        if (operation !== this.searchEpoch) return;
        if (this.bt.state !== "on") throw Error(this.bt.reason || this.t("failed"));
        await this.physical("bluetooth", "scan", { seconds: 60 });
      } catch (error) {
        this.notice("operationFailed", { error: error.message }, true);
      } finally {
        this.searchBusy = false;
      }
    },
    pauseChanged(event) {
      this.paused = event.currentTarget.checked;
    },
    levelChanged(event) {
      if (["error", "warn", "info", "debug", "trace"].includes(event.currentTarget.value))
        this.level = event.currentTarget.value;
    },
    userChanged(event) {
      if (event) this.selectedUser = event.currentTarget.value;
      this.stableSince = Date.now();
      this.now = this.stableSince;
    },
    async refresh() {
      if (this.refreshing || this.stopping) return;
      this.refreshing = true;
      const btEpoch = this.btEpoch,
        usbEpoch = this.usbEpoch;
      try {
        const [system, users, session, bt, usb] = await Promise.all(
          ["/system", "/users", "/test-controller", "/bluetooth", "/usb"].map((p) =>
            UCAPI.request(p),
          ),
        );
        if (
          !Array.isArray(users?.users) ||
          !users.users.every((u) => Number.isInteger(u.user_id) && u.user_id > 0) ||
          !session ||
          typeof session !== "object" ||
          !validSource(bt, true) ||
          !validSource(usb, false)
        )
          throw Error("Invalid device response");
        this.now = Date.now();
        this.system = system;
        const next = users.users
          .filter((u) => u.signed_in !== false)
          .map((u) => ({ id: String(u.user_id), name: u.display_name || String(u.user_id) }));
        const usersKey = JSON.stringify(next);
        if (usersKey !== this.usersKey) {
          this.users = next;
          this.usersKey = usersKey;
          if (!next.some((u) => u.id === this.selectedUser)) {
            const preferred = String(users.preferred_user_id);
            this.selectedUser = next.find((u) => u.id === preferred)?.id || next[0]?.id || "";
          }
          this.stableSince = this.now;
        }
        if (btEpoch === this.btEpoch && usbEpoch === this.usbEpoch && !this.busy) {
          this.acceptBluetooth(bt);
          this.usb = usb;
          this.session = session;
          this.ready = true;
        }
        if ((!this.error || this.message === "waiting") && this.now >= this.rumbleNoticeUntil)
          this.notice("ready");
        if (!this.paused) {
          try {
            this.logs = this.formatLogs(
              await UCAPI.request("/logs?level=" + encodeURIComponent(this.level)),
            );
          } catch (e) {
            this.logs = this.t("operationFailed", { error: e.message });
          }
        }
      } catch (e) {
        this.ready = false;
        this.stableSince = Date.now();
        this.notice("operationFailed", { error: e.message }, true);
      } finally {
        this.refreshing = false;
      }
    },
    async physical(source, action, body) {
      const wireless = source === "bluetooth",
        busy = wireless ? "btBusy" : "usbBusy",
        epoch = wireless ? "btEpoch" : "usbEpoch";
      if (this.stopping || this[busy]) return;
      if (action === "start" && (wireless ? this.btStartDisabled : this.usbStartDisabled)) return;
      if (action === "pair" && this.pairDisabled) return;
      if (action === "scan" && this.scanning) return;
      if (action === "assign" && this.assignmentDisabled) return;
      this[busy] = true;
      const operation = ++this[epoch];
      try {
        await UCAPI.request(
          "/" + source + "/" + action,
          "POST",
          action === "start" ? { user_id: Number(this.selectedUser) } : body,
        );
        const latest = await UCAPI.request("/" + source);
        if (!validSource(latest, wireless)) throw Error("Invalid device response");
        if (operation === this[epoch]) {
          if (wireless) this.acceptBluetooth(latest);
          else this.usb = latest;
        }
        this.notice("ready");
        return true;
      } catch (e) {
        this.ready = false;
        this.notice("operationFailed", { error: e.message }, true);
      } finally {
        if (operation === this[epoch]) this[busy] = false;
      }
    },
    startBT() {
      return this.physical("bluetooth", "start");
    },
    stopBT() {
      ++this.searchEpoch;
      return this.physical("bluetooth", "stop");
    },
    startUSB() {
      return this.physical("usb", "start");
    },
    stopUSB() {
      return this.physical("usb", "stop");
    },
    pairDevice(event) {
      const id = Number(event.currentTarget.dataset.discoveryId);
      const found = this.discovered.find((d) => d.id === id);
      if (!found || found.disabled || !Number.isInteger(id)) return;
      return this.physical("bluetooth", "pair", { discovery_id: id });
    },
    assignBT(e) {
      if (!this.appAssignment) return;
      return this.assign(e, true);
    },
    assignUSB(e) {
      return this.assign(e, false);
    },
    assign(e, wireless) {
      const el = e.currentTarget;
      const body = { slot: Number(el.dataset.slot), user_id: Number(this.selectedUser) };
      body[wireless ? "address" : "node"] = el.dataset.identity;
      return this.physical(wireless ? "bluetooth" : "usb", "assign", body);
    },
    forget(e) {
      if (!window.confirm(this.t("confirmForget"))) return;
      return this.physical("bluetooth", "forget", { address: e.currentTarget.dataset.identity });
    },
    async stopApp() {
      if (this.stopping || !window.confirm(this.t("confirmStop"))) return;
      this.stopping = true;
      try {
        await UCAPI.request("/stop", "POST");
        clearInterval(this.timer);
        clearInterval(this.clockTimer);
        this.notice("stopped");
      } catch (e) {
        this.stopping = false;
        this.notice("operationFailed", { error: e.message }, true);
      }
    },
    formatLogs(data) {
      const entries = Array.isArray(data.entries) ? data.entries : [];
      const lines = entries.map((entry) => {
        const ms = Number(entry.timestamp_ms);
        const time = Number.isFinite(ms) ? (ms / 1000).toFixed(3) : "?";
        const clean = (value) => String(value ?? "-").replace(/[\r\n\t]/g, " ");
        return `[mono ${time}s] [${clean(entry.level).toUpperCase()}] core ${clean(entry.code)} generation=${clean(entry.generation)} session=${clean(entry.session_id)}`;
      });
      if (data.dropped) lines.unshift(`# core ring buffer: overwritten=${data.dropped}`);
      return lines.join("\n");
    },
    async diagnosticText() {
      // Independent sections remain available even if one endpoint fails.
      const routes = [
        "/system",
        "/bluetooth",
        "/bluetooth/diagnostics",
        "/usb",
        "/usb/diagnostics",
        "/test-controller",
      ];
      const sections = await Promise.all(
        routes.map(async (route) => {
          try {
            return `# ${route}\n${JSON.stringify(await UCAPI.request(route))}`;
          } catch (error) {
            return `# ${route} unavailable: ${error.message}`;
          }
        }),
      );
      try {
        sections.push(
          "# runtime.log (last 32 KiB)\n" +
            (await UCAPI.request("/diagnostics", "GET", undefined, "text")),
        );
      } catch (error) {
        sections.push("# runtime.log unavailable: " + error.message);
      }
      return [
        "# UniController diagnostics " + new Date().toISOString(),
        "# Core timestamps are monotonic uptime, not wall-clock time.",
        "# core events (current page level / paused snapshot)",
        this.logs,
        ...sections,
      ].join("\n\n");
    },
    async copy() {
      try {
        await navigator.clipboard.writeText(await this.diagnosticText());
        this.notice("copied");
      } catch (e) {
        this.notice("operationFailed", { error: e.message }, true);
      }
    },
    async exportLogs() {
      const text = await this.diagnosticText();
      const url = URL.createObjectURL(new Blob([text], { type: "text/plain;charset=utf-8" }));
      const a = document.createElement("a");
      a.href = url;
      a.download = "unicontroller-logs.txt";
      a.click();
      setTimeout(() => URL.revokeObjectURL(url), 1000);
    },
  }));
});
