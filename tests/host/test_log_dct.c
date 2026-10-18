/*
 * test_log_dct.c -- confere, no PC, o log, a matriz da DCT e o resumo por Welford
 *
 * O harness compara os estagios com as referencias de 5 segmentos; este teste
 * cobre o que essas referencias nao exercitam:
 *   - a matriz da DCT calculada no init, contra a formula do contrato em float64
 *     (DCT-II ortonormal, docs/contrato_numerico.md), e as linhas ortonormais;
 *   - o epsilon do log SOMADO, nao como piso: com mel = 1e-10, a soma da
 *     ln(2e-10) e o piso daria ln(1e-10). Nenhum quadro das referencias tem Mel
 *     tao pequeno, entao o harness nao distingue os dois;
 *   - o desvio populacional (ddof = 0) num caso conhecido, e o resumo vazio;
 *   - o mfcc_segmento() e o mfcc_quadro_coefs() recusam rodar (-3, e zeros) antes
 *     do mfcc_estagios_init().
 *
 * Compilar e rodar (na raiz do repositorio; a CMSIS-DSP generica vem do harness):
 *   cmake -S tests/host/harness -B build/harness -G Ninja && cmake --build build/harness
 *   ./build/harness/test_log_dct
 */

#include <math.h>
#include <stdio.h>

#include "mfcc_estagios.h"

static int falhas = 0;

static void conferir(int ok, const char *o_que, double obtido, double esperado)
{
    if (!ok) {
        printf("FALHOU: %s: %.9g (esperado %.9g)\n", o_que, obtido, esperado);
        ++falhas;
    }
}

int main(void)
{
    /* antes do init: as tabelas estao zeradas, e o mfcc_segmento() tem que recusar */
    static int16_t silencio[MFCC_N_AMOSTRAS];
    float x[2 * MFCC_N_COEFS];
    for (int i = 0; i < 2 * MFCC_N_COEFS; ++i) {
        x[i] = 1.0f;
    }
    const int st_antes = mfcc_segmento(silencio, x);
    conferir(st_antes == -3, "mfcc_segmento antes do init", (double)st_antes, -3.0);
    conferir(x[0] == 0.0f && x[2 * MFCC_N_COEFS - 1] == 0.0f, "saida zerada antes do init", (double)x[0], 0.0);
    float c_antes[MFCC_N_COEFS];
    for (int i = 0; i < MFCC_N_COEFS; ++i) {
        c_antes[i] = 1.0f;
    }
    const int stq_antes = mfcc_quadro_coefs(silencio, c_antes);
    conferir(stq_antes == -3, "mfcc_quadro_coefs antes do init", (double)stq_antes, -3.0);
    conferir(c_antes[0] == 0.0f && c_antes[MFCC_N_COEFS - 1] == 0.0f, "coeficientes zerados antes do init",
             (double)c_antes[0], 0.0);

    const int st = mfcc_estagios_init();
    if (st != 0) {
        printf("FALHOU: mfcc_estagios_init devolveu %d (-1 = FFT, -2 = tabela do Mel)\n", st);
        return 1;
    }
    const double pi = 3.14159265358979323846;

    /* DCT: cada elemento a menos de 1e-7 da formula (o float32 arredondado erra ate
     * 1,3e-8; sem a reducao do angulo no init, o erro chegaria a 6e-7) */
    const float *c = mfcc_dct_matriz();
    double erro_max = 0.0;
    for (int k = 0; k < MFCC_N_COEFS; ++k) {
        const double s = k == 0 ? sqrt(1.0 / MFCC_N_MEL) : sqrt(2.0 / MFCC_N_MEL);
        for (int n = 0; n < MFCC_N_MEL; ++n) {
            const double ref = s * cos(pi * k * (2 * n + 1) / (2.0 * MFCC_N_MEL));
            const double e = fabs((double)c[k * MFCC_N_MEL + n] - ref);
            erro_max = e > erro_max ? e : erro_max;
        }
    }
    conferir(erro_max < 1e-7, "erro maximo da matriz da DCT", erro_max, 0.0);

    /* linhas ortonormais: C C^T = I (13 x 13) */
    double orto_max = 0.0;
    for (int i = 0; i < MFCC_N_COEFS; ++i) {
        for (int j = 0; j < MFCC_N_COEFS; ++j) {
            double p = 0.0;
            for (int n = 0; n < MFCC_N_MEL; ++n) {
                p += (double)c[i * MFCC_N_MEL + n] * (double)c[j * MFCC_N_MEL + n];
            }
            const double e = fabs(p - (i == j ? 1.0 : 0.0));
            orto_max = e > orto_max ? e : orto_max;
        }
    }
    conferir(orto_max < 1e-6, "desvio da ortonormalidade das linhas", orto_max, 0.0);

    /* DCT de um vetor constante: so o c0 = sqrt(20) * valor */
    float um[MFCC_N_MEL], coefs[MFCC_N_COEFS];
    for (int n = 0; n < MFCC_N_MEL; ++n) {
        um[n] = 1.0f;
    }
    mfcc_dct(um, coefs);
    conferir(fabs((double)coefs[0] - sqrt((double)MFCC_N_MEL)) < 1e-6, "c0 de um vetor de uns",
             (double)coefs[0], sqrt((double)MFCC_N_MEL));
    for (int k = 1; k < MFCC_N_COEFS; ++k) {
        conferir(fabs((double)coefs[k]) < 1e-6, "c[k>0] de um vetor de uns", (double)coefs[k], 0.0);
    }

    /* log: o epsilon e somado (contrato, linha 10) */
    float mel[MFCC_N_MEL] = {0}, logmel[MFCC_N_MEL];
    mel[1] = 1e-10f;
    mel[2] = 1.0f;
    mfcc_log(mel, logmel);
    conferir(fabs((double)logmel[0] - log(1e-10)) < 1e-5, "ln(0 + 1e-10)", (double)logmel[0], log(1e-10));
    conferir(fabs((double)logmel[1] - log(2e-10)) < 1e-5, "ln(1e-10 + 1e-10), soma e nao piso",
             (double)logmel[1], log(2e-10));
    conferir(fabs((double)logmel[2]) < 1e-6, "ln(1 + 1e-10)", (double)logmel[2], 0.0);
    mfcc_log(mel, mel);   /* em lugar */
    conferir(mel[1] == logmel[1], "mfcc_log em lugar", (double)mel[1], (double)logmel[1]);

    /* Welford: quadros 1, 2, ..., 98 no coeficiente 0; desvio populacional (ddof = 0)
     * = sqrt((98^2 - 1) / 12) = 28,2887...; com ddof = 1 seria 28,4341 */
    mfcc_resumo_t r;
    float media[MFCC_N_COEFS], desvio[MFCC_N_COEFS];
    mfcc_resumo_iniciar(&r);
    mfcc_resumo_finalizar(&r, media, desvio);
    conferir(media[0] == 0.0f && desvio[0] == 0.0f, "resumo vazio", (double)desvio[0], 0.0);
    for (int q = 1; q <= MFCC_N_QUADROS; ++q) {
        float x[MFCC_N_COEFS] = {0};
        x[0] = (float)q;
        mfcc_resumo_acumular(&r, x);
    }
    mfcc_resumo_finalizar(&r, media, desvio);
    const double n = MFCC_N_QUADROS, dp = sqrt((n * n - 1.0) / 12.0);
    conferir(fabs((double)media[0] - (n + 1.0) / 2.0) < 1e-5, "media de 1..98", (double)media[0],
             (n + 1.0) / 2.0);
    conferir(fabs((double)desvio[0] - dp) / dp < 1e-6, "desvio populacional de 1..98", (double)desvio[0],
             dp);

    /* depois do init: silencio da so o log do epsilon, constante nos 98 quadros */
    const int st_depois = mfcc_segmento(silencio, x);
    conferir(st_depois == 0, "mfcc_segmento depois do init", (double)st_depois, 0.0);
    conferir(fabs((double)x[0] - sqrt((double)MFCC_N_MEL) * log(1e-10)) < 1e-4, "media do c0 no silencio",
             (double)x[0], sqrt((double)MFCC_N_MEL) * log(1e-10));
    conferir(x[MFCC_N_COEFS] == 0.0f, "desvio do c0 no silencio", (double)x[MFCC_N_COEFS], 0.0);
    const int stq_depois = mfcc_quadro_coefs(silencio, c_antes);
    conferir(stq_depois == 0 && c_antes[0] == x[0], "mfcc_quadro_coefs depois do init (silencio = media)",
             (double)c_antes[0], (double)x[0]);
    if (falhas) {
        printf("LOG DCT FALHOU (%d)\n", falhas);
        return 1;
    }

    printf("LOG DCT OK (DCT a %.1e da formula, linhas ortonormais a %.1e; epsilon somado; ddof = 0;"
           " mfcc_segmento e mfcc_quadro_coefs exigem o init)\n", erro_max, orto_max);
    return 0;
}
