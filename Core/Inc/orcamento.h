/*
 * orcamento.h -- build de medida do orcamento de flash e RAM (opcao ORCAMENTO)
 *
 * Com -DORCAMENTO=ON, o firmware leva para o link tudo o que o firmware final vai
 * ter e que o --gc-sections descartaria se ninguem usasse:
 *   - a cadeia completa (mfcc_estagios_init e mfcc_segmento, com os ~11 KB de
 *     buffers estaticos) e a LDA (lda_escore);
 *   - os 5 clipes a 12,8 kHz e o recorte de 1 s a 51,2 kHz (clipes_demo.h).
 *
 * NADA DISSO E EXECUTADO. A chamada fica atras de uma leitura volatile que vale
 * sempre 0: o compilador nao pode provar que o codigo e morto, entao o linker o
 * mantem, mas a placa nunca entra nele. O objetivo e so medir pelo .map (script
 * tools/orcamento_memoria.py).
 *
 * Desde 20/10 o build padrao ja usa tudo isso: a cadeia completa roda nos clipes
 * (cadeia_clipes.c) e a decimacao embarcada roda no recorte (decimacao.c, com os
 * buffers do decimador que ficavam aqui). O build de medida passa a medir o mesmo
 * que o padrao, mais os ~0,5 KB deste arquivo; fica como garantia para quando
 * alguma parte do firmware final ainda nao tiver quem a chame.
 *
 * Desligado por padrao. Nao grave na placa um firmware de demonstracao com ele
 * ligado: o comportamento e o mesmo, mas a flash fica cheia de audio sem uso.
 */

#ifndef ORCAMENTO_H
#define ORCAMENTO_H

/* Chamada uma vez no main(); nao faz nada visivel. */
void orcamento_referenciar(void);

#endif /* ORCAMENTO_H */
