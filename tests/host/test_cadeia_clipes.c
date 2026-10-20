/*
 * test_cadeia_clipes.c -- a cadeia completa dos clipes no PC (Core/Src/cadeia_clipes.c)
 *
 * Roda no PC, com a CMSIS-DSP generica do harness, a mesma verificacao que a placa
 * faz ao ligar (cadeia_verificar_com), e confere:
 *   - os 10 segmentos dos clipes passam (escore e decisao contra o CSV);
 *   - a passada bloco a bloco da as mesmas 26 caracteristicas do mfcc_segmento();
 *   - a comparacao reprova quando deve (escore fora da tolerancia, sinal trocado, NaN);
 *   - o pacote $SEG sai com origem C, clipe e segmento dentro do clipe.
 *
 * Na saida padrao vai o que a placa mandaria (as linhas "cadeia ...", um $CAB e um
 * $SEG por segmento), com os ciclos trocados por nanossegundos do PC (clock de
 * 1000 MHz na conta): o ler_pacotes.py le os $SEG. Os erros vao para a saida de erro.
 *
 *   ./build/harness/test_cadeia_clipes > build/cadeia_pc.txt
 *   python3 tests/host/pacote/ler_pacotes.py --arquivo build/cadeia_pc.txt --resumo
 */

#define _POSIX_C_SOURCE 199309L

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "cadeia_clipes.h"
#include "clipes_demo.h"
#include "lda_modelo.h"
#include "mfcc_estagios.h"

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

/* saida da verificacao: para a saida padrao e para um buffer, para procurar nela */
static char saida[16384];
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

static uint32_t sem_ciclos(void)
{
    return 0u;
}

int main(void)
{
    /* 1. sem o init, a cadeia recusa (o escore sairia o bias da LDA, sem aviso) */
    {
        cadeia_resultado_t r;
        const int e = cadeia_inferir(0, sem_ciclos, 0u, &r);
        CONFERIR(e == -3, "cadeia_inferir sem init devolveu %d, esperado -3", e);
    }

    /* 2. a verificacao da placa, no PC */
    const int ok = cadeia_verificar_com(enviar, ns, 1000000000u);
    CONFERIR(ok == 1, "cadeia_verificar_com devolveu %d", ok);
    CONFERIR(strstr(saida, "CADEIA OK\r\n") != NULL, "sem CADEIA OK na saida");
    CONFERIR(strstr(saida, "ok=10/10") != NULL, "resumo sem ok=10/10");
    CONFERIR(strstr(saida, "decisoes_iguais=10/10") != NULL, "resumo sem decisoes_iguais=10/10");
    CONFERIR(strstr(saida, "iguais_blocos=10/10") != NULL,
             "a passada bloco a bloco nao deu as mesmas caracteristicas do mfcc_segmento()");
    CONFERIR(strstr(saida, "FALHOU") == NULL, "alguma linha FALHOU");

    /* 3. cada segmento: mesma decisao do CSV, dentro da tolerancia, e o pacote */
    char cab[PACOTE_TAM_MAX];
    const int lc = pacote_cabecalho(cab, sizeof cab, CLIPES_MODELO_ID, CLIPES_JSON_SHA256_12,
                                    (unsigned)LDA_N_FEATURES, 100u);
    CONFERIR(lc > 0, "pacote_cabecalho devolveu %d", lc);
    fwrite(cab, 1, (size_t)(lc > 0 ? lc : 0), stdout);

    float razao_max = 0.0f;
    for (uint16_t s = 0; s < cadeia_n_segmentos(); ++s) {
        cadeia_resultado_t r;
        CONFERIR(cadeia_avaliar(s, sem_ciclos, 0u, &r) == 0, "cadeia_avaliar(%u) falhou", (unsigned)s);
        CONFERIR(r.ok_escore && r.ok_decisao, "segmento %u: dif=%g tol=%g", (unsigned)s,
                 (double)r.dif, (double)r.tol);
        CONFERIR(r.iguais, "segmento %u: passada bloco a bloco diferente", (unsigned)s);
        razao_max = fmaxf(razao_max, r.dif / r.tol);

        pacote_segmento_t p;
        cadeia_pacote(s, &r, s, &p);
        const clipe_segmento_t *seg = &clipes_segmentos[s];
        CONFERIR(p.origem == 'C', "origem %c", p.origem);
        CONFERIR(p.clipe == seg->clipe, "segmento %u: clipe %ld", (unsigned)s, (long)p.clipe);
        CONFERIR(p.segmento == (int32_t)(s - clipes[seg->clipe].primeiro_segmento),
                 "segmento %u: indice no clipe %ld", (unsigned)s, (long)p.segmento);
        CONFERIR(p.falha == seg->falha_ref, "segmento %u: decisao do pacote diferente do CSV", (unsigned)s);
        CONFERIR(p.cod_novidade == 0, "codigo de novidade %u", (unsigned)p.cod_novidade);

        char linha[PACOTE_TAM_MAX];
        const int n = pacote_linha(linha, sizeof linha, &p);
        CONFERIR(n > 0, "pacote_linha devolveu %d", n);
        char inicio[64];
        snprintf(inicio, sizeof inicio, "$SEG,1,%u,C,%ld,%ld,%c,", (unsigned)s, (long)p.clipe,
                 (long)p.segmento, p.falha ? 'F' : 'N');
        CONFERIR(n > 0 && strncmp(linha, inicio, strlen(inicio)) == 0, "pacote: %s", linha);
        if (n > 0) {
            fwrite(linha, 1, (size_t)n, stdout);
        }
    }

    /* 4. a comparacao reprova quando deve */
    {
        const float ref = clipes_segmentos[2].escore_ref;   /* falha no CSV */
        const float tol = CADEIA_TOL_ESCORE_REL * fabsf(ref);
        cadeia_resultado_t r;

        cadeia_comparar(ref + 0.9f * tol, ref, 1u, &r);
        CONFERIR(r.ok_escore && r.ok_decisao, "0,9 tol deveria passar");
        cadeia_comparar(ref + 2.0f * tol, ref, 1u, &r);
        CONFERIR(!r.ok_escore, "2 tol deveria reprovar o escore");
        cadeia_comparar(-ref, ref, 1u, &r);
        CONFERIR(!r.ok_escore && !r.ok_decisao, "sinal trocado deveria reprovar escore e decisao");
        cadeia_comparar(NAN, ref, 1u, &r);
        CONFERIR(!r.ok_escore, "NaN deveria reprovar");
        cadeia_comparar(0.0f, -0.0001f, 0u, &r);
        CONFERIR(r.falha == 1 && !r.ok_decisao, "escore 0 e falha (empate e falha)");
    }

    fprintf(stderr, "maior dif/tol nos 10 segmentos: %.1f%%\n", (double)(100.0f * razao_max));
    if (falhas) {
        fprintf(stderr, "TEST CADEIA FALHOU (%d)\n", falhas);
        return 1;
    }
    fprintf(stderr, "TEST CADEIA OK (10 segmentos, passada bloco a bloco igual, reprova quando deve)\n");
    return 0;
}
