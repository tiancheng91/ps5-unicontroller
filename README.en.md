![UniController for PS5 — Make Any Controller Work on PS5.](assets/branding/banner.png)

[中文](README.md) · **English**

**UniController for PS5** connects third-party controllers to PS5 over Bluetooth or USB. Manage them from a phone, computer or the console's web interface, without installing plugins into games.

- Protocol support for Switch Pro-compatible controllers, selected Xbox models and generic Bluetooth HID devices.
- Up to four physical input sources in normal mode, assigned to separate PS5 local users.
- Game rumble feedback, button mapping, ABXY swapping and a touchscreen test controller.

> **Development build:** protocol implementations and automated builds are available, but controller, firmware, game and rest-mode compatibility still require hardware validation. “Make Any Controller Work on PS5” is the goal, not a guarantee. Keep an original controller available and leave automatic startup off for your first test.

[Download test builds](https://github.com/tiancheng91/ps5-unicontroller/actions/workflows/build.yml) · [Report an issue](https://github.com/tiancheng91/ps5-unicontroller/issues) · [Hardware test checklist](docs/test-checklist.md) (Chinese)

## Quick start

### 1. Load the payload

You need a **PS5 capable of loading homebrew ELF payloads**, Payload Manager or another compatible loader, and a phone or computer on the same LAN. A complete firmware compatibility matrix is not available. Unmodified retail systems are not supported.

1. Choose a successful build for the desired commit in [Actions](https://github.com/tiancheng91/ps5-unicontroller/actions/workflows/build.yml), then download and extract **UniController-PS5**. GitHub sign-in may be required.
2. Run `unicontroller.elf` using your loader. The artifact includes `build-info.txt` and a SHA256 checksum.
3. Open `http://<PS5-IP>:7456/`. If the port is busy, try **7457**; the startup log records the actual port.

Web assets are included in the payload. Use the interface on a trusted LAN and do not expose its ports to the internet.

### 2. Connect a controller

Sign into PS5 with an original controller. Normal mode uses the recommended signed-in user; assign another signed-in local user from advanced device details when needed for multiplayer.

| Input      | Steps                                                                                                                                                                                                            |
| ---------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Bluetooth  | Choose Add Bluetooth device, put the controller in pairing mode, and start a **60-second search**. Click Pair on a discovered device to connect. Use Classic for Switch mode. Stored devices reconnect normally. |
| USB        | Enable USB in Settings and connect a supported controller mode using a data cable.                                                                                                                               |
| Phone test | Open `/controller`, select a user and enable it. Disable it before using physical input sources.                                                                                                                 |

Check input and binding status on the device card, then test in a game. In normal mode, the first valid input device uses the user selected at startup; assign additional devices on their cards. Bluetooth and USB can run together. The phone test controller is exclusive.

**Use separate users for two independent players.** If menus work but games do not, check account assignment first. PS5's same-user assist-controller feature is separate and is not enabled automatically.

### 3. Open the configuration page on PS5

After startup, the application attempts to register a **UniController** icon when the system is signed in, all input sources are stopped and conditions remain stable. Settings also offers manual installation, updates and removal.

**The icon is a web shortcut, not a payload loader.** Run the ELF before using it. A missing icon does not prevent LAN access. [Launcher details](docs/launcher.md) (Chinese)

## Protocol support

These are **implemented paths**, not a certified hardware list. A product can use different protocols across modes and firmware versions.

| Device / protocol     | Bluetooth                                   | USB                                                 | Current limits                                                           |
| --------------------- | ------------------------------------------- | --------------------------------------------------- | ------------------------------------------------------------------------ |
| Switch Pro-compatible | Classic                                     | Not supported                                       | Switch BLE identities are rejected to avoid unusable device entries      |
| Joy-Con               | Separate left/right profiles                | —                                                   | No paired Joy-Con combination                                            |
| Xbox                  | Registered Xbox HID models                  | Xbox 360 / XInput-compatible interfaces and reports | No Xbox One/Series USB GIP or proprietary Xbox wireless receiver support |
| Android / generic HID | Supported HID descriptors and known layouts | HID diagnostics only                                | Arbitrary Android modes and USB HID are not guaranteed                   |

BLE uses an allowlist; the only built-in enabled identity is currently Xbox `045e:0b13`. For third-party devices, start with a known **Switch Classic mode**. A VID/PID, name or “XInput-compatible” label alone does not establish compatibility.

Rumble and battery reporting depend on the device protocol and firmware. Game rumble feedback is implemented for selected protocols; adaptive triggers and full DualSense haptics are not implied. An unknown battery reading does not necessarily prevent input.

[Bluetooth scope](docs/bluetooth.md) · [USB scope](docs/usb.md) · [GameSir Nova notes](docs/gamesir-nova.md) (Chinese)

## Everyday use

### Buttons and settings

Switch protocol defaults follow physical positions: B (bottom)→×, A (right)→○, Y (left)→□, X (top)→△; Home→PS, Capture→touchpad **click**. These are protocol button names. Third-party button labels and controller-side swapping may differ.

**Swap A/B and X/Y** exchanges both pairs, is off by default, and runs before custom button mapping. It affects all application input sources. If the controller also swaps these pairs, the two swaps cancel out; choose the combination that produces your intended output and check it in the web interface.

Connected Bluetooth and USB controllers share one list; “Connect a controller” opens pairing and adapter controls. The management page supports Chinese/English, initially chosen from the browser language, four color themes and a large-text TV mode. Appearance is stored in this browser and does not affect connections. Optional arrow-key/standard Gamepad navigation is experimental and still needs PS5 WebView validation.

Settings also includes rumble and log levels. Stop input sources before making changes. File editing is generally unnecessary; advanced adaptation is described in [custom controller configuration](docs/controller-config.md) (Chinese).

### Rest mode and reconnection

Rest mode stops Bluetooth/USB and releases application-owned virtual controllers, **preserving Bluetooth bonds**. Reconnect after resume; pairing again is normally unnecessary. Startup is manual by default, and USB does not restart automatically.

Optional Bluetooth automatic startup offers two triggers, both delayed by **five seconds**:

| Trigger                                  | Behavior                                                                                                                                                                   |
| ---------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| After login and home focus (recommended) | Wait for a fresh home-focus event and signed-in foreground and saved target users. Reuse the account from the last successful manual Bluetooth start.                      |
| After resume (experimental)              | Start without waiting for login; after a third-party controller connects, create an application-unbound virtual controller and inject input to test system user selection. |

The experiment permits one input source, ignores the saved account and lets you press PS/Home to choose a user on PS5. **User selection and game input still require hardware validation.** “System binding unverified” is not a success indicator. This mode does not start immediately when settings are saved or the payload loads, and it cannot wake the console with a third-party controller. Use the console power button for this test and keep an original controller available.

Each automatic attempt runs once. Start manually if no trigger arrives, observation expires or startup fails. [Lifecycle details](docs/bluetooth.md#可选自动启动) · [Test procedure](docs/test-checklist.md) (Chinese)

### Updates and data

Stop the old process from the web interface, then deploy and run the new ELF. Keep `/data/unicontroller/` to retain settings and bonds. “Forget device” removes that device's pairing record. Do not publish `bonds.db`, which contains keys.

[Settings and storage](docs/settings-storage.md) (Chinese)

## Troubleshooting and reports

| Symptom                                | Check first                                                                                                                                                                |
| -------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Web page unavailable                   | Payload running, correct PS5 IP, ports 7456/7457.                                                                                                                          |
| Connected but no buttons               | Controller mode, input counts and errors; use Classic for Switch.                                                                                                          |
| Menus work but games do not            | Account assignment in normal mode; record user-selection and game-input results separately in experimental mode.                                                           |
| Held inputs release early or stick     | Report model, firmware and actions; report frequency needs per-device adaptation.                                                                                          |
| Original controller also stops working | Stop the test, stop Bluetooth or the payload through the web interface, and save diagnostics. If control remains unavailable, recover using normal console power controls. |

In [Issues](https://github.com/tiancheng91/ps5-unicontroller/issues), include the **build commit, PS5 firmware, controller model/firmware/mode, transport, account setup, reproduction steps and diagnostic logs**. Export logs from the control panel; runtime logs are stored at `/data/unicontroller/runtime.log`. Check for personal information before sharing.

Use the [hardware checklist](docs/test-checklist.md) (Chinese) to mark each item as passed, failed or not tested. This helps distinguish pairing, parsing, account and game-output issues.

## Development and contributions

Built with C11 and HTML/CSS/JavaScript. Reproducible issues, hardware results and fixes with tests are welcome. Device adaptations should identify the model, firmware, connection mode and tested behavior. Host tests and successful builds do not establish hardware compatibility.

```sh
make host
./build/unicontroller-host 7456 /tmp/unicontroller-settings.json
make test
make preview # Isolated demo preview, no hardware operations
```

The management shell uses a pinned offline Alpine CSP build; the touch input loop stays native JavaScript. Run host regressions with `make test`. For real-browser tests, run `npm ci` then `make test-browser` with Chrome/Chromium (`CHROME_PATH` can override its path). Runtime needs no Node, NPM or CDN.

GitHub Actions uses a pinned PS5 SDK; local Docker is not required. Optional local cross-build: `PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk make ps5`. The host mock must not be loaded as a PS5 payload.

[Architecture](docs/architecture.md) · [Code style](docs/code-style.md) · [Developer validation](docs/validation.md) · [Documentation index](docs/README.md) (Chinese)

## License

Licensed under [GNU GPLv3](LICENSE). Third-party sources and licensing requirements are documented in [NOTICE](NOTICE).
