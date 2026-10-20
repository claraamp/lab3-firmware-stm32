/*
 * validacao_placa.c -- segmento completo na placa (ver validacao_placa.h)
 */

#include "validacao_placa.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "clipes_demo.h"
#include "lda_modelo.h"
#include "mfcc_estagios.h"

_Static_assert(LDA_N_FEATURES == 2 * MFCC_N_COEFS,
               "lda_modelo.h espera outro numero de caracteristicas que o mfcc_estagios.h");

/* Impede o compilador de mover a conta para fora da janela do contador. */
#define BARREIRA() __asm__ volatile("" ::: "memory")

static uint32_t bits(float v)
{
    uint32_t u;
    memcpy(&u, &v, sizeof u);
    return u;
}

/* Acrescenta texto a linha a partir de n e devolve o novo tamanho, ou -1 se nao
 * couber (e continua -1 nas chamadas seguintes): assim o tamanho que sobra nunca
 * da a volta e nenhum snprintf escreve fora do buffer. */
static int __attribute__((format(printf, 4, 5)))
acrescentar(char *linha, size_t tam, int n, const char *fmt, ...)
{
    if (n < 0 || (size_t)n >= tam) {
        return -1;
    }
    va_list ap;
    va_start(ap, fmt);
    const int m = vsnprintf(linha + n, tam - (size_t)n, fmt, ap);
    va_end(ap);
    return (m < 0 || (size_t)m >= tam - (size_t)n) ? -1 : n + m;
}

int validacao_placa_rodar(val_enviar_fn enviar, val_ciclos_fn ciclos, uint32_t clock_hz)
{
    /* estatico, e nao na pilha: o linker so reserva 4 KB de pilha (_Min_Stack_Size)
     * e nao avisa se a pilha real passar disso */
    static char linha[384];
    int n;

    const int st = mfcc_estagios_init();
    if (st != 0) {
        n = snprintf(linha, sizeof linha, "val ERRO mfcc_estagios_init=%d\r\n", st);
        enviar(linha, (size_t)n);
        return st;
    }
    n = snprintf(linha, sizeof linha, "val inicio modelo=%s json=%s segmentos=%d clock=%luMHz\r\n",
                 CLIPES_MODELO_ID, CLIPES_JSON_SHA256_12, CLIPES_N_SEGMENTOS,
                 (unsigned long)(clock_hz / 1000000u));
    enviar(linha, (size_t)n);

    for (int c = 0; c < CLIPES_N_CLIPES; ++c) {
        for (int k = 0; k < clipes[c].n_segmentos; ++k) {
            const clipe_segmento_t *seg = &clipes_segmentos[clipes[c].primeiro_segmento + k];
            const int16_t *pcm = &clipes[c].pcm[(size_t)k * CLIPES_AMOSTRAS_POR_SEGMENTO];
            float x[2 * MFCC_N_COEFS];

            BARREIRA();
            const uint32_t t0 = ciclos();
            BARREIRA();
            const int r = mfcc_segmento(pcm, x);
            BARREIRA();
            const uint32_t t1 = ciclos();
            BARREIRA();
            if (r != 0) {
                n = snprintf(linha, sizeof linha, "val ERRO mfcc_segmento=%d id=%u\r\n", r,
                             (unsigned)seg->id);
                enviar(linha, (size_t)n);
                return r;
            }
            const float escore = lda_escore(x);

            n = acrescentar(linha, sizeof linha, 0, "val seg id=%u classe=%s ciclos=%lu escore=%08lx x=",
                            (unsigned)seg->id, clipes[c].classe, (unsigned long)(t1 - t0),
                            (unsigned long)bits(escore));
            for (int i = 0; i < 2 * MFCC_N_COEFS; ++i) {
                n = acrescentar(linha, sizeof linha, n, i ? ",%08lx" : "%08lx", (unsigned long)bits(x[i]));
            }
            n = acrescentar(linha, sizeof linha, n, "\r\n");
            if (n < 0) {
                /* nao acontece com os clipes de hoje (~310 B); uma linha cortada seria
                 * recusada pelo validador, mas o aviso diz o motivo */
                n = snprintf(linha, sizeof linha, "val ERRO linha_longa id=%u\r\n", (unsigned)seg->id);
                enviar(linha, (size_t)n);
                return -4;
            }
            enviar(linha, (size_t)n);
        }
    }
    enviar("val FIM\r\n", 9u);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Na placa                                                                   */
/* ------------------------------------------------------------------------- */
#ifndef VALIDACAO_HOST
#include "main.h"
#include "usart.h"

static void enviar_uart(const char *texto, size_t n)
{
    HAL_UART_Transmit(&huart1, (uint8_t *)texto, (uint16_t)n, HAL_MAX_DELAY);
}

static uint32_t ler_dwt(void)
{
    return DWT->CYCCNT;
}

int validacao_placa_executar(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    return validacao_placa_rodar(enviar_uart, ler_dwt, SystemCoreClock);
}
#endif
