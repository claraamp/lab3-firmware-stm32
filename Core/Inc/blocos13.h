/*
 * blocos13.h -- blocos 1 a 3 da extracao de caracteristicas na placa
 *
 * Roda a janela (escala e Hann), o espectro de potencia e o banco de Mel do
 * mfcc_estagios.c no primeiro segmento de cada clipe do clipes_demo.h (os mesmos
 * 5 segmentos das referencias do harness, bit a bit), mede os ciclos de cada
 * bloco com o DWT e manda a saida de cada bloco pela serial, em binario, para o receptor no PC
 * (tests/host/blocos13/receber_blocos13.py) comparar com a referencia do Python.
 *
 * Protocolo (linhas de texto terminadas em "\r\n"; os blocos binarios vem logo
 * depois da linha "b13 bin", com exatamente `bytes` bytes, float32 little-endian,
 * quadro a quadro):
 *
 *   b13 inicio modelo=exp239 json=ee0c3e088366 segmentos=5 clock=100MHz
 *   b13 seg i=0 nome=normal_seg0029 entrada_crc32=<crc das 12.800 amostras int16>
 *   b13 ciclos nome=normal_seg0029 janela=<min>/<med>/<max> espectro=... mel=... quadro=... segmento=<ciclos> us=<us>
 *   b13 bin nome=normal_seg0029 estagio=janela linhas=98 colunas=320 bytes=125440
 *   <125440 bytes>
 *   b13 fim nome=normal_seg0029 estagio=janela crc32=1a2b3c4d
 *   ... espectro (98 x 257) e mel (98 x 20) ...
 *   b13 resumo janela=<min>/<med>/<max> espectro=... mel=... quadro=... segmento_med=<ciclos> us=<us>
 *   b13 FIM
 *
 * O CRC-32 e o do zlib (polinomio 0xEDB88320) sobre os bytes do bloco (ou, no
 * entrada_crc32, sobre os 25.600 B da entrada int16).
 *
 * A medicao de ciclos e uma passada separada, sem a serial: cada bloco de cada
 * quadro e medido sozinho, e o segmento inteiro (98 quadros, os tres blocos) e
 * medido de uma vez. Depois vem uma passada por estagio para mandar os dados; a
 * conta e barata (dezenas de ms por segmento), o tempo e todo da serial.
 *
 * A parte sem HAL (blocos13_rodar) compila no PC com -DBLOCOS13_HOST, e o
 * tests/host/blocos13/emular_blocos13.c a usa para gerar a mesma captura sem a
 * placa (com ciclos zerados), o que testa o protocolo e o receptor.
 */

#ifndef BLOCOS13_H
#define BLOCOS13_H

#include <stddef.h>
#include <stdint.h>

typedef void (*b13_enviar_fn)(const void *dados, size_t n);
typedef uint32_t (*b13_ciclos_fn)(void);

/* CRC-32 do zlib, incremental: comece com crc = 0. */
uint32_t b13_crc32(uint32_t crc, const void *dados, size_t n);

/* Roda os 5 segmentos (primeiro de cada clipe) e manda tudo por `enviar`. `ciclos`
 * le o contador (DWT->CYCCNT na placa); `clock_hz` so entra no texto.
 * Devolve 0 se tudo certo, ou o codigo de erro de mfcc_estagios_init(). */
int blocos13_rodar(b13_enviar_fn enviar, b13_ciclos_fn ciclos, uint32_t clock_hz);

#ifndef BLOCOS13_HOST
/* Na placa: liga o DWT e roda tudo pela USART1. */
int blocos13_executar(void);
#endif

#endif /* BLOCOS13_H */
