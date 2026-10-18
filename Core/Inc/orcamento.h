/*
 * orcamento.h -- build de medida do orcamento de flash e RAM (opcao ORCAMENTO)
 *
 * Com -DORCAMENTO=ON, o firmware leva para o link tudo o que o firmware final vai
 * ter e que hoje o --gc-sections descarta, porque ninguem usa ainda:
 *   - a cadeia completa (mfcc_estagios_init e mfcc_segmento, com os ~11 KB de
 *     buffers estaticos) e a LDA (lda_escore);
 *   - os 5 clipes a 12,8 kHz e o recorte de 1 s a 51,2 kHz (clipes_demo.h);
 *   - os buffers da decimacao em blocos (estado da CMSIS e os blocos de entrada e
 *     saida, nos tamanhos do clipes_demo.h) e a arm_fir_decimate_f32.
 *
 * NADA DISSO E EXECUTADO. A chamada fica atras de uma leitura volatile que vale
 * sempre 0: o compilador nao pode provar que o codigo e morto, entao o linker o
 * mantem, mas a placa nunca entra nele. O objetivo e so medir pelo .map (script
 * tools/orcamento_memoria.py). A cadeia nos clipes de verdade e o card da
 * integracao da cadeia completa; a decimacao de verdade e o card da decimacao.
 *
 * Desligado por padrao. Nao grave na placa um firmware de demonstracao com ele
 * ligado: o comportamento e o mesmo, mas a flash fica cheia de audio sem uso.
 */

#ifndef ORCAMENTO_H
#define ORCAMENTO_H

/* Chamada uma vez no main(); nao faz nada visivel. */
void orcamento_referenciar(void);

#endif /* ORCAMENTO_H */
