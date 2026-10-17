/*
 * emular_blocos13.c -- gera no PC a mesma captura que a placa manda no teste dos blocos 1 a 3
 *
 * Roda o blocos13_rodar() com a CMSIS-DSP generica e escreve tudo num arquivo,
 * byte a byte como sairia da USART1, mas com os ciclos zerados (o PC nao tem DWT).
 * Serve para testar o protocolo e o receptor sem a placa:
 *
 *   ./build/harness/emular_blocos13 build/captura_pc.bin
 *   python3 tests/host/blocos13/receber_blocos13.py --arquivo build/captura_pc.bin
 */

#include <stdio.h>

#include "blocos13.h"

static FILE *saida;

static void enviar_arquivo(const void *dados, size_t n)
{
    fwrite(dados, 1, n, saida);
}

static uint32_t sem_ciclos(void)
{
    return 0u;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "uso: %s CAPTURA.bin\n", argv[0]);
        return 2;
    }
    saida = fopen(argv[1], "wb");
    if (!saida) {
        fprintf(stderr, "erro: nao abri %s\n", argv[1]);
        return 2;
    }
    const int st = blocos13_rodar(enviar_arquivo, sem_ciclos, 100000000u);
    fclose(saida);
    if (st != 0) {
        fprintf(stderr, "erro: blocos13_rodar devolveu %d\n", st);
        return 1;
    }
    printf("captura em %s\n", argv[1]);
    return 0;
}
