/*
 * Teste no PC da parte sem HAL do marco_lda.c (comparacao com a referencia e
 * formatacao da serial). Nao precisa da placa:
 *
 *   gcc -std=c11 -O2 -Wall -Wextra -Werror -Wdouble-promotion -DMARCO_HOST \
 *       -ICore/Inc tests/host/test_marco_lda.c Core/Src/marco_lda.c -lm -o build/test_marco_lda
 *   ./build/test_marco_lda
 *
 * No PC o gcc nao funde multiplicacao e soma (no Cortex-M4 funde, vfma.f32); os
 * dois ficam dentro da mesma tolerancia, mas os escores nao sao bit a bit iguais.
 */
#include <stdio.h>
#include <string.h>

#include "marco_lda.h"

static int falhas = 0;

static void confere_fmt(float v, unsigned casas, const char *esperado)
{
    char buf[32];
    marco_fmt_fixo(buf, sizeof buf, v, casas);
    if (strcmp(buf, esperado) != 0) {
        printf("fmt(%g, %u) = \"%s\", esperado \"%s\"\n", (double)v, casas, buf, esperado);
        falhas++;
    }
}

int main(void)
{
    confere_fmt(-1577.95867f, 3, "-1577.959");
    confere_fmt(1300.5f, 1, "1300.5");
    confere_fmt(0.000732f, 6, "0.000732");
    confere_fmt(-0.0000001f, 3, "0.000");        /* sem "-0.000" */
    confere_fmt(2527.68311f, 0, "2528");
    confere_fmt(0.05f, 1, "0.1");

    if (!marco_cabecalhos_coerentes()) {
        printf("cabecalhos de modelos diferentes\n");
        falhas++;
    }

    unsigned n_ok = 0;
    char linha[200];
    for (uint16_t i = 0; i < marco_n_segmentos(); ++i) {
        marco_resultado_t r;
        marco_avaliar(i, &r);
        if (r.ok_escore && r.ok_decisao) {
            n_ok++;
        } else {
            marco_linha(linha, sizeof linha, "seg", i, &r, 0u);
            fputs(linha, stdout);
        }
    }
    if (n_ok != marco_n_segmentos()) {
        falhas++;
    }
    marco_resultado_t r;
    marco_avaliar(marco_demo_indice(0), &r);
    marco_linha(linha, sizeof linha, "demo", marco_demo_indice(0), &r, 123u);
    fputs(linha, stdout);

    printf("%u/%u segmentos OK; %s\n", n_ok, (unsigned)marco_n_segmentos(),
           falhas ? "FALHOU" : "tudo certo");
    return falhas ? 1 : 0;
}
