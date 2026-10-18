/*
 * mfcc_estagios.c -- estagios da extracao de caracteristicas em float32 (CMSIS-DSP)
 *
 * Ver mfcc_estagios.h. Cada funcao segue uma linha da tabela "Cadeia, estagio a
 * estagio" do docs/contrato_numerico.md do repositorio do Python.
 */

#include "mfcc_estagios.h"

#include <math.h>
#include <string.h>

#include "arm_math.h"

static float janela_hann[MFCC_N_QUADRO];
static arm_rfft_fast_instance_f32 rfft;
static int pronto;   /* 1 depois de um mfcc_estagios_init() sem erro */

/* Bordas do banco de Mel, copiadas do docs/contrato_numerico.md ("Valores exatos
 * para conferir"): floor(513 * f / 12800) nos 22 pontos igualmente espacados na
 * escala HTK, de 20 Hz a 6.400 Hz. Sao constantes da cadeia congelada em 13/10; o
 * teste tests/host/test_mel_bordas.c recalcula pela formula e confere. Com elas, a
 * correcao de filtros degenerados do dsp.py (c == l ou r == c) nunca e acionada. */
static const uint16_t mel_bordas[MFCC_N_MEL + 2] = {
    0, 4, 7, 11, 16, 21, 27, 33, 40, 48, 57, 67, 78, 90, 104, 119, 136, 155, 177, 200, 227, 256};

/* Pesos nao nulos de cada filtro, em sequencia: o filtro m comeca no bin
 * mel_bordas[m] e tem mel_bordas[m + 2] - mel_bordas[m] pesos, a partir de
 * mel_pesos[mel_inicio[m]]. O primeiro peso de cada filtro (bin l) e 0, como no
 * Python; fica na tabela para o indice do bin continuar simples. */
static float mel_pesos[MFCC_N_PESOS_MEL];
static uint16_t mel_inicio[MFCC_N_MEL];

/* Matriz da DCT-II ortonormal, 13 x 20 (1.040 B), calculada no init:
 * dct_matriz[k][n] = s(k) cos(pi k (2n + 1) / 40), s(0) = sqrt(1/20), s(k>0) = sqrt(2/20). */
static float dct_matriz[MFCC_N_COEFS][MFCC_N_MEL];

/* cos(pi m / (2 MFCC_N_MEL)) para m inteiro, com a reducao feita em inteiros (periodo
 * 4N = 80, simetrias em 2N = 40 e N = 20), para o cosf so ver angulos de 0 a pi/2.
 * Sem a reducao, o angulo chega a 36,7 rad e o arredondamento dele em float32 erra
 * o coeficiente em ate 6e-7; com ela, o erro fica em ~2 ulp (3e-8). */
static float cos_pi_m_2n(int m)
{
    const int quatro_n = 4 * MFCC_N_MEL, dois_n = 2 * MFCC_N_MEL;
    m %= quatro_n;
    if (m > dois_n) {
        m = quatro_n - m;            /* cos(2 pi - a) = cos(a) */
    }
    float sinal = 1.0f;
    if (m > MFCC_N_MEL) {
        m = dois_n - m;              /* cos(pi - a) = -cos(a) */
        sinal = -1.0f;
    }
    return sinal * cosf(3.14159265358979323846f * (float)m / (float)dois_n);
}

int mfcc_estagios_init(void)
{
    pronto = 0;
    /* w[n] = 0,5 - 0,5 cos(2 pi n / (N - 1)), simetrica: calcula a metade e espelha,
     * para w[n] = w[N-1-n] exato e w[0] = w[N-1] = 0. */
    const float k = 6.28318530717958647692f / (float)(MFCC_N_QUADRO - 1);
    for (int n = 0; n < MFCC_N_QUADRO / 2; ++n) {
        const float w = 0.5f - 0.5f * cosf(k * (float)n);
        janela_hann[n] = w;
        janela_hann[MFCC_N_QUADRO - 1 - n] = w;
    }
    janela_hann[0] = 0.0f;
    janela_hann[MFCC_N_QUADRO - 1] = 0.0f;

    /* Banco de Mel (dsp.mel_filterbank): subida (k - l)/(c - l) em [l, c) e descida
     * (r - k)/(r - c) em [c, r); o bin r fica fora.
     * Nunca escreve alem do fim de mel_pesos: as bordas sao conferidas antes do
     * laco (total de pesos), e o laco para com -2 se for escrever alem do fim (um
     * erro no proprio laco, como incluir o bin r) ou se terminar antes dele. */
    int total = 0;
    for (int m = 0; m < MFCC_N_MEL; ++m) {
        if (mel_bordas[m + 1] <= mel_bordas[m] || mel_bordas[m + 2] <= mel_bordas[m + 1]
                || mel_bordas[m + 2] > MFCC_N_BINS - 1) {
            return -2;   /* filtro degenerado ou fora do espectro */
        }
        total += mel_bordas[m + 2] - mel_bordas[m];
    }
    if (total != MFCC_N_PESOS_MEL) {
        return -2;   /* tabela de bordas e MFCC_N_PESOS_MEL fora de sincronia */
    }
    uint16_t pos = 0;
    for (int m = 0; m < MFCC_N_MEL; ++m) {
        const int l = mel_bordas[m], c = mel_bordas[m + 1], r = mel_bordas[m + 2];
        mel_inicio[m] = pos;
        for (int k = l; k < c; ++k) {
            if (pos >= MFCC_N_PESOS_MEL) {
                return -2;
            }
            mel_pesos[pos++] = (float)(k - l) / (float)(c - l);
        }
        for (int k = c; k < r; ++k) {
            if (pos >= MFCC_N_PESOS_MEL) {
                return -2;
            }
            mel_pesos[pos++] = (float)(r - k) / (float)(r - c);
        }
    }
    if (pos != MFCC_N_PESOS_MEL) {
        return -2;
    }

    /* DCT-II ortonormal (scipy.fft.dct(type=2, norm="ortho")), so as 13 primeiras linhas */
    const float s0 = sqrtf(1.0f / (float)MFCC_N_MEL), s1 = sqrtf(2.0f / (float)MFCC_N_MEL);
    for (int k = 0; k < MFCC_N_COEFS; ++k) {
        for (int n = 0; n < MFCC_N_MEL; ++n) {
            dct_matriz[k][n] = (k == 0 ? s0 : s1) * cos_pi_m_2n(k * (2 * n + 1));
        }
    }
    if (arm_rfft_fast_init_f32(&rfft, MFCC_N_FFT) != ARM_MATH_SUCCESS) {
        return -1;
    }
    pronto = 1;
    return 0;
}

const float *mfcc_janela_hann(void)
{
    return janela_hann;
}

void mfcc_escala(const int16_t *pcm, float *x, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        x[i] = (float)pcm[i] / 32767.0f;
    }
}

void mfcc_janela_quadro(const float x[MFCC_N_QUADRO], float quadro[MFCC_N_QUADRO])
{
    arm_mult_f32(x, janela_hann, quadro, MFCC_N_QUADRO);
}

void mfcc_quadro_pcm(const int16_t pcm[MFCC_N_QUADRO], float quadro[MFCC_N_QUADRO])
{
    /* as mesmas duas operacoes de mfcc_escala e mfcc_janela_quadro, sem buffer extra */
    mfcc_escala(pcm, quadro, MFCC_N_QUADRO);
    arm_mult_f32(quadro, janela_hann, quadro, MFCC_N_QUADRO);
}

void mfcc_espectro(const float quadro[MFCC_N_QUADRO], float potencia[MFCC_N_BINS])
{
    /* O arm_rfft_fast_f32 sobrescreve a entrada: trabalha numa copia com os zeros. */
    static float entrada[MFCC_N_FFT];
    static float saida[MFCC_N_FFT];
    memcpy(entrada, quadro, sizeof(float) * MFCC_N_QUADRO);
    memset(&entrada[MFCC_N_QUADRO], 0, sizeof(float) * (MFCC_N_FFT - MFCC_N_QUADRO));
    arm_rfft_fast_f32(&rfft, entrada, saida, 0);
    /* Saida empacotada: saida[0] = Re(bin 0), saida[1] = Re(bin 256); os bins 1 a 255
     * vem em pares (re, im) a partir de saida[2]. */
    potencia[0] = saida[0] * saida[0];
    potencia[MFCC_N_BINS - 1] = saida[1] * saida[1];
    arm_cmplx_mag_squared_f32(&saida[2], &potencia[1], MFCC_N_BINS - 2);
}

void mfcc_mel(const float potencia[MFCC_N_BINS], float mel[MFCC_N_MEL])
{
    for (int m = 0; m < MFCC_N_MEL; ++m) {
        const uint16_t l = mel_bordas[m];
        const uint32_t n = (uint32_t)(mel_bordas[m + 2] - l);
        arm_dot_prod_f32(&potencia[l], &mel_pesos[mel_inicio[m]], n, &mel[m]);
    }
}

const uint16_t *mfcc_mel_bordas(void)
{
    return mel_bordas;
}

void mfcc_log(const float mel[MFCC_N_MEL], float logmel[MFCC_N_MEL])
{
    /* o epsilon e somado, nao e piso: ln(0 + 1e-10) = -23,0259 num quadro em zero */
    for (int m = 0; m < MFCC_N_MEL; ++m) {
        logmel[m] = logf(mel[m] + MFCC_LOG_EPS);
    }
}

void mfcc_dct(const float logmel[MFCC_N_MEL], float coefs[MFCC_N_COEFS])
{
    for (int k = 0; k < MFCC_N_COEFS; ++k) {
        arm_dot_prod_f32(dct_matriz[k], logmel, MFCC_N_MEL, &coefs[k]);
    }
}

const float *mfcc_dct_matriz(void)
{
    return &dct_matriz[0][0];
}

int mfcc_quadro_coefs(const int16_t pcm[MFCC_N_QUADRO], float coefs[MFCC_N_COEFS])
{
    if (!pronto) {
        /* sem o init, as tabelas estao zeradas: os coeficientes sairiam errados sem aviso */
        memset(coefs, 0, sizeof(float) * MFCC_N_COEFS);
        return -3;
    }
    /* Buffers de um quadro estaticos (2,4 KB), e nao na pilha: assim entram na conta
     * de RAM do linker. A pilha nao entra (o linker so reserva o _Min_Stack_Size, 1 KB),
     * e 2,4 KB nela passariam dessa reserva sem aviso. Como o mfcc_espectro, nao e
     * reentrante: chamar de um so contexto (nao de uma interrupcao e do laco ao mesmo tempo). */
    static float quadro[MFCC_N_QUADRO], potencia[MFCC_N_BINS], mel[MFCC_N_MEL];
    mfcc_quadro_pcm(pcm, quadro);
    mfcc_espectro(quadro, potencia);
    mfcc_mel(potencia, mel);
    mfcc_log(mel, mel);              /* em lugar: cada saida depende so da entrada de mesmo indice */
    mfcc_dct(mel, coefs);
    return 0;
}

int mfcc_segmento(const int16_t pcm[MFCC_N_AMOSTRAS], float caracteristicas[2 * MFCC_N_COEFS])
{
    if (!pronto) {
        /* sem o init, as tabelas estao zeradas e o escore sairia o bias da LDA, sem aviso */
        memset(caracteristicas, 0, sizeof(float) * 2 * MFCC_N_COEFS);
        return -3;
    }
    mfcc_resumo_t r;
    float coefs[MFCC_N_COEFS];
    mfcc_resumo_iniciar(&r);
    for (int q = 0; q < MFCC_N_QUADROS; ++q) {
        (void)mfcc_quadro_coefs(&pcm[q * MFCC_PASSO], coefs);   /* nao falha: o pronto ja foi conferido */
        mfcc_resumo_acumular(&r, coefs);
    }
    /* ordem do JSON do modelo: as 13 medias e depois os 13 desvios */
    mfcc_resumo_finalizar(&r, &caracteristicas[0], &caracteristicas[MFCC_N_COEFS]);
    return 0;
}

void mfcc_resumo_iniciar(mfcc_resumo_t *r)
{
    memset(r, 0, sizeof *r);
}

void mfcc_resumo_acumular(mfcc_resumo_t *r, const float coefs[MFCC_N_COEFS])
{
    r->n += 1u;
    const float n = (float)r->n;
    for (int c = 0; c < MFCC_N_COEFS; ++c) {
        const float d = coefs[c] - r->media[c];
        r->media[c] += d / n;
        r->m2[c] += d * (coefs[c] - r->media[c]);
    }
}

void mfcc_resumo_finalizar(const mfcc_resumo_t *r, float media[MFCC_N_COEFS],
                           float desvio[MFCC_N_COEFS])
{
    for (int c = 0; c < MFCC_N_COEFS; ++c) {
        media[c] = r->media[c];
        desvio[c] = r->n ? sqrtf(r->m2[c] / (float)r->n) : 0.0f;
    }
}
