/*
 * decimacao.c -- decimacao embarcada (bloco 0), ver decimacao.h
 *
 * Compilado com -O2 inclusive no Debug (CMakeLists.txt da raiz), como a cadeia,
 * para os ciclos medidos pelo DWT serem os do firmware final.
 */

#include "decimacao.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "arm_math.h"
#include "lda_modelo.h"
#include "mfcc_estagios.h"
#include "texto_fixo.h"

_Static_assert(RECORTE_FATOR == 4 && RECORTE_N_TAPS == 147 && RECORTE_FASE == 0,
               "o recorte do clipes_demo.h foi gerado para outro decimador");
_Static_assert(RECORTE_N_AMOSTRAS == RECORTE_N_BLOCOS_CHEIOS * RECORTE_BLOCO + RECORTE_ULTIMO_BLOCO,
               "recorte com outro numero de amostras que os blocos do clipes_demo.h");
_Static_assert(RECORTE_BLOCO % RECORTE_FATOR == 0 && RECORTE_ULTIMO_BLOCO % RECORTE_FATOR == 0,
               "o arm_fir_decimate_f32 exige blocos multiplos do fator");
_Static_assert(DECIM_N_SAIDAS_TOTAL - RECORTE_DESCARTAR == DECIM_N_SAIDAS,
               "saidas do recorte menos as descartadas tem que dar um segmento");
_Static_assert(DECIM_N_SAIDAS == CLIPES_AMOSTRAS_POR_SEGMENTO && DECIM_N_SAIDAS == MFCC_N_AMOSTRAS,
               "o segmento decimado tem que ter o tamanho do da cadeia");
_Static_assert(RECORTE_FS_HZ / RECORTE_FATOR == CLIPES_FS_HZ, "51,2 kHz / 4 tem que dar 12,8 kHz");

/* Impede o compilador de mover a conta para fora da janela do contador. */
#define BARREIRA() __asm__ volatile("" ::: "memory")

/* Buffers do decimador, nos tamanhos do clipes_demo.h. Estaticos, e nao na pilha
 * (4 KB): so o estado sao 2.632 B. Os nomes comecam por decimador_ para o
 * tools/orcamento_memoria.py. */
static arm_fir_decimate_instance_f32 decimador;
static float decimador_estado[RECORTE_N_TAPS + RECORTE_BLOCO - 1];
static float decimador_x[RECORTE_BLOCO];
static float decimador_y[RECORTE_BLOCO / RECORTE_FATOR];

/* O segmento decimado e requantizado: a entrada do mfcc_segmento(). */
static int16_t decimador_saida[DECIM_N_SAIDAS];

/* Um buffer de texto so para o modulo (um contexto so, sem interrupcao). */
static char linha[320];

const int16_t *decimacao_saida(void) { return decimador_saida; }

static uint32_t descontar(uint32_t bruto, uint32_t custo)
{
    return bruto > custo ? bruto - custo : 0u;
}

int16_t decimacao_quantizar(float y, uint32_t *saturou)
{
    /* lrintf segue o modo corrente (meio-para-par por padrao), como o np.round */
    long q = lrintf(y * 32767.0f);
    if (q > 32767L || q < -32768L) {
        q = q > 0 ? 32767L : -32768L;
        if (saturou) {
            ++*saturou;
        }
    }
    return (int16_t)q;
}

/* ------------------------------------------------------------------------- */
/* Fase                                                                       */
/* ------------------------------------------------------------------------- */
int decimacao_fase(void)
{
    arm_fir_decimate_init_f32(&decimador, RECORTE_N_TAPS, RECORTE_FATOR, recorte_fir,
                              decimador_estado, RECORTE_BLOCO);
    memset(decimador_x, 0, sizeof decimador_x);
    decimador_x[0] = 1.0f;
    arm_fir_decimate_f32(&decimador, decimador_x, decimador_y, RECORTE_BLOCO);

    /* Com x = impulso, y[m] = b[4m + p] (0 se o indice passar do fim do filtro).
     * 1 * b + 0 * ... da b exato, entao a comparacao e exata. */
    for (int p = 0; p < RECORTE_FATOR; ++p) {
        int bate = 1;
        for (int m = 0; m < RECORTE_BLOCO / RECORTE_FATOR && bate; ++m) {
            const int k = RECORTE_FATOR * m + p;
            const float esperado = k < RECORTE_N_TAPS ? recorte_fir[k] : 0.0f;
            bate = (decimador_y[m] == esperado);
        }
        if (bate) {
            return p == 0 ? 0 : 1 + p;
        }
    }
    return -1;
}

/* ------------------------------------------------------------------------- */
/* Decimacao                                                                  */
/* ------------------------------------------------------------------------- */
void decimacao_executar(int16_t *saida, decimacao_observar_fn obs, void *ctx,
                        cadeia_ciclos_fn ciclos, uint32_t custo, decimacao_medida_t *m)
{
    memset(m, 0, sizeof *m);
    uint32_t t0 = 0u, fir = 0u, g = 0u, j = 0u, sat = 0u;

    arm_fir_decimate_init_f32(&decimador, RECORTE_N_TAPS, RECORTE_FATOR, recorte_fir,
                              decimador_estado, RECORTE_BLOCO);
    BARREIRA();
    if (ciclos) {
        t0 = ciclos();
    }
    BARREIRA();

    /* RECORTE_N_BLOCOS_CHEIOS blocos de 512 e um ultimo de 148: sem o ultimo, as
     * 37 ultimas saidas se perdem sem erro (n_saidas fica em 12.763). */
    for (int b = 0; b <= RECORTE_N_BLOCOS_CHEIOS; ++b) {
        const int n = b < RECORTE_N_BLOCOS_CHEIOS ? RECORTE_BLOCO : RECORTE_ULTIMO_BLOCO;
        const int16_t *x = &recorte_51200[(size_t)b * RECORTE_BLOCO];
        /* x = int16 / 32767, por divisao, como o mfcc_escala() e o Python */
        for (int i = 0; i < n; ++i) {
            decimador_x[i] = (float)x[i] / 32767.0f;
        }

        uint32_t f0 = 0u;
        BARREIRA();
        if (ciclos) {
            f0 = ciclos();
        }
        BARREIRA();
        arm_fir_decimate_f32(&decimador, decimador_x, decimador_y, (uint32_t)n);
        BARREIRA();
        if (ciclos) {
            fir += descontar(ciclos() - f0, custo);
        }
        BARREIRA();

        /* as RECORTE_DESCARTAR primeiras saidas (todas no 1o bloco) tem os zeros do
         * estado inicial; as outras sao o segmento */
        const int n_y = n / RECORTE_FATOR;
        for (int i = 0; i < n_y; ++i, ++g) {
            if (g < RECORTE_DESCARTAR || j >= DECIM_N_SAIDAS) {
                continue;
            }
            const float y = decimador_y[i];
            if (saida) {
                saida[j] = decimacao_quantizar(y, &sat);
            }
            if (obs) {
                obs(ctx, j, y);
            }
            ++j;
        }
    }

    BARREIRA();
    if (ciclos) {
        m->ciclos = descontar(ciclos() - t0, custo);
    }
    BARREIRA();
    m->ciclos_fir = fir;
    m->n_saidas = j;
    m->n_total = g;
    m->n_saturadas = sat;
}

/* ------------------------------------------------------------------------- */
/* Conferencia                                                                */
/* ------------------------------------------------------------------------- */
typedef struct {
    const int16_t *ref;
    const int16_t *saida;
    decimacao_observar_fn extra;
    void *ctx_extra;
    decimacao_comparacao_t *c;
} conferencia_t;

static void observar(void *ctx, uint32_t j, float y)
{
    conferencia_t *k = (conferencia_t *)ctx;
    decimacao_comparacao_t *c = k->c;
    const float r = (float)k->ref[j];
    if (fabsf(r) > c->pico) {
        c->pico = fabsf(r);
    }

    const float e = fabsf(y * 32767.0f - r);
    if (!(e <= c->max_lsb)) {   /* tambem pega NaN */
        c->max_lsb = e;
        c->j_max_lsb = j;
    }
    const int16_t q = decimacao_quantizar(y, NULL);
    const int32_t d = q > k->ref[j] ? (int32_t)q - k->ref[j] : (int32_t)k->ref[j] - q;
    if (d > c->dif_int16_max) {
        c->dif_int16_max = d;
    }
    c->n_dif += (d != 0);
    if (k->saida && q != k->saida[j]) {
        c->iguais = 0u;
    }
    if (k->extra) {
        k->extra(k->ctx_extra, j, y);
    }
}

void decimacao_conferir(const int16_t *ref, const int16_t *saida, decimacao_observar_fn extra,
                        void *ctx, decimacao_comparacao_t *c)
{
    memset(c, 0, sizeof *c);
    c->iguais = 1u;
    conferencia_t k = {ref, saida, extra, ctx, c};
    decimacao_medida_t m;
    decimacao_executar(NULL, observar, &k, NULL, 0u, &m);
    if (m.n_saidas != DECIM_N_SAIDAS) {
        c->iguais = 0u;
    }
}

int decimacao_avaliar(const decimacao_medida_t *m, decimacao_comparacao_t *c)
{
    c->ok_saida = (uint8_t)(m->n_saidas == DECIM_N_SAIDAS && m->n_total == DECIM_N_SAIDAS_TOTAL
                            && m->n_saturadas == 0u && c->max_lsb <= DECIM_TOL_LSB_PLACA(c->pico)
                            && c->dif_int16_max <= DECIM_MAX_DIF_INT16 && c->n_dif <= DECIM_MAX_N_DIF
                            && c->iguais);
    return c->ok_saida;
}

/* CRC-32 (zlib), por tabela de 16 entradas, como o do blocos13.c */
uint32_t decimacao_crc32(const int16_t *x, size_t n)
{
    static const uint32_t t[16] = {
        0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu, 0x76DC4190u, 0x6B6B51F4u,
        0x4DB26158u, 0x5005713Cu, 0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
        0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu};
    uint32_t crc = ~0u;
    for (size_t i = 0; i < n; ++i) {
        const uint16_t v = (uint16_t)x[i];
        const uint8_t bytes[2] = {(uint8_t)(v & 0xFFu), (uint8_t)(v >> 8)};   /* little-endian */
        for (int b = 0; b < 2; ++b) {
            crc ^= bytes[b];
            crc = (crc >> 4) ^ t[crc & 0x0Fu];
            crc = (crc >> 4) ^ t[crc & 0x0Fu];
        }
    }
    return ~crc;
}

/* ------------------------------------------------------------------------- */
/* Verificacao (sem HAL: a saida vai por `enviar`)                            */
/* ------------------------------------------------------------------------- */
static void fmt_ms(char *buf, size_t tam, uint32_t ciclos, uint32_t clock_hz)
{
    const uint32_t por_ms = clock_hz / 1000u;
    if (por_ms == 0u) {
        snprintf(buf, tam, "0.00");
        return;
    }
    const uint64_t x100 = ((uint64_t)ciclos * 100u + por_ms / 2u) / por_ms;
    snprintf(buf, tam, "%lu.%02lu", (unsigned long)(x100 / 100u), (unsigned long)(x100 % 100u));
}

/* porcentagem do tempo real de 1 s de audio, com uma casa */
static void fmt_pct(char *buf, size_t tam, uint32_t ciclos, uint32_t clock_hz)
{
    const uint64_t x10 = clock_hz ? ((uint64_t)ciclos * 1000u + clock_hz / 2u) / clock_hz : 0u;
    snprintf(buf, tam, "%lu.%lu", (unsigned long)(x10 / 10u), (unsigned long)(x10 % 10u));
}

static void enviar_linha(cadeia_enviar_fn enviar, int len)
{
    if (len > 0 && (size_t)len < sizeof linha) {
        enviar(linha, (size_t)len);
    } else {
        static const char aviso[] = "decim ERRO linha_longa\r\n";
        enviar(aviso, sizeof aviso - 1u);
    }
}

static int falhou(cadeia_enviar_fn enviar)
{
    static const char s[] = "DECIMACAO FALHOU\r\n";
    enviar(s, sizeof s - 1u);
    return 0;
}

int decimacao_verificar_com(cadeia_enviar_fn enviar, cadeia_ciclos_fn ciclos, uint32_t clock_hz)
{
    int len = snprintf(linha, sizeof linha,
                       "decim inicio recorte=seg%d clipe=%s fs=%d->%d taps=%d fator=%d bloco=%d "
                       "amostras=%d descartar=%d\r\n",
                       RECORTE_SEGMENTO, clipes[RECORTE_CLIPE].classe, RECORTE_FS_HZ, CLIPES_FS_HZ,
                       RECORTE_N_TAPS, RECORTE_FATOR, RECORTE_BLOCO, RECORTE_N_AMOSTRAS,
                       RECORTE_DESCARTAR);
    enviar_linha(enviar, len);

    /* custo de ler o contador duas vezes seguidas (o menor de 16) */
    uint32_t custo = UINT32_MAX;
    for (int i = 0; i < 16; ++i) {
        BARREIRA();
        const uint32_t a = ciclos();
        BARREIRA();
        const uint32_t b = ciclos();
        BARREIRA();
        custo = (b - a) < custo ? (b - a) : custo;
    }

    /* 1. fase da CMSIS: o recorte so vale para a fase 0 */
    const int fase = decimacao_fase();
    if (fase == 0) {
        len = snprintf(linha, sizeof linha, "decim fase=0 OK\r\n");
    } else if (fase > 0) {
        len = snprintf(linha, sizeof linha, "decim fase=%d FALHOU (o recorte espera a fase 0)\r\n", fase - 1);
    } else {
        len = snprintf(linha, sizeof linha, "decim fase=? FALHOU (impulso nao da nenhuma fase)\r\n");
    }
    enviar_linha(enviar, len);
    if (fase != 0) {
        return falhou(enviar);
    }

    /* 2. a passada medida (o caminho do firmware) e a conferencia */
    decimacao_medida_t m;
    decimacao_executar(decimador_saida, NULL, NULL, ciclos, custo, &m);
    decimacao_comparacao_t c;
    const int16_t *ref = clipes[RECORTE_CLIPE].pcm;   /* o segmento 29 e o 1o do clipe normal */
    decimacao_conferir(ref, decimador_saida, NULL, NULL, &c);
    const int ok_saida = decimacao_avaliar(&m, &c);

    char s_lsb[20], s_tol[20];
    texto_fixo(s_lsb, sizeof s_lsb, c.max_lsb, 6);
    texto_fixo(s_tol, sizeof s_tol, DECIM_TOL_LSB_PLACA(c.pico), 6);
    len = snprintf(linha, sizeof linha,
                   "decim saida n=%lu/%lu saturadas=%lu max_lsb=%s (j=%lu) tol=%s pico=%ld dif_int16_max=%ld "
                   "n_dif=%lu/%lu iguais=%u crc32=%08lX crc32_ref=%08lX %s\r\n",
                   (unsigned long)m.n_saidas, (unsigned long)m.n_total, (unsigned long)m.n_saturadas,
                   s_lsb, (unsigned long)c.j_max_lsb, s_tol, (long)c.pico, (long)c.dif_int16_max,
                   (unsigned long)c.n_dif, (unsigned long)DECIM_MAX_N_DIF, (unsigned)c.iguais,
                   (unsigned long)decimacao_crc32(decimador_saida, DECIM_N_SAIDAS),
                   (unsigned long)decimacao_crc32(ref, DECIM_N_SAIDAS), ok_saida ? "OK" : "FALHOU");
    enviar_linha(enviar, len);

    char s_tot[16], s_fir[16], s_resto[16], s_pct[16];
    fmt_ms(s_tot, sizeof s_tot, m.ciclos, clock_hz);
    fmt_ms(s_fir, sizeof s_fir, m.ciclos_fir, clock_hz);
    const uint32_t resto = m.ciclos > m.ciclos_fir ? m.ciclos - m.ciclos_fir : 0u;
    fmt_ms(s_resto, sizeof s_resto, resto, clock_hz);
    fmt_pct(s_pct, sizeof s_pct, m.ciclos, clock_hz);
    len = snprintf(linha, sizeof linha,
                   "decim ciclos total=%lu (%s ms) fir=%lu (%s ms) conversao_e_quantizacao=%lu (%s ms) "
                   "tempo_real=%s%%\r\n",
                   (unsigned long)m.ciclos, s_tot, (unsigned long)m.ciclos_fir, s_fir,
                   (unsigned long)resto, s_resto, s_pct);
    enviar_linha(enviar, len);

    /* 3. o segmento decimado pela cadeia ja validada */
    if (mfcc_estagios_init() != 0) {
        len = snprintf(linha, sizeof linha, "decim ERRO mfcc_estagios_init\r\n");
        enviar_linha(enviar, len);
        return falhou(enviar);
    }
    static float x[2 * MFCC_N_COEFS];
    const int16_t *pcm = decimador_saida;
    float *px = x;
    __asm__ volatile("" : "+r"(pcm), "+r"(px) :: "memory");
    const uint32_t t0 = ciclos();
    BARREIRA();
    const int st = mfcc_segmento(pcm, px);
    const float escore = lda_escore(px);
    BARREIRA();
    const uint32_t t1 = ciclos();
    BARREIRA();
    if (st != 0) {
        len = snprintf(linha, sizeof linha, "decim ERRO mfcc_segmento=%d\r\n", st);
        enviar_linha(enviar, len);
        return falhou(enviar);
    }
    const uint32_t c_cadeia = descontar(t1 - t0, custo);

    /* o recorte e um segmento normal (clipe 0, segmento 29) */
    cadeia_resultado_t r;
    cadeia_comparar(escore, RECORTE_ESCORE_REF_INT16, 0u, &r);
    const int igual_clipe = memcmp(decimador_saida, ref, sizeof decimador_saida) == 0;
    char s_ref[20], s_placa[20], s_dif[20], s_tol_e[20];
    texto_fixo(s_ref, sizeof s_ref, r.ref, 3);
    texto_fixo(s_placa, sizeof s_placa, r.escore, 3);
    texto_fixo(s_dif, sizeof s_dif, r.dif, 6);
    texto_fixo(s_tol_e, sizeof s_tol_e, r.tol, 6);
    char s_cad[16];
    fmt_ms(s_cad, sizeof s_cad, c_cadeia, clock_hz);
    len = snprintf(linha, sizeof linha,
                   "decim escore ref=%s placa=%s dif=%s tol=%s dec=%s igual_clipe=%d ciclos_cadeia=%lu "
                   "(%s ms) %s\r\n",
                   s_ref, s_placa, s_dif, s_tol_e, r.falha ? "falha" : "normal", igual_clipe,
                   (unsigned long)c_cadeia, s_cad, (r.ok_escore && r.ok_decisao) ? "OK" : "FALHOU");
    enviar_linha(enviar, len);

    /* 4. do int16 a 51,2 kHz ao escore */
    const uint32_t c_tudo = m.ciclos + c_cadeia;
    fmt_ms(s_tot, sizeof s_tot, c_tudo, clock_hz);
    fmt_pct(s_pct, sizeof s_pct, c_tudo, clock_hz);
    len = snprintf(linha, sizeof linha,
                   "decim cadeia_completa ciclos=%lu (%s ms, %s%% do tempo real) decimacao=%lu cadeia=%lu\r\n",
                   (unsigned long)c_tudo, s_tot, s_pct, (unsigned long)m.ciclos, (unsigned long)c_cadeia);
    enviar_linha(enviar, len);

    const int ok = ok_saida && r.ok_escore && r.ok_decisao;
    if (!ok) {
        return falhou(enviar);
    }
    static const char s_ok[] = "DECIMACAO OK\r\n";
    enviar(s_ok, sizeof s_ok - 1u);
    return 1;
}

/* ------------------------------------------------------------------------- */
/* Com HAL                                                                    */
/* ------------------------------------------------------------------------- */
#ifndef DECIMACAO_HOST

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

int decimacao_verificar(void)
{
    /* liga o DWT de novo de proposito: o modulo nao depende de quem chama */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    return decimacao_verificar_com(enviar_uart, ler_dwt, SystemCoreClock);
}

#endif /* DECIMACAO_HOST */
