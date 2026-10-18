/*
 * orcamento.c -- build de medida do orcamento de flash e RAM (ver orcamento.h)
 *
 * So entra no build com -DORCAMENTO=ON (CMakeLists.txt da raiz).
 */

#include "orcamento.h"

#include <stdint.h>

#include "arm_math.h"
#include "clipes_demo.h"
#include "lda_modelo.h"
#include "mfcc_estagios.h"

/* Sempre 0. Por ser volatile, o compilador tem que ler a memoria a cada vez e
 * nao pode apagar o ramo abaixo; o linker mantem tudo o que ele usa. */
static volatile uint32_t orcamento_executar = 0u;

/* Para onde vai o resultado do ramo nunca executado: sem isso o compilador
 * poderia descartar as contas cujo resultado ninguem le. */
static volatile float orcamento_saida;

/* Buffers da decimacao em blocos, nos tamanhos que o clipes_demo.h descreve
 * (estado da CMSIS com RECORTE_N_TAPS + RECORTE_BLOCO - 1 floats, um bloco de
 * entrada em float e as saidas de um bloco). Estaticos, como os da cadeia,
 * para entrarem na conta de RAM do linker. */
static arm_fir_decimate_instance_f32 decimador;
static float decimador_estado[RECORTE_N_TAPS + RECORTE_BLOCO - 1];
static float decimador_x[RECORTE_BLOCO];
static float decimador_y[RECORTE_BLOCO / RECORTE_FATOR];

/* Nunca chamada de verdade (ver orcamento_executar). Nao e o laco da cadeia nem
 * o da decimacao: so toca em cada coisa uma vez para ela ir para o link. */
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

    arm_fir_decimate_init_f32(&decimador, RECORTE_N_TAPS, RECORTE_FATOR, recorte_fir,
                              decimador_estado, RECORTE_BLOCO);
    for (int i = 0; i < RECORTE_BLOCO; ++i) {
        decimador_x[i] = (float)recorte_51200[i];
    }
    arm_fir_decimate_f32(&decimador, decimador_x, decimador_y, RECORTE_BLOCO);
    s += decimador_y[0] + (float)recorte_51200[RECORTE_N_AMOSTRAS - 1];

    orcamento_saida = s;
}

void orcamento_referenciar(void)
{
    if (orcamento_executar != 0u) {
        referenciar_tudo();
    }
}
