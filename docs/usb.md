# USB 有线手柄接入方案

USB手柄接入已实现，已有版本通过主机测试与Actions构建，最新提交的验证结果需单独核对，PS5实机待验证。蓝牙连接提示震动独立实现。USB 不通过蓝牙 host，不访问或重置内部 HCI。

## 支持范围

当前实现Switch Pro精确057e:2009及有线XInput接口FF/5D/01；XInput未知VID/PID需实际00/14头的20字节报告才能绑定虚拟输出，不能仅凭字符串识别。045e:028e及已登记Nova原生XInput身份允许先发LED激活；未知候选在首个有效输入前不发命令。通用 USB HID、Xbox One/Series GIP、无线接收器与更多第三方 ID 后续接入，不凭名称选择协议。

## 最小目录与数据流

src/drivers/usb.c、usb.h处理有线源启停与core路由；drivers/usb/下transport处理ugen、protocol处理非阻塞初始化和有线报文；共享Nintendo/HID解析在drivers/protocols，不增加通用插件框架。现有 Switch 解析函数可通过 pad_profile 接口复用，不复制整份蓝牙 host。XInput 的20字节输入新增独立解析，不能调用 xbox_parse 蓝牙解析器。

```text
USB手柄 -> ugen接口/端点 -> Switch USB握手 或 XInput解析
       -> pad_state -> core映射与会话 -> 现有PS5 backend -> 独立虚拟手柄
```

直接 /dev/hid 虚拟输出是另一项独立改造；实现 USB 输入不要求替换现有 libScePad/libSceMbus 后端，也不引入 SDL。

## 运行边界

- 默认关闭，选择独立已登录账号后手动启动；沿用明确工作状态持续5秒的门禁。USB与蓝牙可同时运行，总计最多四个独立输出；虚拟设备识别/注册串行推进。网页测试输入仍独占。
- 根据配置描述符选择匹配接口及 interrupt/bulk IN、OUT端点。
- 仅处理明确支持的外部手柄；排除原装Sony手柄、内部蓝牙、Hub、存储等设备。不reset设备或detach已有系统驱动；接口占用、claim失败则退出该设备并记录原因，不继续读写。
- Switch USB加入0x80/0x81握手、可选波特率、禁用USB超时，再复用Nintendo子命令和完整报告解析。ACK、步骤、重试、总初始化时间均受限，使用非阻塞状态机。
- XInput读取0x00/0x14头的20字节输入，转换有符号双摇杆、Y方向、扳机、按键；LED初始化仅用于已确认设备，不向未知USB设备发送命令。
- 初始化成功且首个有效输入后创建虚拟输出，拒绝旧设备实例的延迟数据；拔线、休眠、账号离线、停止立即释放并删除自有虚拟手柄，关闭并释放持有的接口。
- USB静默不等于断线。必须验证长按时仅状态变化上报的设备；将输入状态更新与连接健康/输出租约分开，不能直接把500ms无报文当成设备离线，也不能伪造收到的物理输入帧数。
- 日志覆盖发现、VID/PID、接口与端点选择、claim、握手步骤/ACK/超时、输入计数、源选择、账号绑定、断开和资源释放。不记录无限量原始载荷。

实机验收先原装手柄单独，再有线手柄独立账号与原装共存；覆盖游戏输入、PS键、长按、反复插拔、停止和待机。未完成这些验证不能声称 USB 已支持。

## 当前实现与限制

默认关闭；控制面提供USB启动/停止、设备节点、VID/PID、输入帧数与绑定状态。检测周期不超过100ms；只续期已提交且仍有明确传输健康证据的状态。拔线关闭接口并清理输出，已启用时可重新枚举；休眠/账号离线/主动停止后必须手动启动。

Switch握手等待ACK，单步250ms、最多3次、全程4.8秒，无sleep。已知XInput可发送LED激活，未知候选首帧前只读。连续3个无效输入报告拒绝该设备。USB接口只选择alt0、单interrupt IN/OUT对；不支持的布局暂时拒绝，不猜测端点。

FreeBSD原生USB_FS收发只开选中设备的两个传输槽；USB_IFACE_DRIVER_ACTIVE、CLAIM_INTERFACE等所有权检查在PS5是否完整实现仍需实机。缺失/占用即拒绝，不通过detach绕过。已接入Switch及Xbox360/XInput游戏震动回传，初始化与震动共用输出调度，反馈超过500ms停止。具体型号需实机验证。

## FreeBSD、Linux参考

ugen是FreeBSD的原始USB访问接口，不是统一手柄驱动；Linux对应usbfs(/dev/bus/usb)，读取事件还可通过hidraw/evdev。USB设备描述符和协议一致，节点、ioctl、异步传输与接口占用API不同。PS5的FreeBSD衍生实现不能等同当前FreeBSD。

FreeBSD已有[通用HID游戏手柄hgame](https://github.com/freebsd/freebsd-src/blob/main/sys/dev/hid/hgame.c)与[Xbox360 xb360gp](https://github.com/freebsd/freebsd-src/blob/main/sys/dev/hid/xb360gp.c)。xb360gp用合成HID描述符适配没有标准HID描述符的Xbox360协议，可参考其识别、按键与坐标映射；不能把FreeBSD内核模块直接载入PS5。参考[ugen手册源码](https://github.com/freebsd/freebsd-src/blob/main/share/man/man4/ugen.4)、[Linux xpad](https://github.com/torvalds/linux/blob/master/drivers/input/joystick/xpad.c)。本轮未复制FreeBSD驱动实现。

Nova系列型号、接收器输入静默保护及不扩展USB DInput的范围见[型号说明](gamesir-nova.md)。
