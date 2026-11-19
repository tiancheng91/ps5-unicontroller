# SDL controller identification headers

Vendored unmodified from SDL commit `b2de7a19ad3c0d5063a5fb8e4cf5deeeb9ce4560`. No SDL runtime library is linked. Original Valve Corporation notices and zlib license are retained in the headers and LICENSE.txt.

- [controller_list.h](https://github.com/libsdl-org/SDL/blob/b2de7a19ad3c0d5063a5fb8e4cf5deeeb9ce4560/src/joystick/controller_list.h) — SHA-256 `e4fcfc57bdbf1d637f294d4bad0f777c823873c1c14ede9be7d54d5b8cf1357f`
- [controller_type.h](https://github.com/libsdl-org/SDL/blob/b2de7a19ad3c0d5063a5fb8e4cf5deeeb9ce4560/src/joystick/controller_type.h) — SHA-256 `95ca316db673da70924301d1d55e8930bc5225e639cd1f17cf2011597d4070ae`

To update, download both headers from the same pinned SDL commit and replace these files without reformatting. Update this revision and both hashes, review added/changed family enums and the Classic mapping in `src/drivers/protocols/profiles.c`, then run `make test-config test-overrides test-bluetooth`. Review license changes as well.

The original table supplies VID/PID family candidates and optional explicit names. It is not a PS5 compatibility list or a button mapping database. Local exceptions take precedence; transport and valid input remain required. NULL names stay NULL, and duplicate IDs use the first table entry. BLE permission and default rumble are maintained locally.
