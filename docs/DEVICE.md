# 目标设备说明

小米 17 Pro Max（`altdb`）/ Android 17 HyperOS 4 / KernelSU，2026-10-29 实测。

## 背光

- 主屏 `panel0-backlight`，`max_brightness` 16383；副屏 `panel1-backlight`，最大 4095，熄灭时 `bl_power` 为 4。
- 把主屏节点设为 0444 后，系统自动亮度仍会持续改写背光（实测 75 秒内从 188 变到 6352 再回到 116），显示 HAL 持有已打开的写句柄，文件权限挡不住。v6.8 的 0444 锁定因此不是独占：系统与模块各自在自己的目标变化时写入。

## 设置

- `/data/system/users/0/settings_system.xml` 为 ABX 格式（约 140 KB），`screen_brightness` 为 0～255。
- 自动亮度下，系统会随环境光改写 `screen_brightness`（实测未触碰亮度条时依次为 17、36、255、27、19）。
- 拖动亮度条后，`dumpsys display` 的 `mUserLux`、`mUserBrightness` 从 `-1.0`/`NaN` 变为拖动时的照度与亮度；系统自动改写不会改变它们。

## 光感

- 前置 `tcs3760 Ambient Light Sensor Non-wakeup`（类型 5），后置 `tcs3760_sec_back Ambient Light Back Sensor Non-wakeup`（类型 33171055，`xiaomi.sensor.back_lux`），接近 `proximity Proximity Sensor Non-wakeup`（类型 8）。
- 系统光感参数：短/长时间窗 1000/3000 ms，增亮/变暗防抖 1000 ms，小幅增亮防抖 5000 ms，HBM 门限 4475 lux。阈值曲线见 `core/dual_sensor.cpp`。
