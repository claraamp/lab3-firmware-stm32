/*
 * medida.c -- pilha medida pela marca d'agua (ver medida.h). So no build com
 * -DMEDIDA=ON.
 */

#include "medida.h"

#include "main.h"   /* __get_MSP() pelo CMSIS do Cortex-M4 */

extern uint32_t _estack;   /* topo da RAM (linker script) */
extern uint32_t _end;      /* fim do .bss: o heap comeca aqui */

/* Margem abaixo da pilha atual que nao e pintada: o quadro desta funcao e o do
 * main() ja estao em uso, e o que fica acima do comeco da pintura conta como usado
 * de qualquer jeito. */
#define MARGEM 256u

static uint32_t *inicio_pintura(void)
{
    uint32_t *fundo = (uint32_t *)((uintptr_t)&_estack - MEDIDA_PILHA_PINTADA);
    return fundo < &_end ? &_end : fundo;   /* nunca pinta o .bss */
}

void medida_pintar_pilha(void)
{
    uint32_t *p = inicio_pintura();
    uint32_t *const fim = (uint32_t *)(__get_MSP() - MARGEM);
    while (p < fim) {
        *p++ = MEDIDA_PADRAO;
    }
}

uint32_t medida_pilha_usada(void)
{
    const uint32_t *p = inicio_pintura();
    const uint32_t *const topo = &_estack;
    while (p < topo && *p == MEDIDA_PADRAO) {
        ++p;
    }
    return (uint32_t)((uintptr_t)topo - (uintptr_t)p);
}
