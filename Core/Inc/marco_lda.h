/*
 * marco_lda.h -- marco de 13/10: LDA na placa com caracteristicas de referencia
 *
 * A placa recebe as caracteristicas que o Python calculou (lda_vetores_teste.h),
 * calcula o escore com o lda_modelo.h, compara com o escores_referencia.csv,
 * acende o LED da decisao e manda o resultado pela USART1. O MFCC ainda nao esta
 * portado: o marco confere o porte do classificador, nao o desempenho.
 *
 * As funcoes "puras" (sem HAL) compilam tambem no PC com -DMARCO_HOST, para
 * testar a logica de comparacao e a formatacao sem a placa.
 */

#ifndef MARCO_LDA_H
#define MARCO_LDA_H

#include <stddef.h>
#include <stdint.h>

#include "pacote_serial.h"

typedef struct {
    float escore;       /* escore calculado na placa */
    float dif;          /* |escore - referencia| */
    uint8_t falha;      /* decisao da placa: 1 = falha, 0 = normal */
    uint8_t ok_escore;  /* dif <= tolerancia do segmento */
    uint8_t ok_decisao; /* decisao igual a do CSV */
} marco_resultado_t;

/* ---- sem HAL ------------------------------------------------------------ */

/* Numero de segmentos e de segmentos da demonstracao com os LEDs. */
uint16_t marco_n_segmentos(void);
uint16_t marco_n_demo(void);
uint16_t marco_demo_indice(uint16_t k);

/* 1 se o lda_modelo.h e o lda_vetores_teste.h vem do mesmo modelo. */
int marco_cabecalhos_coerentes(void);

/* Calcula o escore do segmento i e compara com a referencia. */
void marco_avaliar(uint16_t i, marco_resultado_t *r);

/* A comparacao em si, separada dos vetores de teste para poder ser testada com
 * uma referencia qualquer (inclusive uma que tem que reprovar). */
void marco_comparar(float escore, float ref, float tol, uint8_t falha_ref,
                    marco_resultado_t *r);

/* Escreve v com `casas` casas decimais (o printf do nano.specs nao tem %f).
 * Chama o texto_fixo() do texto_fixo.c, que o pacote da serial tambem usa.
 * Faixa: |v| * 10^casas < 4e9 (cabe em uint32_t); acima disso escreve "inf".
 * Com 6 casas, o limite e |v| < ~4000; com 3 casas, |v| < ~4e6. */
int marco_fmt_fixo(char *buf, size_t n, float v, unsigned casas);

/* Linha da serial de um segmento. */
int marco_linha(char *buf, size_t n, const char *prefixo, uint16_t i,
                const marco_resultado_t *r, uint32_t ciclos);

/* Pacote da serial (formato v1, pacote_serial.h) do segmento i do marco:
 * origem R, clipe = indice da gravacao, segmento = id do segmento no conjunto.
 * Sem detector de novidade (codigo 0). */
void marco_pacote(uint16_t i, const marco_resultado_t *r, uint32_t n, uint32_t ciclos,
                  pacote_segmento_t *p);

/* ---- com HAL (so na placa) ---------------------------------------------- */
#ifndef MARCO_HOST
/* Confere todos os segmentos e manda uma linha por segmento e o resumo.
 * Devolve 1 se todos os escores e decisoes bateram com a referencia. */
int marco_lda_verificar(void);

/* Um passo do laco principal: se a verificacao passou, mostra o proximo
 * segmento da demonstracao nos LEDs (2 s) e manda a linha demo e o pacote $SEG;
 * senao, pisca os dois LEDs juntos. */
void marco_lda_passo(int verificacao_ok);
#endif

#endif /* MARCO_LDA_H */
