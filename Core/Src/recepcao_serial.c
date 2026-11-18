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

/* Voltas do anel (decisao de 03/11, sugestao da Julia e da Amanda no PR #11). As
 * interrupcoes de meia transferencia e de transferencia completa do DMA contam as
 * metades do anel que o DMA ja encheu desde o armar(). Com elas e o NDTR, o laco
 * sabe quantos bytes o DMA escreveu (escrito_dma) e compara com quantos leu
 * (lidos): mais de um anel de diferenca e byte sobrescrito antes de ser lido, o
 * "anel estourado". So pelo NDTR, uma volta inteira perdida deixa a posicao igual
 * e passa despercebida; o quadro vira crc ou truncado sem o motivo aparecer.
 * A 921.600 bit/s, o anel de 2 KB da cerca de 22 ms de folga ao laco. */
static volatile uint32_t meias = 0u;   /* metades do anel cheias (nas interrupcoes) */
static uint32_t lidos = 0u;            /* bytes lidos do anel desde o armar() */
static uint32_t n_estouros = 0u;       /* deteccoes desde o boot, como o n_erros_uart */
static uint32_t estouros_no_pedido = 0u;

/* A HAL aborta a recepcao por DMA em qualquer erro de linha e chama o callback. */
static volatile uint8_t erro_uart = 0u;
static uint32_t n_erros_uart = 0u;

#ifdef FIRMWARE_MEDIDA
#include "medida.h"

/* Build de medida (-DMEDIDA=ON; card de tempo e memoria, 11/2026). Separa, em cada
 * pedido, o tempo da linha (os bytes chegando), o trabalho da CPU com eles e a
 * resposta, e manda tudo numa linha "med" depois do $SEG. O firmware normal nao
 * tem nada disto: as linhas rx e $SEG ficam iguais, e os scripts do PC ignoram a
 * med como qualquer linha que nao conhecem.
 *
 * Tudo pelo DWT (100 MHz; 1 ciclo = 10 ns), lido no laco do modo:
 * - espera: do fim do $PED ao primeiro byte do quadro (o PC lendo o $PED e
 *   mandando o quadro);
 * - linha: do primeiro ao ultimo byte do quadro, como o campo ms da rx. O DWT tem
 *   resolucao de 10 ns, mas o primeiro e o ultimo byte so sao vistos quando o laco
 *   le o anel, o que pode atrasar ate uma chamada ao consumir() (~0,6 ms);
 * - lacunas: a soma, entre leituras seguidas com bytes novos, do que passou do
 *   tempo de linha dos bytes que chegaram (n x 10 bits no baud da placa, 925.926
 *   bit/s; se o adaptador manda a 921.600, ~1,3 ms por segmento das lacunas sao
 *   essa diferenca de baud, e nao linha parada). E o tempo em
 *   que a linha ficou parada (o PC e o adaptador entre um pacote USB e outro);
 *   linha - lacunas e o tempo de linha "cheia";
 * - consumir: os ciclos das chamadas ao consumir() que acharam bytes, ou seja,
 *   todo o trabalho da CPU com o quadro: a copia do anel, o CRC, a montagem das
 *   amostras e os quadros do MFCC. O campo ciclos da rx tem so o MFCC, o resumo e
 *   a LDA; a diferenca (sem a LDA) e o custo da recepcao byte a byte;
 * - leituras: quantas chamadas ao consumir() acharam bytes. bytes / leituras e
 *   quantos bytes cada leitura pega: com a CPU livre, o laco le o anel a cada um
 *   ou dois bytes, e o custo fixo de cada chamada entra no consumir;
 * - consumir_max: a chamada mais longa ao consumir(), o maior tempo em que o laco
 *   ficou sem ler o anel. Tem que ficar bem abaixo dos ~22 ms em que o anel de
 *   2 KB enche a 921.600 bit/s;
 * - resposta: do quadro completo ao fim do envio do $SEG (LDA, formatacao e o
 *   HAL_UART_Transmit bloqueante da rx e do $SEG); sem a propria linha med;
 * - pilha: a marca d'agua da recepcao. A pilha e pintada de novo logo depois da
 *   linha "med inicio" (que traz a do boot, pilha_boot), entao cada med mostra so
 *   a recepcao, ate aquele segmento (medida.h). */
static struct {
    uint32_t t_ped;          /* DWT no fim do $PED */
    uint32_t t_primeiro;     /* DWT da primeira leitura com bytes */
    uint32_t t_ultimo;       /* DWT da ultima leitura com bytes */
    uint32_t espera;
    uint32_t lacunas;
    uint32_t lacuna_max;
    uint32_t consumir;
    uint32_t consumir_max;
    uint32_t leituras;       /* chamadas ao consumir() que acharam bytes */
    uint32_t ciclos_por_byte;   /* 10 bits no baud real (BRR), em ciclos */
    uint8_t teve_byte;
} med;

static void medida_pedido(uint32_t agora)
{
    const uint32_t ciclos_por_byte = med.ciclos_por_byte;
    memset(&med, 0, sizeof med);
    med.ciclos_por_byte = ciclos_por_byte;
    med.t_ped = agora;
}

/* Uma leitura que achou n bytes novos, no instante t (antes de entregar ao parser). */
static void medida_chegada(uint32_t t, uint32_t n)
{
    if (!med.teve_byte) {
        med.teve_byte = 1u;
        med.espera = t - med.t_ped;
        med.t_primeiro = t;
    } else {
        const uint32_t dt = t - med.t_ultimo;
        const uint32_t esperado = n * med.ciclos_por_byte;
        if (dt > esperado) {
            const uint32_t l = dt - esperado;
            med.lacunas += l;
            if (l > med.lacuna_max) {
                med.lacuna_max = l;
            }
        }
    }
    med.t_ultimo = t;
}

static uint32_t us(uint32_t ciclos)
{
    return ciclos / (SystemCoreClock / 1000000u);
}
#endif

static recepcao_t rx;            /* estatico, e nao na pilha (856 B) */
static char linha[256];   /* a linha rx com todos os campos no maximo passa de 192 */
_Static_assert(sizeof linha >= PACOTE_TAM_MAX, "o $SEG tem que caber no buffer");

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    if (huart == &huart1) {
        erro_uart = 1u;
    }
}

/* Meia transferencia (o DMA escreveu a ultima posicao da primeira metade) e
 * transferencia completa (a ultima do anel; no modo circular, o NDTR recarrega e o
 * DMA continua da posicao 0). */
void HAL_UART_RxHalfCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart == &huart1) {
        ++meias;
    }
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart == &huart1) {
        ++meias;
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
    meias = 0u;   /* o DMA esta parado: nenhuma interrupcao dele ate o Receive_DMA */
    lidos = 0u;
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

/* Bytes que o DMA escreveu desde o armar(): as voltas inteiras (meias / 2) vezes o
 * anel, mais a posicao. O contador e o NDTR sao lidos de novo se uma interrupcao
 * rodou entre as duas leituras. Se o DMA acabou de passar da metade ou da volta e
 * a interrupcao ainda nao rodou, a metade em que a posicao esta nao bate com a
 * paridade do contador: falta uma meia, que entra na conta. */
static uint32_t escrito_dma(void)
{
    uint32_t m, p;
    do {
        m = meias;
        p = pos_dma();
    } while (m != meias);
    const uint32_t na_segunda = p >= tam_anel / 2u ? 1u : 0u;
    if ((m & 1u) != na_segunda) {
        ++m;
    }
    return (m / 2u) * tam_anel + p;
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
 * pedacos quando o anel deu a volta) e devolve o estado. Com o anel estourado (o
 * DMA escreveu mais de um anel a frente da leitura), os bytes nao lidos ja foram
 * sobrescritos: nada e entregue, a leitura pula para onde o DMA esta, o estouro e
 * contado e o parser sabe da perda (recepcao_perder: o quadro vira truncado na hora,
 * mesmo se a magica se perdeu), e a linha rx mostra o motivo. Um byte sobrescrito
 * enquanto era lido tambem conta, mas so no contador: a conferencia e feita depois
 * da leitura e e conservadora, e o CRC do quadro decide. */
static recepcao_estado_t consumir(void)
{
    const uint32_t escrito = escrito_dma();
    const uint32_t n = escrito - lidos;
    recepcao_estado_t e = rx.estado;
    if (n > tam_anel) {
        ++n_estouros;
        e = recepcao_perder(&rx, n, HAL_GetTick());
        lidos = escrito;
        pos_lida = lidos % tam_anel;
        return e;
    }
    if (n > 0u) {
#ifdef FIRMWARE_MEDIDA
        const uint32_t t0 = ler_dwt();
        medida_chegada(t0, n);
#endif
        const uint32_t agora = HAL_GetTick();
        const uint32_t ate_o_fim = tam_anel - pos_lida;
        if (n <= ate_o_fim) {
            e = recepcao_receber(&rx, &anel[pos_lida], n, agora);
        } else {
            e = recepcao_receber(&rx, &anel[pos_lida], ate_o_fim, agora);
            e = recepcao_receber(&rx, anel, n - ate_o_fim, agora);
        }
        /* o mais antigo dos bytes lidos (o de numero `lidos`) e sobrescrito quando o
         * DMA escreve o de numero lidos + tam_anel */
        if (escrito_dma() - lidos > tam_anel) {
            ++n_estouros;
        }
        lidos += n;
        pos_lida = lidos % tam_anel;
#ifdef FIRMWARE_MEDIDA
        const uint32_t dc = ler_dwt() - t0;
        med.consumir += dc;
        ++med.leituras;
        if (dc > med.consumir_max) {
            med.consumir_max = dc;
        }
#endif
    }
    return e;
}

/* Voltas do anel e estouros do pedido atual, para as linhas rx. O estouros e um
 * alarme conservador: conta deteccoes (um estouro pode ser visto duas vezes, depois
 * da leitura e antes da seguinte), e a conferencia depois da leitura pode marcar um
 * quadro que passou no CRC. So o zero e garantia. Entram no fim das
 * linhas (regra 6 do README: campo novo da rx so no fim), e com estouro o texto
 * " anel=estourado" fecha a linha. */
static uint32_t voltas(void)
{
    return tam_anel ? lidos / tam_anel : 0u;
}

static uint32_t estouros_do_pedido(void)
{
    return n_estouros - estouros_no_pedido;
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
                          "ciclos=%lu escore_hex=0x%08lX voltas=%lu estouros=%lu%s\r\n",
                          (unsigned)rx.grupo, (unsigned long)rx.id, (unsigned long)rx.n_bytes,
                          (unsigned long)(rx.fim_ms - rx.inicio_ms), (unsigned long)rx.n_ignorados,
                          (unsigned long)rx.n_a_mais, (unsigned long)n_erros_uart, (unsigned long)ciclos,
                          (unsigned long)bits, (unsigned long)voltas(), (unsigned long)estouros_do_pedido(),
                          estouros_do_pedido() ? " anel=estourado" : ""));
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
                          "rx erro=%s grupo=%s id=%s bytes=%lu descartados=%lu ignorados=%lu erros_uart=%lu "
                          "voltas=%lu estouros=%lu%s\r\n",
                          recepcao_motivo_texto(rx.motivo), grupo, id, (unsigned long)rx.n_bytes,
                          (unsigned long)rx.n_descartados, (unsigned long)rx.n_ignorados,
                          (unsigned long)n_erros_uart, (unsigned long)voltas(), (unsigned long)estouros_do_pedido(),
                          estouros_do_pedido() ? " anel=estourado" : ""));
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
#ifdef FIRMWARE_MEDIDA
    {
        /* O baud real sai do BRR: com sobreamostragem de 16, baud = PCLK2 / BRR
         * (100 MHz / 0x6C = 925.926 bit/s para os 921.600 pedidos). Um byte na
         * linha sao 10 bits (8N1). A pilha aqui e a do boot e da demonstracao. */
        const uint32_t pclk2 = HAL_RCC_GetPCLK2Freq();
        const uint32_t brr = huart1.Instance->BRR;
        med.ciclos_por_byte = (uint32_t)(((uint64_t)SystemCoreClock * 10u * brr) / pclk2);
        enviar_linha(snprintf(linha, sizeof linha,
                              "med inicio baud_real=%lu ciclos_por_byte=%lu pilha_boot=%lu pilha_pintada=%u "
                              "pilha_reserva=%lu\r\n",
                              (unsigned long)(pclk2 / brr), (unsigned long)med.ciclos_por_byte,
                              (unsigned long)medida_pilha_usada(), (unsigned)MEDIDA_PILHA_PINTADA,
                              (unsigned long)medida_pilha_reserva()));
        /* Daqui em diante, a marca d'agua das linhas med e so a da recepcao. */
        medida_pintar_pilha();
    }
#endif

    uint32_t pedidos = 0u;
    for (;;) {
        armar(ANEL_RX);
        recepcao_iniciar(&rx, ler_dwt, HAL_GetTick());
        estouros_no_pedido = n_estouros;
        enviar_linha(pacote_pedido(linha, sizeof linha, ++pedidos));
#ifdef FIRMWARE_MEDIDA
        medida_pedido(ler_dwt());
#endif

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

#ifdef FIRMWARE_MEDIDA
        const uint32_t t_resp = ler_dwt();
#endif
        if (e == RECEPCAO_COMPLETO) {
            responder_segmento();
#ifdef FIRMWARE_MEDIDA
            const uint32_t resposta = ler_dwt() - t_resp;
            enviar_linha(snprintf(linha, sizeof linha,
                                  "med grupo=%u id=%lu espera_us=%lu linha_us=%lu lacunas_us=%lu "
                                  "lacuna_max_us=%lu consumir=%lu leituras=%lu consumir_max_us=%lu "
                                  "resposta_us=%lu pilha=%lu\r\n",
                                  (unsigned)rx.grupo, (unsigned long)rx.id, (unsigned long)us(med.espera),
                                  (unsigned long)us(med.t_ultimo - med.t_primeiro),
                                  (unsigned long)us(med.lacunas), (unsigned long)us(med.lacuna_max),
                                  (unsigned long)med.consumir, (unsigned long)med.leituras,
                                  (unsigned long)us(med.consumir_max),
                                  (unsigned long)us(resposta), (unsigned long)medida_pilha_usada()));
#endif
        } else {
            responder_erro();
        }
    }
}
