# 配置与日志

配置文件 /data/unicontroller/settings.json，schema_version=1。字段 recording_level/page_level（off/error/warn/info/debug/trace）、rumble（bool）、swap_ab（bool）、button_mapping（32个0..31的整数）。默认Info、允许震动、无翻转、identity映射。震动只是偏好值；手机后端没有物理马达。

设置页 /settings。读写通过 /api/v1/settings；手柄关闭且没有创建任务时才允许修改，先校验再持久化后应用。日志 /api/v1/logs?level=info；页面阈值不能恢复后台未记录日志。

持久化采用JSON和同目录0600临时文件、fsync与rename；损坏或未来版本拒绝启动，不覆盖原文件。当前单进程写入，后续增加实例锁、目录fsync与并发更新版本校验。记录这些限制而不声称跨进程事务保证。

本阶段没有蓝牙配对记录。后续密钥将独立存储，不放入JSON配置或普通日志/API响应。按设备覆盖设置待物理设备会话实现后再加入。

A/B翻转为标准Cross14/Circle13交换，之后单次映射，多源按OR。页面明确显示Cross/Circle，避免按厂商印字误判。
