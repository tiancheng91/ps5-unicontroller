# UniController for PS5

Universal Controller Support for PS5

**Make Any Controller Work on PS5.**

Repository: `ps5-unicontroller` · Payload: `unicontroller.elf`

C11 + HTML/CSS/JavaScript。当前包含手机模拟手柄与默认关闭的蓝牙输入接入；蓝牙实体设备兼容性待实机验证，USB有线Switch Pro/XInput也已接入，均待实机验证。口号为愿景，不表示所有手柄已经支持。

```text
src/core/     输入会话、映射、分级日志、超时和清理
src/ps5/      PS5 虚拟手柄、账号、电源、系统接口
src/web/      HTTP、路由、配置文件、JSON
src/drivers/  输入源适配、蓝牙/USB传输、共享协议与配对存储
web/          控制面、手机手柄、设置页面
tests/       核心、平台和 HTTP 测试
tools/       离线资源打包
docs/        设计、计划、验证说明
```

```sh
make test-core
make host
./build/unicontroller-host 7456 /tmp/unicontroller-settings.json
make test-web
PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk make ps5
```

宿主机使用模拟后端；PS5 构建连接真实虚拟手柄 API。两者的运行结果不能互相替代。

[设计](docs/architecture.md) · [实机验证](docs/validation.md) · [视觉规范](design.md) · [蓝牙接入](docs/bluetooth.md)

默认监听 7456；绑定失败时仅再尝试 7457。两者均失败则启动失败，日志记录失败原因及实际监听端口。页面使用相对 URL，备用端口同样可用。

## 页面格式

HTML、独立 CSS 和 JavaScript 使用两空格缩进，由根目录 `.prettierrc.json` 统一。格式化命令：

```sh
npx --yes prettier@3.6.2 --write "web/**/*.{html,css,js}"
```

页面源码保持可读格式，离线嵌入资源由 `make assets` 生成，不手工修改 build/assets.c。

公共设计变量维护在 web/shared/tokens.css，公共组件在 components.css。各页布局和交互在同目录独立 CSS/JS 文件；由离线资源表统一嵌入 ELF，无 CDN 依赖。

## 静态页面预览

```sh
make preview
# 默认 http://127.0.0.1:7456/
make preview PREVIEW_PORT=7457
# 手机预览：访问电脑的局域网 IP
make preview PREVIEW_BIND=0.0.0.0
```

底层使用 `python3 -m http.server`，Ctrl+C 停止。控制面 `/`、手柄 `/controller/`、设置 `/settings/` 与共享 CSS/JS 均可访问。预览通过生成目录的符号链接读取源码，修改页面后刷新即可。

这是静态预览，不提供手柄或设置 API；页面出现 API 连接错误是预期行为。需要验证交互链路时使用 `make host` 启动模拟应用服务。

## 手机触控手柄

`/controller` 使用 SVG 手柄外形和真实按钮，支持多点触控、双摇杆、PS/Touchpad、L3/R3。先选择账号并启用；未启用时控制区域禁用。按钮获得焦点后，可用空格或 Enter 按住/松开。

横屏可开启“专注操控”隐藏说明区域；点击“退出专注”或 Escape 返回。切换视图会先释放输入。紧急释放随时可用；切后台或断网仍保留服务端超时释放机制。

## 蓝牙手柄

控制面选择独立已登录账号，手动启动蓝牙，再开启限时配对。Switch Pro使用Classic专用协议；已知Xbox蓝牙型号使用对应协议，未知手柄根据HID描述符解析。非Switch不能统一视为XInput。最多四个实体输入源分别绑定独立账号，手机测试手柄仍需单独使用。休眠后默认关闭，不自动重开蓝牙。

当前尚未完成PS5实体设备验证；游戏震动回传已接入，需实机验证。详细范围、配对记录与验证步骤见[蓝牙接入](docs/bluetooth.md)。

项目许可证及第三方来源声明见 [LICENSE](LICENSE) 与 [NOTICE](NOTICE)。

## USB有线输入

控制面选择独立已登录账号，手动启动USB，接入Switch Pro或有线Xbox360/XInput设备。蓝牙与USB可同时运行，总计最多四个实体输出；网页测试手柄保持独占；休眠后不会自动恢复。协议、接口占用限制及FreeBSD/Linux差异见[USB说明](docs/usb.md)。源码分层见[架构](docs/architecture.md)。

## 手柄设备配置

内置型号、协议引用、共享 HID 字段布局与 USB 特殊标记维护在源码 [src/drivers/protocols/controllers.json](src/drivers/protocols/controllers.json)，构建时生成 C 表；终端用户无需修改此文件。

可选运行时覆盖位于 `/data/unicontroller/controllers.json`：通过 FTP 手动上传，重启 payload 后生效。固定版本 LibJSON 0.2.5 事件解析器限制文件 32 KiB、32 条规则，启动仅加载一次，不热重载；缺文件正常使用内置规则，整份文件无效时回退内置并记录日志。没有网页上传或写入 JSON 的 API。

规则按 exact transport（classic/ble/usb）与 VID/PID 匹配，可设置显示别名、选择既有 profile，或配置原始通用 HID 按键/轴字段。USB hint 不绕过接口描述符、Sony 或系统驱动保护，别名不认证真实型号；新协议仍需 C 实现，不支持脚本、原始命令或组合键。

settings.json 继续保存 UI/core 偏好，包括 A/B 翻转和全局 32 位按键映射；当前没有新增每会话轴方向反转。Touchpad 映射仅是点击位；Switch Capture 已有默认映射，背键必须独立上报才能单独映射。游戏震动受全局震动开关控制，依赖具体协议的输出支持。

示例见 [examples/controllers.json](examples/controllers.json)，详细字段与边界见[配置说明](docs/controller-config.md)。
