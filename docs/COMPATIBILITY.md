# 兼容性

- **系统**：Android 12（API 31）及以上，安装包含 ARM64、ARMv7、x86_64、x86 核心。
- **Root**：KernelSU、APatch、Magisk。监督进程依赖管理器自带的 BusyBox（`flock`）。WebUI 需要支持 `ksu.exec` 的宿主。
- **背光**：优先 `/sys/class/backlight/panel0-backlight`，否则按名称排序取第一个 `max_brightness > 0` 的设备。节点值按“亮度值 × `max_brightness`”换算（见下方亮度曲线）。
- **光感**：通过 `libandroid.so` 的 NDK 接口读取默认光感（类型 5）、接近传感器（类型 8）和小米后置光感（类型 33171055）。NDK 不可用时依次尝试 `/sys/bus/iio/devices/*/in_illuminance_input`、`/sys/class/sensors/*/lux`、`dumpsys sensorservice`。
- **设置**：读取 `/data/system/users/0/settings_system.xml`（ABX 或文本 XML），与 v6.8 一样只支持主用户。读不到时保持上一次的值；从未读到时按自动模式处理。
- **亮度曲线**：按本机系统自己的自动亮度曲线计算，启动日志的 `Curve:` 行写明来源。
  - HyperOS 开启 `config_refactor_brightness_policy`（`cmd overlay lookup` 查询，本机由 `AospFrameworkResOverlay` 打开）时，使用 `RefactorNitController` 的逻辑尼特曲线。参数（默认锚点、亮度条换算系数、高光系数、good curve、重置策略）从 `pm path com.miui.rom` 指向的 `framework-ext-res.apk` 读取，再按优先级叠加 `cmd overlay list com.miui.rom` 中已启用 overlay 的同名值；`resources.arsc` 必须未压缩存放（Android 对系统包的要求）。读不到或数值不合理时使用小米 17 Pro Max 的值。
  - 没有开启该策略、但 `dumpsys display` 有 `BrightnessConfiguration` 时，按其中的点做单调三次样条，与 AOSP 映射一致，不学习。
  - 两者都没有时使用小米 17 Pro Max 的逻辑尼特曲线。
  - 屏幕亮度表（尼特 ↔ 亮度值）从 `dumpsys display` 读取，读不到时用同一设备的配置。节点值按“亮度值 × `max_brightness`”换算；在本机暗处，这比系统经驱动写出的值低约 8%（系统写 `brightness_clone` 143 时节点为 154），别的屏幕驱动可能差得更多。
- **学习**：依赖 `dumpsys display` 中主屏（`mDisplayId=0`）`Refactor Nit Controller Configuration` 段的 `mUserLux`/`mUserLogicalNit`。没有该段（未开启上述策略的 HyperOS、非小米系统）时不学习。学习记录与 ROM 的默认锚点绑定：系统更新改了默认锚点后，旧记录会被丢弃并从默认开始。副屏和折叠屏的外屏不参与。
- **同系统的其他机型**：
  - 曲线参数、overlay 和屏幕亮度表都按本机读取，正常情况下与该机系统的默认曲线一致，可对照启动日志的 `Curve check`（与系统表相差超过 5% 时会注明）。
  - 后置光感阈值、HBM 门限仍是小米 17 Pro Max 的值；没有后置光感的机型只用前置。
  - 阳光模式的温度门槛沿用 v6.8：只看前 20 个温度区。本机匹配的温度区是第 79、90 个，所以门槛实际不起作用，其他机型取决于温度区的编号。
  - 背光节点的位数和低亮度驱动映射因屏而异，见上方节点换算。
- **双光感**：阈值曲线取自小米 17 Pro Max 的系统配置；其他设备没有后置光感时自动只用前置。
- **熄屏判断**：读背光设备的 `bl_power`，无法打开时视为亮屏（v6.8 行为）。
