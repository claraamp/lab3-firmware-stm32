#!/usr/bin/env python3
"""
ler_pacotes.py -- le os pacotes da serial da placa (formato v1) e confere cada um

O formato esta na secao "Pacote da serial" do README e no Core/Inc/pacote_serial.h.
Este script e a referencia de leitura para o painel de novembro:

1. le as linhas da serial (ou de uma captura salva, com --arquivo);
2. so trata como pacote o que comeca com "$CAB," ou "$SEG,"; ignora o resto
   (hello, fft512, marco13, demo ... e o binario do teste dos blocos 1 a 3, em
   que um byte de float pode valer '$'). Se vir as linhas "b13 ..." desse teste,
   avisa que o firmware foi gravado com -DBLOCOS13=ON;
3. confere em cada $CAB e $SEG: checksum, versao, numero e tipo dos campos;
4. confere a coerencia da decisao com os sentidos da decisao de 17/10
   (LDA: falha se escore >= 0; novidade: falha se pontuacao > limiar). A decisao
   que vale e a do campo da placa: o texto tem 3 casas, e na fronteira (escore
   arredondado a 0,000, ou pontuacao arredondada igual ao limiar) o script nao
   recalcula a decisao a partir do texto;
5. avisa pacotes perdidos (salto no contador n) e reinicios da placa (n volta a 0);
6. mostra uma linha por segmento e um resumo; com --csv, grava os segmentos.

Uso, com a placa ligada ao adaptador USB-TTL:
    python3 tests/host/pacote/ler_pacotes.py --porta /dev/ttyUSB0 --segundos 30   # Linux
    python  tests/host/pacote/ler_pacotes.py --porta COM5 --segundos 30           # Windows
  e aperte o RESET da placa para pegar tambem o $CAB.

A partir de uma captura (do picocom, do --salvar ou do test_pacote_serial):
    python3 tests/host/pacote/ler_pacotes.py --arquivo build/pacotes_exemplo.txt

Teste do proprio script, sem placa nem arquivo:
    python3 tests/host/pacote/ler_pacotes.py --autoteste

Saida: 0 se todos os pacotes estao certos; 1 se algum pacote veio com checksum
errado, campo invalido ou decisao incoerente; 2 se nao deu para ler a fonte.

So precisa da biblioteca padrao; com --porta, tambem do pyserial (pip install pyserial).
"""

import argparse
import csv
import sys
import time
from functools import reduce
from pathlib import Path

VERSAO = 1
CASAS = 3
MEIA_UNIDADE = 0.5 * 10.0 ** -CASAS  # erro maximo do arredondamento do texto

CAMPOS_SEG = ("versao", "n", "origem", "clipe", "segmento", "decisao", "disparou",
              "escore_lda", "cod_novidade", "pont_novidade", "lim_novidade", "ciclos")
CAMPOS_CAB = ("versao", "modelo", "json_sha12", "n_features", "clock_mhz")

ORIGENS = {"R": "referencia", "C": "clipe", "A": "ao vivo"}
NOVIDADE = {0: "nao adotado", 1: "pente BPFO (dB)", 2: "pente BPFI (dB)",
            3: "Mahalanobis2 MFCC", 4: "Mahalanobis2 MFCC+pente"}
DISPAROU_LDA, DISPAROU_NOVIDADE = 1, 2


class ErroPacote(Exception):
    pass


def checksum(corpo: str) -> int:
    return reduce(lambda x, c: x ^ ord(c), corpo, 0)


def _int(txt, nome, vazio_ok=False):
    if txt == "" and vazio_ok:
        return None
    try:
        return int(txt)
    except ValueError:
        raise ErroPacote(f"{nome} nao e inteiro: {txt!r}") from None


def _float(txt, nome):
    try:
        v = float(txt)
    except ValueError:
        raise ErroPacote(f"{nome} nao e numero: {txt!r}") from None
    return v


def ler_linha(linha: str):
    """Devolve (tipo, dict) de uma linha que comeca com '$'; ErroPacote se invalida."""
    linha = linha.strip("\r\n")
    if not linha.startswith("$"):
        raise ErroPacote("nao comeca com '$'")
    corpo, sep, cs = linha[1:].rpartition("*")
    if not sep or len(cs) != 2:
        raise ErroPacote("sem '*HH' no fim")
    try:
        cs_lido = int(cs, 16)
    except ValueError:
        raise ErroPacote(f"checksum nao e hexadecimal: {cs!r}") from None
    if checksum(corpo) != cs_lido:
        raise ErroPacote(f"checksum errado (lido {cs_lido:02X}, calculado {checksum(corpo):02X})")

    tipo, *campos = corpo.split(",")
    if tipo == "CAB":
        nomes = CAMPOS_CAB
    elif tipo == "SEG":
        nomes = CAMPOS_SEG
    else:
        raise ErroPacote(f"tipo desconhecido: {tipo!r}")
    if len(campos) != len(nomes):
        raise ErroPacote(f"{tipo} com {len(campos)} campos, esperados {len(nomes)}")
    d = dict(zip(nomes, campos))
    if _int(d["versao"], "versao") != VERSAO:
        raise ErroPacote(f"versao {d['versao']}; este script le a {VERSAO}")
    d["versao"] = VERSAO

    if tipo == "CAB":
        d["n_features"] = _int(d["n_features"], "n_features")
        d["clock_mhz"] = _int(d["clock_mhz"], "clock_mhz")
        return tipo, d

    d["n"] = _int(d["n"], "n")
    if d["origem"] not in ORIGENS:
        raise ErroPacote(f"origem desconhecida: {d['origem']!r}")
    d["clipe"] = _int(d["clipe"], "clipe", vazio_ok=True)
    d["segmento"] = _int(d["segmento"], "segmento")
    if d["decisao"] not in ("N", "F"):
        raise ErroPacote(f"decisao tem que ser N ou F: {d['decisao']!r}")
    d["disparou"] = _int(d["disparou"], "disparou")
    if not 0 <= d["disparou"] <= 3:
        raise ErroPacote(f"disparou fora de 0..3: {d['disparou']}")
    d["escore_lda"] = _float(d["escore_lda"], "escore_lda")
    d["cod_novidade"] = _int(d["cod_novidade"], "cod_novidade")
    if d["cod_novidade"] not in NOVIDADE:
        raise ErroPacote(f"codigo de novidade desconhecido: {d['cod_novidade']}")
    if d["cod_novidade"] == 0:
        if d["pont_novidade"] != "" or d["lim_novidade"] != "":
            raise ErroPacote("novidade nao adotada (codigo 0) com pontuacao ou limiar")
        d["pont_novidade"] = d["lim_novidade"] = None
    else:
        d["pont_novidade"] = _float(d["pont_novidade"], "pont_novidade")
        d["lim_novidade"] = _float(d["lim_novidade"], "lim_novidade")
    d["ciclos"] = _int(d["ciclos"], "ciclos")
    conferir_decisao(d)
    return tipo, d


def conferir_decisao(d):
    """Coerencia entre decisao, disparou e os valores, com os sentidos de 17/10."""
    disp = d["disparou"]
    if (d["decisao"] == "F") != (disp != 0):
        raise ErroPacote(f"decisao {d['decisao']} com disparou={disp}")
    if d["cod_novidade"] == 0 and disp & DISPAROU_NOVIDADE:
        raise ErroPacote("disparou novidade sem detector adotado")

    e = d["escore_lda"]
    if abs(e) > MEIA_UNIDADE:  # fora da fronteira, o sinal do texto e o da placa
        if bool(disp & DISPAROU_LDA) != (e >= 0.0):
            raise ErroPacote(f"escore {e} com o bit da LDA = {disp & DISPAROU_LDA} (falha se >= 0)")
    if d["cod_novidade"] != 0:
        p, lim = d["pont_novidade"], d["lim_novidade"]
        if abs(p - lim) > 2 * MEIA_UNIDADE:
            if bool(disp & DISPAROU_NOVIDADE) != (p > lim):
                raise ErroPacote(f"pontuacao {p} e limiar {lim} com o bit da novidade = "
                                 f"{(disp & DISPAROU_NOVIDADE) >> 1} (falha se pontuacao > limiar)")


def disparou_txt(disp):
    return {0: "nenhum", 1: "LDA", 2: "novidade", 3: "ambos"}[disp]


# --------------------------------------------------------------------------- #
# Leitura da fonte                                                            #
# --------------------------------------------------------------------------- #
def linhas_arquivo(caminho: Path):
    with open(caminho, "rb") as f:
        for bruta in f:
            yield bruta.decode("ascii", errors="replace")


def linhas_porta(porta: str, segundos: float, salvar: Path | None):
    try:
        import serial  # pyserial
    except ImportError:
        raise SystemExit("--porta precisa do pyserial: pip install pyserial") from None
    destino = open(salvar, "wb") if salvar else None
    fim = time.monotonic() + segundos
    try:
        with serial.Serial(porta, 115200, timeout=0.5) as s:
            while time.monotonic() < fim:
                bruta = s.readline()
                if not bruta:
                    continue
                if destino:
                    destino.write(bruta)
                yield bruta.decode("ascii", errors="replace")
    finally:
        if destino:
            destino.close()


# --------------------------------------------------------------------------- #
# Processamento                                                               #
# --------------------------------------------------------------------------- #
PREFIXOS = ("$CAB,", "$SEG,")
AVISO_BLOCOS13 = ("AVISO: a placa esta mandando o teste dos blocos 1 a 3 (linhas \"b13 ...\" e binario):\n"
                  "  o firmware foi gravado com -DBLOCOS13=ON. Os pacotes so saem depois do teste (~100 s).\n"
                  "  Para o firmware normal: cmake --preset Debug -DBLOCOS13=OFF, compile e grave de novo.")


def processar(linhas, mostrar=True, saida_csv: Path | None = None):
    n_ok = n_ruins = n_perdidos = n_reinicios = 0
    viu_blocos13 = False
    n_anterior = None
    cabecalho = None
    segmentos = []
    for num, linha in enumerate(linhas, 1):
        if linha.startswith("b13 ") and not viu_blocos13:
            viu_blocos13 = True
            print(AVISO_BLOCOS13)
        if not linha.startswith(PREFIXOS):
            continue
        try:
            tipo, d = ler_linha(linha)
        except ErroPacote as e:
            n_ruins += 1
            print(f"linha {num}: DESCARTADA ({e}): {linha.strip()}")
            continue
        n_ok += 1
        if tipo == "CAB":
            cabecalho = d
            n_anterior = None
            if mostrar:
                print(f"CAB modelo={d['modelo']} json={d['json_sha12']} "
                      f"n_features={d['n_features']} clock={d['clock_mhz']}MHz")
            continue
        if n_anterior is not None:
            if d["n"] == 0:
                n_reinicios += 1
                print(f"linha {num}: a placa reiniciou (n voltou a 0) sem mandar o $CAB")
            elif d["n"] != n_anterior + 1:
                perdidos = d["n"] - n_anterior - 1
                n_perdidos += max(perdidos, 0)
                print(f"linha {num}: salto no contador ({n_anterior} -> {d['n']})")
        n_anterior = d["n"]
        segmentos.append(d)
        if mostrar:
            nov = "nao adotado" if d["cod_novidade"] == 0 else (
                f"{NOVIDADE[d['cod_novidade']]} {d['pont_novidade']:.3f} "
                f"(limiar {d['lim_novidade']:.3f})")
            clipe = "-" if d["clipe"] is None else d["clipe"]
            print(f"n={d['n']:>5} {ORIGENS[d['origem']]:<10} clipe={clipe} seg={d['segmento']:<5} "
                  f"{'FALHA ' if d['decisao'] == 'F' else 'normal'} disparou={disparou_txt(d['disparou']):<8} "
                  f"escore da LDA={d['escore_lda']:.3f} novidade: {nov} ciclos={d['ciclos']}")

    if saida_csv and segmentos:
        saida_csv.parent.mkdir(parents=True, exist_ok=True)
        with open(saida_csv, "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=CAMPOS_SEG)
            w.writeheader()
            w.writerows(segmentos)

    modelo = cabecalho["modelo"] if cabecalho else "sem $CAB (aperte o RESET da placa)"
    print(f"resumo: {n_ok} pacotes certos, {n_ruins} descartados, {n_perdidos} perdidos, "
          f"{n_reinicios} reinicios; {len(segmentos)} segmentos; modelo {modelo}")
    return n_ok, n_ruins


# --------------------------------------------------------------------------- #
# Autoteste                                                                   #
# --------------------------------------------------------------------------- #
def autoteste():
    # As mesmas linhas exatas do tests/host/test_pacote_serial.c
    boas = [
        "$CAB,1,exp239,ee0c3e088366,26,100*3B",
        "$SEG,1,7,R,2,1043,F,1,231.475,0,,,412*69",
        "$SEG,1,7,R,2,1043,N,0,-1577.959,0,,,412*7A",
        "$SEG,1,7,C,0,1,F,2,-1400.000,3,812.250,61.500,412*6D",
        "$SEG,1,7,A,,0,F,1,10.000,2,3.000,4.000,412*4C",
    ]

    def assinar(corpo):
        return f"${corpo}*{checksum(corpo):02X}"

    fronteira = [  # na fronteira, a decisao da placa vale qualquer que seja o texto
        assinar("SEG,1,1,R,0,5,N,0,0.000,0,,,0"),
        assinar("SEG,1,1,R,0,5,F,1,0.000,0,,,0"),
        assinar("SEG,1,1,C,0,5,N,0,-3.000,3,5.000,5.000,0"),
    ]
    ruins = {
        "checksum": "$SEG,1,7,R,2,1043,F,1,231.475,0,,,412*68",
        "campo a menos": assinar("SEG,1,7,R,2,1043,F,1,231.475,0,,"),
        "versao": assinar("SEG,2,7,R,2,1043,F,1,231.475,0,,,412"),
        "decisao x disparou": assinar("SEG,1,7,R,2,1043,N,1,231.475,0,,,412"),
        "sentido da LDA": assinar("SEG,1,7,R,2,1043,N,0,231.475,0,,,412"),
        "sentido da novidade": assinar("SEG,1,7,C,0,1,N,0,-1400.000,3,812.250,61.500,412"),
        "novidade sem detector": assinar("SEG,1,7,R,2,1043,F,3,231.475,0,,,412"),
        "pontuacao com codigo 0": assinar("SEG,1,7,R,2,1043,F,1,231.475,0,1.000,2.000,412"),
        "origem": assinar("SEG,1,7,X,2,1043,F,1,231.475,0,,,412"),
    }
    falhas = 0
    import io, contextlib
    lixo = ["b13 bin nome=normal_seg0000 estagio=janela linhas=98 colunas=320 bytes=125440",
            "${\x8bA\xf0\x9a x0\x1c=", "$=\xe2\x96<s1"]
    with contextlib.redirect_stdout(io.StringIO()) as saida:
        ok_lixo, ruins_lixo = processar(lixo + boas[:2], mostrar=False)
    if (ok_lixo, ruins_lixo) != (2, 0) or "BLOCOS13=ON" not in saida.getvalue():
        print(f"binario do blocos13: esperado 2 certos, 0 descartados e o aviso; veio {ok_lixo}, {ruins_lixo}")
        falhas += 1
    for l in boas + fronteira:
        try:
            ler_linha(l)
        except ErroPacote as e:
            print(f"recusou uma linha certa ({e}): {l}")
            falhas += 1
    for motivo, l in ruins.items():
        try:
            ler_linha(l)
            print(f"aceitou uma linha errada ({motivo}): {l}")
            falhas += 1
        except ErroPacote:
            pass
    print(f"autoteste: {'FALHOU' if falhas else 'tudo certo'}")
    return 1 if falhas else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    fonte = ap.add_mutually_exclusive_group(required=True)
    fonte.add_argument("--porta", help="porta da serial (ex.: /dev/ttyUSB0, COM5)")
    fonte.add_argument("--arquivo", type=Path, help="captura salva antes")
    fonte.add_argument("--autoteste", action="store_true", help="testa o proprio script")
    ap.add_argument("--segundos", type=float, default=30.0, help="tempo de leitura com --porta")
    ap.add_argument("--salvar", type=Path, help="com --porta, grava a captura bruta neste arquivo")
    ap.add_argument("--csv", type=Path, help="grava os segmentos lidos neste CSV")
    ap.add_argument("--resumo", action="store_true", help="so os erros e o resumo, sem uma linha por segmento")
    args = ap.parse_args()

    if args.autoteste:
        return autoteste()
    try:
        linhas = (linhas_arquivo(args.arquivo) if args.arquivo
                  else linhas_porta(args.porta, args.segundos, args.salvar))
        n_ok, n_ruins = processar(linhas, mostrar=not args.resumo, saida_csv=args.csv)
    except OSError as e:
        print(f"nao deu para ler a fonte: {e}")
        return 2
    if n_ok == 0 and n_ruins == 0:
        print("nenhum pacote na fonte")
        return 2
    return 1 if n_ruins else 0


if __name__ == "__main__":
    sys.exit(main())
