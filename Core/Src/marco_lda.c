/*
 * marco_lda.c -- marco de 13/10: LDA na placa com caracteristicas de referencia
 *
 * Ver marco_lda.h. Os dois cabecalhos do modelo vem do repositorio do Python
 * (diagnostico-acustico-motores) e nao se editam a mao:
 *   lda_modelo.h        <- scripts/pipeline/06_export_lda_header.py
 *   lda_vetores_teste.h <- scripts/pipeline/07_export_lda_test_vectors.py
 *
 * Este arquivo e compilado sempre com -O2 (CMakeLists.txt da raiz), inclusive no
 * Debug, para o tempo medido pelo DWT ser o do firmware final.
 */

#include "marco_lda.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "lda_modelo.h"
#include "lda_vetores_teste.h"

_Static_assert(VT_N_FEATURES == LDA_N_FEATURES,
               "lda_vetores_teste.h e lda_modelo.h com numero de caracteristicas diferente");

/* ------------------------------------------------------------------------- */
/* Sem HAL                                                                    */
/* ------------------------------------------------------------------------- */

/* Opaca para o compilador (noipa): a chamada nao sai do lugar entre as duas
 * leituras do DWT, e o tempo medido e o de um escore de verdade. */
static float __attribute__((noinline, noipa)) escore_opaco(const float *x)
{
    return lda_escore(x);
}

uint16_t marco_n_segmentos(void) { return VT_N_SEGMENTOS; }
uint16_t marco_n_demo(void) { return VT_N_DEMO; }
uint16_t marco_demo_indice(uint16_t k) { return vt_demo[k % VT_N_DEMO]; }

int marco_cabecalhos_coerentes(void)
{
    return lda_bias == VT_LDA_BIAS;
}

static void preencher(uint16_t i, float escore, marco_resultado_t *r)
{
    r->escore = escore;
    r->dif = fabsf(escore - vt_escore_ref[i]);
    r->falha = (uint8_t)lda_e_falha(escore);
    r->ok_escore = (uint8_t)(r->dif <= vt_tolerancia[i]);
    r->ok_decisao = (uint8_t)(r->falha == vt_falha_ref[i]);
}

void marco_avaliar(uint16_t i, marco_resultado_t *r)
{
    preencher(i, escore_opaco(vt_x[i]), r);
}

int marco_fmt_fixo(char *buf, size_t n, float v, unsigned casas)
{
    static const uint32_t pot10[] = {1u, 10u, 100u, 1000u, 10000u, 100000u, 1000000u};
    if (casas > 6u) {
        casas = 6u;
    }
    if (isnan(v)) {
        return snprintf(buf, n, "nan");
    }
    const float a = fabsf(v) * (float)pot10[casas] + 0.5f;
    if (!(a < 4.0e9f)) {
        return snprintf(buf, n, "%sinf", v < 0.0f ? "-" : "");
    }
    const uint32_t q = (uint32_t)a;
    const char *sinal = (v < 0.0f && q != 0u) ? "-" : "";
    if (casas == 0u) {
        return snprintf(buf, n, "%s%lu", sinal, (unsigned long)q);
    }
    return snprintf(buf, n, "%s%lu.%0*lu", sinal, (unsigned long)(q / pot10[casas]),
                    (int)casas, (unsigned long)(q % pot10[casas]));
}

int marco_linha(char *buf, size_t n, const char *prefixo, uint16_t i,
                const marco_resultado_t *r, uint32_t ciclos)
{
    char ref[20], placa[20], dif[20], tol[20];
    marco_fmt_fixo(ref, sizeof ref, vt_escore_ref[i], 3);
    marco_fmt_fixo(placa, sizeof placa, r->escore, 3);
    marco_fmt_fixo(dif, sizeof dif, r->dif, 6);
    marco_fmt_fixo(tol, sizeof tol, vt_tolerancia[i], 6);
    return snprintf(buf, n,
                    "%s seg=%u grav=%s ref=%s placa=%s dif=%s tol=%s dec=%s %s ciclos=%lu\r\n",
                    prefixo, (unsigned)vt_segmento[i], vt_nomes_gravacao[vt_gravacao[i]],
                    ref, placa, dif, tol, r->falha ? "falha" : "normal",
                    (r->ok_escore && r->ok_decisao) ? "OK" : "FALHOU",
                    (unsigned long)ciclos);
}

/* ------------------------------------------------------------------------- */
/* Com HAL                                                                    */
/* ------------------------------------------------------------------------- */
#ifndef MARCO_HOST

#include "main.h"
#include "usart.h"

static void tx(const char *s, int len)
{
    if (len > 0) {
        HAL_UART_Transmit(&huart1, (uint8_t *)s, (uint16_t)len, HAL_MAX_DELAY);
    }
}

static void tx_str(const char *s)
{
    tx(s, (int)strlen(s));
}

static void leds(int verde, int vermelho)
{
    HAL_GPIO_WritePin(LED_G_GPIO_Port, LED_G_Pin, verde ? GPIO_PIN_SET : GPIO_PIN_RESET);
    HAL_GPIO_WritePin(LED_R_GPIO_Port, LED_R_Pin, vermelho ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

static void dwt_ligar(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

/* Ciclos do escore (com a chamada), descontado o custo de ler o DWT. */
static uint32_t avaliar_medindo(uint16_t i, marco_resultado_t *r, uint32_t custo_leitura)
{
    const float *x = vt_x[i];
    /* As barreiras impedem o compilador de trazer para dentro da medicao o
     * calculo do endereco ou as leituras de preencher(). */
    __asm volatile("" ::: "memory");
    const uint32_t t0 = DWT->CYCCNT;
    const float s = escore_opaco(x);
    const uint32_t t1 = DWT->CYCCNT;
    __asm volatile("" ::: "memory");
    preencher(i, s, r);
    const uint32_t c = t1 - t0;
    return c > custo_leitura ? c - custo_leitura : 0u;
}

static uint32_t us_x100(uint32_t ciclos)
{
    const uint32_t mhz = SystemCoreClock / 1000000u;
    return (uint32_t)(((uint64_t)ciclos * 100u + mhz / 2u) / mhz);
}

int marco_lda_verificar(void)
{
    char linha[200];
    int len;

    leds(0, 0);
    HAL_GPIO_WritePin(LED_PLACA_GPIO_Port, LED_PLACA_Pin, GPIO_PIN_RESET);   /* acende */

    len = snprintf(linha, sizeof linha,
                   "marco13 modelo=%s json=%s n_features=%d segmentos=%u clock=%luMHz\r\n",
                   VT_MODELO_ID, VT_JSON_SHA256_12, LDA_N_FEATURES, (unsigned)VT_N_SEGMENTOS,
                   (unsigned long)(SystemCoreClock / 1000000u));
    tx(linha, len);

    if (!marco_cabecalhos_coerentes()) {
        len = snprintf(linha, sizeof linha,
                       "ERRO: lda_modelo.h e lda_vetores_teste.h sao de modelos diferentes "
                       "(bias diferente); gere os dois do mesmo lda_final.json\r\n");
        tx(linha, len);
        return 0;
    }

    dwt_ligar();
    const uint32_t a = DWT->CYCCNT;
    const uint32_t b = DWT->CYCCNT;
    const uint32_t custo_leitura = b - a;

    uint32_t n_ok = 0, n_dec_ok = 0;
    uint32_t c_min = UINT32_MAX, c_max = 0, c_soma = 0;
    float dif_max = 0.0f, razao_max = 0.0f;
    uint16_t seg_dif_max = 0;

    for (uint16_t i = 0; i < VT_N_SEGMENTOS; ++i) {
        marco_resultado_t r;
        const uint32_t c = avaliar_medindo(i, &r, custo_leitura);
        n_ok += (r.ok_escore && r.ok_decisao);
        n_dec_ok += r.ok_decisao;
        c_min = c < c_min ? c : c_min;
        c_max = c > c_max ? c : c_max;
        c_soma += c;
        if (r.dif > dif_max) {
            dif_max = r.dif;
            seg_dif_max = vt_segmento[i];
        }
        const float razao = r.dif / vt_tolerancia[i];
        razao_max = razao > razao_max ? razao : razao_max;
        tx(linha, marco_linha(linha, sizeof linha, "seg", i, &r, c));
    }

    char s_dif[20], s_razao[20];
    marco_fmt_fixo(s_dif, sizeof s_dif, dif_max, 6);
    marco_fmt_fixo(s_razao, sizeof s_razao, 100.0f * razao_max, 1);
    const uint32_t c_med = (c_soma + VT_N_SEGMENTOS / 2u) / VT_N_SEGMENTOS;
    const uint32_t us = us_x100(c_med);
    len = snprintf(linha, sizeof linha,
                   "resumo ok=%lu/%u decisoes_iguais=%lu/%u dif_max=%s (seg %u) "
                   "dif/tol_max=%s%% ciclos min=%lu med=%lu max=%lu (med %lu.%02lu us)\r\n",
                   (unsigned long)n_ok, (unsigned)VT_N_SEGMENTOS,
                   (unsigned long)n_dec_ok, (unsigned)VT_N_SEGMENTOS,
                   s_dif, (unsigned)seg_dif_max, s_razao,
                   (unsigned long)c_min, (unsigned long)c_med, (unsigned long)c_max,
                   (unsigned long)(us / 100u), (unsigned long)(us % 100u));
    tx(linha, len);

    const int ok = (n_ok == VT_N_SEGMENTOS);
    tx_str(ok ? "MARCO OK\r\n" : "MARCO FALHOU\r\n");
    return ok;
}

void marco_lda_passo(int verificacao_ok)
{
    static uint16_t k = 0;
    char linha[200];

    if (!verificacao_ok) {
        /* padrao de erro: os dois LEDs juntos, rapido (nao e uma decisao) */
        for (int j = 0; j < 16; ++j) {
            leds(j & 1, j & 1);
            HAL_Delay(125);
        }
        tx_str("ERRO: verificacao do marco falhou (ver as linhas FALHOU acima)\r\n");
        return;
    }

    const uint16_t i = vt_demo[k];
    marco_resultado_t r;
    marco_avaliar(i, &r);
    leds(!r.falha, r.falha);
    tx(linha, marco_linha(linha, sizeof linha, "demo", i, &r, 0u));
    HAL_Delay(2000);
    leds(0, 0);
    HAL_Delay(300);
    k = (uint16_t)((k + 1u) % VT_N_DEMO);
}

#endif /* MARCO_HOST */
