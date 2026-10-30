/*
 * blocos13.c -- blocos 1 a 3 da extracao de caracteristicas na placa
 *
 * Ver blocos13.h (protocolo da serial e como a medicao de ciclos e feita).
 */

#include "blocos13.h"

#include <stdio.h>
#include <string.h>

#include "clipes_demo.h"
#include "crc32.h"
#include "mfcc_estagios.h"

enum { B_JANELA, B_ESPECTRO, B_MEL, B_QUADRO, N_MEDIDAS };
static const char *const NOME_MEDIDA[N_MEDIDAS] = {"janela", "espectro", "mel", "quadro"};

/* Buffers de um quadro: a cadeia nunca guarda o segmento inteiro (ver mfcc_estagios.h). */
static float quadro[MFCC_N_QUADRO];
static float potencia[MFCC_N_BINS];
static float mel[MFCC_N_MEL];

/* Os segmentos do teste sao o primeiro de cada clipe do clipes_demo.h: os mesmos
 * 5 das referencias por estagio do harness (normal_seg0029, bpfi_0.3mm_seg0088,
 * ...), bit a bit. O receptor confere pelo CRC-32 de cada entrada. */
#define B13_N_SEGMENTOS CLIPES_N_CLIPES

/* Ciclos de cada medida em cada quadro do segmento atual, e de todos os segmentos
 * para o resumo (5 x 98 x 4 x 4 B = 7,8 KB de RAM). */
static uint32_t ciclos_q[N_MEDIDAS][MFCC_N_QUADROS];
static uint32_t ciclos_todos[N_MEDIDAS][B13_N_SEGMENTOS * MFCC_N_QUADROS];
static uint32_t ciclos_segmento[B13_N_SEGMENTOS];

static b13_enviar_fn enviar;
static b13_ciclos_fn ler_ciclos;

/* As 12.800 amostras do primeiro segmento do clipe s */
static const int16_t *entrada(int s)
{
    return clipes[s].pcm;
}

/* Nome do segmento como nas referencias: <classe>_seg<id com 4 digitos> */
static void nome_segmento(int s, char *buf, size_t tam)
{
    const clipe_segmento_t *seg = &clipes_segmentos[clipes[s].primeiro_segmento];
    snprintf(buf, tam, "%s_seg%04u", clipes[s].classe, (unsigned)seg->id);
}

/* Impede o compilador de mover a conta para fora da janela do contador. */
#define BARREIRA() __asm__ volatile("" ::: "memory")

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
    const int16_t *pcm = entrada(s);

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

static void exportar(int s, int e, const char *nome)
{
    char linha[160];
    const size_t por_quadro = sizeof(float) * (size_t)COLUNAS[e];
    snprintf(linha, sizeof linha, "b13 bin nome=%s estagio=%s linhas=%d colunas=%d bytes=%lu\r\n",
             nome, NOME_ESTAGIO[e], MFCC_N_QUADROS, COLUNAS[e],
             (unsigned long)(por_quadro * MFCC_N_QUADROS));
    enviar_texto(linha);

    uint32_t crc = 0;
    const int16_t *pcm = entrada(s);
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
        crc = crc32_bytes(crc, saida, por_quadro);
        enviar(saida, por_quadro);
    }
    snprintf(linha, sizeof linha, "\r\nb13 fim nome=%s estagio=%s crc32=%08lx\r\n", nome,
             NOME_ESTAGIO[e], (unsigned long)crc);
    enviar_texto(linha);
}

int blocos13_rodar(b13_enviar_fn f_enviar, b13_ciclos_fn f_ciclos, uint32_t clock_hz)
{
    enviar = f_enviar;
    ler_ciclos = f_ciclos;
    char linha[256], us[24], nome[48];

    const int st = mfcc_estagios_init();
    if (st != 0) {
        snprintf(linha, sizeof linha, "b13 ERRO mfcc_estagios_init=%d\r\n", st);
        enviar_texto(linha);
        return st;
    }
    const uint32_t custo = custo_leitura();

    snprintf(linha, sizeof linha,
             "b13 inicio modelo=%s json=%s segmentos=%d clock=%luMHz custo_leitura=%lu\r\n",
             CLIPES_MODELO_ID, CLIPES_JSON_SHA256_12, B13_N_SEGMENTOS, (unsigned long)(clock_hz / 1000000u),
             (unsigned long)custo);
    enviar_texto(linha);

    for (int s = 0; s < B13_N_SEGMENTOS; ++s) {
        nome_segmento(s, nome, sizeof nome);
        /* CRC-32 das 12.800 amostras int16 (25.600 B), para o receptor conferir que a
         * entrada e a mesma da referencia */
        const uint32_t crc_entrada =
            crc32_bytes(0, entrada(s), sizeof(int16_t) * (size_t)CLIPES_AMOSTRAS_POR_SEGMENTO);
        snprintf(linha, sizeof linha, "b13 seg i=%d nome=%s entrada_crc32=%08lx\r\n", s, nome,
                 (unsigned long)crc_entrada);
        enviar_texto(linha);

        medir_segmento(s, custo);
        int n = snprintf(linha, sizeof linha, "b13 ciclos nome=%s", nome);
        for (int m = 0; m < N_MEDIDAS; ++m) {
            n += snprintf(linha + n, sizeof linha - (size_t)n, " %s=", NOME_MEDIDA[m]);
            n += fmt_estat(linha + n, sizeof linha - (size_t)n, ciclos_q[m], MFCC_N_QUADROS);
        }
        fmt_us(us, sizeof us, ciclos_segmento[s], clock_hz);
        snprintf(linha + n, sizeof linha - (size_t)n, " segmento=%lu us=%s\r\n",
                 (unsigned long)ciclos_segmento[s], us);
        enviar_texto(linha);

        for (int e = 0; e < N_EXPORTA; ++e) {
            exportar(s, e, nome);
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
