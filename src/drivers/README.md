# 设备输入边界

bt.c、usb.c是输入源到core的适配；core持有虚拟会话、映射、租约和清理策略。

- bluetooth/：Classic/LE配对、密钥存储、HCI与链路收发、L2CAP/GATT解包。
- usb/：ugen设备枚举、接口/端点所有权、异步收发、Switch USB初始化及有线XInput适配。
- protocols/：传输无关的HID报告解析与输出编码；统一状态在core/pad.h。
- common/：驱动共用的日志、字节序、时钟与CRC工具。

协议解析不创建系统手柄、不绑定账号、不依赖HTTP。只有明确手动启动才访问实体设备；实际设备与PS5共存待实机验证。迁移源码遵循根目录LICENSE、NOTICE。
