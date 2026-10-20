/*
 * test_decimacao.c -- a decimacao embarcada no PC (Core/Src/decimacao.c)
 *
 * Roda no PC, com a CMSIS-DSP generica do harness, a mesma verificacao que a placa
 * faz ao ligar (decimacao_verificar_com), e confere:
 *   - a fase do arm_fir_decimate_f32 e a 0 (impulso);
 *   - o recorte da 12.800 saidas (com o ultimo bloco de 148), sem saturar, dentro
 *     dos criterios contra o int16 do Python, e o escore do segmento decimado;
 *   - a requantizacao satura em vez de dar a volta;
 *   - os criterios reprovam quando devem (saida curta, 2 LSB, amostras demais
 *     diferentes, erro em float acima da tolerancia, NaN, passadas diferentes).
 *
 * Com um argumento, grava as 12.800 saidas em float32 (little-endian) nesse
 * arquivo, para o comparar_decimacao.py comparar com a referencia em float64:
 *
 *   ./build/harness/test_decimacao build/decimacao_c.f32 > build/decimacao_pc.txt
 *   python3 tests/host/decimacao/comparar_decimacao.py build/decimacao_c.f32
 *
 * Na saida padrao vai o que a placa mandaria (as linhas "decim ..."), com os ciclos
 * trocados por nanossegundos do PC (clock de 1000 MHz na conta).
 */

#define _POSIX_C_SOURCE 199309L

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "clipes_demo.h"
#include "decimacao.h"

static int falhas = 0;

#define CONFERIR(cond, ...)                                    \
    do {                                                       \
        if (!(cond)) {                                         \
            fprintf(stderr, "FALHOU (linha %d): ", __LINE__);  \
            fprintf(stderr, __VA_ARGS__);                      \
            fprintf(stderr, "\n");                             \
            ++falhas;                                          \
        }                                                      \
    } while (0)

static char saida[8192];
static size_t n_saida = 0;

static void enviar(const char *texto, size_t n)
{
    fwrite(texto, 1, n, stdout);
    if (n_saida + n < sizeof saida) {
        memcpy(&saida[n_saida], texto, n);
        n_saida += n;
        saida[n_saida] = '\0';
    }
}

static uint32_t ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)((uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec);
}

/* as saidas em float, na ordem, para o arquivo */
static float y_c[DECIM_N_SAIDAS];
static uint32_t n_y = 0u;

static void guardar(void *ctx, uint32_t j, float y)
{
    (void)ctx;
    if (j < DECIM_N_SAIDAS) {
        y_c[j] = y;
        n_y = j + 1u;
    }
}

/* uma comparacao que passa, para estragar um campo de cada vez */
static void base(decimacao_medida_t *m, decimacao_comparacao_t *c)
{
    memset(m, 0, sizeof *m);
    memset(c, 0, sizeof *c);
    m->n_saidas = DECIM_N_SAIDAS;
    m->n_total = DECIM_N_SAIDAS_TOTAL;
    c->max_lsb = 0.5f;
    c->pico = 5533.0f;
    c->dif_int16_max = 1;
    c->n_dif = 2u;
    c->iguais = 1u;
}

int main(int argc, char **argv)
{
    /* 1. requantizacao: escala de 32767, arredonda e satura */
    {
        uint32_t sat = 0u;
        CONFERIR(decimacao_quantizar(1.0f, &sat) == 32767 && sat == 0u, "1,0 deveria dar 32767");
        CONFERIR(decimacao_quantizar(-1.0f, &sat) == -32767 && sat == 0u, "-1,0 deveria dar -32767");
        CONFERIR(decimacao_quantizar(0.0f, &sat) == 0, "0 deveria dar 0");
        CONFERIR(decimacao_quantizar(1.5f, &sat) == 32767 && sat == 1u, "1,5 deveria saturar em 32767");
        CONFERIR(decimacao_quantizar(-1.5f, &sat) == -32768 && sat == 2u, "-1,5 deveria saturar em -32768");
        /* 2,5 e 3,5 LSB exatos (32767 * k / 32767 em float nao e exato; usar a
         * conta inversa com valores representaveis): meio-para-par */
        CONFERIR(lrintf(2.5f) == 2 && lrintf(3.5f) == 4 && lrintf(-2.5f) == -2,
                 "lrintf nao arredonda o meio para o par neste modo de arredondamento");
    }

    /* 2. a fase do decimador */
    CONFERIR(decimacao_fase() == 0, "decimacao_fase() = %d, esperado 0", decimacao_fase());

    /* 3. a verificacao da placa, no PC */
    const int ok = decimacao_verificar_com(enviar, ns, 1000000000u);
    CONFERIR(ok == 1, "decimacao_verificar_com devolveu %d", ok);
    CONFERIR(strstr(saida, "DECIMACAO OK\r\n") != NULL, "sem DECIMACAO OK na saida");
    CONFERIR(strstr(saida, "FALHOU") == NULL, "alguma linha FALHOU");
    CONFERIR(strstr(saida, "n=12800/12837") != NULL, "a saida nao tem 12.800 de 12.837 saidas");

    /* 4. a passada medida e a conferencia, separadas, e as saidas em float */
    decimacao_medida_t m;
    decimacao_executar(NULL, guardar, NULL, NULL, 0u, &m);
    CONFERIR(m.n_saidas == DECIM_N_SAIDAS && n_y == DECIM_N_SAIDAS, "n_saidas=%lu",
             (unsigned long)m.n_saidas);
    CONFERIR(m.n_total == DECIM_N_SAIDAS_TOTAL, "n_total=%lu", (unsigned long)m.n_total);
    CONFERIR(m.n_saturadas == 0u, "saturadas=%lu", (unsigned long)m.n_saturadas);

    decimacao_comparacao_t c;
    decimacao_conferir(clipes[RECORTE_CLIPE].pcm, decimacao_saida(), NULL, NULL, &c);
    CONFERIR(c.iguais, "a conferencia deu outro int16 que a passada medida");
    CONFERIR(c.dif_int16_max <= 1, "dif_int16_max=%ld", (long)c.dif_int16_max);
    fprintf(stderr, "PC: max_lsb=%.6f (j=%lu) tol=%.6f pico=%.0f dif_int16_max=%ld n_dif=%lu\n",
            (double)c.max_lsb, (unsigned long)c.j_max_lsb, (double)DECIM_TOL_LSB_PLACA(c.pico),
            (double)c.pico, (long)c.dif_int16_max, (unsigned long)c.n_dif);

    /* 5. os criterios reprovam quando devem */
    {
        decimacao_medida_t mm;
        decimacao_comparacao_t cc;
        base(&mm, &cc);
        CONFERIR(decimacao_avaliar(&mm, &cc), "a comparacao base deveria passar");

        base(&mm, &cc);
        mm.n_saidas = DECIM_N_SAIDAS - 37u;   /* esqueceu o ultimo bloco de 148 */
        mm.n_total = DECIM_N_SAIDAS_TOTAL - 37u;
        CONFERIR(!decimacao_avaliar(&mm, &cc), "sem o ultimo bloco deveria reprovar");

        base(&mm, &cc);
        mm.n_saturadas = 1u;
        CONFERIR(!decimacao_avaliar(&mm, &cc), "amostra saturada deveria reprovar");

        base(&mm, &cc);
        cc.dif_int16_max = 2;
        CONFERIR(!decimacao_avaliar(&mm, &cc), "2 LSB deveria reprovar");

        base(&mm, &cc);
        cc.n_dif = DECIM_MAX_N_DIF + 1u;
        CONFERIR(!decimacao_avaliar(&mm, &cc), "amostras diferentes demais deveria reprovar");

        base(&mm, &cc);
        cc.max_lsb = DECIM_TOL_LSB_PLACA(cc.pico) * 1.01f;
        CONFERIR(!decimacao_avaliar(&mm, &cc), "erro em float acima da tolerancia deveria reprovar");

        base(&mm, &cc);
        cc.max_lsb = NAN;
        CONFERIR(!decimacao_avaliar(&mm, &cc), "NaN deveria reprovar");

        base(&mm, &cc);
        cc.iguais = 0u;
        CONFERIR(!decimacao_avaliar(&mm, &cc), "passadas diferentes deveriam reprovar");
    }

    /* 6. uma referencia errada de 1 LSB em muitas amostras e pega pela conferencia */
    {
        static int16_t ref_errada[DECIM_N_SAIDAS];
        memcpy(ref_errada, clipes[RECORTE_CLIPE].pcm, sizeof ref_errada);
        for (uint32_t j = 0; j < DECIM_N_SAIDAS; j += 100u) {
            ref_errada[j] = (int16_t)(ref_errada[j] + 1);
        }
        decimacao_comparacao_t ce;
        decimacao_conferir(ref_errada, decimacao_saida(), NULL, NULL, &ce);
        CONFERIR(!decimacao_avaliar(&m, &ce), "128 amostras erradas deveriam reprovar (n_dif=%lu)",
                 (unsigned long)ce.n_dif);
    }

    if (argc > 1) {
        FILE *f = fopen(argv[1], "wb");
        CONFERIR(f != NULL, "nao abriu %s", argv[1]);
        if (f) {
            CONFERIR(fwrite(y_c, sizeof y_c[0], DECIM_N_SAIDAS, f) == DECIM_N_SAIDAS, "fwrite");
            fclose(f);
            fprintf(stderr, "saidas em float32 gravadas em %s\n", argv[1]);
        }
    }

    if (falhas) {
        fprintf(stderr, "TEST DECIMACAO FALHOU (%d)\n", falhas);
        return 1;
    }
    fprintf(stderr, "TEST DECIMACAO OK (fase 0, 12.800 saidas, escore, reprova quando deve)\n");
    return 0;
}
