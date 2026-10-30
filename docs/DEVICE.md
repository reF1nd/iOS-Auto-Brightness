# 目标设备说明

小米 17 Pro Max（`altdb`）/ Android 17 HyperOS 4 / KernelSU，2026-10-29 实测。

## 背光

- 主屏 `panel0-backlight`，`max_brightness` 16383；副屏 `panel1-backlight`，最大 4095，熄灭时 `bl_power` 为 4。
- 把主屏节点设为 0444 后，系统自动亮度仍会持续改写背光（实测 75 秒内从 188 变到 6352 再回到 116），显示 HAL 持有已打开的写句柄，文件权限挡不住。v6.8 的 0444 锁定因此不是独占：系统与模块各自在自己的目标变化时写入。

## 设置

- `/data/system/users/0/settings_system.xml` 为 ABX 格式（约 140 KB），`screen_brightness` 为 0～255。
- 自动亮度下，系统会随环境光改写 `screen_brightness`（实测未触碰亮度条时依次为 17、36、255、27、19）。
- 拖动亮度条后，`dumpsys display` 的 `mUserLux`、`mUserBrightness`（AOSP 映射）从 `-1.0`/`NaN` 变为拖动时的照度与亮度，主屏 `Refactor Nit Controller Configuration` 段的 `mUserLux`、`mUserLogicalNit` 变为拖动时的照度与逻辑尼特；系统自动改写不会改变它们。

## 光感

- 前置 `tcs3760 Ambient Light Sensor Non-wakeup`（类型 5），后置 `tcs3760_sec_back Ambient Light Back Sensor Non-wakeup`（类型 33171055，`xiaomi.sensor.back_lux`），接近 `proximity Proximity Sensor Non-wakeup`（类型 8）。
- 系统光感参数：短/长时间窗 1000/3000 ms，增亮/变暗防抖 1000 ms，小幅增亮防抖 5000 ms，HBM 门限 4475 lux。阈值曲线见 `core/dual_sensor.cpp`。

## 系统亮度曲线与学习（2026-10-30）

依据 `miui-services.jar`、`services.jar`、`framework-ext-res.apk`（`com.miui.rom`）及设备 overlay。

- **用哪条曲线**：`framework-res` 默认 `config_refactor_brightness_policy=false`，本机 `/product/overlay/AospFrameworkResOverlay.apk` 把它改为 true（`cmd overlay lookup android android:bool/config_refactor_brightness_policy` 输出 `true`）。于是 `AutomaticBrightnessController.updateAutoBrightness` 用 `getRefactorBrightness(环境光)` 出亮度，`mBrightnessSpline`（AOSP 映射加用户点）不参与。
- **逻辑尼特**：亮度条对应 1～1060 的逻辑尼特（`config_logicalBrightnessSet` 101 点）。`getCurrentNit` 在 `min(lux, 8000)` 处对锚点线性插值，保留 3 位小数。默认锚点为 (0 lux, 2)、(30, 135)、(600, 220)、(5000, 1060)，来自 `config_defaultLogicalCurve` `[0, 2, 30, 135, 0, 600, 220, 5000, 1060]`。
- **实际尼特**：逻辑尼特超过 30 时，把 30～1060 线性压到 30～1060·B；结果不到 100 时，把 1～100 线性抬到 A～100。A 在 5/30/100/500/1000/66520 lux 时为 1/2/6/6/12/80，B 在 1/30/1000/3000/5000/8000 lux 时为 0.24/0.52/0.52/0.58/0.64/1。照度按 8000 封顶；8000 lux 以上再乘高光系数，在 8000/17000/37200/72000 lux 时为 1/2.05/2.05/3.30。
- **与系统表的差别**：`BrightnessConfiguration`（`config_autoBrightnessDisplayValuesNits`，同在 `AospFrameworkResOverlay`）是预先算好的表，从 1 lux 起与上面的计算最多相差 1.5%（16000 lux）；0 lux 时表为 1 尼特，计算为 2 尼特。
- **屏幕亮度表**：实际尼特与亮度值（0～1）按屏幕配置 `screenBrightnessMap` 线性换算：1 尼特对应 0.000366，10 尼特对应 0.0104，59.8 尼特对应 0.037，800 尼特对应 0.4999，3500 尼特对应 1.0。
  - 实测在暗处拖到 7.9595 逻辑尼特时 `mUserBrightness` 为 0.008644696，与线性换算一致（三次样条应为 0.00886）。
  - 自动亮度下节点值约为亮度值 × 16383（实测 0.008645 时 `brightness_clone` 143、节点 154，暗处驱动映射偏高约 8%）。
  - `cmd display set-brightness` 的手动亮度会再经手动换算，同一亮度值的节点值约小一半，不能用来标定自动模式。
- **学习**：环境光按 30、600 lux 分为三段。拖动时：
  - 同段的旧拖动和与新拖动矛盾的旧拖动被删掉，段端点（0/30/600/5000 lux）不会被删。
  - 默认锚点先恢复默认值，再把拖动所在段的两端按“拖动值减默认曲线值”、按距离线性加权移动，夹在原值与拖动值之间。
  - 拖动点左边取较小值、右边取较大值，保持单调。
  - 实测暗处拖动后锚点为 `[(0.0,7.9595423,user), (30.0,135.0,def), (600.0,220.0,def), (5000.0,1060.0,def)]`。
  - 锚点只在内存里，重启后恢复默认；系统另把当前逻辑尼特存到 `/data/system/refactor_brightness.xml`。
- **good curve**：`config_goodCurveLuxPoints/NitPoints` 为 0/30/1000/5000 lux 时 50/300/550/1060。拖动值高于它时，系统另存一份把拖动换成 good curve 值的锚点。
  - 触发条件是 `config_brightness_reset_policy`（框架默认 false，本机由 `MiuiFrameworkResOverlay` 改为 true）且云端 `mResetShortTermModelPolicyDisable=false`，并且息屏 ≥ 30 分钟，或息屏 ≥ 6 分钟且照度变化超过 20 lux。
  - 满足时，亮屏后的第一个有效照度到来时切换到那份锚点（`AutomaticBrightnessController$ShortTermModel.maybeReset`）。

## 阳光模式（2026-10-30）

- v6.8 的温度门槛只在前 20 个温度区里找名称含 battery/skin/shell/quiet/back/soc 的区。本机共 163 个温度区，第 0～19 个都是处理器相关的温度区（`cpullc-*`、`qmx-*`、`cpu-*`），匹配的只有第 79 个 `quiet_therm`（室内使用时 44.1℃）和第 90 个 `battery`（37.7℃）。因此门槛读到 0℃，永远允许增强。
- 电量门槛读 `/sys/class/power_supply/battery/capacity` 与 `status`，本机可用。
- 前后光感为 TCS3760，系统高光系数曲线一直到 72000 lux，说明读数能超过 15000 lux 的触发阈值；后置光感接管后（双光感策略）更容易达到。截至记录时日志里最高 185 lux，户外尚未实测。
- 系统曲线在 15000 lux 时约 1927 尼特（节点约为最大值的 80%），阳光模式把目标抬到至少 90%（约 2650 尼特），约 23000 lux 起为最大值 3500 尼特；系统自己在 17000～37200 lux 为 2175 尼特，72000 lux 才到 3500 尼特。系统的 HBM 门限是 4475 lux（600 尼特以上），阳光模式直接写节点，不经过它。
