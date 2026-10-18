/*
 * pacote_serial.h -- formato v1 do pacote que a placa manda pela serial
 *
 * Uma linha de texto por segmento de 1 s, no estilo das sentencas NMEA: comeca
 * com '$', os campos sao separados por virgula e a linha termina com '*' mais o
 * checksum (XOR dos bytes entre '$' e '*', em 2 digitos hexadecimais) e "\r\n".
 * Assim a linha e legivel no picocom e o painel descarta a que chegou corrompida.
 * As outras linhas da serial (hello, fft512, marco13, seg ..., demo ...) nao
 * comecam com '$', e o painel as ignora.
 *
 *   $CAB,1,exp239,ee0c3e088366,26,100*3B
 *   $SEG,1,7,R,2,1043,F,1,231.475,0,,,412*69
 *
 * Os campos, a ordem e os sentidos das comparacoes estao na secao "Pacote da
 * serial" do README e vem da decisao "O que o painel mostra no lugar de grau de
 * confianca" (Registro de Decisoes, 17/10):
 *   - LDA: falha se escore >= 0 (o mesmo lda_e_falha() do lda_modelo.h);
 *   - novidade: falha se pontuacao > limiar (estritamente maior, como no script);
 *   - pente (D1/D1b): vai a serie (BPFO ou BPFI) com a maior pontuacao - limiar.
 * A razao pontuacao / limiar e a consistencia entre segmentos NAO vao no pacote.
 *
 * Os campos de novidade estao reservados: enquanto nenhum detector for adotado,
 * o codigo e 0 e a pontuacao e o limiar vao vazios. Adotar um detector nao muda o
 * formato; mudar o formato exige subir PACOTE_VERSAO.
 *
 * Nao usa a HAL: compila no PC (tests/host/test_pacote_serial.c) e na placa.
 */

#ifndef PACOTE_SERIAL_H
#define PACOTE_SERIAL_H

#include <stddef.h>
#include <stdint.h>

#define PACOTE_VERSAO 1

/* Maior linha possivel com os limites de cada campo, com "\r\n" e o '\0'.
 * Um buffer deste tamanho nunca corta a linha. */
#define PACOTE_TAM_MAX 128

/* De onde veio o audio do segmento. */
typedef enum {
    PACOTE_ORIGEM_REFERENCIA = 'R', /* caracteristicas prontas do Python (marco de 13/10) */
    PACOTE_ORIGEM_CLIPE = 'C',      /* clipe gravado na flash (clipes_demo.h) */
    PACOTE_ORIGEM_AO_VIVO = 'A',    /* microfone; reservado, ainda nao existe */
} pacote_origem_t;

/* Qual detector disparou: mascara de bits (0 = nenhum, 3 = ambos). */
enum {
    PACOTE_DISPAROU_LDA = 1u,
    PACOTE_DISPAROU_NOVIDADE = 2u,
};

/* Detector de novidade cuja pontuacao vai no pacote. 0 = nao adotado (tambem no
 * D4, que nao tem detector separado). No pente, o codigo diz qual serie foi. */
typedef enum {
    PACOTE_NOV_NAO_ADOTADO = 0,
    PACOTE_NOV_PENTE_BPFO = 1,  /* D1/D1b, serie BPFO; pontuacao e limiar em dB */
    PACOTE_NOV_PENTE_BPFI = 2,  /* D1/D1b, serie BPFI; pontuacao e limiar em dB */
    PACOTE_NOV_MAHAL_MFCC = 3,  /* D2: distancia de Mahalanobis^2 no MFCC */
    PACOTE_NOV_MAHAL_MFCC_PENTE = 4, /* D3: Mahalanobis^2 no MFCC + pente */
} pacote_novidade_t;

/* Um segmento. Preencha origem, clipe, segmento, ciclos e chame
 * pacote_decidir(), que preenche o resto. */
typedef struct {
    uint32_t n;             /* contador desde o boot; o painel ve pacotes perdidos */
    char origem;            /* pacote_origem_t */
    int32_t clipe;          /* R: gravacao; C: clipe; A: -1 (sai vazio) */
    int32_t segmento;       /* R: id do segmento no conjunto; C: segmento no clipe */
    uint8_t falha;          /* decisao: 1 = falha, 0 = normal */
    uint8_t disparou;       /* mascara PACOTE_DISPAROU_* */
    float escore_lda;       /* cru, nunca em %; falha se >= 0 */
    uint8_t cod_novidade;   /* pacote_novidade_t */
    float pont_novidade;    /* ignorada se cod_novidade == 0 */
    float lim_novidade;     /* ignorado se cod_novidade == 0 */
    uint32_t ciclos;        /* DWT do processamento do segmento (ver README) */
} pacote_segmento_t;

/* As duas regras de decisao, com os sentidos da decisao de 17/10. */
int pacote_lda_e_falha(float escore);                     /* escore >= 0 */
int pacote_novidade_dispara(float pontuacao, float limiar); /* pontuacao > limiar */

/* Preenche escore, codigo, pontuacao, limiar, disparou e falha. A decisao e
 * "LDA OU novidade"; com cod_novidade == 0 so a LDA conta. */
void pacote_decidir(pacote_segmento_t *p, float escore_lda, uint8_t cod_novidade,
                    float pont_novidade, float lim_novidade);

/* Pente (D1/D1b): escolhe a serie com a maior pontuacao - limiar (em dB) e
 * devolve o codigo dela (BPFO ou BPFI), com a pontuacao e o limiar escolhidos.
 * Empate: BPFO. Reservado ate o pente ser adotado. */
uint8_t pacote_escolher_pente(float pont_bpfo, float lim_bpfo, float pont_bpfi,
                              float lim_bpfi, float *pontuacao, float *limiar);

/* Checksum do formato: XOR dos bytes de s[0..len-1] (sem o '$' e o '*'). */
uint8_t pacote_checksum(const char *s, size_t len);

/* Linha $SEG do segmento, com "*HH\r\n". Devolve o tamanho escrito, ou -1 se o
 * buffer for pequeno (use PACOTE_TAM_MAX). */
int pacote_linha(char *buf, size_t n, const pacote_segmento_t *p);

/* Linha $CAB, mandada uma vez ao ligar: versao, modelo, SHA-256 do JSON (12
 * digitos), numero de caracteristicas e clock em MHz. O painel confere o modelo
 * antes de mostrar os segmentos. Devolve o tamanho, ou -1. */
int pacote_cabecalho(char *buf, size_t n, const char *modelo_id, const char *json_sha12,
                     unsigned n_features, unsigned clock_mhz);

#endif /* PACOTE_SERIAL_H */
