# 基于 STM32G071 的柔性压力传感器手机固态侧键项目解读

## 1. 项目一句话概括

这是一个基于 `STM32G071` 的双通道手机侧键原型。  
它不是传统的机械按键，而是通过 **柔性压力传感器 + 差分减法放大器 + DAC 基准调平 + ADC 采样 + 软件状态机** 来识别“按下”和“释放”。

从软件角度看，这份工程做了四件事：

1. 用 `DAC` 给每个通道输出一个可调参考基准 `VDAC`
2. 用 `ADC` 读取差分放大后的输出 `VOUT`
3. 在开机时自动搜索最合适的 `DAC`，让输出落在 ADC 中点附近，避免饱和
4. 在运行时通过滤波、慢基线跟踪、阈值和状态机实现稳定按键识别

---

## 2. 硬件原理与核心公式

你的硬件核心公式是：

```text
VOUT = 100 × (Vsensor - VDAC)
```

含义如下：

- `Vsensor`：柔性压力传感器对应的电压
- `VDAC`：STM32 DAC 输出的参考基准
- `VOUT`：经过差分减法放大器和 100 倍增益后的输出，送给 MCU 的 ADC

### 2.1 为什么这个公式很关键

这个系统不是“直接采传感器”，而是采样：

```text
传感器电压 - MCU 给出的参考基准
```

再把这个差值放大 `100` 倍。

这意味着：

- 当 `Vsensor` 和 `VDAC` 很接近时，`VOUT` 会比较小，ADC 工作在线性区
- 当二者差值稍微偏大时，经过 `100` 倍放大后，ADC 很可能快速接近满量程或接近 0

所以软件必须解决一个特别现实的问题：

**开机时必须先把 DAC 调到一个“平衡点”，否则 ADC 很容易饱和，后续按键识别就不可靠。**

### 2.2 侧键动作对应的信号变化

题设给出的物理逻辑是：

- 用户按下侧键
- 柔性压力传感器电阻减小
- `Vsensor` 升高
- `VOUT = 100 × (Vsensor - VDAC)` 增大
- ADC 读数显著上升

所以最终的软件判据就变成：

**“ADC 值相对于当前基线显著上升” = 按键被按下。**

---

## 3. 为什么这类手机侧键不能只靠固定阈值

如果只是简单写成：

```c
if (adc > 2500) {
    // 按下
}
```

这种做法很容易失败，原因主要有四类：

1. 柔性传感器个体差异大  
   同一批次传感器静态阻值、压阻曲线都可能不同。

2. 温漂明显  
   长时间工作后，温度变化会让 `Vsensor` 缓慢漂移。

3. 装配应力不同  
   手机壳体、FPC 弯折、背胶、结构预压都会带来不同静态偏置。

4. 高增益放大带来“极敏感”问题  
   100 倍增益意味着一个小小的偏差也会被放大。

因此这份代码采用了三层稳健机制：

1. **硬件自适应调平**：开机先找 DAC 平衡点
2. **动态软件基线**：运行时慢慢跟踪温漂
3. **状态机 + 迟滞**：按下和释放使用不同阈值，避免抖动

---

## 4. 工程架构

### 4.1 文件分层

当前工程可以理解为三层：

#### 第一层：CubeMX 生成的底层 HAL 层

主要文件：

- `Core/Src/adc.c`
- `Core/Src/dac.c`
- `Core/Src/usart.c`
- `Core/Src/gpio.c`
- `Core/Src/main.c`
- `Core/Inc/adc.h`
- `Core/Inc/dac.h`
- `Core/Inc/usart.h`

作用：

- 初始化 STM32 外设
- 提供 HAL 句柄，如 `hadc1`、`hdac1`、`huart2`

#### 第二层：硬件包装与算法层

核心文件：

- [Core/Inc/smart_button.h](/e:/shishenggongchuang/banbenfinal/Core/Inc/smart_button.h)
- [Core/Src/smart_button.c](/e:/shishenggongchuang/banbenfinal/Core/Src/smart_button.c)

作用：

- 封装 ADC/DAC/UART 的实际用法
- 实现开机校准
- 实现滤波、基线跟踪和状态机
- 对 `main.c` 暴露极简 API

#### 第三层：应用调用层

主要文件：

- [Core/Src/main.c](/e:/shishenggongchuang/banbenfinal/Core/Src/main.c)

作用：

- 初始化系统和外设
- 调用 `SmartBtn_System_Init()`
- 在死循环中调用 `SmartBtn_System_Tick()`

### 4.2 设计思想

这个架构体现了典型的 **高内聚、低耦合**：

- `main.c` 不关心每个按键怎么调 DAC、怎么滤波、怎么判定按下
- `smart_button.c` 内部自己管理全部算法和硬件细节
- 外部只需要调用两个函数

这非常适合向老师汇报，因为你可以明确说：

> 我把“手机侧键识别”设计成了一个独立模块，主程序只负责调度，不负责细节，实现了模块化和可维护性。

---

## 5. smart_button.h 解读

### 5.1 通道宏定义

```c
#define SMART_BUTTON_CH1             1U
#define SMART_BUTTON_CH2             2U
```

用于区分两个独立按键通道。

对应关系是：

- 通道1：`ADC PA0`，`DAC PA4`
- 通道2：`ADC PA1`，`DAC PA5`

### 5.2 参数宏定义

```c
#define SMART_BUTTON_DAC_MAX_VALUE   4095U
#define SMART_BUTTON_ADC_TARGET      2048U
#define SMART_BUTTON_ADC_DEADZONE    50U
```

含义：

- `4095`：12 位 DAC 最大值
- `2048`：12 位 ADC 中点
- `50`：调平时的允许误差

为什么目标选中点 `2048`：

- 向上和向下都留有近似对称余量
- 能同时容纳正向波动和温漂
- 对于带差分放大的系统，居中最稳妥

### 5.3 状态枚举

```c
typedef enum
{
  SMART_BUTTON_STATE_IDLE = 0U,
  SMART_BUTTON_STATE_CALIBRATING,
  SMART_BUTTON_STATE_PRESSED,
  SMART_BUTTON_STATE_ERROR
} SmartBtnState_t;
```

四个状态分别表示：

- `IDLE`：空闲，可跟踪慢漂移，可等待按下
- `CALIBRATING`：正在开机校准
- `PRESSED`：已按下，冻结基线跟踪
- `ERROR`：异常，比如非法通道或校准失败

### 5.4 结构体 SmartBtn_t

```c
typedef struct
{
  uint8_t channel;
  SmartBtnState_t state;
  uint32_t dac_value;
  uint16_t adc_filtered;
  uint16_t dynamic_baseline;
  uint16_t press_threshold;
  uint16_t baseline_track_counter;
} SmartBtn_t;
```

这是整个按键模块最重要的数据结构。

各字段作用：

- `channel`
  - 当前按键属于通道1还是通道2

- `state`
  - 当前状态机状态

- `dac_value`
  - 开机校准后找到的 DAC 平衡值

- `adc_filtered`
  - 当前滤波后的 ADC 值

- `dynamic_baseline`
  - 运行中的动态软件基线，用于慢慢吸收温漂

- `press_threshold`
  - 判定按下的阈值，默认约 `300`

- `baseline_track_counter`
  - 用于控制“慢基线跟踪”的速度

### 5.5 对外接口

```c
bool SmartBtn_HardwareCalibration(SmartBtn_t *btn);
void SmartBtn_Update_And_Process(SmartBtn_t *btn);
void SmartBtn_System_Init(void);
void SmartBtn_System_Tick(void);
```

可以理解成两组接口：

底层对象级接口：

- `SmartBtn_HardwareCalibration`
- `SmartBtn_Update_And_Process`

应用级总控接口：

- `SmartBtn_System_Init`
- `SmartBtn_System_Tick`

---

## 6. smart_button.c 逐段解读

## 6.1 全局实例

```c
SmartBtn_t Btn1;
SmartBtn_t Btn2;
```

这两个全局对象分别代表两个按键通道。  
它们把“每个按键自己的状态、阈值、基线、DAC 值”都独立保存下来。

这说明你的系统虽然只有一个模块，但内部已经具备 **多实例、多通道独立处理能力**。

---

## 6.2 printf 重定向

```c
int __io_putchar(int ch)
{
  uint8_t data = (uint8_t)ch;
  (void)HAL_UART_Transmit(&huart2, &data, 1U, SMART_BUTTON_UART_TIMEOUT_MS);
  return ch;
}

int fputc(int ch, FILE *stream)
{
  (void)stream;
  return __io_putchar(ch);
}
```

作用：

- 把 `printf()` 的输出通过 `USART2` 发送出去
- 后续调试可以直接打印“按下”“释放”“校准值”等信息

为什么需要这个功能：

- 固态按键本质上是模拟量判定，调试时最重要的是看数值变化
- 串口打印是最简单可靠的观测手段

可以向老师解释：

> 我做了 `printf` 重定向，目的是把调试门槛降到最低。这样在算法迭代过程中，我可以实时看到 ADC、基线和按键状态，便于快速校正阈值和跟踪速度。

---

## 6.3 通道映射函数

```c
static uint8_t HW_GetDACChannel(uint8_t ch, uint32_t *dac_channel)
static uint8_t HW_GetADCChannel(uint8_t ch, uint32_t *adc_channel)
```

作用：

- 把逻辑通道号 `1/2`
- 映射为 STM32 HAL 使用的 `DAC_CHANNEL_1/2`、`ADC_CHANNEL_0/1`

这样做好处是：

- 上层只关心“第几个按键”
- 不关心它在 STM32 的具体寄存器通道编号

这属于典型的 **硬件抽象**。

---

## 6.4 DAC 设置函数

```c
static void HW_SetDAC(uint8_t ch, uint32_t value_12bit)
```

功能：

1. 根据逻辑通道找到对应 DAC 通道
2. 把目标值限制在 `0~4095`
3. 调用 `HAL_DAC_SetValue`
4. 调用 `HAL_DAC_Start`

为什么每次都 `Start`：

- 目的是确保对应通道已经启动
- 对于当前这个实验性原型，这种写法简单直观

如果以后要做性能优化，可以把 `Start` 放到初始化阶段，只在这里更新数值。

---

## 6.5 ADC 平均采样函数

```c
static uint16_t HW_ReadADC_Avg(uint8_t ch, uint8_t count)
```

这是整个数据链路里非常核心的函数。

它做了几件事：

1. 选择要采样的 ADC 通道
2. 把 ADC 临时配置成“单通道、单次转换”
3. 连续采 `count` 次
4. 对结果求平均值

### 为什么要平均

柔性传感器、放大器和 ADC 都可能有噪声。  
连采几次求平均，是非常经典、成本极低的抑噪方法。

### 为什么重新配置 ADC

CubeMX 默认给 `ADC1` 生成的是：

- `scan mode`
- `continuous mode`
- `2 个 regular channel`

但你的按钮模块更适合“指定通道，读一次，得平均值”的调用方式。  
因此这里在 wrapper 中显式重配 ADC，让模块以自己的工作方式运行。

这也是向老师汇报时一个很加分的点：

> 我没有强依赖 CubeMX 默认的 ADC 工作模式，而是在应用层重新封装成更适合业务逻辑的单通道采样接口，从而保证模块行为确定、接口统一。

---

## 7. 开机硬件自适应调平

对应函数：

```c
bool SmartBtn_HardwareCalibration(SmartBtn_t *btn)
```

### 7.1 为什么必须调平

由于：

```text
VOUT = 100 × (Vsensor - VDAC)
```

如果 `VDAC` 与 `Vsensor` 差得太多，即使差值很小，经过 `100` 倍放大后也可能让 ADC 直接接近：

- `0`
- 或 `4095`

一旦饱和，就丢失了线性信息，后续再怎么滤波都没意义。

### 7.2 目标

让 ADC 读数尽量接近：

```c
SMART_BUTTON_ADC_TARGET = 2048
```

也就是让系统工作在中点附近。

### 7.3 采用的方法：二分逼近

代码思路：

```text
low = 0
high = 4095
mid = (low + high) / 2
```

然后循环：

1. 输出 `DAC = mid`
2. 等待硬件稳定
3. 读取 ADC
4. 判断 ADC 相对 2048 的方向

根据公式方向：

- 若 `ADC > 2048`
  - 说明 `Vsensor - VDAC` 偏大
  - 说明 `VDAC` 需要增大
  - 因此 `low = mid + 1`

- 若 `ADC < 2048`
  - 说明 `VDAC` 偏大或差值偏小
  - 需要减小 `VDAC`
  - 因此 `high = mid - 1`

### 7.4 为什么二分法合适

优点：

- 收敛快
- 实现简单
- 不依赖复杂数学模型
- 对单调关系系统非常有效

由于这里默认“DAC 增大，ADC 值总体下降”呈单调趋势，所以二分法是合理的。

### 7.5 成功后做了什么

成功命中死区后：

- 保存 `btn->dac_value`
- 状态切回 `IDLE`
- 将 `dynamic_baseline` 初始化为当前 ADC
- 将 `press_threshold` 初始化为 `300`

这保证系统从“硬件校准态”平滑进入“运行判定态”。

---

## 8. 动态信号处理与慢基线跟踪

对应函数：

```c
void SmartBtn_Update_And_Process(SmartBtn_t *btn)
```

这个函数每次在主循环中被调用一次，是运行态的核心。

它可以拆成三步理解。

### 8.1 第一步：采样与一阶低通滤波

```c
btn->adc_filtered = (adc_raw + btn->adc_filtered * 3) >> 2;
```

这相当于：

```text
新滤波值 = 25% 新数据 + 75% 旧数据
```

作用：

- 去掉瞬时毛刺
- 保留按压趋势
- 算法成本非常低，适合 MCU

为什么不用更复杂的滤波：

- 手机侧键最重要的是实时性和稳定性
- 一阶低通已经能在低成本下明显改善噪声
- 更复杂滤波虽然可能更“平滑”，但计算更重、参数更难调

### 8.2 第二步：动态基线

`dynamic_baseline` 的作用是记录：

**当前“未按下状态”下系统认为的正常电平。**

在 `IDLE` 状态中，代码每 `100` 次循环只允许基线移动 `1`：

```c
if (btn->baseline_track_counter >= SMART_BUTTON_TRACK_PERIOD)
{
    btn->baseline_track_counter = 0U;
    if (btn->adc_filtered > btn->dynamic_baseline) btn->dynamic_baseline++;
    else if (btn->adc_filtered < btn->dynamic_baseline) btn->dynamic_baseline--;
}
```

这是一种 **极慢跟踪**。

#### 为什么要慢

因为你希望它吸收的是：

- 温漂
- 器件缓慢变化
- 长期静态偏置变化

而不是吸收“按压本身”。

如果基线移动太快，用户按下后，基线会跟着往上跑，最终按压波形会被抵消掉，按键就识别不出来了。

#### 为什么按下时必须冻结

一旦进入 `PRESSED` 状态，这份代码完全不更新基线。  
这是一个非常正确的设计。

因为按下过程本身就是你要识别的有效信号，绝不能把它当成漂移去吞掉。

---

## 9. 状态机设计

### 9.1 IDLE 状态

空闲状态下做两件事：

1. 慢基线跟踪
2. 检测是否按下

判定按下条件：

```c
if (btn->adc_filtered > (btn->dynamic_baseline + btn->press_threshold))
```

解释：

- 当前值比动态基线高出明显一截
- 说明这不是噪声，也不是慢漂移，更像真实按压

于是：

- 状态切换到 `PRESSED`
- 打印“按键X按下”

### 9.2 PRESSED 状态

按下状态下：

- 冻结基线跟踪
- 只判断是否释放

释放条件：

```c
release_threshold = dynamic_baseline + press_threshold / 2;
if (adc_filtered < release_threshold)
```

这叫 **迟滞**。

### 9.3 什么是迟滞

如果按下阈值和释放阈值完全一样，那么在阈值附近波动时会反复出现：

- 按下
- 释放
- 按下
- 释放

导致抖动。

所以这里采用：

- 按下阈值：`baseline + threshold`
- 释放阈值：`baseline + threshold/2`

释放门槛更低，就形成了一定回差，提高稳定性。

这点非常适合汇报时强调：

> 我在状态机中引入了迟滞机制，避免按压边缘信号在阈值附近来回抖动，提高了系统鲁棒性。

---

## 10. 系统级 API 的意义

### 10.1 SmartBtn_System_Init

```c
void SmartBtn_System_Init(void)
```

它做了两类事：

1. 初始化 `Btn1` 和 `Btn2`
2. 分别执行硬件校准

这意味着应用层不需要知道：

- 每个按钮用哪个通道
- 初始阈值设多少
- 先调 DAC 再读 ADC 的细节

### 10.2 SmartBtn_System_Tick

```c
void SmartBtn_System_Tick(void)
```

它依次执行：

- `SmartBtn_Update_And_Process(&Btn1);`
- `SmartBtn_Update_And_Process(&Btn2);`

这样主循环里只要放一行代码即可。

这是非常典型的 **模块化任务轮询设计**。

---

## 11. main.c 如何运行这套系统

当前 [Core/Src/main.c](/e:/shishenggongchuang/banbenfinal/Core/Src/main.c) 的主流程可以概括成：

### 11.1 上电启动

```c
HAL_Init();
SystemClock_Config();
```

完成 HAL 初始化和系统时钟配置。

### 11.2 外设初始化

```c
MX_GPIO_Init();
MX_ADC1_Init();
MX_DAC1_Init();
MX_I2C1_Init();
MX_I2C2_Init();
MX_USART2_UART_Init();
```

这里和按键直接相关的是：

- `ADC1`
- `DAC1`
- `USART2`

### 11.3 按键系统初始化

```c
SmartBtn_System_Init();
```

这个阶段会为两个按键做硬件校准。

### 11.4 主循环轮询

```c
while (1)
{
    SmartBtn_System_Tick();
}
```

系统不停轮询两个通道：

1. 采样
2. 滤波
3. 跟踪基线
4. 判断按下或释放
5. 打印事件

这是一种 **前后台、裸机轮询式架构**。

### 11.5 程序运行时序图

可以这样理解整个运行过程：

```text
上电
 -> HAL 初始化
 -> ADC/DAC/UART 初始化
 -> Btn1 校准 DAC
 -> Btn2 校准 DAC
 -> 进入 while(1)
 -> 通道1采样/滤波/判断
 -> 通道2采样/滤波/判断
 -> 重复执行
```

---

## 12. CMake 构建方法

## 12.1 工程构建文件

关键文件有两个：

- [CMakePresets.json](/e:/shishenggongchuang/banbenfinal/CMakePresets.json)
- [cmake/stm32cubemx/CMakeLists.txt](/e:/shishenggongchuang/banbenfinal/cmake/stm32cubemx/CMakeLists.txt)

### 12.2 `CMakePresets.json` 的作用

它定义了构建预设：

- `Debug`
- `Release`

并指定：

- 生成器：`Ninja`
- 工具链文件：`cmake/gcc-arm-none-eabi.cmake`

说明这个工程希望使用 **ARM GCC 交叉编译工具链** 来构建 STM32 固件。

### 12.3 `cmake/stm32cubemx/CMakeLists.txt` 的作用

这个文件把 CubeMX 生成的源码和 HAL 驱动源码加入工程。

你新增的模块已经被加入：

```cmake
${CMAKE_CURRENT_SOURCE_DIR}/../../Core/Src/smart_button.c
```

如果不把它加进去，虽然文件存在，编译器也不会真正编译它。

### 12.4 标准构建命令

如果本机已安装：

- `cmake`
- `ninja`
- `arm-none-eabi-gcc`

可以在工程根目录执行：

```bash
cmake --preset Debug
cmake --build --preset Debug
```

如果想编译发布版：

```bash
cmake --preset Release
cmake --build --preset Release
```

### 12.5 构建产物

通常会在类似目录生成：

```text
build/Debug/
```

并输出：

- `.elf`
- `.map`
- 可能还有 `.bin` 或 `.hex`（取决于后续工具链脚本）

### 12.6 下载运行

编译完成后，一般通过以下方式下载到板子：

- STM32CubeProgrammer
- ST-Link
- IDE 内部下载按钮

下载到单片机后，上电运行，即进入前面描述的流程。

---

## 13. 你为什么选择这种实现方法

这部分是向老师汇报时最重要的“设计理由”。

你可以从以下几个层面回答。

### 13.1 从硬件特性出发

由于前端采用：

- 柔性压力传感器
- 差分减法放大器
- 100 倍增益

所以系统天生对偏置敏感。  
固定参考值无法保证不同样机、不同温度、不同装配应力下都工作在线性区。

因此必须引入：

- `DAC` 自适应调平

### 13.2 从可靠性出发

柔性压力传感器是模拟器件，不像机械微动开关那样天然只有“0/1”。

模拟量一定会带来：

- 噪声
- 温漂
- 个体离散性

因此你选择：

- 平均采样
- 低通滤波
- 动态基线
- 状态机
- 迟滞

这是一整套工程化的抗干扰思路。

### 13.3 从软件架构出发

你没有把算法直接堆在 `main.c` 里，而是做成独立模块。  
这说明你的目标不只是“能跑”，而是“可维护、可扩展、可复用”。

### 13.4 从扩展性出发

当前系统是双通道，但这套设计天然可以扩展到：

- 更多按键
- 长按/短按/双击
- 压力等级判断
- 自学习阈值

也就是说，这不是一次性脚本，而是可继续演进的架构。

---

## 14. 手机侧键常见实现方案有哪些

老师很可能会问：  
“为什么不用更传统的方法？”

你可以从以下角度回答。

### 14.1 机械微动开关

最传统、最常见。

特点：

- 成本低
- 软件简单
- 信号是数字量，容易处理

缺点：

- 有机械寿命限制
- 有按压噪音
- 难以做超薄化、无孔化、密封化设计

### 14.2 霍尔侧键

通过磁体和霍尔传感器识别位移或按压。

优点：

- 非接触
- 寿命高
- 一致性较好

缺点：

- 结构复杂
- 需要磁体
- 对磁路和装配要求高
- 成本较高

### 14.3 电容式侧键

通过手指接近或触摸导致电容变化。

优点：

- 可做无机械结构
- 外观一体化较好

缺点：

- 误触控制难
- 戴手套、湿手、环境变化会影响体验
- 对抗干扰设计要求高

### 14.4 应变片/压阻式/FSR 柔性压力方案

也就是你现在这一类。

优点：

- 可以做固态侧键
- 结构灵活、易贴合曲面
- 可以感知“压力”，不仅是简单触发
- 更利于静音、防水、防尘、无孔化设计

缺点：

- 模拟信号处理复杂
- 温漂明显
- 个体一致性与装配依赖较强

---

## 15. 你这种方案的好处

这部分可以直接作为汇报亮点。

### 15.1 更接近手机高端设计趋势

相比传统机械按键，固态侧键更适合：

- 一体化外观
- 更高防水等级
- 减少机械磨损
- 减少按压噪音

### 15.2 具备压力感知潜力

机械开关通常只有：

- 按下
- 松开

而柔性压力方案理论上还可以进一步扩展为：

- 轻按
- 重按
- 长按压力曲线

这对交互设计很有想象空间。

### 15.3 软件可补偿硬件不一致

这是你方案很有工程价值的一点。

通过：

- DAC 开机自调平
- 运行中慢基线跟踪

系统可以在一定程度上自动适应：

- 传感器个体差异
- 环境温度变化
- 装配偏置变化

这说明你不是单纯“依赖硬件精度”，而是通过 **软硬协同** 提高整体鲁棒性。

### 15.4 模块化程度高

你把问题分成：

1. 硬件映射
2. 自动调平
3. 运行态滤波
4. 动态基线
5. 状态机
6. 系统级封装

这种设计思路在工程评审里是很加分的。

---

## 16. 这份代码目前的亮点

### 16.1 亮点一：问题抓得准

最关键的问题其实不是“怎么判断按下”，而是：

**在 100 倍增益前提下，如何先把系统拉回可工作的线性区。**

你通过 `DAC + ADC` 闭环调平准确解决了这个核心矛盾。

### 16.2 亮点二：算法轻量化

你用的都是非常适合 MCU 的低成本方法：

- 平均采样
- 一阶低通
- 计数式慢跟踪
- 简单双阈值状态机

这些方法的优点是：

- 资源开销小
- 易调试
- 易解释
- 工程上靠谱

### 16.3 亮点三：接口简洁

应用层只需要：

```c
SmartBtn_System_Init();
SmartBtn_System_Tick();
```

这非常适合作为可复用组件。

---

## 17. 这份代码目前的不足与后续可优化方向

汇报时如果你能主动指出改进空间，老师通常会觉得你理解更深。

### 17.1 字符串编码问题

当前串口打印内容在源码里显示为：

```c
printf("鎸夐敭%u鎸変笅\r\n", btn->channel);
printf("鎸夐敭%u閲婃斁\r\n", btn->channel);
```

这明显是中文编码不一致导致的乱码。  
本意应当是“按键X按下”“按键X释放”。

这是一个显示层问题，不影响核心算法，但最好修正成 UTF-8 或直接改成英文。

### 17.2 `main.c` 当前已经接入模块

最初设计目标是不改 `main.c`，但当前实际文件中已经加入了：

```c
#include "smart_button.h"
SmartBtn_System_Init();
SmartBtn_System_Tick();
```

这本身没有问题，只是汇报时要统一表述：

- 模块本身是独立的
- 最终系统接入时仍然需要在 `main.c` 中调用模块接口

### 17.3 ADC 每次采样都重新初始化

`HW_ReadADC_Avg()` 每次都 `HAL_ADC_Init()` 并重新配置通道，逻辑最清晰，但效率不是最高。

后续可以优化成：

- 初始化时配置好规则
- 运行中只切换必要通道，或使用 scan + DMA

### 17.4 缺少更丰富事件

当前只有：

- 按下
- 释放

可以继续扩展：

- 短按
- 长按
- 双击
- 连按
- 压力等级

### 17.5 缺少故障恢复策略

如果校准失败，目前只是进入 `ERROR`。  
后续可增加：

- 重试机制
- 超时上报
- 使用上次保存的 DAC 值

---

## 18. 向老师汇报时可以这样概括你的方案

你可以用下面这段话作为口头总结：

> 我的项目是基于 STM32G071 的双通道固态侧键识别系统。硬件采用柔性压力传感器和差分减法放大器，前端满足 `VOUT = 100 × (Vsensor - VDAC)`。由于放大倍数高，系统对静态偏置很敏感，所以我没有直接用固定阈值，而是让 MCU 利用 DAC 输出参考基准，在开机时通过 ADC 反馈做闭环二分调平，把工作点拉到 ADC 中点附近。运行过程中，再通过平均采样、一阶低通滤波、动态慢基线跟踪和带迟滞的状态机来识别按下和释放。这样可以同时解决饱和、噪声、温漂和抖动问题，并且整个按键逻辑被封装成独立模块，主函数只需要调用初始化和轮询接口即可。

---

## 19. 老师可能会问的问题与回答思路

### 19.1 为什么目标值选 2048，不选 0 或 4095 附近？

回答思路：

- 2048 是 12 位 ADC 中点
- 往上和往下都有足够动态范围
- 能避免一开始就靠近饱和区
- 对后续压力上升和漂移都更安全

### 19.2 为什么用 DAC，不直接固定一个电阻分压参考？

回答思路：

- 固定分压难以适应不同传感器、不同温度和装配应力
- DAC 可调，能做开机闭环校准
- 软件可补偿硬件离散性，更灵活

### 19.3 为什么要动态基线？

回答思路：

- 因为柔性压力传感器和模拟前端存在慢漂移
- 仅靠开机一次校准不够
- 动态基线可以吸收长期温漂，但又不会吞掉瞬时按压

### 19.4 为什么按下时要冻结基线？

回答思路：

- 按下本身就是有效信号
- 如果继续跟踪，会把按压抬升误认为漂移并抵消掉
- 所以按下态必须冻结

### 19.5 为什么释放阈值比按下阈值低？

回答思路：

- 这是迟滞设计
- 防止阈值附近来回跳变
- 提升按键稳定性

### 19.6 为什么用轮询而不是中断？

回答思路：

- 这是模拟量判断，不是简单 GPIO 电平翻转
- 每次都需要采样、滤波、比较和状态判断
- 轮询实现更直接，便于调试和调参
- 后续若性能要求提高，可改成定时器触发 ADC

---

## 20. 一句话总结这个项目的工程价值

这份代码的核心价值不只是“实现了一个按键”，而是：

**针对高增益模拟柔性压力侧键，建立了一套从硬件工作点校准到运行时漂移补偿，再到事件状态机识别的完整软件闭环方案。**

如果老师追问更深一层，你可以强调：

- 这是一种软硬协同设计
- 重点解决的是模拟侧键工程落地中的一致性和鲁棒性问题
- 相比单纯阈值判断，更贴近真实产品开发思路

