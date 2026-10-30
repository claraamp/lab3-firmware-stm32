/*
 * stub/main.h -- HAL de mentira para compilar o Core/Src/recepcao_serial.c no PC
 * (tests/host/recepcao/emular_placa.c). So o que o recepcao_serial.c usa; o "DMA" e
 * uma thread do emular_placa.c que le um pseudo-terminal.
 */
#ifndef STUB_MAIN_H
#define STUB_MAIN_H

#include <stdint.h>

typedef enum { HAL_OK = 0, HAL_ERROR = 1 } HAL_StatusTypeDef;
typedef enum { GPIO_PIN_RESET = 0, GPIO_PIN_SET = 1 } GPIO_PinState;
typedef struct { int nada; } DMA_HandleTypeDef;
typedef struct { uint32_t BaudRate; } UART_InitTypeDef;
typedef struct { UART_InitTypeDef Init; DMA_HandleTypeDef *hdmarx; } UART_HandleTypeDef;
typedef struct { uint32_t CYCCNT; } DWT_Type;

#define HAL_MAX_DELAY 0xFFFFFFFFu
#define LED_G_GPIO_Port 0
#define LED_G_Pin 1
#define LED_R_GPIO_Port 0
#define LED_R_Pin 2

extern UART_HandleTypeDef huart1;
extern DWT_Type emul_dwt;
extern uint32_t SystemCoreClock;
#define DWT (&emul_dwt)

HAL_StatusTypeDef HAL_UART_Transmit(UART_HandleTypeDef *h, uint8_t *dados, uint16_t n, uint32_t espera);
HAL_StatusTypeDef HAL_UART_Receive_DMA(UART_HandleTypeDef *h, uint8_t *dados, uint16_t n);
HAL_StatusTypeDef HAL_UART_AbortReceive(UART_HandleTypeDef *h);
void HAL_GPIO_WritePin(int porta, int pino, GPIO_PinState estado);
uint32_t HAL_GetTick(void);
uint32_t emul_ndtr(void);
void emul_limpar_erros(void);

#define __HAL_DMA_GET_COUNTER(h) emul_ndtr()
#define __HAL_UART_CLEAR_OREFLAG(h) emul_limpar_erros()

#endif
