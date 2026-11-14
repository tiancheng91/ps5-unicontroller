"use strict";
document.addEventListener("alpine:init", () => {
  const names = {
    0: "Create",
    1: "L3",
    2: "R3",
    3: "Options",
    4: "↑",
    5: "→",
    6: "↓",
    7: "←",
    8: "L2",
    9: "R2",
    10: "L1",
    11: "R1",
    12: "△ Triangle",
    13: "○ Circle",
    14: "× Cross",
    15: "□ Square",
    16: "PS",
    20: "Touchpad",
  };
  Alpine.data("settings", () => ({
    current: null,
    form: {
      recording_level: "info",
      page_level: "info",
      usb_auto_start: false,
      bluetooth_auto_start: true,
      bluetooth_auto_trigger: "login",
      rumble: true,
      swap_ab: false,
    },
    loading: false,
    saving: false,
    error: false,
    message: "waiting",
    errorText: "",
    levels: ["off", "error", "warn", "info", "debug", "trace"],
    launcher: { state: "pending" },
    launcherBusy: false,
    fields: Array.from({ length: 32 }, (_, id) => ({
      id,
      value: String(id),
      common: id in names,
      get name() {
        return names[id] || Alpine.store("ui").t("bit", { bit: id });
      },
    })),
    init() {
      this.load();
      this.loadLauncher();
    },
    t(key, values) {
      return Alpine.store("ui").t(key, values);
    },
    get commonFields() {
      return this.fields.filter((f) => f.common);
    },
    get reservedFields() {
      return this.fields.filter((f) => !f.common);
    },
    get errorClass() {
      return this.error ? "error" : "";
    },
    get statusText() {
      return this.error
        ? this.t("operationFailed", { error: this.errorText })
        : this.t(this.message);
    },
    get saveDisabled() {
      return !this.current || this.loading || this.saving || this.autoConflict;
    },
    get autoConflict() {
      return (
        this.form.bluetooth_auto_trigger === "resume" &&
        this.form.bluetooth_auto_start &&
        this.form.usb_auto_start
      );
    },
    get usbAutoNeedsUser() {
      return this.form.usb_auto_start && !(this.current?.usb_auto_user > 0);
    },
    get savedUser() {
      return this.current?.bluetooth_auto_user > 0
        ? this.t("account", { id: this.current.bluetooth_auto_user })
        : this.t("noSavedUser");
    },
    get launcherText() {
      return this.t(this.launcher.state) + (this.launcher.port ? " · " + this.launcher.port : "");
    },
    get installDisabled() {
      return this.launcherBusy || this.launcher.state === "unsupported";
    },
    get removeDisabled() {
      return this.installDisabled || this.launcher.state === "removed";
    },
    failure(e) {
      this.error = true;
      this.errorText = e.message;
    },
    async load() {
      if (this.loading || this.saving) return;
      this.loading = true;
      try {
        const s = await UCAPI.request("/settings");
        if (
          s.schema_version !== 1 ||
          !this.levels.includes(s.recording_level) ||
          !this.levels.includes(s.page_level) ||
          typeof s.rumble !== "boolean" ||
          typeof s.swap_ab !== "boolean" ||
          typeof s.bluetooth_auto_start !== "boolean" ||
          typeof s.usb_auto_start !== "boolean" ||
          !Number.isInteger(s.usb_auto_user) ||
          s.usb_auto_user < 0 ||
          !["login", "resume"].includes(s.bluetooth_auto_trigger) ||
          !Number.isInteger(s.bluetooth_auto_user) ||
          s.bluetooth_auto_user < 0 ||
          !Array.isArray(s.button_mapping) ||
          s.button_mapping.length !== 32 ||
          !s.button_mapping.every((v) => Number.isInteger(v) && v >= 0 && v < 32)
        )
          throw Error("Invalid settings response");
        this.current = s;
        this.form = { ...s };
        this.fields.forEach((f) => {
          f.value = String(s.button_mapping[f.id]);
        });
        this.error = false;
        this.message = "loaded";
      } catch (e) {
        this.current = null;
        this.failure(e);
      } finally {
        this.loading = false;
      }
    },
    preferenceChanged(event) {
      UCUI.set(event.currentTarget.dataset.preference, event.currentTarget.value);
    },
    formChanged(event) {
      const el = event.currentTarget,
        key = el.dataset.form;
      if (["recording_level", "page_level", "bluetooth_auto_trigger"].includes(key))
        this.form[key] = el.value;
      else if (["rumble", "swap_ab", "bluetooth_auto_start", "usb_auto_start"].includes(key))
        this.form[key] = el.checked;
    },
    get formDisabled() {
      return !this.current || this.loading || this.saving;
    },
    identity() {
      this.fields.forEach((f) => {
        f.value = String(f.id);
      });
    },
    async save() {
      if (this.saveDisabled) return;
      this.saving = true;
      try {
        const payload = {
          ...this.current,
          recording_level: this.form.recording_level,
          page_level: this.form.page_level,
          rumble: this.form.rumble,
          swap_ab: this.form.swap_ab,
          bluetooth_auto_start: this.form.bluetooth_auto_start,
          usb_auto_start: this.form.usb_auto_start,
          bluetooth_auto_trigger: this.form.bluetooth_auto_trigger,
          button_mapping: this.fields.map((f) => Number(f.value)),
        };
        this.current = await UCAPI.request("/settings", "POST", payload);
        this.error = false;
        this.message = "saved";
      } catch (e) {
        this.failure(e);
      } finally {
        this.saving = false;
      }
    },
    async loadLauncher() {
      return this.launcherAction("GET");
    },
    install() {
      return this.launcherAction("POST");
    },
    remove() {
      return this.launcherAction("DELETE");
    },
    async launcherAction(method) {
      if (this.launcherBusy) return;
      this.launcherBusy = true;
      try {
        this.launcher = await UCAPI.request("/launcher", method);
      } catch (e) {
        this.failure(e);
      } finally {
        this.launcherBusy = false;
      }
    },
  }));
});
