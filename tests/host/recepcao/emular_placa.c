/*
 * emular_placa.c -- o modo de recepcao da placa rodando no PC, num pseudo-terminal
 *
 * Compila o Core/Src/recepcao_serial.c DE VERDADE (com o parser, a cadeia, a LDA e o
 * pacote) contra uma HAL de mentira (stub/main.h): a USART1 e um pseudo-terminal, e
 * o DMA circular e uma thread que escreve no anel os bytes que chegam e conta o
 * NDTR para baixo, como o DMA2 Stream2. O enviar_segmento.py conversa com ele como
 * com a placa. Nao testa a HAL nem o tempo da serial real (o pseudo-terminal nao
 * tem baud): testa o dialogo, o anel, a volta do anel, o parser e as respostas.
 *
 *   ./build/harness/emular_placa &             # imprime a porta: /dev/pts/N
 *   python3 tests/host/recepcao/enviar_segmento.py --porta /dev/pts/N --todos
 *
 * Os bytes chegam no ritmo de --baud (padrao 115200: 87 us por byte), como na UART.
 * Com --baud 0, chegam de uma vez, o anel de 2 KB da a volta antes de o laco ler,
 * e os quadros se corrompem: a placa tem de responder $ERR (crc ou truncado), e
 * NUNCA um $SEG com escore errado (o CRC tem de pegar tudo).
 *
 * Com --erro-uart K, a thread do DMA simula UM erro de linha (a HAL aborta o DMA e
 * chama o HAL_UART_ErrorCallback) no K-esimo byte do primeiro quadro: o laco tem de
 * rearmar e responder $ERR (crc ou truncado), e o quadro seguinte tem de passar.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "main.h"
#include "mfcc_estagios.h"
#include "recepcao_serial.h"

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart);

UART_HandleTypeDef huart1 = {{115200u}, NULL};
DWT_Type emul_dwt = {0u};
uint32_t SystemCoreClock = 100000000u;

static int mestre = -1;
static pthread_mutex_t trava = PTHREAD_MUTEX_INITIALIZER;
static uint8_t *volatile anel = NULL;
static volatile uint32_t tam = 0u;
static atomic_uint ndtr = 0u;
static atomic_int armado = 0;
static long erro_no_byte = -1;   /* --erro-uart */
static long baud = 115200;       /* --baud; 0 = sem ritmo */
static long bytes_recebidos = 0;

uint32_t emul_ndtr(void) { return atomic_load(&ndtr); }
void emul_limpar_erros(void) {}

HAL_StatusTypeDef HAL_UART_Transmit(UART_HandleTypeDef *h, uint8_t *dados, uint16_t n, uint32_t espera)
{
    (void)h;
    (void)espera;
    size_t f = 0;
    while (f < n) {
        const ssize_t w = write(mestre, dados + f, n - f);
        if (w > 0) {
            f += (size_t)w;
        }
    }
    return HAL_OK;
}

HAL_StatusTypeDef HAL_UART_Receive_DMA(UART_HandleTypeDef *h, uint8_t *dados, uint16_t n)
{
    (void)h;
    pthread_mutex_lock(&trava);
    anel = dados;
    tam = n;
    atomic_store(&ndtr, n);
    atomic_store(&armado, 1);
    pthread_mutex_unlock(&trava);
    return HAL_OK;
}

HAL_StatusTypeDef HAL_UART_AbortReceive(UART_HandleTypeDef *h)
{
    (void)h;
    pthread_mutex_lock(&trava);
    atomic_store(&armado, 0);
    pthread_mutex_unlock(&trava);
    return HAL_OK;
}

void HAL_GPIO_WritePin(int porta, int pino, GPIO_PinState estado)
{
    (void)porta;
    (void)pino;
    (void)estado;
}

uint32_t HAL_GetTick(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000);
}

/* O "DMA": cada byte que chega vai para anel[tam - ndtr], e o NDTR desce; na volta,
 * recarrega (modo circular). Desarmado, o byte se perde (como o overrun). */
static void *dma(void *arg)
{
    (void)arg;
    uint8_t buf[256];
    for (;;) {
        const ssize_t n = read(mestre, buf, baud > 0 ? 16 : sizeof buf);
        if (n <= 0) {
            usleep(1000);
            continue;
        }
        if (baud > 0) {   /* 10 bits por byte (8N1) */
            usleep((useconds_t)(n * 10L * 1000000L / baud));
        }
        pthread_mutex_lock(&trava);
        for (ssize_t i = 0; i < n; ++i) {
            if (!atomic_load(&armado)) {
                continue;
            }
            if (erro_no_byte >= 0 && tam > 64u && bytes_recebidos++ == erro_no_byte) {
                atomic_store(&armado, 0);   /* a HAL aborta o DMA e chama o callback */
                HAL_UART_ErrorCallback(&huart1);
                continue;
            }
            const uint32_t pos = tam - atomic_load(&ndtr);
            anel[pos] = buf[i];
            __atomic_thread_fence(__ATOMIC_RELEASE);
            const uint32_t novo = atomic_load(&ndtr) - 1u;
            atomic_store(&ndtr, novo == 0u ? tam : novo);
        }
        pthread_mutex_unlock(&trava);
    }
    return NULL;
}

int main(int argc, char **argv)
{
    for (int i = 1; i + 1 < argc; ++i) {
        if (strcmp(argv[i], "--erro-uart") == 0) {
            erro_no_byte = atol(argv[i + 1]);
        } else if (strcmp(argv[i], "--baud") == 0) {
            baud = atol(argv[i + 1]);
        }
    }
    mestre = posix_openpt(O_RDWR | O_NOCTTY);
    if (mestre < 0 || grantpt(mestre) != 0 || unlockpt(mestre) != 0) {
        perror("pseudo-terminal");
        return 1;
    }
    /* sem eco nem traducao de fim de linha: os bytes passam como na UART */
    char cmd[128];
    snprintf(cmd, sizeof cmd, "stty -F %s raw -echo", ptsname(mestre));
    if (system(cmd) != 0) {
        fprintf(stderr, "stty falhou\n");
    }
    printf("%s\n", ptsname(mestre));
    fflush(stdout);
    if (mfcc_estagios_init() != 0) {
        fprintf(stderr, "mfcc_estagios_init falhou\n");
        return 1;
    }
    pthread_t t;
    pthread_create(&t, NULL, dma, NULL);

    static const char hello[] = "hello frente1 (emulador)\r\n";
    HAL_UART_Transmit(&huart1, (uint8_t *)hello, sizeof hello - 1u, HAL_MAX_DELAY);
    recepcao_serial_escutar();
    for (;;) {   /* a "demonstracao": uma linha a cada ~2,3 s, como a da placa */
        if (recepcao_serial_pedido()) {
            recepcao_serial_executar(1);
        }
        static const char demo[] = "demo (emulador)\r\n";
        HAL_UART_Transmit(&huart1, (uint8_t *)demo, sizeof demo - 1u, HAL_MAX_DELAY);
        usleep(2300000);
    }
}
