/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
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

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "ota_metadata.h"
#include <string.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
CRC_HandleTypeDef hcrc;

UART_HandleTypeDef huart1;
UART_HandleTypeDef huart2;

/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_USART2_UART_Init(void);
static void MX_CRC_Init(void);
static void MX_USART1_UART_Init(void);
/* USER CODE BEGIN PFP */
static void bl_print(const char *msg);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
typedef void (*app_entry_t)(void);

static app_entry_t app_entry;

static uint8_t metadata_is_valid(const ota_metadata_t *meta) {
	if (meta->magic != METADATA_MAGIC) {
		return 0U;
	}

	if ((meta->active_slot != SLOT_A) && (meta->active_slot != SLOT_B)) {
		return 0U;
	}

	return 1U;
}

static uint32_t slot_base_address(uint32_t slot) {
	return (slot == SLOT_A) ? SLOT_A_BASE_ADDRESS: SLOT_B_BASE_ADDRESS;
}

static uint32_t slot_size(const ota_metadata_t *meta, uint32_t slot) {
	return (slot == SLOT_A) ? meta->slot_a_size : meta->slot_b_size;
}

static uint32_t slot_crc32(const ota_metadata_t *meta, uint32_t slot) {
    return (slot == SLOT_A) ? meta->slot_a_crc32 : meta->slot_b_crc32;
}

static uint8_t slot_is_valid(const ota_metadata_t *meta, uint32_t slot) {
	uint32_t size = slot_size(meta, slot);

	if ((size == 0U) || (size > SLOT_SIZE)) {
		return 0U;
	}

	// hardware crc works on 32bit words
	if ((size % 4U) != 0U) {
		return 0U;
	}

	uint32_t computed = HAL_CRC_Calculate(&hcrc, (uint32_t *)slot_base_address(slot), size / 4U);

	return (computed == slot_crc32(meta, slot)) ? 1U : 0U;
}

static HAL_StatusTypeDef metadata_write(const ota_metadata_t *new_meta) {
	FLASH_EraseInitTypeDef erase = {0};
	uint32_t sector_error = 0U;
	const uint32_t *src = (const uint32_t *)new_meta;

	if (HAL_FLASH_Unlock() != HAL_OK) {
		return HAL_ERROR;
	}

	erase.TypeErase = FLASH_TYPEERASE_SECTORS;
	erase.Sector = FLASH_SECTOR_7;
	erase.NbSectors = 1U;
	erase.VoltageRange = FLASH_VOLTAGE_RANGE_3;

	if (HAL_FLASHEx_Erase(&erase, &sector_error) != HAL_OK) {
		HAL_FLASH_Lock();
		return HAL_ERROR;
	}

	for (uint32_t i = 0U; i < (sizeof(ota_metadata_t) / 4U); i++) {
		if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, METADATA_BASE_ADDRESS + (i * 4U), src[i]) != HAL_OK) {
			HAL_FLASH_Lock();
			return HAL_ERROR;
		}
	}

	HAL_FLASH_Lock();
	return HAL_OK;
}

static uint32_t slot_sector(uint32_t slot) {
	return (slot == SLOT_A) ? FLASH_SECTOR_5 : FLASH_SECTOR_6;
}

static HAL_StatusTypeDef slot_erase(uint32_t slot) {
	FLASH_EraseInitTypeDef erase = {0};
	uint32_t sector_error = 0U;

	if (HAL_FLASH_Unlock() != HAL_OK) {
		return HAL_ERROR;
	}

	erase.TypeErase = FLASH_TYPEERASE_SECTORS;
	erase.Sector = slot_sector(slot);
	erase.NbSectors = 1U;
	erase.VoltageRange = FLASH_VOLTAGE_RANGE_3;

	HAL_StatusTypeDef status = HAL_FLASHEx_Erase(&erase, &sector_error);

	HAL_FLASH_Lock();
	return status;
}

static HAL_StatusTypeDef slot_write(uint32_t slot, uint32_t offset, const uint8_t *data, uint32_t length) {
	uint32_t base = slot_base_address(slot);

	// bounds check, never write outside the slot
	if ((offset + length) > SLOT_SIZE) {
		return HAL_ERROR;
	}

	// word-aligned writes only
	if (((offset % 4U) != 0U) || ((length % 4U) != 0U)) {
		return HAL_ERROR;
	}

	if (HAL_FLASH_Unlock() != HAL_OK) {
		return HAL_ERROR;
	}

	for (uint32_t i = 0U; i < length; i += 4U) {
		uint32_t word;
		memcpy(&word, &data[i], 4U);

		if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, base + offset + i, word) != HAL_OK) {
			HAL_FLASH_Lock();
			return HAL_ERROR;
		}
	}

	HAL_FLASH_Lock();
	return HAL_OK;
}

#define UPDATE_PROMPT '?'
#define UPDATE_REQUEST 'U'
#define UPDATE_HEADER_MAGIC 0xB2U
#define CHUNK_SIZE 256U
#define PROMPT_TIMEOUT_MS 1000U
#define HEADER_TIMEOUT_MS 2000U
#define CHUNK_TIMEOUT_MS 5000U

typedef struct {
	uint8_t magic;
	uint32_t size;
	uint32_t crc32;
	uint32_t version;
} __attribute__((packed)) update_header_t;

static uint8_t receive_update(uint32_t target_slot) {
	uint8_t reply = 0U;
	uint8_t chunk[CHUNK_SIZE];
	update_header_t header;

	// offer an update window, announcing current version
	const ota_metadata_t *current = (const ota_metadata_t *)METADATA_BASE_ADDRESS;
	uint32_t my_version = (current->active_slot == SLOT_A) ? current->slot_a_version : current->slot_b_version;

	uint8_t prompt_msg[6];
	prompt_msg[0] = UPDATE_PROMPT;
	memcpy(&prompt_msg[1], &my_version, 4U);
	prompt_msg[5] = (uint8_t)target_slot;

	HAL_UART_Transmit(&huart1, prompt_msg, 6U, HAL_MAX_DELAY);

	if (HAL_UART_Receive(&huart1, &reply, 1U, PROMPT_TIMEOUT_MS) != HAL_OK) {
		return 0U; // nobody there
	}

	if (reply != UPDATE_REQUEST) {
		return 0U;
	}

	bl_print("Update offered\r\n");

	if (HAL_UART_Receive(&huart1, (uint8_t *)&header, sizeof(header), HEADER_TIMEOUT_MS) != HAL_OK) {
		bl_print("Header timeout\r\n");
		return 0U;
	}

	// a wrong magic means the stream is out of sync, not that the image is bad
	if (header.magic != UPDATE_HEADER_MAGIC) {
		HAL_UART_Transmit(&huart1, (uint8_t *)"NO", 2U, HAL_MAX_DELAY);
		bl_print("Bad header magic\r\n");
		return 0U;
	}

	if ((header.size == 0U) || (header.size > SLOT_SIZE) || ((header.size % 4U) != 0U)) {
		HAL_UART_Transmit(&huart1, (uint8_t *)"NO", 2U, HAL_MAX_DELAY);
		bl_print("Bad header\r\n");
		return 0U;
	}

	HAL_UART_Transmit(&huart1, (uint8_t *)"OK", 2U, HAL_MAX_DELAY);

	if (slot_erase(target_slot) != HAL_OK) {
		bl_print("Slot erase failed\r\n");
		return 0U;
	}

	HAL_UART_Transmit(&huart1, (uint8_t *)"R", 1U, HAL_MAX_DELAY);

	uint32_t received = 0U;

	while (received < header.size) {
		uint32_t remaining = header.size - received;
		uint32_t this_chunk = (remaining < CHUNK_SIZE) ? remaining : CHUNK_SIZE;

		if (HAL_UART_Receive(&huart1, chunk, this_chunk, CHUNK_TIMEOUT_MS) != HAL_OK) {
			bl_print("Chunk timeout\r\n");
			return 0U;
		}

		if (slot_write(target_slot, received, chunk, this_chunk) != HAL_OK) {
			bl_print("Slot write failed\r\n");
			return 0U;
		}

		received += this_chunk;

		HAL_UART_Transmit(&huart1, (uint8_t *)"A", 1U, HAL_MAX_DELAY);
	}

	// verify what actually landed in flash
	uint32_t computed = HAL_CRC_Calculate(&hcrc, (uint32_t *)slot_base_address(target_slot), header.size / 4U);

	if (computed != header.crc32) {
		bl_print("CRC mismatch - discarding\r\n");
		return 0U;
	}

	bl_print("Update verified\r\n");

	// commit - point at the new slot and mark as trial
	const ota_metadata_t *meta = (const ota_metadata_t *)METADATA_BASE_ADDRESS;
	ota_metadata_t updated = *meta;

	updated.active_slot = target_slot;
	updated.boot_confirmed = BOOT_TRIAL;
	updated.boot_attempts = BOOT_ATTEMPTS_FRESH;

	if (target_slot == SLOT_A) {
		updated.slot_a_size = header.size;
		updated.slot_a_crc32 = header.crc32;
		updated.slot_a_version = header.version;
	} else {
		updated.slot_b_size = header.size;
		updated.slot_b_crc32 = header.crc32;
		updated.slot_b_version = header.version;
	}

	if (metadata_write(&updated) != HAL_OK) {
		bl_print("Metadata write failed\r\n");
		return 0U;
	}

	return 1U;
}

static void jump_to_application(uint32_t slot) {
	uint32_t base = slot_base_address(slot);

    uint32_t app_stack_pointer = *(volatile uint32_t *)(base);
    uint32_t app_reset_handler = *(volatile uint32_t *)(base + 4UL);

    app_entry = (app_entry_t)app_reset_handler;

    /* Stop peripherals and interrupts before handing over */
    HAL_RCC_DeInit();
    HAL_DeInit();

    __disable_irq();

    SysTick->CTRL = 0;
    SysTick->LOAD = 0;
    SysTick->VAL = 0;

    for (uint32_t i = 0; i< 8UL; i++) {
    	NVIC->ICER[i] = 0xFFFFFFFFUL;
    	NVIC->ICPR[i] = 0xFFFFFFFFUL;
    }

    /* Point the vector table at the application */
    SCB->VTOR = base;

    /* Set the application's stack pointer */
    __set_MSP(app_stack_pointer);

    __enable_irq();

    /* Hand over control - never returns */
    app_entry();
}

#define BOOT_ATTEMPTS_ADDRESS   (METADATA_BASE_ADDRESS + 12UL)

static uint32_t attempts_used(uint32_t attempts_word)
{
    uint32_t count = 0U;

    for (uint32_t i = 0U; i < 32U; i++)
    {
        if ((attempts_word & (1UL << i)) == 0U)
        {
            count++;
        }
    }

    return count;
}

static HAL_StatusTypeDef record_boot_attempt(uint32_t attempts_word)
{
    uint32_t used = attempts_used(attempts_word);

    /* Clear one more bit, lowest first */
    uint32_t updated = attempts_word & ~(1UL << used);

    if (HAL_FLASH_Unlock() != HAL_OK)
    {
        return HAL_ERROR;
    }

    HAL_StatusTypeDef status = HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD,
                                                 BOOT_ATTEMPTS_ADDRESS,
                                                 updated);
    HAL_FLASH_Lock();
    return status;
}
/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_USART2_UART_Init();
  MX_CRC_Init();
  MX_USART1_UART_Init();
  /* USER CODE BEGIN 2 */
  bl_print("\r\n=== B2 Bootloader ===\r\n");

  const ota_metadata_t *meta = (const ota_metadata_t *)METADATA_BASE_ADDRESS;

  if (!metadata_is_valid(meta)) {
	  bl_print("No valid metadata - halting\r\n");
  } else {
	  uint32_t inactive = (meta->active_slot == SLOT_A) ? SLOT_B : SLOT_A;
	  if (receive_update(inactive)) {
		  bl_print("Rebooting into new image\r\n");
		  HAL_NVIC_SystemReset();
	  }

      uint32_t target_slot = meta->active_slot;

      if (meta->boot_confirmed != BOOT_CONFIRMED) {
          uint32_t used = attempts_used(meta->boot_attempts);

          if (used >= BOOT_ATTEMPTS_MAX) {
              bl_print("Trial image failed - rolling back\r\n");

              target_slot = (meta->active_slot == SLOT_A) ? SLOT_B : SLOT_A;

              ota_metadata_t updated = *meta;
              updated.active_slot    = target_slot;
              updated.boot_confirmed = BOOT_CONFIRMED;
              updated.boot_attempts  = BOOT_ATTEMPTS_FRESH;

              if (metadata_write(&updated) != HAL_OK) {
                  bl_print("Metadata write failed\r\n");
              }
          } else {
              bl_print("Trial boot - recording attempt\r\n");
              record_boot_attempt(meta->boot_attempts);
          }
      }

	  if (slot_is_valid(meta, target_slot)) {
		  bl_print((target_slot == SLOT_A) ? "Booting Slot A\r\n" : "Booting Slot B\r\n");
		  jump_to_application(target_slot);
	  } else {
		  // target slot is bad, try the other one
		  uint32_t other = (target_slot == SLOT_A) ? SLOT_B : SLOT_A;

		  bl_print("Target slot invalid, trying the other slot\r\n");

		  if (slot_is_valid(meta, other)) {
			  bl_print((other == SLOT_A ? "Booting Slot A\r\n" : "Booting Slot B\r\n"));
			  jump_to_application(other);
		  } else {
			  bl_print("Both slots invalid, halting\r\n");
		  }
	  }
  }

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE2);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLM = 16;
  RCC_OscInitStruct.PLL.PLLN = 336;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV4;
  RCC_OscInitStruct.PLL.PLLQ = 7;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief CRC Initialization Function
  * @param None
  * @retval None
  */
static void MX_CRC_Init(void)
{

  /* USER CODE BEGIN CRC_Init 0 */

  /* USER CODE END CRC_Init 0 */

  /* USER CODE BEGIN CRC_Init 1 */

  /* USER CODE END CRC_Init 1 */
  hcrc.Instance = CRC;
  if (HAL_CRC_Init(&hcrc) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN CRC_Init 2 */

  /* USER CODE END CRC_Init 2 */

}

/**
  * @brief USART1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART1_UART_Init(void)
{

  /* USER CODE BEGIN USART1_Init 0 */

  /* USER CODE END USART1_Init 0 */

  /* USER CODE BEGIN USART1_Init 1 */

  /* USER CODE END USART1_Init 1 */
  huart1.Instance = USART1;
  huart1.Init.BaudRate = 115200;
  huart1.Init.WordLength = UART_WORDLENGTH_8B;
  huart1.Init.StopBits = UART_STOPBITS_1;
  huart1.Init.Parity = UART_PARITY_NONE;
  huart1.Init.Mode = UART_MODE_TX_RX;
  huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart1.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART1_Init 2 */

  /* USER CODE END USART1_Init 2 */

}

/**
  * @brief USART2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART2_UART_Init(void)
{

  /* USER CODE BEGIN USART2_Init 0 */

  /* USER CODE END USART2_Init 0 */

  /* USER CODE BEGIN USART2_Init 1 */

  /* USER CODE END USART2_Init 1 */
  huart2.Instance = USART2;
  huart2.Init.BaudRate = 115200;
  huart2.Init.WordLength = UART_WORDLENGTH_8B;
  huart2.Init.StopBits = UART_STOPBITS_1;
  huart2.Init.Parity = UART_PARITY_NONE;
  huart2.Init.Mode = UART_MODE_TX_RX;
  huart2.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart2.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart2) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART2_Init 2 */

  /* USER CODE END USART2_Init 2 */

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  /* USER CODE BEGIN MX_GPIO_Init_1 */

  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOH_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(LD2_GPIO_Port, LD2_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin : B1_Pin */
  GPIO_InitStruct.Pin = B1_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_IT_FALLING;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(B1_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pin : LD2_Pin */
  GPIO_InitStruct.Pin = LD2_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(LD2_GPIO_Port, &GPIO_InitStruct);

  /* USER CODE BEGIN MX_GPIO_Init_2 */

  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */
static void bl_print(const char *msg) {
	HAL_UART_Transmit(&huart2, (uint8_t *)msg, strlen(msg), HAL_MAX_DELAY);
}
/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
