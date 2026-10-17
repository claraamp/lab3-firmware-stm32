/*
 * clipes_demo.c -- define, uma unica vez, os vetores do clipes_demo.h na flash
 *
 * O clipes_demo.h vem do repositorio do Python (scripts/pipeline/09_export_clips.py
 * -> reports/c_reference/clipes_demo.h, Frente 3C) e so declara os vetores; este
 * e o unico .c que os define. Os outros .c so fazem #include "clipes_demo.h".
 *
 * O que ninguem usa sai no link (--gc-sections): sem o teste dos blocos 1 a 3 e
 * sem a integracao dos clipes, os 358.696 B de audio nao ocupam flash.
 */

#define CLIPES_DEMO_DADOS
#include "clipes_demo.h"
