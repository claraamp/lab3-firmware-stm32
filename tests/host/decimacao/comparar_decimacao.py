#!/usr/bin/env python3
"""
comparar_decimacao.py -- saida da decimacao em C (float32) contra a do Python (float64)

Le as 12.800 saidas em float32 que o test_decimacao grava (antes de requantizar) e
compara com a decimacao em float64 do contrato (docs/contrato_numerico.md do Python,
secao "Decimacao"), calculada aqui sobre o mesmo recorte do clipes_demo.h:

    y[j] = soma_k b[k] * x[4j + 73 - k],   x = int16 / 32767

no instante 4j do segmento (o recorte comeca 75 amostras antes dele). E a conta do
resample_clip (lfilter, descarta as 73 primeiras saidas e fica com uma a cada 4).

Mostra tambem que, em float64, a saida requantizada (np.round, meio-para-par, e
saturacao) da bit a bit o clipe_normal, que e o int16 do 02 que a placa usa como
referencia, e que o CRC-32 dele e o crc32_ref da linha "decim saida" da placa.

Os coeficientes sao os do clipes_demo.h (float32). Com --repo-python, usa os do
dsp.taps_decimacao em float64, que sao os que o resample_clip aplica de fato.

    ./build/harness/test_decimacao build/decimacao_c.f32 > build/decimacao_pc.txt
    python3 tests/host/decimacao/comparar_decimacao.py build/decimacao_c.f32 \\
        [--repo-python ../diagnostico-acustico-motores]

Precisa do numpy.
"""

import argparse
import re
import sys
import zlib
from pathlib import Path

import numpy as np

RAIZ = Path(__file__).resolve().parents[3]
CLIPES_H = RAIZ / "Core" / "Inc" / "clipes_demo.h"
DECIMACAO_H = RAIZ / "Core" / "Inc" / "decimacao.h"
N = 12800
DESCARTAR = 37
FATOR = 4


def vetor(txt: str, nome: str, dtype) -> np.ndarray:
    m = re.search(r"\b%s\[[^\]]*\]\s*=\s*\{(.*?)\};" % re.escape(nome), txt, re.S)
    if not m:
        raise SystemExit(f"{nome} nao encontrado no clipes_demo.h")
    corpo = re.sub(r"/\*.*?\*/", "", m.group(1), flags=re.S)
    return np.array([float(v.strip().rstrip("fF")) for v in corpo.split(",") if v.strip()], dtype=dtype)


def macro(txt: str, nome: str) -> float:
    m = re.search(r"#define\s+%s\s+([-0-9.eE+]+)f?" % nome, txt)
    if not m:
        raise SystemExit(f"{nome} nao encontrado")
    return float(m.group(1))


def taps_python(repo: Path) -> np.ndarray:
    sys.path.insert(0, str(repo / "scripts"))
    import dsp  # noqa: E402  (o modulo do repositorio do Python)

    spec = dsp.design_decimation(51200, 12800)
    return np.asarray(dsp.taps_decimacao(spec, 51200), dtype=np.float64)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("saidas_c", type=Path, help="arquivo float32 do test_decimacao")
    ap.add_argument("--repo-python", type=Path, help="raiz do diagnostico-acustico-motores")
    a = ap.parse_args()

    txt = CLIPES_H.read_text()
    recorte = vetor(txt, "recorte_51200", np.float64)
    fir32 = vetor(txt, "recorte_fir", np.float32)
    ref = vetor(txt, "clipe_normal", np.int64)[:N]
    taps = taps_python(a.repo_python) if a.repo_python else fir32.astype(np.float64)
    origem = "dsp.taps_decimacao (float64)" if a.repo_python else "recorte_fir do clipes_demo.h"

    # float64: convolucao causal (estado zerado), uma saida a cada 4 a partir da 0,
    # descartando as 37 primeiras (a fase 0 do arm_fir_decimate_f32)
    x = recorte / 32767.0
    y64 = np.convolve(x, taps)[: len(x)][::FATOR][DESCARTAR:DESCARTAR + N]
    if len(y64) != N:
        print(f"ERRO: o recorte deu {len(y64)} saidas, esperado {N}")
        return 1

    q64 = np.clip(np.round(y64 * 32767.0), -32768, 32767).astype(np.int64)
    bit_a_bit = int(np.count_nonzero(q64 != ref))
    crc_ref = zlib.crc32(ref.astype("<i2").tobytes())

    yc = np.fromfile(a.saidas_c, dtype="<f4").astype(np.float64)
    if len(yc) != N:
        print(f"ERRO: {a.saidas_c} tem {len(yc)} saidas, esperado {N}")
        return 1

    err = np.abs(yc - y64) * 32767.0          # em LSB
    rel = np.abs(yc - y64).max() / np.abs(y64).max()
    j = int(np.argmax(err))
    # o que a placa mede e a requantizacao dela, com o produto em float32 como no C
    yq = (yc.astype(np.float32) * np.float32(32767.0)).astype(np.float64)
    proxy = np.abs(yq - ref)
    pico = float(np.abs(ref).max())
    qc = np.clip(np.round(yq), -32768, 32767).astype(np.int64)
    n_dif = int(np.count_nonzero(qc != ref))
    empates = np.abs(np.abs(y64 * 32767.0 - ref) - 0.5)

    dh = DECIMACAO_H.read_text()
    tol_rel = macro(dh, "DECIM_TOL_REL_FLOAT")

    print(f"coeficientes: {origem}")
    print(f"float64 requantizado vs clipe_normal: {bit_a_bit} amostras diferentes "
          f"({'bit a bit' if bit_a_bit == 0 else 'NAO bate'}); crc32_ref={crc_ref:08X}")
    print(f"pico do segmento: {np.abs(y64).max():.6f} ({np.abs(y64).max() * 32767:.1f} LSB)")
    print(f"C float32 vs float64: max {err.max():.3e} LSB (j={j}), media {err.mean():.3e} LSB, "
          f"relativo ao pico {rel:.2e}")
    print(f"o que a placa mede (|y_c*32767 - int16|): max {proxy.max():.6f} LSB, "
          f"tolerancia 0,5 + {tol_rel:g} * {pico:.0f} = {0.5 + tol_rel * pico:.6f} LSB")
    print(f"depois de requantizar: {n_dif} amostras diferentes do int16 do Python; "
          f"max {int(np.abs(qc - ref).max())} LSB")
    print(f"amostras a menos de 1e-4 LSB de um empate em float64: {int(np.count_nonzero(empates < 1e-4))}")
    print(f"tolerancia no decimacao.h: DECIM_TOL_REL_FLOAT={tol_rel:g} "
          f"(o maior erro medido usa {100 * rel / tol_rel:.1f} % dela)")

    ok = (bit_a_bit == 0 and rel <= tol_rel and proxy.max() <= 0.5 + tol_rel * pico
          and n_dif <= 20 and int(np.abs(qc - ref).max()) <= 1)
    print("COMPARACAO OK" if ok else "COMPARACAO FALHOU")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
