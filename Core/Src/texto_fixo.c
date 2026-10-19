/*
 * texto_fixo.c -- float em texto com casas decimais fixas (ver texto_fixo.h)
 */

#include "texto_fixo.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>

int texto_fixo(char *buf, size_t n, float v, unsigned casas)
{
    static const uint32_t pot10[] = {1u, 10u, 100u, 1000u, 10000u, 100000u, 1000000u};
    if (casas > 6u) {
        casas = 6u;
    }
    if (isnan(v)) {
        return snprintf(buf, n, "nan");
    }
    const float a = fabsf(v) * (float)pot10[casas] + 0.5f;
    if (!(a < 4.0e9f)) {
        return snprintf(buf, n, "%sinf", v < 0.0f ? "-" : "");
    }
    const uint32_t q = (uint32_t)a;
    const char *sinal = (v < 0.0f && q != 0u) ? "-" : "";
    if (casas == 0u) {
        return snprintf(buf, n, "%s%lu", sinal, (unsigned long)q);
    }
    return snprintf(buf, n, "%s%lu.%0*lu", sinal, (unsigned long)(q / pot10[casas]),
                    (int)casas, (unsigned long)(q % pot10[casas]));
}
