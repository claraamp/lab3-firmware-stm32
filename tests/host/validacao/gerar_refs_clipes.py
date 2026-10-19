#!/usr/bin/env python3
"""
gerar_refs_clipes.py -- referencias por estagio dos 10 segmentos dos clipes, sem o PCM decimado

O teste do segmento completo na placa (Core/Src/validacao_placa.c) roda os 10
segmentos dos clipes do Core/Inc/clipes_demo.h. Para comparar a captura da placa
(validar_blocos.py --placa) so precisa das referencias desses 10, e nao das 295
(180 MB, que exigem o PCM decimado do repositorio do Python).

Este script tira o audio do proprio clipes_demo.h e calcula as referencias com o
referencias() do 08_export_stage_references.py (dsp.estagios_mfcc e o escore da
LDA em float64), no mesmo formato do 08. Como o 08, confere o escore de cada
segmento contra o escores_referencia.csv e aborta se nao bater: assim o audio do
.h e, comprovadamente, o dos segmentos da particao.

Uso (na raiz do repositorio do firmware, com o do Python ao lado):
    python3 tests/host/validacao/gerar_refs_clipes.py --out build/refs_clipes
    python3 tests/host/validacao/validar_blocos.py --refs build/refs_clipes --placa CAPTURA

Precisa de numpy e scipy (as versoes do requirements.txt do Python).
"""

import argparse
import hashlib
import importlib.util
import json
import re
import subprocess
import sys
from pathlib import Path
from types import SimpleNamespace

import numpy as np

RAIZ = Path(__file__).resolve().parents[3]
PYTHON_PADRAO = RAIZ.parent / "diagnostico-acustico-motores"
CLIPES_H = RAIZ / "Core" / "Inc" / "clipes_demo.h"


def carregar_08(repo: Path):
    caminho = repo / "scripts" / "pipeline" / "08_export_stage_references.py"
    if not caminho.is_file():
        raise SystemExit(f"nao achei {caminho}; use --python-repo")
    spec = importlib.util.spec_from_file_location("export_stage_references", caminho)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def ler_clipes(caminho: Path):
    """Os segmentos dos clipes: [(id, classe, pcm int16 de 12.800 amostras)]."""
    texto = caminho.read_text(encoding="utf-8")
    amostras = int(re.search(r"#define CLIPES_AMOSTRAS_POR_SEGMENTO (\d+)", texto).group(1))
    vetores = {nome: np.array([int(v) for v in corpo.replace("\n", " ").split(",") if v.strip()], dtype=np.int16)
               for nome, corpo in re.findall(r"const int16_t (clipe_\w+)\[\d+\] = \{(.*?)\};", texto, re.S)}
    tabela = re.search(r"const clipe_t clipes\[CLIPES_N_CLIPES\] = \{(.*?)\};", texto, re.S).group(1)
    clipes = [(classe, nome, int(prim), int(n)) for classe, nome, prim, n in
              re.findall(r'\{\s*"([^"]+)",\s*(clipe_\w+),\s*\d+u,\s*(\d+)u,\s*(\d+)u\s*\}', tabela)]
    corpo_segs = re.search(r"const clipe_segmento_t clipes_segmentos\[CLIPES_N_SEGMENTOS\] = \{(.*?)\};",
                           texto, re.S).group(1)
    ids = [int(i) for i in re.findall(r"\{\s*(\d+)u,", corpo_segs)]
    if not clipes or not vetores or not ids:
        raise SystemExit(f"{caminho}: nao reconheci os clipes (o formato do .h mudou?)")
    segs = []
    for classe, nome, prim, n in clipes:
        for k in range(n):
            pcm = vetores[nome][k * amostras:(k + 1) * amostras]
            if pcm.size != amostras:
                raise SystemExit(f"{nome}: o segmento {k} nao tem {amostras} amostras")
            segs.append((ids[prim + k], classe, pcm))
    return segs


def commit_do_python(repo: Path) -> str:
    r = subprocess.run(["git", "-C", str(repo), "rev-parse", "--short", "HEAD"], capture_output=True, text=True)
    return r.stdout.strip() or "sem-git"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--out", type=Path, required=True, help="pasta das referencias (formato do 08)")
    ap.add_argument("--python-repo", type=Path, default=PYTHON_PADRAO)
    ap.add_argument("--clipes", type=Path, default=CLIPES_H)
    args = ap.parse_args()

    m08 = carregar_08(args.python_repo)
    modelo_json = args.python_repo / "reports" / "modelo_final" / "lda_final.json"
    p = json.loads(modelo_json.read_text(encoding="utf-8"))
    pesos = np.asarray(p["dobrada"]["pesos"], dtype=float)
    bias = float(p["dobrada"]["bias"])
    csv_ref = {}
    for linha in (args.python_repo / "reports" / "modelo_final" / "escores_referencia.csv").read_text().splitlines()[1:]:
        campos = linha.split(",")
        csv_ref[int(campos[0])] = float(campos[4])

    args.out.mkdir(parents=True, exist_ok=True)
    pastas = []
    for sid, classe, pcm in ler_clipes(args.clipes):
        e = m08.referencias(SimpleNamespace(id=sid), pcm, pesos, bias)
        ref = csv_ref[sid]
        if abs(e["escore"][0] - ref) > 1e-8 * abs(ref) + 1e-9:
            print(f"Abortado: escore do segmento {sid} ({e['escore'][0]:.6f}) difere do CSV ({ref}): "
                  f"o audio do clipes_demo.h nao e o do segmento da particao")
            return 1
        pasta = f"{classe}_seg{sid:04d}"
        m08.gravar_segmento(args.out / pasta, e, [f"segmento {sid} gravacao {classe}, do {args.clipes.name}"])
        pastas.append(pasta)
        print(f"  {pasta}: escore {e['escore'][0]:.3f} (CSV {ref:.3f})")

    (args.out / "segmentos.txt").write_text("\n".join(pastas) + "\n", encoding="utf-8", newline="\n")
    (args.out / "tolerancias.txt").write_text(m08.texto_tolerancias(), encoding="utf-8", newline="\n")
    sha = hashlib.sha256(modelo_json.read_bytes()).hexdigest()
    (args.out / "modelo.txt").write_text(f"id {p.get('id', '?')}\njson_sha256_12 {sha[:12]}\n"
                                         f"bias {m08.H._f32(bias).rstrip('f')}\n", encoding="utf-8", newline="\n")
    (args.out / "manifest.json").write_text(json.dumps({
        "gerador": "tests/host/validacao/gerar_refs_clipes.py (firmware), com o referencias() do 08",
        "commit": commit_do_python(args.python_repo), "fonte": str(args.clipes.name),
        "modelo": p.get("id"), "modelo_sha256": sha, "segmentos": pastas}, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8", newline="\n")
    print(f"referencias: {args.out} ({len(pastas)} segmentos)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
