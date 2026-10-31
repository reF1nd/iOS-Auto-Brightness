# 与闭源 v6.8 的对照

参考对象是 `ios_brightness_v6.8.zip` 中的 `system/bin/ios_brightness_daemon`（ARM64，未剥离符号，源文件名 `ios_brightness.c`）。下表列出它的每一项行为、依据的函数或全局变量，以及本仓库的对应实现。地址为该二进制中的虚拟地址。

## 曲线

v6.8 的曲线（`ios_curve` 0x5dd4：分 7 段把 lux 换算成 nits；`nits_tbl` 0x1958：52 点面板表，按 `max_brightness/8191` 缩放）已不再使用。亮度曲线改用系统自己的，见下方[系统亮度曲线与学习](#系统亮度曲线与学习)。主循环在曲线之外的每一步仍与 v6.8 相同。

## 主循环

`main`（0x2e38）在启动后进入轮询：每次最多睡 1 秒，直到距上次处理满 `current_poll_ms`，再执行一轮。`Engine::tick` 与这一轮一一对应。

| 步骤 | v6.8 | 本仓库 |
|---|---|---|
| 设置 | `cached_auto` < 0 或距上次满 8 秒时读取 `screen_brightness_mode` 与 `screen_brightness`（≤255） | `refresh_settings`；文件读取见“差异” |
| 模式切换 | 自动：节点 0444、启用光感、800 ms；手动：0644、停用光感、5000 ms；两者都清除阳光状态并标记首轮 | 同 |
| 光感 | 自动模式下 `ALooper_pollOnce(0)` 后逐个取事件：类型 5 为前置，33171055 为后置，8 为接近（读数 0 视为遮挡） | `Sensors::poll` / `apply_events` |
| 亮灭屏 | 每 2 秒读 `bl_power`，首字符为 `0`（或无法打开）视为亮屏；亮屏瞬间直接写入当前曲线目标，不渐变 | `tick` / `wake`。息屏显示（AOD）时 `bl_power` 仍为 `0`，v6.8 会把它当成亮屏、光线一变就把正常亮度写进 AOD。Zygisk 上报连接时改用主屏的 `Display.STATE_*`：DOZE、DOZE_SUSPEND 按熄屏处理，状态一变立刻检查；没有上报时保持 v6.8 行为 |
| 熄屏 | 停用光感，15000 ms 一轮；亮屏后的下一轮仍隔 15 秒 | 同 |
| 接近遮挡 | 自动模式下不处理，8000 ms 一轮 | 同 |
| 手动模式 | 只记录节点当前值，5000 ms 一轮 | 同 |
| 输入照度 | 前置；前置为负且后置可用时用后置；无 NDK 时用 sysfs / dumpsys 备用读取（dumpsys 缓存 15 秒） | `select_lux`；后置规则见“差异” |
| 快速变暗 | 连续三次读数递减且相对第一次下降超过 35% | 同 |
| 尖峰限幅 | 平滑值 ≥ 1 时限制在 `[sm·spike_down, max(sm·spike_up, 5)]` | 同 |
| 趋势 | 最近 5 个限幅值的线性回归斜率，绝对值 > 50 时 alpha 至少 0.3 | 同 |
| EMA | 相对变化 ≥ `fast_threshold` 用 `alpha_fast`，≤ `slow_threshold` 用 `alpha_slow`，之间线性插值；变暗时至少 `alpha_slow + (0.7 − alpha_slow)·min(rel/fast, 1)` | 同 |
| 曲线目标 | `ios_curve(平滑照度)` 再加学习偏移 | 系统曲线在平滑照度处的节点值（`SystemCurve`），不再加偏移 |
| 夜间 | 本地时间 18:00–5:59 目标乘 0.95 | 同 |
| 低电量 | 电量 ≤ 14% 且未充电时不超过 `0.7·max`；电量 30 秒刷新一次 | 同 |
| 阳光模式 | `highlux_can_boost`（0x6284）：电量 > 29% 或充电，且 battery/skin/shell/quiet/back/soc 温度区（前 20 个中最多 8 个，16～79℃ 有效）最高不超过 45℃，60 秒刷新；连续两轮超过阈值进入，`(1+boost)·目标` 与 `max·min_pct` 取大；低于 0.7 倍阈值后保持 `high_lux_duration_ms` 再退出；温度或电量不满足时立即回到普通目标 | 电量门槛同；温度门槛去掉，改用系统温控上限（见“差异”） |
| 回差 | 非快速变暗时，与上次目标相差不超过 `hyst_low + (ref/max)·(hyst_high − hyst_low)` 则沿用上次目标 | 同，空目标的修正见“差异” |
| 渐变 | 阳光 1000 ms、快速变暗 200 ms；其他按变化占 max 的比例取 0 / 80 / 180 / 300 / 400 ms，变亮乘 0.7；三次缓动；只在一轮处理中采样 | 同。由于每轮才采样一次，新渐变的第一轮停在当前值，目标稳定后的下一轮写到位；变化小于 max 的 2% 时当轮直接写入。阳光与快速变暗的渐变在目标不变时沿用进行中的渐变，见“差异” |
| 写入 | 计算值与自己上次写入的值不同才写，不读回节点；每次重新 `open` 节点，失败即跳过 | 同。节点 0444 时内核拒绝以写方式打开，root 也一样，所以本机自动模式下两者的写入都没有生效（见 [DEVICE.md](DEVICE.md#背光)） |
| 学习 | 亮度条与上一轮相差 ≥ 10 时，`offset = clamp(0.9·offset + 0.1·(参照级别 − 目标)/max, ±0.3)` 并写文件 | 不同，见“差异”；迁移来的偏移不再使用 |
| 刷新间隔 | 渐变中 800 ms；否则低电量 2000 ms、普通 800 ms；照度连续 10 轮以上变化小于 0.5 lux 时至少 1600 ms，20 轮以上 8000 ms | 同 |
| 退出 | 节点恢复 0644，保存偏移，关闭光感 | `Engine::shutdown` 与 `run` 收尾 |

配置项、默认值与读取方式（只覆盖文件中出现的键，5 秒检查一次修改时间）同 `load_config`（0x5800）；启动时等待前置读数最多 10 秒，超时按 100 lux 开始。

## 差异

| 项目 | v6.8 | 本仓库 | 原因 |
|---|---|---|---|
| 设置读取 | `read_settings_xml_int`（0x6068）只读前 8191 字节并查找 `name="..."` | `settings_reader.cpp` 完整解析 ABX（`android.util.BinaryXmlSerializer`）与文本 XML | Android 12 起该文件默认是 ABX，v6.8 读不到，`cached_auto` 保持 −1 被当作自动模式，亮度条值固定为初始的 128 |
| 亮度曲线 | `ios_curve` 与 52 点面板表 | 系统的自动亮度曲线（`system_curve.cpp`） | 用户要求换成系统内置曲线 |
| 学习触发 | 亮度条变化即学习 | 主屏 `RefactorNitController` 的 `mUserLux`/`mUserLogicalNit` 变化即为一次拖动（设置文件变化时检查，否则每 8 秒），立即按系统规则更新锚点；节点值稳定 1 秒前不写入；`learning_enabled=0` 关闭 | HyperOS 自动亮度会随环境光改写 `screen_brightness`；暗处拖动时它只在个位数内变化，达不到 10 格 |
| 学习方式 | 全局偏移，每次吸收 10% | `nit_controller.cpp` 完整移植系统的 3 点锚点学习与 good curve | 用户要求完全按系统的学习逻辑 |
| 学习保存 | 偏移写文件，重启保留 | 锚点写 `learn.conf`，重启保留；没有可用记录时启动接管系统当前的锚点（`dumpsys` 的 `anchors=`） | 系统重启后会清空锚点；保留是用户最初的要求。装有 Zygisk 时开机把 `learn.conf` 写回系统，否则系统重启后按默认曲线写入 |
| 后置光感 | 仅前置为负时使用 | `dual_sensor.cpp` 按 HyperOS `DualSensorPolicy` 选择照度，参数取自运行中系统的 `dumpsys display`，场景判断由 Zygisk 上报；系统没有该策略或 `dual_sensor=0` 时恢复 v6.8 行为 | 按用户要求参考系统原生的前后光感逻辑 |
| 温控 | 阳光模式前检查前 20 个温度区，超过 45℃ 不增强 | 去掉温度检查；所有输出不超过系统的温控上限（`DisplayPowerControllerImpl.mThermalMaxBrightness` 与 `mBatteryThermalLimitBrightness`） | 用户要求温控直接用系统的；v6.8 的检查在本机从未生效 |
| 阳光渐变 | 阳光模式下只要目标与当前值不同，每轮都重新开始 1000 ms 渐变（0x4e78），并在起点采样，采到的就是当前值；快速变暗的 200 ms 渐变同样如此 | 目标不变时沿用进行中的渐变，与其他渐变一致：进入后第一轮不动，下一轮写到约 98%，再下一轮到位 | v6.8 在阳光模式期间从不写入，增强没有生效，节点停在进入前的值或系统写的值 |
| 前置光感 | 默认光感（类型 5） | 与系统一致：`ro.miui.support.light.fov` 为 1 时改用 FOV 光感（类型 33172111） | 跟随系统实际读取的光感 |
| 启动回差 | 尚无上次目标（0）时，与当前亮度比较落入回差就把目标换成 0，写入最低亮度后再回升 | 沿用当前亮度 | 明显缺陷，只出现在启动后的第一轮 |
| 路径 | `/data/local/tmp/ios_brightness*` | `/data/adb/ios_auto_brightness/`，安装时迁移 v6.8 的配置与偏移 | 私有目录与 WebUI |
| 背光节点 | 固定 `panel0-backlight` | 优先 `panel0-backlight`，否则第一个有效背光设备 | 兼容其他设备 |

## 系统亮度曲线与学习

依据是小米 17 Pro Max 的 `miui-services.jar`（`RefactorNitController`、`RefactorBrightnessUtil`、`DisplayPowerControllerImpl`、`AutomaticBrightnessControllerImpl`）、`services.jar`（`AutomaticBrightnessController`）以及 `framework-ext-res.apk` 和设备 overlay，由 `dexdump` 与 `aapt2` 反汇编。

| 系统 | 本仓库 |
|---|---|
| `AutomaticBrightnessController.updateAutoBrightness`：`mUseRefactorBrightnessPolicy`（`config_refactor_brightness_policy`）为真时，亮度为 `getRefactorBrightness(环境光)`，否则为 AOSP 映射 | `load_system_curve` 按同一开关选择 |
| `RefactorNitController.getCurrentNit(lux, NaN)`：锚点在 `min(lux, 8000)` 处线性插值并保留 3 位小数，得到逻辑尼特；超过 30 时把 30～1060 线性压到 30～1060·B，结果不到 100 时把 1～100 线性抬到 A～100；8000 lux 以上乘高光系数 | `NitController::logical`、`refactor_nits` |
| `DisplayDeviceConfig.getBrightnessFromNit`：屏幕亮度表线性换算 | `PanelMap` |
| `resetDefaultSpline`：锚点 (0, `nit0`)、(seg0, `nit30`)、(seg1, `nitMidHigh`)、(luxMax, 1060)，均来自 `config_defaultLogicalCurve` | `NitController::reset` |
| `updateLogicalCurve`：照度按 8000 封顶；更新主锚点；若拖动值高于 good curve，复制一份并把拖动替换成 good curve 的值 | `NitController::drag` |
| `addDragOnArrays`：删除同段的、以及与新拖动矛盾的旧用户锚点（段端点除外）；距现有锚点不到 0.001 lux 时改写该锚点，否则插入（最多 7 个，满时替换最旧的内侧锚点） | `NitController::add_drag` |
| `enforceMonotonicityAndRestore` 与 `blendedNitAtLux`：默认锚点先恢复默认值；拖动所在段的两端按与默认曲线的差值、按距离线性加权移动，夹在原值与拖动值之间；拖动点左侧取较小值、右侧取较大值，保证单调 | `NitController::enforce`、`blended` |
| `needResetShortTermModelPolicy` → `shouldUseGoodCurve`：`config_brightness_reset_policy` 为真且云端未禁用时，亮屏后首个有效照度满足“息屏 ≥ 30 分钟”或“息屏 ≥ 6 分钟且照度变化 > 20 lux”，主锚点换成 good curve 版本 | `Engine::check_good_curve` |
| AOSP `Spline.createSpline`：`BrightnessConfiguration` 的点单调时用单调三次样条 | `MonotoneSpline` |

## HyperOS 双光感策略

依据是小米 17 Pro Max（HyperOS 4，Android 17）`/system_ext/framework/miui-services.jar` 中的 `com.android.server.display.DualSensorPolicy`、`AmbientLightRingBuffer`、`HysteresisLevelsImpl`、`AutomaticBrightnessControllerImpl` 和 `SceneDetector`，由 `dexdump` 反汇编。

- 前后各有一个环形缓冲区，照度按框架的 `calculateAmbientLux` 加权（近的样本权重大），快速值取短时间窗、慢速值取长时间窗。前置的加权截距是 `mWeightingIntercept`，后置的是长时间窗。
- 增亮、小幅增亮、变暗阈值来自 `MiuiHysteresisLevels` 的三条线性样条，超过 HBM 门限（`HighBrightnessModeData.minimumLux`，没有 HBM 时为 6000）后增亮阈值为 lux+1。快速值低于增亮阈值时改用小幅阈值和小幅防抖。
- 以上曲线、门限、时间窗和前后防抖都在启动时从 `dumpsys display` 主屏（`mDisplayId=0`）的 Display Power Controller 段读取（`display_dump.cpp`）；没有 `Dual Sensor Policy Configuration` 段时不启用该策略。
- 后置有明显变化时：若当前用前置且后置快、慢值都超过前置的增亮阈值，或当前用后置且后置仍比前置亮，就用后置快速值；否则回到前置。
- 前置有明显增亮且不低于后置时回到前置；前置明显变暗且低于后置时改用后置（前置被遮住而背后有光）。
- 副屏点亮且前置快速值 ≤ 10 lux 时丢弃后置读数（副屏的光会照到后置光感）。
- 后置读数乘以 `ro.miui.sensor.fov_back.factor`（`AutomaticBrightnessControllerImpl.loadSensorConfig`，默认 1）。
- 依赖系统内部状态的判断由 Zygisk 部分在 system_server 中读出并上报（`SystemState.java`）：
  - `handleAssistLightSensorEvent`：`getIsTorchOpen()` 为真或 `checkAssistSensorValid()` 为假（手电筒开着、关闭不到 1800 ms，或 ROM 关闭了后置光感；后置相机使用中除外）时丢弃后置读数。上报每 500 ms 检查一次，收到时再丢掉此前 1 秒内的后置样本。
  - `DualSensorPolicy$1.onSensorChanged`：`SceneDetector.getDrivingStatus()`（夜间驾驶）为真且前置在用时丢弃后置读数；进入夜间驾驶时 `shouldResetAssistSensorValue` 清空后置的值（正在用后置时推迟到下一个后置读数）。
  - `updateDualSensorPolicy`：后屏机型（`isRearScreenDevice`）在 `SceneDetector.isReflectiveScene()` 时不切换到后置，阈值照常更新。
  - 没有 Zygisk 时这些状态读不到，按“无”处理。
- 前置光感：`switchLightSensor` 在 `ro.miui.support.light.fov` 为 1 且有 FOV 光感时让主屏改用它；夜间驾驶且读数 `values[1]` 为 1 时，`AutomaticBrightnessController` 取 `values[2] × ro.miui.sensor.fov.factor`。模块照做。
- 没有移植：`SceneDetector` 的 AON 炫光检测（17～200 lux 时暂缓前置照度更新）和夜间驾驶的最低亮度（`mMinDrivingEnableNit`），它们不属于前后光感的选择。

## 系统温控

依据是同一 `miui-services.jar` 的 `DisplayPowerControllerImpl` 与 `ThermalBrightnessController`。

- `ThermalBrightnessController` 按机身温度和环境光查表得到尼特上限（本机配置见 [DEVICE.md](DEVICE.md#温控)），`updateThermalBrightness` 经屏幕亮度表换算成亮度值存入 `mThermalMaxBrightness`。
- `adjustBrightnessByThermal`：温控可用（`mThermalBrightnessControlAvailable`）且该值不是 -1 或 NaN 时，亮度取两者较小值；`adjustBrightnessByBattery` 再与电池温控上限 `mBatteryThermalLimitBrightness` 取较小值。
- 模块取这两个上限中较低的一个，乘以节点最大值作为所有输出（含阳光模式、亮屏瞬间）的上限（`Engine::apply_thermal_limit`）。装有 Zygisk 时实时上报，否则从 `dumpsys display` 主屏段的 `mThermalMaxBrightness=`、`mBatteryThermalLimitBrightness=` 读取，最多 8 秒一次。
- 低电量限亮（`mLowBatteryLimitBrightness`）不属于温控，模块仍用 v6.8 的低电量规则。
