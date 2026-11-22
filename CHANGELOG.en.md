# Changelog

[中文](CHANGELOG.md) · **English**

## [Unreleased]

## [0.2.0] - 2026-11-22

### Controller compatibility

- First-generation Switch Pro controllers (`057e:2009`) now support buttons, sticks, game rumble, and the web rumble test over USB. Wired operation has been confirmed on hardware.
- The PowerA Enhanced Wireless Controller (`20d6:0002`) now tries the first-generation Switch Pro handshake over USB. Wired input on this model still needs hardware verification.
- Switch 2 Pro controllers (`057e:2069`) have experimental USB input. Buttons and sticks have been tested on a PS5 Pro running firmware 13.20. Rumble is unavailable. If audio moves to the controller, select your TV or headphones again in PS5 audio output settings. Switch 2 BLE devices appear in discovery with pairing disabled.
- Bluetooth Classic identification uses SDL device data to select candidate protocols. You can override the protocol by exact address or name. First-generation Nintendo controller calibration and input identification have improved. Known unsupported BLE devices are marked in discovery.
- Fixed USB initialization failures and repeated reconnects exhausting controller slots. Fixed controllers being detected after reconnection but sending no usable input. Generic HID devices have 30 seconds to send their first valid input. Replug the device to retry after a timeout.

### Users, startup, and web controls

- Bluetooth and USB can be started manually before sign-in. Use the device details page to reopen user selection on PS5. Healthy physical connections survive user switches and sign-out.
- Bluetooth and USB auto-start can be enabled independently. The base delay is configurable from 5 to 60 seconds. By default, USB starts after 10 seconds if enabled, and Bluetooth starts after 13 seconds. Only Bluetooth auto-start is enabled by default. Auto-start also works when the payload loads after a foreground user has signed in.
- The mobile simulator creates and removes a virtual controller. Choose its user on PS5 and create it again after rest mode. Landscape focus mode fits the buttons and both sticks on one screen.
- Device details show rumble availability for the current connection mode. Pairing history shows known model names, and the web interface displays the build version and commit. Diagnostics now include Bluetooth authentication stages, the first USB input report, and startup failures.

## [0.1.2] - 2026-11-14

- Unified Bluetooth, USB, network, and lifecycle messages in one runtime log. The web interface supports severity filters, copying visible entries, and exporting diagnostics.
- Removed the wait before manually starting services after opening the page or switching users. Automatic recovery retained the 15-second Bluetooth and 25-second USB delays in this release.
- **API change:** `/api/v1/logs` returns plain text. Scripts that consume structured core events should use `/api/v1/core/events`.
- Host and browser tests and the PS5 cross-build passed. Hardware validation was pending at release.

## [0.1.1] - 2026-11-14

- Added a half-second rumble test for supported Bluetooth and USB XInput controllers. The page explains unavailable output and limits repeated requests.
- Changed auto-start delays to 15 seconds for Bluetooth and 25 seconds for USB. New configurations enable only Bluetooth auto-start. Existing preferences are preserved. USB auto-start was discouraged pending hardware validation.
- Removed stale USB slots from the connected device list and fixed desktop observation stopping when its diagnostic capture reached 256 KiB. Services paused during rest now appear as off.
- The HTTP listener stays open during rest. On resume, the service checks the listener, limits recovery attempts, and exits after repeated failures. The web interface uses port 7456 and exits if it cannot listen there. The former fallback to 7457 was removed.
- Settings can be saved while a controller is connected. Layout changes apply to the next input report.

## [0.1.0] - 2026-11-14

- Added manual Bluetooth discovery, pairing, reconnection, and management for Classic Switch controllers, selected Xbox models, and generic HID gamepads.
- Added hotplug and diagnostics for USB XInput and descriptor-identified Generic HID gamepads. Wired Switch mode was unavailable in this release. If a firmware rejected the USB device-info query, enumeration continued using descriptors and recorded the error.
- Supported up to four physical inputs, Bluetooth and USB together, independent virtual outputs, and local user assignment. Assign separate PS5 users for independent players.
- Added per-controller rumble and ABXY swap preferences. Known devices default to allowing rumble. Unknown devices default to off and are marked experimental. Supported devices receive game rumble and a Bluetooth connection pulse. Generic HID has no universal rumble output. USB rumble and LED failures leave input available.
- Added a mobile test controller for buttons, PS input, user selection, and games. It runs separately from the physical input services.
- Added optional delayed startup after desktop login or rest recovery, disabled by default in this release. Rest mode stops connections and removes owned virtual controllers while retaining Bluetooth bonds. Recovery behavior depends on the device and firmware.
- Added Chinese and English web controls, themes, device details, and diagnostic export. The PS5 browser uses TV-sized text and directional navigation. The home-screen shortcut opens the local web interface after the payload is running.
- Added tagged releases with the ELF, SHA-256 checksum, and build metadata. Fixed Bluetooth scan and pairing coordination and simulator state synchronization.
- Use Classic for Switch-compatible modes. BLE endpoints reporting a Switch identity are rejected. Similar addresses are kept separate. Compatibility depends on model, connection mode, firmware, and console environment.
