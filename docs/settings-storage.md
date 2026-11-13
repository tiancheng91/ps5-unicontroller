# 配置与日志

配置文件 /data/unicontroller/settings.json，schema_version=1。字段 recording_level/page_level（off/error/warn/info/debug/trace）、rumble（bool）、swap_ab（bool）、button_mapping（32个0..31的整数）。另有 bluetooth_auto_start（bool，默认false）、bluetooth_auto_trigger（login/resume，默认login）、bluetooth_auto_user（上次手动成功启动蓝牙的账号，默认0，仅服务端更新）。旧配置缺少新增字段时按默认值读取。默认Info、允许震动、无翻转、identity映射。震动只是偏好值；手机后端没有物理马达。

设置分区 `/#settings`；旧 `/settings` 入口兼容。读写通过 /api/v1/settings；手柄关闭且没有创建任务时才允许修改，先校验再持久化后应用。日志 /api/v1/logs?level=info；页面阈值不能恢复后台未记录日志。

持久化采用JSON和同目录0600临时文件、fsync与rename；损坏或未来版本拒绝启动，不覆盖原文件。当前通过进程实例锁保持单实例；尚无目录fsync与并发更新版本校验。记录这些限制而不声称跨进程事务保证。

蓝牙配对密钥独立保存于 bonds.db，不放入JSON偏好配置或普通日志/API响应。自动启动规则见 [蓝牙说明](bluetooth.md)。

翻转 ABXY 按键为标准Cross14↔Circle13与Square15↔Triangle12两组交换，之后单次映射，多源按OR。页面明确显示两组面键交换，避免按厂商印字误判。

为兼容已有 JSON，字段名仍为 `swap_ab`，但其含义现在是完整面键布局切换（A/B 和 X/Y 同时交换）。旧配置为 true 时升级后也会交换 X/Y；默认 false。此项为全局偏好，应用于本程序所有输入源，不按 VID/PID 自动启停。

网页外观（语言、主题、尺寸、导航）与此文件分离，保存在当前浏览器localStorage `unicontroller.appearance.v1`。首次按浏览器语言选择中文/英文；存储不可用时回退默认，不影响后端配置及蓝牙配对。外观区立即应用，不需要先停止输入源；后端手柄设置仍按原有API规则保存。

## 诊断导出

管理页核心事件显示为逐行文本，例如 `[mono 93082.281s] [ERROR] core create_failed generation=1 session=569`。mono 是单调时钟秒数，不能当作日历时间。覆盖计数独立显示；HTTP `/logs` 保留结构化 JSON，方便工具处理。

复制与导出同时收集系统、蓝牙、USB、测试输出的即时状态，以及 `/diagnostics` 返回的 runtime.log 最后32 KiB。核心事件遵循页面日志级别与暂停状态；底层日志和核心事件保留各自来源，不伪造统一时间顺序。某一诊断接口不可用时保留其它部分并写明错误，不把缺失日志解释为硬件正常。

蓝牙虚拟输出创建失败后停止整个蓝牙服务，关闭共享 HCI，再继续必要的虚拟设备清理；不会因下一帧输入自动重新创建。此时所有桥接蓝牙手柄均断开，需手动启用。该保护不能证明共享 HCI 与原装手柄共存安全。

USB运行日志记录 ugen 枚举、VID/PID、描述符筛选和接口打开失败阶段；使用有界、限次输出避免轮询刷屏。不强制解除系统驱动占用，不执行设备重置。

## 日志实现

运行日志使用固定提交的 `rxi/log.c`（MIT），源码随仓库离线编译，来源与校验值见 `vendor/rxi-log/README.md`。上游文件不修改；`src/logging.c` 只适配现有设置级别、单行输出及文件轮转。`src/drivers/common/log.h` 是旧驱动入口的兼容头，不再包含另一套日志实现。

应用代码使用 `uc_log_error/warn/info/debug/trace`，不能直接使用 rxi 的 `log_set_level`：项目仍是 Off=0、Error=1、Warn=2、Info=3、Debug=4、Trace=5，而上游枚举方向不同。`uc_log_set_level` 统一转换；默认 Info。旧驱动的 `log_line` 按 Info 处理，具体错误点可改为相应的 `uc_log_*`。当前是单事件循环，同步输出并刷新，不引入额外日志线程。

`runtime.log` 采用日历时间、级别、源码位置及消息的一行格式；时间使用运行环境的本地时间设置，秒级精度。核心事件消息额外保留 `mono_ms`、generation 和 session，便于与输入时序关联。格式示例：

```text
2026-11-13 18:15:32 ERROR src/ps5/backend.c:104 bind failed rc=0x...
```

消息最多2047字节，超长标记 `[truncated]`；换行、回车和制表符转换为空格，避免设备元信息伪造多行记录。达到1 MiB后，在后续可记录消息到来时轮转为 `runtime.log.1`，最多保留当前文件与上一份；单条记录可使文件暂时超过阈值。轮转失败继续写现有句柄，不递归记录故障。关闭文件后回退 stderr；生产入口仍重定向 stderr 以承接第三方直接输出。

核心环形事件继续供 `/api/v1/logs` 使用，并同时进入统一运行日志。因此完整导出中的核心片段与 runtime.log 可能包含同一事件；两者分别是过滤快照和文件尾部，不是两个独立的输入事件。页面级别只过滤核心页面快照，不改变已经写入的文件。生产环境仍需 Actions 交叉构建和实机验证，不以主机轮转测试代表 PS5 文件系统表现。
