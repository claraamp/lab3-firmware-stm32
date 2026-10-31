/*
 * test_recepcao.c -- o parser do quadro da recepcao pela serial no PC (Core/Src/recepcao.c)
 *
 * Confere, com os 10 segmentos dos clipes (clipes_demo.h):
 *   - um quadro valido, entregue em pedacos de varios tamanhos (que cortam o
 *     cabecalho e as amostras ao meio), da as 26 caracteristicas e o escore da LDA
 *     BIT A BIT iguais aos do mfcc_segmento() no segundo inteiro;
 *   - bytes a mais depois do quadro so sao contados;
 *   - CRC errado (no corpo, num campo do cabecalho coberto pelo CRC, no proprio CRC)
 *     vira erro "crc" so no fim, sem caracteristicas;
 *   - o que vem antes da magica (um "RECEBER\n" atrasado, falsos comecos "Q", "QS",
 *     "QSE") e ignorado, e o quadro que vem atras sai bit a bit; um quadro com a
 *     magica corrompida e ignorado inteiro, sem erro e sem timeout;
 *   - versao, formato (inclusive o 2, reservado) e n_amostras errados viram erro
 *     assim que o cabecalho chega, e o resto e descartado;
 *   - truncado: RECEPCAO_SILENCIO_MS sem bytes no meio do quadro (no cabecalho ou no
 *     corpo), e um ms antes nao;
 *   - depois de um erro, so o silencio libera o proximo pedido, e um byte no meio
 *     recomeca a espera;
 *   - esperando o primeiro byte, nao ha timeout; e o tempo funciona na volta do
 *     contador de ms.
 *
 *   cmake -S tests/host/harness -B build/harness -G Ninja && cmake --build build/harness
 *   ./build/harness/test_recepcao     # TEST RECEPCAO OK, na saida de erro
 */

#include <stdio.h>
#include <string.h>

#include "clipes_demo.h"
#include "crc32.h"
#include "lda_modelo.h"
#include "mfcc_estagios.h"
#include "recepcao.h"

static int falhas = 0;

#define CONFERIR(cond, ...)                                    \
    do {                                                       \
        if (!(cond)) {                                         \
            fprintf(stderr, "FALHOU (linha %d): ", __LINE__);  \
            fprintf(stderr, __VA_ARGS__);                      \
            fprintf(stderr, "\n");                             \
            ++falhas;                                          \
        }                                                      \
    } while (0)

static uint8_t quadro[RECEPCAO_TAM_QUADRO + 64];
static recepcao_t r;

/* Monta o quadro como o script do PC: cabecalho, CRC sobre os bytes 4..13 e o corpo. */
static void montar(const int16_t *pcm, uint16_t grupo, uint32_t id, uint8_t versao, uint8_t formato,
                   uint16_t n_amostras)
{
    memcpy(quadro, "QSEG", 4);
    quadro[4] = versao;
    quadro[5] = formato;
    quadro[6] = (uint8_t)(grupo & 0xFFu);
    quadro[7] = (uint8_t)(grupo >> 8);
    for (int b = 0; b < 4; ++b) {
        quadro[8 + b] = (uint8_t)(id >> (8 * b));
    }
    quadro[12] = (uint8_t)(n_amostras & 0xFFu);
    quadro[13] = (uint8_t)(n_amostras >> 8);
    for (uint32_t k = 0; k < RECEPCAO_N_AMOSTRAS; ++k) {
        const uint16_t u = (uint16_t)pcm[k];
        quadro[18 + 2 * k] = (uint8_t)(u & 0xFFu);
        quadro[18 + 2 * k + 1] = (uint8_t)(u >> 8);
    }
    const uint32_t crc = crc32_bytes(crc32_bytes(0u, &quadro[4], 10u), &quadro[18], 2u * RECEPCAO_N_AMOSTRAS);
    for (int b = 0; b < 4; ++b) {
        quadro[14 + b] = (uint8_t)(crc >> (8 * b));
    }
}

/* O PCM do segmento s dos clipes: o segmento k do clipe comeca em k * 12.800. */
static const int16_t *pcm_segmento(int s)
{
    const clipe_segmento_t *seg = &clipes_segmentos[s];
    const clipe_t *c = &clipes[seg->clipe];
    const int k = s - (int)c->primeiro_segmento;
    return &c->pcm[(size_t)k * CLIPES_AMOSTRAS_POR_SEGMENTO];
}

/* Entrega `n` bytes em pedacos com os tamanhos de `pedacos` (em ciclo), todos no
 * instante `t`. Devolve o estado depois do ultimo; *saiu_antes guarda se algum
 * pedaco antes do ultimo ja terminou o quadro (completo ou erro). Esperando (a
 * magica ainda incompleta, com pedacos de 1 a 3 bytes) e normal. */
static recepcao_estado_t entregar(const uint8_t *b, size_t n, const size_t *pedacos, size_t n_pedacos,
                                  uint32_t t, int *saiu_antes)
{
    size_t i = 0, p = 0;
    recepcao_estado_t e = r.estado;
    *saiu_antes = 0;
    while (i < n) {
        size_t m = pedacos[p++ % n_pedacos];
        if (m > n - i) {
            m = n - i;
        }
        e = recepcao_receber(&r, &b[i], m, t);
        i += m;
        if (i < n && e != RECEPCAO_EM_ANDAMENTO && e != RECEPCAO_ESPERANDO) {
            *saiu_antes = 1;
        }
    }
    return e;
}

static uint32_t relogio = 0u;
static uint32_t ciclos_falsos(void)
{
    return relogio += 3u;
}

static void teste_validos(void)
{
    /* cortam o cabecalho (5 + 13), amostras ao meio (os impares), e o tamanho do
     * meio anel do DMA (1024) */
    static const size_t padroes[][6] = {
        {RECEPCAO_TAM_QUADRO, 0, 0, 0, 0, 0},
        {1, 0, 0, 0, 0, 0},
        {5, 13, 7, 1024, 3, 511},
        {17, 2, 1, 1024, 1024, 999},
        {18, 25600, 0, 0, 0, 0},
    };
    static const size_t n_padroes[] = {1, 1, 6, 6, 2};
    for (int s = 0; s < CLIPES_N_SEGMENTOS; ++s) {
        const int16_t *pcm = pcm_segmento(s);
        float ref[2 * MFCC_N_COEFS];
        CONFERIR(mfcc_segmento(pcm, ref) == 0, "mfcc_segmento falhou");
        const float escore_ref = lda_escore(ref);
        const uint16_t grupo = (uint16_t)(s % 3 + 1);
        const uint32_t id = 100000u + (uint32_t)s;
        montar(pcm, grupo, id, 1, 1, RECEPCAO_N_AMOSTRAS);
        for (size_t p = 0; p < sizeof n_padroes / sizeof n_padroes[0]; ++p) {
            recepcao_iniciar(&r, ciclos_falsos, 1000u);
            int saiu = 0;
            const recepcao_estado_t e =
                entregar(quadro, RECEPCAO_TAM_QUADRO, padroes[p], n_padroes[p], 1000u, &saiu);
            CONFERIR(!saiu, "seg %d padrao %zu: saiu de EM_ANDAMENTO antes do fim", s, p);
            CONFERIR(e == RECEPCAO_COMPLETO, "seg %d padrao %zu: estado %d (motivo %s)", s, p, (int)e,
                     recepcao_motivo_texto(r.motivo));
            CONFERIR(r.grupo == grupo && r.id == id && r.tem_id, "seg %d: grupo/id lidos errado", s);
            CONFERIR(r.n_amostras == RECEPCAO_N_AMOSTRAS && r.n_quadros == MFCC_N_QUADROS && r.n_a_mais == 0,
                     "seg %d: %lu amostras, %lu quadros", s, (unsigned long)r.n_amostras,
                     (unsigned long)r.n_quadros);
            float x[2 * MFCC_N_COEFS];
            CONFERIR(recepcao_caracteristicas(&r, x) == 0, "seg %d: caracteristicas falharam", s);
            CONFERIR(memcmp(x, ref, sizeof x) == 0, "seg %d padrao %zu: caracteristicas nao sao bit a bit as "
                     "do mfcc_segmento", s, p);
            const float escore = lda_escore(x);
            CONFERIR(memcmp(&escore, &escore_ref, sizeof escore) == 0, "seg %d: escore %.6f, ref %.6f", s,
                     (double)escore, (double)escore_ref);
            CONFERIR(lda_e_falha(escore) == clipes_segmentos[s].falha_ref, "seg %d: decisao diferente do CSV",
                     s);
            CONFERIR(r.ciclos > 0u, "seg %d: ciclos nao medidos", s);
        }
    }
}

static void teste_a_mais(void)
{
    montar(pcm_segmento(0), 1, 7, 1, 1, RECEPCAO_N_AMOSTRAS);
    memset(&quadro[RECEPCAO_TAM_QUADRO], 0xAA, 10);
    recepcao_iniciar(&r, NULL, 0u);
    recepcao_estado_t e = recepcao_receber(&r, quadro, RECEPCAO_TAM_QUADRO + 10u, 5u);
    CONFERIR(e == RECEPCAO_COMPLETO && r.n_a_mais == 10u, "a mais: estado %d, n_a_mais %lu", (int)e,
             (unsigned long)r.n_a_mais);
    e = recepcao_receber(&r, quadro, 3u, 6u);
    CONFERIR(e == RECEPCAO_COMPLETO && r.n_a_mais == 13u, "a mais depois do completo");
}

/* Um quadro com um byte trocado que so o CRC pega. */
static void teste_crc(size_t pos, const char *onde)
{
    montar(pcm_segmento(3), 2, 4242u, 1, 1, RECEPCAO_N_AMOSTRAS);
    quadro[pos] ^= 0x01u;
    recepcao_iniciar(&r, NULL, 0u);
    recepcao_estado_t e = recepcao_receber(&r, quadro, RECEPCAO_TAM_QUADRO - 1u, 10u);
    CONFERIR(e == RECEPCAO_EM_ANDAMENTO, "crc (%s): erro antes do fim (estado %d)", onde, (int)e);
    e = recepcao_receber(&r, &quadro[RECEPCAO_TAM_QUADRO - 1u], 1u, 11u);
    CONFERIR(e == RECEPCAO_ERRO && r.motivo == RECEPCAO_E_CRC, "crc (%s): estado %d motivo %s", onde, (int)e,
             recepcao_motivo_texto(r.motivo));
    CONFERIR(r.tem_id, "crc (%s): sem grupo/id para o $ERR", onde);
    float x[2 * MFCC_N_COEFS];
    CONFERIR(recepcao_caracteristicas(&r, x) == -1, "crc (%s): caracteristicas sairam com CRC errado", onde);
}

/* Lixo antes da magica e ignorado, e o quadro de tras sai inteiro e bit a bit. */
static void teste_antes_da_magica(void)
{
    static uint8_t buf[64 + RECEPCAO_TAM_QUADRO];
    /* falsos comecos de todos os tipos; o "QS" do fim faz o 'Q' do quadro de verdade
     * ser o byte que desfaz o falso comeco (e tem de recomecar a magica) */
    static const char lixo[] = "RECEBER\nQRECEBER\nQSQSE\x00QQ\x01QS";   /* sem o '\0' final */
    const size_t n_lixo = sizeof lixo - 1u;
    const int16_t *pcm = pcm_segmento(4);
    float ref[2 * MFCC_N_COEFS];
    mfcc_segmento(pcm, ref);
    montar(pcm, 9, 147u, 1, 1, RECEPCAO_N_AMOSTRAS);
    memcpy(buf, lixo, n_lixo);
    memcpy(&buf[n_lixo], quadro, RECEPCAO_TAM_QUADRO);
    for (size_t corte = 1; corte <= n_lixo + 6u; ++corte) {   /* corta o lixo e a magica em todo ponto */
        recepcao_iniciar(&r, NULL, 0u);
        recepcao_estado_t e = recepcao_receber(&r, buf, corte, 10u);
        CONFERIR(e == RECEPCAO_ESPERANDO || corte > n_lixo + 3u, "lixo cortado em %zu: estado %d", corte, (int)e);
        if (e == RECEPCAO_ESPERANDO) {   /* depois da magica, o silencio trunca (e o certo) */
            CONFERIR(recepcao_tempo(&r, 10u + 10u * RECEPCAO_SILENCIO_MS) == e, "lixo: timeout esperando a magica");
        }
        e = recepcao_receber(&r, &buf[corte], n_lixo + RECEPCAO_TAM_QUADRO - corte, 3000u);
        CONFERIR(e == RECEPCAO_COMPLETO, "lixo cortado em %zu: o quadro nao completou (estado %d)", corte, (int)e);
        CONFERIR(r.n_ignorados == n_lixo, "lixo: %lu ignorados, esperados %zu", (unsigned long)r.n_ignorados,
                 n_lixo);
        float x[2 * MFCC_N_COEFS];
        CONFERIR(recepcao_caracteristicas(&r, x) == 0 && memcmp(x, ref, sizeof x) == 0,
                 "lixo cortado em %zu: caracteristicas diferentes", corte);
    }

    /* magica corrompida: o quadro inteiro e ignorado, sem erro e sem timeout */
    quadro[3] = 'H';
    recepcao_iniciar(&r, NULL, 0u);
    recepcao_estado_t e = recepcao_receber(&r, quadro, RECEPCAO_TAM_QUADRO, 10u);
    CONFERIR(e == RECEPCAO_ESPERANDO && r.n_bytes < 4u, "magica corrompida: estado %d", (int)e);
    CONFERIR(recepcao_tempo(&r, 10u + RECEPCAO_SILENCIO_MS) == RECEPCAO_ESPERANDO,
             "magica corrompida virou timeout");
}

/* Cabecalho errado: erro assim que o 18o byte chega, e o resto e descartado. */
static void teste_cabecalho(uint8_t versao, uint8_t formato, uint16_t n_amostras, recepcao_motivo_t esperado)
{
    montar(pcm_segmento(1), 3, 99u, versao, formato, n_amostras);
    recepcao_iniciar(&r, NULL, 0u);
    recepcao_estado_t e = recepcao_receber(&r, quadro, 17u, 1u);
    CONFERIR(e == RECEPCAO_EM_ANDAMENTO, "cabecalho %s: erro antes do 18o byte",
             recepcao_motivo_texto(esperado));
    e = recepcao_receber(&r, &quadro[17], 101u, 2u);   /* o 18o e mais 100 */
    CONFERIR(e == RECEPCAO_ERRO && r.motivo == esperado, "cabecalho: estado %d motivo %s, esperado %s", (int)e,
             recepcao_motivo_texto(r.motivo), recepcao_motivo_texto(esperado));
    CONFERIR(r.n_descartados == 100u, "cabecalho %s: %lu descartados, esperados 100",
             recepcao_motivo_texto(esperado), (unsigned long)r.n_descartados);
    CONFERIR(r.tem_id && r.grupo == 3 && r.id == 99u, "cabecalho %s: grupo/id errados",
             recepcao_motivo_texto(esperado));
}

static void teste_truncado(void)
{
    montar(pcm_segmento(5), 1, 55u, 1, 1, RECEPCAO_N_AMOSTRAS);

    /* no corpo */
    recepcao_iniciar(&r, NULL, 900u);
    recepcao_receber(&r, quadro, 10000u, 1000u);
    CONFERIR(recepcao_tempo(&r, 1000u + RECEPCAO_SILENCIO_MS - 1u) == RECEPCAO_EM_ANDAMENTO,
             "truncado um ms antes do silencio");
    CONFERIR(recepcao_tempo(&r, 1000u + RECEPCAO_SILENCIO_MS) == RECEPCAO_SILENCIO && r.motivo == RECEPCAO_E_TRUNCADO,
             "corpo parado nao virou truncado (estado %d motivo %s)", (int)r.estado,
             recepcao_motivo_texto(r.motivo));
    CONFERIR(r.tem_id && r.id == 55u, "truncado no corpo sem o id");

    /* no cabecalho, depois da magica: sem grupo e id */
    recepcao_iniciar(&r, NULL, 0u);
    recepcao_receber(&r, quadro, 10u, 50u);
    CONFERIR(recepcao_tempo(&r, 50u + RECEPCAO_SILENCIO_MS) == RECEPCAO_SILENCIO && r.motivo == RECEPCAO_E_TRUNCADO
                 && !r.tem_id,
             "cabecalho parado nao virou truncado sem id");

    /* bytes chegando devagar, mas sem parar 200 ms, nao truncam */
    recepcao_iniciar(&r, NULL, 0u);
    uint32_t t = 0u;
    for (size_t i = 0; i < RECEPCAO_TAM_QUADRO; i += 2048u, t += RECEPCAO_SILENCIO_MS - 1u) {
        const size_t m = RECEPCAO_TAM_QUADRO - i < 2048u ? RECEPCAO_TAM_QUADRO - i : 2048u;
        CONFERIR(recepcao_tempo(&r, t) != RECEPCAO_ERRO, "truncou com bytes chegando a cada 199 ms");
        recepcao_receber(&r, &quadro[i], m, t);
    }
    CONFERIR(r.estado == RECEPCAO_COMPLETO, "quadro lento nao completou");

    /* esperando o primeiro byte: sem timeout */
    recepcao_iniciar(&r, NULL, 0u);
    CONFERIR(recepcao_tempo(&r, 1000000u) == RECEPCAO_ESPERANDO, "timeout sem nenhum byte");

    /* na volta do contador de ms */
    recepcao_iniciar(&r, NULL, 0xFFFFFF00u);
    recepcao_receber(&r, quadro, 100u, 0xFFFFFFF0u);
    CONFERIR(recepcao_tempo(&r, 0xFFFFFFF0u + RECEPCAO_SILENCIO_MS - 1u) == RECEPCAO_EM_ANDAMENTO,
             "volta do contador: truncou cedo");
    CONFERIR(recepcao_tempo(&r, 0xFFFFFFF0u + RECEPCAO_SILENCIO_MS) == RECEPCAO_SILENCIO,
             "volta do contador: nao truncou");
}

static void teste_silencio_depois_do_erro(void)
{
    montar(pcm_segmento(2), 1, 1u, 1, 1, RECEPCAO_N_AMOSTRAS);
    quadro[100] ^= 0x80u;
    recepcao_iniciar(&r, NULL, 0u);
    recepcao_receber(&r, quadro, RECEPCAO_TAM_QUADRO, 5000u);
    CONFERIR(r.estado == RECEPCAO_ERRO && r.motivo == RECEPCAO_E_CRC, "silencio: o CRC nao errou");
    CONFERIR(recepcao_tempo(&r, 5100u) == RECEPCAO_ERRO, "silencio antes do tempo");
    recepcao_receber(&r, quadro, 30u, 5100u);   /* lixo atrasado recomeca a espera */
    CONFERIR(recepcao_tempo(&r, 5299u) == RECEPCAO_ERRO, "silencio sem contar o byte atrasado");
    CONFERIR(recepcao_tempo(&r, 5300u) == RECEPCAO_SILENCIO, "sem silencio depois de 200 ms");
    CONFERIR(r.n_descartados == 30u && r.motivo == RECEPCAO_E_CRC, "silencio: descartados %lu, motivo %s",
             (unsigned long)r.n_descartados, recepcao_motivo_texto(r.motivo));
    recepcao_receber(&r, quadro, 1u, 5400u);   /* e de novo, depois do silencio */
    CONFERIR(r.estado == RECEPCAO_ERRO, "byte depois do silencio nao voltou a esperar");
}

int main(void)
{
    if (mfcc_estagios_init() != 0) {
        fprintf(stderr, "mfcc_estagios_init falhou\n");
        return 1;
    }
    teste_validos();
    teste_a_mais();
    teste_crc(18u + 2u * 6000u + 1u, "corpo");
    teste_crc(9u, "id no cabecalho");
    teste_crc(6u, "grupo no cabecalho");
    teste_crc(15u, "o proprio CRC");
    teste_crc(RECEPCAO_TAM_QUADRO - 1u, "ultimo byte");
    teste_antes_da_magica();
    teste_cabecalho(2, 1, RECEPCAO_N_AMOSTRAS, RECEPCAO_E_VERSAO);
    teste_cabecalho(1, 2, RECEPCAO_N_AMOSTRAS, RECEPCAO_E_FORMATO);
    teste_cabecalho(1, 0, RECEPCAO_N_AMOSTRAS, RECEPCAO_E_FORMATO);
    teste_cabecalho(1, 1, 12799u, RECEPCAO_E_TAMANHO);
    teste_truncado();
    teste_silencio_depois_do_erro();

    if (falhas) {
        fprintf(stderr, "TEST RECEPCAO FALHOU (%d)\n", falhas);
        return 1;
    }
    fprintf(stderr, "TEST RECEPCAO OK (10 segmentos bit a bit em 5 padroes de pedacos, lixo antes da magica, a mais, "
                    "crc, cabecalho, truncado, silencio)\n");
    return 0;
}
