/*
 * orcamento.c -- build de medida do orcamento de flash e RAM (ver orcamento.h)
 *
 * So entra no build com -DORCAMENTO=ON (CMakeLists.txt da raiz).
 */

#include "orcamento.h"

#include <stdint.h>

#include "clipes_demo.h"
#include "lda_modelo.h"
#include "mfcc_estagios.h"

/* Sempre 0. Por ser volatile, o compilador tem que ler a memoria a cada vez e
 * nao pode apagar o ramo abaixo; o linker mantem tudo o que ele usa. */
static volatile uint32_t orcamento_executar = 0u;

/* Para onde vai o resultado do ramo nunca executado: sem isso o compilador
 * poderia descartar as contas cujo resultado ninguem le. */
static volatile float orcamento_saida;

/* Os buffers da decimacao ficavam aqui ate 20/10. Agora sao os de verdade, no
 * decimacao.c, que o build padrao usa; manter uma copia aqui contaria a RAM deles
 * duas vezes no build de medida. */

/* Nunca chamada de verdade (ver orcamento_executar). Nao e o laco da cadeia: so
 * toca em cada coisa uma vez para ela ir para o link. */
static void __attribute__((noinline)) referenciar_tudo(void)
{
    float x[2 * MFCC_N_COEFS];
    float s = 0.0f;

    if (mfcc_estagios_init() != 0) {
        return;
    }
    for (int c = 0; c < CLIPES_N_CLIPES; ++c) {
        if (mfcc_segmento(clipes[c].pcm, x) == 0) {
            s += lda_escore(x);
        }
    }
    s += clipes_segmentos[0].escore_ref;

    s += (float)recorte_51200[RECORTE_N_AMOSTRAS - 1] + recorte_fir[0];

    orcamento_saida = s;
}

void orcamento_referenciar(void)
{
    if (orcamento_executar != 0u) {
        referenciar_tudo();
    }
}
