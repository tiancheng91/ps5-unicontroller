# Native HTTP runtime

Pure C11, single event-loop owner, no external libraries. `uc_runtime_run(bind,port,settings_path)` follows backend initialization. Core polling and 500ms deadman checks run every iteration even with slow clients. Eight sockets, 4KiB headers, 8KiB request bodies, three-second deadlines, one request per connection, bounded JSON depth/tokens, and five-millisecond loop pacing bound resource use. No Bluetooth or WebSocket implementation is included. The mobile controller uses HTTP input and heartbeat posts.

GET `/api/v1/session` returns a fresh process-local 256-bit token. Mutations require `X-UC-Token` and exact `Origin` matching the accepted socket's local literal IPv4 address and port. Host must match that authority too; use `http://IP:PORT`, not a DNS hostname. No CORS is emitted. Session bootstrap relies on browser same-origin isolation, and plain HTTP has no confidentiality; run on a trusted LAN. Tokens are neither logged nor persisted.

GET `/api/v1/system`, `/users`, `/settings`, `/logs?level=info` and `/test-controller` are under `/api/v1`. POST `/test-controller` accepts `{user_id}`; 202 `{pending:true}` requires explicit retry while the platform identifies the new output. DELETE cancels pending creation and releases/deletes the owned output. POST `/test-controller/input` accepts session ID, monotonic sequence, button mask, four signed 16-bit axes and two unsigned 16-bit triggers. Neutral input releases buttons; lost clients are released by the independent core timeout.

Settings POST requires the complete flat schema with schema_version=1, recording_level, page_level, rumble, swap_ab, and 32 mapping entries in 0..31. Unknown/duplicate keys and invalid numbers fail. Disable the controller before changing settings. A mode-0600 temporary document is fully written and fsynced before rename; only then is memory replaced and core recording/mapping applied. A corrupt stored file blocks startup. Directory fsync, concurrent process writers and symlink-hardening of a hostile settings directory are outside this single-process trusted-directory MVP. Rumble is stored intent; feedback forwarding is not implemented. Pairing secrets are outside this model.

Recording level limits retained lifecycle logs; page level/query filters existing entries only. Settings persistence errors emit fixed redacted stderr codes. Core provides bounded entries and dropped count; arbitrary input bodies, headers, user names and credentials are never copied into logs.

HTML is embedded through `uc_index_html`, `uc_controller_html` and `uc_settings_html`, served at `/`, `/controller`, `/settings`. Without assets, weak minimal fallback pages are served. Transport responses use no-store, nosniff, no-referrer and frame-ancestor protection.

Host checks from repository root:

```
cc -std=c11 -Wall -Wextra -Werror src/web/runtime.c tests/web/host_main.c tests/web/host_backend.c src/core/uc_core.c -o /tmp/uc-c-http-host
cc -std=c11 -Wall -Wextra tests/web/test_json.c -o /tmp/uc-c-json-test
/tmp/uc-c-json-test
python3 tests/web/test_runtime.py /tmp/uc-c-http-host
```

The HTTP integration check requires local socket permission. The host backend records lifecycle in memory and does not validate PS5 hardware behavior.

默认监听 7456；绑定失败时仅再尝试 7457。两者均失败则启动失败，日志记录失败原因及实际监听端口。页面使用相对 URL，备用端口同样可用。
