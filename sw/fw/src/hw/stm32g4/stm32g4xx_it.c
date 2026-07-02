/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    stm32g4xx_it.c
  * @brief   Interrupt Service Routines.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "stm32g4xx_it.h"
/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "HW_DMA.h"   // HW_DMA_irqHandler + HW_DMA_CHANNEL_* (DMA IRQ dispatch)
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN TD */

/* USER CODE END TD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/* External variables --------------------------------------------------------*/
extern DAC_HandleTypeDef hdac1;
extern TIM_HandleTypeDef htim6;

/* USER CODE BEGIN EV */

/* USER CODE END EV */

/******************************************************************************/
/*           Cortex-M4 Processor Interruption and Exception Handlers          */
/******************************************************************************/
/**
  * @brief This function handles Non maskable interrupt.
  */
void NMI_Handler(void)
{
  /* USER CODE BEGIN NonMaskableInt_IRQn 0 */

  /* USER CODE END NonMaskableInt_IRQn 0 */
  /* USER CODE BEGIN NonMaskableInt_IRQn 1 */
   while (1)
  {
  }
  /* USER CODE END NonMaskableInt_IRQn 1 */
}

/**
  * @brief This function handles Hard fault interrupt.
  */
void HardFault_Handler(void)
{
  /* USER CODE BEGIN HardFault_IRQn 0 */

  /* USER CODE END HardFault_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_HardFault_IRQn 0 */
    /* USER CODE END W1_HardFault_IRQn 0 */
  }
}

/**
  * @brief This function handles Memory management fault.
  */
void MemManage_Handler(void)
{
  /* USER CODE BEGIN MemoryManagement_IRQn 0 */

  /* USER CODE END MemoryManagement_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_MemoryManagement_IRQn 0 */
    /* USER CODE END W1_MemoryManagement_IRQn 0 */
  }
}

/**
  * @brief This function handles Prefetch fault, memory access fault.
  */
void BusFault_Handler(void)
{
  /* USER CODE BEGIN BusFault_IRQn 0 */

  /* USER CODE END BusFault_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_BusFault_IRQn 0 */
    /* USER CODE END W1_BusFault_IRQn 0 */
  }
}

/**
  * @brief This function handles Undefined instruction or illegal state.
  */
void UsageFault_Handler(void)
{
  /* USER CODE BEGIN UsageFault_IRQn 0 */

  /* USER CODE END UsageFault_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_UsageFault_IRQn 0 */
    /* USER CODE END W1_UsageFault_IRQn 0 */
  }
}

/**
  * @brief This function handles Debug monitor.
  */
void DebugMon_Handler(void)
{
  /* USER CODE BEGIN DebugMonitor_IRQn 0 */

  /* USER CODE END DebugMonitor_IRQn 0 */
  /* USER CODE BEGIN DebugMonitor_IRQn 1 */

  /* USER CODE END DebugMonitor_IRQn 1 */
}

/******************************************************************************/
/* STM32G4xx Peripheral Interrupt Handlers                                    */
/* Add here the Interrupt Handlers for the used peripherals.                  */
/* For the available peripheral interrupt handler names,                      */
/* please refer to the startup file (startup_stm32g4xx.s).                    */
/******************************************************************************/

/**
  * @brief This function handles TIM6 global interrupt, DAC1 and DAC3 channel underrun error interrupts.
  */
void TIM6_DAC_IRQHandler(void)
{
  /* USER CODE BEGIN TIM6_DAC_IRQn 0 */

  /* USER CODE END TIM6_DAC_IRQn 0 */
  HAL_TIM_IRQHandler(&htim6);
  HAL_DAC_IRQHandler(&hdac1);
  /* USER CODE BEGIN TIM6_DAC_IRQn 1 */

  /* USER CODE END TIM6_DAC_IRQn 1 */
}

/* USER CODE BEGIN 1 */

/**
  * @brief This function handles EXTI lines 5..9 interrupt.
  */
void EXTI9_5_IRQHandler(void)
{
  for (uint16_t line = 5U; line <= 9U; line++)
  {
    HAL_GPIO_EXTI_IRQHandler((uint16_t)(1U << line));
  }
}

/**
  * @brief This function handles EXTI lines 10..15 interrupt.
  */
void EXTI15_10_IRQHandler(void)
{
  for (uint16_t line = 10U; line <= 15U; line++)
  {
    HAL_GPIO_EXTI_IRQHandler((uint16_t)(1U << line));
  }
}

/**
  * @brief This function handles USB low priority interrupt (services TinyUSB).
  *
  * Must live here (force-linked via --whole-archive on fw_hw). A handler placed
  * in io_usb would be dropped by the linker and the weak Default_Handler would
  * win. Forward-declared to avoid pulling tusb.h into this glue file. Note:
  * tusb.h's tud_int_handler is only a macro alias for the real exported symbol
  * dcd_int_handler, so we call dcd_int_handler directly here.
  */
extern void dcd_int_handler(unsigned char rhport);
void USB_LP_IRQHandler(void)
{
  dcd_int_handler(0);
}

/**
  * @brief This function handles the USB wakeup interrupt (EXTI line 18).
  *
  * The USB peripheral pulses this EXTI line during the host's reset/resume
  * signalling. Without a handler the vector falls through to the infinite-loop
  * Default_Handler and wedges the CPU (starving SysTick). We only clear the
  * EXTI pending bit here — the USB resume itself is serviced via USB_LP.
  */
void USBWakeUp_IRQHandler(void)
{
  EXTI->PR1 = (0x1UL << 18);   /* USB_WAKEUP_EXTI_LINE */
}

/**
  * @brief DMA1 channel 1 (SK6805 LED frame TX) and channel 2 (AS5048 encoder
  * RX) interrupts. Each forwards to HW_DMA, which drives the transfer's
  * completion/error callbacks. The NVIC lines are enabled by HW_DMA_init once
  * the SPI-DMA path is wired (M4); until then these handlers are dormant.
  */
void DMA1_Channel1_IRQHandler(void)
{
  HW_DMA_irqHandler(HW_DMA_CHANNEL_SK6805_TX);
}

void DMA1_Channel2_IRQHandler(void)
{
  HW_DMA_irqHandler(HW_DMA_CHANNEL_AS5048_RX);
}

/* USER CODE END 1 */
