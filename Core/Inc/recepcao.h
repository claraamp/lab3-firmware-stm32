/*
 * recepcao.h -- recepcao de segmentos pela serial: o parser do quadro, em fluxo
 *
 * Decisao de 29/10 no Registro de Decisoes ("Recepcao de segmentos pela serial:
 * formato do quadro, origem S e modo de recepcao"), revisada pela Amanda e pela
 * Julia. O formato esta no README, secao "Recepcao de segmentos pela serial".
 *
 * Quadro do PC para a placa, 25.618 B, tudo little-endian:
 *
 *   offset  bytes   campo
 *        0      4   magica "QSEG"
 *        4      1   versao do quadro (1)
 *        5      1   formato (1 = int16 a 12,8 kHz; 2 reservado para 51,2 kHz)
 *        6      2   grupo (uint16): vai no campo clipe do $SEG
 *        8      4   id (uint32, ate 2^31 - 1): vai no campo segmento do $SEG
 *       12      2   n_amostras (uint16): 12800
 *       14      4   CRC-32 do zlib sobre os bytes 4..13 e o corpo
 *       18  25600   corpo: 12.800 amostras int16
 *
 * O parser recebe os bytes como chegam, em pedacos de qualquer tamanho (na placa,
 * o que o DMA circular escreveu no anel desde a ultima leitura), e NAO guarda o
 * segmento: cada amostra vai para um quadro de 320 do MFCC e para o CRC, como no
 * decimacao_executar(), com as mesmas chamadas e na mesma ordem do
 * mfcc_segmento(). As 26 caracteristicas saem bit a bit iguais as do segundo
 * inteiro (tests/host/test_recepcao.c confere). A decisao so sai depois do CRC: um
 * quadro com CRC errado nunca vira $SEG.
 *
 * O cabecalho e conferido assim que os 18 bytes chegam: magica, versao, formato e
 * n_amostras errados viram erro na hora, sem esperar o corpo. A partir do erro, o
 * parser so conta os bytes descartados e espera a linha ficar RECEPCAO_SILENCIO_MS
 * sem bytes (recepcao_tempo devolve RECEPCAO_SILENCIO); so entao a placa manda o
 * $ERR e o $PED seguinte. Um quadro que para no meio por RECEPCAO_SILENCIO_MS e
 * "truncado".
 *
 * O tempo vem de fora (agora_ms: HAL_GetTick() na placa, um relogio de mentira no
 * teste), entao o timeout tambem e testado no PC. As comparacoes de tempo sao por
 * subtracao em uint32_t: funcionam na volta do contador (49 dias).
 *
 * Nao usa a HAL: compila no PC (tests/host/test_recepcao.c) e na placa. Usa os
 * buffers estaticos do mfcc_estagios.c: nao e reentrante, como a cadeia.
 */

#ifndef RECEPCAO_H
#define RECEPCAO_H

#include <stddef.h>
#include <stdint.h>

#include "mfcc_estagios.h"

#define RECEPCAO_TAM_CABECALHO 18u
#define RECEPCAO_N_AMOSTRAS 12800u
#define RECEPCAO_TAM_QUADRO (RECEPCAO_TAM_CABECALHO + 2u * RECEPCAO_N_AMOSTRAS) /* 25.618 */
#define RECEPCAO_VERSAO 1u
#define RECEPCAO_FORMATO_12K8 1u
/* Sem byte por este tempo no meio de um quadro: truncado. Depois de um erro: a
 * linha ficou em silencio e a placa pode pedir o proximo. */
#define RECEPCAO_SILENCIO_MS 200u

typedef enum {
    RECEPCAO_ESPERANDO = 0, /* nenhum byte do quadro ainda */
    RECEPCAO_EM_ANDAMENTO,  /* chegou parte do quadro */
    RECEPCAO_COMPLETO,      /* quadro inteiro e CRC certo: caracteristicas prontas */
    RECEPCAO_ERRO,          /* erro: descartando ate a linha ficar em silencio */
    RECEPCAO_SILENCIO,      /* depois do erro, RECEPCAO_SILENCIO_MS sem bytes */
} recepcao_estado_t;

/* Motivo do erro: o texto e o do campo motivo do $ERR (recepcao_motivo_texto). */
typedef enum {
    RECEPCAO_OK = 0,
    RECEPCAO_E_MAGICA,
    RECEPCAO_E_VERSAO,
    RECEPCAO_E_FORMATO,
    RECEPCAO_E_TAMANHO,
    RECEPCAO_E_CRC,
    RECEPCAO_E_TRUNCADO,
} recepcao_motivo_t;

typedef uint32_t (*recepcao_ciclos_fn)(void);

typedef struct {
    recepcao_estado_t estado;
    recepcao_motivo_t motivo;
    /* cabecalho: os bytes e o que foi lido deles (grupo e id so valem se tem_id) */
    uint8_t cab[RECEPCAO_TAM_CABECALHO];
    uint8_t tem_id;         /* a magica chegou certa: grupo e id vao no $ERR */
    uint16_t grupo;
    uint32_t id;
    uint32_t crc_lido;
    uint32_t crc;           /* CRC acumulado dos bytes 4..13 e do corpo */
    /* contagens */
    uint32_t n_bytes;       /* bytes do quadro recebidos (ate RECEPCAO_TAM_QUADRO) */
    uint32_t n_descartados; /* bytes depois de um erro */
    uint32_t n_a_mais;      /* bytes que chegaram junto depois do quadro completo */
    uint32_t ultimo_ms;     /* quando chegou o ultimo byte */
    uint32_t inicio_ms;     /* quando chegou o primeiro byte do quadro */
    uint32_t fim_ms;        /* quando o quadro completou (ou deu erro) */
    /* o corpo, amostra a amostra */
    uint8_t byte_baixo;     /* primeiro byte de uma amostra, esperando o segundo */
    uint32_t n_amostras;    /* amostras completas do corpo */
    int16_t quadro[MFCC_N_QUADRO];
    uint32_t n_pcm;         /* amostras no quadro do MFCC */
    uint32_t n_quadros;
    mfcc_resumo_t resumo;
    int st;                 /* 0, ou o erro do mfcc_quadro_coefs (-3 sem o init) */
    /* ciclos so do processamento (quadros do MFCC e resumo), sem a espera */
    recepcao_ciclos_fn ler_ciclos;
    uint32_t ciclos;
} recepcao_t;

/* Comeca um quadro novo. `ler_ciclos` pode ser NULL (ciclos ficam 0). */
void recepcao_iniciar(recepcao_t *r, recepcao_ciclos_fn ler_ciclos, uint32_t agora_ms);

/* Entrega `n` bytes que chegaram (n pode ser 0). Devolve o estado depois deles.
 * Em RECEPCAO_COMPLETO, bytes a mais so sao contados (n_a_mais); em
 * RECEPCAO_ERRO, sao descartados e adiam o silencio. */
recepcao_estado_t recepcao_receber(recepcao_t *r, const uint8_t *dados, size_t n,
                                   uint32_t agora_ms);

/* Chamar periodicamente (no laco): no meio de um quadro, RECEPCAO_SILENCIO_MS sem
 * bytes viram erro "truncado"; em erro, o mesmo silencio vira RECEPCAO_SILENCIO.
 * Nos outros estados, nao muda nada. Devolve o estado. */
recepcao_estado_t recepcao_tempo(recepcao_t *r, uint32_t agora_ms);

/* As 26 caracteristicas (13 medias e 13 desvios, na ordem do modelo), so em
 * RECEPCAO_COMPLETO. Devolve 0, ou -1 se o quadro nao esta completo, ou o erro do
 * mfcc (r->st). Os ciclos do resumo entram em r->ciclos. */
int recepcao_caracteristicas(recepcao_t *r, float caracteristicas[2 * MFCC_N_COEFS]);

/* "magica", "versao", "formato", "tamanho", "crc", "truncado" (ou "" para OK). */
const char *recepcao_motivo_texto(recepcao_motivo_t m);

#endif /* RECEPCAO_H */
