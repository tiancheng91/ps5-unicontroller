# 手柄配置：内置协议与运行时覆盖

内置协议身份、型号名称、HID 布局与 USB 特殊标记维护在驱动源码 [src/drivers/protocols/controllers.json](../src/drivers/protocols/controllers.json)。这是开发者维护的编译输入，由 `tools/embed_controllers.py` 严格校验并生成 `build/controller_registry.inc`，随 ELF 嵌入；不是要求终端用户编辑的设置文件。协议解码、握手和输出编码仍由既有 C 实现提供。

用户可以另行提供 `/data/unicontroller/controllers.json`，为观测到的设备身份选择既有协议、显示别名或通用 HID 字段映射。运行时覆盖与源码内置表使用不同结构，不能直接用完整内置文件替代用户覆盖文件。

## 内置表的维护边界

优先维护 Switch 兼容协议与 USB Xbox 360/XInput 协议，同协议的型号共用解析和默认按键布局，不逐型号复制映射。Xbox 蓝牙单独适配，不把它当作 USB XInput 报文。已有 DS4、DualSense、Joy-Con 和通用 HID 适配继续保留。

设备条目用于身份识别、显示名称、协议选择以及必要例外，例如 USB 初始化、电量或震动能力限制。名称相同不代表传输协议相同，新增型号也不会自动获得 BLE 接入权限。

通用 HID 的 `layouts` 可以通过 `extends` 复用基础布局，`bindings` 只声明差异。例如：

```json
"android-ipega": {
  "extends": "android",
  "bindings": {
    "button3": "l3",
    "button6": "r3",
    "button13": "none"
  }
}
```

子布局覆盖基础布局的同名字段；`none` 显式禁用该绑定。未知基础布局、循环引用、无效字段在构建时拒绝，合并后的映射仍受大小限制。此机制仅用于开发者维护的内置表，构建时展开，不增加运行时解析或改变用户 JSON 格式。

不引入 SDL 映射数据库。当前通用 HID 映射依据 HID Usage，不能直接使用 SDL 的平台相关按钮/轴编号。保留已有型号差异，后续仅为实际需要的兼容性问题增加例外；用户的 翻转 ABXY 按键和全局按键设置仍独立生效。

## SDL 设备分类

直接引入 SDL 原始 [controller_list.h](../vendor/sdl/controller_list.h) 和 [controller_type.h](../vendor/sdl/controller_type.h)，不链接 SDL 库，也不生成中间名称 JSON。固定版本、许可和更新方法见 [vendor/sdl/README.md](../vendor/sdl/README.md)。普通构建不访问网络。

Classic 协议选择优先使用用户覆盖和本地规则（包括已维护的通用 HID 布局），其次使用 SDL `k_eControllerType_*` 选择已有协议候选：Switch Pro、Joy-Con、Xbox One/Elite、PS4、PS5/Edge。SDL 类型是设备家族分类，不能跨传输直接等同于报文协议。USB XInput、Xbox One USB GIP、Switch 2 等不会因此进入 Xbox Bluetooth 或一代 Switch 解析器；本轮未扩展 USB。

SDL 的 `SwitchInputOnlyController` 是已知分类，主要描述 USB 仅输入设备，不等同于未知型号或单独的“无震动”标记。SDL 的 Switch 驱动仅在非蓝牙连接设置 input-only 状态，蓝牙仍使用完整 Pro 协议；因此本项目在 Classic 下也将其作为 Switch 候选，BLE 仍拒绝，本分支不改变 USB 处理。参考 [SDL Switch 驱动](https://github.com/libsdl-org/SDL/blob/main/src/joystick/hidapi/SDL_hidapi_switch.c)。

新增 SDL Switch Pro 候选必须通过 `0x02` 设备信息及完整输入验证；Sony/Joy-Con 候选必须收到完整输入，Xbox 候选必须收到主输入报告，单独的电量或 Guide 报告不能启用虚拟输出。已知但缺少 Classic 实现的专用协议在取得 SDP 身份后标记为不支持并断开；首次扫描没有 VID/PID 时不能提前确认此状态。未知或通用 HID 家族继续读取描述符，描述符可用才解析。

显示名称优先级：用户别名 → 本地非空名称 → SDL 显式名称 → 广播／协议／通用名称。SDL 多数条目的名称指针为 NULL，不把源码注释转换成设备名。重复 VID/PID 按原表顺序取第一条，与 SDL 查找方式一致。本地表继续维护布局、例外及能力限制；SDL 条目不会自动开启 BLE 或默认震动。

本地表不维护 8BitDo (`2dc8`) 条目或 Android HID 布局绑定，名称及 Classic 家族候选以 SDL 为准。SDL 的 `8BitDoController` 专用类型当前没有实现，会显示不支持；SDL 未收录的身份仍按普通未知设备处理。USB 继续按实际接口和报告描述符选择已实现的入口，不根据品牌强制指定协议。

`ds5` 已有输入、输出和初始化实现；原装 Sony DualSense 身份仍保留给系统，配置不能绕过。BLE 继续白名单策略，并拒绝 Switch 家族；设备出现在 SDL 表中不表示已通过 PS5 实机验证。

## 启动加载与失败处理

运行时复用项目现有的严格 JSON 解析器，不引入外部解析库。文件上限 32 KiB，最多 32 条设备规则，只在 payload 启动时加载一次；没有热重载。文件不存在是正常情况，直接使用内置规则。

文件语法、版本、字段、重复匹配、引用或映射任一项无效时，整份覆盖被拒绝，回退到内置规则并记录错误；不会部分应用。未知 profile 同样使整份文件失败。修改后需重启 payload 才会生效。

通过 FTP 手动上传文件；当前没有用于上传或写入此 JSON 的网页 API。先停止输入源再重启 payload，不把重新启动配置与自动恢复蓝牙/USB混为一谈：输入源仍默认关闭。

## JSON 结构

完整可上传示例见 [examples/controllers.json](../examples/controllers.json)。以下 ID 为演示值，不对应兼容性声明：

```json
{
  "schema_version": 1,
  "devices": [
    {
      "match": {
        "transport": "classic",
        "vid": "1234",
        "pid": "5678"
      },
      "name": "My generic HID controller",
      "profile": "generic-hid",
      "mapping": {
        "button1": "touchpad",
        "lx": "x",
        "ly": "y",
        "rx": "z",
        "ry": "rz"
      }
    },
    {
      "match": {
        "transport": "usb",
        "vid": "1234",
        "pid": "5679"
      },
      "name": "My XInput-compatible controller",
      "profile": "xinput-usb"
    }
  ]
}
```

顶层只包含 `schema_version: 1` 与 `devices` 列表。`match.transport` 必填，只能为 `classic`、`ble`、`usb`。原有 VID/PID 规则保持兼容：`vid`、`pid` 必须成对出现，值为四位十六进制字符串。

Classic 另支持 `match.address`（`AA:BB:CC:DD:EE:FF`，大小写均可）及 `match.name`（区分大小写的完整设备名）。至少提供 VID/PID、地址、名称中的一种；提供多个条件时必须全部匹配，没有通配符。规则优先级按地址、名称、VID/PID 依次比较，因此精确地址覆盖名称规则，名称规则覆盖仅 VID/PID 的规则；同一组条件重复定义会拒绝整份配置。`match.name` 用于匹配广播或远端查询的名称，外层 `name` 仍只是显示别名。

名称和地址条件本轮仅开放给 Classic，不改变 BLE 白名单或 USB 选择规则。带名称／地址条件的规则暂不支持原始 `mapping`；通用 HID 字段映射继续使用原有 VID/PID 规则。

例如只对一台 PowerA 手柄试验 Switch 协议：

```json
{
  "schema_version": 1,
  "devices": [
    {
      "match": {
        "transport": "classic",
        "address": "30:31:7D:6E:97:F6",
        "vid": "0000",
        "pid": "0000"
      },
      "name": "PowerA Enhanced Wireless Controller",
      "profile": "switch"
    }
  ]
}
```

地址替换为自己的设备地址；`0000:0000` 是该型号反馈的蓝牙 SDP 身份，不是 USB 的 `20d6:0002`。配置按实际蓝牙身份匹配，不伪造 VID/PID。确认 Classic 设备名精确为 `Lic Pro Controller`、`Nintendo Wireless Gamepad` 或 `Wireless Gamepad` 且 VID/PID 均为零时，也会自动进入 Switch 候选流程，不要求额外配置。

这类候选（包括显式选择 `switch` 的 Classic 覆盖规则）必须在认证、加密和 HID 通道就绪后，通过只读子命令 `0x02` 确认 Pro 类型 `0x03` 或 Licensed Pro 类型 `0x06`，再收到完整 `0x30` 输入，才通知桥接层连接成功。查询最多发送成功 3 次、总写尝试 16 次、整体期限 2.4 秒；拒绝、其他类型或超时则断开。有效类型作为提示保存在该地址的配对记录中，回连仍重新验证。缺失名称时最多等待远端名称查询 2 秒。

候选名称参考 [SDL 类型识别](https://github.com/libsdl-org/SDL/blob/main/src/joystick/SDL_joystick.c)，另参见 [Valve 的 PowerA 名称规则](https://github.com/ValveSoftware/steam-devices/blob/master/60-steam-input.rules) 和 [Linux `joycon_read_info`／Licensed Pro 类型](https://github.com/torvalds/linux/blob/master/drivers/hid/hid-nintendo.c)。名称和类型只是协议兼容证据，不是品牌认证。已知 Nintendo profile 使用可选设备信息和工厂校准查询，失败保持默认校准；候选使用同一 Nintendo 模块，Licensed Pro 不读取未知 SPI 布局。识别顺序和设备差异见[蓝牙识别顺序](bluetooth.md#蓝牙识别顺序)。配置不能修复发生在此前的蓝牙认证／密钥错误。

规则可选字段：

| 字段      | 含义                                               |
| --------- | -------------------------------------------------- |
| `name`    | 1..63 个可打印 ASCII 字符的显示别名，不是型号认证  |
| `profile` | 选择已有协议，不能定义新解码器                     |
| `mapping` | 原始通用 HID 的按键/轴字段映射，不是解码后全局映射 |

profile 白名单为 `switch`、`joycon-left`、`joycon-right`、`ds4`、`ds5`、`xbox`、`generic-hid`；USB 专用入口仅有 `xinput-usb`。协议仍受传输及现有驱动能力限制，不能把任意 profile 跨传输使用。`generic-hid` 表示请求 HID 描述符解析，不意味着兼容任意报告格式。

USB profile hint 只能选择已经实现且接口描述符满足要求的入口，不能凭改名把 DInput 转成 XInput。配置不能绕过 Sony/内部设备保护、系统驱动占用、接口所有权、报告长度、初始化时限、超时释放或休眠清理；不会 detach Sony 系统驱动。

## 原始 HID 映射与用户设置

配置校验与 Classic 内置协议选择共用规则。只写 `mapping` 而省略 `profile` 时，如果设备会选择专用协议或属于未实现的专用家族，则拒绝配置，并提示显式设置 `"profile": "generic-hid"`。显式选择后仍需有效 HID 描述符；不会自动把专用报文转换为 Generic HID。已有内置 HID 布局（如 `powera-moga`）优先于 SDL 推导，避免更新设备表后改变解析路径。

`mapping` 仅作用于通用 HID 解码路径。来源键为 `button1`..`button32` 或轴角色 `lx/ly/rx/ry/l2/r2`；轴值为 `x/y/z/rx/ry/rz/slider/dial/brake/accelerator/accel/none`。轴角色说明报告字段如何组成标准状态，不是会话用户的方向偏好。

按键目标为 `cross/circle/square/triangle`、`l1/r1/l2/r2/l3/r3`、`create/share/options/ps/touchpad`、`up/down/left/right/none`。映射不能重写 Switch/Xbox 等专用协议的字节格式。JSON 不接受方向反转选项、脚本、函数名、原始 USB/HCI 命令或任意文件路径。

`settings.json` 继续保存网页/core 偏好，例如日志级别、允许震动、翻转 ABXY 按键和全局 32 位按键映射。翻转 ABXY 按键对应标准 PS5 Cross14↔Circle13、Square15↔Triangle12，再执行一次全局按键映射。当前没有新增每会话摇杆轴反转设置；不要把协议层字段绑定当作已实现的会话偏好。

## Touchpad 与背键

`touchpad` 对应 `PAD_TOUCHPAD` 点击位，不是触控坐标、多指手势或滑动。Switch Capture 已有默认映射，无需另外增加组合键。

独立上报的通用 HID 按键可映射为 `touchpad`。背键只有在设备把它作为独立 HID 字段上报时才能独立映射；如果固件只是重复其他按键，软件不能从相同报告中识别它。当前不支持通过 JSON 定义按键组合或宏。

## 开发者维护内置表

| 内置节点   | 内容                                                                   |
| ---------- | ---------------------------------------------------------------------- |
| `profiles` | 既有 implementation、显示名、输出编码能力、完整报告要求、震动刷新间隔  |
| `layouts`  | 命名 `bindings`：通用 HID 按键与轴角色字段                             |
| `devices`  | VID/PID、显示名、bluetooth_profile、hid_layout、USB 特殊标记与身份来源 |

内置 layout 只接受 `bindings`。构建期未知字段、重复 VID/PID/JSON 键、非法 implementation、错误引用与越界映射均导致构建失败。新报文协议仍需 C 实现；生成物不手工维护、不入 Git。

USB 对所有通过 FF/5D/01 接口校验的 XInput 候选发送标准 LED 激活，不再保留逐型号激活标记。`usb.input_watchdog` 用于需要实际输入续期的接收器身份，不允许仅因 USB 节点在线而保留旧按键。启用后 500ms 没有有效输入即释放按键、摇杆归中；不要求按键发生变化，新报文即可续期。

```sh
python3 tools/embed_controllers.py
python3 -m unittest discover -s tests/config -v
make test
```

## 身份与反馈能力

显示别名、上报 VID/PID 和 profile hint 都不是制造商认证。共享模拟 Switch/Xbox/DS4 ID 不能反推出第三方品牌或产品代次。

profile 的输出编码器和连接提示短震动不代表 PS5 游戏反馈已接通。游戏震动回传尚未实现，USB XInput 马达输出仍未接入；本次配置变化不改变这些能力。主机测试与 CI 编译不能代替实体按键、震动和原装手柄共存测试。

内置复古 Nintendo Switch Online 设备继续统一引用 `switch`。Nintendo 模块根据 VID/PID 初始类型及设备信息应答区分 NES、SNES、N64、Genesis；缺失的摇杆居中，特殊键归一到 PS 输入。原有 `rumble: false` 能力限制继续生效，设备信息不自动开放该限制。具体映射、初始化和实机边界见[蓝牙文档](bluetooth.md#nintendo-classic-协议模块)。

BLE 是独立接入权限：内置设备字段 `ble` 缺省为 false。用户 `match.transport` 为 `ble` 的规则可显式设置 `ble: true/false`；仅设置别名或 profile 不开放 BLE。SDL 分类为 Switch 家族或本地选择 Switch 协议的身份，拒绝优先于配置；SDL 未收录的身份不维护额外 PID 黑名单，按普通白名单及覆盖规则处理。Switch 2 当前只识别广播型号并禁止配对，详见[蓝牙说明](bluetooth.md#switch-2-ble仅识别不支持配对)。
