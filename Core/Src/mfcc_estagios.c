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

int mfcc_estagios_init(void)
{
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
    return arm_rfft_fast_init_f32(&rfft, MFCC_N_FFT) == ARM_MATH_SUCCESS ? 0 : -1;
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

void mfcc_janela(const float x[MFCC_N_AMOSTRAS], float quadros[MFCC_N_QUADROS][MFCC_N_QUADRO])
{
    /* Os quadros ficam dentro do segmento: as 64 ultimas amostras nao entram. */
    for (int q = 0; q < MFCC_N_QUADROS; ++q) {
        arm_mult_f32(&x[q * MFCC_PASSO], janela_hann, quadros[q], MFCC_N_QUADRO);
    }
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

void mfcc_resumo(const float mfcc[][MFCC_N_COEFS], size_t n_quadros,
                 float media[MFCC_N_COEFS], float desvio[MFCC_N_COEFS])
{
    float m2[MFCC_N_COEFS];
    for (int c = 0; c < MFCC_N_COEFS; ++c) {
        media[c] = 0.0f;
        m2[c] = 0.0f;
    }
    for (size_t i = 0; i < n_quadros; ++i) {
        const float n = (float)(i + 1u);
        for (int c = 0; c < MFCC_N_COEFS; ++c) {
            const float d = mfcc[i][c] - media[c];
            media[c] += d / n;
            m2[c] += d * (mfcc[i][c] - media[c]);
        }
    }
    for (int c = 0; c < MFCC_N_COEFS; ++c) {
        desvio[c] = sqrtf(m2[c] / (float)n_quadros);
    }
}
