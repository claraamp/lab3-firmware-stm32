/*
 * lda_modelo.h -- LDA final do detector binario (normal/falha)
 *
 * GERADO por scripts/pipeline/06_export_lda_header.py; nao editar a mao.
 * Modelo:  reports/modelo_final/lda_final.json (exp239, versao_formato 4)
 * SHA-256 do JSON: ee0c3e088366dd6dfb1f9578432aa056af8e7ec918e26055b791d0b3741a8f36
 * Commit do gerador: 7dcd2f7
 *
 * escore = bias + soma(pesos[i] * x[i]), i = 0..LDA_N_FEATURES-1; escore >= 0 -> falha.
 * A padronizacao ja esta incorporada nos pesos (forma dobrada do JSON). As
 * caracteristicas seguem a ordem dos comentarios abaixo, a mesma do JSON.
 * O tamanho do escore nao e confianca: usar so o sinal.
 */

#ifndef LDA_MODELO_H
#define LDA_MODELO_H

#define LDA_N_FEATURES 26
#define LDA_EMPATE_E_FALHA 1

static const float lda_pesos[LDA_N_FEATURES] = {
    -39.9177284f,  /*  0 media_c0 */
    77.170845f,    /*  1 media_c1 */
    206.457108f,   /*  2 media_c2 */
    -136.421967f,  /*  3 media_c3 */
    -659.061157f,  /*  4 media_c4 */
    322.710602f,   /*  5 media_c5 */
    659.47113f,    /*  6 media_c6 */
    -406.196686f,  /*  7 media_c7 */
    -247.69136f,   /*  8 media_c8 */
    116.728661f,   /*  9 media_c9 */
    374.401398f,   /* 10 media_c10 */
    -397.59024f,   /* 11 media_c11 */
    27.4323997f,   /* 12 media_c12 */
    31.5983334f,   /* 13 desvio_c0 */
    -57.186306f,   /* 14 desvio_c1 */
    44.6991501f,   /* 15 desvio_c2 */
    -12.0770941f,  /* 16 desvio_c3 */
    -38.5275345f,  /* 17 desvio_c4 */
    -7.04471207f,  /* 18 desvio_c5 */
    140.934082f,   /* 19 desvio_c6 */
    -196.089828f,  /* 20 desvio_c7 */
    -220.685791f,  /* 21 desvio_c8 */
    17.1172333f,   /* 22 desvio_c9 */
    20.0018158f,   /* 23 desvio_c10 */
    320.123169f,   /* 24 desvio_c11 */
    19.2834435f,   /* 25 desvio_c12 */
};

static const float lda_bias = 2527.68311f;

/* Escore da LDA para um vetor de LDA_N_FEATURES caracteristicas. */
static inline float lda_escore(const float x[LDA_N_FEATURES])
{
    float s = lda_bias;
    for (int i = 0; i < LDA_N_FEATURES; ++i) {
        s += lda_pesos[i] * x[i];
    }
    return s;
}

/* 1 = falha, 0 = normal (escore >= 0 -> falha, como o modelo validado). */
static inline int lda_e_falha(float escore)
{
    return escore >= 0.0f;
}

#endif /* LDA_MODELO_H */
