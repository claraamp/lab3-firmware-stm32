/*
 * recepcao_serial.c -- o modo de recepcao de segmentos pela serial (ver
 * recepcao_serial.h e recepcao.h)
 *
 * Compilado com -O2 inclusive no Debug (CMakeLists.txt da raiz): o lda_escore() e
 * inline no lda_modelo.h e tem que sair com a mesma conta do cadeia_clipes.c, para
 * o escore de um segmento mandado pelo PC poder ser comparado bit a bit com o do
 * clipe da flash.
 */

#include "recepcao_serial.h"

#include <stdio.h>
#include <string.h>

#include "main.h"
#include "usart.h"

#include "cadeia_clipes.h"
#include "clipes_demo.h"
#include "lda_modelo.h"
#include "pacote_serial.h"
#include "recepcao.h"
#include "texto_fixo.h"

/* O anel do DMA: 2 KB no modo de recepcao; os 64 primeiros bytes servem a escuta
 * do RECEBER durante a demonstracao. Um anel so, armado na posicao 0 antes de cada
 * $PED, recebe o quadro inteiro (cabecalho e corpo) sem rearme no meio. */
#define ANEL_RX 2048u
#define ANEL_ESCUTA 64u
static uint8_t anel[ANEL_RX];
static uint32_t tam_anel = 0u;   /* tamanho armado no DMA */
static uint32_t pos_lida = 0u;   /* proximo byte a ler do anel */

/* A HAL aborta a recepcao por DMA em qualquer erro de linha e chama o callback. */
static volatile uint8_t erro_uart = 0u;
static uint32_t n_erros_uart = 0u;

static recepcao_t rx;            /* estatico, e nao na pilha (856 B) */
static char linha[192];
_Static_assert(sizeof linha >= PACOTE_TAM_MAX, "o $SEG tem que caber no buffer");

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    if (huart == &huart1) {
        erro_uart = 1u;
    }
}

static void enviar(const char *texto, size_t n)
{
    HAL_UART_Transmit(&huart1, (uint8_t *)texto, (uint16_t)n, HAL_MAX_DELAY);
}

static void enviar_linha(int len)
{
    if (len > 0 && (size_t)len < sizeof linha) {
        enviar(linha, (size_t)len);
    }
}

static uint32_t ler_dwt(void)
{
    return DWT->CYCCNT;
}

static void leds(int verde, int vermelho)
{
    HAL_GPIO_WritePin(LED_G_GPIO_Port, LED_G_Pin, verde ? GPIO_PIN_SET : GPIO_PIN_RESET);
    HAL_GPIO_WritePin(LED_R_GPIO_Port, LED_R_Pin, vermelho ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

/* Para o DMA, limpa os erros pendentes da UART (le SR e DR) e arma o anel de novo
 * na posicao 0, com `n` bytes. */
static void armar(uint32_t n)
{
    HAL_UART_AbortReceive(&huart1);
    __HAL_UART_CLEAR_OREFLAG(&huart1);
    erro_uart = 0u;
    tam_anel = n;
    pos_lida = 0u;
    if (HAL_UART_Receive_DMA(&huart1, anel, (uint16_t)n) != HAL_OK) {
        erro_uart = 1u;   /* tenta de novo na proxima volta do laco */
    }
}

/* Onde o DMA vai escrever o proximo byte (o NDTR conta o que falta ate a volta). */
static uint32_t pos_dma(void)
{
    const uint32_t p = tam_anel - (uint32_t)__HAL_DMA_GET_COUNTER(huart1.hdmarx);
    return p >= tam_anel ? 0u : p;
}

/* ------------------------------------------------------------------------- */
/* Escuta do RECEBER durante a demonstracao                                    */
/* ------------------------------------------------------------------------- */
static char comando[16];
static uint32_t n_comando = 0u;

void recepcao_serial_escutar(void)
{
    n_comando = 0u;
    armar(ANEL_ESCUTA);
}

int recepcao_serial_pedido(void)
{
    if (erro_uart) {
        ++n_erros_uart;
        recepcao_serial_escutar();
        return 0;
    }
    int achou = 0;
    const uint32_t fim = pos_dma();
    while (pos_lida != fim) {
        const char c = (char)anel[pos_lida];
        pos_lida = (pos_lida + 1u) % tam_anel;
        if (c == '\n' || c == '\r') {
            if (n_comando == 7u && memcmp(comando, "RECEBER", 7) == 0) {
                achou = 1;
            }
            n_comando = 0u;
        } else if (n_comando < sizeof comando) {
            comando[n_comando++] = c;   /* linha longa demais: nunca bate */
        }
    }
    return achou;
}

/* ------------------------------------------------------------------------- */
/* O modo de recepcao                                                          */
/* ------------------------------------------------------------------------- */

/* Uma linha "ref" por segmento dos clipes: o escore que a cadeia do firmware
 * (mfcc_segmento + lda_escore, a mesma conta da demonstracao) da para o clipe da
 * flash, em hexadecimal. O escore de um segmento mandado pelo PC tem que bater
 * bit a bit com o do mesmo segmento aqui. */
static void enviar_referencias(void)
{
    for (uint16_t s = 0; s < CLIPES_N_SEGMENTOS; ++s) {
        cadeia_resultado_t r;
        if (cadeia_inferir(s, ler_dwt, 0u, &r) != 0) {
            continue;
        }
        const clipe_segmento_t *seg = &clipes_segmentos[s];
        const clipe_t *c = &clipes[seg->clipe];
        uint32_t bits;
        memcpy(&bits, &r.escore, sizeof bits);
        char escore[24];
        texto_fixo(escore, sizeof escore, r.escore, 3u);
        enviar_linha(snprintf(linha, sizeof linha, "ref seg=%u clipe=%s k=%u escore=%s escore_hex=0x%08lX\r\n",
                              (unsigned)seg->id, c->classe, (unsigned)(s - c->primeiro_segmento), escore,
                              (unsigned long)bits));
    }
}

/* Entrega ao parser os bytes que o DMA escreveu desde a ultima leitura (em dois
 * pedacos quando o anel deu a volta) e devolve o estado. */
static recepcao_estado_t consumir(void)
{
    const uint32_t fim = pos_dma();
    recepcao_estado_t e = rx.estado;
    if (fim != pos_lida) {
        const uint32_t agora = HAL_GetTick();
        if (fim > pos_lida) {
            e = recepcao_receber(&rx, &anel[pos_lida], fim - pos_lida, agora);
        } else {
            e = recepcao_receber(&rx, &anel[pos_lida], tam_anel - pos_lida, agora);
            e = recepcao_receber(&rx, anel, fim, agora);
        }
        pos_lida = fim;
    }
    return e;
}

/* Um quadro completo e com o CRC certo: caracteristicas, LDA, linha rx e $SEG. */
static void responder_segmento(void)
{
    float x[2 * MFCC_N_COEFS];
    const int st = recepcao_caracteristicas(&rx, x);
    const uint32_t t0 = ler_dwt();
    const float escore = st == 0 ? lda_escore(x) : __builtin_nanf("");   /* sem cadeia: erro */
    const uint32_t ciclos = rx.ciclos + (ler_dwt() - t0);

    pacote_segmento_t p;
    memset(&p, 0, sizeof p);
    p.n = cadeia_proximo_pacote();
    p.origem = (char)PACOTE_ORIGEM_SERIAL;
    p.clipe = (int32_t)rx.grupo;
    p.segmento = (int32_t)rx.id;
    p.ciclos = ciclos;
    pacote_decidir(&p, escore, PACOTE_NOV_NAO_ADOTADO, 0.0f, 0.0f);

    uint32_t bits;
    memcpy(&bits, &escore, sizeof bits);
    enviar_linha(snprintf(linha, sizeof linha,
                          "rx grupo=%u id=%lu bytes=%lu ms=%lu ignorados=%lu a_mais=%lu erros_uart=%lu "
                          "ciclos=%lu escore_hex=0x%08lX\r\n",
                          (unsigned)rx.grupo, (unsigned long)rx.id, (unsigned long)rx.n_bytes,
                          (unsigned long)(rx.fim_ms - rx.inicio_ms), (unsigned long)rx.n_ignorados,
                          (unsigned long)rx.n_a_mais, (unsigned long)n_erros_uart, (unsigned long)ciclos,
                          (unsigned long)bits));
    enviar_linha(pacote_linha(linha, sizeof linha, &p));
    if (p.disparou & PACOTE_ERRO) {
        leds(1, 1);   /* a conta deu errado: os dois, nunca o verde sozinho */
    } else {
        leds(!p.falha, p.falha);
    }
}

/* Erro de recepcao, ja com a linha em silencio: linha rx e $ERR. */
static void responder_erro(void)
{
    char grupo[8] = "-", id[12] = "-";
    if (rx.tem_id) {
        snprintf(grupo, sizeof grupo, "%u", (unsigned)rx.grupo);
        snprintf(id, sizeof id, "%lu", (unsigned long)rx.id);
    }
    enviar_linha(snprintf(linha, sizeof linha,
                          "rx erro=%s grupo=%s id=%s bytes=%lu descartados=%lu ignorados=%lu erros_uart=%lu\r\n",
                          recepcao_motivo_texto(rx.motivo), grupo, id, (unsigned long)rx.n_bytes,
                          (unsigned long)rx.n_descartados, (unsigned long)rx.n_ignorados,
                          (unsigned long)n_erros_uart));
    enviar_linha(pacote_erro_recepcao(linha, sizeof linha, rx.tem_id, rx.grupo, rx.id,
                                      recepcao_motivo_texto(rx.motivo)));
    leds(0, 0);
}

void recepcao_serial_executar(int ok)
{
    if (!ok) {
        static const char recusa[] =
            "recepcao recusada: as verificacoes do boot falharam (ver as linhas FALHOU acima)\r\n";
        enviar(recusa, sizeof recusa - 1u);
        return;
    }
    HAL_UART_AbortReceive(&huart1);   /* o resto do que chegou na escuta e descartado */
    leds(0, 0);
    enviar_linha(snprintf(linha, sizeof linha,
                          "recepcao inicio modelo=%s json=%s quadro=%u anel=%u baud=%lu silencio_ms=%u\r\n",
                          CLIPES_MODELO_ID, CLIPES_JSON_SHA256_12, (unsigned)RECEPCAO_TAM_QUADRO,
                          (unsigned)ANEL_RX, (unsigned long)huart1.Init.BaudRate,
                          (unsigned)RECEPCAO_SILENCIO_MS));
    enviar_referencias();
    enviar_linha(pacote_cabecalho(linha, sizeof linha, CLIPES_MODELO_ID, CLIPES_JSON_SHA256_12,
                                  (unsigned)LDA_N_FEATURES, (unsigned)(SystemCoreClock / 1000000u)));

    uint32_t pedidos = 0u;
    for (;;) {
        armar(ANEL_RX);
        recepcao_iniciar(&rx, ler_dwt, HAL_GetTick());
        enviar_linha(pacote_pedido(linha, sizeof linha, ++pedidos));

        recepcao_estado_t e;
        for (;;) {
            if (erro_uart) {
                /* a HAL abortou o DMA: arma de novo sem zerar o parser; os bytes
                 * perdidos no meio viram crc ou truncado */
                ++n_erros_uart;
                armar(ANEL_RX);
            }
            e = consumir();
            if (e != RECEPCAO_COMPLETO) {
                e = recepcao_tempo(&rx, HAL_GetTick());
            }
            if (e == RECEPCAO_COMPLETO || e == RECEPCAO_SILENCIO) {
                break;
            }
        }
        HAL_UART_AbortReceive(&huart1);   /* nada chega entre a resposta e o $PED */

        if (e == RECEPCAO_COMPLETO) {
            responder_segmento();
        } else {
            responder_erro();
        }
    }
}
