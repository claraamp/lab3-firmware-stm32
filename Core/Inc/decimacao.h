/*
 * decimacao.h -- decimacao embarcada (bloco 0): 51,2 kHz -> 12,8 kHz na placa
 *
 * Card "Decimacao embarcada (bloco 0) e medida de ciclos". Fecha a cadeia desde a
 * taxa da gravacao original, quadro a quadro, como o audio ao vivo vai rodar:
 *
 *     recorte_51200 (int16, 51,2 kHz) -> arm_fir_decimate_f32 (147 coeficientes,
 *     fator 4, blocos de 512) -> 128 saidas requantizadas para int16 por bloco
 *     -> buffer de um quadro (320 amostras) -> mfcc_quadro_coefs() e
 *     mfcc_resumo_acumular() a cada passo de 128 -> lda_escore()
 *
 * O recorte e o do clipes_demo.h: o segmento 29 (normal) com as margens do
 * decimador causal de fase 0 (75 amostras antes, 73 depois; docs/contrato_numerico.md
 * do Python, secao "Decimacao"). As 37 primeiras saidas tem os zeros do estado
 * inicial e sao descartadas; as 12.800 seguintes sao o segmento.
 *
 * CONTA EM LSB. A entrada vai para o FIR como (float)int16, sem a divisao por 32767
 * de cada amostra, e os coeficientes ficam como estao (numa copia em RAM, 588 B):
 * sum b[k] x[n-k] ja sai em LSB, pronta para o arredondamento. E a mesma conta do
 * Python, que divide a entrada por 32767 e multiplica a saida por 32767: as duas
 * escalas se cancelam. A requantizacao e
 * a do contrato (meio-para-par, saturada), pelo vcvtr.s32.f32 na placa (uma
 * instrucao, pelo modo do FPSCR, que e meio-para-par) e pelo lrintf no PC.
 *
 * Ao ligar, depois da cadeia, o decimacao_verificar() manda, pela serial:
 *
 *   decim inicio ...                     parametros do recorte e do filtro
 *   decim fase=0 OK                      impulso: a CMSIS calcula y[m] com x[4m] (fase 0)
 *   decim saida n=12800/12837 ... OK     saida comparada com o int16 do Python (clipe_normal)
 *   decim ciclos total=... fir=...       DWT: a decimacao e a cadeia, quadro a quadro, 1 s
 *   decim escore ref=... placa=... OK    o escore do segmento decimado, contra o de
 *                                        referencia do recorte requantizado
 *   DECIMACAO OK duracao=... ms          (ou DECIMACAO FALHOU)
 *
 * COMPARACAO DA SAIDA. A referencia e o clipe_normal (os 12.800 primeiros int16):
 * em float64, o recorte decimado e requantizado da ele bit a bit (conferido no
 * gerador, 09_export_clips.py). Em float32 nao precisa dar: amostras perto de um
 * empate (x,5 LSB) podem arredondar para o outro lado. Os criterios da placa:
 *   - antes de requantizar, |y - ref| <= 0,5 + DECIM_TOL_REL_FLOAT * pico (y em LSB,
 *     pico = max|ref|). E uma conferencia FROUXA: a referencia da placa e o int16,
 *     que ja tem ate 0,5 LSB de arredondamento, entao um erro de ate ~0,5 LSB passa
 *     (~1e-4 do pico de 5.533). A tolerancia de verdade, DECIM_TOL_REL_FLOAT contra o
 *     float64, so e conferida no PC (tests/host/decimacao/comparar_decimacao.py e
 *     tolerancias_gravacoes.py). Na placa, quem pega um erro de porte fino e o escore;
 *   - depois de requantizar, nenhuma amostra a mais de 1 LSB da referencia. Quantas
 *     diferem de 1 LSB nao e criterio: sao empates, e o numero cresce com a amplitude
 *     (ate 31 por segmento nas gravacoes fortes, revisao da Julia no PR #9). Vai na
 *     linha so como informacao;
 *   - as duas passadas (a medida e a de conferencia) dao o mesmo int16 (CRC-32).
 * O escore usa a tolerancia ENCADEADA do A4 (rel 1e-5), contra RECORTE_ESCORE_REF_INT16.
 *
 * MEMORIA. Nao ha buffer do segundo inteiro: a cadeia recebe as saidas num buffer
 * de um quadro (320 int16, 640 B), com a sobreposicao de 192 amostras entre quadros.
 * Os buffers do decimador sao os do clipes_demo.h: estado de 147 + 512 - 1 floats,
 * um bloco de entrada (512 floats) e as saidas de um bloco (128 floats). O recorte
 * em float inteiro (205 KB) nao caberia na RAM.
 *
 * NO LACO (22/10, primeira inferencia de ponta a ponta). Antes de cada volta dos
 * 10 segmentos dos clipes, o decimacao_passo() roda o mesmo decimacao_executar(),
 * do int16 a 51,2 kHz ao escore, acende o LED da decisao por 2 s e manda:
 *
 *   ponta seg=29 clipe=normal fs=51200->12800 ref=... placa=... dif=... tol=...
 *         dec=normal OK ciclos=... (... ms, ...% do tempo real) decimacao=...
 *         cadeia=... crc32=... igual_boot=1
 *   $SEG,1,n,D,0,0,N,0,-1331.769,0,,,17453346*HH
 *
 * (a primeira numa linha so). E uma inferencia de verdade a cada volta, nao um valor
 * guardado: o recorte passa de novo pelo FIR e pela cadeia. `igual_boot=1` diz que
 * ela deu o mesmo int16 (CRC-32) e o mesmo escore, bit a bit, da passada medida no
 * boot; OK pede isso, a tolerancia encadeada do A4 contra RECORTE_ESCORE_REF_INT16
 * e a decisao do CSV (normal). Sem OK, a linha e o pacote saem do mesmo jeito, mas
 * os LEDs piscam o erro em vez de mostrar uma decisao. No pacote, origem D, clipe
 * e segmento sao os do clipe normal (o recorte e o primeiro segmento dele), e os
 * ciclos vao do int16 a 51,2 kHz ao escore.
 *
 * O RECORTE E UM SEGMENTO DO TREINO DO MODELO FINAL, como os dos clipes: mostra a
 * fidelidade do porte, nao o desempenho.
 *
 * A parte sem HAL compila no PC com -DDECIMACAO_HOST (tests/host/test_decimacao.c e
 * tests/host/decimacao/decimar_gravacao.c).
 */

#ifndef DECIMACAO_H
#define DECIMACAO_H

#include <stddef.h>
#include <stdint.h>

#include "cadeia_clipes.h"
#include "clipes_demo.h"
#include "pacote_serial.h"

/* Maior erro do float32 antes de requantizar, relativo ao pico do segmento
 * (max|y_c - y_py| / max|y_py|, o rel_quadro do A4 com o segmento como um quadro so),
 * contra a decimacao em float64 do contrato. Tolerancia pela regra do A4: 10 vezes o
 * maior erro medido, para cima na sequencia 1, 2, 5. Relativa, e nao em LSB, porque
 * o erro do float32 cresce com a amplitude.
 *
 * Medida nos 295 segmentos das 5 gravacoes pelo
 * tests/host/decimacao/tolerancias_gravacoes.py, com o codigo C deste arquivo (a
 * conta em LSB), 20/10: 8,67e-7 no pior caso (segmento 57), o que pela regra da
 * 1e-5; o pior caso usa 8,7 % dela. Antes da conta em LSB, a Julia mediu 9,3e-7
 * (revisao do PR #9). Tabela no README, "Medida nos 295 segmentos". */
#define DECIM_TOL_REL_FLOAT 1e-5f

/* Na placa a referencia e o int16 do Python, que ja esta a ate 0,5 LSB do float64. */
#define DECIM_TOL_LSB_PLACA(pico) (0.5f + DECIM_TOL_REL_FLOAT * (pico))

/* Depois de requantizar, nenhuma amostra pode estar a mais de 1 LSB do Python. */
#define DECIM_MAX_DIF_INT16 1

/* Saidas que o decimador produz no recorte inteiro e as que ficam. */
#define DECIM_N_SAIDAS_TOTAL (RECORTE_N_AMOSTRAS / RECORTE_FATOR)   /* 12.837 */
#define DECIM_N_SAIDAS 12800u

/* Chamado para cada saida aproveitada j (0 a 12.799), com a saida em float (em LSB)
 * antes de requantizar. So a conferencia e o teste no PC usam; a passada medida nao. */
typedef void (*decimacao_observar_fn)(void *ctx, uint32_t j, float y);

typedef struct {
    uint32_t n_saidas;      /* aproveitadas (tem que ser DECIM_N_SAIDAS) */
    uint32_t n_total;       /* todas as do decimador (tem que ser DECIM_N_SAIDAS_TOTAL) */
    uint32_t n_saturadas;   /* requantizadas fora de -32768..32767 (esperado: 0) */
    uint32_t n_quadros;     /* quadros que entraram no resumo (tem que ser MFCC_N_QUADROS) */
    uint32_t crc32;         /* CRC-32 das 12.800 saidas requantizadas */
    float escore;           /* lda_escore() do segmento decimado */
    uint32_t ciclos;        /* tudo: conversao, FIR, requantizacao, cadeia e LDA, 1 s */
    uint32_t ciclos_fir;    /* so as chamadas da arm_fir_decimate_f32 */
    uint32_t ciclos_cadeia; /* so a cadeia: os 98 quadros, o fim do resumo e a LDA */
} decimacao_medida_t;

typedef struct {
    float max_lsb;          /* max |y - ref| antes de requantizar, em LSB */
    uint32_t j_max_lsb;     /* onde */
    float pico;             /* max |ref|, em LSB: a escala da tolerancia em float */
    int32_t dif_int16_max;  /* max |q - ref| depois de requantizar */
    uint32_t n_dif;         /* amostras com q != ref (so informacao: empates) */
    uint32_t crc32;         /* CRC-32 das saidas requantizadas desta passada */
    uint8_t iguais;         /* o mesmo CRC da passada medida (decimacao_avaliar) */
    uint8_t ok_saida;       /* os criterios de cima (decimacao_avaliar) */
} decimacao_comparacao_t;

/* ---- sem HAL ------------------------------------------------------------ */

/* Coeficientes copiados para a RAM e o estado do decimador zerado. Chamar antes de uma sequencia de decimacao_bloco(). */
void decimacao_iniciar(void);

/* Um bloco de n amostras int16 (multiplo de 4, no maximo RECORTE_BLOCO) pelo
 * decimador, continuando o estado das chamadas anteriores: grava n / 4 saidas em
 * LSB em y. Devolve os ciclos so do FIR (0 se `ciclos` for NULL). O
 * decimar_gravacao.c usa isto para decimar as gravacoes inteiras com o mesmo codigo
 * da placa. */
uint32_t decimacao_bloco(const int16_t *x, uint32_t n, float *y, cadeia_ciclos_fn ciclos,
                         uint32_t custo);

/* Requantizacao do contrato (item 4) de uma saida ja em LSB: arredonda o meio para
 * o par e SATURA em -32768..32767 antes do cast. Se `saturou`, soma 1 quando saturar. */
int16_t decimacao_quantizar(float y_lsb, uint32_t *saturou);

/* Impulso pelo arm_fir_decimate_f32: com x = [1, 0, 0, ...], a fase 0 da
 * y[m] = b[4m]. Devolve 0 se for a fase 0, ou 1 + a fase encontrada (2 a 4),
 * ou -1 se a saida nao for nenhuma das 4 fases. */
int decimacao_fase(void);

/* O caminho do firmware: decima o recorte inteiro e, a cada saida aproveitada
 * requantizada, alimenta a cadeia quadro a quadro, ate o escore. `obs` (pode ser
 * NULL) recebe cada saida aproveitada em float. `ciclos` (pode ser NULL) mede o
 * total, o FIR e a cadeia; `custo` e o de ler o contador, descontado. Devolve 0, ou
 * -3 se o mfcc_estagios_init() nao rodou. */
int decimacao_executar(decimacao_observar_fn obs, void *ctx, cadeia_ciclos_fn ciclos,
                       uint32_t custo, decimacao_medida_t *m);

/* Decima de novo, sem a cadeia e sem medir, e compara cada saida com `ref` (antes e
 * depois de requantizar). `extra` (pode ser NULL) tambem recebe cada saida. */
void decimacao_conferir(const int16_t *ref, decimacao_observar_fn extra, void *ctx,
                        decimacao_comparacao_t *c);

/* Aplica os criterios a uma medida e uma comparacao ja preenchidas (separado para o
 * teste no PC mostrar que reprova). Preenche iguais e ok_saida e devolve ok_saida. */
int decimacao_avaliar(const decimacao_medida_t *m, decimacao_comparacao_t *c);

/* CRC-32 (zlib) das amostras int16 em little-endian, como o Python calcula com
 * zlib.crc32(np.asarray(x, '<i2').tobytes()). O _acumular continua um CRC
 * (comece com 0) com uma amostra. */
uint32_t decimacao_crc32(const int16_t *x, size_t n);
uint32_t decimacao_crc32_acumular(uint32_t crc, int16_t v);

/* A verificacao inteira, com a saida por `enviar`. Devolve 1 se tudo passou.
 * Guarda o custo de ler o contador e o resultado da passada medida (CRC e escore)
 * para o decimacao_inferir() do laco. */
int decimacao_verificar_com(cadeia_enviar_fn enviar, cadeia_ciclos_fn ciclos, uint32_t clock_hz);

/* Uma inferencia de ponta a ponta no laco. */
typedef struct {
    decimacao_medida_t m;   /* a passada: contagens, CRC, escore e ciclos */
    cadeia_resultado_t r;   /* o escore contra RECORTE_ESCORE_REF_INT16 (normal) */
    uint8_t igual_boot;     /* mesmo CRC e mesmo escore, bit a bit, da passada do boot */
    uint8_t ok;             /* contagens certas, escore e decisao do A4 e igual_boot */
} decimacao_inferencia_t;

/* O recorte de novo pelo decimacao_executar(), do int16 a 51,2 kHz ao escore, com o
 * custo do contador guardado pela verificacao. Sem a verificacao antes,
 * igual_boot e ok ficam 0. Devolve 0, ou o codigo do decimacao_executar(). */
int decimacao_inferir(cadeia_ciclos_fn ciclos, decimacao_inferencia_t *inf);

/* Pacote $SEG de origem D: clipe e segmento do clipe normal, ciclos do int16 a
 * 51,2 kHz ao escore, sem detector de novidade (codigo 0). */
void decimacao_pacote(const decimacao_inferencia_t *inf, uint32_t n, pacote_segmento_t *p);

/* Linha `ponta ...` de uma inferencia. Devolve o tamanho, ou -1 se nao coube. */
int decimacao_linha_ponta(char *buf, size_t n, const decimacao_inferencia_t *inf, uint32_t clock_hz);

/* ---- com HAL (so na placa) ---------------------------------------------- */
#ifndef DECIMACAO_HOST
/* Liga o DWT e roda o decimacao_verificar_com() pela USART1. Duas decimacoes do
 * recorte (a medida, com a cadeia, e a de conferencia) e a serial: a duracao sai na
 * ultima linha. */
int decimacao_verificar(void);

/* Um passo do laco: se `ok`, a inferencia de ponta a ponta, a linha ponta, o $SEG
 * de origem D e o LED da decisao (2 s). Se nao, nao faz nada: o cadeia_passo() ja
 * pisca e avisa o erro. */
void decimacao_passo(int ok);
#endif

#endif /* DECIMACAO_H */
