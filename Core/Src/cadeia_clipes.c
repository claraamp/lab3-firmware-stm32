/*
 * cadeia_clipes.c -- cadeia completa na placa com os clipes da flash
 *
 * Ver cadeia_clipes.h. Compilado com -O2 inclusive no Debug (CMakeLists.txt da
 * raiz), como o marco_lda.c e o mfcc_estagios.c, para os ciclos medidos pelo DWT
 * serem os do firmware final.
 */

#include "cadeia_clipes.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "clipes_demo.h"
#include "lda_modelo.h"
#include "mfcc_estagios.h"
#include "texto_fixo.h"

_Static_assert(LDA_N_FEATURES == 2 * MFCC_N_COEFS,
               "lda_modelo.h espera outro numero de caracteristicas que o mfcc_segmento() produz");
_Static_assert(CLIPES_AMOSTRAS_POR_SEGMENTO == MFCC_N_AMOSTRAS,
               "clipes_demo.h com segmentos de tamanho diferente do da cadeia");
_Static_assert(CLIPES_FS_HZ == 12800, "a cadeia espera os clipes a 12,8 kHz");

/* Impede o compilador de mover a conta para fora da janela do contador. */
#define BARREIRA() __asm__ volatile("" ::: "memory")

static const char *const NOME_BLOCO[CADEIA_N_BLOCOS] = {
    "janela", "espectro", "mel", "log", "dct", "resumo", "final", "lda"};

/* Custo de ler o contador, medido no cadeia_verificar_com() e usado no laco. */
static uint32_t custo_leitura_dwt = 0u;

/* Um buffer de texto so para o modulo: a verificacao do boot, o $CAB e o laco nunca
 * escrevem ao mesmo tempo (um so contexto, sem interrupcao). Estatico, e nao na
 * pilha. (Os parametros `linha` do acrescentar() e do enviar_linha() sao outros.) */
static char linha[320];
_Static_assert(sizeof linha >= PACOTE_TAM_MAX, "o $SEG e o $CAB tem que caber no buffer");

uint16_t cadeia_n_segmentos(void) { return CLIPES_N_SEGMENTOS; }

/* Contador dos $SEG do laco, um so para as origens C e D. */
static uint32_t n_pacotes = 0u;

uint32_t cadeia_proximo_pacote(void) { return n_pacotes++; }

const char *cadeia_nome_bloco(int b)
{
    return (b >= 0 && b < CADEIA_N_BLOCOS) ? NOME_BLOCO[b] : "?";
}

static uint32_t descontar(uint32_t bruto, uint32_t custo)
{
    return bruto > custo ? bruto - custo : 0u;
}

/* O PCM do segmento s: o segmento k do clipe comeca em k * 12.800 amostras. */
static const int16_t *pcm_segmento(uint16_t s, uint16_t *k_out)
{
    const clipe_segmento_t *seg = &clipes_segmentos[s];
    const clipe_t *c = &clipes[seg->clipe];
    const uint16_t k = (uint16_t)(s - c->primeiro_segmento);
    if (k_out) {
        *k_out = k;
    }
    return &c->pcm[(size_t)k * CLIPES_AMOSTRAS_POR_SEGMENTO];
}

void cadeia_comparar(float escore, float ref, uint8_t falha_ref, cadeia_resultado_t *r)
{
    r->escore = escore;
    r->ref = ref;
    r->dif = fabsf(escore - ref);
    r->tol = CADEIA_TOL_ESCORE_REL * fabsf(ref);
    r->falha = (uint8_t)lda_e_falha(escore);
    /* NaN em escore da dif NaN, e NaN <= tol e falso: reprova, como deve */
    r->ok_escore = (uint8_t)(r->dif <= r->tol);
    r->ok_decisao = (uint8_t)(r->falha == falha_ref);
}

/* As 26 caracteristicas da ultima passada do caminho do firmware, para a passada
 * bloco a bloco conferir que da o mesmo. */
static float x_firmware[2 * MFCC_N_COEFS];

int cadeia_inferir(uint16_t s, cadeia_ciclos_fn ciclos, uint32_t custo, cadeia_resultado_t *r)
{
    memset(r, 0, sizeof *r);
    const int16_t *pcm = pcm_segmento(s, NULL);
    float *x = x_firmware;

    /* "+r"(pcm) e "+r"(x): os enderecos ficam prontos em registrador antes de t0 */
    __asm__ volatile("" : "+r"(pcm), "+r"(x) :: "memory");
    const uint32_t t0 = ciclos();
    BARREIRA();
    const int st = mfcc_segmento(pcm, x);
    const float escore = lda_escore(x);
    BARREIRA();
    const uint32_t t1 = ciclos();
    BARREIRA();

    if (st != 0) {
        return st;
    }
    cadeia_comparar(escore, clipes_segmentos[s].escore_ref, clipes_segmentos[s].falha_ref, r);
    r->ciclos = descontar(t1 - t0, custo);
    return 0;
}

/* Buffers de um quadro da passada bloco a bloco: estaticos (2,4 KB), pelo mesmo
 * motivo dos do mfcc_estagios.c (o linker so reserva 4 KB de pilha e nao avisa
 * se a pilha real passar disso). */
static float b_quadro[MFCC_N_QUADRO];
static float b_potencia[MFCC_N_BINS];
static float b_mel[MFCC_N_MEL];
static float b_coefs[MFCC_N_COEFS];

/* Mede `chamada` entre duas leituras do contador e soma em `acc`. */
#define MEDIR(acc, chamada)                       \
    do {                                          \
        BARREIRA();                               \
        const uint32_t m0_ = ciclos();            \
        BARREIRA();                               \
        chamada;                                  \
        BARREIRA();                               \
        const uint32_t m1_ = ciclos();            \
        BARREIRA();                               \
        (acc) += descontar(m1_ - m0_, custo);     \
    } while (0)

int cadeia_avaliar(uint16_t s, cadeia_ciclos_fn ciclos, uint32_t custo, cadeia_resultado_t *r)
{
    const int st = cadeia_inferir(s, ciclos, custo, r);
    if (st != 0) {
        return st;
    }

    /* Segunda passada no mesmo segmento, com os estagios do mfcc_quadro_coefs()
     * chamados um a um, na mesma ordem, para medir cada bloco. */
    const int16_t *pcm = pcm_segmento(s, NULL);
    uint32_t *b = r->blocos;
    /* estaticos, e nao na pilha: o caminho mais fundo do boot (verificacao ->
     * cadeia_linha -> vsnprintf, mais uma interrupcao com o contexto da FPU) ja
     * chega a ~1,05 KB; por isso a reserva da pilha subiu para 4 KB em 20/10 */
    static mfcc_resumo_t res;
    static float x[2 * MFCC_N_COEFS];
    float escore = 0.0f;

    mfcc_resumo_iniciar(&res);
    for (int q = 0; q < MFCC_N_QUADROS; ++q) {
        const int16_t *pq = &pcm[q * MFCC_PASSO];
        MEDIR(b[CADEIA_B_JANELA], mfcc_quadro_pcm(pq, b_quadro));
        MEDIR(b[CADEIA_B_ESPECTRO], mfcc_espectro(b_quadro, b_potencia));
        MEDIR(b[CADEIA_B_MEL], mfcc_mel(b_potencia, b_mel));
        MEDIR(b[CADEIA_B_LOG], mfcc_log(b_mel, b_mel));
        MEDIR(b[CADEIA_B_DCT], mfcc_dct(b_mel, b_coefs));
        MEDIR(b[CADEIA_B_RESUMO], mfcc_resumo_acumular(&res, b_coefs));
    }
    MEDIR(b[CADEIA_B_FINAL], mfcc_resumo_finalizar(&res, &x[0], &x[MFCC_N_COEFS]));
    MEDIR(b[CADEIA_B_LDA], escore = lda_escore(x));

    r->iguais = (uint8_t)(memcmp(x, x_firmware, sizeof x) == 0 && escore == r->escore);
    return 0;
}

/* Acrescenta texto a linha a partir de n e devolve o novo tamanho, ou -1 se nao
 * couber (e continua -1 nas chamadas seguintes). */
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

int cadeia_linha(char *buf, size_t n, const char *prefixo, uint16_t s,
                 const cadeia_resultado_t *r)
{
    uint16_t k;
    (void)pcm_segmento(s, &k);
    const clipe_segmento_t *seg = &clipes_segmentos[s];
    char ref[20], placa[20], dif[20], tol[20];
    texto_fixo(ref, sizeof ref, r->ref, 3);
    texto_fixo(placa, sizeof placa, r->escore, 3);
    texto_fixo(dif, sizeof dif, r->dif, 6);
    texto_fixo(tol, sizeof tol, r->tol, 6);

    int m = acrescentar(buf, n, 0,
                        "%s seg=%u clipe=%s k=%u ref=%s placa=%s dif=%s tol=%s dec=%s %s ciclos=%lu",
                        prefixo, (unsigned)seg->id, clipes[seg->clipe].classe, (unsigned)k, ref,
                        placa, dif, tol, r->falha ? "falha" : "normal",
                        (r->ok_escore && r->ok_decisao) ? "OK" : "FALHOU",
                        (unsigned long)r->ciclos);
    uint32_t soma = 0u;
    for (int i = 0; i < CADEIA_N_BLOCOS; ++i) {
        soma += r->blocos[i];
    }
    if (soma != 0u) {
        for (int i = 0; i < CADEIA_N_BLOCOS; ++i) {
            m = acrescentar(buf, n, m, "%s%s:%lu", i ? "," : " blocos=", NOME_BLOCO[i],
                            (unsigned long)r->blocos[i]);
        }
        m = acrescentar(buf, n, m, " soma=%lu iguais=%u", (unsigned long)soma, (unsigned)r->iguais);
    }
    return acrescentar(buf, n, m, "\r\n");
}

void cadeia_pacote(uint16_t s, const cadeia_resultado_t *r, uint32_t n, pacote_segmento_t *p)
{
    uint16_t k;
    (void)pcm_segmento(s, &k);
    p->n = n;
    p->origem = (char)PACOTE_ORIGEM_CLIPE;
    p->clipe = (int32_t)clipes_segmentos[s].clipe;
    p->segmento = (int32_t)k;
    p->ciclos = r->ciclos;
    pacote_decidir(p, r->escore, PACOTE_NOV_NAO_ADOTADO, 0.0f, 0.0f);
}

/* ------------------------------------------------------------------------- */
/* Verificacao (sem HAL: a saida vai por `enviar`)                            */
/* ------------------------------------------------------------------------- */
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

/* mediana inferior de n valores (ordena v) */
static uint32_t mediana(uint32_t *v, int n)
{
    ordenar(v, n);
    return v[(n - 1) / 2];
}

/* milissegundos com duas casas, sem %f (o printf do nano.specs nao tem) */
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

static void enviar_linha(cadeia_enviar_fn enviar, const char *linha, int len, size_t tam)
{
    if (len > 0 && (size_t)len < tam) {
        enviar(linha, (size_t)len);
    } else {
        /* nao acontece com os tamanhos de hoje; uma linha cortada confundiria o leitor */
        static const char aviso[] = "cadeia ERRO linha_longa\r\n";
        enviar(aviso, sizeof aviso - 1u);
    }
}

int cadeia_verificar_com(cadeia_enviar_fn enviar, cadeia_ciclos_fn ciclos, uint32_t clock_hz)
{
    /* estaticos, e nao na pilha (a linha e a do modulo) */
    static uint32_t tot[CLIPES_N_SEGMENTOS];
    static uint32_t por_bloco[CADEIA_N_BLOCOS][CLIPES_N_SEGMENTOS];
    int len;

    len = snprintf(linha, sizeof linha,
                   "cadeia inicio modelo=%s json=%s segmentos=%d origem=C clock=%luMHz\r\n",
                   CLIPES_MODELO_ID, CLIPES_JSON_SHA256_12, CLIPES_N_SEGMENTOS,
                   (unsigned long)(clock_hz / 1000000u));
    enviar_linha(enviar, linha, len, sizeof linha);

    const int st = mfcc_estagios_init();
    if (st != 0) {
        len = snprintf(linha, sizeof linha, "cadeia ERRO mfcc_estagios_init=%d\r\nCADEIA FALHOU\r\n", st);
        enviar_linha(enviar, linha, len, sizeof linha);
        return 0;
    }

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
    custo_leitura_dwt = custo;

    uint32_t n_ok = 0u, n_dec = 0u, n_iguais = 0u;
    float razao_max = 0.0f, dif_max = 0.0f;
    uint16_t seg_dif_max = 0u;

    for (uint16_t s = 0; s < CLIPES_N_SEGMENTOS; ++s) {
        cadeia_resultado_t r;
        const int e = cadeia_avaliar(s, ciclos, custo, &r);
        if (e != 0) {
            len = snprintf(linha, sizeof linha, "cadeia ERRO mfcc_segmento=%d seg=%u\r\nCADEIA FALHOU\r\n",
                           e, (unsigned)clipes_segmentos[s].id);
            enviar_linha(enviar, linha, len, sizeof linha);
            return 0;
        }
        n_ok += (r.ok_escore && r.ok_decisao);
        n_dec += r.ok_decisao;
        n_iguais += r.iguais;
        const float razao = r.tol > 0.0f ? r.dif / r.tol : INFINITY;
        if (!(razao <= razao_max)) {   /* tambem pega NaN */
            razao_max = razao;
        }
        if (r.dif > dif_max) {
            dif_max = r.dif;
            seg_dif_max = clipes_segmentos[s].id;
        }
        tot[s] = r.ciclos;
        for (int b = 0; b < CADEIA_N_BLOCOS; ++b) {
            por_bloco[b][s] = r.blocos[b];
        }
        enviar_linha(enviar, linha, cadeia_linha(linha, sizeof linha, "cadeia", s, &r), sizeof linha);
    }

    /* resumo: min, mediana e max do segmento inteiro */
    ordenar(tot, CLIPES_N_SEGMENTOS);
    const uint32_t c_min = tot[0], c_max = tot[CLIPES_N_SEGMENTOS - 1];
    const uint32_t c_med = tot[(CLIPES_N_SEGMENTOS - 1) / 2];
    char s_dif[20], s_razao[20], s_ms[16], s_ms_max[16];
    texto_fixo(s_dif, sizeof s_dif, dif_max, 6);
    texto_fixo(s_razao, sizeof s_razao, 100.0f * razao_max, 1);
    fmt_ms(s_ms, sizeof s_ms, c_med, clock_hz);
    fmt_ms(s_ms_max, sizeof s_ms_max, c_max, clock_hz);
    len = snprintf(linha, sizeof linha,
                   "cadeia resumo ok=%lu/%d decisoes_iguais=%lu/%d iguais_blocos=%lu/%d dif_max=%s "
                   "(seg %u) dif/tol_max=%s%% ciclos min=%lu med=%lu max=%lu (med %s ms, max %s ms)\r\n",
                   (unsigned long)n_ok, CLIPES_N_SEGMENTOS, (unsigned long)n_dec, CLIPES_N_SEGMENTOS,
                   (unsigned long)n_iguais, CLIPES_N_SEGMENTOS, s_dif, (unsigned)seg_dif_max, s_razao,
                   (unsigned long)c_min, (unsigned long)c_med, (unsigned long)c_max, s_ms, s_ms_max);
    enviar_linha(enviar, linha, len, sizeof linha);

    /* mediana de cada bloco entre os 10 segmentos (ciclos por segmento de 1 s) */
    int m = acrescentar(linha, sizeof linha, 0, "cadeia blocos_mediana");
    uint32_t soma_med = 0u;
    for (int b = 0; b < CADEIA_N_BLOCOS; ++b) {
        const uint32_t med = mediana(por_bloco[b], CLIPES_N_SEGMENTOS);
        soma_med += med;
        m = acrescentar(linha, sizeof linha, m, " %s=%lu", NOME_BLOCO[b], (unsigned long)med);
    }
    fmt_ms(s_ms, sizeof s_ms, soma_med, clock_hz);
    m = acrescentar(linha, sizeof linha, m, " soma=%lu (%s ms)\r\n", (unsigned long)soma_med, s_ms);
    enviar_linha(enviar, linha, m, sizeof linha);

    /* iguais_blocos nao reprova: so diz se a passada de medida bate com a do
     * firmware. O que decide e o escore e a decisao. */
    const int ok = (n_ok == CLIPES_N_SEGMENTOS);
    static const char s_ok[] = "CADEIA OK\r\n", s_falhou[] = "CADEIA FALHOU\r\n";
    if (ok) {
        enviar(s_ok, sizeof s_ok - 1u);
    } else {
        enviar(s_falhou, sizeof s_falhou - 1u);
    }
    return ok;
}

/* ------------------------------------------------------------------------- */
/* Com HAL                                                                    */
/* ------------------------------------------------------------------------- */
#ifndef CADEIA_HOST

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

static void leds(int verde, int vermelho)
{
    HAL_GPIO_WritePin(LED_G_GPIO_Port, LED_G_Pin, verde ? GPIO_PIN_SET : GPIO_PIN_RESET);
    HAL_GPIO_WritePin(LED_R_GPIO_Port, LED_R_Pin, vermelho ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

void cadeia_piscar_erro(void)
{
    for (int j = 0; j < 16; ++j) {
        leds(j & 1, j & 1);
        HAL_Delay(125);
    }
}

int cadeia_verificar(void)
{
    /* liga o DWT de novo de proposito: o modulo nao depende de quem chama */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    const int ok = cadeia_verificar_com(enviar_uart, ler_dwt, SystemCoreClock);

    /* $CAB de novo, logo antes dos $SEG de origem C: o painel confere o modelo dos
     * clipes, e a demonstracao nao depende do $CAB do marco */
    const int len = pacote_cabecalho(linha, sizeof linha, CLIPES_MODELO_ID, CLIPES_JSON_SHA256_12,
                                     (unsigned)LDA_N_FEATURES,
                                     (unsigned)(SystemCoreClock / 1000000u));
    if (len > 0) {
        enviar_uart(linha, (size_t)len);
    }
    return ok;
}

void cadeia_mostrar(const pacote_segmento_t *p)
{
    if (p->disparou & PACOTE_ERRO) {
        cadeia_piscar_erro();   /* escore nao finito: nunca o verde de "normal" */
    } else {
        leds(!p->falha, p->falha);
        HAL_Delay(2000);
    }
    leds(0, 0);
    HAL_Delay(300);
}

int cadeia_passo(int ok)
{
    static uint16_t s = 0u;

    if (!ok) {
        cadeia_piscar_erro();
        static const char aviso[] =
            "ERRO: verificacao do marco, da cadeia ou da decimacao falhou (ver as linhas FALHOU acima)\r\n";
        enviar_uart(aviso, sizeof aviso - 1u);
        return 0;
    }

    cadeia_resultado_t r;
    if (cadeia_inferir(s, ler_dwt, custo_leitura_dwt, &r) != 0) {
        cadeia_piscar_erro();
        static const char aviso[] = "ERRO: mfcc_segmento sem init\r\n";
        enviar_uart(aviso, sizeof aviso - 1u);
        return 0;
    }
    pacote_segmento_t p;
    cadeia_pacote(s, &r, cadeia_proximo_pacote(), &p);

    int len = cadeia_linha(linha, sizeof linha, "demo", s, &r);
    if (len > 0) {
        enviar_uart(linha, (size_t)len);
    }
    len = pacote_linha(linha, sizeof linha, &p);
    if (len > 0) {
        enviar_uart(linha, (size_t)len);
    }

    cadeia_mostrar(&p);
    s = (uint16_t)((s + 1u) % CLIPES_N_SEGMENTOS);
    return s == 0u;   /* acabou a volta */
}

#endif /* CADEIA_HOST */
