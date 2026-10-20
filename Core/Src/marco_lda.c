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
#include "mfcc_estagios.h"
#include "texto_fixo.h"

_Static_assert(VT_N_FEATURES == LDA_N_FEATURES,
               "lda_vetores_teste.h e lda_modelo.h com numero de caracteristicas diferente");
/* O lda_escore() vai receber o vetor do mfcc_segmento() (13 medias e 13 desvios).
 * Fica aqui porque este arquivo e sempre compilado no firmware e ja inclui o
 * lda_modelo.h; o harness tem a mesma conferencia. */
_Static_assert(LDA_N_FEATURES == 2 * MFCC_N_COEFS,
               "lda_modelo.h espera outro numero de caracteristicas que o mfcc_segmento() produz");

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

void marco_comparar(float escore, float ref, float tol, uint8_t falha_ref,
                    marco_resultado_t *r)
{
    r->escore = escore;
    r->dif = fabsf(escore - ref);
    r->falha = (uint8_t)lda_e_falha(escore);
    r->ok_escore = (uint8_t)(r->dif <= tol);
    r->ok_decisao = (uint8_t)(r->falha == falha_ref);
}

static void preencher(uint16_t i, float escore, marco_resultado_t *r)
{
    marco_comparar(escore, vt_escore_ref[i], vt_tolerancia[i], vt_falha_ref[i], r);
}

void marco_avaliar(uint16_t i, marco_resultado_t *r)
{
    preencher(i, escore_opaco(vt_x[i]), r);
}

int marco_fmt_fixo(char *buf, size_t n, float v, unsigned casas)
{
    return texto_fixo(buf, n, v, casas);
}

void marco_pacote(uint16_t i, const marco_resultado_t *r, uint32_t n, uint32_t ciclos,
                  pacote_segmento_t *p)
{
    p->n = n;
    p->origem = (char)PACOTE_ORIGEM_REFERENCIA;
    p->clipe = (int32_t)vt_gravacao[i];
    p->segmento = (int32_t)vt_segmento[i];
    p->ciclos = ciclos;
    pacote_decidir(p, r->escore, PACOTE_NOV_NAO_ADOTADO, 0.0f, 0.0f);
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

/* Manda len bytes de s, so se couberem no buffer de tam bytes. O snprintf
 * devolve o tamanho que a linha TERIA: se ela foi cortada, len >= tam, e mandar
 * len bytes leria alem do buffer. Nesse caso nao manda nada. */
static void tx(const char *s, int len, size_t tam)
{
    if (len > 0 && (size_t)len < tam) {
        HAL_UART_Transmit(&huart1, (uint8_t *)s, (uint16_t)len, HAL_MAX_DELAY);
    }
}

static void tx_str(const char *s)
{
    const size_t n = strlen(s);
    tx(s, (int)n, n + 1u);
}

static void leds(int verde, int vermelho)
{
    HAL_GPIO_WritePin(LED_G_GPIO_Port, LED_G_Pin, verde ? GPIO_PIN_SET : GPIO_PIN_RESET);
    HAL_GPIO_WritePin(LED_R_GPIO_Port, LED_R_Pin, vermelho ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

/* Repete o dwt_init() do main.c de proposito: o modulo nao depende de quem o
 * chama ter ligado o contador antes. */
static void dwt_ligar(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

/* Ciclos do escore (com a chamada), descontado o custo de ler o DWT. */
static uint32_t avaliar_medindo(uint16_t i, marco_resultado_t *r, uint32_t custo_leitura)
{
    const float *x = vt_x[i];
    /* Barreiras em volta da janela do DWT. Um "memory" sozinho so impede o
     * compilador de mover acessos a memoria; contas em registrador (como o
     * endereco de vt_x[i]) ainda poderiam cair entre t0 e a chamada. O "+r"(x)
     * obriga o endereco a estar pronto num registrador antes de ler t0. A
     * segunda barreira impede que as leituras de preencher() subam para dentro
     * da janela. */
    __asm volatile("" : "+r"(x) :: "memory");
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
    tx(linha, len, sizeof linha);

    if (!marco_cabecalhos_coerentes()) {
        len = snprintf(linha, sizeof linha,
                       "ERRO: lda_modelo.h e lda_vetores_teste.h sao de modelos diferentes "
                       "(bias diferente); gere os dois do mesmo lda_final.json\r\n");
        tx(linha, len, sizeof linha);
        return 0;
    }

    /* Cabecalho do pacote da serial: o painel confere o modelo antes de mostrar
     * os $SEG da demonstracao. */
    tx(linha, pacote_cabecalho(linha, sizeof linha, VT_MODELO_ID, VT_JSON_SHA256_12,
                               (unsigned)LDA_N_FEATURES,
                               (unsigned)(SystemCoreClock / 1000000u)),
       sizeof linha);

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
        tx(linha, marco_linha(linha, sizeof linha, "seg", i, &r, c), sizeof linha);
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
    tx(linha, len, sizeof linha);

    const int ok = (n_ok == VT_N_SEGMENTOS);
    tx_str(ok ? "MARCO OK\r\n" : "MARCO FALHOU\r\n");
    return ok;
}

#endif /* MARCO_HOST */
