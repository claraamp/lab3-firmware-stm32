#!/usr/bin/env python3
"""
receber_blocos13.py -- recebe pela serial a saida dos blocos 1 a 3 da placa e compara com o Python

A placa (Core/Src/blocos13.c) roda a janela, o espectro de potencia e o banco de
Mel nos segmentos de referencia e manda a saida de cada bloco em binario
(protocolo no Core/Inc/blocos13.h). Este script:

1. le a captura da serial (ou de um arquivo salvo antes, com --arquivo);
2. confere modelo, entrada (CRC-32 do int16), tamanho e CRC-32 de cada bloco;
3. grava cada estagio em <saida>/<segmento>/<estagio>.f32 e a captura bruta em
   <saida>/captura.bin;
4. compara cada estagio com a referencia em float64 (reports/c_reference/estagios/
   do repositorio do Python) pela metrica e pela tolerancia ENCADEADA do A4, porque
   a placa roda a cadeia desde o int16, como o modo encadeado do harness;
5. mostra os ciclos de cada bloco medidos pelo DWT.

Uso, com a placa ligada ao adaptador USB-TTL:
    python3 tests/host/blocos13/receber_blocos13.py --porta /dev/ttyUSB0     # Linux
    python  tests/host/blocos13/receber_blocos13.py --porta COM5             # Windows
  e entao aperte o RESET da placa (ou grave o firmware): ela manda tudo ao ligar.

Sem a placa, a partir de uma captura gravada (pela placa ou pelo emular_blocos13):
    python3 tests/host/blocos13/receber_blocos13.py --arquivo build/blocos13/captura.bin

Saida: 0 se todos os estagios ficaram dentro da tolerancia; 1 se algum passou;
2 se a captura ou as referencias nao puderam ser usadas.

Precisa de numpy; com --porta, tambem do pyserial (pip install pyserial).
"""

import argparse
import hashlib
import json
import sys
import time
import zlib
from pathlib import Path

import numpy as np

RAIZ = Path(__file__).resolve().parents[3]
REFS_PADRAO = RAIZ.parent / "diagnostico-acustico-motores" / "reports" / "c_reference" / "estagios"
SAIDA_PADRAO = RAIZ / "build" / "blocos13"
ESTAGIOS = ("janela", "espectro", "mel")
MEDIDAS = ("janela", "espectro", "mel", "quadro")


class ErroCaptura(Exception):
    pass


# --------------------------------------------------------------------------- #
# Leitura                                                                     #
# --------------------------------------------------------------------------- #
class Leitor:
    """readline() e read(n) sobre a serial ou um arquivo, guardando tudo o que foi lido."""

    def __init__(self, fonte, timeout_s: float):
        self.fonte = fonte
        self.timeout_s = timeout_s
        self.bruto = bytearray()

    def _ler(self, n: int) -> bytes:
        partes, falta, ultimo = [], n, time.monotonic()
        while falta > 0:
            b = self.fonte.read(falta)
            if b:
                partes.append(b)
                falta -= len(b)
                ultimo = time.monotonic()
            elif not hasattr(self.fonte, "in_waiting") or time.monotonic() - ultimo > self.timeout_s:
                raise ErroCaptura(f"a captura acabou no meio de um bloco binario (faltaram {falta} B)")
        dados = b"".join(partes)
        self.bruto += dados
        return dados

    def read(self, n: int) -> bytes:
        return self._ler(n)

    def readline(self) -> str:
        linha, ultimo = bytearray(), time.monotonic()
        while not linha.endswith(b"\n"):
            b = self.fonte.read(1)
            if b:
                linha += b
                ultimo = time.monotonic()
            elif not hasattr(self.fonte, "in_waiting"):
                break   # fim do arquivo
            elif time.monotonic() - ultimo > self.timeout_s:
                raise ErroCaptura(f"nada chegou pela serial em {self.timeout_s:.0f} s")
        self.bruto += linha
        return linha.decode("ascii", errors="replace").strip()


def campos(linha: str) -> dict:
    return dict(p.split("=", 1) for p in linha.split()[2:] if "=" in p)


def ler_captura(leitor: Leitor, verboso: bool) -> dict:
    cap = {"segmentos": {}, "ciclos": {}, "resumo": None, "inicio": None}
    # tudo antes do "b13 inicio" (hello, FFT de teste, marco) e ignorado
    while True:
        linha = leitor.readline()
        if linha.startswith("b13 inicio"):
            cap["inicio"] = campos(linha)
            leitor.bruto = bytearray(linha.encode() + b"\r\n")
            print(linha)
            break
        if linha.startswith("b13 ERRO"):
            raise ErroCaptura(f"a placa avisou: {linha}")
        if not linha and not hasattr(leitor.fonte, "in_waiting"):
            raise ErroCaptura("a captura nao tem a linha 'b13 inicio'")
        if verboso and linha:
            print(f"  (ignorada) {linha}")

    while True:
        linha = leitor.readline()
        if not linha:
            if not hasattr(leitor.fonte, "in_waiting"):
                raise ErroCaptura("a captura acabou antes do 'b13 FIM'")
            continue
        if linha == "b13 FIM":
            return cap
        tipo = linha.split()[1] if len(linha.split()) > 1 else ""
        c = campos(linha)
        if tipo == "seg":
            cap["segmentos"][c["nome"]] = {"entrada": c.get("entrada"), "entrada_crc32": c.get("entrada_crc32"),
                                           "dados": {}}
            print(f"{c['nome']}: recebendo...", flush=True)
        elif tipo == "ciclos":
            cap["ciclos"][c["nome"]] = c
        elif tipo == "resumo":
            cap["resumo"] = c
        elif tipo == "bin":
            nome, estagio = c["nome"], c["estagio"]
            linhas, colunas, n_bytes = int(c["linhas"]), int(c["colunas"]), int(c["bytes"])
            if n_bytes != 4 * linhas * colunas:
                raise ErroCaptura(f"{nome}/{estagio}: bytes={n_bytes} nao e 4 x {linhas} x {colunas}")
            t0 = time.monotonic()
            dados = leitor.read(n_bytes)
            fim = ""
            while not fim:
                fim = leitor.readline()
            cf = campos(fim)
            if not fim.startswith("b13 fim") or cf.get("nome") != nome or cf.get("estagio") != estagio:
                raise ErroCaptura(f"{nome}/{estagio}: esperava 'b13 fim', veio {fim[:60]!r}")
            crc = f"{zlib.crc32(dados) & 0xFFFFFFFF:08x}"
            if crc != cf.get("crc32"):
                raise ErroCaptura(f"{nome}/{estagio}: CRC-32 {crc} na chegada, {cf.get('crc32')} na placa "
                                  "(bytes corrompidos ou perdidos na serial)")
            matriz = np.frombuffer(dados, dtype="<f4").reshape(linhas, colunas)
            cap["segmentos"][nome]["dados"][estagio] = matriz
            print(f"  {estagio:<9} {linhas}x{colunas}  {n_bytes} B em {time.monotonic() - t0:.1f} s  crc ok",
                  flush=True)
        elif tipo == "ERRO":
            raise ErroCaptura(f"a placa avisou: {linha}")
        elif verboso:
            print(f"  (linha desconhecida) {linha}")


# --------------------------------------------------------------------------- #
# Comparacao                                                                  #
# --------------------------------------------------------------------------- #
METRICAS = ("rel_quadro", "abs", "rel")


def ler_tolerancias(refs: Path) -> dict:
    """Como o harness: recusa metrica desconhecida e tolerancia nao positiva."""
    tol = {}
    for linha in (refs / "tolerancias.txt").read_text().splitlines():
        p = linha.split()
        if len(p) != 4 or linha.startswith("#"):
            continue
        estagio, metrica = p[0], p[1]
        if metrica not in METRICAS:
            raise ErroCaptura(f"tolerancias.txt: metrica desconhecida {metrica!r} no estagio {estagio} "
                              f"(use {', '.join(METRICAS)})")
        iso, enc = float(p[2]), float(p[3])
        if not (iso > 0.0 and enc > 0.0):
            raise ErroCaptura(f"tolerancias.txt: tolerancia nao positiva no estagio {estagio}")
        tol[estagio] = (metrica, iso, enc)
    faltam = [e for e in ESTAGIOS if e not in tol]
    if faltam:
        raise ErroCaptura(f"tolerancias.txt sem os estagios {', '.join(faltam)}")
    return tol


def metrica_a4(nome: str, c: np.ndarray, r: np.ndarray) -> float:
    """A mesma do harness e do dsp.erro_estagio (so a rel_quadro e usada nos blocos 1 a 3)."""
    d = np.abs(c.astype(np.float64) - r)
    if nome == "rel_quadro":
        escala = np.abs(r).max(axis=1)
        if np.any(escala == 0.0):
            raise ErroCaptura("referencia com um quadro todo em zero: rel_quadro nao se aplica")
        return float((d.max(axis=1) / escala).max())
    if nome == "abs":
        return float(d.max())
    if nome != "rel":
        raise ErroCaptura(f"metrica desconhecida {nome!r} (use {', '.join(METRICAS)})")
    if np.any(r == 0.0):
        raise ErroCaptura("referencia com zero: rel nao se aplica")
    return float((d / np.abs(r)).max())


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    fonte = ap.add_mutually_exclusive_group(required=True)
    fonte.add_argument("--porta", help="porta serial do adaptador (ex.: /dev/ttyUSB0, COM5)")
    fonte.add_argument("--arquivo", type=Path, help="captura gravada antes, em vez da serial")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--timeout", type=float, default=60.0, help="segundos sem dados ate desistir")
    ap.add_argument("--refs", type=Path, default=REFS_PADRAO, help="pasta das referencias por estagio")
    ap.add_argument("--saida", type=Path, default=SAIDA_PADRAO, help="onde gravar a captura e os .f32")
    ap.add_argument("-v", action="store_true", help="mostra as linhas ignoradas")
    args = ap.parse_args()

    refs = args.refs
    if not (refs / "tolerancias.txt").is_file():
        print(f"erro: nao achei {refs / 'tolerancias.txt'}; os dois repositorios estao lado a lado?")
        return 2

    try:
        if args.porta:
            try:
                import serial  # pyserial
            except ImportError:
                print("erro: falta o pyserial (pip install pyserial)")
                return 2
            porta = serial.Serial(args.porta, args.baud, timeout=0.5)
            porta.reset_input_buffer()
            print(f"esperando a placa em {args.porta} a {args.baud} baud: aperte o RESET agora")
            leitor = Leitor(porta, args.timeout)
            try:
                cap = ler_captura(leitor, args.v)
            finally:
                porta.close()
        else:
            with open(args.arquivo, "rb") as f:
                leitor = Leitor(f, args.timeout)
                cap = ler_captura(leitor, args.v)
    except ErroCaptura as e:
        print(f"erro: {e}")
        return 2

    args.saida.mkdir(parents=True, exist_ok=True)
    if not args.arquivo or args.arquivo.resolve() != (args.saida / "captura.bin").resolve():
        (args.saida / "captura.bin").write_bytes(bytes(leitor.bruto))

    # modelo e entrada: a placa tem que ter rodado os mesmos segmentos das referencias
    modelo = dict(l.split(None, 1) for l in (refs / "modelo.txt").read_text().splitlines() if l.strip())
    ini = cap["inicio"]
    if ini.get("modelo") != modelo.get("id") or ini.get("json") != modelo.get("json_sha256_12"):
        print(f"erro: a placa usou o modelo {ini.get('modelo')} ({ini.get('json')}); as referencias sao do "
              f"{modelo.get('id')} ({modelo.get('json_sha256_12')}). Copie o clipes_demo.h do mesmo modelo e grave de novo.")
        return 2
    manifest = json.loads((refs / "manifest.json").read_text())
    try:
        tol = ler_tolerancias(refs)
    except ErroCaptura as e:
        print(f"erro: {e}")
        return 2

    linhas_res, falhas, problemas = [], 0, 0
    print(f"\ncomparacao com as referencias (tolerancia encadeada do A4; modelo {modelo['id']})")
    cab = f"{'segmento':<22} {'estagio':<9} {'metrica':<10} {'metrica_A4':>11} {'tol':>8} {'razao':>7}  situacao"
    print(cab)
    linhas_res.append(cab)
    for nome, seg in cap["segmentos"].items():
        pasta = refs / nome
        if not pasta.is_dir():
            print(f"erro: {nome} nao esta nas referencias")
            problemas += 1
            continue
        # a placa manda o CRC-32 da entrada int16; capturas anteriores a 17/10 (entrada
        # vinda do blocos13_entrada.h) mandavam os 12 primeiros digitos do SHA-256
        bruto_ref = (pasta / "entrada.i16").read_bytes()
        if seg["entrada_crc32"] is not None:
            esperado, veio = f"{zlib.crc32(bruto_ref) & 0xFFFFFFFF:08x}", seg["entrada_crc32"]
        else:
            esperado, veio = hashlib.sha256(bruto_ref).hexdigest()[:12], seg["entrada"]
        if veio != esperado:
            print(f"erro: {nome}: a entrada da placa ({veio}) nao e a das referencias ({esperado})")
            problemas += 1
            continue
        destino = args.saida / nome
        destino.mkdir(exist_ok=True)
        for estagio in ESTAGIOS:
            if estagio not in seg["dados"]:
                print(f"erro: {nome}: a captura nao tem o estagio {estagio}")
                problemas += 1
                continue
            c = seg["dados"][estagio]
            c.astype("<f4").tofile(destino / f"{estagio}.f32")
            r = np.fromfile(pasta / f"{estagio}.f64", dtype="<f8")
            if r.size != c.size:
                print(f"erro: {nome}/{estagio}: {c.size} valores na placa, {r.size} na referencia")
                problemas += 1
                continue
            r = r.reshape(c.shape)
            metrica, _, tol_enc = tol[estagio]
            try:
                m = metrica_a4(metrica, c, r)
            except ErroCaptura as e:
                print(f"erro: {nome}/{estagio}: {e}")
                problemas += 1
                continue
            ok = m <= tol_enc
            falhas += not ok
            l = (f"{nome:<22} {estagio:<9} {metrica:<10} {m:>11.3e} {tol_enc:>8.0e} {100 * m / tol_enc:>6.1f}%  "
                 f"{'OK' if ok else 'FALHOU'}")
            print(l)
            linhas_res.append(l)

    print("\nciclos pelo DWT (min/med/max por quadro; segmento = 98 quadros dos tres blocos)")
    for nome, c in cap["ciclos"].items():
        l = (f"{nome:<22} " + " ".join(f"{k}={c.get(k, '-')}" for k in MEDIDAS)
             + f" segmento={c.get('segmento')} ({c.get('us')} us)")
        print(l)
        linhas_res.append(l)
    if cap["resumo"]:
        r = cap["resumo"]
        l = ("resumo                 " + " ".join(f"{k}={r.get(k, '-')}" for k in MEDIDAS)
             + f" segmento_med={r.get('segmento_med')} ({r.get('us')} us)")
        print(l)
        linhas_res.append(l)
    if all(v.get("segmento") == "0" for v in cap["ciclos"].values()):
        print("(ciclos zerados: captura do emulador no PC, nao da placa)")

    n_seg = len(cap["segmentos"])
    if problemas or n_seg == 0:
        final, codigo = "BLOCOS13 ERRO (captura ou referencias inconsistentes)", 2
    elif falhas:
        final, codigo = f"BLOCOS13 FALHOU ({falhas} estagio(s) fora da tolerancia)", 1
    else:
        final, codigo = f"BLOCOS13 OK ({n_seg} segmentos x {len(ESTAGIOS)} estagios dentro da tolerancia)", 0
    print(f"\n{final}")
    linhas_res.append(final)
    (args.saida / "resultado.txt").write_text(
        f"# referencias: {refs} (commit {manifest.get('commit', '?')})\n" + "\n".join(linhas_res) + "\n")
    print(f"captura, .f32 e resultado.txt em {args.saida}")
    return codigo


if __name__ == "__main__":
    sys.exit(main())
