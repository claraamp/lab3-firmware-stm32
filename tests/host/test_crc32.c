/*
 * test_crc32.c -- confere, no PC, o CRC-32 comum (Core/Src/crc32.c) contra o zlib
 *
 * Os valores esperados sao do zlib.crc32() do Python (calculados uma vez e
 * copiados aqui; o comando que gera cada um esta no comentario ao lado):
 *   - o vetor de conferencia do CRC-32, "123456789" -> 0xCBF43926, e o vazio -> 0;
 *   - um CRC continuado da o mesmo que o de uma vez so, cortado em qualquer ponto
 *     (a recepcao pela serial acumula o CRC conforme os bytes chegam);
 *   - crc32_int16() amostra a amostra da o mesmo que o zlib sobre '<i2', inclusive
 *     com amostras negativas e nos extremos;
 *   - 25.618 bytes pseudoaleatorios (o tamanho de um quadro da recepcao), inteiros
 *     e no recorte do CRC do quadro: bytes 4..17 do cabecalho e depois o corpo.
 *
 * Compilar e rodar (na raiz do repositorio):
 *   cmake -S tests/host/harness -B build/harness -G Ninja && cmake --build build/harness
 *   ./build/harness/test_crc32
 */

#include <stdio.h>
#include <string.h>

#include "crc32.h"

static int falhas = 0;

static void conferir(uint32_t obtido, uint32_t esperado, const char *o_que)
{
    if (obtido != esperado) {
        printf("FALHOU: %s: %08lX (esperado %08lX)\n", o_que, (unsigned long)obtido,
               (unsigned long)esperado);
        ++falhas;
    }
}

/* Bytes pseudoaleatorios reproduziveis no Python:
 *   x = 1; x = (x * 1664525 + 1013904223) % 2**32; byte = x >> 24 */
#define N_QUADRO 25618u
static uint8_t quadro[N_QUADRO];

static void gerar_quadro(void)
{
    uint32_t x = 1u;
    for (size_t i = 0; i < N_QUADRO; ++i) {
        x = x * 1664525u + 1013904223u;
        quadro[i] = (uint8_t)(x >> 24);
    }
}

int main(void)
{
    /* zlib.crc32(b'123456789') */
    const char *ref = "123456789";
    conferir(crc32_bytes(0u, ref, strlen(ref)), 0xCBF43926u, "\"123456789\"");
    conferir(crc32_bytes(0u, ref, 0), 0x00000000u, "vazio");
    /* comecar com um CRC e nao somar nada devolve o mesmo CRC */
    conferir(crc32_bytes(0xCBF43926u, ref, 0), 0xCBF43926u, "continuar com nada");

    /* continuado, cortado em todos os pontos */
    for (size_t k = 0; k <= strlen(ref); ++k) {
        const uint32_t c = crc32_bytes(crc32_bytes(0u, ref, k), ref + k, strlen(ref) - k);
        char o_que[48];
        snprintf(o_que, sizeof o_que, "\"123456789\" cortado em %zu", k);
        conferir(c, 0xCBF43926u, o_que);
    }

    /* zlib.crc32(struct.pack('<8h', 0, 1, -1, 32767, -32768, 12345, -12345, 256)) */
    const int16_t v[] = {0, 1, -1, 32767, -32768, 12345, -12345, 256};
    uint32_t c16 = 0u;
    for (size_t i = 0; i < sizeof v / sizeof v[0]; ++i) {
        c16 = crc32_int16(c16, v[i]);
    }
    conferir(c16, 0x9FE7A0B9u, "crc32_int16 amostra a amostra");

    /* o mesmo, montando os bytes em little-endian a mao, independente do PC */
    uint8_t le[2 * sizeof v / sizeof v[0]];
    for (size_t i = 0; i < sizeof v / sizeof v[0]; ++i) {
        const uint16_t u = (uint16_t)v[i];
        le[2 * i] = (uint8_t)(u & 0xFFu);
        le[2 * i + 1] = (uint8_t)(u >> 8);
    }
    conferir(crc32_bytes(0u, le, sizeof le), 0x9FE7A0B9u, "bytes little-endian das amostras");

    /* zlib.crc32(b) dos 25.618 bytes */
    gerar_quadro();
    conferir(crc32_bytes(0u, quadro, N_QUADRO), 0xDA87726Fu, "25.618 bytes de uma vez");
    uint32_t aos_poucos = 0u;
    for (size_t i = 0; i < N_QUADRO; i += 7u) {
        const size_t n = N_QUADRO - i < 7u ? N_QUADRO - i : 7u;
        aos_poucos = crc32_bytes(aos_poucos, quadro + i, n);
    }
    conferir(aos_poucos, 0xDA87726Fu, "25.618 bytes de 7 em 7");

    /* zlib.crc32(b[18:], zlib.crc32(b[4:18])): o recorte do CRC do quadro */
    conferir(crc32_bytes(crc32_bytes(0u, quadro + 4, 14u), quadro + 18, N_QUADRO - 18u),
             0x36CCAB54u, "bytes 4..17 e depois 18..");

    if (falhas) {
        printf("TEST CRC32 FALHOU (%d)\n", falhas);
        return 1;
    }
    printf("TEST CRC32 OK (zlib: vetor 123456789, continuado, int16 little-endian, 25.618 B)\n");
    return 0;
}
