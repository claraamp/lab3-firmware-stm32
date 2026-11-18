/*
 * medida.h -- build de medida de tempo e pilha (opcao MEDIDA do CMake, card
 * "Medir tempo de execucao e ocupacao de memoria da cadeia embarcada", 11/2026)
 *
 * So entra no build com -DMEDIDA=ON. O firmware normal nao tem nada disto, e a
 * flash e a RAM do card sao medidas nele, nao aqui.
 *
 * Pilha medida: no comeco do main(), medida_pintar_pilha() enche com um padrao os
 * MEDIDA_PILHA_PINTADA bytes do topo da RAM que ainda nao estao em uso (abaixo da
 * pilha atual). medida_pilha_usada() procura, de baixo para cima, a primeira
 * palavra que nao tem mais o padrao: dali ate o topo da RAM foi usado em algum
 * momento desde o boot (a "marca d'agua"). Inclui as interrupcoes que de fato
 * aconteceram, com o quadro de excecao da FPU, e as funcoes da newlib, que a conta
 * estatica (tools/pilha_estatica.py) tira do .elf.
 *
 * A pintura cobre 16 KB, quatro vezes a reserva do linker (_Min_Stack_Size, 4 KB):
 * se a pilha passar da reserva, a medida mostra quanto, em vez de saturar nela.
 */

#ifndef MEDIDA_H
#define MEDIDA_H

#include <stdint.h>

#define MEDIDA_PILHA_PINTADA (16u * 1024u)
#define MEDIDA_PADRAO 0xA5A5A5A5u

/* Pinta a pilha livre abaixo do ponto atual. Chamada no comeco do main() (USER
 * CODE BEGIN 1), antes de tudo, e de novo no modo de recepcao, depois da linha
 * "med inicio": a partir dai a marca d'agua e so a da recepcao, sem o boot. A
 * marca nunca fica abaixo da profundidade em que a pintura foi feita mais 256 B
 * (a margem nao pintada). */
void medida_pintar_pilha(void);

/* Reserva minima de pilha do linker script (_Min_Stack_Size), em bytes. */
uint32_t medida_pilha_reserva(void);

/* Bytes de pilha usados desde o boot (marca d'agua), contados do topo da RAM.
 * MEDIDA_PILHA_PINTADA se a pintura inteira foi usada (a medida saturou). */
uint32_t medida_pilha_usada(void);

#endif /* MEDIDA_H */
