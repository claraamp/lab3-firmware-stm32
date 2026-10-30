/*
 * pacote_serial.c -- formato v1 do pacote da serial (ver pacote_serial.h e a
 * secao "Pacote da serial" do README)
 */

#include "pacote_serial.h"

#include <math.h>
#include <stdio.h>

#include "texto_fixo.h"

/* Casas decimais dos campos em float. Com 3 casas, |v| ate ~4e6 (texto_fixo.h);
 * o escore da LDA fica em +-1.600 e as pontuacoes de novidade bem abaixo disso. */
#define CASAS 3u

int pacote_lda_e_falha(float escore)
{
    return escore >= 0.0f;
}

int pacote_novidade_dispara(float pontuacao, float limiar)
{
    return pontuacao > limiar;
}

void pacote_decidir(pacote_segmento_t *p, float escore_lda, uint8_t cod_novidade,
                    float pont_novidade, float lim_novidade)
{
    p->escore_lda = escore_lda;
    p->cod_novidade = cod_novidade;
    p->pont_novidade = pont_novidade;
    p->lim_novidade = lim_novidade;

    /* Valor nao finito (NaN ou infinito): erro, nunca "normal". O NaN passaria
     * como normal (NaN >= 0 e falso) e acenderia o LED verde. Hoje a cadeia nao
     * gera NaN com entrada valida (o log tem o epsilon, o Welford divide por
     * n >= 1); o caso provavel e futuro: a pontuacao do pente com fundo zero
     * (infinito menos infinito). */
    const int nao_finito =
        !isfinite(escore_lda) ||
        (cod_novidade != PACOTE_NOV_NAO_ADOTADO && (!isfinite(pont_novidade) ||
                                                    !isfinite(lim_novidade)));
    if (nao_finito) {
        p->disparou = PACOTE_ERRO;
        p->falha = 1u;
        return;
    }

    p->disparou = 0u;
    if (pacote_lda_e_falha(escore_lda)) {
        p->disparou |= PACOTE_DISPAROU_LDA;
    }
    if (cod_novidade != PACOTE_NOV_NAO_ADOTADO &&
        pacote_novidade_dispara(pont_novidade, lim_novidade)) {
        p->disparou |= PACOTE_DISPAROU_NOVIDADE;
    }
    p->falha = (uint8_t)(p->disparou != 0u);
}

uint8_t pacote_escolher_pente(float pont_bpfo, float lim_bpfo, float pont_bpfi,
                              float lim_bpfi, float *pontuacao, float *limiar)
{
    if (pont_bpfi - lim_bpfi > pont_bpfo - lim_bpfo) {
        *pontuacao = pont_bpfi;
        *limiar = lim_bpfi;
        return PACOTE_NOV_PENTE_BPFI;
    }
    *pontuacao = pont_bpfo;
    *limiar = lim_bpfo;
    return PACOTE_NOV_PENTE_BPFO;
}

uint8_t pacote_checksum(const char *s, size_t len)
{
    uint8_t x = 0u;
    for (size_t i = 0; i < len; ++i) {
        x ^= (uint8_t)s[i];
    }
    return x;
}

/* Acrescenta "*HH\r\n" a uma linha que comeca com '$' e tem `len` bytes. */
static int fechar(char *buf, size_t n, int len)
{
    if (len < 0 || (size_t)len >= n) {
        return -1;
    }
    const uint8_t cs = pacote_checksum(buf + 1, (size_t)len - 1u);
    const int m = snprintf(buf + len, n - (size_t)len, "*%02X\r\n", (unsigned)cs);
    if (m < 0 || (size_t)m >= n - (size_t)len) {
        return -1;
    }
    return len + m;
}

int pacote_linha(char *buf, size_t n, const pacote_segmento_t *p)
{
    char escore_buf[21], pont[20] = "", lim[20] = "", clipe[16] = "";

    /* O sinal do texto do escore segue sempre a regra da LDA: qualquer escore
     * negativo (normal) sai com '-', inclusive os que arredondam a zero
     * (-0,0004 -> "-0.000"). O texto_fixo() evita o "-0.000" de proposito, para
     * o marco; aqui o '-' volta, para o painel poder ler a decisao da LDA pelo
     * sinal do texto sem excecao na fronteira. O -0.0f nao e negativo
     * (-0.0f < 0 e falso): sai "0.000", e e falha, como no lda_e_falha(). */
    texto_fixo(escore_buf + 1, sizeof escore_buf - 1u, p->escore_lda, CASAS);
    const char *escore = escore_buf + 1;
    if (p->escore_lda < 0.0f && escore_buf[1] != '-') {
        escore_buf[0] = '-';
        escore = escore_buf;
    }
    if (p->cod_novidade != PACOTE_NOV_NAO_ADOTADO) {
        texto_fixo(pont, sizeof pont, p->pont_novidade, CASAS);
        texto_fixo(lim, sizeof lim, p->lim_novidade, CASAS);
    }
    if (p->clipe >= 0) {
        snprintf(clipe, sizeof clipe, "%ld", (long)p->clipe);
    }

    const int len = snprintf(buf, n, "$SEG,%d,%lu,%c,%s,%ld,%c,%u,%s,%u,%s,%s,%lu",
                             PACOTE_VERSAO, (unsigned long)p->n, p->origem, clipe,
                             (long)p->segmento, p->falha ? 'F' : 'N', (unsigned)p->disparou,
                             escore, (unsigned)p->cod_novidade, pont, lim,
                             (unsigned long)p->ciclos);
    return fechar(buf, n, len);
}

int pacote_pedido(char *buf, size_t n, uint32_t pedido)
{
    const int len = snprintf(buf, n, "$PED,%d,%lu", PACOTE_VERSAO, (unsigned long)pedido);
    return fechar(buf, n, len);
}

int pacote_erro_recepcao(char *buf, size_t n, int tem_id, uint16_t grupo, uint32_t id,
                         const char *motivo)
{
    int len;
    if (tem_id) {
        len = snprintf(buf, n, "$ERR,%d,%u,%lu,%s", PACOTE_VERSAO, (unsigned)grupo,
                       (unsigned long)id, motivo);
    } else {
        len = snprintf(buf, n, "$ERR,%d,,,%s", PACOTE_VERSAO, motivo);
    }
    return fechar(buf, n, len);
}

int pacote_cabecalho(char *buf, size_t n, const char *modelo_id, const char *json_sha12,
                     unsigned n_features, unsigned clock_mhz)
{
    const int len = snprintf(buf, n, "$CAB,%d,%s,%s,%u,%u", PACOTE_VERSAO, modelo_id,
                             json_sha12, n_features, clock_mhz);
    return fechar(buf, n, len);
}
