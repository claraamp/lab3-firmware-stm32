/*
 * test_decimacao.c -- a decimacao embarcada no PC (Core/Src/decimacao.c)
 *
 * Roda no PC, com a CMSIS-DSP generica do harness, a mesma verificacao que a placa
 * faz ao ligar (decimacao_verificar_com), e confere:
 *   - a fase do arm_fir_decimate_f32 e a 0 (impulso);
 *   - o recorte da 12.800 saidas (com o ultimo bloco de 148), sem saturar, a no
 *     maximo 1 LSB do int16 do Python, e o escore do segmento decimado;
 *   - a cadeia quadro a quadro (98 quadros, buffer de 320) da o escore BIT A BIT
 *     igual ao do mfcc_segmento() no segundo inteiro, com o mesmo int16;
 *   - o CRC acumulado e o das 12.800 saidas;
 *   - a requantizacao satura em vez de dar a volta e arredonda o meio para o par;
 *   - os criterios reprovam quando devem (saida curta, saturada, quadros a menos,
 *     2 LSB, erro em float acima da tolerancia, NaN, passadas diferentes) e nao
 *     reprovam por amostras demais a 1 LSB (empates, revisao da Julia no PR #9);
 *   - o laco (decimacao_inferir, 22/10): duas inferencias seguidas dao o mesmo int16
 *     e o mesmo escore, bit a bit, da passada do boot; o $SEG sai com origem D e a
 *     linha ponta diz OK (e FALHOU sem ok).
 *
 * Com um argumento, grava as 12.800 saidas em float32 (little-endian, em LSB) nesse
 * arquivo, para o comparar_decimacao.py comparar com a referencia em float64:
 *
 *   ./build/harness/test_decimacao build/decimacao_c.f32 > build/decimacao_pc.txt
 *   python3 tests/host/decimacao/comparar_decimacao.py build/decimacao_c.f32
 *
 * Na saida padrao vai o que a placa mandaria (as linhas "decim ..."), com os ciclos
 * trocados por nanossegundos do PC (clock de 1000 MHz na conta).
 */

#define _POSIX_C_SOURCE 199309L

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "clipes_demo.h"
#include "decimacao.h"
#include "lda_modelo.h"
#include "mfcc_estagios.h"

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

static char saida[8192];
static size_t n_saida = 0;

static void enviar(const char *texto, size_t n)
{
    fwrite(texto, 1, n, stdout);
    if (n_saida + n < sizeof saida) {
        memcpy(&saida[n_saida], texto, n);
        n_saida += n;
        saida[n_saida] = '\0';
    }
}

static uint32_t ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)((uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec);
}

/* as saidas em float (LSB) e requantizadas, na ordem */
static float y_c[DECIM_N_SAIDAS];
static int16_t q_c[DECIM_N_SAIDAS];
static uint32_t n_y = 0u;

static void guardar(void *ctx, uint32_t j, float y)
{
    (void)ctx;
    if (j < DECIM_N_SAIDAS) {
        y_c[j] = y;
        q_c[j] = decimacao_quantizar(y, NULL);
        n_y = j + 1u;
    }
}

/* uma medida e uma comparacao que passam, para estragar um campo de cada vez */
static void base(decimacao_medida_t *m, decimacao_comparacao_t *c)
{
    memset(m, 0, sizeof *m);
    memset(c, 0, sizeof *c);
    m->n_saidas = DECIM_N_SAIDAS;
    m->n_total = DECIM_N_SAIDAS_TOTAL;
    m->n_quadros = MFCC_N_QUADROS;
    m->crc32 = c->crc32 = 0x12345678u;
    c->max_lsb = 0.5f;
    c->pico = 5533.0f;
    c->dif_int16_max = 1;
    c->n_dif = 2u;
}

int main(int argc, char **argv)
{
    /* 1. requantizacao (ja em LSB): arredonda o meio para o par e satura */
    {
        uint32_t sat = 0u;
        CONFERIR(decimacao_quantizar(32767.0f, &sat) == 32767 && sat == 0u, "32767 deveria dar 32767");
        CONFERIR(decimacao_quantizar(-32768.0f, &sat) == -32768 && sat == 0u, "-32768 deveria dar -32768");
        CONFERIR(decimacao_quantizar(0.0f, &sat) == 0, "0 deveria dar 0");
        CONFERIR(decimacao_quantizar(2.5f, &sat) == 2 && decimacao_quantizar(3.5f, &sat) == 4
                     && decimacao_quantizar(-2.5f, &sat) == -2,
                 "o meio deveria ir para o par (2,5 -> 2; 3,5 -> 4; -2,5 -> -2)");
        CONFERIR(decimacao_quantizar(40000.0f, &sat) == 32767 && sat == 1u, "40000 deveria saturar em 32767");
        CONFERIR(decimacao_quantizar(-40000.0f, &sat) == -32768 && sat == 2u,
                 "-40000 deveria saturar em -32768");
        CONFERIR(decimacao_quantizar(32767.6f, &sat) == 32767 && sat == 3u,
                 "32767,6 arredonda para 32768 e deveria saturar");
    }

    /* 2. a fase do decimador */
    CONFERIR(decimacao_fase() == 0, "decimacao_fase() = %d, esperado 0", decimacao_fase());

    /* 3. sem o init do MFCC, o executar recusa */
    {
        decimacao_medida_t m;
        CONFERIR(decimacao_executar(NULL, NULL, NULL, 0u, &m) == -3,
                 "decimacao_executar sem mfcc_estagios_init deveria devolver -3");
    }

    /* 3b. o laco tambem recusa, sem a verificacao do boot */
    {
        decimacao_inferencia_t inf;
        CONFERIR(decimacao_inferir(NULL, &inf) == -3 && !inf.ok && !inf.igual_boot,
                 "decimacao_inferir antes do init deveria devolver -3, sem ok");
    }

    /* 4. a verificacao da placa, no PC */
    const int ok = decimacao_verificar_com(enviar, ns, 1000000000u);
    CONFERIR(ok == 1, "decimacao_verificar_com devolveu %d", ok);
    CONFERIR(strstr(saida, "DECIMACAO OK duracao=") != NULL, "sem DECIMACAO OK na saida");
    CONFERIR(strstr(saida, "FALHOU") == NULL, "alguma linha FALHOU");
    CONFERIR(strstr(saida, "n=12800/12837") != NULL, "a saida nao tem 12.800 de 12.837 saidas");
    CONFERIR(strstr(saida, "quadros=98") != NULL, "a cadeia nao recebeu 98 quadros");

    /* 5. a passada medida, com as saidas guardadas */
    decimacao_medida_t m;
    CONFERIR(decimacao_executar(guardar, NULL, NULL, 0u, &m) == 0, "decimacao_executar falhou");
    CONFERIR(m.n_saidas == DECIM_N_SAIDAS && n_y == DECIM_N_SAIDAS, "n_saidas=%lu",
             (unsigned long)m.n_saidas);
    CONFERIR(m.n_total == DECIM_N_SAIDAS_TOTAL, "n_total=%lu", (unsigned long)m.n_total);
    CONFERIR(m.n_saturadas == 0u, "saturadas=%lu", (unsigned long)m.n_saturadas);
    CONFERIR(m.n_quadros == MFCC_N_QUADROS, "n_quadros=%lu", (unsigned long)m.n_quadros);
    CONFERIR(m.crc32 == decimacao_crc32(q_c, DECIM_N_SAIDAS), "o CRC acumulado nao e o das saidas");

    /* 6. quadro a quadro = segundo inteiro, bit a bit (as mesmas chamadas, na mesma ordem) */
    {
        float x[2 * MFCC_N_COEFS];
        CONFERIR(mfcc_segmento(q_c, x) == 0, "mfcc_segmento falhou");
        const float escore_inteiro = lda_escore(x);
        CONFERIR(memcmp(&escore_inteiro, &m.escore, sizeof m.escore) == 0,
                 "quadro a quadro deu %.9g e o segundo inteiro %.9g", (double)m.escore,
                 (double)escore_inteiro);
    }

    /* 7. a conferencia */
    decimacao_comparacao_t c;
    decimacao_conferir(clipes[RECORTE_CLIPE].pcm, NULL, NULL, &c);
    CONFERIR(decimacao_avaliar(&m, &c), "a conferencia deveria passar");
    CONFERIR(c.iguais, "a conferencia deu outro int16 que a passada medida");
    CONFERIR(c.dif_int16_max <= 1, "dif_int16_max=%ld", (long)c.dif_int16_max);
    fprintf(stderr, "PC: max_lsb=%.6f (j=%lu) tol=%.6f pico=%.0f dif_int16_max=%ld n_dif=%lu escore=%.4f\n",
            (double)c.max_lsb, (unsigned long)c.j_max_lsb, (double)DECIM_TOL_LSB_PLACA(c.pico),
            (double)c.pico, (long)c.dif_int16_max, (unsigned long)c.n_dif, (double)m.escore);

    /* 8. os criterios reprovam quando devem */
    {
        decimacao_medida_t mm;
        decimacao_comparacao_t cc;
        base(&mm, &cc);
        CONFERIR(decimacao_avaliar(&mm, &cc), "a comparacao base deveria passar");

        base(&mm, &cc);
        cc.n_dif = 1000u;   /* empates a 1 LSB nao reprovam (gravacoes fortes) */
        CONFERIR(decimacao_avaliar(&mm, &cc), "amostras a 1 LSB nao deveriam reprovar");

        base(&mm, &cc);
        mm.n_saidas = DECIM_N_SAIDAS - 37u;   /* esqueceu o ultimo bloco de 148 */
        mm.n_total = DECIM_N_SAIDAS_TOTAL - 37u;
        CONFERIR(!decimacao_avaliar(&mm, &cc), "sem o ultimo bloco deveria reprovar");

        base(&mm, &cc);
        mm.n_saturadas = 1u;
        CONFERIR(!decimacao_avaliar(&mm, &cc), "amostra saturada deveria reprovar");

        base(&mm, &cc);
        mm.n_quadros = MFCC_N_QUADROS - 1u;
        CONFERIR(!decimacao_avaliar(&mm, &cc), "quadros a menos deveria reprovar");

        base(&mm, &cc);
        cc.dif_int16_max = 2;
        CONFERIR(!decimacao_avaliar(&mm, &cc), "2 LSB deveria reprovar");

        base(&mm, &cc);
        cc.max_lsb = DECIM_TOL_LSB_PLACA(cc.pico) * 1.01f;
        CONFERIR(!decimacao_avaliar(&mm, &cc), "erro em float acima da tolerancia deveria reprovar");

        base(&mm, &cc);
        cc.max_lsb = NAN;
        CONFERIR(!decimacao_avaliar(&mm, &cc), "NaN deveria reprovar");

        base(&mm, &cc);
        cc.crc32 ^= 1u;
        CONFERIR(!decimacao_avaliar(&mm, &cc) && !cc.iguais, "passadas diferentes deveriam reprovar");
    }

    /* 9. uma referencia errada de 2 LSB numa amostra e pega pela conferencia */
    {
        static int16_t ref_errada[DECIM_N_SAIDAS];
        memcpy(ref_errada, clipes[RECORTE_CLIPE].pcm, sizeof ref_errada);
        ref_errada[6400] = (int16_t)(ref_errada[6400] + 2);
        decimacao_comparacao_t ce;
        decimacao_conferir(ref_errada, NULL, NULL, &ce);
        CONFERIR(!decimacao_avaliar(&m, &ce), "uma amostra a 2 LSB deveria reprovar (max %ld)",
                 (long)ce.dif_int16_max);
    }

    if (argc > 1) {
        FILE *f = fopen(argv[1], "wb");
        CONFERIR(f != NULL, "nao abriu %s", argv[1]);
        if (f) {
            CONFERIR(fwrite(y_c, sizeof y_c[0], DECIM_N_SAIDAS, f) == DECIM_N_SAIDAS, "fwrite");
            fclose(f);
            fprintf(stderr, "saidas em float32 (LSB) gravadas em %s\n", argv[1]);
        }
    }

    /* 10. o laco (primeira inferencia de ponta a ponta, 22/10): a inferencia de
     * novo da o mesmo int16 e o mesmo escore, bit a bit, da passada do boot, e o
     * pacote sai com origem D e a decisao da referencia (normal) */
    {
        static const char PREFIXO_PONTA[] = "ponta seg=29 clipe=normal fs=51200->12800 ";
        static const char PREFIXO_SEG_D[] = "$SEG,1,41,D,0,0,N,0,-1331.";
        decimacao_inferencia_t inf;
        for (int volta = 0; volta < 2; ++volta) {
            CONFERIR(decimacao_inferir(ns, &inf) == 0, "decimacao_inferir falhou (volta %d)", volta);
            CONFERIR(inf.igual_boot, "a inferencia do laco nao deu o mesmo da passada do boot (volta %d)",
                     volta);
            CONFERIR(inf.ok, "a inferencia do laco deveria passar (volta %d)", volta);
            CONFERIR(memcmp(&inf.m.escore, &m.escore, sizeof m.escore) == 0,
                     "escore do laco %.9g, da passada medida %.9g", (double)inf.m.escore,
                     (double)m.escore);
            CONFERIR(inf.m.crc32 == m.crc32, "CRC do laco diferente");
            CONFERIR(!inf.r.falha && inf.r.ok_escore && inf.r.ok_decisao,
                     "o recorte e normal e tem que bater com a referencia");
        }
        pacote_segmento_t p;
        decimacao_pacote(&inf, 41u, &p);
        CONFERIR(p.n == 41u && p.origem == 'D' && p.clipe == RECORTE_CLIPE && p.segmento == 0
                     && !p.falha && p.disparou == 0u && p.ciclos == inf.m.ciclos,
                 "pacote de origem D com campos errados");
        char buf[320];
        int len = pacote_linha(buf, sizeof buf, &p);
        CONFERIR(len > 0 && strncmp(buf, PREFIXO_SEG_D, strlen(PREFIXO_SEG_D)) == 0, "$SEG: %s", buf);
        fputs(buf, stdout);
        len = decimacao_linha_ponta(buf, sizeof buf, &inf, 1000000000u);
        CONFERIR(len > 0, "linha ponta nao coube");
        CONFERIR(strncmp(buf, PREFIXO_PONTA, strlen(PREFIXO_PONTA)) == 0
                     && strstr(buf, " dec=normal OK ") != NULL && strstr(buf, " igual_boot=1\r\n") != NULL,
                 "linha ponta: %s", buf);
        fputs(buf, stdout);
        CONFERIR(decimacao_linha_ponta(buf, 40u, &inf, 1000000000u) == -1,
                 "buffer pequeno deveria devolver -1");

        /* sem ok, a linha diz FALHOU (o laco pisca o erro em vez de mostrar a decisao) */
        inf.ok = 0u;
        inf.igual_boot = 0u;
        len = decimacao_linha_ponta(buf, sizeof buf, &inf, 1000000000u);
        CONFERIR(len > 0 && strstr(buf, " FALHOU ") != NULL && strstr(buf, "igual_boot=0") != NULL,
                 "linha ponta sem ok: %s", buf);
    }

    if (falhas) {
        fprintf(stderr, "TEST DECIMACAO FALHOU (%d)\n", falhas);
        return 1;
    }
    fprintf(stderr, "TEST DECIMACAO OK (fase 0, 12.800 saidas, quadro a quadro = segundo inteiro, "
                    "reprova quando deve, laco de ponta a ponta igual ao boot)\n");
    return 0;
}
