# UniController for PS5

[中文](README.md) · **English**

![UniController for PS5 — Make Any Controller Work on PS5.](assets/branding/banner.png)

Universal Controller Support for PS5 — **Make Any Controller Work on PS5.**

Connect third-party controllers to PS5 over Bluetooth or USB, and manage connections, buttons and settings through a web interface on your phone or console. A touchscreen controller is also available for testing.

> This project is under development and hardware validation. Implemented protocol support does not guarantee compatibility with every controller, firmware or game. Keep an original controller available for recovery.

## Install and open

You need a PS5 capable of running homebrew ELF payloads, Payload Manager or another compatible loader, and a phone or computer on the same local network. This is not an official application for an unmodified console. A complete firmware compatibility matrix is not yet available.

1. Open [GitHub Actions](https://github.com/tiancheng91/ps5-unicontroller/actions/workflows/build.yml) and select a successful build for the commit you want to test.
2. Download and extract the **UniController-PS5** artifact. It contains `unicontroller.elf`, a SHA256 checksum and `build-info.txt`. Downloading Actions artifacts may require signing into GitHub.
3. Deploy and run `unicontroller.elf` using your loader's instructions. Leave automatic startup disabled for your first test.
4. Visit `http://<PS5-LAN-IP>:7456/` on your phone or computer. If that port is unavailable, the server tries **7457**; check the startup log for the actual port.
5. With all input sources stopped and the console logged in and stable, the application attempts to install a **UniController** icon. It opens the local configuration page. If the icon is missing, use the LAN address or install it manually from Settings.

**The icon opens the configuration page; it does not load the payload.** It can remain after the process stops, but the page will be unavailable. Web assets are embedded in the ELF without CDN dependencies. Use the interface only on a trusted LAN; do not expose its ports to the internet.

To update, stop the old process from the control panel, then deploy and run the new ELF. Normally, keep `/data/unicontroller/` to preserve settings and Bluetooth bonds. Remove the icon through Settings if needed. See [launcher details](docs/launcher.md) (Chinese).

## Controller compatibility

| Connection / mode               | Implemented support                                       | Limitations                                                                                 |
| ------------------------------- | --------------------------------------------------------- | ------------------------------------------------------------------------------------------- |
| Switch Pro Bluetooth Classic    | Original controllers and compatible third-party protocols | Preferred test path for Switch mode; Switch identities over BLE are rejected                |
| Joy-Con Bluetooth               | Separate left/right profiles                              | Combining two Joy-Cons into one controller is not implemented                               |
| Xbox Bluetooth                  | Registered Xbox HID models                                | Not a guarantee for every device advertised as XInput-compatible                            |
| BLE                             | Allowlist; Xbox `045e:0b13` enabled by default            | Other identities are not automatically accepted; names alone do not establish compatibility |
| Android / generic Bluetooth HID | Supported HID descriptors and known layouts               | Buttons, axes, held inputs and battery reporting need per-model validation                  |
| USB Switch Pro                  | `057e:2009` with compatible handshake and reports         | Requires a data cable; not general USB HID support                                          |
| USB Xbox 360 / XInput           | Matching interfaces and input reports                     | Xbox One/Series USB GIP and proprietary Xbox wireless receivers are not supported           |
| Phone touchscreen               | Sticks, face buttons, PS, touchpad click and more         | Test tool; exclusive with physical input sources                                            |

Controllers such as GameSir Nova Lite and Nova 2 Lite can report different identities depending on mode and firmware. Check **connection type and protocol mode**, not just the product name or VID/PID. Some receiver identities are registered, but their actual reports and disconnection behavior still require testing. [Model notes](docs/gamesir-nova.md) · [Bluetooth](docs/bluetooth.md) · [USB](docs/usb.md) (Chinese)

Game rumble feedback is implemented for protocols with output support, pending per-model hardware validation. This does not imply adaptive-trigger or full DualSense haptic support. Battery display depends on device reports: dedicated Switch/Xbox Bluetooth parsers handle certain battery reports, while USB XInput and generic HID may show “unknown.”

## First connection

### Bluetooth controllers

1. Log into PS5 using an original controller. For two independent players, prepare two signed-in local users.
2. Open the control panel, disable the touchscreen test controller, select the third-party controller's user and start Bluetooth.
3. Open the **60-second pairing window**, then put the controller into the intended mode and pairing state.
4. Check input frames, buttons and battery on its device card. A successful connection alone does not prove usable input.
5. After valid input arrives, the application creates a virtual controller and binds it to the selected user. Switch Home maps to PS; Capture maps to touchpad **click**, not touch gestures.
6. Test in a game. Previously paired controllers should normally reconnect without pairing again.

Two identical controllers can share a VID/PID and still connect separately using different Bluetooth addresses. One physical device may also expose different Classic and BLE addresses. Similar addresses do not prove two usable controllers. Switch BLE is filtered; historical bonds may remain visible and can be removed with “Forget device.”

### USB controllers

Select a signed-in user, start USB, and connect a supported controller mode with a data cable. Check input and binding status. The application does not forcibly detach a system driver when an interface is busy or unsupported. Bluetooth and USB can run together, with up to **four virtual outputs** in normal mode. Assign additional devices to separate users on their cards.

### Phone touchscreen test

Open `/controller`, select a signed-in user and enable it. Landscape mode, dual sticks, PS, touchpad click and emergency release are available. Disable it before starting physical inputs. This page tests the input path; it is not designed as a low-latency gaming controller.

## Accounts and multiplayer

Normal mode calls the system binding API for the selected user. Independent players should use different users. If menus respond but games do not, first check whether the controller is assigned to the game's intended user.

PS5's main/assist controller feature is separate. UniController does not enable it automatically or treat two controllers on the same account as two independent players.

## Rest mode and automatic startup

Automatic startup is off by default. Entering rest stops Bluetooth/USB and removes the application's virtual controllers, **without deleting Bluetooth bonds**. Resuming does not require pairing again; virtual controllers are recreated and user selection may be needed again.

| Setting                                         | Behavior after resume                                                                                                                                                                            |
| ----------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| Automatic startup off                           | Wait for a manual start from the web interface                                                                                                                                                   |
| After login and home screen focus (recommended) | Wait for a fresh home-focus event, signed-in foreground and saved target users, then five stable seconds before starting Bluetooth                                                               |
| After resume (experimental)                     | Wait for five stable seconds in the working power state, start Bluetooth, then let the first valid physical input create an application-unbound virtual controller and inject input before login |

**The resume experiment tests: press PS/Home on the third-party controller → select a PS5 user → enter a game.** It ignores the saved account and leaves selection to the system, without calling the application's binding API. Only one experimental source is allowed, exclusive with other application-owned virtual outputs. “System binding unverified” in the UI is intentional; this path still needs real-console validation.

Resume mode does not start immediately when the payload loads or settings are saved. It does not wake the console using a third-party controller. Use the console power button to resume when testing login without an original controller. The original controller can still wake the console in normal use. USB does not restart automatically.

Each automatic attempt runs once. If conditions are not met, observation expires or startup fails, start manually. Login mode requires a new home-focus event; historical logs and an existing login list do not substitute for it. See [Bluetooth details](docs/bluetooth.md) (Chinese).

## Settings, data and troubleshooting

Settings include log levels, rumble, **Swap A/B and X/Y** and button mapping. Stop input sources before changing settings. Most users do not need to edit files.

**Swap A/B and X/Y** exchanges both button pairs and is off by default. If the controller already applies the same swap (for example, Nova Lite’s M+A shortcut), normally enable it in only one place: applying the swap twice cancels it out.

| Data                                               | Location / purpose                                                                                                                              |
| -------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------- |
| Settings and last manually selected Bluetooth user | `/data/unicontroller/settings.json`; the resume experiment ignores this account                                                                 |
| Bluetooth bonds                                    | `/data/unicontroller/bonds.db`; contains keys, do not publish it                                                                                |
| Runtime log                                        | `/data/unicontroller/runtime.log`; diagnostics can be exported from the control panel                                                           |
| Optional device overrides                          | `/data/unicontroller/controllers.json`; for advanced users, see [example](examples/controllers.json) and [reference](docs/controller-config.md) |

- **Web page unavailable:** verify the payload is running, the IP is correct, and check ports 7456/7457.
- **Connected but no input:** check the controller mode; use Classic for Switch. Capture input counts, device address and errors.
- **Menus work but games do not:** check the account in normal mode. In the experiment, record whether system user selection works; login success alone does not prove game input works.
- **Held buttons release early or stick:** report the model and firmware. Report frequency varies; timeout-based input release is not a substitute for device adaptation.
- **The original controller also stops working:** stop the experiment, try stopping Bluetooth or the payload through the web interface, and save logs. If stopping the process does not restore control, recover using normal console power controls rather than repeatedly pairing devices.

Reports should include the build commit, PS5 firmware, controller model/firmware/mode, transport, account setup, reproduction steps and diagnostics. Review logs for personal information before posting. Use the [hardware test checklist](docs/test-checklist.md) (Chinese) to record results.

## For developers

C11 + HTML/CSS/JavaScript. Web assets are embedded in the ELF. A host mock cannot replace PS5 hardware validation.

```text
src/core/     Sessions, mappings, logs and input timeouts
src/ps5/      Virtual controllers, accounts, power and launcher
src/drivers/  Bluetooth/USB transports, protocols and device configuration
src/web/      HTTP API and settings storage
web/          Control panel, touchscreen controller and settings
tests/       Host regression tests
```

```sh
make host
./build/unicontroller-host 7456 /tmp/unicontroller-settings.json
make test
make preview                     # Static preview without APIs
make preview PREVIEW_BIND=0.0.0.0 # Access from a phone using the computer's LAN IP
```

GitHub Actions builds the PS5 ELF with a pinned SDK; local Docker is not required. Optional local cross-build: `PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk make ps5`. Load only the SDK/Actions-built `unicontroller.elf` on PS5; do not rename the host mock as an ELF payload.

Built-in devices live in [controllers.json](src/drivers/protocols/controllers.json); end users need not modify it. Shared web styles live in `web/shared/`. Use `make format` / `make format-check`; do not manually edit generated `build/` files.

[Architecture](docs/architecture.md) · [Code style](docs/code-style.md) · [Visual design](design.md) · [Developer validation](docs/validation.md) · [Documentation index](docs/README.md) (Chinese)

See [LICENSE](LICENSE) and [NOTICE](NOTICE) for licensing and third-party attribution.
