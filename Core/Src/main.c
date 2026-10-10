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
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <stdio.h>
#include "arm_math.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define FFT_LEN 512U
#define FFT_BIN 32U

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */
static arm_rfft_fast_instance_f32 fft;
static float32_t fft_in[FFT_LEN];
static float32_t fft_out[FFT_LEN];
static float32_t fft_mag[FFT_LEN / 2];

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
static void fft_fill_sine(void)
{
  for (uint32_t n = 0; n < FFT_LEN; n++)
  {
    fft_in[n] = arm_sin_f32(2.0f * PI * (float32_t)(FFT_BIN * n) / (float32_t)FFT_LEN);
  }
}

static void dwt_init(void)
{
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CYCCNT = 0;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
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
  MX_USART1_UART_Init();
  /* USER CODE BEGIN 2 */
  const char hello[] = "hello frente1\r\n";
  HAL_UART_Transmit(&huart1, (uint8_t *)hello, sizeof(hello) - 1, HAL_MAX_DELAY);

  dwt_init();
  arm_status fft_status = arm_rfft_fast_init_f32(&fft, FFT_LEN);
  char status_line[32];
  int slen = snprintf(status_line, sizeof status_line, "fft_init status=%d\r\n", (int)fft_status);
  HAL_UART_Transmit(&huart1, (uint8_t *)status_line, slen, HAL_MAX_DELAY);
  uint32_t last_fft = HAL_GetTick();
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    static uint32_t n = 0;
  char buf[32];
  int len = snprintf(buf, sizeof buf, "tick %lu\r\n", (unsigned long)n);
  HAL_UART_Transmit(&huart1, (uint8_t *)buf, len, HAL_MAX_DELAY);

  HAL_GPIO_TogglePin(LED_PLACA_GPIO_Port, LED_PLACA_Pin);
  HAL_GPIO_WritePin(LED_G_GPIO_Port, LED_G_Pin, (n & 1) ? GPIO_PIN_SET : GPIO_PIN_RESET);
  HAL_GPIO_WritePin(LED_R_GPIO_Port, LED_R_Pin, (n & 1) ? GPIO_PIN_RESET : GPIO_PIN_SET);
  n++;

  if (HAL_GetTick() - last_fft >= 1000U)
  {
    last_fft += 1000U;

    /* arm_rfft_fast_f32 overwrites its input, so regenerate it every run */
    fft_fill_sine();
    uint32_t t0 = DWT->CYCCNT;
    arm_rfft_fast_f32(&fft, fft_in, fft_out, 0);
    uint32_t cycles = DWT->CYCCNT - t0;

    /* out[0] = DC and out[1] = Nyquist (both real); bins 1..255 are re,im pairs */
    arm_cmplx_mag_f32(fft_out, fft_mag, FFT_LEN / 2);
    fft_mag[0] = fabsf(fft_out[0]);

    float32_t peak;
    uint32_t peak_bin;
    arm_max_f32(fft_mag, FFT_LEN / 2, &peak, &peak_bin);

    /* nano.specs printf has no %f: print fixed point by hand */
    uint32_t us_x10 = (uint32_t)(((uint64_t)cycles * 10U + SystemCoreClock / 2000000U) / (SystemCoreClock / 1000000U));
    uint32_t mag_x10 = (uint32_t)(peak * 10.0f + 0.5f);
    char line[80];
    int flen = snprintf(line, sizeof line, "fft512 ciclos=%lu us=%lu.%lu pico_bin=%lu mag=%lu.%lu\r\n",
                        (unsigned long)cycles,
                        (unsigned long)(us_x10 / 10U), (unsigned long)(us_x10 % 10U),
                        (unsigned long)peak_bin,
                        (unsigned long)(mag_x10 / 10U), (unsigned long)(mag_x10 % 10U));
    HAL_UART_Transmit(&huart1, (uint8_t *)line, flen, HAL_MAX_DELAY);
  }

  HAL_Delay(500);
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
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 25;
  RCC_OscInitStruct.PLL.PLLN = 200;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 4;
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

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_3) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */

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
