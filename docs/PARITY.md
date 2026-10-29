# 与闭源 v6.8 的对照

参考对象是 `ios_brightness_v6.8.zip` 中的 `system/bin/ios_brightness_daemon`（ARM64，未剥离符号，源文件名 `ios_brightness.c`）。下表列出它的每一项行为、依据的函数或全局变量，以及本仓库的对应实现。地址为该二进制中的虚拟地址。

## 曲线

| v6.8 | 本仓库 |
|---|---|
| `ios_curve`（0x5dd4）：lux ≤ 0 返回 1；分 7 段把 lux 换算成 nits：≤0.5 为 `0.222+1.556·lux`，≤2 为 `2·log2(lux)+1`，≤10 为 `12·log5(lux/2)+3`，≤100、≤1000、≤10000 为常用对数段，以上为 `500+38.5·log10(min(lux,1e5)/1e4)` | `curve.cpp` `nits_for_lux`，乘加处与原指令一样使用融合乘加 |
| `nits_tbl`（0x1958）：52 个面板亮度点，对应原始级别 `i·8191/51`；在表内线性插值后按 `max_brightness/8191` 缩放，`+0.5` 截断，限制在 1～max | `kNitsTable` 由原二进制逐位生成；`level_for_nits` |
| 0.5～2 lux 段在 0.5 lux 处从 1 nit 跳到 −1 nit，0.5～0.707 lux 映射为最低亮度 | 保留（`curve_tests` 有对应用例） |
| 学习参照：`pow(slider/255, manual_gamma)·538.316+0.222` nits，同一张表换算 | `slider_level` |

闭源 WebUI 的 `app.js` 也有作者独立写的 `luxToNits`/`nitsToBl`，`curve_tests` 用它做双精度对照，误差不超过 1 级。

## 主循环

`main`（0x2e38）在启动后进入轮询：每次最多睡 1 秒，直到距上次处理满 `current_poll_ms`，再执行一轮。`Engine::tick` 与这一轮一一对应。

| 步骤 | v6.8 | 本仓库 |
|---|---|---|
| 设置 | `cached_auto` < 0 或距上次满 8 秒时读取 `screen_brightness_mode` 与 `screen_brightness`（≤255） | `refresh_settings`；文件读取见“差异” |
| 模式切换 | 自动：节点 0444、启用光感、800 ms；手动：0644、停用光感、5000 ms；两者都清除阳光状态并标记首轮 | 同 |
| 光感 | 自动模式下 `ALooper_pollOnce(0)` 后逐个取事件：类型 5 为前置，33171055 为后置，8 为接近（读数 0 视为遮挡） | `Sensors::poll` / `apply_events` |
| 亮灭屏 | 每 2 秒读 `bl_power`，首字符为 `0`（或无法打开）视为亮屏；亮屏瞬间直接写入当前曲线目标，不渐变 | `tick` / `wake` |
| 熄屏 | 停用光感，15000 ms 一轮；亮屏后的下一轮仍隔 15 秒 | 同 |
| 接近遮挡 | 自动模式下不处理，8000 ms 一轮 | 同 |
| 手动模式 | 只记录节点当前值，5000 ms 一轮 | 同 |
| 输入照度 | 前置；前置为负且后置可用时用后置；无 NDK 时用 sysfs / dumpsys 备用读取（dumpsys 缓存 15 秒） | `select_lux`；后置规则见“差异” |
| 快速变暗 | 连续三次读数递减且相对第一次下降超过 35% | 同 |
| 尖峰限幅 | 平滑值 ≥ 1 时限制在 `[sm·spike_down, max(sm·spike_up, 5)]` | 同 |
| 趋势 | 最近 5 个限幅值的线性回归斜率，绝对值 > 50 时 alpha 至少 0.3 | 同 |
| EMA | 相对变化 ≥ `fast_threshold` 用 `alpha_fast`，≤ `slow_threshold` 用 `alpha_slow`，之间线性插值；变暗时至少 `alpha_slow + (0.7 − alpha_slow)·min(rel/fast, 1)` | 同 |
| 学习偏移 | `|offset| ≥ 0.001` 时目标加 `(int)(offset·max)`，限制在 1～max | `apply_offset_and_limits` |
| 夜间 | 本地时间 18:00–5:59 目标乘 0.95 | 同 |
| 低电量 | 电量 ≤ 14% 且未充电时不超过 `0.7·max`；电量 30 秒刷新一次 | 同 |
| 阳光模式 | `highlux_can_boost`（0x6284）：电量 > 29% 或充电，且 battery/skin/shell/quiet/back/soc 温度区（前 20 个中最多 8 个，16～79℃ 有效）最高不超过 45℃，60 秒刷新；连续两轮超过阈值进入，`(1+boost)·目标` 与 `max·min_pct` 取大；低于 0.7 倍阈值后保持 `high_lux_duration_ms` 再退出；温度或电量不满足时立即回到普通目标 | 同 |
| 回差 | 非快速变暗时，与上次目标相差不超过 `hyst_low + (ref/max)·(hyst_high − hyst_low)` 则沿用上次目标 | 同，空目标的修正见“差异” |
| 渐变 | 阳光 1000 ms、快速变暗 200 ms；其他按变化占 max 的比例取 0 / 80 / 180 / 300 / 400 ms，变亮乘 0.7；三次缓动；只在一轮处理中采样 | 同。由于每轮才采样一次，新渐变的第一轮停在当前值，目标稳定后的下一轮写到位；变化小于 max 的 2% 时当轮直接写入 |
| 写入 | 计算值与自己上次写入的值不同才写，不读回节点 | 同 |
| 学习 | 亮度条与上一轮相差 ≥ 10 时，`offset = clamp(0.9·offset + 0.1·(参照级别 − 目标)/max, ±0.3)` 并写文件 | 同，触发条件见“差异” |
| 刷新间隔 | 渐变中 800 ms；否则低电量 2000 ms、普通 800 ms；照度连续 10 轮以上变化小于 0.5 lux 时至少 1600 ms，20 轮以上 8000 ms | 同 |
| 退出 | 节点恢复 0644，保存偏移，关闭光感 | `Engine::shutdown` 与 `run` 收尾 |

配置项、默认值与读取方式（只覆盖文件中出现的键，5 秒检查一次修改时间）同 `load_config`（0x5800）；启动时等待前置读数最多 10 秒，超时按 100 lux 开始。

## 差异

| 项目 | v6.8 | 本仓库 | 原因 |
|---|---|---|---|
| 设置读取 | `read_settings_xml_int`（0x6068）只读前 8191 字节并查找 `name="..."` | `settings_reader.cpp` 完整解析 ABX（`android.util.BinaryXmlSerializer`）与文本 XML | Android 12 起该文件默认是 ABX，v6.8 读不到，`cached_auto` 保持 −1 被当作自动模式，亮度条值固定为初始的 128 |
| 学习触发 | 亮度条变化即学习 | 另外核对 `dumpsys display` 的 `mUserLux`/`mUserBrightness` 是否变化，只学习真实拖动；可用 `learning_enabled=0` 关闭 | HyperOS 自动亮度会随环境光改写 `screen_brightness`，原样生效会把系统自身的调光当作偏好 |
| 后置光感 | 仅前置为负时使用 | `dual_sensor.cpp` 按 HyperOS `DualSensorPolicy` 选择照度，`dual_sensor=0` 恢复 v6.8 行为 | 按用户要求参考系统原生的前后光感逻辑 |
| 启动回差 | 尚无上次目标（0）时，与当前亮度比较落入回差就把目标换成 0，写入最低亮度后再回升 | 沿用当前亮度 | 明显缺陷，只出现在启动后的第一轮 |
| 路径 | `/data/local/tmp/ios_brightness*` | `/data/adb/ios_auto_brightness/`，安装时迁移 v6.8 的配置与偏移 | 私有目录、热更新与 WebUI |
| 背光节点 | 固定 `panel0-backlight` | 优先 `panel0-backlight`，否则第一个有效背光设备 | 兼容其他设备 |

## HyperOS 双光感策略

依据是小米 17 Pro Max（HyperOS 4，Android 17）`/system_ext/framework/miui-services.jar` 中的 `com.android.server.display.DualSensorPolicy`、`AmbientLightRingBuffer` 和 `HysteresisLevelsImpl`，由 `dexdump` 反汇编。

- 前后各有一个环形缓冲区，照度按框架的 `calculateAmbientLux` 加权（近的样本权重大，截距为 3000 ms），快速值取 1 秒、慢速值取 3 秒。
- 增亮、小幅增亮、变暗阈值来自设备的 `MiuiHysteresisLevels` 曲线（超过 HBM 门限 4475 lux 后为 lux+1）。快速值低于增亮阈值时改用小幅阈值和 5 秒防抖，否则 1 秒；变暗 1 秒。
- 后置有明显变化时：若当前用前置且后置快、慢值都超过前置的增亮阈值，或当前用后置且后置仍比前置亮，就用后置快速值；否则回到前置。
- 前置有明显增亮且不低于后置时回到前置；前置明显变暗且低于后置时改用后置（前置被遮住而背后有光）。
- 副屏点亮且前置快速值 ≤ 10 lux 时丢弃后置读数（副屏的光会照到后置光感）。
- 闪光灯、反光场景等依赖系统内部状态的判断没有移植。
