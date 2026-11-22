![UniController for PS5](assets/branding/banner.png)

[中文](README.md) · **English**

# UniController for PS5

Play PS5 games with a third-party controller. UniController runs on the console and passes Bluetooth or USB controller input to a virtual DualSense. Use a phone to pair devices, inspect input and test rumble. The phone can also act as a test controller.

**Requirements:** A PS5 that can run homebrew ELF payloads through Payload Manager or a compatible loader. For initial setup, keep an original DualSense handy and connect your phone or computer to the same trusted LAN as the console.

[Download the latest release](https://github.com/tiancheng91/ps5-unicontroller/releases/latest) · [Changelog](CHANGELOG.en.md) · [Report an issue](https://github.com/tiancheng91/ps5-unicontroller/issues)

## Connect your first controller

1. Download `unicontroller.elf` from the release **Assets**, upload it to the PS5, and run it with your loader.
2. Open **`http://<PS5-IP>:7456/`** on your phone or computer. Replace `<PS5-IP>` with the console’s LAN address.
3. Choose your connection:
   - **Bluetooth:** Put the controller in pairing mode. Under **Controllers → Add Bluetooth device**, start a search, find the device and select **Pair**. In Switch mode, choose the named **Bluetooth Classic** entry if available. On subsequent connections in the same mode, turning the controller on is usually enough.
   - **USB:** Start USB under **Settings → Connection services**, then plug in the controller with a data cable or insert its compatible receiver. If prompted for input, press a button or move a stick.
4. When PS5 asks who is using the controller, select the player using that controller. In device details, check that the input frame count increases when you press buttons, then try a game. **For independent local multiplayer, assign each controller to a different PS5 user.** To change players, choose **Choose user again** in device details.

Settings can install a PS5 home-screen shortcut to the same web interface. Load the ELF before opening the shortcut. To upgrade, stop UniController from the web interface, replace the ELF and run the new version. Keep `/data/unicontroller/` to preserve your settings and pairing records. [Shortcut installation](docs/launcher.md)

## Which controller mode should I use?

A multi-mode controller can report a different identity and offer different features in each mode. **First-generation Switch Pro mode** has the most complete input and rumble support. **Xbox 360 / XInput mode** is also a good starting point for wired controllers.

| Controller mode             | Bluetooth                                                                 | USB                                                                                              |
| --------------------------- | ------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------ |
| First-generation Switch Pro | Classic input and rumble                                                  | `057e:2009` input and rumble, confirmed on hardware                                              |
| Switch 2 Pro                | Visible over BLE, pairing disabled                                        | `057e:2069` experimental button and stick input, no rumble                                       |
| First-generation Joy-Con    | Left and right controllers operate separately                             | —                                                                                                |
| Xbox                        | Selected Xbox HID models. The built-in BLE allowlist includes `045e:0b13` | Xbox 360 / XInput. Xbox One/Series USB GIP and dedicated Xbox wireless receivers are unsupported |
| Android / Generic HID       | Input decoded from the HID descriptor                                     | Input for compatible HID gamepad interfaces. No generic rumble output                            |

**Switch 2 Pro over USB** has been tested for basic input on PS5 Pro firmware 13.20. If plugging it in silences your TV, open the PS5 audio output settings, select your TV or headphones again, and disable automatic output device switching.

For example, the GameSir Nova Lite supports rumble in Classic Switch Pro mode. Its `3537:1041` Generic HID mode provides button input but currently has no rumble output. The connection mode matters even on the same physical controller. [GameSir modes](docs/gamesir-nova.md) · [Bluetooth support](docs/bluetooth.md) · [USB support](docs/usb.md)

**Model missing or buttons mapped incorrectly?** UniController looks up the device’s VID/PID in SDL’s controller table to help identify the model. For Bluetooth Classic, the listed controller family also helps select a candidate protocol. USB protocol detection uses the device’s interfaces and report descriptors. [View the SDL table used by this version](https://github.com/libsdl-org/SDL/blob/b2de7a19ad3c0d5063a5fb8e4cf5deeeb9ce4560/src/joystick/controller_list.h)

If your controller uses a supported protocol, adapt the example to its connection type and device identity. Upload it to `/data/unicontroller/controllers.json` on the PS5 via FTP, then restart UniController to test. An override can select an existing protocol or adjust generic HID button mappings. **Switch 2 Pro over BLE is unsupported, and an override cannot enable pairing.** Use USB to test basic input instead. [Configuration guide](docs/controller-config.md) · [Example file](examples/controllers.json)

## Everyday use

- **Button layout:** Switch face buttons map by position: bottom B → ×, right A → ○, left Y → □, top X → △. Home maps to PS and Capture to a touchpad click. To swap both A/B and X/Y, enable the ABXY swap in that controller’s details.
- **Rumble:** Allow rumble in device details and select **Test rumble**. The master rumble switch in Settings must also be on. The page explains when the connection mode has no available rumble output. Game feedback supports traditional motor rumble.
- **Rest mode and startup:** Rest mode releases virtual controllers and stops the physical input services. Bluetooth pairing records are retained. On a fresh install, only Bluetooth auto-start is enabled. Enable USB auto-start in Settings to start USB on the selected trigger as well. Upgrades retain your saved preferences. The base delay is 10 seconds, adjustable from 5 to 60. USB uses the base delay and Bluetooth starts three seconds later. These settings also apply if the payload loads after the foreground user has signed in. For pre-login testing, start the services manually or select the experimental resume trigger.
- **Phone simulator:** Stop the Bluetooth and USB services, open **Simulator**, create a controller and select its user on PS5. Landscape focus mode puts both sticks and the buttons on one screen. Create the controller again after rest mode.

## Troubleshooting

| Symptom                                                         | What to check                                                                                                                                                                                                                  |
| --------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| The web interface won’t open                                    | Check that the ELF is running and the console address and port 7456 are correct. The payload exits if the port is occupied.                                                                                                    |
| The controller pairs but buttons do nothing                     | Press the D-pad while watching the input frame count in device details. If it stays still, check the mode. Use Classic for Switch mode. If a generic USB HID device times out waiting for input, replug it and press a button. |
| Input frames increase, but the intended player does not respond | The controller may be assigned to another local PS5 user. Open its **Device details**, select **Choose user again**, then choose the intended user on PS5 and retry. Pairing is retained.                                      |
| Menus work but the game ignores the controller                  | Check whether both controllers selected the same local user. The original controller may still control the game. See the player assignment steps below.                                                                        |
| The original DualSense also stops responding                    | Use your phone to stop the connection services and export logs. If needed, stop UniController and use the console’s normal power controls to recover.                                                                          |

### How does PS5 assign controllers to players?

The input frame count shows that UniController received your button presses. The PS5 user selection screen assigns the controller to a user, and the game may then ask that player to join. For example, if the original controller is playing as user A and the third-party controller also selects A, navigating system menus with the second controller does not add an independent player to the game.

- **Switch controllers in single-player:** Turn off the original controller, press PS/Home on the third-party controller, and select user A to continue the same game.
- **Play local multiplayer independently:** Leave the original controller with A. In the third-party controller’s device details, select **Choose user again**, select a different local user B on PS5, then follow the game’s prompt to join.
- **Share control of one player:** For compatible controllers, enable **Use Second Controller for Assistance** in PS5 accessibility settings under **Controllers**, then assign both controllers to A. Both provide input for the same player. [PlayStation assist controller guide](https://www.playstation.com/en-us/support/hardware/second-controller-assistance/)

When reporting an issue, include the version and commit shown in the web interface, PS5 model and firmware, controller model and mode, steps to reproduce, and the export from **Logs → Export logs**. Keep `bonds.db` private because it contains pairing keys. [Open an issue](https://github.com/tiancheng91/ps5-unicontroller/issues) · [Hardware test checklist](docs/test-checklist.md)

## Development and contributions

The backend is C11. Web assets are bundled in the ELF and work offline. From the repository root:

```sh
make preview        # Browse the interface with demo data
make test           # Run host regressions
npm ci
make test-browser   # Requires Chrome or Chromium
```

GitHub Actions cross-compiles the PS5 payload with a pinned SDK. When contributing controller support or hardware results, include the connection mode and which buttons, sticks and rumble functions you tested. [Developer documentation](docs/README.md) · [Custom controller profiles](docs/controller-config.md) · [Actions builds](https://github.com/tiancheng91/ps5-unicontroller/actions/workflows/build.yml)

## License and acknowledgments

UniController is licensed under GNU GPLv3. The inherited Bluetooth bridge code retains its GPL-3.0-or-later license. Bundled components retain their own licenses and copyright notices. [LICENSE](LICENSE) · [NOTICE](NOTICE)

Thank you to StonedModder and seregonwar for Ghostcontrol and its early research on creating a virtual DualSense on PS5 and assigning it to a user. [Ghostcontrol](https://github.com/StonedModder/Ghostcontrol-PS5-USB-Controller-Patcher)

The Bluetooth bridge, pairing and controller protocol code derive from AngelLopezTorres’s ps5-wireless-pads, through X-F1REBALL-X’s PadBridge PS5 and sinfiltros’s original AnyPad-PS5. We thank their authors, and SDL and Valve for the data used to identify controller models and types. [ps5-wireless-pads](https://github.com/AngelLopezTorres/ps5-wireless-pads) · [PadBridge PS5](https://github.com/X-F1REBALL-X/PadBridge-PS5) · [SDL data source](vendor/sdl/README.md)
