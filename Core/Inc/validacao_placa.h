/*
 * validacao_placa.h -- segmento completo na placa, para a validacao bloco a bloco
 *
 * Roda a cadeia inteira (mfcc_segmento(): int16 -> 26 caracteristicas) e o
 * lda_escore() nos 10 segmentos dos clipes do clipes_demo.h, mede os ciclos do
 * segmento com o DWT e manda o resultado pela serial. No PC, o
 * tests/host/validacao/validar_blocos.py --placa compara as caracteristicas com
 * as referencias por estagio do Python (media e desvio, pelo A4) e o escore com o
 * escores_referencia.csv. Complementa o blocos13 (blocos 1 a 3 quadro a quadro):
 * aqui entram os blocos 4 a 6 e o escore, com a CMSIS otimizada do Cortex-M4.
 *
 * Protocolo (texto, uma linha por segmento; floats como os 8 digitos hexadecimais
 * do float32, little-endian nao importa: e o valor do uint32 com os mesmos bits):
 *
 *   val inicio modelo=exp239 json=ee0c3e088366 segmentos=10 clock=100MHz
 *   val seg id=29 classe=normal ciclos=<ciclos do segmento> escore=<hex> x=<26 hex separados por ,>
 *   ...
 *   val FIM
 *
 * Leva menos de 1 s: na placa, cada segmento levou 6,145 M ciclos (61,5 ms a
 * 100 MHz, medido em 19/10), e as 10 linhas tem ~310 B cada.
 *
 * A parte sem HAL compila no PC com -DVALIDACAO_HOST
 * (tests/host/validacao/emular_validacao_placa.c gera a mesma saida sem a placa).
 */

#ifndef VALIDACAO_PLACA_H
#define VALIDACAO_PLACA_H

#include <stddef.h>
#include <stdint.h>

typedef void (*val_enviar_fn)(const char *texto, size_t n);
typedef uint32_t (*val_ciclos_fn)(void);

/* Roda os segmentos e manda as linhas por `enviar`. Devolve 0, o codigo do
 * mfcc_estagios_init() / mfcc_segmento() que falhou, ou -4 se uma linha nao
 * coubesse no buffer (manda "val ERRO linha_longa" em vez de uma linha cortada). */
int validacao_placa_rodar(val_enviar_fn enviar, val_ciclos_fn ciclos, uint32_t clock_hz);

#ifndef VALIDACAO_HOST
/* Na placa: liga o DWT e manda tudo pela USART1. */
int validacao_placa_executar(void);
#endif

#endif /* VALIDACAO_PLACA_H */
