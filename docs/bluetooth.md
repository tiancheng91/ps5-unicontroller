# 蓝牙输入接入（实机待验证）

## 范围

默认关闭，仅网页手动启动。启动时选择首台手柄的已登录PS5账号；每个实体输入源占用独立虚拟输出，额外手柄在设备卡片绑定其他已登录账号。网页测试手柄与蓝牙互斥，不把两个来源混到同一会话。重启/休眠后不自动打开HCI；继续要求明确工作状态持续5秒、所选账号已登录，再手动启动。

当前接入Classic HID与BLE HID。已知Switch Pro与Joy-Con L/R走专用配置，已知Xbox蓝牙型号走Xbox配置；未知型号必须通过可用HID描述符解析，不把所有非Switch设备强制按Xbox格式读取。Switch2、Joy-Con合并、厂商私有蓝牙协议、Xbox专用无线接收器不在支持范围。

第三方Switch Pro兼容模式若使用057e:2009，BLE入口被拒绝并不保存新的BLE配对，使用Classic入口。不同蓝牙地址不凭相似性合并；同VID/PID不等于同设备。其他已知Nintendo BLE身份仍需要实机验证。

## 流程

1. 保持原装手柄可用，登录两个账号。
2. 控制面选独立账号，关闭网页测试输出，再点启动蓝牙。
3. 点击配对60秒，把实体手柄放入其蓝牙配对模式。
4. 页面分别显示连接、输入帧、电量和是否绑定。连接成功不等于输入有效。
5. 实体连接成功1秒后，支持震动的手柄短震动200ms，每次连接仅提示一次；受设置里的震动开关控制。协议初始化未完成时最多等到连接后5秒，断线/停止/休眠取消提示，不阻塞输入。
6. 首个有效输入后才创建虚拟设备；初始化期间不会重放过期的按键。
7. 使用相同独立账号进入游戏，测试PS键、方向、面键和双摇杆。
8. 通过忘记设备删除本应用配对记录；休眠后需手动启动，已保存设备再尝试回连。

首个有效输入源使用启动时所选账号，额外设备保持连接待分配账号。蓝牙与USB合计最多四个输出，同一账号不会分配给两台实体手柄。当前输入静默500ms释放按键，这是偏向安全的策略；只在状态变化时上报的设备可能需要专门的保活/租约适配，不能仅凭通用HID识别成功就声称所有型号已可用。游戏震动从各虚拟设备独立读取，按会话转发到对应实体槽位；旧反馈超过500ms停止。读取失败只禁用对应设备的反馈，不阻断输入。

## 生命周期与安全边界

HCI使用旧项目的受限访问实现，不reset或detach系统驱动，不提高线程优先级。第一HCI功能与系统功能是否完全隔离尚未证明。用户手动启动后才打开；首次实机仍须确认原装DualSense共存正常，不能把模拟测试当成共存验证。

停止/休眠先尝试释放/删除自有虚拟输出，同时保证即使删除失败也关闭共享蓝牙传输；保留句柄继续清理。初始化等待期间持续观察电源。配对任务有期限，连接、通道、报文重组均有上限。不开放自动切换到其他HCI接口。

## 存储与日志

本应用独立 /data/unicontroller/bonds.db，格式继承迁移的host栈；临时文件权限0600、fsync后原子替换。与settings.json隔离，不读取/清理PS5系统配对记录。原项目的配对文件不会自动迁移。

保留驱动匹配、初始化、收包/解析计数、状态、错误原因等元信息；不记录配对密钥或原始HCI/ACL载荷。移植栈的兼容日志统一归Info，受后台记录阈值控制。结构化core日志与平台runtime.log仍可从网页导出。

## 来源与参考

第三方代码许可和来源集中记录在根目录 NOTICE 与 LICENSE。

- [Linux hid-nintendo](https://github.com/torvalds/linux/blob/master/drivers/hid/hid-nintendo.c)：Switch初始化与型号处理参考；没有移植Linux内核接口。
- [Linux hid-generic](https://github.com/torvalds/linux/blob/master/drivers/hid/hid-generic.c)：通用HID接入仍依赖HID核心解析器。
- [SDL PS5 joystick](https://github.com/ps5-payload-dev/SDL/blob/release-2.30.x-ps5/src/joystick/ps5/SDL_ps5joystick.c)：系统手柄读取与映射参考。其PS5后端使用scePadReadState，不能直接替代第三方蓝牙配对/HCI/GATT。
- [SDL Switch HIDAPI](https://github.com/ps5-payload-dev/SDL/blob/release-2.30.x-ps5/src/joystick/hidapi/SDL_hidapi_switch.c)：可继续参考协议兼容，没有引入整个SDL。

## 验证

make test-bluetooth、test-bt-manager、test-bt-ui、test-web，随后Actions实际链接ELF。主机默认不具备PS5 HCI：启动返回明确失败，不模拟成功连接。实体手柄兼容矩阵必须在PS5实机后补充。

## BLE 接入策略

BLE 默认拒绝，只有内置设备记录明确 `ble: true` 或用户覆盖规则明确开放的身份可以接入。当前内置仅开放 Xbox `045e:0b13`，依据为端到端模拟测试，PS5 实机支持仍待验证。设备名称、Classic profile、USB ID 或 HID 布局本身不授予 BLE 权限。Switch 家族身份继续拒绝，即使用户配置 `ble: true` 也不绕过。

用户覆盖文件 `/data/unicontroller/controllers.json` 的设备规则可使用 `match: {transport: "ble", vid: "1234", pid: "5678"}` 和 `ble: true`。`ble: false` 可关闭内置已开放身份；字段仅允许用于 BLE 规则。通用 HID 仍可用于显式开放的未知设备。

首次需要连接读取 PnP ID。当前栈仍先完成加密握手再查询 GATT，未改为配对前查询；身份未通过前不保存新密钥或发布手柄就绪事件。缺少/读取失败的身份拒绝。已有配对在发起回连前也检查权限，记录保留供用户查看与删除，不自动删除历史数据。拒绝身份暂缓再次尝试 60 秒。输入通知订阅失败或没有可订阅输入报告时退出初始化。
