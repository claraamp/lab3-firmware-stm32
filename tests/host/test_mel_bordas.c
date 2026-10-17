/*
 * test_mel_bordas.c -- confere, no PC, a tabela de bordas do banco de Mel
 *
 * O mfcc_estagios.c guarda as 22 bordas como constantes, copiadas do
 * docs/contrato_numerico.md. Este teste as recalcula pela formula do dsp.py
 * (mel_filterbank: HTK, 22 pontos igualmente espacados em Mel de 20 Hz a fs/2,
 * bin = floor((n_fft + 1) * f / fs)) em float64 e confere uma a uma. Tambem
 * confere que nenhum filtro e degenerado (c == l ou r == c), o caso em que o
 * dsp.py corrige as bordas e a tabela deixaria de valer.
 *
 * Compilar e rodar (na raiz do repositorio; a CMSIS-DSP generica vem do harness):
 *   cmake -S tests/host/harness -B build/harness -G Ninja && cmake --build build/harness
 *   ./build/harness/test_mel_bordas
 */

#include <math.h>
#include <stdio.h>

#include "mfcc_estagios.h"

static double hz_para_mel(double f) { return 2595.0 * log10(1.0 + f / 700.0); }
static double mel_para_hz(double m) { return 700.0 * (pow(10.0, m / 2595.0) - 1.0); }

int main(void)
{
    const double fs = 12800.0, fmin = 20.0, fmax = fs / 2.0;
    const int n_pontos = MFCC_N_MEL + 2;
    const int st = mfcc_estagios_init();
    if (st != 0) {
        printf("FALHOU: mfcc_estagios_init devolveu %d (-1 = FFT, -2 = tabela do Mel)\n", st);
        return 1;
    }
    const uint16_t *b = mfcc_mel_bordas();
    int falhas = 0;
    const double m0 = hz_para_mel(fmin), m1 = hz_para_mel(fmax);
    for (int i = 0; i < n_pontos; ++i) {
        /* np.linspace: start + i * passo, e o ultimo ponto exatamente igual a stop */
        const double m = (i == n_pontos - 1) ? m1 : m0 + i * (m1 - m0) / (n_pontos - 1);
        int bin = (int)floor((MFCC_N_FFT + 1) * mel_para_hz(m) / fs);
        bin = bin < 0 ? 0 : (bin > MFCC_N_FFT / 2 ? MFCC_N_FFT / 2 : bin);
        if (bin != b[i]) {
            printf("FALHOU: borda %d: formula %d, tabela %u\n", i, bin, b[i]);
            ++falhas;
        }
    }
    int soma = 0;
    for (int m = 0; m < MFCC_N_MEL; ++m) {
        if (b[m + 1] == b[m] || b[m + 2] == b[m + 1]) {
            printf("FALHOU: filtro %d degenerado (%u, %u, %u)\n", m, b[m], b[m + 1], b[m + 2]);
            ++falhas;
        }
        soma += b[m + 2] - b[m];
    }
    if (soma != MFCC_N_PESOS_MEL) {
        printf("FALHOU: soma de (r - l) = %d, MFCC_N_PESOS_MEL = %d\n", soma, MFCC_N_PESOS_MEL);
        ++falhas;
    }
    printf(falhas ? "MEL BORDAS FALHOU\n" : "MEL BORDAS OK (22 bordas, %d pesos)\n", soma);
    return falhas ? 1 : 0;
}
