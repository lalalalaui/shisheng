# 基于 VSCode 的 STM32G071 智能固态侧键工程完整说明

## 1. 这份工程现在达成了什么

这份工程已经不再依赖 STM32CubeMX 来“生成后才能继续开发”。  
CubeMX 仍然可以作为最初的外设初始化来源，但当前仓库已经具备：

- 完整的 `CMake` 构建脚本
- `VSCode` 任务配置
- 可直接编译的应用层模块
- 软件 SPI 驱动的数字电位器增益控制
- 开机 DAC 自动调平
- 运行时动态基线跟踪
- 按压峰值复盘式 AGC 调节

也就是说，后续你完全可以把这个仓库当成一个普通嵌入式 C 工程，在 VSCode 中直接编译、下载、调试，而不需要每次回到 CubeMX 里点生成代码。

---

## 2. 硬件接线

## 2.1 STM32 与模拟前端

双通道柔性压力侧键系统当前对应关系如下：

| 功能 | 通道1 | 通道2 | 说明 |
| --- | --- | --- | --- |
| 传感器差分输出采样 | `PA0 / ADC1_IN0` | `PA1 / ADC1_IN1` | 采集放大器输出 `Vout` |
| DAC 基准输出 | `PA4 / DAC1_OUT1` | `PA5 / DAC1_OUT2` | 输出 `VDAC` 作为差分基准 |
| 串口调试 | `USART2` | `USART2` | 用于 `printf` 输出 |

你的模拟关系仍然是：

```text
Vout = Gain × (Vsensor - VDAC)
```

和之前不同的是，`Gain` 不再是固定值，而是由 MCP42100 控制的可变增益。

---

## 2.2 STM32 与 MCP42100

当前软件 SPI 接线按你给的示例实现为：

| MCP42100 引脚 | STM32 引脚 | 作用 |
| --- | --- | --- |
| `CS` | `PB3` | 片选 |
| `SCK` | `PB4` | 软件 SPI 时钟 |
| `SI` / `MOSI` | `PB5` | 软件 SPI 数据输出 |
| `SO` | 未使用 | 本项目只写不读 |
| `VDD` | `3.3V` | 数字电位器供电 |
| `VSS` | `GND` | 地 |
| `P0A/P0W/P0B` | 接运放通道0反馈网络 | 控制通道0增益 |
| `P1A/P1W/P1B` | 接运放通道1反馈网络 | 控制通道1增益 |

说明：

- 你现在使用的是 **双通道数字电位器 MCP42100**
- 两个电位器通道分别替代原来运放反馈网络中的两只 `100k` 固定电阻
- 软件要求两个通道阻值始终一致，因此写阻值时必须连续分别写 `POT0` 和 `POT1`

---

## 3. 系统工作原理

## 3.1 原来的固定增益方案

原来系统增益是固定的，比如 `100x`。  
此时系统只需要解决一个问题：

- 用 DAC 调节 `VDAC`
- 让静态工作点尽量落在 ADC 中点附近

优点是简单，缺点是：

- 轻按时可能信号太小
- 重按时可能 ADC 饱和

也就是说，固定增益无法同时兼顾：

- 微小压力的高灵敏度
- 大压力时不削顶

---

## 3.2 现在的自适应增益方案

现在你的系统变成了双闭环：

### 第一闭环：DAC 自动调平

目标：

- 让 `Vsensor ≈ VDAC`
- 让 `Vout` 回到 ADC 线性工作区中点附近

对应代码：

- `SmartBtn_HardwareCalibration()`

### 第二闭环：AGC 自适应增益

目标：

- 若按压太弱，增大增益，提高灵敏度
- 若按压过强导致饱和，降低增益，防止削顶

对应代码：

- `HW_SetGain()`
- `SmartBtn_Update_And_Process()`

这两层闭环配合后，系统能同时解决：

- 静态偏置不准
- 温漂
- 轻按信号太弱
- 重按信号过饱和

---

## 4. 为什么 AGC 不能在按下过程中立刻切换

这是这次架构升级最关键的设计思想之一。

如果在 `PRESSED` 状态中一边按压、一边直接切换增益，会带来几个问题：

1. 基线会突变  
   因为增益变了，同样的传感器差分电压会映射成完全不同的 ADC 值。

2. 状态机会混乱  
   原本处于“按下”的波形，可能因为增益瞬时变化看起来像“释放”或“再按下”。

3. 峰值信息失真  
   一次按压还没结束，量程已经变了，这样峰值不能用于下一次合理决策。

因此，这份代码采用的是：

> 按压过程中只记录证据，不改增益；等释放时再复盘，并决定下一次按压该用什么增益。

这是非常适合汇报时强调的工程点。

---

## 5. 代码实现架构

## 5.1 模块分层

### 底层 HAL 层

CubeMX 最初生成，但现在已经作为普通源码纳入仓库：

- `adc.c / adc.h`
- `dac.c / dac.h`
- `usart.c / usart.h`
- `gpio.c / gpio.h`
- `main.c / main.h`

### 智能按键模块

核心文件：

- [Core/Inc/smart_button.h](/e:/shishenggongchuang/banbenfinal/Core/Inc/smart_button.h)
- [Core/Src/smart_button.c](/e:/shishenggongchuang/banbenfinal/Core/Src/smart_button.c)

它负责：

- DAC 调平
- ADC 读取与滤波
- 软件 SPI 写 MCP42100
- 动态基线跟踪
- 按压状态机
- 释放后 AGC 复盘

### VSCode 构建层

新增或调整的文件：

- [CMakePresets.json](/e:/shishenggongchuang/banbenfinal/CMakePresets.json)
- [cmake/gcc-arm-none-eabi.cmake](/e:/shishenggongchuang/banbenfinal/cmake/gcc-arm-none-eabi.cmake)
- [.vscode/settings.json](/e:/shishenggongchuang/banbenfinal/.vscode/settings.json)
- [.vscode/tasks.json](/e:/shishenggongchuang/banbenfinal/.vscode/tasks.json)
- [.vscode/launch.json](/e:/shishenggongchuang/banbenfinal/.vscode/launch.json)

---

## 5.2 `SmartBtn_t` 结构体现在包含什么

当前每个按键对象包含：

- `channel`
- `state`
- `dac_value`
- `adc_filtered`
- `dynamic_baseline`
- `press_threshold`
- `baseline_track_counter`
- `current_gain_val`
- `last_peak_adc`
- `saturation_flag`

可以这样理解：

- 前 6 项用于按键检测
- `current_gain_val` 用于记录当前增益档位
- `last_peak_adc` 和 `saturation_flag` 用于一次按压结束后的 AGC 决策

---

## 6. 核心代码逻辑解释

## 6.1 软件 SPI 写数字电位器

函数：

```c
void HW_SetGain(uint8_t gain_value)
```

实现逻辑：

1. 初始化 `PB3/PB4/PB5` 为推挽输出
2. 拉低 `CS`
3. 发送 `0x11`
4. 发送 `gain_value`
5. 拉高 `CS`
6. 再次拉低 `CS`
7. 发送 `0x12`
8. 发送 `gain_value`
9. 拉高 `CS`

为什么要这样做：

- `0x11` 对应写通道0
- `0x12` 对应写通道1
- 两个通道必须保持一致，才能保证两路运放增益匹配

---

## 6.2 开机自动调平

函数：

```c
bool SmartBtn_HardwareCalibration(SmartBtn_t *btn)
```

逻辑：

- 用二分法不断试探 DAC 值
- 目标是让 ADC 读数尽量接近 `2048`
- 命中死区 `2048 ± 50` 即视为成功

为什么仍然需要这一层：

因为即使有 AGC，系统也必须先保证静态工作点正确。  
AGC 负责的是“波形幅度”，DAC 调平负责的是“工作中心点”。

---

## 6.3 运行时按键识别

函数：

```c
void SmartBtn_Update_And_Process(SmartBtn_t *btn)
```

每次调用都做：

1. 读取 ADC 平均值
2. 一阶低通滤波
3. 若在 `IDLE` 状态，慢速跟踪基线
4. 若超过阈值，转入 `PRESSED`
5. 若在 `PRESSED` 状态，记录峰值与饱和情况
6. 释放时决定是否调整增益

---

## 6.4 峰值复盘式 AGC

当前 AGC 逻辑是：

### 按下期间

- 更新：
  - `last_peak_adc = max(last_peak_adc, adc_filtered)`
- 若 `adc_filtered > 3900`
  - 置位 `saturation_flag`

### 释放瞬间

分两种情况：

#### 情景 A：发生饱和

如果本次按压中出现：

```text
adc_filtered > 3900
```

说明增益过高。  
这时：

- `current_gain_val` 乘以 `0.6`
- 调用 `HW_SetGain()`
- 重新调用 `SmartBtn_HardwareCalibration()`

#### 情景 B：波峰太矮

如果未饱和，且：

```text
last_peak_adc - dynamic_baseline < 400
```

说明本次按压信号太弱。  
这时：

- `current_gain_val` 乘以 `1.5`
- 上限不超过 `255`
- 调用 `HW_SetGain()`
- 再做一次 DAC 校准

### 为什么释放后要重新校准 DAC

因为增益一旦变了，同样的差分输入会映射成不同的 ADC 输出。  
如果不重新调 `VDAC`，原本的中点工作区就被打破了。

所以这里形成了一个非常完整的控制链：

```text
改变增益 -> 模拟链路比例变化 -> 必须重新校准 DAC -> 恢复最佳工作点
```

---

## 7. 与之前版本的对比

## 7.1 之前版本：固定增益

特点：

- 增益固定
- 只做 DAC 调平
- 只做动态基线和按键状态机

优点：

- 实现简单
- 调试容易

缺点：

- 轻按不够敏感
- 重按容易饱和

---

## 7.2 现在版本：自适应增益

特点：

- 增益由 MCP42100 控制
- 软件 SPI 写双通道数字电位器
- 释放后根据上一次按压复盘调节增益
- 改增益后自动重校准 DAC

优点：

- 小信号更灵敏
- 大信号不易削顶
- 自动适配不同手劲和不同样机

代价：

- 状态机更复杂
- 增益与基线之间存在联动，需要更严谨的软件设计

---

## 8. 为什么说现在不用 CubeMX 也能继续开发

这里要准确表达，不要说成“完全不需要 CubeMX 文件”。

更准确的说法是：

> 工程最初的底层初始化来源于 CubeMX，但现在所有需要的源码、CMake 脚本和 VSCode 配置都已经在仓库中固定下来，后续修改业务逻辑、编译、下载、调试都不再依赖重新打开 CubeMX 生成代码。

当前仓库已经具备：

- 固定的源码树
- 固定的工具链文件
- 固定的构建预设
- VSCode 一键构建任务
- VSCode 一键下载任务

所以你可以在 VSCode 中直接完成日常开发。

---

## 9. VSCode 中如何编译

前提：

- 已安装 `CMake`
- 已安装 `Ninja`
- 已安装 `arm-none-eabi-gcc`
- 已安装 `STM32_Programmer_CLI`

推荐把这些工具加入系统 `PATH`。

### 方法一：命令行

在工程根目录执行：

```bash
cmake --preset Debug
cmake --build --preset Debug
```

编译成功后会生成：

```text
build/Debug/banbenfinal.elf
```

### 方法二：VSCode 任务

已新增：

- `CMake: Configure Debug`
- `CMake: Build Debug`
- `STM32: Flash ELF`

你可以在 VSCode 中：

1. `Ctrl + Shift + P`
2. 运行 `Tasks: Run Task`
3. 选择 `STM32: Flash ELF`

它会自动：

1. 配置 CMake
2. 编译工程
3. 调用 `STM32_Programmer_CLI` 烧录 `ELF`

---

## 10. VSCode 中如何下载和调试

当前已新增 [.vscode/launch.json](/e:/shishenggongchuang/banbenfinal/.vscode/launch.json)，默认面向 `Cortex-Debug` 插件和 `ST-Link`。

你可以：

1. 安装 VSCode 插件 `Cortex-Debug`
2. 连接 ST-Link
3. 选择 `Cortex Debug STM32G071`
4. 直接启动下载和调试

注意：

- `launch.json` 中目前引用了 `${workspaceFolder}/.vscode/STM32G071.svd`
- 如果你本地还没有这个 `svd` 文件，调试本身通常仍可进行，但外设寄存器视图会不完整

---

## 11. 这份代码现在最值得汇报的亮点

你可以重点讲这四点：

### 1. 从固定增益升级为自适应增益

这让系统从“只能工作”升级为“能适应不同力度和动态范围”。

### 2. 增益调节与 DAC 调平联动

这不是简单改一个电阻值，而是建立了：

```text
增益改变 -> 工作点变化 -> DAC 重新校准
```

的闭环补偿机制。

### 3. AGC 只在空闲边界执行

避免了按压过程中切增益导致的基线跳变和状态错乱。

### 4. 工程已脱离 CubeMX 工作流依赖

后续构建、下载、调试都可直接在 VSCode 中完成，更接近真实开发流程。

---

## 12. 还可以继续优化的方向

如果老师继续追问，你可以主动说：

1. 当前软件 SPI 采用 `GPIO + bit-bang`，后续可换成硬件 SPI 提升速度和时序稳定性
2. 当前 AGC 以“单次按压复盘”方式调整，后续可引入多次按压统计平均
3. 当前 ADC 读取仍是轮询式，后续可改成定时器触发 + DMA
4. 当前双通道增益同步更新，后续如果硬件允许，也可扩展成双通道独立增益

---

## 13. 一句话总结

这次升级后的系统，不再只是一个“基于柔性压力传感器的固态侧键检测程序”，而是一个：

**同时具备工作点自动调平、动态增益控制、温漂补偿、状态机识别，并可在 VSCode 中独立构建下载的完整嵌入式原型工程。**

