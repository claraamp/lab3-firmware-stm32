#!/usr/bin/env python3
"""
validar_blocos.py -- validacao independente, bloco a bloco, da cadeia em C contra o Python

Card "Validar cada bloco da cadeia em C contra a referencia em Python". Compara a
saida de cada estagio do C com a referencia em float64 do Python, nos dois modos
do A4 (isolado e encadeado), e o escore final com o escores_referencia.csv.

Independente do harness: o C so grava as saidas (exportar_saidas_c), e a conta
do erro e a do proprio repositorio do Python (dsp.erro_estagio), com as
tolerancias de la (dsp.ESTAGIOS_PORTE). Para cada estagio, mostra:

  erro_abs    max|c - r|, no pior segmento;
  erro_rel    max|c - r| / max|r| do estagio, no pior segmento;
  A4          a metrica do A4 (rel_quadro, abs ou rel) e a tolerancia;
  situacao    OK ou FALHOU, e o pior segmento.

Com --placa, le tambem a saida do teste do segmento completo na placa
(Core/Src/validacao_placa.c, opcao VALIDACAO_PLACA do CMake): as 26
caracteristicas contra a media e o desvio de referencia (tolerancia encadeada do
A4) e o escore contra o CSV.

Uso (na raiz do repositorio do firmware, com o do Python ao lado):
    cmake -S tests/host/harness -B build/harness -G Ninja && cmake --build build/harness
    ./build/harness/exportar_saidas_c REFS build/saidas_c
    python3 tests/host/validacao/validar_blocos.py --refs REFS --saidas build/saidas_c [--placa CAPTURA] [--md SAIDA.md]

REFS e uma pasta do 08_export_stage_references.py: a versionada (5 segmentos) ou
uma gerada com --segmentos (os 295 da particao, ver docs/validacao-blocos.md).

Saida: 0 se tudo dentro das tolerancias e com as mesmas decisoes do Python; 1 se
algo falhou; 2 se faltou entrada.

Precisa de numpy (e do scipy, que o dsp.py importa).
"""

import argparse
import csv
import json
import re
import sys
from pathlib import Path

import numpy as np

RAIZ = Path(__file__).resolve().parents[3]
PYTHON_PADRAO = RAIZ.parent / "diagnostico-acustico-motores"
ESTAGIOS = ("escala", "janela", "espectro", "mel", "logmel", "mfcc", "media", "desvio", "escore")
MODOS = ("isolado", "encadeado")


class ErroEntrada(Exception):
    pass


# --------------------------------------------------------------------------- #
# Leitura                                                                     #
# --------------------------------------------------------------------------- #
def importar_dsp(repo: Path):
    scripts = repo / "scripts"
    if not (scripts / "dsp.py").is_file():
        raise ErroEntrada(f"nao achei {scripts / 'dsp.py'}; use --python-repo")
    sys.path.insert(0, str(scripts))
    import dsp  # noqa: E402  (o do repositorio do Python)
    return dsp


def ler_refs(pasta: Path) -> dict:
    out = {}
    for linha in (pasta / "indice.txt").read_text(encoding="utf-8").splitlines():
        if not linha.strip() or linha.startswith("#"):
            continue
        nome, tipo, n, m, arquivo = linha.split()
        dtype = {"i16": "<i2", "f64": "<f8"}[tipo]
        out[nome] = np.fromfile(pasta / arquivo, dtype=dtype).reshape(int(n), int(m))
    return out


def ler_c(pasta: Path, modo: str, estagio: str, forma) -> np.ndarray:
    arq = pasta / f"{modo}_{estagio}.f32"
    if not arq.is_file():
        raise ErroEntrada(f"falta {arq} (rode o exportar_saidas_c)")
    c = np.fromfile(arq, dtype="<f4")
    if c.size != int(np.prod(forma)):
        raise ErroEntrada(f"{arq}: {c.size} valores, a referencia tem {int(np.prod(forma))}")
    return c.reshape(forma)


def id_do_segmento(pasta: str) -> int:
    m = re.search(r"_seg(\d+)$", pasta)
    if not m:
        raise ErroEntrada(f"nome de segmento sem o id: {pasta}")
    return int(m.group(1))


def ler_csv(caminho: Path) -> dict:
    return {int(l["segmento"]): (float(l["escore"]), l["previsao"], l["gravacao"])
            for l in csv.DictReader(caminho.open(encoding="utf-8"))}


# --------------------------------------------------------------------------- #
# Comparacao                                                                  #
# --------------------------------------------------------------------------- #
class Pior:
    """Maximos de um estagio num modo, ao longo dos segmentos."""

    def __init__(self):
        self.erro_abs = self.erro_rel = self.a4 = -1.0
        self.seg_a4 = "-"
        self.n = 0

    def somar(self, c, r, metrica, dsp, seg):
        c64 = np.asarray(c, dtype=np.float64)
        d = np.abs(c64 - r)
        escala = np.abs(r).max()
        self.erro_abs = max(self.erro_abs, float(d.max()))
        self.erro_rel = max(self.erro_rel, float(d.max() / escala) if escala > 0 else 0.0)
        a4 = dsp.erro_estagio(c64, r, metrica)
        if a4 > self.a4:
            self.a4, self.seg_a4 = a4, seg
        self.n += 1


def comparar_pc(refs: Path, saidas: Path, dsp, csv_ref: dict):
    tol = {e: (m, ti, te) for e, m, ti, te in dsp.ESTAGIOS_PORTE}
    faltam = [e for e in ESTAGIOS if e not in tol]
    if faltam:
        raise ErroEntrada(f"dsp.ESTAGIOS_PORTE sem os estagios {faltam}")
    pastas = (refs / "segmentos.txt").read_text(encoding="utf-8").split()
    if not pastas:
        raise ErroEntrada("segmentos.txt vazio")
    pior = {(m, e): Pior() for m in MODOS for e in ESTAGIOS}
    segmento_difere = []
    escores = []   # (pasta, escore C encadeado, escore do CSV, decisao do CSV)
    for pasta in pastas:
        r = ler_refs(refs / pasta)
        for modo in MODOS:
            for e in ESTAGIOS:
                c = ler_c(saidas / pasta, modo, e, r[e].shape)
                pior[(modo, e)].somar(c, r[e], tol[e][0], dsp, pasta)
        x_laco = np.concatenate([ler_c(saidas / pasta, "encadeado", e, (13,)) for e in ("media", "desvio")])
        x_seg = ler_c(saidas / pasta, "segmento", "x", (26,))
        if x_seg.tobytes() != x_laco.tobytes():
            segmento_difere.append(pasta)
        sid = id_do_segmento(pasta)
        if sid not in csv_ref:
            raise ErroEntrada(f"segmento {sid} nao esta no escores_referencia.csv")
        escore_c = float(ler_c(saidas / pasta, "encadeado", "escore", (1,))[0])
        escores.append((pasta, escore_c, csv_ref[sid][0], csv_ref[sid][1]))
    return pastas, tol, pior, segmento_difere, escores


def ler_placa(caminho: Path) -> tuple[dict, list]:
    inicio, segs = None, []
    for bruta in caminho.read_bytes().splitlines():
        linha = bruta.decode("ascii", errors="replace").strip()
        if linha.startswith("val inicio"):
            inicio = dict(p.split("=", 1) for p in linha.split()[2:] if "=" in p)
            segs = []   # a placa reiniciou: vale a ultima execucao
        elif linha.startswith("val ERRO"):
            raise ErroEntrada(f"a placa avisou: {linha}")
        elif linha.startswith("val seg"):
            c = dict(p.split("=", 1) for p in linha.split()[2:] if "=" in p)
            x = np.array([int(h, 16) for h in c["x"].split(",")], dtype="<u4").view("<f4")
            if x.size != 26:
                raise ErroEntrada(f"linha com {x.size} caracteristicas: {linha[:60]}")
            escore = np.array([int(c["escore"], 16)], dtype="<u4").view("<f4")[0]
            segs.append({"id": int(c["id"]), "classe": c["classe"], "ciclos": int(c["ciclos"]),
                         "x": x, "escore": float(escore)})
    if inicio is None or not segs:
        raise ErroEntrada(f"{caminho} nao tem as linhas 'val inicio' e 'val seg' do validacao_placa")
    return inicio, segs


def comparar_placa(caminho: Path, refs: Path, dsp, csv_ref: dict, tol: dict, modelo: dict):
    inicio, segs = ler_placa(caminho)
    if inicio.get("modelo") != modelo.get("id") or inicio.get("json") != modelo.get("json_sha256_12"):
        raise ErroEntrada(f"a placa usou o modelo {inicio.get('modelo')} ({inicio.get('json')}); "
                          f"as referencias sao do {modelo.get('id')} ({modelo.get('json_sha256_12')})")
    pastas = {id_do_segmento(p): p for p in (refs / "segmentos.txt").read_text(encoding="utf-8").split()}
    linhas = []
    for s in segs:
        if s["id"] not in pastas:
            raise ErroEntrada(f"segmento {s['id']} da placa nao esta nas referencias {refs}")
        r = ler_refs(refs / pastas[s["id"]])
        a4 = {e: dsp.erro_estagio(v, r[e], tol[e][0])
              for e, v in (("media", s["x"][:13]), ("desvio", s["x"][13:]), ("escore", [s["escore"]]))}
        ok = {e: a4[e] <= tol[e][2] for e in a4}
        csv_escore, csv_prev, _ = csv_ref[s["id"]]
        dec_placa = "falha" if s["escore"] >= 0.0 else "normal"
        dec_csv = "normal" if csv_prev == "normal" else "falha"
        linhas.append({**s, "pasta": pastas[s["id"]], "a4": a4, "ok": ok,
                       "dif_csv": abs(s["escore"] - csv_escore), "csv": csv_escore,
                       "dec_ok": dec_placa == dec_csv})
    return inicio, linhas


# --------------------------------------------------------------------------- #
# Relatorio                                                                   #
# --------------------------------------------------------------------------- #
SOBRESCRITO = str.maketrans("-0123456789", "⁻⁰¹²³⁴⁵⁶⁷⁸⁹")


def cientifico(v: float, md: bool) -> str:
    if v == 0.0:
        return "0"
    if not md:
        return f"{v:.2e}"
    mant, exp = f"{v:.1e}".split("e")
    return f"{mant.replace('.', ',')} × 10{str(int(exp)).translate(SOBRESCRITO)}"


def pct(v: float, md: bool) -> str:
    s = f"{100 * v:.1f}%"
    return s.replace(".", ",") if md else s


def tabela_pc(pior, tol, modo, md):
    cab = ["Estágio", "Métrica", "Erro abs.", "Erro rel.", "Métrica A4", "Tolerância", "% da tol.",
           "Pior segmento", "Situação"]
    linhas = []
    falhas = 0
    for e in ESTAGIOS:
        p = pior[(modo, e)]
        t = tol[e][1] if modo == "isolado" else tol[e][2]
        ok = p.a4 <= t
        falhas += not ok
        linhas.append([e, tol[e][0], cientifico(p.erro_abs, md), cientifico(p.erro_rel, md),
                       cientifico(p.a4, md), cientifico(t, md), pct(p.a4 / t, md), p.seg_a4,
                       "OK" if ok else "**FALHOU**" if md else "FALHOU"])
    return cab, linhas, falhas


def imprimir(cab, linhas, md, saida):
    if md:
        saida.append("| " + " | ".join(cab) + " |")
        saida.append("|" + "|".join("---:" if i in (2, 3, 4, 5, 6) else "---" for i in range(len(cab))) + "|")
        saida.extend("| " + " | ".join(str(c) for c in l) + " |" for l in linhas)
    else:
        larg = [max(len(str(x)) for x in col) for col in zip(cab, *linhas)]
        saida.append("  ".join(str(c).ljust(w) for c, w in zip(cab, larg)))
        saida.extend("  ".join(str(c).ljust(w) for c, w in zip(l, larg)) for l in linhas)


def relatorio(md, pastas, tol, pior, segmento_difere, escores, placa):
    s, falhas = [], 0
    for modo in MODOS:
        s.append(f"\n{'### ' if md else ''}Modo {modo} ({len(pastas)} segmentos)\n")
        cab, linhas, f = tabela_pc(pior, tol, modo, md)
        falhas += f
        imprimir(cab, linhas, md, s)

    s.append(f"\n{'### ' if md else ''}Chamada única e escore final\n")
    if segmento_difere:
        falhas += 1
        s.append(f"- `mfcc_segmento()` DIFERE do laço estágio a estágio em {len(segmento_difere)} "
                 f"segmentos (ex.: {segmento_difere[0]}).")
    else:
        s.append(f"- `mfcc_segmento()`: igual, bit a bit, ao laço estágio a estágio nos {len(pastas)} segmentos.")
    dif = np.array([abs(c - r) for _, c, r, _ in escores])
    rel = np.array([abs(c - r) / abs(r) for _, c, r, _ in escores])
    dec_iguais = sum((c >= 0.0) == (prev != "normal") for _, c, _, prev in escores)
    menor = min(abs(r) for _, _, r, _ in escores)
    i = int(dif.argmax())
    if dec_iguais != len(escores):
        falhas += 1
    s.append(f"- Escore da cadeia em C (do `int16` à LDA) contra o `escores_referencia.csv`: "
             f"diferença máxima {dif.max():.4f} ({escores[i][0]}), mediana {np.median(dif):.4f}; "
             f"erro relativo máximo {cientifico(rel.max(), md)}.")
    s.append(f"- Decisões iguais às do CSV: {dec_iguais}/{len(escores)}. Menor |escore| do CSV: {menor:.1f}, "
             f"{menor / dif.max():.0f} vezes a maior diferença.")

    if placa is not None:
        inicio, linhas = placa
        s.append(f"\n{'### ' if md else ''}Placa: segmento completo ({len(linhas)} segmentos, "
                 f"clock {inicio.get('clock', '?')})\n")
        cab = ["Segmento", "Média (abs)", "Desvio (rel)", "Escore (rel)", "Escore − CSV", "Decisão",
               "Ciclos", "Situação"]
        tab = []
        for l in linhas:
            ok = all(l["ok"].values()) and l["dec_ok"]
            falhas += not ok
            tab.append([l["pasta"], cientifico(l["a4"]["media"], md), cientifico(l["a4"]["desvio"], md),
                        cientifico(l["a4"]["escore"], md), f"{l['dif_csv']:.4f}",
                        "igual" if l["dec_ok"] else "DIFERENTE", l["ciclos"], "OK" if ok else "FALHOU"])
        imprimir(cab, tab, md, s)
        for e in ("media", "desvio", "escore"):
            m = max(l["a4"][e] for l in linhas)
            s.append(f"- {e}: maior métrica A4 {cientifico(m, md)}, {pct(m / tol[e][2], md)} da tolerância "
                     f"encadeada ({cientifico(tol[e][2], md)}).")
        ciclos = sorted(l["ciclos"] for l in linhas)
        if ciclos[-1] == 0:
            s.append("- Ciclos zerados: saída do emulador no PC, não da placa.")
        else:
            mhz = int(re.sub(r"\D", "", inicio.get("clock", "100")) or 100)
            med = ciclos[(len(ciclos) - 1) // 2]
            s.append(f"- Segmento de 1 s: mediana de {med} ciclos ({med / mhz / 1000:.1f} ms a {mhz} MHz), "
                     f"de {ciclos[0]} a {ciclos[-1]}.")
    s.append(f"\n{'**' if md else ''}{'VALIDACAO FALHOU' if falhas else 'VALIDACAO OK'}"
             f"{'**' if md else ''}" + ("" if not falhas else f" ({falhas} item(ns) fora)"))
    return s, falhas


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--refs", type=Path, required=True, help="pasta do 08 (referencias por estagio)")
    ap.add_argument("--saidas", type=Path, required=True, help="pasta do exportar_saidas_c")
    ap.add_argument("--python-repo", type=Path, default=PYTHON_PADRAO)
    ap.add_argument("--escores", type=Path, default=None, help="padrao: reports/modelo_final/escores_referencia.csv")
    ap.add_argument("--placa", type=Path, help="saida do validacao_placa (captura da serial)")
    ap.add_argument("--md", type=Path, help="grava as tabelas em Markdown neste arquivo")
    args = ap.parse_args()

    try:
        dsp = importar_dsp(args.python_repo)
        escores_csv = args.escores or args.python_repo / "reports" / "modelo_final" / "escores_referencia.csv"
        csv_ref = ler_csv(escores_csv)
        modelo = dict(l.split(None, 1) for l in (args.refs / "modelo.txt").read_text().splitlines() if l.strip())
        pastas, tol, pior, difere, escores = comparar_pc(args.refs, args.saidas, dsp, csv_ref)
        placa = comparar_placa(args.placa, args.refs, dsp, csv_ref, tol, modelo) if args.placa else None
    except (ErroEntrada, OSError, ValueError, KeyError) as e:
        print(f"erro: {e}")
        return 2

    manifest = json.loads((args.refs / "manifest.json").read_text(encoding="utf-8"))
    print(f"referencias: {args.refs} (commit {manifest.get('commit', '?')}, modelo {modelo.get('id')})")
    texto, falhas = relatorio(False, pastas, tol, pior, difere, escores, placa)
    print("\n".join(texto))
    if args.md:
        md, _ = relatorio(True, pastas, tol, pior, difere, escores, placa)
        args.md.parent.mkdir(parents=True, exist_ok=True)
        args.md.write_text("\n".join(md).lstrip("\n") + "\n", encoding="utf-8")
        print(f"tabelas em {args.md}")
    return 1 if falhas else 0


if __name__ == "__main__":
    sys.exit(main())
