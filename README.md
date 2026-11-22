![UniController for PS5](assets/branding/banner.png)

**中文** · [English](README.en.md)

# UniController for PS5

用第三方手柄玩 PS5 游戏。UniController 在主机上运行，把蓝牙或 USB 手柄的输入转给虚拟 DualSense。你可以在手机上配对手柄、查看按键输入和测试震动，也可以直接把手机当作测试手柄。

**使用条件**：一台能通过 Payload Manager 或兼容加载器运行自制 ELF 的 PS5。首次设置建议备好原装 DualSense，并将手机或电脑接入与 PS5 相同的可信局域网。

[下载正式版](https://github.com/tiancheng91/ps5-unicontroller/releases/latest) · [更新日志](CHANGELOG.md) · [反馈问题](https://github.com/tiancheng91/ps5-unicontroller/issues)

## 第一次连接

1. 从正式版的 **Assets** 下载 `unicontroller.elf`，上传到 PS5，用加载器运行。
2. 在手机或电脑浏览器打开 **`http://<PS5-IP>:7456/`**。把 `<PS5-IP>` 换成主机的局域网地址。
3. 按连接方式操作：
   - **蓝牙**：让手柄进入配对模式。在「手柄 → 新增蓝牙设备」开始搜索，找到设备后点「配对」。Switch 模式优先选有名称的 **Bluetooth Classic** 条目。以后使用同一模式，通常只需打开手柄即可回连。
   - **USB**：在「设置 → 连接服务」点「启动 USB」，用数据线接入手柄或插入兼容接收器。若提示等待输入，按一下按键或推动摇杆。
4. PS5 弹出使用者选择画面时，用这只手柄选择账号。在网页「设备详情」确认输入帧随操作增加，再进入游戏。**本地双人游戏请为两个手柄选择不同的 PS5 用户。** 要换玩家，点「重新选择使用者」。

也可以在设置中安装 PS5 桌面图标。它打开同一配置页，使用前仍需加载 ELF。升级时先点「停止 UniController」，再替换并运行新 ELF。保留 `/data/unicontroller/` 即可沿用设置和配对记录。[图标安装说明](docs/launcher.md)

## 我的手柄该选什么模式？

多模式手柄会随模式改变设备身份和功能。一代 **Switch Pro 模式**对输入和游戏震动的支持最完整；有线手柄也可优先尝试 **Xbox 360 / XInput 模式**。

| 手柄模式              | 蓝牙                                              | USB                                                                         |
| --------------------- | ------------------------------------------------- | --------------------------------------------------------------------------- |
| 一代 Switch Pro       | Classic 输入与震动                                | `057e:2009` 输入与震动，已有实机反馈                                        |
| Switch 2 Pro          | BLE 可发现，配对未开放                            | `057e:2069` 实验性按键与摇杆输入；暂无震动                                  |
| 一代 Joy-Con          | 左右手柄分别使用                                  | —                                                                           |
| Xbox                  | 部分 Xbox HID 型号；内置 BLE 白名单含 `045e:0b13` | Xbox 360 / XInput；不支持 Xbox One/Series 的 USB GIP 和专用 Xbox 无线接收器 |
| Android / Generic HID | 按 HID 描述符解析输入                             | 支持可解析的 HID 手柄接口；暂无通用震动输出                                 |

**Switch 2 Pro 有线模式**已在 PS5 Pro 13.20 上验证基础输入。接入后如果电视突然没声音，请到 PS5「声音 → 音频输出」选回电视或耳机，并关闭输出设备自动切换。

例如，GameSir Nova Lite 在 Classic 的 Switch Pro 模式下可使用震动，切到上报 `3537:1041` 的 Generic HID 模式后仍可操作按键，但当前没有震动输出。型号相同也要看实际连接模式。[GameSir 模式说明](docs/gamesir-nova.md) · [蓝牙兼容性](docs/bluetooth.md) · [USB 兼容性](docs/usb.md)

**找不到你的型号，或按键识别不对？** UniController 会用设备的 VID/PID 查询 SDL 手柄表，辅助识别型号，并根据设备类别为蓝牙 Classic 选择候选协议。USB 则结合实际接口和报告描述符识别协议。[查看本版本使用的 SDL 设备表](https://github.com/libsdl-org/SDL/blob/b2de7a19ad3c0d5063a5fb8e4cf5deeeb9ce4560/src/joystick/controller_list.h)

如果手柄使用项目已支持的协议，可以参考示例，按实际连接方式和设备标识填写规则，通过 FTP 上传到 PS5 的 `/data/unicontroller/controllers.json`，再重启 UniController 测试。配置可指定已有协议，也可调整通用 HID 的按键映射。**Switch 2 Pro 的 BLE 模式目前不支持，通过配置也无法启用配对**；可改用 USB 测试基础输入。[配置指南](docs/controller-config.md) · [示例文件](examples/controllers.json)

## 常用设置

- **按键布局**：Switch 默认按位置映射，下 B → ×、右 A → ○、左 Y → □、上 X → △。Home 对应 PS，截图键对应触摸板点击。要同时交换 A/B 和 X/Y，在该手柄详情中开启「翻转 ABXY 按键」。
- **震动**：在设备详情中允许震动并点「测试震动」。测试还受设置中的震动总开关控制。当前模式没有可用输出时，页面会说明原因。游戏反馈支持传统马达震动。
- **休眠与启动**：休眠时释放虚拟手柄、停止实体连接服务，保留蓝牙配对。首次安装默认只开启蓝牙自动启动。勾选「自动启用 USB」并保存后，USB 也会按所选时机自动启动；升级会沿用已保存的选择。基础延迟默认 10 秒，范围 5–60 秒；USB 按该延迟启动，蓝牙再晚 3 秒。加载 ELF 时前台账号已登录，也按此配置启动。需要测试登录前连接时，可手动启动服务，或选用实验性的「休眠恢复后」。
- **手机模拟**：先停止蓝牙和 USB 服务，再打开「模拟」创建手柄，在 PS5 上选人。横屏点「专注操控」可同时操作双摇杆和按键。休眠后需手动重新创建。

## 出现问题时

| 现象                                 | 检查方法                                                                                                                                     |
| ------------------------------------ | -------------------------------------------------------------------------------------------------------------------------------------------- |
| 网页打不开                           | 在加载器确认 ELF 已运行，核对 PS5 地址和端口 7456。端口被占用会导致程序退出。                                                                |
| 配对成功但按键没反应                 | 打开设备详情按几下方向键。若输入帧不增加，检查连接模式；Switch 模式请选择 Classic。USB 通用 HID 等待首份输入超时后，可重新插入并按键。       |
| 输入帧在增加，但当前玩家的操作没反应 | 手柄可能分给了另一个 PS5 本地用户。在这只手柄的「设备详情」点「重新选择使用者」，然后在 PS5 上选择当前要操作的用户，再试一次。无需重新配对。 |
| 能操作系统菜单，进游戏没反应         | 先检查两只手柄是否选了同一个本地用户。这种情况下，游戏可能仍由原手柄控制。按下方说明切换主手柄或加入第二位玩家。                             |
| 原装 DualSense 也失去响应            | 用手机停止连接服务并导出日志。需要时停止 UniController，通过 PS5 正常电源操作恢复。                                                          |

### PS5 如何分配手柄和玩家？

网页的输入帧说明 UniController 收到了按键。PS5 上的使用者选择决定这只手柄分给谁，游戏还可能要求玩家按键加入。例如，原装手柄正在为用户 A 玩游戏，第三方手柄也选了 A，这时即使能用它操作系统菜单，游戏也不会因此多出一位独立玩家。

- **单人换手柄**：关闭原装手柄，按第三方手柄的 PS/Home 键，选择正在玩游戏的用户 A。
- **本地双人，各自控制**：原装手柄留给 A。在第三方手柄详情点「重新选择使用者」，在 PS5 上选另一个本地用户 B，再按游戏提示加入。
- **两人合控同一玩家**：对支持的手柄，在 PS5「设定 → 协助工具 → 控制器」开启「使用第二控制器进行辅助」，然后两只手柄都选 A。PS5 会把两只手柄的输入用于同一位玩家。[PlayStation 辅助控制器说明](https://www.playstation.com/zh-hans-cn/support/hardware/second-controller-assistance/)

反馈时请附上网页显示的版本和提交编号、PS5 型号与固件、手柄型号与连接模式、复现步骤，以及「日志 → 导出日志」的结果。请勿上传含配对密钥的 `bonds.db`。[提交 Issue](https://github.com/tiancheng91/ps5-unicontroller/issues) · [实机测试清单](docs/test-checklist.md)

## 开发与贡献

后端使用 C11，网页资源随 ELF 离线提供。在仓库根目录运行：

```sh
make preview        # 在浏览器查看演示数据
make test           # 主机端回归测试
npm ci
make test-browser   # 需要 Chrome 或 Chromium
```

PS5 交叉构建使用固定版本的 SDK，可直接使用 GitHub Actions。新增手柄适配或报告测试结果时，请注明具体连接模式，以及已验证的按键、摇杆和震动功能。[开发文档](docs/README.md) · [自定义设备配置](docs/controller-config.md) · [Actions 构建](https://github.com/tiancheng91/ps5-unicontroller/actions/workflows/build.yml)

## 许可与致谢

UniController 采用 GNU GPLv3。继承的蓝牙桥接代码保留 GPL-3.0-or-later 许可；随附组件保留各自的许可和版权声明。[LICENSE](LICENSE) · [NOTICE](NOTICE)

感谢 StonedModder 和 seregonwar 的 Ghostcontrol 项目，为在 PS5 上创建虚拟 DualSense 并分配给使用者提供了早期研究基础。[Ghostcontrol](https://github.com/StonedModder/Ghostcontrol-PS5-USB-Controller-Patcher)

蓝牙桥接、配对和手柄协议代码衍生自 AngelLopezTorres 的 ps5-wireless-pads，其上游是 X-F1REBALL-X 的 PadBridge PS5 和 sinfiltros 的 AnyPad-PS5。感谢这些项目的作者，也感谢 SDL 和 Valve 提供用于识别手柄型号与类型的设备表。[ps5-wireless-pads](https://github.com/AngelLopezTorres/ps5-wireless-pads) · [PadBridge PS5](https://github.com/X-F1REBALL-X/PadBridge-PS5) · [SDL 数据来源](vendor/sdl/README.md)
