# iOS Auto Brightness

[![CI](https://github.com/reF1nd/iOS-Auto-Brightness/actions/workflows/ci.yml/badge.svg)](https://github.com/reF1nd/iOS-Auto-Brightness/actions/workflows/ci.yml)

面向 KernelSU 的 Android 自动亮度模块重实现项目，重点是平滑调光、可靠的系统控制权交接，以及可跨重启保留的环境照度与用户亮度偏好。

**当前处于仓库初始化和设计阶段，尚无可安装的新模块，也未完成设备验证。** CI 的初始化检查通过不代表亮度功能已实现或已验证。项目名称描述调光体验目标，不表示使用或复现了 Apple 的原生算法。

## 首要适配目标

- 小米 17 Pro Max
- 基于 Android 17 的 HyperOS 4
- KernelSU

这是计划中的目标设备组合；具体背光、传感器、系统滑块行为及权限需要设备侧验证。

## 实现目标

- 光线与距离传感器辅助调光，平滑、回差和亮暗变化响应。
- 自动模式下学习不同环境照度对应的用户亮度偏好。
- 学习数据可靠保存，服务重启、整机重启和模块升级后恢复。
- 手动模式及时交还系统，避免与系统自动调光争用。
- 温度、电量和设备亮度上限保护，异常时明确降级。
- 配置热加载、日志诊断和 KernelSU WebUI。

设计与验收顺序见 [重构计划](docs/PLAN.md)。

## 构建与测试

构建、自动测试及工作流检查统一由 [GitHub Actions](https://github.com/reF1nd/iOS-Auto-Brightness/actions) 执行。详见 [贡献约定](CONTRIBUTING.md)。

初始化阶段仅执行仓库及工作流检查；根目录加入 `CMakeLists.txt` 后，CI 自动启用 Linux 核心测试及 Android ARM64 交叉编译。测试任务要求 CTest 中实际存在测试，没有测试会失败。Android 构建产物在通过核心测试后上传为 Actions artifact。

当前不生成模块 ZIP，也不创建 Release；待核心、安装脚本及打包检查实现后接入。CI 无法替代真机亮度、传感器、温控和耗电验收。

## 项目来源与许可证

本项目以已有闭源 v6.8 模块的功能需求和行为分析为参考，重新实现可维护的源码。参考模块的 ZIP、二进制、网页资源和作者头像不包含在本仓库中；不将原作者作品重新声明为本项目的 GPL 代码。

本仓库新编写的代码和文档采用 **GNU GPL v3.0 only**（`GPL-3.0-only`），见 [LICENSE](LICENSE)。后续引入第三方组件时保留其原有许可证及归属说明。
