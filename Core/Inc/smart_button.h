#ifndef __SMART_BUTTON_H__
#define __SMART_BUTTON_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

#include "main.h"
#include "adc.h"
#include "dac.h"
#include "usart.h"

#define SMART_BUTTON_CH1             1U
#define SMART_BUTTON_CH2             2U

#define SMART_BUTTON_DAC_MAX_VALUE   4095U
#define SMART_BUTTON_ADC_TARGET      2048U
#define SMART_BUTTON_ADC_DEADZONE    50U

typedef enum
{
  SMART_BUTTON_STATE_IDLE = 0U,
  SMART_BUTTON_STATE_CALIBRATING,
  SMART_BUTTON_STATE_PRESSED,
  SMART_BUTTON_STATE_ERROR
} SmartBtnState_t;

typedef struct
{
  uint8_t channel;
  SmartBtnState_t state;
  uint32_t dac_value;
  uint16_t adc_filtered;
  uint16_t dynamic_baseline;
  uint16_t press_threshold;
  uint16_t baseline_track_counter;
  uint8_t current_gain_val;
  uint16_t last_peak_adc;
  uint8_t saturation_flag;
} SmartBtn_t;

extern SmartBtn_t Btn1;
extern SmartBtn_t Btn2;

void HW_SetGain(uint8_t gain_value);
bool SmartBtn_HardwareCalibration(SmartBtn_t *btn);
void SmartBtn_Update_And_Process(SmartBtn_t *btn);
void SmartBtn_System_Init(void);
void SmartBtn_System_Tick(void);

#ifdef __cplusplus
}
#endif

#endif /* __SMART_BUTTON_H__ */
