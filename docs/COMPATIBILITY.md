# 兼容性与降级规则

## 支持范围与实测范围

安装基线为 Android 12 / API 31；不设厂商、ROM、型号或分辨率白名单。Actions 构建 arm64-v8a、armeabi-v7a、x86_64、x86 四种核心。标准 KernelSU、APatch、Magisk 模块安装/服务机制均有入口，监督进程寻找对应管理器 BusyBox。非标准发行版缺失 BusyBox 时给出错误，不进入无锁重复运行。

现有真机证据只来自小米 17 Pro Max / Android 17 HyperOS 4 / KernelSU。多 ABI 编译和模拟数据测试不等于其他设备或 Root 管理器实测。Android 11 及更早版本仍不支持；未采用绕过系统限亮的直接 sysfs 写入。

## 能力选择

- 必需：默认屏幕、临时亮度设置、亮度上下限、当前亮度、当前用户和系统设置读取。失败时不接管屏幕，独立诊断和配置修复仍可用。
- 系统校准：只读取当前物理屏幕对应的 `mNits` 与 `mBrightness`，不把 HAL `mBacklight` 当成系统亮度。校验长度、有限数值、范围和严格递增。不同面板、不同标定数据使用独立的学习身份；原小米映射完整匹配时保留旧身份和学习记录。
- 无校准：使用显式标记的通用比例曲线，仍遵守系统上下限。亮度事件的 nits 无法可靠换算，自动事件学习禁用并解释原因，仍可显式开启一次调节学习；不会把其他机型标定套上来。通用曲线需要用户实测，不保证主观亮度与已校准设备相同。
- 自动学习：需要系统真实调节事件和校准映射。事件缺失或字段不兼容会报告错误；显式学习回退见下方 beta.8 说明。
- 窗口亮度：优先框架标记；没有厂商扩展标记时异步读取 PowerManager 的主屏窗口覆盖状态。无法识别或状态过期时暂停接管；轮询路径存在检测延迟，需实测前台应用自设亮度场景。
- 光感：默认前置 → 独立后置 → 唯一可读 sysfs/IIO 输入 → 带注册周期和新鲜度验证的 dumpsys 读数。没有默认 Sensor 对象时可发现唯一前置名称；多个候选不猜测。
- 温度：识别 battery/batt、skin、shell、quiet、back 表面传感器；SoC/CPU/GPU 不使用表面 45°C 阈值。系统热状态从 LIGHT 开始即释放控制并暂停学习。温度不可用不增强，亮度始终服从系统上限。
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

## beta.8：系统保护与兼容回退

系统保护优先于调光、阳光增强和学习。读取 `BrightnessInfo.brightnessMaxReason`，非零原因（包括未知厂商扩展）一律释放临时亮度；`PowerManager` 热状态从 LIGHT 开始同样释放，读取失败也不继续控制。监听热状态变化并轮询复核，两个信号持续正常 10 秒后才能恢复。保持使用系统临时亮度接口及实时上下限，不写背光、温控节点，不改温控配置或限亮原因。可见信号之外的厂商底层保护仍由系统执行。

稳定目标与实际输出不同不会直接触发回拉。仅当主屏当前 dump 明确显示 `automatic`、临时亮度为 NaN、限亮原因为 0，且下一帧保护、应用覆盖、HDR、用户操作等检查都通过，才重新提交丢失的临时请求。无法确认的 OEM 格式不自动重试。

没有事件接口或 nits 校准时，WebUI 的“学习下一次手动调节”提供 30 秒显式窗口：用户随后操作亮度条，适配层等待实际归一化亮度稳定，再调用核心学习。不会把未经确认的 Settings 变化当作偏好，也不会用固定 255/Gamma 换算。无光感、遮挡、光线不稳定、熄屏、切用户或系统保护均取消学习。此回退需要系统发出带 URI 的亮度设置通知。

前置、后置和 sysfs/dumpsys 来源均可学习，按面板、用户和来源隔离，默认前置继续使用原文件。切换来源不覆盖旧记录。WebUI 光感选择写入私有 `sensors.conf`，支持 `front=完整名称`、`back=完整名称`（`none` 禁用）、`sysfs=/sys/.../lux` 或 `in_illuminance_input`；留空自动发现。没有显式前置选择时兼容 `persist.sys.display.als_name`，匹配失败明确报告并回退系统默认传感器。指定 sysfs 时优先使用该照度节点，失败不会静默切换另一来源。

离线 `control.sh log-read` 按时间拼接轮转备份和当前日志的最近 80 行；`log-clear` 清除当前日志及备份，与轮转互斥。健康诊断检查进程身份、启动时间、同次开机和心跳新鲜度；CPU 分开报告 Java 桥接与 native 核心的进程平均占用（不含 supervisor 和短命 dumpsys 子进程）。

Android 12/12L 的 AOSP `BrightnessInfo` 尚无 `brightnessMaxReason`，该已知能力缺失不会冒充字段读取成功：状态显示 `thermal_and_system_cap_only`，使用系统热状态及实时亮度上下限保护，禁用丢失输出的自动重试。Android 13+ 应有该字段而缺失、或热状态查询失败时，暂停接管。接口依据：[Android 12L](https://raw.githubusercontent.com/aosp-mirror/platform_frameworks_base/android-12.1.0_r1/core/java/android/hardware/display/BrightnessInfo.java)、[Android 13](https://raw.githubusercontent.com/aosp-mirror/platform_frameworks_base/android-13.0.0_r1/core/java/android/hardware/display/BrightnessInfo.java)。
