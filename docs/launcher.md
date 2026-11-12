# PS5 应用图标

UniController 在 HTTP 成功监听后只做一次只读安装检查，使用独立 title ID `UNIC00001`，显示名称为 UniController。文件仅写入 `/user/app/UNIC00001/sce_sys/`：`param.json` 与 512×512 的 `icon0.png`。采用 Web 快捷方式类别 `65536`，可能显示在系统的媒体应用分类，具体位置需要实机确认。

已安装且资源一致时，不写文件、不调用安装器。首次安装、图标/名称/实际端口变化或上次失败时，才在本次加载中排队；系统工作、有用户登录且所有输入源关闭连续5秒后尝试一次。状态不满足时重置等待时间，失败不循环重试。休眠或运行间隔异常会取消尚未执行的任务，恢复不重新排队；可手动重试或下次加载时再检查。

入口指向 `http://127.0.0.1:7456/`；如果本次服务使用备用端口7457，会更新为对应地址。图标本身不加载 payload，也不启动蓝牙或虚拟手柄；使用前需通过 Payload Manager 等方式运行 `unicontroller.elf`。停止 payload 后图标仍保留，但配置页不可访问。

注册沿用旧实现的安装器路径：初始化 AppInstUtil，通过 NID `Wudg3Xe3heE` 解析并优先调用 TitleDir；不可用时调用 AppInstallAll。后者会请求系统扫描应用目录，范围不局限于本应用，因此仅在首次安装、资源变化、失败后重试或手动更新时调用。没有 ShellCore patch、游戏注册 hook 或空闲代码区占用。

资源通过临时文件和 rename 替换，目录755、文件644，避免 payload 私有 umask 导致系统读取不到图标。更新前清除成功标记，只有安装返回成功或旧实现认可的已存在状态才保存标记。失败记录到运行日志，HTTP仍继续提供服务，不自动反复重试。

设置页提供安装/更新和移除按钮，需要先停止所有输入源，避免安装器同步调用影响输入调度。移除只针对本 title ID；系统卸载失败时保留文件。成功移除会记录 `launcher.disabled`，后续加载不自动重装，可在设置页重新安装。

API：`GET /api/v1/launcher` 查询本进程状态及端口；`POST` 强制重试安装；`DELETE` 移除。写请求沿用HTTP会话令牌及Origin校验。主机模拟后端返回 unsupported。成功标记不是系统数据库查询结果，若外部删除入口可手动重装。

## Logo 与主题

图形保留用户提供SVG的手柄外轮廓、十字键、两个圆按钮及顶部短线，统一为填充轮廓，没有文字、阴影或渐变。默认橙底 `#FF8C00`、炭黑图形 `#0D0E11`。

- `assets/branding/icon.template.svg`：几何源文件，背景与前景为两个独立配色参数。
- `assets/branding/icon.svg`：当前配色的可编辑矢量版本。
- `assets/branding/icon0.png`：PS5实际安装资源，提交到仓库；CI无需图形渲染器。
- `tools/build_icon.py`：修改配色并用 `rsvg-convert`（librsvg）导出512px PNG。也支持 `--svg-only`，之后用其他SVG渲染工具导出同尺寸PNG。

```sh
python3 tools/build_icon.py --background '#FF8C00' --foreground '#0D0E11'
```

换色后同时提交 SVG 和 PNG，重新构建并加载 payload 即可更新安装资源。PS5桌面图标使用静态PNG，不随网页CSS主题实时变化。设计使用用户指定的 [logo-design 技能](https://github.com/kaankiziltug/logo-design-skill)，已检查512px及32px渲染。

## 实机验证

首次加载等待系统稳定5秒后确认显示图标；点击后打开本机控制面。测试7456占用时图标是否更新至7457，及重新加载后的重复注册行为。设置页移除后确认图标消失、重载不复现、手动重装可恢复。新实现尚需PS5实机验证；主机测试只验证文件权限、端口内容、注册回退与错误处理。

重复加载保护独立于端口选择：PS5入口先持有 `/data/unicontroller/instance.lock` 的非阻塞独占锁，失败直接退出。锁保持到退出清理结束，端口降级不会启动第二实例。
