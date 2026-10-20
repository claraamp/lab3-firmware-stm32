#!/usr/bin/env python3
"""
tolerancias_gravacoes.py -- a decimacao do firmware nos 295 segmentos, contra o Python

Decima as 5 gravacoes inteiras a 51,2 kHz com o codigo C do firmware (o
decimar_gravacao do harness, que usa o mesmo decimacao_bloco() da placa) e compara
cada segmento da particao (splits.json) com o dsp.resample_clip em float64. Mede,
por segmento:

  - o erro do float32 antes de requantizar, relativo ao pico (max|y_c - y_py| /
    max|y_py|), que e o DECIM_TOL_REL_FLOAT do decimacao.h;
  - o que a placa confere (|y_c - int16|, contra 0,5 + tol * pico);
  - depois de requantizar, a maior diferenca para o int16 do Python (tem que ser
    no maximo 1) e quantas amostras diferem (empates; so informacao).

No fim, a tabela por gravacao e a tolerancia pela regra do A4 (10 vezes o maior
erro, para cima na sequencia 1, 2, 5), comparada com a do decimacao.h.

Precisa das gravacoes (data/processed/pcm_raw/*.bin) e do ambiente do repositorio do
Python (numpy, scipy). Rode com o Python do venv dele, da raiz do firmware:

    cmake -S tests/host/harness -B build/harness -G Ninja && cmake --build build/harness
    ~/diagnostico-acustico-motores/.venv/bin/python tests/host/decimacao/tolerancias_gravacoes.py \\
        --repo-python ~/diagnostico-acustico-motores [--recorte build/decimacao_c.f32]

Com --recorte (o arquivo do test_decimacao), confere tambem que o segmento 29 da
gravacao normal decimada inteira da BIT A BIT as saidas do recorte da placa. Com
--markdown arquivo.md, grava a tabela tambem num arquivo.
"""

import argparse
import json
import math
import re
import subprocess
import sys
from pathlib import Path

import numpy as np

RAIZ = Path(__file__).resolve().parents[3]
DECIMACAO_H = RAIZ / "Core" / "Inc" / "decimacao.h"
EXE_PADRAO = RAIZ / "build" / "harness" / "decimar_gravacao"


def para_cima_125(v: float) -> float:
    """O menor valor da sequencia 1, 2, 5 (vezes 10^k) que e >= v."""
    if v <= 0:
        return 0.0
    k = math.floor(math.log10(v))
    for m in (1, 2, 5, 10):
        c = m * 10.0 ** k
        if c >= v * (1 - 1e-12):
            return c
    return 10.0 ** (k + 1)


def macro(txt: str, nome: str) -> float:
    m = re.search(r"#define\s+%s\s+([-0-9.eE+]+)f?" % nome, txt)
    if not m:
        raise SystemExit(f"{nome} nao encontrado no decimacao.h")
    return float(m.group(1))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--repo-python", type=Path, required=True, help="raiz do diagnostico-acustico-motores")
    ap.add_argument("--exe", type=Path, default=EXE_PADRAO, help="o decimar_gravacao do harness")
    ap.add_argument("--saida-dir", type=Path, default=RAIZ / "build" / "gravacoes",
                    help="onde gravar as saidas do C (float32)")
    ap.add_argument("--recorte", type=Path, help="saidas do test_decimacao (segmento 29), para conferir")
    ap.add_argument("--markdown", type=Path, help="grava a tabela tambem neste arquivo")
    a = ap.parse_args()

    repo = a.repo_python.resolve()
    sys.path.insert(0, str(repo / "scripts"))
    import dsp  # noqa: E402  (o modulo do repositorio do Python)

    manifesto = json.loads((repo / "data/processed/pcm_raw/manifest.json").read_text())
    particao = json.loads((repo / "data/processed/splits/splits.json").read_text())
    spec = dsp.design_decimation(51200, 12800)
    tol_rel = macro(DECIMACAO_H.read_text(), "DECIM_TOL_REL_FLOAT")
    if not a.exe.exists():
        raise SystemExit(f"{a.exe} nao existe: compile o harness primeiro")
    a.saida_dir.mkdir(parents=True, exist_ok=True)

    linhas = []
    geral = {"rel": 0.0, "proxy_excesso": -1.0, "dif": 0, "n_dif": 0, "seg": None, "saturadas": 0,
             "n_seg": 0}
    for rotulo, info in manifesto.items():
        arquivo = repo / info["arquivo_pcm"]
        if not arquivo.exists():
            raise SystemExit(f"{arquivo} nao existe: esta maquina tem as gravacoes?")
        saida = a.saida_dir / f"{rotulo}_c.f32"
        subprocess.run([str(a.exe), str(arquivo), str(saida)], check=True)

        x = np.fromfile(arquivo, dtype="<i2")
        y64 = dsp.resample_clip(x.astype(np.float64) / 32767.0, 51200, 12800, spec) * 32767.0
        q64 = np.clip(np.round(y64), -32768, 32767).astype(np.int64)   # o int16 do 02
        yc = np.fromfile(saida, dtype="<f4").astype(np.float64)
        if len(yc) != len(y64):
            raise SystemExit(f"{rotulo}: o C deu {len(yc)} saidas e o Python {len(y64)}")

        segs = [s for s in particao["segmentos"] if s["rotulo"] == rotulo]
        picos, rels, difs, n_difs = [], [], [], []
        for s in segs:
            i0, i1 = s["inicio"], s["fim"]
            r64, c, q = y64[i0:i1], yc[i0:i1], q64[i0:i1]
            pico = float(np.abs(q).max())
            rel = float(np.abs(c - r64).max() / np.abs(r64).max())
            qc = np.round(c)
            sat = int(np.count_nonzero((qc > 32767) | (qc < -32768)))
            qc = np.clip(qc, -32768, 32767).astype(np.int64)
            dif = int(np.abs(qc - q).max())
            n_dif = int(np.count_nonzero(qc != q))
            excesso = float(np.abs(c - q).max() - (0.5 + tol_rel * pico))   # <= 0 passa na placa
            picos.append(pico)
            rels.append(rel)
            difs.append(dif)
            n_difs.append(n_dif)
            geral["n_seg"] += 1
            geral["saturadas"] += sat
            if rel > geral["rel"]:
                geral["rel"], geral["seg"] = rel, s["id"]
            geral["proxy_excesso"] = max(geral["proxy_excesso"], excesso)
            geral["dif"] = max(geral["dif"], dif)
            geral["n_dif"] = max(geral["n_dif"], n_dif)

            if a.recorte is not None and rotulo == "normal" and s["indice"] == 29:
                rec = np.fromfile(a.recorte, dtype="<f4")
                iguais = len(rec) == i1 - i0 and np.array_equal(rec, yc[i0:i1].astype(np.float32))
                print(f"recorte (segmento 29) contra a gravacao inteira: "
                      f"{'bit a bit igual' if iguais else 'DIFERENTE'}")
                if not iguais:
                    return 1

        linhas.append(f"| {rotulo} | {min(picos):.0f}–{max(picos):.0f} | {max(rels):.2e} | "
                      f"{max(difs)} | {max(n_difs)} |")

    tol_regra = para_cima_125(10 * geral["rel"])
    texto = "\n".join([
        "| Gravação | Pico (LSB) | Erro relativo máx. | Diferença máx. depois de requantizar (LSB) "
        "| Amostras diferentes (máx.) |",
        "|---|---:|---:|---:|---:|",
        *linhas,
        "",
        f"- {geral['n_seg']} segmentos; pior erro relativo {geral['rel']:.2e} (segmento {geral['seg']}).",
        f"- Pela regra do A4 (10 vezes, para cima em 1, 2, 5): {tol_regra:g}. "
        f"No decimacao.h: {tol_rel:g} (o pior caso usa {100 * geral['rel'] / tol_rel:.1f} % dela).",
        f"- Depois de requantizar: no máximo {geral['dif']} LSB; até {geral['n_dif']} amostras "
        f"diferentes num segmento (empates); {geral['saturadas']} saturadas.",
        f"- A conferência da placa (|y - int16| ≤ 0,5 + tol × pico) "
        f"{'passaria em todos' if geral['proxy_excesso'] <= 0 else 'REPROVARIA algum segmento'}.",
    ])
    print(texto)
    if a.markdown:
        a.markdown.parent.mkdir(parents=True, exist_ok=True)
        a.markdown.write_text(texto + "\n")

    ok = (geral["rel"] <= tol_rel and geral["dif"] <= 1 and geral["saturadas"] == 0
          and geral["proxy_excesso"] <= 0 and geral["n_seg"] == len(particao["segmentos"]))
    print("TOLERANCIAS OK" if ok else "TOLERANCIAS FALHOU")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
