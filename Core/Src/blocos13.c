/*
 * blocos13.c -- blocos 1 a 3 da extracao de caracteristicas na placa
 *
 * Ver blocos13.h (protocolo da serial e como a medicao de ciclos e feita).
 */

#include "blocos13.h"

#include <stdio.h>
#include <string.h>

#include "blocos13_entrada.h"
#include "mfcc_estagios.h"

enum { B_JANELA, B_ESPECTRO, B_MEL, B_QUADRO, N_MEDIDAS };
static const char *const NOME_MEDIDA[N_MEDIDAS] = {"janela", "espectro", "mel", "quadro"};

/* Buffers de um quadro: a cadeia nunca guarda o segmento inteiro (ver mfcc_estagios.h). */
static float quadro[MFCC_N_QUADRO];
static float potencia[MFCC_N_BINS];
static float mel[MFCC_N_MEL];

/* Ciclos de cada medida em cada quadro do segmento atual, e de todos os segmentos
 * para o resumo (5 x 98 x 4 x 4 B = 7,8 KB de RAM). */
static uint32_t ciclos_q[N_MEDIDAS][MFCC_N_QUADROS];
static uint32_t ciclos_todos[N_MEDIDAS][B13_N_SEGMENTOS * MFCC_N_QUADROS];
static uint32_t ciclos_segmento[B13_N_SEGMENTOS];

static b13_enviar_fn enviar;
static b13_ciclos_fn ler_ciclos;

/* Impede o compilador de mover a conta para fora da janela do contador. */
#define BARREIRA() __asm__ volatile("" ::: "memory")

/* ------------------------------------------------------------------------- */
/* CRC-32 (zlib), por tabela de 16 entradas: 64 B de flash                    */
/* ------------------------------------------------------------------------- */
uint32_t b13_crc32(uint32_t crc, const void *dados, size_t n)
{
    static const uint32_t t[16] = {
        0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu, 0x76DC4190u, 0x6B6B51F4u,
        0x4DB26158u, 0x5005713Cu, 0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
        0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu};
    const uint8_t *p = (const uint8_t *)dados;
    crc = ~crc;
    for (size_t i = 0; i < n; ++i) {
        crc ^= p[i];
        crc = (crc >> 4) ^ t[crc & 0x0Fu];
        crc = (crc >> 4) ^ t[crc & 0x0Fu];
    }
    return ~crc;
}

/* ------------------------------------------------------------------------- */
/* Texto                                                                      */
/* ------------------------------------------------------------------------- */
static void enviar_texto(const char *s)
{
    enviar(s, strlen(s));
}

static void ordenar(uint32_t *v, int n)
{
    for (int i = 1; i < n; ++i) {
        const uint32_t x = v[i];
        int j = i - 1;
        while (j >= 0 && v[j] > x) {
            v[j + 1] = v[j];
            --j;
        }
        v[j + 1] = x;
    }
}

/* min/med/max de n valores (ordena uma copia); med = mediana inferior */
static int fmt_estat(char *buf, size_t tam, const uint32_t *v, int n)
{
    static uint32_t copia[B13_N_SEGMENTOS * MFCC_N_QUADROS];
    memcpy(copia, v, sizeof(uint32_t) * (size_t)n);
    ordenar(copia, n);
    return snprintf(buf, tam, "%lu/%lu/%lu", (unsigned long)copia[0],
                    (unsigned long)copia[(n - 1) / 2], (unsigned long)copia[n - 1]);
}

/* microssegundos com uma casa, sem %f (o printf do nano.specs nao tem) */
static int fmt_us(char *buf, size_t tam, uint32_t ciclos, uint32_t clock_hz)
{
    if (clock_hz < 1000000u) {
        return snprintf(buf, tam, "0.0");
    }
    const uint32_t por_us = clock_hz / 1000000u;
    const uint64_t us_x10 = ((uint64_t)ciclos * 10u + por_us / 2u) / por_us;
    return snprintf(buf, tam, "%lu.%lu", (unsigned long)(us_x10 / 10u), (unsigned long)(us_x10 % 10u));
}

/* ------------------------------------------------------------------------- */
/* Medicao                                                                    */
/* ------------------------------------------------------------------------- */
/* Custo de ler o contador duas vezes seguidas, descontado de cada medida. */
static uint32_t custo_leitura(void)
{
    uint32_t menor = UINT32_MAX;
    for (int i = 0; i < 16; ++i) {
        BARREIRA();
        const uint32_t t0 = ler_ciclos();
        BARREIRA();
        const uint32_t t1 = ler_ciclos();
        BARREIRA();
        if (t1 - t0 < menor) {
            menor = t1 - t0;
        }
    }
    return menor;
}

static uint32_t descontar(uint32_t bruto, uint32_t custo)
{
    return bruto > custo ? bruto - custo : 0u;
}

static void medir_segmento(int s, uint32_t custo)
{
    const int16_t *pcm = b13_entrada[s];

    /* cada bloco sozinho; a entrada de cada um e a saida do anterior, como na cadeia */
    for (int q = 0; q < MFCC_N_QUADROS; ++q) {
        const int16_t *x = &pcm[q * MFCC_PASSO];
        uint32_t t0, t1;

        BARREIRA();
        t0 = ler_ciclos();
        BARREIRA();
        mfcc_quadro_pcm(x, quadro);
        BARREIRA();
        t1 = ler_ciclos();
        BARREIRA();
        ciclos_q[B_JANELA][q] = descontar(t1 - t0, custo);

        t0 = ler_ciclos();
        BARREIRA();
        mfcc_espectro(quadro, potencia);
        BARREIRA();
        t1 = ler_ciclos();
        BARREIRA();
        ciclos_q[B_ESPECTRO][q] = descontar(t1 - t0, custo);

        t0 = ler_ciclos();
        BARREIRA();
        mfcc_mel(potencia, mel);
        BARREIRA();
        t1 = ler_ciclos();
        BARREIRA();
        ciclos_q[B_MEL][q] = descontar(t1 - t0, custo);

        /* os tres juntos, como num quadro de verdade */
        t0 = ler_ciclos();
        BARREIRA();
        mfcc_quadro_pcm(x, quadro);
        mfcc_espectro(quadro, potencia);
        mfcc_mel(potencia, mel);
        BARREIRA();
        t1 = ler_ciclos();
        BARREIRA();
        ciclos_q[B_QUADRO][q] = descontar(t1 - t0, custo);
    }

    /* o segmento inteiro de uma vez: 98 quadros, os tres blocos */
    BARREIRA();
    const uint32_t t0 = ler_ciclos();
    BARREIRA();
    for (int q = 0; q < MFCC_N_QUADROS; ++q) {
        mfcc_quadro_pcm(&pcm[q * MFCC_PASSO], quadro);
        mfcc_espectro(quadro, potencia);
        mfcc_mel(potencia, mel);
    }
    BARREIRA();
    const uint32_t t1 = ler_ciclos();
    BARREIRA();
    ciclos_segmento[s] = descontar(t1 - t0, custo);

    for (int m = 0; m < N_MEDIDAS; ++m) {
        memcpy(&ciclos_todos[m][s * MFCC_N_QUADROS], ciclos_q[m], sizeof ciclos_q[m]);
    }
}

/* ------------------------------------------------------------------------- */
/* Exportacao                                                                 */
/* ------------------------------------------------------------------------- */
enum { E_JANELA, E_ESPECTRO, E_MEL, N_EXPORTA };
static const char *const NOME_ESTAGIO[N_EXPORTA] = {"janela", "espectro", "mel"};
static const int COLUNAS[N_EXPORTA] = {MFCC_N_QUADRO, MFCC_N_BINS, MFCC_N_MEL};

static void exportar(int s, int e)
{
    char linha[160];
    const size_t por_quadro = sizeof(float) * (size_t)COLUNAS[e];
    snprintf(linha, sizeof linha, "b13 bin nome=%s estagio=%s linhas=%d colunas=%d bytes=%lu\r\n",
             b13_nomes[s], NOME_ESTAGIO[e], MFCC_N_QUADROS, COLUNAS[e],
             (unsigned long)(por_quadro * MFCC_N_QUADROS));
    enviar_texto(linha);

    uint32_t crc = 0;
    const int16_t *pcm = b13_entrada[s];
    for (int q = 0; q < MFCC_N_QUADROS; ++q) {
        mfcc_quadro_pcm(&pcm[q * MFCC_PASSO], quadro);
        const float *saida = quadro;
        if (e >= E_ESPECTRO) {
            mfcc_espectro(quadro, potencia);
            saida = potencia;
        }
        if (e >= E_MEL) {
            mfcc_mel(potencia, mel);
            saida = mel;
        }
        /* o Cortex-M4 e little-endian, como o receptor espera */
        crc = b13_crc32(crc, saida, por_quadro);
        enviar(saida, por_quadro);
    }
    snprintf(linha, sizeof linha, "\r\nb13 fim nome=%s estagio=%s crc32=%08lx\r\n", b13_nomes[s],
             NOME_ESTAGIO[e], (unsigned long)crc);
    enviar_texto(linha);
}

int blocos13_rodar(b13_enviar_fn f_enviar, b13_ciclos_fn f_ciclos, uint32_t clock_hz)
{
    enviar = f_enviar;
    ler_ciclos = f_ciclos;
    char linha[256], us[24];

    const int st = mfcc_estagios_init();
    if (st != 0) {
        snprintf(linha, sizeof linha, "b13 ERRO mfcc_estagios_init=%d\r\n", st);
        enviar_texto(linha);
        return st;
    }
    const uint32_t custo = custo_leitura();

    snprintf(linha, sizeof linha,
             "b13 inicio modelo=%s json=%s segmentos=%d clock=%luMHz custo_leitura=%lu\r\n",
             B13_MODELO_ID, B13_MODELO_JSON, B13_N_SEGMENTOS, (unsigned long)(clock_hz / 1000000u),
             (unsigned long)custo);
    enviar_texto(linha);

    for (int s = 0; s < B13_N_SEGMENTOS; ++s) {
        snprintf(linha, sizeof linha, "b13 seg i=%d nome=%s entrada=%s\r\n", s, b13_nomes[s],
                 b13_entrada_sha[s]);
        enviar_texto(linha);

        medir_segmento(s, custo);
        int n = snprintf(linha, sizeof linha, "b13 ciclos nome=%s", b13_nomes[s]);
        for (int m = 0; m < N_MEDIDAS; ++m) {
            n += snprintf(linha + n, sizeof linha - (size_t)n, " %s=", NOME_MEDIDA[m]);
            n += fmt_estat(linha + n, sizeof linha - (size_t)n, ciclos_q[m], MFCC_N_QUADROS);
        }
        fmt_us(us, sizeof us, ciclos_segmento[s], clock_hz);
        snprintf(linha + n, sizeof linha - (size_t)n, " segmento=%lu us=%s\r\n",
                 (unsigned long)ciclos_segmento[s], us);
        enviar_texto(linha);

        for (int e = 0; e < N_EXPORTA; ++e) {
            exportar(s, e);
        }
    }

    int n = snprintf(linha, sizeof linha, "b13 resumo");
    for (int m = 0; m < N_MEDIDAS; ++m) {
        n += snprintf(linha + n, sizeof linha - (size_t)n, " %s=", NOME_MEDIDA[m]);
        n += fmt_estat(linha + n, sizeof linha - (size_t)n, ciclos_todos[m],
                       B13_N_SEGMENTOS * MFCC_N_QUADROS);
    }
    uint32_t seg[B13_N_SEGMENTOS];
    memcpy(seg, ciclos_segmento, sizeof seg);
    ordenar(seg, B13_N_SEGMENTOS);
    const uint32_t seg_med = seg[(B13_N_SEGMENTOS - 1) / 2];
    fmt_us(us, sizeof us, seg_med, clock_hz);
    snprintf(linha + n, sizeof linha - (size_t)n, " segmento_med=%lu us=%s\r\n", (unsigned long)seg_med, us);
    enviar_texto(linha);
    enviar_texto("b13 FIM\r\n");
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Na placa                                                                   */
/* ------------------------------------------------------------------------- */
#ifndef BLOCOS13_HOST
#include "main.h"
#include "usart.h"

static void enviar_uart(const void *dados, size_t n)
{
    const uint8_t *p = (const uint8_t *)dados;
    while (n > 0) {
        const uint16_t parte = n > 0xFFFFu ? 0xFFFFu : (uint16_t)n;
        HAL_UART_Transmit(&huart1, (uint8_t *)p, parte, HAL_MAX_DELAY);
        p += parte;
        n -= parte;
    }
}

static uint32_t ler_dwt(void)
{
    return DWT->CYCCNT;
}

int blocos13_executar(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    return blocos13_rodar(enviar_uart, ler_dwt, SystemCoreClock);
}
#endif
