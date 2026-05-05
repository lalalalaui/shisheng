#include "smart_button.h"

#include <stdio.h>

#include "gpio.h"

#define SMART_BUTTON_UART_TIMEOUT_MS 100U
#define SMART_BUTTON_ADC_TIMEOUT_MS  10U
#define SMART_BUTTON_ADC_AVG_COUNT   8U
#define SMART_BUTTON_CAL_MAX_ITER    16U
#define SMART_BUTTON_DAC_SETTLE_MS   2U
#define SMART_BUTTON_FILTER_SHIFT    2U
#define SMART_BUTTON_TRACK_PERIOD    100U
#define SMART_BUTTON_GAIN_MAX        255U
#define SMART_BUTTON_SAT_ADC_TH      3900U
#define SMART_BUTTON_WEAK_PEAK_TH    400U
#define SMART_BUTTON_GAIN_DOWN_NUM   3U
#define SMART_BUTTON_GAIN_DOWN_DEN   5U
#define SMART_BUTTON_GAIN_UP_NUM     3U
#define SMART_BUTTON_GAIN_UP_DEN     2U

#define MCP42100_CS_GPIO_PORT        GPIOB
#define MCP42100_CS_GPIO_PIN         GPIO_PIN_3
#define MCP42100_SCK_GPIO_PORT       GPIOB
#define MCP42100_SCK_GPIO_PIN        GPIO_PIN_4
#define MCP42100_MOSI_GPIO_PORT      GPIOB
#define MCP42100_MOSI_GPIO_PIN       GPIO_PIN_5

#define MCP42100_CMD_WRITE_POT0      0x11U
#define MCP42100_CMD_WRITE_POT1      0x12U

SmartBtn_t Btn1;
SmartBtn_t Btn2;

static void HW_GainGPIO_Init(void);
static void HW_GainSPI_WriteByte(uint8_t data);
static uint8_t HW_GetDACChannel(uint8_t ch, uint32_t *dac_channel);
static uint8_t HW_GetADCChannel(uint8_t ch, uint32_t *adc_channel);
static void HW_SetDAC(uint8_t ch, uint32_t value_12bit);
static uint16_t HW_ReadADC_Avg(uint8_t ch, uint8_t count);
static void SmartBtn_VofaStream(void);
static void SmartBtn_ResetPressTrace(SmartBtn_t *btn);
static void SmartBtn_ApplyGainAndRecalibrate(SmartBtn_t *btn, uint8_t new_gain_val);

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

void HW_SetGain(uint8_t gain_value)
{
  HW_GainGPIO_Init();

  HAL_GPIO_WritePin(MCP42100_CS_GPIO_PORT, MCP42100_CS_GPIO_PIN, GPIO_PIN_RESET);
  HW_GainSPI_WriteByte(MCP42100_CMD_WRITE_POT0);
  HW_GainSPI_WriteByte(gain_value);
  HAL_GPIO_WritePin(MCP42100_CS_GPIO_PORT, MCP42100_CS_GPIO_PIN, GPIO_PIN_SET);

  HAL_GPIO_WritePin(MCP42100_CS_GPIO_PORT, MCP42100_CS_GPIO_PIN, GPIO_PIN_RESET);
  HW_GainSPI_WriteByte(MCP42100_CMD_WRITE_POT1);
  HW_GainSPI_WriteByte(gain_value);
  HAL_GPIO_WritePin(MCP42100_CS_GPIO_PORT, MCP42100_CS_GPIO_PIN, GPIO_PIN_SET);
}

static void HW_GainGPIO_Init(void)
{
  static uint8_t is_initialized = 0U;
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  if (is_initialized != 0U)
  {
    return;
  }

  __HAL_RCC_GPIOB_CLK_ENABLE();

  GPIO_InitStruct.Pin = MCP42100_CS_GPIO_PIN | MCP42100_SCK_GPIO_PIN | MCP42100_MOSI_GPIO_PIN;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  HAL_GPIO_WritePin(MCP42100_CS_GPIO_PORT, MCP42100_CS_GPIO_PIN, GPIO_PIN_SET);
  HAL_GPIO_WritePin(MCP42100_SCK_GPIO_PORT, MCP42100_SCK_GPIO_PIN, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(MCP42100_MOSI_GPIO_PORT, MCP42100_MOSI_GPIO_PIN, GPIO_PIN_RESET);

  is_initialized = 1U;
}

static void HW_GainSPI_WriteByte(uint8_t data)
{
  uint8_t bit_index;

  for (bit_index = 0U; bit_index < 8U; bit_index++)
  {
    if ((data & 0x80U) != 0U)
    {
      HAL_GPIO_WritePin(MCP42100_MOSI_GPIO_PORT, MCP42100_MOSI_GPIO_PIN, GPIO_PIN_SET);
    }
    else
    {
      HAL_GPIO_WritePin(MCP42100_MOSI_GPIO_PORT, MCP42100_MOSI_GPIO_PIN, GPIO_PIN_RESET);
    }

    HAL_GPIO_WritePin(MCP42100_SCK_GPIO_PORT, MCP42100_SCK_GPIO_PIN, GPIO_PIN_SET);
    __NOP();
    HAL_GPIO_WritePin(MCP42100_SCK_GPIO_PORT, MCP42100_SCK_GPIO_PIN, GPIO_PIN_RESET);
    __NOP();

    data <<= 1;
  }
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

  if (HAL_DAC_SetValue(&hdac1, dac_channel, DAC_ALIGN_12B_R, value_12bit) != HAL_OK)
  {
    Error_Handler();
  }

  if (HAL_DAC_Start(&hdac1, dac_channel) != HAL_OK)
  {
    Error_Handler();
  }
}

static uint16_t HW_ReadADC_Avg(uint8_t ch, uint8_t count)
{
  ADC_ChannelConfTypeDef sConfig = {0};
  uint32_t adc_channel = 0U;
  uint32_t sum = 0U;
  uint8_t i;

  if ((count == 0U) || (HW_GetADCChannel(ch, &adc_channel) == 0U))
  {
    return 0U;
  }

  (void)HAL_ADC_Stop(&hadc1);

  hadc1.Init.ScanConvMode = ADC_SCAN_DISABLE;
  hadc1.Init.ContinuousConvMode = DISABLE;
  hadc1.Init.NbrOfConversion = 1;
  hadc1.Init.EOCSelection = ADC_EOC_SINGLE_CONV;

  if (HAL_ADC_Init(&hadc1) != HAL_OK)
  {
    Error_Handler();
  }

  sConfig.Channel = adc_channel;
  sConfig.Rank = ADC_REGULAR_RANK_1;
  sConfig.SamplingTime = ADC_SAMPLINGTIME_COMMON_1;

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
    }

    sum += HAL_ADC_GetValue(&hadc1);

    if (HAL_ADC_Stop(&hadc1) != HAL_OK)
    {
      Error_Handler();
    }
  }

  return (uint16_t)(sum / count);
}

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

static void SmartBtn_ResetPressTrace(SmartBtn_t *btn)
{
  btn->last_peak_adc = 0U;
  btn->saturation_flag = 0U;
}

static void SmartBtn_ApplyGainAndRecalibrate(SmartBtn_t *btn, uint8_t new_gain_val)
{
  if (new_gain_val == btn->current_gain_val)
  {
    SmartBtn_ResetPressTrace(btn);
    return;
  }

  Btn1.current_gain_val = new_gain_val;
  Btn2.current_gain_val = new_gain_val;
  HW_SetGain(new_gain_val);
  (void)SmartBtn_HardwareCalibration(btn);
  if (btn != &Btn1)
  {
    (void)SmartBtn_HardwareCalibration(&Btn1);
  }
  if (btn != &Btn2)
  {
    (void)SmartBtn_HardwareCalibration(&Btn2);
  }
  SmartBtn_ResetPressTrace(&Btn1);
  SmartBtn_ResetPressTrace(&Btn2);
  SmartBtn_ResetPressTrace(btn);
}

bool SmartBtn_HardwareCalibration(SmartBtn_t *btn)
{
  uint32_t low = 0U;
  uint32_t high = SMART_BUTTON_DAC_MAX_VALUE;
  uint32_t mid = 0U;
  uint16_t adc_value = 0U;
  uint8_t iter;

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
      SmartBtn_ResetPressTrace(btn);
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
    SmartBtn_ResetPressTrace(btn);
    return true;
  }

  btn->state = SMART_BUTTON_STATE_ERROR;
  return false;
}

void SmartBtn_Update_And_Process(SmartBtn_t *btn)
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
  }

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
      btn->last_peak_adc = btn->adc_filtered;
      btn->saturation_flag = (btn->adc_filtered > SMART_BUTTON_SAT_ADC_TH) ? 1U : 0U;
    }

    return;
  }

  if (btn->state == SMART_BUTTON_STATE_PRESSED)
  {
    if (btn->adc_filtered > btn->last_peak_adc)
    {
      btn->last_peak_adc = btn->adc_filtered;
    }

    if (btn->adc_filtered > SMART_BUTTON_SAT_ADC_TH)
    {
      btn->saturation_flag = 1U;
    }

    release_threshold = (uint16_t)(btn->dynamic_baseline + (btn->press_threshold >> 1));

    if (btn->adc_filtered < release_threshold)
    {
      uint8_t new_gain_val = btn->current_gain_val;
      uint16_t peak_delta = 0U;

      btn->state = SMART_BUTTON_STATE_IDLE;
      btn->baseline_track_counter = 0U;

      if (btn->last_peak_adc > btn->dynamic_baseline)
      {
        peak_delta = (uint16_t)(btn->last_peak_adc - btn->dynamic_baseline);
      }

      if (btn->saturation_flag != 0U)
      {
        new_gain_val = (uint8_t)(((uint16_t)btn->current_gain_val * SMART_BUTTON_GAIN_DOWN_NUM) /
                                 SMART_BUTTON_GAIN_DOWN_DEN);
      }
      else if (peak_delta < SMART_BUTTON_WEAK_PEAK_TH)
      {
        uint16_t gain_up = ((uint16_t)btn->current_gain_val * SMART_BUTTON_GAIN_UP_NUM) /
                           SMART_BUTTON_GAIN_UP_DEN;

        if (gain_up > SMART_BUTTON_GAIN_MAX)
        {
          gain_up = SMART_BUTTON_GAIN_MAX;
        }

        new_gain_val = (uint8_t)gain_up;
      }

      SmartBtn_ApplyGainAndRecalibrate(btn, new_gain_val);
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
  Btn1.current_gain_val = SMART_BUTTON_GAIN_MAX;
  Btn1.last_peak_adc = 0U;
  Btn1.saturation_flag = 0U;

  Btn2.channel = SMART_BUTTON_CH2;
  Btn2.state = SMART_BUTTON_STATE_IDLE;
  Btn2.dac_value = SMART_BUTTON_DAC_MAX_VALUE / 2U;
  Btn2.adc_filtered = 0U;
  Btn2.dynamic_baseline = 0U;
  Btn2.press_threshold = 300U;
  Btn2.baseline_track_counter = 0U;
  Btn2.current_gain_val = SMART_BUTTON_GAIN_MAX;
  Btn2.last_peak_adc = 0U;
  Btn2.saturation_flag = 0U;

  HW_SetGain(SMART_BUTTON_GAIN_MAX);

  (void)SmartBtn_HardwareCalibration(&Btn1);
  (void)SmartBtn_HardwareCalibration(&Btn2);
}

void SmartBtn_System_Tick(void)
{
  SmartBtn_Update_And_Process(&Btn1);
  SmartBtn_Update_And_Process(&Btn2);
  SmartBtn_VofaStream();
}
