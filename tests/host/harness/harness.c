/*
 * harness.c -- confere, no PC, o porte da extracao de caracteristicas estagio a estagio
 *
 * Le as referencias por estagio exportadas pelo repositorio do Python
 * (diagnostico-acustico-motores, scripts/pipeline/08_export_stage_references.py,
 * em reports/c_reference/estagios/), roda os estagios ja portados em
 * Core/Src/mfcc_estagios.c com a implementacao generica em C da CMSIS-DSP e
 * compara cada saida com a referencia, pela metrica e pela tolerancia do A4
 * (docs/contrato_numerico.md, "Tolerancias por estagio (A4)").
 *
 * Dois modos:
 *   isolado    cada estagio recebe a referencia do estagio anterior (em float32):
 *              mede o erro do proprio estagio, mesmo com a cadeia incompleta;
 *   encadeado  a cadeia em C desde o int16, como na placa, ate o primeiro estagio
 *              que ainda nao foi portado.
 *
 * Uso:  harness [--refs PASTA] [--modo isolado|encadeado|ambos] [-v]
 * Saida: 0 se todos os estagios portados ficam dentro da tolerancia; 1 se algum
 * passa dela; 2 se as referencias nao puderem ser lidas.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mfcc_estagios.h"

#if defined(__has_include)
#if __has_include("lda_modelo.h")
#include "lda_modelo.h"
#define TEM_LDA 1
#endif
#endif
#ifndef TEM_LDA
#define TEM_LDA 0
#endif

#define REFS_PADRAO "../diagnostico-acustico-motores/reports/c_reference/estagios"
#define MAX_CAMINHO 1024

/* ------------------------------------------------------------------------- */
/* Estagios, na ordem da cadeia                                               */
/* ------------------------------------------------------------------------- */
enum { ESCALA, JANELA, ESPECTRO, MEL, LOGMEL, MFCC, MEDIA, DESVIO, ESCORE, N_ESTAGIOS };
static const char *const NOME[N_ESTAGIOS] = {
    "escala", "janela", "espectro", "mel", "logmel", "mfcc", "media", "desvio", "escore"};
/* 1 = ja portado em mfcc_estagios.c (ou, no escore, lda_modelo.h presente) */
static const int PORTADO[N_ESTAGIOS] = {1, 1, 1, 1, 0, 0, 1, 1, TEM_LDA};
/* linhas x colunas de cada referencia: os buffers do harness tem esse tamanho fixo,
 * entao um indice.txt com outra forma e recusado antes de qualquer leitura */
static const int FORMA[N_ESTAGIOS][2] = {
    {1, MFCC_N_AMOSTRAS},              /* escala */
    {MFCC_N_QUADROS, MFCC_N_QUADRO},   /* janela */
    {MFCC_N_QUADROS, MFCC_N_BINS},     /* espectro */
    {MFCC_N_QUADROS, MFCC_N_MEL},      /* mel */
    {MFCC_N_QUADROS, MFCC_N_MEL},      /* logmel */
    {MFCC_N_QUADROS, MFCC_N_COEFS},    /* mfcc */
    {1, MFCC_N_COEFS},                 /* media */
    {1, MFCC_N_COEFS},                 /* desvio */
    {1, 1},                            /* escore */
};

enum { ISOLADO, ENCADEADO };
static const char *const NOME_MODO[2] = {"isolado", "encadeado"};

typedef struct {
    char metrica[16];
    double tol[2];
    int lida;
} tolerancia_t;

typedef struct {
    int linhas, colunas;
    double *d;          /* referencia em float64 */
} matriz_t;

typedef struct {
    char pasta[256];
    int16_t entrada[MFCC_N_AMOSTRAS];
    matriz_t m[N_ESTAGIOS];
} segmento_t;

typedef struct {
    double erro_abs, erro_rel, metrica;
    char pior[256];
    int avaliado;
} resultado_t;

static tolerancia_t tolerancia[N_ESTAGIOS];
static int referencia_invalida = 0;   /* ha referencia em que a metrica nao se aplica */
static resultado_t resultado[2][N_ESTAGIOS];
static int verboso = 0;

static int indice_estagio(const char *nome)
{
    for (int e = 0; e < N_ESTAGIOS; ++e) {
        if (strcmp(nome, NOME[e]) == 0) {
            return e;
        }
    }
    return -1;
}

/* ------------------------------------------------------------------------- */
/* Leitura das referencias                                                    */
/* ------------------------------------------------------------------------- */
static void juntar(char *out, const char *a, const char *b)
{
    const int n = snprintf(out, MAX_CAMINHO, "%s/%s", a, b);
    if (n < 0 || n >= MAX_CAMINHO) {
        out[0] = '\0';   /* caminho longo demais: o fopen seguinte falha e avisa */
    }
}

static int ler_binario(const char *caminho, void *dest, size_t tam_elem, size_t n)
{
    FILE *f = fopen(caminho, "rb");
    if (!f) {
        fprintf(stderr, "erro: nao abri %s\n", caminho);
        return -1;
    }
    size_t lidos = fread(dest, tam_elem, n, f);
    int sobra = fgetc(f);
    fclose(f);
    if (lidos != n || sobra != EOF) {
        fprintf(stderr, "erro: %s nao tem %zu valores de %zu bytes\n", caminho, n, tam_elem);
        return -1;
    }
    return 0;
}

static int ler_tolerancias(const char *refs)
{
    char caminho[MAX_CAMINHO], linha[256];
    juntar(caminho, refs, "tolerancias.txt");
    FILE *f = fopen(caminho, "r");
    if (!f) {
        fprintf(stderr, "erro: nao abri %s (rode o 08 no repositorio do Python?)\n", caminho);
        return -1;
    }
    while (fgets(linha, sizeof linha, f)) {
        char nome[32], metrica[16];
        double iso, enc;
        if (linha[0] == '#' || sscanf(linha, "%31s %15s %lf %lf", nome, metrica, &iso, &enc) != 4) {
            continue;
        }
        int e = indice_estagio(nome);
        if (e < 0) {
            fprintf(stderr, "aviso: estagio desconhecido em tolerancias.txt: %s\n", nome);
            continue;
        }
        if (strcmp(metrica, "rel_quadro") != 0 && strcmp(metrica, "abs") != 0
                && strcmp(metrica, "rel") != 0) {
            fprintf(stderr, "erro: tolerancias.txt: metrica desconhecida \"%s\" no estagio %s "
                    "(use rel_quadro, abs ou rel)\n", metrica, nome);
            fclose(f);
            return -1;
        }
        if (!(iso > 0.0) || !(enc > 0.0)) {
            fprintf(stderr, "erro: tolerancias.txt: tolerancia nao positiva no estagio %s\n", nome);
            fclose(f);
            return -1;
        }
        snprintf(tolerancia[e].metrica, sizeof tolerancia[e].metrica, "%s", metrica);
        tolerancia[e].tol[ISOLADO] = iso;
        tolerancia[e].tol[ENCADEADO] = enc;
        tolerancia[e].lida = 1;
    }
    fclose(f);
    for (int e = 0; e < N_ESTAGIOS; ++e) {
        if (!tolerancia[e].lida) {
            fprintf(stderr, "erro: tolerancias.txt sem o estagio %s\n", NOME[e]);
            return -1;
        }
    }
    return 0;
}

static void liberar_segmento(segmento_t *s)
{
    for (int e = 0; e < N_ESTAGIOS; ++e) {
        free(s->m[e].d);
        s->m[e].d = NULL;
    }
}

static int ler_segmento(const char *refs, const char *pasta, segmento_t *s)
{
    char dir[MAX_CAMINHO], caminho[MAX_CAMINHO], linha[256];
    memset(s, 0, sizeof *s);
    snprintf(s->pasta, sizeof s->pasta, "%s", pasta);
    juntar(dir, refs, pasta);
    juntar(caminho, dir, "indice.txt");
    FILE *f = fopen(caminho, "r");
    if (!f) {
        fprintf(stderr, "erro: nao abri %s\n", caminho);
        return -1;
    }
    int tem_entrada = 0, erro = 0;
    while (!erro && fgets(linha, sizeof linha, f)) {
        char nome[32], tipo[8], arquivo[64];
        int n, m;
        if (linha[0] == '#' || sscanf(linha, "%31s %7s %d %d %63s", nome, tipo, &n, &m, arquivo) != 5) {
            continue;
        }
        juntar(caminho, dir, arquivo);
        if (strcmp(nome, "entrada") == 0) {
            if (strcmp(tipo, "i16") != 0 || n * m != MFCC_N_AMOSTRAS) {
                fprintf(stderr, "erro: %s: entrada tem que ser i16 com %d amostras\n", pasta, MFCC_N_AMOSTRAS);
                erro = 1;
            } else {
                erro = ler_binario(caminho, s->entrada, sizeof(int16_t), MFCC_N_AMOSTRAS) != 0;
                tem_entrada = 1;
            }
            continue;
        }
        int e = indice_estagio(nome);
        if (e < 0) {
            continue;
        }
        if (strcmp(tipo, "f64") != 0 || n != FORMA[e][0] || m != FORMA[e][1]) {
            fprintf(stderr, "erro: %s: %s e %s %dx%d; o harness espera f64 %dx%d\n", pasta, NOME[e],
                    tipo, n, m, FORMA[e][0], FORMA[e][1]);
            erro = 1;
            continue;
        }
        if (s->m[e].d) {
            fprintf(stderr, "erro: %s: estagio %s repetido no indice.txt\n", pasta, NOME[e]);
            erro = 1;
            continue;
        }
        s->m[e].linhas = n;
        s->m[e].colunas = m;
        s->m[e].d = malloc(sizeof(double) * (size_t)n * (size_t)m);
        erro = !s->m[e].d || ler_binario(caminho, s->m[e].d, sizeof(double), (size_t)n * (size_t)m) != 0;
    }
    fclose(f);
    for (int e = 0; !erro && e < N_ESTAGIOS; ++e) {
        if (!s->m[e].d) {
            fprintf(stderr, "erro: %s sem a referencia do estagio %s\n", pasta, NOME[e]);
            erro = 1;
        }
    }
    if (!erro && !tem_entrada) {
        fprintf(stderr, "erro: %s sem a entrada int16\n", pasta);
        erro = 1;
    }
    if (erro) {
        liberar_segmento(s);
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Comparacao                                                                 */
/* ------------------------------------------------------------------------- */
/* Metrica do A4 (igual a dsp.erro_estagio do Python):
 *   rel_quadro: por linha, max|c - r| / max|r|; vale a pior linha;
 *   abs:        max|c - r|;
 *   rel:        max|c - r| / |r|, elemento a elemento.
 * Como no Python, a metrica nao se aplica a uma referencia com zero (rel) ou a um
 * quadro de referencia todo em zero (rel_quadro): devolve -1 e o chamador avisa. */
static double metrica_a4(const char *metrica, const float *c, const matriz_t *r)
{
    double pior = 0.0;
    for (int i = 0; i < r->linhas; ++i) {
        double dif_max = 0.0, escala = 0.0, rel_max = 0.0;
        int tem_zero = 0;
        for (int j = 0; j < r->colunas; ++j) {
            const double ref = r->d[i * r->colunas + j];
            const double d = fabs((double)c[i * r->colunas + j] - ref);
            dif_max = d > dif_max ? d : dif_max;
            escala = fabs(ref) > escala ? fabs(ref) : escala;
            if (ref == 0.0) {
                tem_zero = 1;
            } else if (d / fabs(ref) > rel_max) {
                rel_max = d / fabs(ref);
            }
        }
        double v;
        if (strcmp(metrica, "rel_quadro") == 0) {
            if (escala == 0.0) {
                return -1.0;
            }
            v = dif_max / escala;
        } else if (strcmp(metrica, "abs") == 0) {
            v = dif_max;
        } else {   /* "rel": o ler_tolerancias recusa outros nomes */
            if (tem_zero) {
                return -1.0;
            }
            v = rel_max;
        }
        pior = v > pior ? v : pior;
    }
    return pior;
}

static void comparar(int modo, int e, const float *c, const segmento_t *s)
{
    const matriz_t *r = &s->m[e];
    double dif_max = 0.0, escala = 0.0;
    for (int k = 0; k < r->linhas * r->colunas; ++k) {
        const double d = fabs((double)c[k] - r->d[k]);
        dif_max = d > dif_max ? d : dif_max;
        escala = fabs(r->d[k]) > escala ? fabs(r->d[k]) : escala;
    }
    const double met = metrica_a4(tolerancia[e].metrica, c, r);
    if (met < 0.0) {
        fprintf(stderr, "erro: %s, estagio %s: referencia com zero; a metrica %s nao se aplica "
                "(o dsp.erro_estagio do Python recusa o mesmo caso)\n", s->pasta, NOME[e],
                tolerancia[e].metrica);
        referencia_invalida = 1;
        return;
    }
    const double rel = escala > 0.0 ? dif_max / escala : 0.0;
    resultado_t *res = &resultado[modo][e];
    res->erro_abs = dif_max > res->erro_abs ? dif_max : res->erro_abs;
    res->erro_rel = rel > res->erro_rel ? rel : res->erro_rel;
    if (!res->avaliado || met > res->metrica) {
        res->metrica = met;
        snprintf(res->pior, sizeof res->pior, "%s", s->pasta);
    }
    res->avaliado = 1;
    if (verboso) {
        printf("  %-9s %-9s %-24s metrica=%.3e tol=%.0e %s\n", NOME_MODO[modo], NOME[e], s->pasta,
               met, tolerancia[e].tol[modo], met <= tolerancia[e].tol[modo] ? "OK" : "FALHOU");
    }
}

/* ------------------------------------------------------------------------- */
/* Execucao dos estagios portados                                             */
/* ------------------------------------------------------------------------- */
/* Os estagios em C trabalham um quadro por vez, como na placa; o harness junta os
 * quadros nestes buffers (no PC ha memoria de sobra) para comparar com a referencia
 * do segmento inteiro. */
static float x_c[MFCC_N_AMOSTRAS];
static float quadros_c[MFCC_N_QUADROS][MFCC_N_QUADRO];
static float espectro_c[MFCC_N_QUADROS][MFCC_N_BINS];
static float mel_c[MFCC_N_QUADROS][MFCC_N_MEL];
static float entrada_f[MFCC_N_QUADROS * MFCC_N_QUADRO];   /* referencia convertida para float32 */

static void para_float(const matriz_t *m, float *out)
{
    for (int k = 0; k < m->linhas * m->colunas; ++k) {
        out[k] = (float)m->d[k];
    }
}

static void rodar_isolado(const segmento_t *s)
{
    /* escala: a entrada e o proprio int16 */
    mfcc_escala(s->entrada, x_c, MFCC_N_AMOSTRAS);
    comparar(ISOLADO, ESCALA, x_c, s);

    /* janela: cada quadro recebe o trecho da escala de referencia */
    para_float(&s->m[ESCALA], entrada_f);
    for (int q = 0; q < MFCC_N_QUADROS; ++q) {
        mfcc_janela_quadro(&entrada_f[q * MFCC_PASSO], quadros_c[q]);
    }
    comparar(ISOLADO, JANELA, &quadros_c[0][0], s);

    /* espectro: cada quadro recebe o quadro janelado de referencia */
    para_float(&s->m[JANELA], entrada_f);
    for (int q = 0; q < MFCC_N_QUADROS; ++q) {
        mfcc_espectro(&entrada_f[q * MFCC_N_QUADRO], espectro_c[q]);
    }
    comparar(ISOLADO, ESPECTRO, &espectro_c[0][0], s);

    /* mel: cada quadro recebe o espectro de referencia */
    para_float(&s->m[ESPECTRO], entrada_f);
    for (int q = 0; q < MFCC_N_QUADROS; ++q) {
        mfcc_mel(&entrada_f[q * MFCC_N_BINS], mel_c[q]);
    }
    comparar(ISOLADO, MEL, &mel_c[0][0], s);

    /* logmel e mfcc: ainda nao portados (log e DCT, os blocos 4 e 5) */

    /* media e desvio: acumulam, quadro a quadro, o MFCC de referencia */
    float media[MFCC_N_COEFS], desvio[MFCC_N_COEFS], coefs[MFCC_N_COEFS];
    mfcc_resumo_t r;
    mfcc_resumo_iniciar(&r);
    for (int q = 0; q < MFCC_N_QUADROS; ++q) {
        for (int c = 0; c < MFCC_N_COEFS; ++c) {
            coefs[c] = (float)s->m[MFCC].d[q * MFCC_N_COEFS + c];
        }
        mfcc_resumo_acumular(&r, coefs);
    }
    mfcc_resumo_finalizar(&r, media, desvio);
    comparar(ISOLADO, MEDIA, media, s);
    comparar(ISOLADO, DESVIO, desvio, s);

#if TEM_LDA
    /* escore: recebe as 26 caracteristicas de referencia */
    float x[LDA_N_FEATURES];
    for (int c = 0; c < MFCC_N_COEFS; ++c) {
        x[c] = (float)s->m[MEDIA].d[c];
        x[MFCC_N_COEFS + c] = (float)s->m[DESVIO].d[c];
    }
    const float escore = lda_escore(x);
    comparar(ISOLADO, ESCORE, &escore, s);
#endif
}

/* A cadeia como na placa: um quadro por vez, desde as amostras int16. Devolve o
 * ultimo estagio alcancado (o anterior ao primeiro nao portado). */
static int rodar_encadeado(const segmento_t *s)
{
    /* a escala do segmento inteiro so existe aqui, para ser comparada; na placa ela
     * acontece dentro de mfcc_quadro_pcm, quadro a quadro */
    mfcc_escala(s->entrada, x_c, MFCC_N_AMOSTRAS);
    comparar(ENCADEADO, ESCALA, x_c, s);
    for (int q = 0; q < MFCC_N_QUADROS; ++q) {
        mfcc_quadro_pcm(&s->entrada[q * MFCC_PASSO], quadros_c[q]);
        mfcc_espectro(quadros_c[q], espectro_c[q]);
        mfcc_mel(espectro_c[q], mel_c[q]);
        /* Quando o log e a DCT forem portados, o laco continua aqui:
         * logmel <- mel_c[q], coefs <- logmel, e
         * mfcc_resumo_acumular(&r, coefs); depois do laco, finalizar e o escore. */
    }
    comparar(ENCADEADO, JANELA, &quadros_c[0][0], s);
    comparar(ENCADEADO, ESPECTRO, &espectro_c[0][0], s);
    comparar(ENCADEADO, MEL, &mel_c[0][0], s);
    return MEL;
}

/* ------------------------------------------------------------------------- */
/* Relatorio                                                                  */
/* ------------------------------------------------------------------------- */
static int relatar(int modo, int ultimo_encadeado)
{
    int falhas = 0;
    printf("\nmodo %s%s\n", NOME_MODO[modo],
           modo == ISOLADO ? " (cada estagio recebe a referencia do anterior)"
                           : " (a cadeia em C desde o int16)");
    printf("%-9s %-10s %10s %10s %11s %8s %7s  %-24s %s\n", "estagio", "metrica", "erro_abs",
           "erro_rel", "metrica_A4", "tol", "razao", "pior segmento", "situacao");
    for (int e = 0; e < N_ESTAGIOS; ++e) {
        const resultado_t *r = &resultado[modo][e];
        if (!r->avaliado) {
            const char *motivo = !PORTADO[e] ? "nao portado"
                                 : (modo == ENCADEADO && e > ultimo_encadeado) ? "aguarda estagio anterior"
                                 : "nao avaliado";
            if (e == ESCORE && !TEM_LDA) {
                motivo = "nao portado (sem Core/Inc/lda_modelo.h)";
            }
            printf("%-9s %-10s %10s %10s %11s %8.0e %7s  %-24s %s\n", NOME[e], tolerancia[e].metrica,
                   "-", "-", "-", tolerancia[e].tol[modo], "-", "-", motivo);
            continue;
        }
        const double razao = r->metrica / tolerancia[e].tol[modo];
        const int ok = r->metrica <= tolerancia[e].tol[modo];
        falhas += !ok;
        printf("%-9s %-10s %10.3e %10.3e %11.3e %8.0e %6.1f%%  %-24s %s\n", NOME[e],
               tolerancia[e].metrica, r->erro_abs, r->erro_rel, r->metrica,
               tolerancia[e].tol[modo], 100.0 * razao, r->pior, ok ? "OK" : "FALHOU");
    }
    return falhas;
}

/* modelo.txt: de que modelo sao as referencias. Obrigatorio, como o tolerancias.txt;
 * com o lda_modelo.h presente, o bias tem que ser o dele. */
static int conferir_modelo(const char *refs)
{
    char caminho[MAX_CAMINHO], linha[128], id[64] = "", sha[64] = "";
    float bias = 0.0f;
    int tem_bias = 0;
    juntar(caminho, refs, "modelo.txt");
    FILE *f = fopen(caminho, "r");
    if (!f) {
        fprintf(stderr, "erro: nao abri %s; sem ele nao se sabe de que modelo sao as referencias\n",
                caminho);
        return -1;
    }
    while (fgets(linha, sizeof linha, f)) {
        if (sscanf(linha, "bias %f", &bias) == 1) {
            tem_bias = 1;
        }
        sscanf(linha, "id %63s", id);
        sscanf(linha, "json_sha256_12 %63s", sha);
    }
    fclose(f);
    if (!tem_bias || id[0] == '\0') {
        fprintf(stderr, "erro: %s sem o id ou o bias do modelo\n", caminho);
        return -1;
    }
    printf("modelo das referencias: %s (json %s)\n", id, sha[0] ? sha : "?");
#if TEM_LDA
    if (bias != lda_bias) {
        fprintf(stderr, "erro: as referencias sao de outro modelo (bias %.9g, lda_modelo.h %.9g)\n",
                (double)bias, (double)lda_bias);
        return -1;
    }
#endif
    return 0;
}

int main(int argc, char **argv)
{
    const char *refs = REFS_PADRAO;
    int modos[2] = {1, 1};
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--refs") == 0 && i + 1 < argc) {
            refs = argv[++i];
        } else if (strcmp(argv[i], "--modo") == 0 && i + 1 < argc) {
            const char *m = argv[++i];
            modos[ISOLADO] = strcmp(m, "isolado") == 0 || strcmp(m, "ambos") == 0;
            modos[ENCADEADO] = strcmp(m, "encadeado") == 0 || strcmp(m, "ambos") == 0;
            if (!modos[ISOLADO] && !modos[ENCADEADO]) {
                fprintf(stderr, "erro: --modo isolado, encadeado ou ambos\n");
                return 2;
            }
        } else if (strcmp(argv[i], "-v") == 0) {
            verboso = 1;
        } else {
            fprintf(stderr, "uso: %s [--refs PASTA] [--modo isolado|encadeado|ambos] [-v]\n", argv[0]);
            return 2;
        }
    }

    if (mfcc_estagios_init() != 0) {
        fprintf(stderr, "erro: arm_rfft_fast_init_f32 falhou para N = %d\n", MFCC_N_FFT);
        return 2;
    }
    printf("harness: referencias em %s\n", refs);
    if (ler_tolerancias(refs) != 0 || conferir_modelo(refs) != 0) {
        return 2;
    }

    char caminho[MAX_CAMINHO], pasta[256];
    juntar(caminho, refs, "segmentos.txt");
    FILE *lista = fopen(caminho, "r");
    if (!lista) {
        fprintf(stderr, "erro: nao abri %s\n", caminho);
        return 2;
    }
    static segmento_t seg;
    int n_seg = 0, ultimo = ESCALA;
    while (fscanf(lista, "%255s", pasta) == 1) {
        if (ler_segmento(refs, pasta, &seg) != 0) {
            fclose(lista);
            return 2;
        }
        if (modos[ISOLADO]) {
            rodar_isolado(&seg);
        }
        if (modos[ENCADEADO]) {
            ultimo = rodar_encadeado(&seg);
        }
        liberar_segmento(&seg);
        ++n_seg;
    }
    fclose(lista);
    if (n_seg == 0) {
        fprintf(stderr, "erro: segmentos.txt vazio\n");
        return 2;
    }
    printf("%d segmentos; erro_rel = max|c - r| / max|r| no estagio; metrica_A4 e tol do "
           "docs/contrato_numerico.md\n", n_seg);

    int falhas = 0;
    for (int m = 0; m < 2; ++m) {
        if (modos[m]) {
            falhas += relatar(m, ultimo);
        }
    }
    if (referencia_invalida) {
        printf("\nHARNESS: referencias invalidas para a metrica do A4 (ver os erros acima)\n");
        return 2;
    }
    printf("\n%s\n", falhas ? "HARNESS FALHOU" : "HARNESS OK (estagios portados dentro da tolerancia)");
    return falhas ? 1 : 0;
}
