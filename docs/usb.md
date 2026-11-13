# USB 手柄接入

当前 USB 支持 **Xbox 360 风格 XInput**，以及由完整报告描述符确认的 **Generic HID Gamepad/Joystick 输入**（实机待验证）。Switch USB 专用协议、Xbox One/Series GIP 和专用 Xbox 无线接收器暂不接入。第三方 2.4GHz 接收器如果实际暴露相同 XInput 接口和报告，可以测试；商品名称或 VID/PID 本身不能证明兼容。蓝牙 Switch 支持不受此收敛影响。

主机回归包括协议、管理器及原生收发逻辑的 syscall 模拟。PS5 ABI 由 GitHub Actions SDK 构建检查，真实设备的接口接管、输入和震动仍需实机验证。

## 识别与接管

1. 用户在设置中启动 USB。要求账号已登录且系统稳定；休眠停止后不自动重启。
2. 遍历 `/dev/ugenB.A`，打开对应 `/dev/usb/B.A.0` 读取设备及当前配置描述符。
3. XInput 路径只选择 alt0 的 `FF/5D/01` 接口，要求唯一匹配接口和一对 interrupt IN/OUT，端点包长 20–64 字节。拒绝畸形、歧义描述符；原装 Sony 与内部蓝牙厂商继续排除。非零 alternate setting、多个匹配接口暂不支持。
4. 对该已验证接口执行 `USB_IFACE_DRIVER_DETACH`，然后 claim。没有 VID/PID 型号白名单要求。detach 不支持或无驱动时仍尝试 claim；claim ioctl 明确不支持（ENOTTY/ENOSYS/EOPNOTSUPP）时允许尝试指定端点，由内核访问权限决定。claim 返回 busy 等其它错误则不打开端点，不复位设备、不修改配置、不分离其它接口。
5. 按描述符端点号打开对应 `/dev/usb/B.A.E`，分别使用非阻塞 `read/write`，不遍历猜测端点，不使用 `USB_FS_*`。空包和 EAGAIN/EINTR 不是拔线。
6. XInput 路径发标准三字节玩家 LED 激活命令；只有真实输入报告头为 `00 14` 且长度为 20–64 字节时才认定输入可用。允许端点填充，不将 LED/status 通知当成错误按键报告。4.8 秒内没有有效输入则失败。
7. 输入解析成 `pad_state`，经 core 映射、会话和现有 PS5 backend 创建独立虚拟手柄。不因设备出现或 LED 命令成功就注册虚拟输出。

启动、首次输入、账号绑定和资源释放沿用现有生命周期。每次轮询读取最多八个报告；空闲槽每 100ms 最多处理一个候选节点。端点读写非阻塞，没有等待 ACK 的 sleep 或无限读取循环；描述符控制 ioctl 仍受固件/内核的超时行为约束，不能把 O_NONBLOCK 理解为所有控制查询都会立即返回。

## 释放与共存

停止、拔线、休眠或失败清理时关闭本应用端点句柄，release 已 claim 的接口并删除自有虚拟输出。FreeBSD 接口没有可依赖的通用驱动重新 attach ioctl，因此 detach 后不能保证系统驱动立即恢复；需要时重新插拔该外部手柄。不会为此 reset 设备。

蓝牙和 USB 可同时运行，共享最多四个虚拟输出；网页测试手柄独占。额外实体手柄仍需独立账号配置。USB 输入静默不直接等于断线，传输健康检查与输入序号分开；需要真实报告续期的已登记接收器保留 watchdog。

游戏震动回传使用 XInput 标准八字节输出；LED 优先，输出背压可重试，过期反馈会停止。运行期震动输出失败不主动关闭可用输入。

## 日志与控制面

设置页显示本次启动最近扫描到但未接入的候选，包含节点、VID/PID、失败阶段和 errno。`GET /api/v1/usb` 的 `rejected` 数组最多保存 16 个按节点去重的诊断快照；它不是实时在线设备列表，重新启动 USB 清空，成功打开对应节点也移除旧失败。Sony 身份显示为 `system_device_excluded` 诊断，不是待桥接候选，也不会 detach。

## Generic HID 输入与诊断

Generic HID 复用通用 HID 描述符解析器与 USB 传输相关的设备映射。只接受 alt0、非 Boot subclass、protocol=0、完整描述符确认 Gamepad/Joystick 的接口；混合键盘/鼠标 Collection、截断描述符、多个 IN 端点或包长超过 512 字节的接口不接管。仅 detach/claim 已验证的接口，打开其唯一 interrupt IN；不打开 OUT，不发送 LED、SET_REPORT 或未知震动报文。

端点输入保留 HID Report ID，按描述符解析按钮、方向帽、摇杆与扳机。多个 Report ID 共享每设备状态，单个报告不携带的控制项保留原值。未知 Report ID 忽略，已知 Report ID 的短包拒绝，连续三个畸形包终止该设备的桥接。没有固定心跳的 HID 可空闲等待首次输入；收到有效输入前不创建虚拟手柄。断线释放本应用虚拟输出，再按原扫描流程处理重新插入。Generic HID 震动与电量尚不支持。

配置诊断保留设备类别、配置索引/值、接口 number/index/alt/class/subclass/protocol，以及端点方向、类型、包长和间隔。对第一个 alt0、非 Boot subclass 的 HID 接口，通过只读标准 GET_DESCRIPTOR（0x22）查询报告描述符，查看顶层 Application Collection 的 Game Pad/Joystick/Keyboard/Mouse usage。接口类别 03 或 VID/PID 本身不证明是 Generic Gamepad；已知 Switch 身份仍标记为专用协议。

报告描述符先做诊断分类，完整且符合接管条件时再交给通用输入解析器。多个 HID 接口只探测第一个符合条件的接口；读取失败、截断和格式不完整都保留诊断，不作为 Gamepad 证据。探测结果按节点、VID/PID、配置摘要与配置索引共享缓存，最多 16 条，失败也缓存。重新启动 USB 清空缓存；配置变化或缓存淘汰后可重新探测。

USB 服务开启期间，每 100ms 最多检查一个已缓存 HID 节点，16 条缓存完整检查一轮约需 1.6 秒（还受事件循环调度影响）。确认控制节点消失时，清除该节点所有描述符缓存和页面诊断记录。支持时比较 `USB_GET_PLUGTIME` 插入时间标记，同时比较节点 inode/device，识别快速拔插或地址复用；使用缓存前也检查设备实例是否变化。此缓存存在性检查本身没有 detach/claim 或输入输出命令。EACCES、EBUSY 等不确定错误不会当作拔出。固件若不支持插入标记，且快速拔插同时复用了节点 inode，轮询可能无法识别这次变化，仍可手动重启 USB 强制重读。未缓存的其它拒绝记录仍是诊断快照。

`GET /api/v1/usb/diagnostics` 仅导出已有快照，不执行新硬件查询。网页日志页的复制/导出会自动包含完整 JSON：最多 8 个接口和 16 个端点，配置描述符前 256 字节、HID 报告描述符最多 512 字节的十六进制数据，以及声明/捕获长度、截断标记、读取 errno、detach/claim 结果。XInput 与 Generic HID 均保留首个原始输入包最多 64 字节；它可能是状态通知，不等同有效按键。

切换启明星模式时，建议先导出当前模式，再切换模式或重新插拔，并另存一份诊断。常规拔插会自动重新读取 HID 描述符；如果没有捕获到设备实例变化，可停止再启动 USB 强制重读。拔出后的 HID 快照会清除，需保留证据时先导出。

关键阶段包括 `control_open`、`device_descriptor`、`device_info`、`configuration_descriptor`、`not_supported_xinput`、`claim_interface`、`input_endpoint_open`、`output_endpoint_open` 和 `short_transfer`。日志还记录选中接口、detach、LED 激活、首包长度/头部、首个有效输入及关闭；相同节点与原因不会每次扫描重复打印。

实机先测试原装手柄独立工作，再启动 USB 插入 XInput 手柄：确认设备列表、输入帧数、独立账号的游戏控制、PS 键、震动、拔插、停止和休眠。验证前不要将主机模拟通过视为 PS5 实机可用。

设备描述符和设备信息查询分别记录 attempted/ok/errno；设备描述符一旦成功，后续信息查询失败也保留真实 VID/PID。控制 ioctl 不受支持仍会明确失败，不猜测配置索引或发送复位命令。
