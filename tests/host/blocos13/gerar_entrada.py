#!/usr/bin/env python3
"""
gerar_entrada.py -- gera Core/Inc/blocos13_entrada.h a partir das referencias por estagio

Copia as 12.800 amostras int16 (entrada.i16) de cada segmento das referencias do
harness (diagnostico-acustico-motores, reports/c_reference/estagios/) para um .h
com `const int16_t`, que a placa le da flash no teste dos blocos 1 a 3. Assim a
placa roda exatamente os mesmos segmentos com que o harness e o receptor comparam.

Guarda tambem o modelo das referencias (modelo.txt) e os 12 primeiros digitos do
SHA-256 de cada entrada.i16 (manifest.json), que a placa repete na serial: o
receptor recusa uma captura feita com outra entrada.

Uso (na raiz do firmware, com os dois repositorios lado a lado):
    python3 tests/host/blocos13/gerar_entrada.py
    python3 tests/host/blocos13/gerar_entrada.py --segmentos normal_seg0029 bpfo_0.3mm_seg0206

Cada segmento ocupa 25.600 B de flash; os 5 juntos, 125 KB.
"""

import argparse
import hashlib
import json
import sys
from pathlib import Path

N_AMOSTRAS = 12800
RAIZ = Path(__file__).resolve().parents[3]
REFS_PADRAO = RAIZ.parent / "diagnostico-acustico-motores" / "reports" / "c_reference" / "estagios"
SAIDA_PADRAO = RAIZ / "Core" / "Inc" / "blocos13_entrada.h"


def ler_modelo(refs: Path) -> dict:
    modelo = {}
    for linha in (refs / "modelo.txt").read_text().splitlines():
        partes = linha.split()
        if len(partes) == 2:
            modelo[partes[0]] = partes[1]
    for chave in ("id", "json_sha256_12"):
        if chave not in modelo:
            sys.exit(f"erro: {refs / 'modelo.txt'} sem a chave {chave}")
    return modelo


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--refs", type=Path, default=REFS_PADRAO, help="pasta das referencias por estagio")
    ap.add_argument("--saida", type=Path, default=SAIDA_PADRAO, help="o .h gerado")
    ap.add_argument("--segmentos", nargs="+", help="so estes segmentos (padrao: todos do segmentos.txt)")
    args = ap.parse_args()

    refs = args.refs
    if not (refs / "segmentos.txt").is_file():
        sys.exit(f"erro: nao achei {refs / 'segmentos.txt'}; os dois repositorios estao lado a lado?")
    todos = [s for s in (refs / "segmentos.txt").read_text().split() if s]
    segmentos = args.segmentos or todos
    desconhecidos = [s for s in segmentos if s not in todos]
    if desconhecidos:
        sys.exit(f"erro: segmentos fora do segmentos.txt: {', '.join(desconhecidos)}")

    modelo = ler_modelo(refs)
    manifest = json.loads((refs / "manifest.json").read_text())
    shas = manifest.get("arquivos_sha256", {})

    blocos, nomes, hashes = [], [], []
    for nome in segmentos:
        bruto = (refs / nome / "entrada.i16").read_bytes()
        if len(bruto) != 2 * N_AMOSTRAS:
            sys.exit(f"erro: {nome}/entrada.i16 tem {len(bruto)} B; esperava {2 * N_AMOSTRAS}")
        sha = hashlib.sha256(bruto).hexdigest()
        esperado = shas.get(f"{nome}/entrada.i16")
        if esperado and esperado != sha:
            sys.exit(f"erro: {nome}/entrada.i16 nao bate com o manifest.json")
        amostras = [int.from_bytes(bruto[i:i + 2], "little", signed=True) for i in range(0, len(bruto), 2)]
        linhas = []
        for i in range(0, N_AMOSTRAS, 16):
            linhas.append("        " + ", ".join(str(v) for v in amostras[i:i + 16]) + ",")
        blocos.append("    { /* " + nome + " */\n" + "\n".join(linhas) + "\n    },")
        nomes.append(nome)
        hashes.append(sha[:12])

    n = len(nomes)
    texto = f"""/*
 * blocos13_entrada.h -- segmentos de referencia para o teste dos blocos 1 a 3 na placa
 *
 * GERADO por tests/host/blocos13/gerar_entrada.py; nao editar a mao.
 * Origem: diagnostico-acustico-motores, reports/c_reference/estagios/ (gerador
 * {manifest.get("gerador", "?")}, commit {manifest.get("commit", "?")}), modelo {modelo["id"]}
 * (json {modelo["json_sha256_12"]}). As amostras sao as de entrada.i16 de cada segmento.
 */

#ifndef BLOCOS13_ENTRADA_H
#define BLOCOS13_ENTRADA_H

#include <stdint.h>

#define B13_N_SEGMENTOS {n}
#define B13_MODELO_ID "{modelo["id"]}"
#define B13_MODELO_JSON "{modelo["json_sha256_12"]}"

static const char *const b13_nomes[B13_N_SEGMENTOS] = {{
{chr(10).join(f'    "{s}",' for s in nomes)}
}};

/* 12 primeiros digitos do SHA-256 de cada entrada.i16 */
static const char *const b13_entrada_sha[B13_N_SEGMENTOS] = {{
{chr(10).join(f'    "{h}",' for h in hashes)}
}};

static const int16_t b13_entrada[B13_N_SEGMENTOS][{N_AMOSTRAS}] = {{
{chr(10).join(blocos)}
}};

#endif /* BLOCOS13_ENTRADA_H */
"""
    args.saida.write_text(texto, newline="\n")
    print(f"{args.saida}: {n} segmento(s), {n * 2 * N_AMOSTRAS} B de flash, modelo {modelo['id']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
