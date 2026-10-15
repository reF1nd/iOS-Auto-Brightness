# iOS Auto Brightness

[![CI](https://github.com/reF1nd/iOS-Auto-Brightness/actions/workflows/ci.yml/badge.svg)](https://github.com/reF1nd/iOS-Auto-Brightness/actions/workflows/ci.yml)

面向 KernelSU 的 Android 自动亮度模块重实现项目，重点是平滑调光、可靠的系统控制权交接，以及可跨重启保留的环境照度与用户亮度偏好。

**当前为 v7.0.0-beta.2：核心、Android 桥接、KernelSU 服务和 WebUI 已实现，Actions 生成可安装 ZIP，正在进行目标设备验收。** 自动测试通过不等于全部真机场景通过。项目名称描述调光体验目标，不表示使用或复现了 Apple 的原生算法。

## 首要适配目标

- 小米 17 Pro Max
- 基于 Android 17 的 HyperOS 4
- KernelSU

启动时核对主屏尺寸和系统标定表；不匹配则停止接管。只控制主屏，具体接口及已知限制见 [设备适配](docs/DEVICE.md)。

## 功能

- 光线与距离传感器辅助调光，平滑、回差和亮暗变化响应。
- 自动模式下学习不同环境照度对应的用户亮度偏好。
- 学习数据可靠保存，服务重启、整机重启和模块升级后恢复。
- 手动模式及时交还系统，避免与系统自动调光争用。
- 温度、电量和设备亮度上限保护，异常时明确降级。
- 配置热加载、日志诊断和 KernelSU WebUI。

设计与验收顺序见 [重构计划](docs/PLAN.md)。

## 构建与测试

构建、自动测试及工作流检查统一由 [GitHub Actions](https://github.com/reF1nd/iOS-Auto-Brightness/actions) 执行。详见 [贡献约定](CONTRIBUTING.md)。

CI 执行 C++ 算法与真实进程持久化测试、ASan/UBSan 检查、Java 事件策略测试、Android ARM64/DEX 编译、ShellCheck、WebUI 语法检查及打包验证。所有任务通过后上传 `kernelsu-module` artifact，包含模块 ZIP 和 SHA-256。

CI 无法替代真机亮度、传感器、温控和耗电验收；具体记录见 [验证记录](docs/VALIDATION.md)。

## 安装与学习

1. 从成功的 Actions 运行下载 `kernelsu-module`，解开 artifact 后在 KernelSU 中安装内部的模块 ZIP。
2. 重启以替换旧服务，开启系统自动亮度。
3. 环境光稳定时调节系统亮度条，停留几秒，在 WebUI 中确认调节事件为 `saved`。学习按当前用户序号和面板配置隔离，重启后从独立数据目录读取。
4. WebUI 支持查看样本、导入导出、清除学习、参数调整和日志。保存配置须收到核心确认。

数据位于 `/data/adb/ios_auto_brightness/`，升级和卸载默认保留。参考版的单个 `user_offset` 仅作为兼容偏移导入，不生成虚构的历史学习点。v6 的配置不直接套用，新配置使用经过校验的默认值。

调光通过系统临时亮度接口完成，不修改 sysfs 权限或持久系统属性；关闭自动亮度时释放控制。温度不可用时禁止强光增强，始终服从系统亮度上限。

## 项目来源与许可证

本项目以已有闭源 v6.8 模块的功能需求和行为分析为参考，重新实现可维护的源码。参考模块的 ZIP、二进制、网页资源和作者头像不包含在本仓库中；不将原作者作品重新声明为本项目的 GPL 代码。

本仓库新编写的代码和文档采用 **GNU GPL v3.0 only**（`GPL-3.0-only`），见 [LICENSE](LICENSE)。后续引入第三方组件时保留其原有许可证及归属说明。
