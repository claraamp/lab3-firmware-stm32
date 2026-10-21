/*
 * decimar_gravacao.c -- uma gravacao inteira a 51,2 kHz pelo decimador do firmware
 *
 * Le um PCM cru (int16 little-endian, sem cabecalho, como os .bin de
 * data/processed/pcm_raw/ do diagnostico-acustico-motores) e o decima com o mesmo
 * decimacao_bloco() da placa (Core/Src/decimacao.c), em blocos de 512, com a
 * CMSIS-DSP generica do harness. Grava as saidas em float32 (little-endian, em LSB,
 * antes de requantizar), alinhadas com as do dsp.resample_clip:
 *
 *   - 75 zeros antes da gravacao, como o lfilter do Python (o recorte do
 *     clipes_demo.h faz o mesmo para o segmento 29, com amostras de verdade);
 *   - as 37 primeiras saidas descartadas (o transitorio do estado zerado);
 *   - ceil((n - 73) / 4) saidas, o tamanho do resample_clip.
 *
 * Cada saida depende so das 147 amostras da sua janela, entao decimar a gravacao
 * inteira de uma vez da o mesmo que decimar o recorte de um segmento.
 *
 *   ./build/harness/decimar_gravacao data/processed/pcm_raw/normal.bin build/normal_c.f32
 *
 * Quem compara com o Python e o tests/host/decimacao/tolerancias_gravacoes.py.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "decimacao.h"

#define MARGEM_ANTES 75   /* RECORTE_MARGEM_ANTES */
#define ATRASO 73         /* (147 - 1) / 2 */

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "uso: %s entrada_int16.bin saida.f32\n", argv[0]);
        return 2;
    }
    FILE *fe = fopen(argv[1], "rb");
    if (!fe) {
        perror(argv[1]);
        return 1;
    }
    fseek(fe, 0, SEEK_END);
    const long tam = ftell(fe);
    fseek(fe, 0, SEEK_SET);
    if (tam <= 0 || tam % 2 != 0) {
        fprintf(stderr, "%s: tamanho %ld nao e de int16\n", argv[1], tam);
        return 1;
    }
    const size_t n = (size_t)tam / 2u;
    if (n <= ATRASO) {
        fprintf(stderr, "%s: curto demais\n", argv[1]);
        return 1;
    }

    /* os 75 zeros, a gravacao e zeros no fim ate completar um multiplo de 4 que
     * cubra todas as saidas pedidas (as do fim so leem amostras de verdade) */
    const size_t n_saidas = (n - ATRASO + 3u) / 4u;
    size_t n_total = (size_t)RECORTE_DESCARTAR * 4u + n_saidas * 4u;
    if (n_total < MARGEM_ANTES + n) {
        n_total = (MARGEM_ANTES + n + 3u) / 4u * 4u;
    }
    int16_t *x = calloc(n_total, sizeof *x);
    float *y = malloc(n_total / 4u * sizeof *y);
    if (!x || !y) {
        fprintf(stderr, "sem memoria\n");
        return 1;
    }
    if (fread(&x[MARGEM_ANTES], sizeof *x, n, fe) != n) {
        fprintf(stderr, "%s: leitura incompleta\n", argv[1]);
        return 1;
    }
    fclose(fe);
    /* o arquivo e little-endian; o PC tambem (x86 e ARM): sem troca de bytes */

    decimacao_iniciar();
    for (size_t i = 0; i < n_total; i += RECORTE_BLOCO) {
        const size_t blk = (n_total - i) < RECORTE_BLOCO ? (n_total - i) : RECORTE_BLOCO;
        (void)decimacao_bloco(&x[i], (uint32_t)blk, &y[i / 4u], NULL, 0u);
    }

    FILE *fs = fopen(argv[2], "wb");
    if (!fs) {
        perror(argv[2]);
        return 1;
    }
    if (fwrite(&y[RECORTE_DESCARTAR], sizeof *y, n_saidas, fs) != n_saidas) {
        fprintf(stderr, "%s: escrita incompleta\n", argv[2]);
        return 1;
    }
    fclose(fs);
    fprintf(stderr, "%s: %zu amostras -> %zu saidas em %s\n", argv[1], n, n_saidas, argv[2]);
    free(x);
    free(y);
    return 0;
}
