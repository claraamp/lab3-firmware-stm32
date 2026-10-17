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
 *         float quadro[MFCC_N_QUADRO], potencia[MFCC_N_BINS], coefs[MFCC_N_COEFS];
 *         mfcc_quadro_pcm(&pcm[q * MFCC_PASSO], quadro);     // escala e janela
 *         mfcc_espectro(quadro, potencia);                    // FFT e potencia
 *         // Mel, log e DCT (blocos 3 a 5) -> coefs
 *         mfcc_resumo_acumular(&r, coefs);                    // Welford
 *     }
 *     mfcc_resumo_finalizar(&r, media, desvio);               // as 26 caracteristicas
 *
 * O `pcm` pode ficar na flash (clipes gravados) ou num buffer circular de 320
 * amostras int16 (audio ao vivo): os quadros se sobrepoem em 192 amostras.
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
#define MFCC_N_QUADROS  98      /* 1 + (12800 - 320) / 128; as 64 ultimas amostras ficam de fora */
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

/* 2-4. Multiplica um quadro de 320 amostras ja em escala pela janela. */
void mfcc_janela_quadro(const float x[MFCC_N_QUADRO], float quadro[MFCC_N_QUADRO]);

/* 1-4 juntos, como na placa: as 320 amostras int16 do quadro (a partir de
 * pcm_segmento + q * MFCC_PASSO), em escala e janeladas. */
void mfcc_quadro_pcm(const int16_t pcm[MFCC_N_QUADRO], float quadro[MFCC_N_QUADRO]);

/* 5-6. Espectro de potencia |X|^2 de um quadro janelado (FFT de 512 com zeros no
 * fim), 257 bins, sem normalizar. Nao altera `quadro`. */
void mfcc_espectro(const float quadro[MFCC_N_QUADRO], float potencia[MFCC_N_BINS]);

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
