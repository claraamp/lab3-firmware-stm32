/*
 * cadeia_clipes.h -- cadeia completa na placa com os clipes da flash
 *
 * Card "Portar a cadeia de DSP para C -- integracao da cadeia completa na placa".
 * E o laco do firmware normal (sem opcao de CMake):
 *
 *     clipes[c].pcm -> mfcc_segmento() -> lda_escore() -> decisao -> LEDs e $SEG (origem C)
 *
 * Ao ligar, depois do marco de 13/10, o cadeia_verificar() roda a cadeia nos 10
 * segmentos dos clipes do clipes_demo.h e confere cada escore com o do
 * escores_referencia.csv (clipes_segmentos[].escore_ref), pela tolerancia
 * ENCADEADA do escore no A4 (rel 1e-5). Para cada segmento, manda uma linha:
 *
 *   cadeia seg=29 clipe=normal k=0 ref=-1331.768 placa=-1331.768 dif=0.000100 tol=0.013318
 *          dec=normal OK ciclos=6145316 blocos=janela:..,espectro:..,mel:..,log:..,dct:..,
 *          resumo:..,final:..,lda:.. soma=.. iguais=1
 *
 * (numa linha so), e no fim um resumo, a mediana de cada bloco e CADEIA OK ou
 * CADEIA FALHOU. Os `ciclos` sao os do mfcc_segmento() mais o lda_escore(), do
 * int16 ao escore, medidos pelo DWT. Os `blocos` vem de uma segunda passada no
 * mesmo segmento, com os estagios chamados um a um e cada um medido em cada um dos
 * 98 quadros (somados no segmento): janela = escala e Hann (mfcc_quadro_pcm),
 * espectro = FFT e potencia, mel, log, dct, resumo = Welford quadro a quadro,
 * final = media e desvio no fim do segmento, lda = o escore. `soma` e a soma dos
 * blocos (fica ~0,5 % abaixo de `ciclos`: as medidas nao incluem o laco dos
 * quadros nem as chamadas entre os estagios); `iguais=1` diz que essa passada deu
 * bit a bit as mesmas 26
 * caracteristicas que o mfcc_segmento().
 *
 * Em laco, o cadeia_passo() mostra os 10 segmentos em ordem, 2 s cada: roda a
 * cadeia de novo (e uma inferencia de verdade a cada vez, nao um valor guardado),
 * acende o LED da decisao (verde = normal, vermelho = falha) e manda uma linha
 * `demo ...` e o pacote $SEG com origem C, clipe = indice do clipe e segmento =
 * indice do segmento dentro do clipe. Os ciclos do pacote vao do int16 ao escore.
 *
 * Desde 22/10 (primeira inferencia de ponta a ponta), o laco intercala o caminho
 * decimado: antes de cada volta dos 10 segmentos, o decimacao_passo() roda o
 * recorte de 51,2 kHz pela decimacao e pela cadeia (origem D). Os dois usam o mesmo
 * contador n dos pacotes (cadeia_proximo_pacote) e os mesmos LEDs (cadeia_mostrar),
 * para o painel ver uma sequencia so, sem salto.
 *
 * TODOS OS SEGMENTOS DOS CLIPES ESTAO NO TREINO DO MODELO FINAL: a demonstracao
 * mostra a fidelidade do porte (a placa reproduz o Python), nao o desempenho.
 *
 * A parte sem HAL compila no PC com -DCADEIA_HOST (tests/host/test_cadeia_clipes.c
 * roda a mesma verificacao com a CMSIS-DSP generica).
 */

#ifndef CADEIA_CLIPES_H
#define CADEIA_CLIPES_H

#include <stddef.h>
#include <stdint.h>

#include "pacote_serial.h"

/* Tolerancia do escore no modo encadeado (docs/contrato_numerico.md do Python,
 * "Tolerancias por estagio (A4)"; e o 1e-5 do tolerancias.txt para o escore
 * encadeado): |placa - ref| <= 1e-5 * |ref|. Na placa (19/10, validacao_placa),
 * o pior segmento usou 5,8 % dela. */
#define CADEIA_TOL_ESCORE_REL 1e-5f

enum {
    CADEIA_B_JANELA,
    CADEIA_B_ESPECTRO,
    CADEIA_B_MEL,
    CADEIA_B_LOG,
    CADEIA_B_DCT,
    CADEIA_B_RESUMO,
    CADEIA_B_FINAL,
    CADEIA_B_LDA,
    CADEIA_N_BLOCOS
};

typedef void (*cadeia_enviar_fn)(const char *texto, size_t n);
typedef uint32_t (*cadeia_ciclos_fn)(void);

typedef struct {
    float escore;      /* escore calculado na placa (mfcc_segmento + lda_escore) */
    float ref;         /* escores_referencia.csv */
    float dif;         /* |escore - ref| */
    float tol;         /* CADEIA_TOL_ESCORE_REL * |ref| */
    uint8_t falha;     /* decisao da placa: 1 = falha */
    uint8_t ok_escore; /* dif <= tol */
    uint8_t ok_decisao;/* decisao igual a do CSV */
    uint8_t iguais;    /* passada bloco a bloco deu as mesmas 26 caracteristicas */
    uint32_t ciclos;   /* int16 -> escore, uma chamada */
    uint32_t blocos[CADEIA_N_BLOCOS]; /* soma dos 98 quadros de cada bloco (0 no demo) */
} cadeia_resultado_t;

/* ---- sem HAL ------------------------------------------------------------ */

uint16_t cadeia_n_segmentos(void);
const char *cadeia_nome_bloco(int b);

/* A comparacao com a referencia, separada para ser testada com uma referencia que
 * tem que reprovar. Preenche escore, ref, dif, tol, falha, ok_escore e ok_decisao. */
void cadeia_comparar(float escore, float ref, uint8_t falha_ref, cadeia_resultado_t *r);

/* Um segmento s (0 a cadeia_n_segmentos() - 1) pelo caminho do firmware:
 * mfcc_segmento() e lda_escore(), medidos juntos. `custo` e o custo de ler o
 * contador, descontado. Devolve 0, ou o codigo do mfcc_segmento() (-3 sem init). */
int cadeia_inferir(uint16_t s, cadeia_ciclos_fn ciclos, uint32_t custo, cadeia_resultado_t *r);

/* cadeia_inferir() e mais a passada bloco a bloco (blocos[] e iguais). */
int cadeia_avaliar(uint16_t s, cadeia_ciclos_fn ciclos, uint32_t custo, cadeia_resultado_t *r);

/* Linha de texto de um segmento ("cadeia" na verificacao, "demo" no laco; com
 * blocos[] zerados, a linha sai sem os blocos). Devolve o tamanho, ou -1 se nao
 * coube. */
int cadeia_linha(char *buf, size_t n, const char *prefixo, uint16_t s,
                 const cadeia_resultado_t *r);

/* Pacote $SEG (formato v1) do segmento s: origem C, clipe = indice do clipe,
 * segmento = indice dentro do clipe, sem detector de novidade (codigo 0). */
void cadeia_pacote(uint16_t s, const cadeia_resultado_t *r, uint32_t n, pacote_segmento_t *p);

/* O proximo n dos pacotes $SEG do laco (0, 1, 2...), um contador so para as
 * origens C e D. */
uint32_t cadeia_proximo_pacote(void);

/* A verificacao inteira (init, 10 segmentos, resumo, CADEIA OK/FALHOU), com a
 * saida por `enviar`. Devolve 1 se os 10 passaram e 0 caso contrario (inclusive
 * se o init falhou). Guarda o custo de ler o contador para o cadeia_passo(). */
int cadeia_verificar_com(cadeia_enviar_fn enviar, cadeia_ciclos_fn ciclos, uint32_t clock_hz);

/* ---- com HAL (so na placa) ---------------------------------------------- */
#ifndef CADEIA_HOST
/* Liga o DWT e roda o cadeia_verificar_com() pela USART1. */
int cadeia_verificar(void);

/* Um passo do laco principal: se `ok`, o proximo segmento dos clipes nos LEDs
 * (2 s), com a linha demo e o $SEG; senao, os dois LEDs piscam juntos (erro).
 * Devolve 1 quando o segmento mostrado foi o ultimo da volta (a proxima chamada
 * recomeca do primeiro), e 0 nos outros casos, inclusive no erro. */
int cadeia_passo(int ok);

/* A decisao de um pacote nos LEDs: verde = normal, vermelho = falha, por 2 s, e
 * 300 ms apagados; com PACOTE_ERRO, os dois piscam juntos. */
void cadeia_mostrar(const pacote_segmento_t *p);

/* Os dois LEDs juntos, rapido, por 2 s: erro, nunca uma decisao. */
void cadeia_piscar_erro(void);
#endif

#endif /* CADEIA_CLIPES_H */
