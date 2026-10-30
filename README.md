# iOS Auto Brightness

[![CI](https://github.com/reF1nd/iOS-Auto-Brightness/actions/workflows/ci.yml/badge.svg)](https://github.com/reF1nd/iOS-Auto-Brightness/actions/workflows/ci.yml)

KernelSU、APatch、Magisk 自动亮度模块。调光流程按闭源 v6.8 守护进程逐条还原，补上它在 Android 12+ 上失效的设置读取；亮度曲线、亮度条学习和后置光感按 HyperOS 系统自己的规则计算。

从 [Releases](https://github.com/reF1nd/iOS-Auto-Brightness/releases) 下载模块；版本变更见 [更新日志](CHANGELOG.md)。项目名称描述调光体验目标，不表示使用或复现了 Apple 的原生算法。

## 调光方式

与 v6.8 相同，由一个原生守护进程按 800 毫秒周期运行：

- 前置光感读数经尖峰限幅、按变化幅度自适应的 EMA 平滑和 5 点趋势检测，再按系统自己的自动亮度曲线换算成背光值（HyperOS 的逻辑尼特曲线，参数取自本机系统配置）。
- 只在曲线目标越过回差（2.5%～3%）时写入主屏背光节点；不追写系统对背光的其他改动。快速变暗、阳光模式使用 v6.8 的短渐变。
- 夜间（18:00–6:00）降低 5%，电量低于 15% 且未充电时限制在 70%；照度超过 15000 lux 时进入阳光模式；电量低于 30% 且未充电，或电池/机身温度高于 45℃ 时不增强。
- 环境光稳定后逐步放慢到 1.6 秒、8 秒一次；熄屏 15 秒一次；接近传感器遮挡时保持亮度不动。
- 自动亮度模式下把背光节点设为 0444，系统切到手动亮度或模块停止时恢复 0644。

0444 无法阻止系统通过已打开的句柄写背光（见[设备说明](docs/DEVICE.md)），v6.8 实际上是与系统自动亮度交替写入：系统按自身节奏调整，模块只在自己的目标变化时覆盖一次。本模块保持这一行为，不与系统抢写。

### 与 v6.8 的差异

- **设置读取**：v6.8 按文本 XML 截取 `settings_system.xml` 的前 8 KB，Android 12 起该文件默认为 ABX 二进制格式，读取失败时它会一直当作自动模式、亮度条学习永不触发。本模块完整解析 ABX 与文本 XML。
- **亮度曲线**：v6.8 用自己的 lux → nits 公式和固定 52 点面板表。本模块改用系统的曲线：HyperOS 的 `RefactorNitController` 锚点给出亮度条位置（逻辑尼特），`getCurrentNit` 按环境光换算成实际尼特，屏幕亮度表换算到节点值。参数启动时从本机的 `framework-ext-res` 和 overlay 读取。没有这套策略的系统跟随 `BrightnessConfiguration` 默认曲线。
- **亮度条学习**：v6.8 把拖动按 10% 并入整条曲线的统一偏移，暗处几乎学不到。本模块按 HyperOS 的规则做 3 点学习：取系统记录的拖动（`mUserLux`/`mUserLogicalNit`），系统自动改写亮度条不计入；暗光、室内、户外三段各保留最近一次拖动，带动所在段两端的锚点并保持单调；拖得比系统建议更亮时，息屏较久后回落。重启后保留：装有 Zygisk 时，开机后把学习结果写回系统，系统也按学习后的曲线调光；之后系统把拖动、good curve 回落和息屏显示（AOD）状态实时报给模块（见[兼容性](docs/COMPATIBILITY.md)）。
- **后置光感**：v6.8 只在前置无读数时使用后置。本模块按 HyperOS `DualSensorPolicy` 的规则：后置的 1 秒、3 秒均值都超过前置的增亮阈值并通过防抖后改用后置照度，前置不低于后置时交还前置；后置只会让屏幕变亮，副屏点亮且前置很暗时忽略后置。可在设置中关闭，恢复 v6.8 行为。
- **启动修正**：v6.8 启动时若曲线目标恰好落在当前亮度的回差内，会把屏幕写到最低亮度再回升，本模块沿用当前亮度。

逐项对照与依据见 [docs/PARITY.md](docs/PARITY.md)。

## 兼容范围

Android 12（API 31）及以上，包含 ARM64、ARMv7、x86_64 和 x86 核心。优先控制 `panel0-backlight`，没有时使用第一个可用的背光设备；光感通过 NDK 读取，失败时回退 sysfs 或 `dumpsys sensorservice`。亮度曲线参数和屏幕亮度表按本机系统配置读取；后置光感使用小米的 `xiaomi.sensor.back_lux` 类型，阈值曲线取自小米 17 Pro Max 的系统配置。其他机型的情况见 [兼容性](docs/COMPATIBILITY.md)。

真机验证目前只覆盖小米 17 Pro Max / Android 17 HyperOS 4 / KernelSU，详见 [验证记录](docs/VALIDATION.md)。

## 安装与更新

1. 下载模块 ZIP，可用同名 `.sha256` 核对。开发构建可从成功的 Actions 运行下载 `kernelsu-module` artifact。
2. 用 Root 管理器安装，然后重启。首次安装和更新都在重启后生效，重启前旧版本照常运行。Zygisk 部分只在 system_server 启动时加载，所以不提供热更新。
3. 开启系统自动亮度。模块列表的简介前会显示运行状态（运行中、已停止、重启后生效等），WebUI 入口可查看详细状态、曲线与参数。

禁用模块后服务全部退出，重新启用后在 WebUI 点击“启动服务”恢复。安装 v6.8 后再安装本模块会按首次安装处理，并迁移其配置；v6.8 的学习偏移不再使用。

## 数据

数据位于 `/data/adb/ios_auto_brightness/`：`config.conf`（v6.8 的配置项，另有 `learning_enabled`、`dual_sensor`）、`learn.conf`（系统亮度曲线的锚点）、`service.log`。升级与卸载默认保留，WebUI 可清除学习记录。

## 构建与发布

编译、测试、静态检查和打包由 [GitHub Actions](https://github.com/reF1nd/iOS-Auto-Brightness/actions) 执行：核心单元测试、真实二进制在模拟 sysfs 上的端到端测试、ASan/UBSan、四种 ABI 构建、安装与启动脚本测试、system_server 锚点恢复测试和 WebUI 测试。Release 工作流复用完整 CI 并校验版本与源码提交。维护者操作见 [贡献约定](CONTRIBUTING.md)。

## 项目来源与许可证

本项目以闭源 v6.8 模块的行为分析为参考重新实现源码，参考 ZIP、二进制、网页资源和作者头像不包含在本仓库中。

代码和文档采用 **GNU GPL v3.0 only**（`GPL-3.0-only`），见 [LICENSE](LICENSE)。
