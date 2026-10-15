# 兼容性与降级规则

## 支持范围与实测范围

安装基线为 Android 12 / API 31；不设厂商、ROM、型号或分辨率白名单。Actions 构建 arm64-v8a、armeabi-v7a、x86_64、x86 四种核心。标准 KernelSU、APatch、Magisk 模块安装/服务机制均有入口，监督进程寻找对应管理器 BusyBox。非标准发行版缺失 BusyBox 时给出错误，不进入无锁重复运行。

现有真机证据只来自小米 17 Pro Max / Android 17 HyperOS 4 / KernelSU。多 ABI 编译和模拟数据测试不等于其他设备或 Root 管理器实测。Android 11 及更早版本仍不支持；未采用绕过系统限亮的直接 sysfs 写入。

## 能力选择

- 必需：默认屏幕、临时亮度设置、亮度上下限、当前亮度、当前用户和系统设置读取。失败时不接管屏幕，独立诊断和配置修复仍可用。
- 系统校准：只读取当前物理屏幕对应的 `mNits` 与 `mBrightness`，不把 HAL `mBacklight` 当成系统亮度。校验长度、有限数值、范围和严格递增。不同面板、不同标定数据使用独立的学习身份；原小米映射完整匹配时保留旧身份和学习记录。
- 无校准：使用显式标记的通用比例曲线，仍遵守系统上下限。亮度事件的 nits 无法可靠换算，学习功能禁用并解释原因；不会把其他机型标定套上来。通用曲线需要用户实测，不保证主观亮度与已校准设备相同。
- 学习：需要系统真实调节事件和校准映射。事件缺失或字段不兼容会报告错误，不猜测用户拖动，不保存虚构样本。
- 窗口亮度：优先框架标记；没有厂商扩展标记时异步读取 PowerManager 的主屏窗口覆盖状态。无法识别或状态过期时暂停接管；轮询路径存在检测延迟，需实测前台应用自设亮度场景。
- 光感：默认前置 → 独立后置 → 唯一可读 sysfs/IIO 输入 → 带注册周期和新鲜度验证的 dumpsys 读数。没有默认 Sensor 对象时可发现唯一前置名称；多个候选不猜测。
- 温度：识别 battery/batt、skin、shell、quiet、back 表面传感器；SoC/CPU/GPU 不使用表面 45°C 阈值。系统严重温控状态也会禁止阳光增强。温度不可用不增强，亮度始终服从系统上限。
- 当前逻辑屏幕更换物理面板时释放控制、重启校准。尚无折叠屏真机验收；不控制外接屏和副屏。

## 管理和排障

WebUI 使用 `ksu.exec` 宿主协议；KernelSU/APatch 的兼容 WebUI 可直接提供，Magisk 需另行安装兼容的 WebUI 宿主。服务和命令行不依赖 WebUI：

```sh
sh /data/adb/modules/ios_auto_brightness/check_status.sh
sh /data/adb/modules/ios_auto_brightness/control.sh config-read
# config-save 接受经过 base64 编码的 UTF-8 配置；由原生解析器验证后原子替换。
```

离线配置保存不需要桥接、传感器或面板校准。不会自动启动已停止的服务；下次启动或现有服务恢复后加载。失败诊断独立检查安装文件、停用标志、监督 PID/命令行、配置、状态时间、系统亮度模式、电量及背光节点权限；缓存状态明确标为可能过期。

曲线预览是稳态示意，不模拟时间渐变、光感冲突保护、温控和所有系统限制。离线预览明确标记示例，不冒充当前屏幕实际输出。阳光下限仍按当前系统上限计算，不强制开启 HBM。系统滑块与模块实际输出同步仍为单独的未实现项目。

## 框架依据

[Android 12 DisplayManager](https://android.googlesource.com/platform/frameworks/base/+/refs/heads/android12-release/core/java/android/hardware/display/DisplayManager.java) 提供按 displayId 设置临时亮度；[DisplayDeviceConfig](https://android.googlesource.com/platform/frameworks/base/+/master/services/core/java/com/android/server/display/DisplayDeviceConfig.java) 区分 nits、HAL backlight 与系统 brightness 映射。这里对隐藏接口均以实际运行能力为准，不假定 ROM 完整遵循 AOSP。
