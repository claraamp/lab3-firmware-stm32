/*
 * exportar_saidas_c.c -- grava a saida de cada estagio do C para a validacao independente
 *
 * Validacao bloco a bloco (card "Validar cada bloco da cadeia em C contra a
 * referencia em Python"). Este programa so RODA os estagios de
 * Core/Src/mfcc_estagios.c, com a CMSIS-DSP generica, e grava as saidas em float32;
 * nao compara nada. A comparacao fica no validar_blocos.py, com o
 * dsp.erro_estagio do proprio repositorio do Python, para nao depender da conta de
 * erro do harness (que e de quem portou).
 *
 * Le as referencias por estagio do 08_export_stage_references.py (qualquer
 * conjunto de segmentos: as 5 versionadas ou os 295 gerados com --segmentos):
 *
 *   REFS/segmentos.txt, REFS/<segmento>/{entrada.i16, escala.f64, ..., desvio.f64}
 *
 * e grava, para cada segmento, nos dois modos do A4:
 *
 *   SAIDA/<segmento>/isolado_<estagio>.f32     cada estagio recebe a referencia
 *                                              do anterior, arredondada para float32
 *   SAIDA/<segmento>/encadeado_<estagio>.f32   a cadeia em C desde o int16, com as
 *                                              mesmas chamadas que a placa usa
 *   SAIDA/<segmento>/segmento_x.f32            as 26 caracteristicas do
 *                                              mfcc_segmento(), a chamada unica
 *
 * Estagios: escala, janela, espectro, mel, logmel, mfcc, media, desvio, escore.
 *
 * Uso: exportar_saidas_c REFS SAIDA
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#define criar_pasta(p) _mkdir(p)
#else
#define criar_pasta(p) mkdir((p), 0755)
#endif

#include "lda_modelo.h"
#include "mfcc_estagios.h"

_Static_assert(LDA_N_FEATURES == 2 * MFCC_N_COEFS,
               "lda_modelo.h espera outro numero de caracteristicas que o mfcc_estagios.h");

#define MAX_CAMINHO 1024

/* Buffers de um segmento inteiro (estaticos: o espectro sozinho tem 100 KB). */
static int16_t entrada[MFCC_N_AMOSTRAS];
static double ref[MFCC_N_QUADROS * MFCC_N_QUADRO];       /* uma referencia por vez */
static float ref_f[MFCC_N_QUADROS * MFCC_N_QUADRO];      /* a mesma, em float32 */
static float escala_c[MFCC_N_AMOSTRAS];
static float janela_c[MFCC_N_QUADROS][MFCC_N_QUADRO];
static float espectro_c[MFCC_N_QUADROS][MFCC_N_BINS];
static float mel_c[MFCC_N_QUADROS][MFCC_N_MEL];
static float logmel_c[MFCC_N_QUADROS][MFCC_N_MEL];
static float mfcc_c[MFCC_N_QUADROS][MFCC_N_COEFS];

static void juntar(char *out, const char *a, const char *b)
{
    const int n = snprintf(out, MAX_CAMINHO, "%s/%s", a, b);
    if (n < 0 || n >= MAX_CAMINHO) {
        out[0] = '\0';   /* caminho longo demais: o fopen seguinte falha e avisa */
    }
}

static int ler(const char *caminho, void *dest, size_t tam_elem, size_t n)
{
    FILE *f = fopen(caminho, "rb");
    if (!f) {
        fprintf(stderr, "erro: nao abri %s\n", caminho);
        return -1;
    }
    const size_t lidos = fread(dest, tam_elem, n, f);
    const int sobra = fgetc(f);
    fclose(f);
    if (lidos != n || sobra != EOF) {
        fprintf(stderr, "erro: %s nao tem %zu valores de %zu bytes\n", caminho, n, tam_elem);
        return -1;
    }
    return 0;
}

/* Le <pasta>/<estagio>.f64 com n valores e converte para float32 em ref_f. */
static int ler_ref(const char *pasta, const char *estagio, size_t n)
{
    char nome[64], caminho[MAX_CAMINHO];
    snprintf(nome, sizeof nome, "%s.f64", estagio);
    juntar(caminho, pasta, nome);
    if (ler(caminho, ref, sizeof(double), n) != 0) {
        return -1;
    }
    for (size_t k = 0; k < n; ++k) {
        ref_f[k] = (float)ref[k];
    }
    return 0;
}

static int gravar(const char *pasta, const char *modo, const char *estagio, const float *v, size_t n)
{
    char nome[64], caminho[MAX_CAMINHO];
    snprintf(nome, sizeof nome, "%s_%s.f32", modo, estagio);
    juntar(caminho, pasta, nome);
    FILE *f = fopen(caminho, "wb");
    if (!f || fwrite(v, sizeof(float), n, f) != n) {
        fprintf(stderr, "erro: nao gravei %s\n", caminho);
        if (f) {
            fclose(f);
        }
        return -1;
    }
    return fclose(f) == 0 ? 0 : -1;
}

/* Isolado: cada estagio recebe a referencia do anterior, em float32. */
static int isolado(const char *refs, const char *saida)
{
    int erro = 0;

    mfcc_escala(entrada, escala_c, MFCC_N_AMOSTRAS);
    erro |= gravar(saida, "isolado", "escala", escala_c, MFCC_N_AMOSTRAS);

    erro |= ler_ref(refs, "escala", MFCC_N_AMOSTRAS);
    for (int q = 0; q < MFCC_N_QUADROS; ++q) {
        mfcc_janela_quadro(&ref_f[q * MFCC_PASSO], janela_c[q]);
    }
    erro |= gravar(saida, "isolado", "janela", &janela_c[0][0], sizeof janela_c / sizeof(float));

    erro |= ler_ref(refs, "janela", MFCC_N_QUADROS * MFCC_N_QUADRO);
    for (int q = 0; q < MFCC_N_QUADROS; ++q) {
        mfcc_espectro(&ref_f[q * MFCC_N_QUADRO], espectro_c[q]);
    }
    erro |= gravar(saida, "isolado", "espectro", &espectro_c[0][0], sizeof espectro_c / sizeof(float));

    erro |= ler_ref(refs, "espectro", MFCC_N_QUADROS * MFCC_N_BINS);
    for (int q = 0; q < MFCC_N_QUADROS; ++q) {
        mfcc_mel(&ref_f[q * MFCC_N_BINS], mel_c[q]);
    }
    erro |= gravar(saida, "isolado", "mel", &mel_c[0][0], sizeof mel_c / sizeof(float));

    erro |= ler_ref(refs, "mel", MFCC_N_QUADROS * MFCC_N_MEL);
    for (int q = 0; q < MFCC_N_QUADROS; ++q) {
        mfcc_log(&ref_f[q * MFCC_N_MEL], logmel_c[q]);
    }
    erro |= gravar(saida, "isolado", "logmel", &logmel_c[0][0], sizeof logmel_c / sizeof(float));

    erro |= ler_ref(refs, "logmel", MFCC_N_QUADROS * MFCC_N_MEL);
    for (int q = 0; q < MFCC_N_QUADROS; ++q) {
        mfcc_dct(&ref_f[q * MFCC_N_MEL], mfcc_c[q]);
    }
    erro |= gravar(saida, "isolado", "mfcc", &mfcc_c[0][0], sizeof mfcc_c / sizeof(float));

    /* media e desvio: Welford sobre o MFCC de referencia */
    erro |= ler_ref(refs, "mfcc", MFCC_N_QUADROS * MFCC_N_COEFS);
    mfcc_resumo_t r;
    float x[2 * MFCC_N_COEFS];
    mfcc_resumo_iniciar(&r);
    for (int q = 0; q < MFCC_N_QUADROS; ++q) {
        mfcc_resumo_acumular(&r, &ref_f[q * MFCC_N_COEFS]);
    }
    mfcc_resumo_finalizar(&r, &x[0], &x[MFCC_N_COEFS]);
    erro |= gravar(saida, "isolado", "media", &x[0], MFCC_N_COEFS);
    erro |= gravar(saida, "isolado", "desvio", &x[MFCC_N_COEFS], MFCC_N_COEFS);

    /* escore: as 26 caracteristicas de referencia */
    erro |= ler_ref(refs, "media", MFCC_N_COEFS);
    memcpy(&x[0], ref_f, sizeof(float) * MFCC_N_COEFS);
    erro |= ler_ref(refs, "desvio", MFCC_N_COEFS);
    memcpy(&x[MFCC_N_COEFS], ref_f, sizeof(float) * MFCC_N_COEFS);
    const float escore = lda_escore(x);
    erro |= gravar(saida, "isolado", "escore", &escore, 1);
    return erro ? -1 : 0;
}

/* Encadeado: desde o int16, quadro a quadro, com as chamadas da placa. */
static int encadeado(const char *saida)
{
    int erro = 0;
    mfcc_escala(entrada, escala_c, MFCC_N_AMOSTRAS);
    mfcc_resumo_t r;
    mfcc_resumo_iniciar(&r);
    for (int q = 0; q < MFCC_N_QUADROS; ++q) {
        mfcc_quadro_pcm(&entrada[q * MFCC_PASSO], janela_c[q]);
        mfcc_espectro(janela_c[q], espectro_c[q]);
        mfcc_mel(espectro_c[q], mel_c[q]);
        mfcc_log(mel_c[q], logmel_c[q]);
        mfcc_dct(logmel_c[q], mfcc_c[q]);
        mfcc_resumo_acumular(&r, mfcc_c[q]);
    }
    float x[2 * MFCC_N_COEFS];
    mfcc_resumo_finalizar(&r, &x[0], &x[MFCC_N_COEFS]);
    const float escore = lda_escore(x);

    erro |= gravar(saida, "encadeado", "escala", escala_c, MFCC_N_AMOSTRAS);
    erro |= gravar(saida, "encadeado", "janela", &janela_c[0][0], sizeof janela_c / sizeof(float));
    erro |= gravar(saida, "encadeado", "espectro", &espectro_c[0][0], sizeof espectro_c / sizeof(float));
    erro |= gravar(saida, "encadeado", "mel", &mel_c[0][0], sizeof mel_c / sizeof(float));
    erro |= gravar(saida, "encadeado", "logmel", &logmel_c[0][0], sizeof logmel_c / sizeof(float));
    erro |= gravar(saida, "encadeado", "mfcc", &mfcc_c[0][0], sizeof mfcc_c / sizeof(float));
    erro |= gravar(saida, "encadeado", "media", &x[0], MFCC_N_COEFS);
    erro |= gravar(saida, "encadeado", "desvio", &x[MFCC_N_COEFS], MFCC_N_COEFS);
    erro |= gravar(saida, "encadeado", "escore", &escore, 1);

    /* a chamada unica da placa, para o validar_blocos.py conferir que e a mesma conta */
    float x_seg[2 * MFCC_N_COEFS];
    if (mfcc_segmento(entrada, x_seg) != 0) {
        fprintf(stderr, "erro: mfcc_segmento recusou rodar\n");
        return -1;
    }
    erro |= gravar(saida, "segmento", "x", x_seg, 2 * MFCC_N_COEFS);
    return erro ? -1 : 0;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "uso: %s REFS SAIDA\n", argv[0]);
        return 2;
    }
    const char *refs = argv[1], *saida = argv[2];
    const int st = mfcc_estagios_init();
    if (st != 0) {
        fprintf(stderr, "erro: mfcc_estagios_init devolveu %d (-1 = FFT, -2 = tabela do Mel)\n", st);
        return 2;
    }
    if (criar_pasta(saida) != 0 && errno != EEXIST) {
        fprintf(stderr, "erro: nao criei %s\n", saida);
        return 2;
    }

    char caminho[MAX_CAMINHO], pasta[256];
    juntar(caminho, refs, "segmentos.txt");
    FILE *lista = fopen(caminho, "r");
    if (!lista) {
        fprintf(stderr, "erro: nao abri %s\n", caminho);
        return 2;
    }
    int n = 0;
    while (fscanf(lista, "%255s", pasta) == 1) {
        char dir_ref[MAX_CAMINHO], dir_saida[MAX_CAMINHO];
        juntar(dir_ref, refs, pasta);
        juntar(dir_saida, saida, pasta);
        if (criar_pasta(dir_saida) != 0 && errno != EEXIST) {
            fprintf(stderr, "erro: nao criei %s\n", dir_saida);
            fclose(lista);
            return 2;
        }
        juntar(caminho, dir_ref, "entrada.i16");
        if (ler(caminho, entrada, sizeof(int16_t), MFCC_N_AMOSTRAS) != 0 ||
            isolado(dir_ref, dir_saida) != 0 || encadeado(dir_saida) != 0) {
            fclose(lista);
            return 2;
        }
        ++n;
    }
    fclose(lista);
    if (n == 0) {
        fprintf(stderr, "erro: segmentos.txt vazio\n");
        return 2;
    }
    printf("exportar_saidas_c: %d segmentos em %s\n", n, saida);
    return 0;
}
