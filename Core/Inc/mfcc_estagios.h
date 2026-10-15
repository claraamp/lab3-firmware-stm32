/*
 * mfcc_estagios.h -- estagios da extracao de caracteristicas em float32 (CMSIS-DSP)
 *
 * Porte da cadeia do Python (diagnostico-acustico-motores, docs/contrato_numerico.md),
 * um estagio por funcao, para ser conferido estagio a estagio pelo harness no PC
 * (tests/host/harness) e depois usado na placa. Sem HAL: compila no PC com a
 * implementacao generica em C da CMSIS-DSP.
 *
 * Portados: escala, janela, espectro e resumo (media e desvio por Welford, o
 * bloco 6). Faltam o banco de Mel (bloco 3), o log (bloco 4) e a DCT (bloco 5):
 * o harness os mostra como "nao portado" ate existirem aqui.
 */

#ifndef MFCC_ESTAGIOS_H
#define MFCC_ESTAGIOS_H

#include <stddef.h>
#include <stdint.h>

#define MFCC_N_AMOSTRAS 12800   /* 1 s a 12,8 kHz */
#define MFCC_N_QUADRO   320     /* 25 ms */
#define MFCC_PASSO      128     /* 10 ms */
#define MFCC_N_QUADROS  98      /* 1 + (12800 - 320) / 128 */
#define MFCC_N_FFT      512
#define MFCC_N_BINS     257     /* 0 a 256 */
#define MFCC_N_MEL      20
#define MFCC_N_COEFS    13

/* Tabela da janela e instancia da FFT. Devolve 0 se tudo certo. */
int mfcc_estagios_init(void);

/* Janela de Hann simetrica calculada no init (w[0] = w[319] = 0). */
const float *mfcc_janela_hann(void);

/* 1. x = int16 / 32767 (nao a conversao Q15 da CMSIS, que divide por 32768). */
void mfcc_escala(const int16_t *pcm, float *x, size_t n);

/* 2-4. Quadros de 320 amostras com passo de 128, multiplicados pela janela. */
void mfcc_janela(const float x[MFCC_N_AMOSTRAS], float quadros[MFCC_N_QUADROS][MFCC_N_QUADRO]);

/* 5-6. Espectro de potencia |X|^2 de um quadro janelado (FFT de 512 com zeros no
 * fim), 257 bins, sem normalizar. Nao altera `quadro`. */
void mfcc_espectro(const float quadro[MFCC_N_QUADRO], float potencia[MFCC_N_BINS]);

/* 12. Media e desvio populacional de cada coeficiente ao longo dos quadros, por
 * Welford, quadro a quadro (docs/contrato_numerico.md, "Media e desvio em float32"). */
void mfcc_resumo(const float mfcc[][MFCC_N_COEFS], size_t n_quadros,
                 float media[MFCC_N_COEFS], float desvio[MFCC_N_COEFS]);

#endif /* MFCC_ESTAGIOS_H */
