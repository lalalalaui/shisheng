#include "smart_button.h"

#include <stdio.h>

#define SMART_BUTTON_UART_TIMEOUT_MS 100U  //串口超时时间
#define SMART_BUTTON_ADC_TIMEOUT_MS  10U //ADC转换超时时间
#define SMART_BUTTON_ADC_AVG_COUNT   8U //ADC平均采样次数，必须为2的幂次，以便移位运算
#define SMART_BUTTON_CAL_MAX_ITER    16U  //硬件校准最大迭代次数，过多可能导致校准时间过长
#define SMART_BUTTON_DAC_SETTLE_MS   2U //DAC设置后等待时间，确保输出稳定
#define SMART_BUTTON_FILTER_SHIFT    2U //滤波器移位，相当于除以4，数值越大滤波越平滑但响应越慢
#define SMART_BUTTON_TRACK_PERIOD    100U //基线跟踪周期，单位为系统Tick次数，过长可能导致基线跟踪不及时，过短可能导致基线过于敏感

SmartBtn_t Btn1;  //头函数声明，这里实际创建。
SmartBtn_t Btn2;  // 目前支持两个按键，分别连接到不同的ADC通道和DAC通道

static uint8_t HW_GetDACChannel(uint8_t ch, uint32_t *dac_channel);
static uint8_t HW_GetADCChannel(uint8_t ch, uint32_t *adc_channel);
static void HW_SetDAC(uint8_t ch, uint32_t value_12bit);
static uint16_t HW_ReadADC_Avg(uint8_t ch, uint8_t count);
static void SmartBtn_VofaStream(void);

int __io_putchar(int ch)
{
  uint8_t data = (uint8_t)ch;

  (void)HAL_UART_Transmit(&huart2, &data, 1U, SMART_BUTTON_UART_TIMEOUT_MS);

  return ch;
} //重定向printf函数到串口，方便调试输出

int fputc(int ch, FILE *stream)
{
  (void)stream;
  return __io_putchar(ch);
}

static uint8_t HW_GetDACChannel(uint8_t ch, uint32_t *dac_channel)
{
  if (ch == SMART_BUTTON_CH1)
  {
    *dac_channel = DAC_CHANNEL_1;
    return 1U;
  }

  if (ch == SMART_BUTTON_CH2)
  {
    *dac_channel = DAC_CHANNEL_2;
    return 1U;
  }

  return 0U;
}

static uint8_t HW_GetADCChannel(uint8_t ch, uint32_t *adc_channel)
{
  if (ch == SMART_BUTTON_CH1)
  {
    *adc_channel = ADC_CHANNEL_0;
    return 1U;
  }

  if (ch == SMART_BUTTON_CH2)
  {
    *adc_channel = ADC_CHANNEL_1;
    return 1U;
  }

  return 0U;
}

static void HW_SetDAC(uint8_t ch, uint32_t value_12bit)
{
  uint32_t dac_channel = 0U;

  if (HW_GetDACChannel(ch, &dac_channel) == 0U)
  {
    return;
  }

  if (value_12bit > SMART_BUTTON_DAC_MAX_VALUE)
  {
    value_12bit = SMART_BUTTON_DAC_MAX_VALUE;
  }

  if (HAL_DAC_SetValue(&hdac1, dac_channel, DAC_ALIGN_12B_R, value_12bit) != HAL_OK)  //设置DAC输出值，12位右对齐
  {
    Error_Handler();
  }

  if (HAL_DAC_Start(&hdac1, dac_channel) != HAL_OK)
  {
    Error_Handler();
  }
}

static uint16_t HW_ReadADC_Avg(uint8_t ch, uint8_t count) //读取ADC值并进行平均，减少噪声影响
{
  ADC_ChannelConfTypeDef sConfig = {0};
  uint32_t adc_channel = 0U;
  uint32_t sum = 0U;
  uint8_t i;

  if ((count == 0U) || (HW_GetADCChannel(ch, &adc_channel) == 0U))
  {
    return 0U;
  } //参数检查，确保采样次数不为0且通道有效

  (void)HAL_ADC_Stop(&hadc1);

  hadc1.Init.ScanConvMode = ADC_SCAN_DISABLE;
  hadc1.Init.ContinuousConvMode = DISABLE;
  hadc1.Init.NbrOfConversion = 1; //单通道单次转换模式，配合软件触发，适合按键扫描
  hadc1.Init.EOCSelection = ADC_EOC_SINGLE_CONV;

  if (HAL_ADC_Init(&hadc1) != HAL_OK)
  {
    Error_Handler();
  }

  sConfig.Channel = adc_channel;
  sConfig.Rank = ADC_REGULAR_RANK_1;//
  sConfig.SamplingTime = ADC_SAMPLINGTIME_COMMON_1; //采样时间设置为较长，确保稳定的ADC读数，特别是在高阻抗源情况下

  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }

  for (i = 0U; i < count; i++)
  {
    if (HAL_ADC_Start(&hadc1) != HAL_OK)
    {
      Error_Handler();
    }

    if (HAL_ADC_PollForConversion(&hadc1, SMART_BUTTON_ADC_TIMEOUT_MS) != HAL_OK)
    {
      Error_Handler();
    } //等待ADC转换完成，超时则认为读取失败

    sum += HAL_ADC_GetValue(&hadc1);

    if (HAL_ADC_Stop(&hadc1) != HAL_OK)
    {
      Error_Handler();
    }
  }

  return (uint16_t)(sum / count);
}//读取ADC值并进行平均，减少噪声影响

static void SmartBtn_VofaStream(void)
{
  static uint32_t last_send_tick = 0U;
  uint32_t now_tick = HAL_GetTick();

  if ((now_tick - last_send_tick) < 20U)
  {
    return;
  }

  last_send_tick = now_tick;

  printf("%u,%u,%u,%u\r\n",
         Btn1.adc_filtered,
         Btn1.dynamic_baseline,
         Btn2.adc_filtered,
         Btn2.dynamic_baseline);
}

bool SmartBtn_HardwareCalibration(SmartBtn_t *btn)
{
  uint32_t low = 0U;
  uint32_t high = SMART_BUTTON_DAC_MAX_VALUE;
  uint32_t mid = 0U;
  uint16_t adc_value = 0U;//adc读到的值
  uint8_t iter;  //循环次数
  if (btn == NULL)
  {
    return false;
  }

  if ((btn->channel != SMART_BUTTON_CH1) && (btn->channel != SMART_BUTTON_CH2))
  {
    btn->state = SMART_BUTTON_STATE_ERROR;
    return false;
  }

  btn->state = SMART_BUTTON_STATE_CALIBRATING;

  for (iter = 0U; iter < SMART_BUTTON_CAL_MAX_ITER; iter++)
  {
    mid = (low + high) / 2U;
    HW_SetDAC(btn->channel, mid);
    HAL_Delay(SMART_BUTTON_DAC_SETTLE_MS);

    adc_value = HW_ReadADC_Avg(btn->channel, SMART_BUTTON_ADC_AVG_COUNT);
    btn->adc_filtered = adc_value;

    if ((adc_value >= (SMART_BUTTON_ADC_TARGET - SMART_BUTTON_ADC_DEADZONE)) &&
        (adc_value <= (SMART_BUTTON_ADC_TARGET + SMART_BUTTON_ADC_DEADZONE)))
    {
      btn->dac_value = mid;
      btn->state = SMART_BUTTON_STATE_IDLE;
      btn->dynamic_baseline = adc_value;
      btn->baseline_track_counter = 0U;
      if (btn->press_threshold == 0U)
      {
        btn->press_threshold = 300U;
      }
      return true;
    }

    if (adc_value > SMART_BUTTON_ADC_TARGET)
    {
      low = mid + 1U;
    }
    else
    {
      if (mid == 0U)
      {
        break;
      }

      high = mid - 1U;
    }

    if (low > high)
    {
      break;
    }
  }

  HW_SetDAC(btn->channel, mid);
  HAL_Delay(SMART_BUTTON_DAC_SETTLE_MS);
  btn->adc_filtered = HW_ReadADC_Avg(btn->channel, SMART_BUTTON_ADC_AVG_COUNT);
  btn->dac_value = mid;

  if ((btn->adc_filtered >= (SMART_BUTTON_ADC_TARGET - SMART_BUTTON_ADC_DEADZONE)) &&
      (btn->adc_filtered <= (SMART_BUTTON_ADC_TARGET + SMART_BUTTON_ADC_DEADZONE)))
  {
    btn->state = SMART_BUTTON_STATE_IDLE;
    btn->dynamic_baseline = btn->adc_filtered;
    btn->baseline_track_counter = 0U;
    if (btn->press_threshold == 0U)
    {
      btn->press_threshold = 300U;
    }
    return true;
  }

  btn->state = SMART_BUTTON_STATE_ERROR;
  return false;
}//硬件校准函数，使用二分法调整DAC输出，使ADC读数接近目标值，确保按键在未按下时有一个稳定的基线

void SmartBtn_Update_And_Process(SmartBtn_t *btn) //更新按键状态并进行处理，包含ADC读数滤波、基线跟踪和按键状态转换逻辑
{
  uint16_t adc_raw;
  uint16_t release_threshold;

  if (btn == NULL)
  {
    return;
  }

  if ((btn->channel != SMART_BUTTON_CH1) && (btn->channel != SMART_BUTTON_CH2))
  {
    btn->state = SMART_BUTTON_STATE_ERROR;
    return;
  }

  if (btn->press_threshold == 0U)
  {
    btn->press_threshold = 300U;
  }

  adc_raw = HW_ReadADC_Avg(btn->channel, SMART_BUTTON_ADC_AVG_COUNT);

  if (btn->adc_filtered == 0U)
  {
    btn->adc_filtered = adc_raw;
  }
  else
  {
    btn->adc_filtered = (uint16_t)((adc_raw + ((uint32_t)btn->adc_filtered * 3U)) >> SMART_BUTTON_FILTER_SHIFT);
  } //简单的IIR滤波器，权重为3:1，平滑ADC读数，减少噪声影响

  if (btn->dynamic_baseline == 0U)
  {
    btn->dynamic_baseline = btn->adc_filtered;
  }

  if (btn->state == SMART_BUTTON_STATE_IDLE)
  {
    btn->baseline_track_counter++;

    if (btn->baseline_track_counter >= SMART_BUTTON_TRACK_PERIOD)
    {
      btn->baseline_track_counter = 0U;

      if (btn->adc_filtered > btn->dynamic_baseline)
      {
        btn->dynamic_baseline++;
      }
      else if (btn->adc_filtered < btn->dynamic_baseline)
      {
        btn->dynamic_baseline--;
      }
    }

    if (btn->adc_filtered > (btn->dynamic_baseline + btn->press_threshold))
    {
      btn->state = SMART_BUTTON_STATE_PRESSED;
    }

    return;
  }

  if (btn->state == SMART_BUTTON_STATE_PRESSED)
  {
    release_threshold = (uint16_t)(btn->dynamic_baseline + (btn->press_threshold >> 1));

    if (btn->adc_filtered < release_threshold)
    {
      btn->state = SMART_BUTTON_STATE_IDLE;
      btn->baseline_track_counter = 0U;
    }

    return;
  }
}

void SmartBtn_System_Init(void)
{
  Btn1.channel = SMART_BUTTON_CH1;
  Btn1.state = SMART_BUTTON_STATE_IDLE;
  Btn1.dac_value = SMART_BUTTON_DAC_MAX_VALUE / 2U;
  Btn1.adc_filtered = 0U;
  Btn1.dynamic_baseline = 0U;
  Btn1.press_threshold = 300U;
  Btn1.baseline_track_counter = 0U;

  Btn2.channel = SMART_BUTTON_CH2;
  Btn2.state = SMART_BUTTON_STATE_IDLE;
  Btn2.dac_value = SMART_BUTTON_DAC_MAX_VALUE / 2U;
  Btn2.adc_filtered = 0U;
  Btn2.dynamic_baseline = 0U;
  Btn2.press_threshold = 300U;
  Btn2.baseline_track_counter = 0U;

  (void)SmartBtn_HardwareCalibration(&Btn1);
  (void)SmartBtn_HardwareCalibration(&Btn2);
}

void SmartBtn_System_Tick(void)
{
  SmartBtn_Update_And_Process(&Btn1);
  SmartBtn_Update_And_Process(&Btn2);
  SmartBtn_VofaStream();
}
