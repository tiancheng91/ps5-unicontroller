# GameSir Nova 系列与协议身份

Nova Lite（启明星一代）、Nova 2 Lite（启明星二代）、Nova（NS版）是不同产品。本轮按用户使用范围收敛到Switch与XInput，保留已有蓝牙通用HID，不新增USB DInput/HID接入。

## 当前接入范围

| 产品/上报身份                  | 连接与条件                                              | 当前实现                                              |
| ------------------------------ | ------------------------------------------------------- | ----------------------------------------------------- |
| Nova Lite 3537:1040            | USB或接收器的FF/5D/01接口，并收到00/14头的20字节输入    | 原有XInput解析，增加已知设备LED激活与型号显示         |
| Nova 2 Lite 3537:100f          | USB的FF/5D/01接口及有效XInput报告                       | 同上，型号区分二代                                    |
| Nova 2 Lite Receiver 3537:1098 | USB接收器实际呈现XInput接口及有效报告                   | 同上，单独显示接收器身份；不等同Xbox专用无线协议      |
| Nova Lite 3537:1041            | HID/DInput模式                                          | 仅型号识别，不新增USB输入；切到XInput或Switch模式测试 |
| 057e:2009                      | 实际符合已实现Switch协议                                | 显示Switch Pro身份，不能确定真实厂商或代次            |
| 045e:028e / 02e0 / 0b13        | 分别是USB Xbox360、Xbox蓝牙身份，结合传输和实际报文选择 | 复用相应既有入口，不反推具体第三方型号                |
| 054c:05c4 / 09cc               | DS4协议身份                                             | 已有蓝牙DS4入口；USB Sony设备仍被保护性排除           |
| Nova NS原生ID                  | 未有可靠枚举证据                                        | 不编造；若实际使用Switch协议身份，则走已有协议        |

这些Nova原生ID可能对应接收器，USB节点在线不等于实体手柄在线，因此不通过USB健康检查续期最后一次按键；500ms无实际输入仍释放。固件若只在状态变化时上报，会影响长按，需实机确认。

仅增加VID/PID显示名称不代表实机兼容已通过。原生ID只说明设备上报的身份，不能认证真实制造商；共享模拟ID不得重命名成某一品牌。例如MOBAPAD若报045e:02e0，只能按Xbox蓝牙协议处理，不能把所有相同ID都显示为MOBAPAD。

现有蓝牙Android/通用HID仍根据HID描述符解析，布局和稀疏上报行为按型号验证。Android不是固定报文格式；USB Dongle只描述连接形态，不能仅由这个词决定协议。

## XInput与DirectInput

两者本来是Windows输入API。XInput面向Xbox风格的统一状态，DirectInput覆盖较广设备、布局不固定。PS5不调用这些Windows库；我们实际实现的是Xbox360 USB、Xbox蓝牙、Nintendo报告和HID描述符解析，不能把“支持一种API”理解为兼容所有设备。

输入和震动是不同链路。本轮沿用蓝牙连接后短震动；USB游戏震动回传已接通支持的Switch/XInput路径，仍需按型号实测。

## 型号配置与身份边界

内置型号与协议引用位于 [src/drivers/protocols/controllers.json](../src/drivers/protocols/controllers.json)，构建时校验并嵌入 ELF。用户可通过可选的 `/data/unicontroller/controllers.json` 提供运行时覆盖，格式和加载边界见[手柄配置](controller-config.md)，示例见 [examples/controllers.json](../examples/controllers.json)。

运行时使用固定版本 LibJSON 0.2.5 事件解析器，最多 32 KiB、32 条规则，启动时加载一次。文件不存在正常使用内置规则；整份文件任一项无效时全部回退内置并记录错误。通过 FTP 手动上传后重启 payload，不热重载，也没有网页写入 JSON 的 API。

匹配必须精确包含 transport（classic/ble/usb）与四位十六进制 VID/PID，没有通配规则。显示别名不认证设备；同一个模拟身份不能被自动视为 Nova，也不能因改名就开放原本不支持的接口。USB 的 xinput-usb/switch-usb hint 仍受既有接口描述符约束，不能开启 DInput 或绕过 Sony/内核驱动保护。

必须拆开三类信息：

1. 内置型号元数据和运行时显示别名：用于说明观测到的身份，不证明真实制造商。
2. 传输/协议匹配：由 USB/Classic/BLE 作用域、描述符、握手和有效报告共同决定；仅能选择已有协议。
3. 原始通用 HID 字段映射与 core 用户偏好：JSON mapping 只绑定通用 HID 字段；settings.json 保存 Switch / PS·Xbox 面键布局切换（A/B 与 X/Y）和全局 32 位按键映射。当前没有新增每会话轴方向反转。

配置不接受脚本、原始 USB/HCI 命令或组合键。独立上报的 HID 背键才能独立映射；映射为 touchpad 仅产生 PAD_TOUCHPAD 点击，不产生触控坐标。Switch Capture 已有默认映射。

输入与反馈链路仍分开验证：游戏震动回传未实现，运行时型号覆盖不会新增输出协议能力。

## 证据与验证

- [Linux xpad](https://github.com/torvalds/linux/blob/master/drivers/input/joystick/xpad.c)明确列出3537:1040 Nova Lite、3537:100f Nova 2 Lite，类型为Xbox360。
- [Nova Lite实测记录](https://github.com/whytheperson/PS3xPAD-Controller-Compatibility)记录1040 XInput及1041 HID/DInput，不能把PS3验证视为PS5验证。
- [Nova 2 Lite接收器测试记录](https://github.com/AdityaHebballe/winebus-high-rate-input-fix)记录3537:1098接收器Xbox模式。
- [GameSir官方FAQ](https://gamesir.com/pages/faq-questions-about-nova-lite)说明XInput/NS/DS4模式切换，模式与固件需随实测记录保存。
- [Microsoft API比较](https://learn.microsoft.com/en-us/windows/win32/xinput/xinput-and-directinput)用于术语澄清。

主机测试覆盖型号区分、未知身份不误识别、模拟身份不冒充Nova、USB接口约束与LED激活范围、网页型号显示。实机请记录具体型号、固件、连接/模式、枚举ID、账号与输入计数；三款产品不能相互代替验收。
