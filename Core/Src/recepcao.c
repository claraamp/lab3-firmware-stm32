/*
 * recepcao.c -- parser do quadro da recepcao pela serial, em fluxo (ver recepcao.h)
 *
 * Compilado com -O2 inclusive no Debug (CMakeLists.txt da raiz), como a cadeia e a
 * decimacao, para os ciclos medidos pelo DWT serem os do firmware final.
 */

#include "recepcao.h"

#include <string.h>

#include "crc32.h"

_Static_assert(RECEPCAO_N_AMOSTRAS == MFCC_N_AMOSTRAS, "o corpo do quadro tem que ser um segmento da cadeia");
_Static_assert(RECEPCAO_TAM_QUADRO == 25618u, "quadro de 18 + 25.600 B");

/* Impede o compilador de mover a conta para fora da janela do contador. */
#define BARREIRA() __asm__ volatile("" ::: "memory")

/* Mede `chamada` e soma em r->ciclos (so se ha contador). */
#define MEDIR(r, chamada)                                \
    do {                                                 \
        uint32_t m0_ = 0u;                               \
        BARREIRA();                                      \
        if ((r)->ler_ciclos) {                           \
            m0_ = (r)->ler_ciclos();                     \
        }                                                \
        BARREIRA();                                      \
        chamada;                                         \
        BARREIRA();                                      \
        if ((r)->ler_ciclos) {                           \
            (r)->ciclos += (r)->ler_ciclos() - m0_;      \
        }                                                \
        BARREIRA();                                      \
    } while (0)

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void recepcao_iniciar(recepcao_t *r, recepcao_ciclos_fn ler_ciclos, uint32_t agora_ms)
{
    memset(r, 0, sizeof *r);
    r->estado = RECEPCAO_ESPERANDO;
    r->motivo = RECEPCAO_OK;
    r->ler_ciclos = ler_ciclos;
    r->ultimo_ms = r->inicio_ms = r->fim_ms = agora_ms;
    mfcc_resumo_iniciar(&r->resumo);
}

static void erro(recepcao_t *r, recepcao_motivo_t motivo, uint32_t agora_ms)
{
    r->estado = RECEPCAO_ERRO;
    r->motivo = motivo;
    r->fim_ms = agora_ms;
}

/* O cabecalho inteiro chegou: confere e comeca o CRC. Devolve o motivo do erro, ou
 * RECEPCAO_OK. */
static recepcao_motivo_t conferir_cabecalho(recepcao_t *r)
{
    const uint8_t *c = r->cab;
    if (memcmp(c, "QSEG", 4) != 0) {
        return RECEPCAO_E_MAGICA;
    }
    /* com a magica certa, o grupo e o id vao no $ERR, mesmo que o resto esteja errado */
    r->tem_id = 1u;
    r->grupo = le16(&c[6]);
    r->id = le32(&c[8]);
    r->crc_lido = le32(&c[14]);
    if (c[4] != RECEPCAO_VERSAO) {
        return RECEPCAO_E_VERSAO;
    }
    if (c[5] != RECEPCAO_FORMATO_12K8) {
        return RECEPCAO_E_FORMATO;   /* inclusive o 2 (51,2 kHz), reservado */
    }
    if (le16(&c[12]) != RECEPCAO_N_AMOSTRAS) {
        return RECEPCAO_E_TAMANHO;
    }
    r->crc = crc32_bytes(0u, &c[4], 10u);   /* bytes 4..13; o 14..17 e o proprio CRC */
    return RECEPCAO_OK;
}

/* Uma amostra do corpo: no quadro do MFCC, um quadro a cada 128 amostras, com a
 * sobreposicao de 192. As mesmas chamadas, na mesma ordem, do mfcc_segmento(); as
 * 64 ultimas amostras nao entram em quadro, como no contrato (98 quadros). */
static void amostra(recepcao_t *r, int16_t v)
{
    r->quadro[r->n_pcm++] = v;
    if (r->n_pcm == MFCC_N_QUADRO) {
        if (r->n_quadros < MFCC_N_QUADROS) {
            static float coefs[MFCC_N_COEFS];   /* estatico, e nao na pilha */
            int e = 0;
            MEDIR(r, e = mfcc_quadro_coefs(r->quadro, coefs); mfcc_resumo_acumular(&r->resumo, coefs));
            if (e != 0) {
                r->st = e;
            }
            ++r->n_quadros;
        }
        memmove(r->quadro, &r->quadro[MFCC_PASSO], (MFCC_N_QUADRO - MFCC_PASSO) * sizeof r->quadro[0]);
        r->n_pcm = MFCC_N_QUADRO - MFCC_PASSO;
    }
}

recepcao_estado_t recepcao_receber(recepcao_t *r, const uint8_t *dados, size_t n, uint32_t agora_ms)
{
    if (n == 0u) {
        return r->estado;
    }
    r->ultimo_ms = agora_ms;
    switch (r->estado) {
    case RECEPCAO_COMPLETO:
        r->n_a_mais += (uint32_t)n;
        return r->estado;
    case RECEPCAO_ERRO:
    case RECEPCAO_SILENCIO:
        /* o silencio acabou de ser quebrado: volta a esperar outro */
        r->estado = RECEPCAO_ERRO;
        r->n_descartados += (uint32_t)n;
        return r->estado;
    case RECEPCAO_ESPERANDO:
        r->inicio_ms = agora_ms;
        r->estado = RECEPCAO_EM_ANDAMENTO;
        break;
    case RECEPCAO_EM_ANDAMENTO:
        break;
    }

    size_t i = 0u;
    /* cabecalho */
    if (r->n_bytes < RECEPCAO_TAM_CABECALHO) {
        while (i < n && r->n_bytes < RECEPCAO_TAM_CABECALHO) {
            r->cab[r->n_bytes++] = dados[i++];
        }
        if (r->n_bytes == RECEPCAO_TAM_CABECALHO) {
            const recepcao_motivo_t m = conferir_cabecalho(r);
            if (m != RECEPCAO_OK) {
                erro(r, m, agora_ms);
                r->n_descartados += (uint32_t)(n - i);
                return r->estado;
            }
        }
    }

    /* corpo */
    if (i < n && r->n_bytes >= RECEPCAO_TAM_CABECALHO) {
        const size_t falta = RECEPCAO_TAM_QUADRO - r->n_bytes;
        const size_t m = n - i < falta ? n - i : falta;
        r->crc = crc32_bytes(r->crc, &dados[i], m);
        for (size_t k = 0; k < m; ++k) {
            const uint32_t pos = r->n_bytes - RECEPCAO_TAM_CABECALHO + (uint32_t)k;
            if ((pos & 1u) == 0u) {
                r->byte_baixo = dados[i + k];   /* little-endian: o byte baixo primeiro */
            } else {
                const uint16_t u = (uint16_t)(r->byte_baixo | ((uint16_t)dados[i + k] << 8));
                ++r->n_amostras;
                amostra(r, (int16_t)u);
            }
        }
        r->n_bytes += (uint32_t)m;
        i += m;
        if (r->n_bytes == RECEPCAO_TAM_QUADRO) {
            if (r->crc != r->crc_lido) {
                erro(r, RECEPCAO_E_CRC, agora_ms);
                r->n_descartados += (uint32_t)(n - i);
            } else {
                r->estado = RECEPCAO_COMPLETO;
                r->fim_ms = agora_ms;
                r->n_a_mais += (uint32_t)(n - i);
            }
        }
    }
    return r->estado;
}

recepcao_estado_t recepcao_tempo(recepcao_t *r, uint32_t agora_ms)
{
    const uint32_t calado = agora_ms - r->ultimo_ms;
    if (r->estado == RECEPCAO_EM_ANDAMENTO && calado >= RECEPCAO_SILENCIO_MS) {
        /* o silencio que define o truncado ja e o silencio que o erro espera */
        erro(r, RECEPCAO_E_TRUNCADO, agora_ms);
    }
    if (r->estado == RECEPCAO_ERRO && calado >= RECEPCAO_SILENCIO_MS) {
        r->estado = RECEPCAO_SILENCIO;
    }
    return r->estado;
}

int recepcao_caracteristicas(recepcao_t *r, float caracteristicas[2 * MFCC_N_COEFS])
{
    if (r->estado != RECEPCAO_COMPLETO || r->n_quadros != MFCC_N_QUADROS) {
        memset(caracteristicas, 0, sizeof(float) * 2 * MFCC_N_COEFS);
        return -1;
    }
    if (r->st != 0) {
        memset(caracteristicas, 0, sizeof(float) * 2 * MFCC_N_COEFS);
        return r->st;
    }
    /* ordem do JSON do modelo: as 13 medias e depois os 13 desvios */
    MEDIR(r, mfcc_resumo_finalizar(&r->resumo, &caracteristicas[0], &caracteristicas[MFCC_N_COEFS]));
    return 0;
}

const char *recepcao_motivo_texto(recepcao_motivo_t m)
{
    switch (m) {
    case RECEPCAO_E_MAGICA:
        return "magica";
    case RECEPCAO_E_VERSAO:
        return "versao";
    case RECEPCAO_E_FORMATO:
        return "formato";
    case RECEPCAO_E_TAMANHO:
        return "tamanho";
    case RECEPCAO_E_CRC:
        return "crc";
    case RECEPCAO_E_TRUNCADO:
        return "truncado";
    case RECEPCAO_OK:
    default:
        return "";
    }
}
