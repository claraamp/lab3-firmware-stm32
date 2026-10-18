/*
 * emular_validacao_placa.c -- gera no PC a mesma saida que a placa manda no teste
 * do segmento completo (Core/Src/validacao_placa.c), com a CMSIS-DSP generica e os
 * ciclos zerados. Serve para testar o protocolo e o validar_blocos.py sem a placa:
 *
 *   ./build/harness/emular_validacao_placa > build/validacao_pc.txt
 *   python3 tests/host/validacao/validar_blocos.py ... --placa build/validacao_pc.txt
 */

#include <stdio.h>

#include "validacao_placa.h"

static void enviar_stdout(const char *texto, size_t n)
{
    fwrite(texto, 1, n, stdout);
}

static uint32_t sem_ciclos(void)
{
    return 0u;
}

int main(void)
{
    const int st = validacao_placa_rodar(enviar_stdout, sem_ciclos, 100000000u);
    if (st != 0) {
        fprintf(stderr, "erro: validacao_placa_rodar devolveu %d\n", st);
        return 1;
    }
    return 0;
}
