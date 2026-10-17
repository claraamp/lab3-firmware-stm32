/*
 * mfcc_estagios.h -- estagios da extracao de caracteristicas em float32 (CMSIS-DSP)
 *
 * Porte da cadeia do Python (diagnostico-acustico-motores, docs/contrato_numerico.md),
 * um estagio por funcao, para ser conferido estagio a estagio pelo harness no PC
 * (tests/host/harness) e depois usado na placa. Sem HAL: compila no PC com a
 * implementacao generica em C da CMSIS-DSP.
 *
 * Tudo trabalha UM QUADRO POR VEZ, porque a F411 tem 128 KB de RAM: os 98 quadros
 * janelados de um segmento ocupariam 125 KB e o espectro de todos, mais 100 KB. Na
 * placa, o laco de um segmento de 1 s fica assim (cerca de 8 KB de RAM: 5,4 KB fixos
 * deste modulo, para a janela e a FFT, e 2,5 KB de pilha no laco):
 *
 *     mfcc_resumo_t r;
 *     mfcc_resumo_iniciar(&r);
 *     for (int q = 0; q < MFCC_N_QUADROS; ++q) {
 *         float quadro[MFCC_N_QUADRO], potencia[MFCC_N_BINS], mel[MFCC_N_MEL];
 *         float coefs[MFCC_N_COEFS];
 *         mfcc_quadro_pcm(&pcm[q * MFCC_PASSO], quadro);     // escala e janela
 *         mfcc_espectro(quadro, potencia);                    // FFT e potencia
 *         mfcc_mel(potencia, mel);                            // banco de Mel
 *         // log e DCT (blocos 4 e 5) -> coefs
 *         mfcc_resumo_acumular(&r, coefs);                    // Welford
 *     }
 *     mfcc_resumo_finalizar(&r, media, desvio);               // as 26 caracteristicas
 *
 * O `pcm` pode ficar na flash (clipes gravados) ou num buffer circular de 320
 * amostras int16 (audio ao vivo): os quadros se sobrepoem em 192 amostras.
 *
 * Portados: escala, janela, espectro, banco de Mel (bloco 3) e resumo (media e
 * desvio por Welford, o bloco 6). Faltam o log (bloco 4) e a DCT (bloco 5): o
 * harness os mostra como "nao portado" ate existirem aqui.
 */

#ifndef MFCC_ESTAGIOS_H
#define MFCC_ESTAGIOS_H

#include <stddef.h>
#include <stdint.h>

#define MFCC_N_AMOSTRAS 12800   /* 1 s a 12,8 kHz */
#define MFCC_N_QUADRO   320     /* 25 ms */
#define MFCC_PASSO      128     /* 10 ms */
#define MFCC_N_QUADROS  98      /* 1 + (12800 - 320) / 128; as 64 ultimas amostras ficam de fora */
#define MFCC_N_FFT      512
#define MFCC_N_BINS     257     /* 0 a 256 */
#define MFCC_N_MEL      20
#define MFCC_N_COEFS    13

/* Tabelas da janela e do banco de Mel e instancia da FFT. Devolve 0 se tudo
 * certo, -1 se a FFT nao inicializou e -2 se a tabela de bordas do Mel e
 * inconsistente (filtro degenerado, fora do espectro ou total de pesos diferente
 * de MFCC_N_PESOS_MEL). Com -2, nada e escrito em mel_pesos. */
int mfcc_estagios_init(void);

/* Janela de Hann simetrica calculada no init (w[0] = w[319] = 0). */
const float *mfcc_janela_hann(void);

/* 1. x = int16 / 32767 (nao a conversao Q15 da CMSIS, que divide por 32768). */
void mfcc_escala(const int16_t *pcm, float *x, size_t n);

/* 2-4. Multiplica um quadro de 320 amostras ja em escala pela janela. */
void mfcc_janela_quadro(const float x[MFCC_N_QUADRO], float quadro[MFCC_N_QUADRO]);

/* 1-4 juntos, como na placa: as 320 amostras int16 do quadro (a partir de
 * pcm_segmento + q * MFCC_PASSO), em escala e janeladas. */
void mfcc_quadro_pcm(const int16_t pcm[MFCC_N_QUADRO], float quadro[MFCC_N_QUADRO]);

/* 5-6. Espectro de potencia |X|^2 de um quadro janelado (FFT de 512 com zeros no
 * fim), 257 bins, sem normalizar. Nao altera `quadro`. */
void mfcc_espectro(const float quadro[MFCC_N_QUADRO], float potencia[MFCC_N_BINS]);

/* 7-9. Banco de Mel: 20 filtros triangulares (HTK, 20 Hz a 6.400 Hz, bordas por
 * floor, pico 1, sem normalizar por area, bin r fora), aplicados ao espectro de
 * potencia de um quadro: mel[m] = soma_k potencia[k] * fb[m][k]. Guarda so os
 * trechos nao nulos de cada filtro (MFCC_N_PESOS_MEL floats, ~1,9 KB) em vez da
 * matriz densa 20 x 257 (~20 KB). */
#define MFCC_N_PESOS_MEL 479   /* soma de (r - l) dos 20 filtros: 256 + 227 - 4 - 0 */
void mfcc_mel(const float potencia[MFCC_N_BINS], float mel[MFCC_N_MEL]);

/* Bins das 22 bordas do banco (l, c e r do filtro m sao bordas[m], bordas[m+1] e
 * bordas[m+2]), para o harness e o teste de tabela conferirem. */
const uint16_t *mfcc_mel_bordas(void);

/* 12. Media e desvio populacional de cada coeficiente, acumulados quadro a quadro
 * por Welford, sem guardar os quadros (docs/contrato_numerico.md, "Media e desvio
 * em float32"). */
typedef struct {
    uint32_t n;
    float media[MFCC_N_COEFS];
    float m2[MFCC_N_COEFS];
} mfcc_resumo_t;

void mfcc_resumo_iniciar(mfcc_resumo_t *r);
void mfcc_resumo_acumular(mfcc_resumo_t *r, const float coefs[MFCC_N_COEFS]);
/* Com r->n == 0, devolve zeros. */
void mfcc_resumo_finalizar(const mfcc_resumo_t *r, float media[MFCC_N_COEFS],
                           float desvio[MFCC_N_COEFS]);

#endif /* MFCC_ESTAGIOS_H */
