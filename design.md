---
name: ESP-EMU DevStudio
description: Dense, terminal-adjacent dark system designed for in-browser hardware emulators, embedded registers, and low-level firmware telemetry.
colors:
  primary: "#ff8c00"
  on-primary: "#000000"
  primary-container: "#3d2200"
  secondary: "#00e5ff"
  on-secondary: "#000000"
  neutral: "#0d0e11"
  surface: "#16181d"
  surface-variant: "#1f232b"
  outline: "#2c313a"
  outline-variant: "#3b424e"
  text-primary: "#e6edf3"
  text-secondary: "#8b949e"
  text-muted: "#545d68"
  status-success: "#2ea043"
  status-error: "#f85149"
  status-warning: "#d29922"
typography:
  fontFamily:
    sans: "Inter, -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif"
    mono: "'JetBrains Mono', 'Fira Code', 'SF Mono', Consolas, monospace"
  heading-sm:
    fontFamily: "{typography.fontFamily.sans}"
    fontSize: "0.75rem"
    fontWeight: "700"
    letterSpacing: "0.08em"
    textTransform: "uppercase"
  heading-md:
    fontFamily: "{typography.fontFamily.sans}"
    fontSize: "0.875rem"
    fontWeight: "600"
    lineHeight: "1.25"
  body-sm:
    fontFamily: "{typography.fontFamily.sans}"
    fontSize: "0.75rem"
    lineHeight: "1.3"
  body-md:
    fontFamily: "{typography.fontFamily.sans}"
    fontSize: "0.8125rem"
    lineHeight: "1.4"
  code-xs:
    fontFamily: "{typography.fontFamily.mono}"
    fontSize: "0.6875rem"
    lineHeight: "1.2"
  code-sm:
    fontFamily: "{typography.fontFamily.mono}"
    fontSize: "0.75rem"
    lineHeight: "1.35"
spacing:
  2xs: "2px"
  xs: "4px"
  sm: "8px"
  md: "12px"
  lg: "16px"
  xl: "24px"
rounded:
  none: "0px"
  xs: "2px"
  sm: "4px"
  md: "6px"
components:
  button-primary:
    backgroundColor: "{colors.primary}"
    textColor: "{colors.on-primary}"
    typography: "{typography.heading-sm}"
    rounded: "{rounded.xs}"
    padding: "4px 10px"
  button-primary-hover:
    backgroundColor: "#ffa333"
  button-control:
    backgroundColor: "{colors.surface-variant}"
    textColor: "{colors.text-primary}"
    typography: "{typography.body-sm}"
    rounded: "{rounded.xs}"
    padding: "4px 8px"
  card-panel:
    backgroundColor: "{colors.surface}"
    textColor: "{colors.text-primary}"
    rounded: "{rounded.xs}"
    padding: "10px"
  input-text:
    backgroundColor: "{colors.neutral}"
    textColor: "{colors.text-primary}"
    typography: "{typography.code-sm}"
    rounded: "{rounded.xs}"
    padding: "4px 8px"
  terminal-window:
    backgroundColor: "#08090a"
    textColor: "{colors.text-primary}"
    typography: "{typography.code-sm}"
    rounded: "{rounded.none}"
    padding: "10px"
---

# ESP-EMU DevStudio — UniController 应用规范

以上设计 token 按用户提供规范保存。视觉采用工业硬件工作台：密集、深色、高对比、严格分区。无浮动阴影和背景模糊，使用 1px 描边与 0–2px 圆角。

橙色标识主要启动操作，青色用于连接和遥测提示；成功、错误、警告分别使用既定语义色，同时提供文字，不能仅依赖颜色。

Inter 用于标题与操作；JetBrains Mono / Fira Code 用于设备 ID、数据表、十六进制和日志。英文分区标题大写，中文不强行转换。字体离线打包或使用系统回退。

## 控制面

桌面三列：左侧设备发现、配对与配置；中间连接会话、操作与持续日志；右侧按键、摇杆、账号、虚拟输出和反馈状态。状态矩阵使用等宽数字，设备状态与错误必须可复制。移动端折叠为单列，日志限高。

## 手机测试手柄

独立 /controller 页面，与控制面共享色彩与组件语言，但操作区不使用微小按钮。触控目标至少 44px，双摇杆、方向键、面键、肩键和 PS/Options 分区。显示启用状态、连接状态、控制租约及紧急释放按钮。默认关闭；隐藏页面、断线、触摸取消自动释放。

## 日志面板

深色终端底色，橙/青前缀，等宽行；支持级别筛选、暂停滚动、复制和导出。日志按文本安全渲染；如支持 ANSI，仅允许经过解析的样式，不解释控制指令或 HTML。

本轮仅提供规范，不实现页面或生成效果图。
