# 贡献与验证约定

## 构建与测试位置

按项目约定，编译、自动测试、静态检查和打包在 GitHub Actions 上运行。本地用于编辑、阅读和 Git 操作，不要求安装 Android NDK 或运行本地构建测试。

推送、Pull Request 和手动触发均可运行 `.github/workflows/ci.yml`。不要用 `pull_request_target` 执行贡献者提供的代码，也不要向 PR 构建开放写权限或签名凭据。

## 核心构建接口

目前使用以下 CI 接口：

- 根目录提供 `CMakeLists.txt`。
- `BUILD_TESTING=ON` 在 Linux 上构建可独立运行的算法、存储及状态机测试，并注册到 CTest；设备接口使用可替换实现。
- `BUILD_TESTING=OFF` 支持 Android NDK `arm64-v8a`、`armeabi-v7a`、`x86_64`、`x86` 构建。
- Android 可执行文件目标命名为 `ios_brightness_daemon`，遵循 `CMAKE_RUNTIME_OUTPUT_DIRECTORY`。
- CI 固定 NDK `27.2.12479018`，使用 API 26 作为原生核心的编译下限，并启用灵活页大小支持；这不代表已支持所有 Android 8.0 及以上系统。Java 桥接使用 SDK 35 编译，安装基线为 Android 12，运行期按接口能力选择校准及降级路径。
- 模块打包等待核心测试、Android 构建、桥接测试及 Android 12/15 模拟器观察模式检查全部成功，不引入旧版闭源二进制。
- ADB 真机调试使用 Actions 产物，单独记录观察模式、主动控制和重启验证结果。

Actions 是自动验证的执行位置。只有对应提交的工作流真实成功后，才能记录为通过；区分工作流检查、核心测试、Android 编译和真机验证。

## 代码与数据

新写源码标注 `SPDX-License-Identifier: GPL-3.0-only`。不提交原闭源参考包、运行时学习数据、设备私有日志或签名材料。

持久化格式必须版本化；升级迁移保留有效数据，写入失败不能在 UI 中报告保存成功。设备控制变更必须有退出与异常恢复设计。

修改工作流、构建契约或学习行为时，同步更新文档及相应的有意义测试。

## Release 发布

1. 更新 `module/module.prop` 的版本与递增的 `versionCode`，在 `CHANGELOG.md` 添加同名 `## vX.Y.Z` 或 `## vX.Y.Z-beta.N` 节。README 只维护当前功能、使用说明和限制，不追加版本变更。
2. 提交并推送后，在该提交上创建并推送同名版本标签。`.github/workflows/release.yml` 自动复用完整 CI；全部成功后发布 ZIP 和 SHA-256。含版本后缀的标签发布为 Prerelease，不设为 Latest。
3. 工作流失败后可重跑，或使用 Release 手动入口指定同一个已存在的标签。附件先写入草稿，完整上传后才公开；失败时保留草稿便于恢复。
4. 工作流逐项校验标签提交、模块版本、更新日志、安装包内源码提交、模块元数据及 SHA-256；附件先上传到草稿，成功后再公开。只允许更新草稿，已公开 Release 不覆写，修正内容使用新版本。

Release 的编译和测试任务仅有读取权限，只有最终发布任务持有 `contents: write`。手动运行要求标签已存在，不自动创建或移动标签。首个预发布保留已有 beta 版本编号；是否正式发布不替代真机验收记录。
