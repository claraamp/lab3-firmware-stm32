#!/usr/bin/env python3
"""
enviar_segmento.py -- manda segmentos dos clipes para a placa no modo de recepcao

Script MINIMO, para testar o firmware (o script completo, com os 295 do Jung e os
40 do HUSTmotor, e o da Amanda). O formato do quadro e o dialogo estao no README,
secao "Recepcao de segmentos pela serial".

Le os 10 segmentos dos clipes do Core/Inc/clipes_demo.h (os mesmos da flash), pede
o modo de recepcao (RECEBER a cada 1 s ate o primeiro $PED), manda os segmentos e
confere cada resposta:
  - com o quadro certo, o $SEG tem de vir com origem S, grupo e id do quadro, e o
    escore (escore_hex da linha rx) tem de ser BIT A BIT o da linha ref que a placa
    mandou para o mesmo segmento da flash, com a mesma decisao;
  - com --estragar, o quadro sai errado de proposito e a placa tem de responder
    $ERR com o motivo certo (crc, versao, formato, tamanho, truncado); com
    --estragar magica, a placa tem de ignorar o quadro (nenhuma resposta) e aceitar
    o quadro certo mandado logo depois.

Uso (placa gravada com o firmware da recepcao, sem o picocom aberto):
    python3 tests/host/recepcao/enviar_segmento.py --porta /dev/ttyUSB0 --seg 29
    python3 tests/host/recepcao/enviar_segmento.py --porta /dev/ttyUSB0 --todos
    python3 tests/host/recepcao/enviar_segmento.py --porta /dev/ttyUSB0 --todos --erros --seg 88
    python3 tests/host/recepcao/enviar_segmento.py --porta /dev/ttyUSB0 --seg 88 --estragar crc
    python3 tests/host/recepcao/enviar_segmento.py --autoteste      # sem a placa

A placa fica no modo de recepcao ate o reset. Ao abrir a porta, o script escuta 3 s:
se a placa fala (a demonstracao), ele pede o modo; se esta calada (ja no modo, de uma
execucao anterior), ele manda direto, mas sem as linhas ref (que a placa so manda ao
entrar no modo) o escore nao e conferido bit a bit. Com alguns adaptadores, abrir ou
fechar a porta reinicia a placa (DTR/RTS ligado ao NRST): por isso o --erros roda os
seis erros na mesma sessao, e o melhor e rodar tudo de uma vez (--todos --erros).
Se chegar uma linha da demonstracao no meio dos quadros, o script para e avisa.
"""

import argparse
import re
import struct
import sys
import time
import zlib
from pathlib import Path

RAIZ = Path(__file__).resolve().parents[3]
CLIPES_H = RAIZ / "Core" / "Inc" / "clipes_demo.h"

N_AMOSTRAS = 12800
TAM_QUADRO = 18 + 2 * N_AMOSTRAS
GRUPO_CLIPES = 9   # grupo usado para os clipes da flash (o do Jung e do HUSTmotor e da Amanda)
ESTRAGOS = ("crc", "versao", "formato", "tamanho", "truncado", "magica")


# --------------------------------------------------------------------------- #
# Clipes da flash                                                             #
# --------------------------------------------------------------------------- #
def carregar_clipes(caminho=CLIPES_H):
    """Devolve {id do segmento: (classe, k, amostras int16 como lista)}, na ordem da flash."""
    texto = caminho.read_text()
    vetores = {}
    for m in re.finditer(r"const int16_t (clipe_\w+)\[(\d+)\] = \{(.*?)\};", texto, re.S):
        valores = [int(v) for v in m.group(3).replace("\n", " ").split(",") if v.strip()]
        if len(valores) != int(m.group(2)):
            raise SystemExit(f"{m.group(1)}: {len(valores)} amostras, esperadas {m.group(2)}")
        vetores[m.group(1)] = valores
    m = re.search(r"const clipe_t clipes\[CLIPES_N_CLIPES\] = \{(.*?)\n\};", texto, re.S)
    clipes = [(c, v, int(p)) for c, v, p in
              re.findall(r'\{ "([^"]+)", (clipe_\w+), \d+u, (\d+)u, \d+u \}', m.group(1))]
    m = re.search(r"const clipe_segmento_t clipes_segmentos\[CLIPES_N_SEGMENTOS\] = \{(.*?)\n\};", texto, re.S)
    segs = [(int(i), int(c)) for i, c in re.findall(r"\{ (\d+)u, (\d+)u, \d+u, [-\d.e]+f \}", m.group(1))]
    if len(clipes) != 5 or len(segs) != 10:
        raise SystemExit(f"clipes_demo.h: {len(clipes)} clipes e {len(segs)} segmentos, esperados 5 e 10")
    saida = {}
    for s, (id_seg, c) in enumerate(segs):
        classe, vetor, primeiro = clipes[c]
        k = s - primeiro
        saida[id_seg] = (classe, k, vetores[vetor][k * N_AMOSTRAS:(k + 1) * N_AMOSTRAS])
    return saida


# --------------------------------------------------------------------------- #
# Quadro                                                                      #
# --------------------------------------------------------------------------- #
def montar_quadro(amostras, grupo, id_seg, versao=1, formato=1, n_amostras=N_AMOSTRAS):
    corpo = struct.pack(f"<{len(amostras)}h", *amostras)
    cab = struct.pack("<BBHIH", versao, formato, grupo, id_seg, n_amostras)
    return b"QSEG" + cab + struct.pack("<I", zlib.crc32(cab + corpo)) + corpo


def estragar(quadro, como):
    q = bytearray(quadro)
    if como == "crc":
        q[18 + 6001] ^= 0x01            # um bit no corpo
    elif como == "versao":
        q[4] = 2
    elif como == "formato":
        q[5] = 2                        # 51,2 kHz: reservado, ainda nao aceito
    elif como == "tamanho":
        q[12:14] = struct.pack("<H", N_AMOSTRAS - 1)
    elif como == "magica":
        q[3] = ord("H")
    elif como == "truncado":
        return bytes(q[:TAM_QUADRO // 2])
    return bytes(q)


# --------------------------------------------------------------------------- #
# Placa                                                                       #
# --------------------------------------------------------------------------- #
class Placa:
    def __init__(self, porta, baud, log):
        import serial  # pyserial
        self.s = serial.Serial(porta, baud, timeout=0.05)
        self.baud = baud
        self.log = open(log, "w") if log else None
        self.refs = {}

    def linha(self, ate):
        """Proxima linha de texto ate o instante `ate` (monotonic), ou None."""
        while time.monotonic() < ate:
            bruta = self.s.readline()
            if not bruta:
                continue
            txt = bruta.decode("ascii", errors="replace").rstrip("\r\n")
            if self.log:
                self.log.write(txt + "\n")
            m = re.match(r"ref seg=(\d+) .*escore_hex=(0x[0-9A-F]{8})", txt)
            if m:
                self.refs[int(m.group(1))] = m.group(2)
            return txt
        return None

    def entrar_no_modo(self, ja_no_modo):
        if ja_no_modo:
            print("supondo a placa ja no modo de recepcao (depois de um $PED anterior)")
            return
        # A demonstracao fala a cada ~2,3 s; no modo, esperando um quadro, a placa
        # fica calada. 3 s de silencio: ja esta no modo (de uma execucao anterior,
        # sem reset), e o script manda direto, sem as linhas ref.
        fim = time.monotonic() + 3.0
        falou = False
        while time.monotonic() < fim:
            if self.linha(fim) is not None:
                falou = True
                break
        if not falou:
            print("a placa esta calada ha 3 s: ja no modo de recepcao (sem as linhas ref; "
                  "para a conferencia bit a bit, aperte o RESET e rode de novo)")
            return
        print("pedindo o modo de recepcao (RECEBER a cada 1 s)...")
        fim = time.monotonic() + 30.0
        proximo = 0.0
        while time.monotonic() < fim:
            if time.monotonic() >= proximo:
                self.s.write(b"RECEBER\n")
                proximo = time.monotonic() + 1.0
            txt = self.linha(time.monotonic() + 0.1)
            if txt is None:
                continue
            if txt.startswith(("recepcao", "ref ", "$CAB")):
                print(f"  {txt}")
            if txt.startswith("recepcao recusada"):
                raise SystemExit("a placa recusou: as verificacoes do boot falharam")
            if txt.startswith("$PED"):
                print(f"  {txt}")
                return
        raise SystemExit("a placa nao entrou no modo de recepcao em 30 s (firmware gravado? picocom fechado?)")

    def mandar(self, quadro):
        """Manda o quadro e devolve (linha rx, $SEG ou $ERR) e espera o $PED seguinte.
        Sem resposta no tempo: (None, None)."""
        self.s.write(quadro)
        espera = len(quadro) * 10.0 / self.baud + 2.0
        fim = time.monotonic() + espera
        rx = resp = None
        while time.monotonic() < fim:
            txt = self.linha(fim)
            if txt is None:
                break
            if txt.startswith(("demo ", "ponta ", "hello")) or re.match(r"\$SEG,\d+,\d+,[^S],", txt):
                raise SystemExit(f"a placa nao esta no modo de recepcao (reiniciou?): chegou '{txt}'.\n"
                                 "Se ela reinicia ao abrir ou fechar a porta, rode todos os testes numa "
                                 "execucao so (--todos, --erros).")
            if txt.startswith("rx "):
                rx = txt
            elif txt.startswith(("$SEG,", "$ERR,")):
                resp = txt
            elif txt.startswith("$PED") and resp:
                return rx, resp
        return rx, resp


def conferir_seg(resp, rx, id_seg, refs):
    campos = resp.split("*")[0].split(",")
    origem, grupo, seg, decisao, escore = campos[3], int(campos[4]), int(campos[5]), campos[6], campos[8]
    problemas = []
    if origem != "S":
        problemas.append(f"origem {origem}")
    if grupo != GRUPO_CLIPES or seg != id_seg:
        problemas.append(f"grupo/id {grupo}/{seg}")
    m = re.search(r"escore_hex=(0x[0-9A-F]{8})", rx or "")
    hexa = m.group(1) if m else None
    ref = refs.get(id_seg)
    if ref is None:
        bit = "sem a linha ref (placa ja no modo): escore NAO conferido bit a bit"
    elif hexa == ref:
        bit = "BIT A BIT"
    else:
        bit = f"DIFERE (ref {ref})"
        problemas.append(f"escore {hexa}: {bit}")
    t = re.search(r"ms=(\d+)", rx or "")
    return problemas, f"{decisao} escore={escore} {hexa} {bit}, recepcao {t.group(1) if t else '?'} ms"


def testar_erros(placa, clipes, id_seg):
    """Os seis erros numa sessao so, e o quadro certo no fim: a placa se recuperou."""
    classe, k, amostras = clipes[id_seg]
    quadro = montar_quadro(amostras, GRUPO_CLIPES, id_seg)
    falhas = 0
    for como in ESTRAGOS:
        rx, resp = placa.mandar(estragar(quadro, como))
        if como == "magica":
            ok = resp is None
            print(f"erro {como}: {'OK' if ok else 'FALHOU'}: "
                  f"{'ignorado (nenhuma resposta)' if ok else 'a placa respondeu ' + str(resp)}")
        else:
            ok = resp is not None and resp.startswith("$ERR,") and resp.split("*")[0].endswith("," + como)
            print(f"erro {como}: {'OK' if ok else 'FALHOU'}: {rx} | {resp}")
        falhas += not ok
    rx, resp = placa.mandar(quadro)
    if resp is None or not resp.startswith("$SEG,"):
        print(f"depois dos erros, seg {id_seg}: FALHOU, resposta {resp!r}")
        return falhas + 1
    problemas, resumo = conferir_seg(resp, rx, id_seg, placa.refs)
    print(f"depois dos erros, seg {id_seg} ({classe}, k={k}): {'OK' if not problemas else 'FALHOU'}: {resumo}")
    return falhas + bool(problemas)


def rodar(args):
    clipes = carregar_clipes()
    ids = [] if args.erros and not args.todos else (list(clipes) if args.todos else [args.seg])
    for i in ids:
        if i not in clipes:
            raise SystemExit(f"segmento {i} nao esta nos clipes; os da flash: {sorted(clipes)}")
    placa = Placa(args.porta, args.baud, args.salvar)
    placa.entrar_no_modo(args.ja_no_modo)
    if placa.refs and len(placa.refs) != len(clipes):
        print(f"AVISO: {len(placa.refs)} linhas ref, esperadas {len(clipes)}")
    falhas = 0
    for id_seg in ids:
        classe, k, amostras = clipes[id_seg]
        quadro = montar_quadro(amostras, GRUPO_CLIPES, id_seg)
        rotulo = f"seg {id_seg} ({classe}, k={k})"
        if args.estragar:
            ruim = estragar(quadro, args.estragar)
            rx, resp = placa.mandar(ruim)
            if args.estragar == "magica":
                if resp is not None:
                    print(f"{rotulo} magica: FALHOU, a placa respondeu {resp}")
                    falhas += 1
                else:
                    print(f"{rotulo} magica: ignorado (nenhuma resposta), como esperado; agora o quadro certo")
            else:
                ok = resp is not None and resp.startswith("$ERR,") and resp.split("*")[0].endswith("," + args.estragar)
                print(f"{rotulo} {args.estragar}: {'OK' if ok else 'FALHOU'}: {rx} | {resp}")
                falhas += not ok
                continue
        rx, resp = placa.mandar(quadro)
        if resp is None or not resp.startswith("$SEG,"):
            print(f"{rotulo}: FALHOU, resposta {resp!r} (rx {rx!r})")
            falhas += 1
            continue
        problemas, resumo = conferir_seg(resp, rx, id_seg, placa.refs)
        print(f"{rotulo}: {'OK' if not problemas else 'FALHOU'}: {resumo}" + (f" [{'; '.join(problemas)}]" if problemas else ""))
        falhas += bool(problemas)
    if args.erros:
        falhas += testar_erros(placa, clipes, args.seg)
    print("tudo certo" if not falhas else f"{falhas} falha(s)")
    return 1 if falhas else 0


def autoteste():
    """Sem a placa: os clipes do .h, o layout do quadro, o CRC e os estragos."""
    falhas = []
    clipes = carregar_clipes()
    if sorted(clipes) != [29, 30, 88, 89, 147, 148, 206, 207, 265, 266]:
        falhas.append(f"ids dos clipes: {sorted(clipes)}")
    for id_seg, (classe, k, a) in clipes.items():
        if len(a) != N_AMOSTRAS or not all(-32768 <= v <= 32767 for v in a):
            falhas.append(f"seg {id_seg}: {len(a)} amostras ou fora de int16")
    q = montar_quadro(clipes[29][2], 2, 117)
    if len(q) != TAM_QUADRO or q[:4] != b"QSEG":
        falhas.append(f"quadro com {len(q)} B")
    versao, formato, grupo, id_seg, n = struct.unpack("<BBHIH", q[4:14])
    if (versao, formato, grupo, id_seg, n) != (1, 1, 2, 117, N_AMOSTRAS):
        falhas.append(f"cabecalho {versao, formato, grupo, id_seg, n}")
    (crc,) = struct.unpack("<I", q[14:18])
    if crc != zlib.crc32(q[4:14] + q[18:]):
        falhas.append("CRC nao cobre os bytes 4..13 e o corpo")
    if struct.unpack("<h", q[18:20])[0] != clipes[29][2][0]:
        falhas.append("primeira amostra fora do lugar")
    for como in ESTRAGOS:
        r = estragar(q, como)
        if como == "truncado":
            if len(r) != TAM_QUADRO // 2:
                falhas.append("truncado com outro tamanho")
        elif len(r) != TAM_QUADRO or r == q:
            falhas.append(f"estrago {como} nao mudou o quadro")
    if estragar(q, "crc")[14:18] != q[14:18]:
        falhas.append("o estrago crc mexeu no CRC em vez do corpo")
    for f in falhas:
        print(f"FALHOU: {f}")
    print(f"autoteste: {'FALHOU' if falhas else 'tudo certo'} ({len(clipes)} segmentos dos clipes, quadro de {TAM_QUADRO} B)")
    return 1 if falhas else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    ap.add_argument("--porta", help="porta da serial (ex.: /dev/ttyUSB0, COM5)")
    ap.add_argument("--baud", type=int, default=115200, help="o mesmo do firmware (padrao 115200)")
    ap.add_argument("--seg", type=int, default=29, help="id do segmento dos clipes (29, 30, 88, ...)")
    ap.add_argument("--todos", action="store_true", help="os 10 segmentos dos clipes, em sequencia")
    ap.add_argument("--erros", action="store_true",
                    help="os seis erros (crc, versao, formato, tamanho, truncado, magica) com o --seg, e o "
                         "quadro certo no fim; junto com --todos, depois dos 10")
    ap.add_argument("--estragar", choices=ESTRAGOS, help="manda o quadro errado de proposito")
    ap.add_argument("--ja-no-modo", action="store_true",
                    help="a placa ja esta no modo (de um uso anterior, sem reset): manda direto")
    ap.add_argument("--salvar", type=Path, help="grava todas as linhas da placa neste arquivo")
    ap.add_argument("--autoteste", action="store_true", help="testa o script, sem a placa")
    args = ap.parse_args()
    if args.autoteste:
        return autoteste()
    if not args.porta:
        ap.error("--porta e obrigatorio (ou --autoteste)")
    try:
        return rodar(args)
    except ImportError:
        raise SystemExit("precisa do pyserial: pip install pyserial") from None


if __name__ == "__main__":
    sys.exit(main())
