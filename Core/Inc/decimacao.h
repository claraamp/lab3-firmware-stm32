/*
 * decimacao.h -- decimacao embarcada (bloco 0): 51,2 kHz -> 12,8 kHz na placa
 *
 * Card "Decimacao embarcada (bloco 0) e medida de ciclos". Fecha a cadeia desde a
 * taxa da gravacao original:
 *
 *     recorte_51200 (int16, 51,2 kHz) -> arm_fir_decimate_f32 (147 coeficientes,
 *     fator 4) -> requantizacao para int16 -> mfcc_segmento() -> lda_escore()
 *
 * O recorte e o do clipes_demo.h: o segmento 29 (normal) com as margens do
 * decimador causal de fase 0 (75 amostras antes, 73 depois; docs/contrato_numerico.md
 * do Python, secao "Decimacao"). As 37 primeiras saidas tem os zeros do estado
 * inicial e sao descartadas; as 12.800 seguintes sao o segmento.
 *
 * Ao ligar, depois da cadeia, o decimacao_verificar() manda, pela serial:
 *
 *   decim inicio ...                     parametros do recorte e do filtro
 *   decim fase=0 OK                      impulso: a CMSIS calcula y[m] com x[4m] (fase 0)
 *   decim saida n=12800/12837 ... OK     saida comparada com o int16 do Python (clipe_normal)
 *   decim ciclos total=... fir=...       DWT: a decimacao de 1 s de audio
 *   decim escore ref=... placa=... OK    o segmento decimado pela cadeia, contra o escore
 *                                        de referencia do recorte requantizado
 *   decim cadeia_completa ciclos=...     do int16 a 51,2 kHz ao escore
 *   DECIMACAO OK  (ou DECIMACAO FALHOU)
 *
 * COMPARACAO DA SAIDA. A referencia e o clipe_normal (os 12.800 primeiros int16):
 * em float64, o recorte decimado e requantizado da ele bit a bit (conferido no
 * gerador, 09_export_clips.py). Em float32 nao precisa dar: amostras perto de um
 * empate (x,5 LSB) podem arredondar para o outro lado. Tres criterios:
 *   - antes de requantizar, |y * 32767 - ref| <= 0,5 + DECIM_TOL_REL_FLOAT * pico,
 *     com pico = max|ref| em LSB: a referencia float64 fica a ate 0,5 LSB do int16
 *     (o arredondamento), e o resto e o erro do float32, relativo ao pico do
 *     segmento como o rel_quadro do A4 (medido no PC contra o float64);
 *   - depois de requantizar, nenhuma amostra a mais de 1 LSB da referencia;
 *   - no maximo DECIM_MAX_N_DIF amostras diferentes (de 1 LSB).
 * O escore usa a tolerancia ENCADEADA do A4 (rel 1e-5), contra RECORTE_ESCORE_REF_INT16.
 *
 * MEMORIA. A saida inteira fica num buffer int16 de 12.800 amostras (25.600 B,
 * estatico): o mfcc_segmento() le o segmento de 1 s inteiro. Os buffers do
 * decimador sao os do clipes_demo.h: estado de 147 + 512 - 1 floats, um bloco de
 * entrada (512 floats) e as saidas de um bloco (128 floats), ~5,2 KB. O recorte em
 * float inteiro (205 KB) nao caberia na RAM.
 *
 * A parte sem HAL compila no PC com -DDECIMACAO_HOST (tests/host/test_decimacao.c).
 */

#ifndef DECIMACAO_H
#define DECIMACAO_H

#include <stddef.h>
#include <stdint.h>

#include "cadeia_clipes.h"
#include "clipes_demo.h"

/* Maior erro do float32 antes de requantizar, relativo ao pico do segmento
 * (max|y_c - y_py| / max|y_py|, o rel_quadro do A4 com o segmento como um quadro so),
 * contra a decimacao em float64 do contrato. Medido no PC com a CMSIS generica
 * (tests/host/decimacao/comparar_decimacao.py, 20/10): 4,6e-7, ou 2,6e-3 LSB num
 * pico de 5.533 LSB. Tolerancia pela regra do A4: 10 vezes o maior erro, para cima
 * na sequencia 1, 2, 5. Relativa, e nao em LSB, porque o erro do float32 cresce
 * com a amplitude: um segmento mais forte erraria mais LSB com o mesmo porte. */
#define DECIM_TOL_REL_FLOAT 5e-6f

/* Na placa a referencia e o int16 do Python, que ja esta a ate 0,5 LSB do float64:
 * |y * 32767 - ref| <= 0,5 + DECIM_TOL_REL_FLOAT * pico (pico = max|ref| em LSB). */
#define DECIM_TOL_LSB_PLACA(pico) (0.5f + DECIM_TOL_REL_FLOAT * (pico))

/* Depois de requantizar: diferenca maxima e quantas amostras podem diferir. As que
 * diferem sao empates (|y_py * 32767 - ref| perto de 0,5): 1 no PC sem FMA, 2 na
 * emulacao com FMA (clipes_demo.h). Regra do A4: 10 vezes o maior, 20. */
#define DECIM_MAX_DIF_INT16 1
#define DECIM_MAX_N_DIF 20u

/* Saidas que o decimador produz no recorte inteiro e as que ficam. */
#define DECIM_N_SAIDAS_TOTAL (RECORTE_N_AMOSTRAS / RECORTE_FATOR)   /* 12.837 */
#define DECIM_N_SAIDAS 12800u

/* Chamado para cada saida aproveitada j (0 a 12.799), com a saida em float antes
 * de requantizar. So a conferencia e o teste no PC usam; a passada medida nao. */
typedef void (*decimacao_observar_fn)(void *ctx, uint32_t j, float y);

typedef struct {
    uint32_t n_saidas;     /* aproveitadas (tem que ser DECIM_N_SAIDAS) */
    uint32_t n_total;      /* todas as do decimador (tem que ser DECIM_N_SAIDAS_TOTAL) */
    uint32_t n_saturadas;  /* requantizadas fora de -32768..32767 (esperado: 0) */
    uint32_t ciclos;       /* conversao int16 -> float, FIR e requantizacao, o recorte inteiro */
    uint32_t ciclos_fir;   /* so as chamadas da arm_fir_decimate_f32 */
} decimacao_medida_t;

typedef struct {
    float max_lsb;         /* max |y * 32767 - ref| antes de requantizar */
    uint32_t j_max_lsb;    /* onde */
    float pico;            /* max |ref|, em LSB: a escala da tolerancia em float */
    int32_t dif_int16_max; /* max |q - ref| depois de requantizar */
    uint32_t n_dif;        /* amostras com q != ref */
    uint8_t iguais;        /* a passada de conferencia deu o mesmo int16 da passada medida */
    uint8_t ok_saida;      /* os criterios de cima (decimacao_avaliar) */
} decimacao_comparacao_t;

/* ---- sem HAL ------------------------------------------------------------ */

/* Requantizacao do contrato (item 4): lrintf(y * 32767), meio-para-par, SATURADA
 * em -32768..32767 antes do cast. Se `saturou`, soma 1 quando saturar. */
int16_t decimacao_quantizar(float y, uint32_t *saturou);

/* Impulso pelo arm_fir_decimate_f32: com x = [1, 0, 0, ...], a fase 0 da
 * y[m] = b[4m]. Devolve 0 se for a fase 0, ou 1 + a fase encontrada (2 a 4), ou
 * -1 se a saida nao for nenhuma das 4 fases. */
int decimacao_fase(void);

/* Decima o recorte inteiro em blocos de RECORTE_BLOCO (o ultimo com
 * RECORTE_ULTIMO_BLOCO), descarta as RECORTE_DESCARTAR primeiras saidas e grava as
 * 12.800 seguintes, requantizadas, em `saida` (se nao for NULL). `obs` (pode ser
 * NULL) recebe cada saida aproveitada em float. `ciclos` (pode ser NULL) mede o
 * total e so o FIR; `custo` e o de ler o contador, descontado. */
void decimacao_executar(int16_t *saida, decimacao_observar_fn obs, void *ctx,
                        cadeia_ciclos_fn ciclos, uint32_t custo, decimacao_medida_t *m);

/* Decima de novo, sem medir, e compara cada saida com `ref` (antes e depois de
 * requantizar) e com `saida` (a da passada medida). `extra` (pode ser NULL) tambem
 * recebe cada saida: o teste no PC grava as saidas em float por ele. */
void decimacao_conferir(const int16_t *ref, const int16_t *saida, decimacao_observar_fn extra,
                        void *ctx, decimacao_comparacao_t *c);

/* Aplica os criterios a uma comparacao ja preenchida (separado para o teste no PC
 * mostrar que reprova). Preenche ok_saida e devolve ele. */
int decimacao_avaliar(const decimacao_medida_t *m, decimacao_comparacao_t *c);

/* CRC-32 (zlib) das amostras int16 em little-endian, como o Python calcula com
 * zlib.crc32(np.asarray(x, '<i2').tobytes()). */
uint32_t decimacao_crc32(const int16_t *x, size_t n);

/* O buffer da saida (12.800 int16), para o teste no PC conferir. */
const int16_t *decimacao_saida(void);

/* A verificacao inteira, com a saida por `enviar`. Devolve 1 se tudo passou. */
int decimacao_verificar_com(cadeia_enviar_fn enviar, cadeia_ciclos_fn ciclos, uint32_t clock_hz);

/* ---- com HAL (so na placa) ---------------------------------------------- */
#ifndef DECIMACAO_HOST
/* Liga o DWT e roda o decimacao_verificar_com() pela USART1. Cerca de 0,1 s. */
int decimacao_verificar(void);
#endif

#endif /* DECIMACAO_H */
