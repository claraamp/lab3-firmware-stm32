/*
 * texto_fixo.h -- float em texto com casas decimais fixas
 *
 * O printf do nano.specs nao tem %f. Esta funcao era o marco_fmt_fixo() do
 * marco_lda.c; saiu dele para o pacote da serial (pacote_serial.c) usar a mesma
 * formatacao sem depender do marco. O marco_fmt_fixo() continua existindo e
 * chama esta.
 *
 * Nao usa a HAL: compila no PC e na placa.
 */

#ifndef TEXTO_FIXO_H
#define TEXTO_FIXO_H

#include <stddef.h>

/* Escreve v com `casas` casas decimais (no maximo 6) e devolve o que o snprintf
 * devolveria. Faixa: |v| * 10^casas < 4e9 (cabe em uint32_t); acima disso
 * escreve "inf" ou "-inf". NaN vira "nan". Nunca escreve "-0.000".
 * Com 6 casas, o limite e |v| < ~4000; com 3 casas, |v| < ~4e6.
 * Esse limite e so o de caber em uint32_t, nao o de precisao: o float tem ~7
 * digitos significativos, entao acima de ~10.000 a terceira casa ja nao e
 * confiavel (perto de 4e6, com 3 casas, o erro chega a dezenas de unidades).
 * Para o escore da LDA (+-1.600) as 3 casas valem; para um numero grande (por
 * exemplo uma distancia de Mahalanobis alta), nao confie nas ultimas casas. */
int texto_fixo(char *buf, size_t n, float v, unsigned casas);

#endif /* TEXTO_FIXO_H */
