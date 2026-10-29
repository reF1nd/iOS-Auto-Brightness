# 兼容性

- **系统**：Android 12（API 31）及以上，安装包含 ARM64、ARMv7、x86_64、x86 核心。
- **Root**：KernelSU、APatch、Magisk。监督进程依赖管理器自带的 BusyBox（`flock`）。WebUI 需要支持 `ksu.exec` 的宿主。
- **背光**：优先 `/sys/class/backlight/panel0-backlight`，否则按名称排序取第一个 `max_brightness > 0` 的设备。曲线按 `max_brightness/8191` 缩放，与 v6.8 相同。
- **光感**：通过 `libandroid.so` 的 NDK 接口读取默认光感（类型 5）、接近传感器（类型 8）和小米后置光感（类型 33171055）。NDK 不可用时依次尝试 `/sys/bus/iio/devices/*/in_illuminance_input`、`/sys/class/sensors/*/lux`、`dumpsys sensorservice`。
- **设置**：读取 `/data/system/users/0/settings_system.xml`（ABX 或文本 XML），与 v6.8 一样只支持主用户。读不到时保持上一次的值；从未读到时按自动模式处理。
- **学习**：依赖 `dumpsys display` 中 AOSP 亮度映射策略的 `mUserLux`/`mUserBrightness`。系统没有这两个字段时不会学习。
- **双光感**：阈值曲线取自小米 17 Pro Max 的系统配置；其他设备没有后置光感时自动只用前置。
- **熄屏判断**：读背光设备的 `bl_power`，无法打开时视为亮屏（v6.8 行为）。
