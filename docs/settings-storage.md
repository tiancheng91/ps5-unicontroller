# 配置与日志

配置文件 /data/unicontroller/settings.json，schema_version=1。字段 recording_level/page_level（off/error/warn/info/debug/trace）、rumble（bool）、swap_ab（bool）、button_mapping（32个0..31的整数）。另有 bluetooth_auto_start（bool，默认false）、bluetooth_auto_trigger（login/resume，默认login）、bluetooth_auto_user（上次手动成功启动蓝牙的账号，默认0，仅服务端更新）。旧配置缺少新增字段时按默认值读取。默认Info、允许震动、无翻转、identity映射。震动只是偏好值；手机后端没有物理马达。

设置页 /settings。读写通过 /api/v1/settings；手柄关闭且没有创建任务时才允许修改，先校验再持久化后应用。日志 /api/v1/logs?level=info；页面阈值不能恢复后台未记录日志。

持久化采用JSON和同目录0600临时文件、fsync与rename；损坏或未来版本拒绝启动，不覆盖原文件。当前通过进程实例锁保持单实例；尚无目录fsync与并发更新版本校验。记录这些限制而不声称跨进程事务保证。

蓝牙配对密钥独立保存于 bonds.db，不放入JSON偏好配置或普通日志/API响应。自动启动规则见 [蓝牙说明](bluetooth.md)。

翻转 ABXY 按键为标准Cross14↔Circle13与Square15↔Triangle12两组交换，之后单次映射，多源按OR。页面明确显示两组面键交换，避免按厂商印字误判。

为兼容已有 JSON，字段名仍为 `swap_ab`，但其含义现在是完整面键布局切换（A/B 和 X/Y 同时交换）。旧配置为 true 时升级后也会交换 X/Y；默认 false。此项为全局偏好，应用于本程序所有输入源，不按 VID/PID 自动启停。
