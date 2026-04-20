

/* Includes */
#include "HW_systemClock.h"
#include "stm32g4xx_hal.h"

/* Defines */

/* Typedefs */

/* Private Function Declarations */

/* Private Data Definitions */

/* Private Function Definitions */

/* Public Function Definitions */
bool HW_systemClock_init(const HW_systemClock_config_S * const config)
{
    bool success = true;
    /** Configure the main internal regulator output voltage
     */
    HAL_PWREx_ControlVoltageScaling(config->PWREx_ControlVoltageScaling);

    /** Initializes the RCC Oscillators according to the specified parameters
     * in the RCC_OscInitTypeDef structure.
     */
    if (HAL_RCC_OscConfig(&config->RCC_OscInitStruct) != HAL_OK)
    {
        success = false;
    }

    /** Initializes the CPU, AHB and APB buses clocks
     */
    if (HAL_RCC_ClockConfig(&config->RCC_ClkInitStruct, config->FLASH_latency) != HAL_OK)
    {
        success = false;
    }

    return success;
}
