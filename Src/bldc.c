/*
* This file implements FOC motor control.
* This control method offers superior performanace
* compared to previous cummutation method. The new method features:
* ► reduced noise and vibrations
* ► smooth torque output
* ► improved motor efficiency -> lower energy consumption
*
* Copyright (C) 2019-2020 Emanuel FERU <aerdronix@gmail.com>
*
* This program is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "stm32f1xx_hal.h"
#include "defines.h"
#include "setup.h"
#include "config.h"
#include "util.h"

// Matlab includes and defines - from auto-code generation
// ###############################################################################
#include "BLDC_controller.h"           /* Model's header file */
#include "rtwtypes.h"

extern RT_MODEL *const rtM_Left;
extern RT_MODEL *const rtM_Right;

extern DW   rtDW_Left;                  /* Observable states */
extern ExtU rtU_Left;                   /* External inputs */
extern ExtY rtY_Left;                   /* External outputs */
extern P    rtP_Left;

extern DW   rtDW_Right;                 /* Observable states */
extern ExtU rtU_Right;                  /* External inputs */
extern ExtY rtY_Right;                  /* External outputs */
// ###############################################################################

static int16_t pwm_margin;              /* This margin allows to have a window in the PWM signal for proper FOC Phase currents measurement */

extern uint8_t ctrlModReq;
static int16_t curDC_max = (I_DC_MAX * A2BIT_CONV);
int16_t curL_phaA = 0, curL_phaB = 0, curL_DC = 0;
int16_t curR_phaB = 0, curR_phaC = 0, curR_DC = 0;

volatile int pwml = 0;
volatile int pwmr = 0;

extern volatile adc_buf_t adc_buffer;

uint8_t buzzerFreq          = 0;
uint8_t buzzerPattern       = 0;
uint8_t buzzerCount         = 0;
volatile uint32_t buzzerTimer = 0;
static uint8_t  buzzerPrev  = 0;
static uint8_t  buzzerIdx   = 0;

uint8_t        enable       = 0;        // initially motors are disabled for SAFETY
static uint8_t enableFin    = 0;

static const uint16_t pwm_res  = 64000000 / 2 / PWM_FREQ; // = 2000

static uint16_t offsetcount = 0;
static int16_t offsetrlA    = 2000;
static int16_t offsetrlB    = 2000;
static int16_t offsetrrB    = 2000;
static int16_t offsetrrC    = 2000;
static int16_t offsetdcl    = 2000;
static int16_t offsetdcr    = 2000;

int16_t        batVoltage       = (400 * BAT_CELLS * BAT_CALIB_ADC) / BAT_CALIB_REAL_VOLTAGE;
static int32_t batVoltageFixdt  = (400 * BAT_CELLS * BAT_CALIB_ADC) / BAT_CALIB_REAL_VOLTAGE << 16;  // Fixed-point filter output initialized at 400 V*100/cell = 4 V/cell converted to fixed-point

uint16_t wheel_left_ticks = 0;
uint16_t wheel_right_ticks = 0;

static uint8_t enc_val_previous_left = 0;
static uint8_t enc_val_previous_right = 0;

int8_t enc_vals_table[6] = { 0, -1, -2, 0, 2, 1 };

uint16_t clamp_module_max(int16_t value, uint16_t max) {
  return ((value % max) + max) % max;
}

int8_t calc_encoder_ticks(uint8_t enc_val_previous, uint8_t enc_val_current) {
  return enc_vals_table[clamp_module_max(enc_val_previous - enc_val_current, 6)];
}

// =================================
// DMA interrupt frequency =~ 16 kHz
// =================================
void DMA1_Channel1_IRQHandler(void) {

  DMA1->IFCR = DMA_IFCR_CTCIF1;
  // HAL_GPIO_WritePin(LED_PORT, LED_PIN, 1);
  // HAL_GPIO_TogglePin(LED_PORT, LED_PIN);

  if(offsetcount < 2000) {  // calibrate ADC offsets
    offsetcount++;
    offsetrlA = (adc_buffer.rlA + offsetrlA) / 2;
    offsetrlB = (adc_buffer.rlB + offsetrlB) / 2;
    offsetrrB = (adc_buffer.rrB + offsetrrB) / 2;
    offsetrrC = (adc_buffer.rrC + offsetrrC) / 2;
    offsetdcl = (adc_buffer.dcl + offsetdcl) / 2;
    offsetdcr = (adc_buffer.dcr + offsetdcr) / 2;
    return;
  }

  if (buzzerTimer % 1000 == 0) {  // Filter battery voltage at a slower sampling rate
    filtLowPass32(adc_buffer.batt1, BAT_FILT_COEF, &batVoltageFixdt);
    batVoltage = (int16_t)(batVoltageFixdt >> 16);  // convert fixed-point to integer
  }

  // Get Left motor currents
  curL_phaA = (int16_t)(offsetrlA - adc_buffer.rlA);
  curL_phaB = (int16_t)(offsetrlB - adc_buffer.rlB);
  curL_DC   = (int16_t)(offsetdcl - adc_buffer.dcl);
  
  // Get Right motor currents
  curR_phaB = (int16_t)(offsetrrB - adc_buffer.rrB);
  curR_phaC = (int16_t)(offsetrrC - adc_buffer.rrC);
  curR_DC   = (int16_t)(offsetdcr - adc_buffer.dcr);

  // Disable PWM when current limit is reached (current chopping)
  // This is the Level 2 of current protection. The Level 1 should kick in first given by I_MOT_MAX
  if(ABS(curL_DC) > curDC_max || enable == 0) {
    LEFT_TIM->BDTR &= ~TIM_BDTR_MOE;
  } else {
    LEFT_TIM->BDTR |= TIM_BDTR_MOE;
  }

  if(ABS(curR_DC)  > curDC_max || enable == 0) {
    RIGHT_TIM->BDTR &= ~TIM_BDTR_MOE;
  } else {
    RIGHT_TIM->BDTR |= TIM_BDTR_MOE;
  }

  // Create square wave for buzzer
  buzzerTimer++;
  if (buzzerFreq != 0 && (buzzerTimer / 5000) % (buzzerPattern + 1) == 0) {
    if (buzzerPrev == 0) {
      buzzerPrev = 1;
      if (++buzzerIdx > (buzzerCount + 2)) {    // pause 2 periods
        buzzerIdx = 1;
      }
    }
    if (buzzerTimer % buzzerFreq == 0 && (buzzerIdx <= buzzerCount || buzzerCount == 0)) {
      HAL_GPIO_TogglePin(BUZZER_PORT, BUZZER_PIN);
    }
  } else if (buzzerPrev) {
      HAL_GPIO_WritePin(BUZZER_PORT, BUZZER_PIN, GPIO_PIN_RESET);
      buzzerPrev = 0;
  }

  // Adjust pwm_margin depending on the selected Control Type
  if (rtP_Left.z_ctrlTypSel == FOC_CTRL) {
    pwm_margin = 110;
  } else {
    pwm_margin = 0;
  }

  // ############################### MOTOR CONTROL ###############################

  int ul, vl, wl;
  int ur, vr, wr;
  static boolean_T OverrunFlag = false;

  /* Check for overrun */
  if (OverrunFlag) {
    return;
  }
  OverrunFlag = true;

  /* Make sure to stop BOTH motors in case of an error */
  enableFin = enable && !rtY_Left.z_errCode && !rtY_Right.z_errCode;
 
  // ========================= LEFT MOTOR ============================ 
    // Get hall sensors values
    uint8_t hall_ul = !(LEFT_HALL_U_PORT->IDR & LEFT_HALL_U_PIN);
    uint8_t hall_vl = !(LEFT_HALL_V_PORT->IDR & LEFT_HALL_V_PIN);
    uint8_t hall_wl = !(LEFT_HALL_W_PORT->IDR & LEFT_HALL_W_PIN);

    /* Set motor inputs here */
    rtU_Left.b_motEna     = enableFin;
    rtU_Left.z_ctrlModReq = ctrlModReq;  
    rtU_Left.r_inpTgt     = pwml;
    rtU_Left.b_hallA      = hall_ul;
    rtU_Left.b_hallB      = hall_vl;
    rtU_Left.b_hallC      = hall_wl;
    rtU_Left.i_phaAB      = curL_phaA;
    rtU_Left.i_phaBC      = curL_phaB;
    rtU_Left.i_DCLink     = curL_DC;
    // rtU_Left.a_mechAngle   = ...; // Angle input in DEGREES [0,360] in fixdt(1,16,4) data type. If `angle` is float use `= (int16_t)floor(angle * 16.0F)` If `angle` is integer use `= (int16_t)(angle << 4)`
    
    /* Step the controller */
    #ifdef MOTOR_LEFT_ENA    
    BLDC_controller_step(rtM_Left);
    #endif

    /* Get motor outputs here */
    ul            = rtY_Left.DC_phaA;
    vl            = rtY_Left.DC_phaB;
    wl            = rtY_Left.DC_phaC;
  // errCodeLeft  = rtY_Left.z_errCode;
  // motSpeedLeft = rtY_Left.n_mot;
  // motAngleLeft = rtY_Left.a_elecAngle;

    // Calc encoder value and find code for hall sensor in table
    uint8_t encoder_left = (uint8_t)((hall_ul << 2) + (hall_vl << 1) + hall_wl); // 0 - 7
    encoder_left = rtConstP.vec_hallToPos_Value[encoder_left]; // 0, 2, 0, 1, 4, 3, 5, 0
    wheel_left_ticks = clamp_module_max(
      wheel_left_ticks + calc_encoder_ticks(enc_val_previous_left, encoder_left), 
      9000 // 100 wheel revolutions
    );
    enc_val_previous_left = encoder_left;

    /* Apply commands */
    LEFT_TIM->LEFT_TIM_U    = (uint16_t)CLAMP(ul + pwm_res / 2, pwm_margin, pwm_res-pwm_margin);
    LEFT_TIM->LEFT_TIM_V    = (uint16_t)CLAMP(vl + pwm_res / 2, pwm_margin, pwm_res-pwm_margin);
    LEFT_TIM->LEFT_TIM_W    = (uint16_t)CLAMP(wl + pwm_res / 2, pwm_margin, pwm_res-pwm_margin);
  // =================================================================
  

  // ========================= RIGHT MOTOR ===========================  
    // Get hall sensors values
    uint8_t hall_ur = !(RIGHT_HALL_U_PORT->IDR & RIGHT_HALL_U_PIN);
    uint8_t hall_vr = !(RIGHT_HALL_V_PORT->IDR & RIGHT_HALL_V_PIN);
    uint8_t hall_wr = !(RIGHT_HALL_W_PORT->IDR & RIGHT_HALL_W_PIN);

    /* Set motor inputs here */
    rtU_Right.b_motEna      = enableFin;
    rtU_Right.z_ctrlModReq  = ctrlModReq;
    rtU_Right.r_inpTgt      = pwmr;
    rtU_Right.b_hallA       = hall_ur;
    rtU_Right.b_hallB       = hall_vr;
    rtU_Right.b_hallC       = hall_wr;
    rtU_Right.i_phaAB       = curR_phaB;
    rtU_Right.i_phaBC       = curR_phaC;
    rtU_Right.i_DCLink      = curR_DC;
    // rtU_Right.a_mechAngle   = ...; // Angle input in DEGREES [0,360] in fixdt(1,16,4) data type. If `angle` is float use `= (int16_t)floor(angle * 16.0F)` If `angle` is integer use `= (int16_t)(angle << 4)`
    
    /* Step the controller */
    #ifdef MOTOR_RIGHT_ENA
    BLDC_controller_step(rtM_Right);
    #endif

    /* Get motor outputs here */
    ur            = rtY_Right.DC_phaA;
    vr            = rtY_Right.DC_phaB;
    wr            = rtY_Right.DC_phaC;
 // errCodeRight  = rtY_Right.z_errCode;
 // motSpeedRight = rtY_Right.n_mot;
 // motAngleRight = rtY_Right.a_elecAngle;

    uint8_t encoder_right = (uint8_t)((hall_ur << 2) + (hall_vr << 1) + hall_wr);
    encoder_right = rtConstP.vec_hallToPos_Value[encoder_right];
    wheel_right_ticks = clamp_module_max(
      wheel_right_ticks - calc_encoder_ticks(enc_val_previous_right, encoder_right), 
      9000 // 100 wheel revolutions
    );
    enc_val_previous_right = encoder_right;

    /* Apply commands */
    RIGHT_TIM->RIGHT_TIM_U  = (uint16_t)CLAMP(ur + pwm_res / 2, pwm_margin, pwm_res-pwm_margin);
    RIGHT_TIM->RIGHT_TIM_V  = (uint16_t)CLAMP(vr + pwm_res / 2, pwm_margin, pwm_res-pwm_margin);
    RIGHT_TIM->RIGHT_TIM_W  = (uint16_t)CLAMP(wr + pwm_res / 2, pwm_margin, pwm_res-pwm_margin);
  // =================================================================

  /* Indicate task complete */
  OverrunFlag = false;
 
 // ###############################################################################

}
