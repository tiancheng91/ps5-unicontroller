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

不引入 SDL 映射数据库。当前通用 HID 映射依据 HID Usage，不能直接使用 SDL 的平台相关按钮/轴编号。保留已有型号差异，后续仅为实际需要的兼容性问题增加例外；用户的 Switch / PS·Xbox 面键布局切换（A/B 与 X/Y）和全局按键设置仍独立生效。

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

顶层只包含 `schema_version: 1` 与 `devices` 列表。每条规则的 `match` 必须给出 `transport`、`vid`、`pid`。transport 只能为 `classic`、`ble`、`usb`；VID/PID 为引号包围的四位十六进制字符串。匹配按传输与 VID/PID 精确限定，没有通配符，不把同一 ID 的 USB 与蓝牙规则混用。

规则可选字段：

| 字段      | 含义                                               |
| --------- | -------------------------------------------------- |
| `name`    | 1..63 个可打印 ASCII 字符的显示别名，不是型号认证  |
| `profile` | 选择已有协议，不能定义新解码器                     |
| `mapping` | 原始通用 HID 的按键/轴字段映射，不是解码后全局映射 |

profile 白名单为 `switch`、`joycon-left`、`joycon-right`、`ds4`、`ds5`、`xbox`、`generic-hid`；USB 专用入口另有 `switch-usb`、`xinput-usb`。协议仍受传输及现有驱动能力限制，不能把任意 profile 跨传输使用。`generic-hid` 表示请求 HID 描述符解析，不意味着兼容任意报告格式。

USB profile hint 只能选择已经实现且接口描述符满足要求的入口，不能凭改名把 DInput 转成 XInput。配置不能绕过 Sony/内部设备保护、系统驱动占用、接口所有权、报告长度、初始化时限、超时释放或休眠清理；不会 detach Sony 系统驱动。

## 原始 HID 映射与用户设置

`mapping` 仅作用于通用 HID 解码路径。来源键为 `button1`..`button32` 或轴角色 `lx/ly/rx/ry/l2/r2`；轴值为 `x/y/z/rx/ry/rz/slider/dial/brake/accelerator/accel/none`。轴角色说明报告字段如何组成标准状态，不是会话用户的方向偏好。

按键目标为 `cross/circle/square/triangle`、`l1/r1/l2/r2/l3/r3`、`create/share/options/ps/touchpad`、`up/down/left/right/none`。映射不能重写 Switch/Xbox 等专用协议的字节格式。JSON 不接受方向反转选项、脚本、函数名、原始 USB/HCI 命令或任意文件路径。

`settings.json` 继续保存网页/core 偏好，例如日志级别、允许震动、Switch / PS·Xbox 面键布局切换（A/B 与 X/Y）和全局 32 位按键映射。Switch / PS·Xbox 面键布局切换（A/B 与 X/Y）对应标准 PS5 Cross14↔Circle13、Square15↔Triangle12，再执行一次全局按键映射。当前没有新增每会话摇杆轴反转设置；不要把协议层字段绑定当作已实现的会话偏好。

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

`usb.xinput_activation` 只在既有 FF/5D/01 接口验证通过后使用已有 LED 激活命令。`usb.input_watchdog` 用于需要实际输入续期的接收器身份，不允许仅因 USB 节点在线而保留旧按键。

```sh
python3 tools/embed_controllers.py
python3 -m unittest discover -s tests/config -v
make test
```

## 身份与反馈能力

显示别名、上报 VID/PID 和 profile hint 都不是制造商认证。共享模拟 Switch/Xbox/DS4 ID 不能反推出第三方品牌或产品代次。`identity_kind` 仅为内置元数据区分 reported-model 与 protocol，不提升匹配权限。

profile 的输出编码器和连接提示短震动不代表 PS5 游戏反馈已接通。游戏震动回传尚未实现，USB XInput 马达输出仍未接入；本次配置变化不改变这些能力。主机测试与 CI 编译不能代替实体按键、震动和原装手柄共存测试。

内置复古 Nintendo Switch Online 设备统一引用 `switch`，通过设备字段 `rumble: false` 保留原来的输出禁用行为。生成器复用相同解析与初始化函数，生成只读能力视图；该字段不表示已完成这些设备的特殊按键适配，也不会改变协议初始化步骤。

BLE 是独立接入权限：内置设备字段 `ble` 缺省为 false。用户 `match.transport` 为 `ble` 的规则可显式设置 `ble: true/false`；仅设置别名或 profile 不开放 BLE。Switch BLE 拒绝优先于配置。
