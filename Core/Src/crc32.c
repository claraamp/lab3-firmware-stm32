/*
 * crc32.c -- CRC-32 do zlib, comum ao firmware (ver crc32.h)
 *
 * Compilado com -O2 inclusive no Debug (CMakeLists.txt da raiz), como o
 * decimacao.c que o chama dentro da janela medida pelo DWT.
 */

#include "crc32.h"

/* Entrada k: o CRC de meio byte k, com o polinomio refletido 0xEDB88320. */
static const uint32_t TABELA[16] = {
    0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu, 0x76DC4190u, 0x6B6B51F4u,
    0x4DB26158u, 0x5005713Cu, 0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
    0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu};

/* Um byte, no estado interno (ja invertido). */
static uint32_t passo(uint32_t estado, uint8_t byte)
{
    estado ^= byte;
    estado = (estado >> 4) ^ TABELA[estado & 0x0Fu];
    estado = (estado >> 4) ^ TABELA[estado & 0x0Fu];
    return estado;
}

uint32_t crc32_bytes(uint32_t crc, const void *dados, size_t n)
{
    const uint8_t *p = (const uint8_t *)dados;
    uint32_t estado = ~crc;
    for (size_t i = 0; i < n; ++i) {
        estado = passo(estado, p[i]);
    }
    return ~estado;
}

uint32_t crc32_int16(uint32_t crc, int16_t v)
{
    const uint16_t u = (uint16_t)v;   /* little-endian: o byte baixo primeiro */
    uint32_t estado = ~crc;
    estado = passo(estado, (uint8_t)(u & 0xFFu));
    estado = passo(estado, (uint8_t)(u >> 8));
    return ~estado;
}
