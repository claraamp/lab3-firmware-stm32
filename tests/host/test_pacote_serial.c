/*
 * Teste no PC do pacote da serial (formato v1, Core/Src/pacote_serial.c) e do
 * pacote que o marco monta. Nao precisa da placa:
 *
 *   gcc -std=c11 -O2 -Wall -Wextra -Werror -Wdouble-promotion -DMARCO_HOST \
 *       -ICore/Inc tests/host/test_pacote_serial.c Core/Src/pacote_serial.c \
 *       Core/Src/texto_fixo.c Core/Src/marco_lda.c -lm -o build/test_pacote_serial
 *   ./build/test_pacote_serial > build/pacotes_exemplo.txt
 *   python3 tests/host/pacote/ler_pacotes.py --arquivo build/pacotes_exemplo.txt
 *
 * A saida padrao tem so linhas de pacote (um $CAB e um $SEG por segmento do
 * marco), para o leitor em Python conferir o mesmo formato; os erros vao para a
 * saida de erro.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "lda_modelo.h"
#include "lda_vetores_teste.h"
#include "marco_lda.h"
#include "pacote_serial.h"

static int falhas = 0;

#define CONFERE(cond, ...)                         \
    do {                                           \
        if (!(cond)) {                             \
            fprintf(stderr, "FALHOU: " __VA_ARGS__); \
            fputc('\n', stderr);                   \
            falhas++;                              \
        }                                          \
    } while (0)

static void confere_linha(const pacote_segmento_t *p, const char *esperada)
{
    char buf[PACOTE_TAM_MAX];
    const int len = pacote_linha(buf, sizeof buf, p);
    CONFERE(len == (int)strlen(esperada) && strcmp(buf, esperada) == 0,
            "linha\n  obtida:   %s  esperada: %s", buf, esperada);
}

/* Linhas exatas, com o checksum calculado a mao (XOR entre '$' e '*'). */
static void confere_formato(void)
{
    pacote_segmento_t p = {0};
    p.n = 7u;
    p.origem = (char)PACOTE_ORIGEM_REFERENCIA;
    p.clipe = 2;
    p.segmento = 1043;
    p.ciclos = 412u;
    pacote_decidir(&p, 231.4749f, PACOTE_NOV_NAO_ADOTADO, 0.0f, 0.0f);
    confere_linha(&p, "$SEG,1,7,R,2,1043,F,1,231.475,0,,,412*69\r\n");

    pacote_decidir(&p, -1577.95867f, PACOTE_NOV_NAO_ADOTADO, 99.0f, 1.0f);
    confere_linha(&p, "$SEG,1,7,R,2,1043,N,0,-1577.959,0,,,412*7A\r\n");

    /* Novidade disparando com a LDA dizendo normal: decisao falha, disparou = 2 */
    p.origem = (char)PACOTE_ORIGEM_CLIPE;
    p.clipe = 0;
    p.segmento = 1;
    pacote_decidir(&p, -1400.0f, PACOTE_NOV_MAHAL_MFCC, 812.25f, 61.5f);
    confere_linha(&p, "$SEG,1,7,C,0,1,F,2,-1400.000,3,812.250,61.500,412*6D\r\n");

    /* Audio ao vivo (reservado): clipe vazio */
    p.origem = (char)PACOTE_ORIGEM_AO_VIVO;
    p.clipe = -1;
    p.segmento = 0;
    pacote_decidir(&p, 10.0f, PACOTE_NOV_PENTE_BPFI, 3.0f, 4.0f);
    confere_linha(&p, "$SEG,1,7,A,,0,F,1,10.000,2,3.000,4.000,412*4C\r\n");

    char buf[PACOTE_TAM_MAX];
    pacote_cabecalho(buf, sizeof buf, "exp239", "ee0c3e088366", 26u, 100u);
    CONFERE(strcmp(buf, "$CAB,1,exp239,ee0c3e088366,26,100*3B\r\n") == 0, "cabecalho: %s", buf);
}

/* Os sentidos da decisao de 17/10: LDA >= 0, novidade > limiar. */
static void confere_sentidos(void)
{
    CONFERE(pacote_lda_e_falha(0.0f), "escore 0 tem que ser falha");
    CONFERE(!pacote_lda_e_falha(-0.0001f), "escore negativo tem que ser normal");
    CONFERE(pacote_lda_e_falha(0.0f) == lda_e_falha(0.0f), "regra diferente do lda_modelo.h");
    CONFERE(!pacote_novidade_dispara(5.0f, 5.0f), "pontuacao igual ao limiar nao dispara");
    CONFERE(pacote_novidade_dispara(5.0001f, 5.0f), "pontuacao acima do limiar dispara");

    pacote_segmento_t p = {0};
    pacote_decidir(&p, -1.0f, PACOTE_NOV_NAO_ADOTADO, 1e6f, 0.0f);
    CONFERE(p.disparou == 0u && !p.falha, "novidade nao adotada nao pode disparar");
    pacote_decidir(&p, 1.0f, PACOTE_NOV_MAHAL_MFCC, 2.0f, 1.0f);
    CONFERE(p.disparou == 3u && p.falha, "LDA e novidade juntas: disparou = 3");

    /* Fronteira: o sinal do texto segue a regra da LDA. -0,0004 e normal e sai
     * "-0.000" (o texto_fixo() sozinho daria "0.000"); o -0.0f nao e negativo,
     * entao e falha e sai "0.000". */
    p.n = 1u; p.origem = 'R'; p.clipe = 0; p.segmento = 5; p.ciclos = 0u;
    pacote_decidir(&p, -0.0004f, PACOTE_NOV_NAO_ADOTADO, 0.0f, 0.0f);
    char buf[PACOTE_TAM_MAX];
    pacote_linha(buf, sizeof buf, &p);
    CONFERE(strstr(buf, ",N,0,-0.000,") != NULL, "fronteira negativa: %s", buf);
    pacote_decidir(&p, -0.0f, PACOTE_NOV_NAO_ADOTADO, 0.0f, 0.0f);
    pacote_linha(buf, sizeof buf, &p);
    CONFERE(strstr(buf, ",F,1,0.000,") != NULL, "-0.0f e falha e sai sem sinal: %s", buf);
    pacote_decidir(&p, 0.0004f, PACOTE_NOV_NAO_ADOTADO, 0.0f, 0.0f);
    pacote_linha(buf, sizeof buf, &p);
    CONFERE(strstr(buf, ",F,1,0.000,") != NULL, "fronteira positiva: %s", buf);
    /* O marco continua sem "-0.000" (o texto_fixo() nao mudou) */
    char fmt[20];
    marco_fmt_fixo(fmt, sizeof fmt, -0.0004f, 3);
    CONFERE(strcmp(fmt, "0.000") == 0, "marco_fmt_fixo mudou: %s", fmt);
}

static void confere_pente(void)
{
    float pont, lim;
    /* BPFO passa 1 dB do limiar, BPFI passa 3 dB: vai a BPFI */
    uint8_t cod = pacote_escolher_pente(11.0f, 10.0f, 23.0f, 20.0f, &pont, &lim);
    CONFERE(cod == PACOTE_NOV_PENTE_BPFI && pont == 23.0f && lim == 20.0f, "pente: BPFI");
    /* Pontuacao maior nao basta: o que conta e a diferenca */
    cod = pacote_escolher_pente(30.0f, 25.0f, 31.0f, 30.0f, &pont, &lim);
    CONFERE(cod == PACOTE_NOV_PENTE_BPFO && pont == 30.0f, "pente: BPFO pela diferenca");
    /* Nenhuma passa: vai a mais perto do limiar, e nao dispara */
    cod = pacote_escolher_pente(-4.0f, 0.0f, -1.0f, 0.0f, &pont, &lim);
    CONFERE(cod == PACOTE_NOV_PENTE_BPFI && !pacote_novidade_dispara(pont, lim),
            "pente: nenhuma passa");
    /* Empate: BPFO */
    cod = pacote_escolher_pente(2.0f, 1.0f, 5.0f, 4.0f, &pont, &lim);
    CONFERE(cod == PACOTE_NOV_PENTE_BPFO, "pente: empate vai a BPFO");
}

/* PACOTE_TAM_MAX cobre o pior caso, e buffer pequeno devolve -1 sem estourar. */
static void confere_limites(void)
{
    pacote_segmento_t p = {0};
    p.n = 4294967295u;
    p.origem = (char)PACOTE_ORIGEM_CLIPE;
    p.clipe = 2147483647;
    p.segmento = -2147483647 - 1;
    p.ciclos = 4294967295u;
    pacote_decidir(&p, -4294967.0f, 255u, -4294967.0f, -4294967.0f);
    char buf[PACOTE_TAM_MAX];
    const int len = pacote_linha(buf, sizeof buf, &p);
    CONFERE(len > 0 && len < PACOTE_TAM_MAX, "pior caso nao coube em PACOTE_TAM_MAX (%d)", len);

    char pequeno[20];
    memset(pequeno, 'x', sizeof pequeno);
    CONFERE(pacote_linha(pequeno, sizeof pequeno, &p) == -1, "buffer pequeno tem que dar -1");
    /* Cabe a linha sem o "*HH\r\n": tambem tem que dar -1, nao uma linha sem checksum */
    pacote_decidir(&p, 1.0f, 0u, 0.0f, 0.0f);
    p.n = 7u; p.clipe = 2; p.segmento = 1043; p.ciclos = 412u; p.origem = 'R';
    const int inteira = pacote_linha(buf, sizeof buf, &p);
    char quase[PACOTE_TAM_MAX];
    CONFERE(pacote_linha(quase, (size_t)inteira - 4u, &p) == -1, "sem espaco para o checksum: -1");
    CONFERE(pacote_linha(quase, (size_t)inteira + 1u, &p) == inteira, "espaco exato tem que caber");
}

int main(void)
{
    confere_formato();
    confere_sentidos();
    confere_pente();
    confere_limites();

    /* O pacote do marco tem a mesma decisao do LED (lda_e_falha), em todos os
     * segmentos, e sai como exemplo na saida padrao. */
    char buf[PACOTE_TAM_MAX];
    pacote_cabecalho(buf, sizeof buf, VT_MODELO_ID, VT_JSON_SHA256_12,
                     (unsigned)LDA_N_FEATURES, 100u);
    fputs(buf, stdout);
    for (uint16_t i = 0; i < marco_n_segmentos(); ++i) {
        marco_resultado_t r;
        marco_avaliar(i, &r);
        pacote_segmento_t p;
        marco_pacote(i, &r, i, 0u, &p);
        CONFERE(p.falha == r.falha && p.disparou == (r.falha ? 1u : 0u),
                "segmento %u: decisao do pacote diferente da do LED", (unsigned)i);
        CONFERE(p.falha == vt_falha_ref[i], "segmento %u: decisao diferente do CSV", (unsigned)i);
        const int len = pacote_linha(buf, sizeof buf, &p);
        CONFERE(len > 0, "segmento %u: linha nao coube", (unsigned)i);
        fputs(buf, stdout);
    }

    fprintf(stderr, "test_pacote_serial: %s\n", falhas ? "FALHOU" : "tudo certo");
    return falhas ? 1 : 0;
}
