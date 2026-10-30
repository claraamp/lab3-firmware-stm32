/*
 * crc32.h -- CRC-32 do zlib (polinomio refletido 0xEDB88320), comum ao firmware
 *
 * O mesmo CRC do zlib.crc32() do Python: crc32_bytes(0, dados, n) e
 * zlib.crc32(dados), e crc32_bytes(crc, ...) continua um CRC ja comecado, como
 * zlib.crc32(dados, crc). Vetor de conferencia: "123456789" da 0xCBF43926.
 *
 * Tabela de 16 entradas (64 B de flash), meio byte por passo. Antes deste modulo,
 * a mesma tabela estava copiada no blocos13.c (b13_crc32) e no decimacao.c
 * (decimacao_crc32_acumular); a recepcao de segmentos pela serial e o terceiro
 * usuario.
 *
 * As duas funcoes sao chamadas comuns, nao inline: o decimacao.c chama o
 * crc32_int16() uma vez por amostra dentro da janela medida pelo DWT, e assim o
 * codigo medido continua o de antes (uma chamada por amostra, compilada com -O2).
 *
 * Nao usa a HAL: compila no PC (tests/host/test_crc32.c) e na placa.
 */

#ifndef CRC32_H
#define CRC32_H

#include <stddef.h>
#include <stdint.h>

/* CRC-32 de `n` bytes, continuando `crc` (comece com 0). */
uint32_t crc32_bytes(uint32_t crc, const void *dados, size_t n);

/* Uma amostra int16 em little-endian, continuando `crc` (comece com 0): o mesmo
 * que zlib.crc32(np.asarray(x, '<i2').tobytes()) amostra a amostra, em qualquer
 * maquina. */
uint32_t crc32_int16(uint32_t crc, int16_t v);

#endif /* CRC32_H */
