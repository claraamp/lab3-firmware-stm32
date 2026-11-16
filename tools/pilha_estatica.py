#!/usr/bin/env python3
"""
pilha_estatica.py -- pior caso da pilha pelo grafo de chamadas, contra a reserva do linker

Le os .ci que o gcc grava com -fcallgraph-info=su (um por arquivo: as funcoes, o
quadro de pilha de cada uma e as chamadas) e soma, pelo caminho mais fundo, os
quadros a partir do main e de cada rotina de interrupcao. As funcoes sem .ci (a
newlib nano: snprintf, logf, memcpy...) sao lidas da desmontagem do .elf: o quadro
sai do prologo (push e sub sp) e as chamadas, dos bl/b.w para simbolos conhecidos.

    cmake --preset Debug -DMEDIDA=ON
    cmake --build --preset Debug
    python3 tools/pilha_estatica.py build/Debug

Na opcao MEDIDA, o CMakeLists.txt liga o -fcallgraph-info=su (nao muda o codigo
gerado). Sem os .ci, o script avisa e para.

O que entra no pior caso:
- o caminho mais fundo a partir do main (o boot e o laco, inclusive o modo de
  recepcao), com o caminho impresso funcao a funcao, e o mais fundo a partir do
  recepcao_serial_executar, para a secao do card;
- as interrupcoes pelos niveis de prioridade do firmware: o SysTick (nivel 15)
  pode ser interrompido pelo USART1 ou pelo DMA2 Stream2 (nivel 0, que nao se
  interrompem). Cada entrada em interrupcao empilha o quadro estendido do
  Cortex-M4F (26 palavras, 104 B, com o contexto da FPU) e o alinhamento de 8 B
  (ate 4 B).

Limites: chamadas por ponteiro (o rx.ler_ciclos, os callbacks da HAL chamados por
ponteiro quando USE_HAL_UART_REGISTER_CALLBACKS=1) nao aparecem no grafo; o script
lista as funcoes com quadro "dynamic" e as chamadas que nao resolveu. A pilha
medida na placa (opcao MEDIDA, campo pilha= da linha rx) e a conferencia.

So usa a biblioteca padrao do Python e o arm-none-eabi-objdump (do STM32CubeCLT).
"""

import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path

RESERVA_PADRAO = 4096        # _Min_Stack_Size dos .ld e do .ioc (desde 20/10)
QUADRO_EXCECAO = 104 + 4     # quadro estendido com a FPU, mais o alinhamento de 8 B

# Chamadas por ponteiro que o grafo nao ve: a HAL chama os callbacks do DMA pelo
# hdma->XferCpltCallback e companhia, e eles chamam os callbacks da UART que o
# recepcao_serial.c define.
INDIRETAS = {
    "HAL_DMA_IRQHandler": ["UART_DMAReceiveCplt", "UART_DMARxHalfCplt", "UART_DMAError",
                           "UART_DMAAbortOnError", "UART_DMATransmitCplt", "UART_DMATxHalfCplt"],
    # erro de linha com DMA: HAL_DMA_Abort_IT -> hdma->XferAbortCallback
    "HAL_UART_IRQHandler": ["UART_DMAAbortOnError"],
}
# As outras chamadas por ponteiro do nosso codigo sao a leitura do DWT passada como
# argumento (ler_dwt, ler_ciclos: 0 B de pilha, uma leitura de registrador), e por
# isso nao mudam o pior caso.

RE_NODE = re.compile(r'node: \{ title: "([^"]+)" label: "([^"]*)"')
RE_EDGE = re.compile(r'edge: \{ sourcename: "([^"]+)" targetname: "([^"]+)"')
RE_BYTES = re.compile(r"\\n(\d+) bytes \((static|dynamic|dynamic,bounded)\)")


def nome_curto(titulo: str) -> str:
    # static: "arquivo.c:funcao"; extern: "funcao"
    return titulo.rsplit(":", 1)[-1] if titulo.count(":") and "/" in titulo else titulo


def ler_ci(pasta: Path):
    quadro, tipo, chamadas = {}, {}, {}
    arquivos = sorted(pasta.rglob("*.ci"))
    for ci in arquivos:
        texto = ci.read_text(errors="replace")
        for titulo, label in RE_NODE.findall(texto):
            m = RE_BYTES.search(label)
            if m:   # so os nos definidos neste arquivo trazem o tamanho
                quadro[titulo] = int(m.group(1))
                tipo[titulo] = m.group(2)
                chamadas.setdefault(titulo, set())
        for origem, destino in RE_EDGE.findall(texto):
            chamadas.setdefault(origem, set()).add(destino)
    return arquivos, quadro, tipo, chamadas


def ler_elf(elf: Path, objdump: str):
    """Quadro e chamadas de cada funcao, pela desmontagem (para quem nao tem .ci).
    Nomes no mesmo endereco (snprintf e sniprintf na newlib nano) viram apelidos."""
    nm = objdump.replace("objdump", "nm")
    apelidos = {}
    for linha in subprocess.run([nm, str(elf)], capture_output=True, text=True,
                                check=True).stdout.splitlines():
        partes = linha.split()
        if len(partes) == 3 and partes[1] in "TtWw":
            apelidos.setdefault(int(partes[0], 16) & ~1, []).append(partes[2])
    saida = subprocess.run([objdump, "-d", "--no-show-raw-insn", str(elf)],
                           capture_output=True, text=True, check=True).stdout
    quadro, chamadas, nomes = {}, {}, {}
    atual, prologo = None, 0
    for linha in saida.splitlines():
        m = re.match(r"^([0-9a-f]+) <([^>]+)>:", linha)
        if m:
            atual, prologo = m.group(2), 0
            quadro[atual], chamadas[atual] = 0, set()
            nomes[atual] = apelidos.get(int(m.group(1), 16), [atual])
            continue
        if atual is None or ":" not in linha:
            continue
        instr = linha.split(":", 1)[1].strip()
        prologo += 1
        if prologo <= 8:   # o prologo fica nas primeiras instrucoes
            m = re.match(r"(?:push|stmdb\s+sp!,)\s*\{([^}]*)\}", instr)
            if m:
                quadro[atual] += 4 * len([r for r in m.group(1).split(",") if r.strip()])
            m = re.match(r"vpush\s*\{([^}]*)\}", instr)
            if m:
                regs = m.group(1)
                r = re.match(r"\s*([sd])(\d+)\s*-\s*[sd](\d+)", regs)
                n = (int(r.group(3)) - int(r.group(2)) + 1) if r else len(regs.split(","))
                quadro[atual] += n * (8 if regs.strip().startswith("d") else 4)
            m = re.match(r"sub(?:\.w|w)?\s+sp,\s*(?:sp,\s*)?#(\d+)", instr)
            if m:
                quadro[atual] += int(m.group(1))
        m = re.match(r"(?:bl|blx|b\.w|b)\s+[0-9a-f]+ <([^>+]+)>", instr)
        if m and m.group(1) != atual:
            chamadas[atual].add(m.group(1))
    for f, ns in nomes.items():   # cada apelido aponta para a mesma funcao
        for n in ns:
            if n != f and n not in quadro:
                quadro[n], chamadas[n] = quadro[f], {f}
    for f in nomes:
        for n in nomes[f]:
            if n != f and n in quadro and quadro[n] == quadro[f] and chamadas[n] == {f}:
                quadro[n] = 0   # o apelido so repassa: o quadro conta uma vez
    return quadro, chamadas


def montar(pasta: Path, elf: Path, objdump: str):
    arquivos, quadro, tipo, chamadas = ler_ci(pasta)
    if not arquivos:
        sys.exit(f"nenhum .ci em {pasta}: compile com -DMEDIDA=ON (liga o -fcallgraph-info=su)")
    q_elf, c_elf = ler_elf(elf, objdump)
    # nome curto -> titulo do no (extern pelo nome; static so se o nome for unico)
    por_nome = {}
    for t in quadro:
        por_nome.setdefault(nome_curto(t), []).append(t)
    origem_elf = set()
    for f in q_elf:   # o que nao tem .ci entra pela desmontagem
        if f not in por_nome:
            quadro[f], tipo[f] = q_elf[f], "elf"
            chamadas[f] = set(c_elf[f])
            por_nome[f] = [f]
            origem_elf.add(f)

    def resolver(destino):
        if destino in quadro:
            return destino
        cands = por_nome.get(nome_curto(destino), [])
        return cands[0] if len(cands) == 1 else None

    for origem, destinos in INDIRETAS.items():
        t = resolver(origem)
        if t is not None:
            chamadas[t] = set(chamadas.get(t, set())) | set(destinos)

    grafo, sem_destino = {}, {}
    for f, ds in chamadas.items():
        grafo[f] = []
        for d in ds:
            r = resolver(d)
            if r is None:
                sem_destino.setdefault(f, set()).add(nome_curto(d))
            elif r != f:
                grafo[f].append(r)
    return quadro, tipo, grafo, por_nome, origem_elf, sem_destino


def alcancaveis(raizes, grafo):
    vistos, fila = set(), list(raizes)
    while fila:
        f = fila.pop()
        if f not in vistos:
            vistos.add(f)
            fila.extend(grafo.get(f, []))
    return vistos


def pior_caminho(raiz, quadro, grafo):
    memo, pilha_rec, recursivas = {}, set(), set()

    def visita(f):
        if f in memo:
            return memo[f]
        if f in pilha_rec:
            recursivas.add(f)
            return (0, [])
        pilha_rec.add(f)
        melhor = (0, [])
        for g in grafo.get(f, []):
            c = visita(g)
            if c[0] > melhor[0]:
                melhor = c
        pilha_rec.discard(f)
        memo[f] = (quadro.get(f, 0) + melhor[0], [f] + melhor[1])
        return memo[f]

    total, caminho = visita(raiz)
    return total, caminho, recursivas


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    ap.add_argument("build", type=Path, help="pasta do build (ex.: build/Debug)")
    ap.add_argument("--elf", type=Path, help="padrao: <build>/frente1.elf")
    ap.add_argument("--objdump", default="arm-none-eabi-objdump")
    ap.add_argument("--reserva", type=int, default=RESERVA_PADRAO, help="_Min_Stack_Size (B)")
    args = ap.parse_args()

    elf = args.elf or args.build / "frente1.elf"
    objdump = shutil.which(args.objdump) or args.objdump
    quadro, tipo, grafo, por_nome, origem_elf, sem_destino = montar(args.build, elf, objdump)

    def titulo(nome):
        cands = por_nome.get(nome, [])
        return cands[0] if cands else None

    def mostrar(rotulo, raiz):
        t = titulo(raiz)
        if t is None:
            print(f"{rotulo}: {raiz} nao encontrado")
            return 0, []
        total, caminho, rec = pior_caminho(t, quadro, grafo)
        print(f"\n{rotulo}: {total} B")
        for f in caminho:
            marca = " (newlib, pelo .elf)" if f in origem_elf else ""
            dyn = " [dynamic]" if tipo.get(f, "").startswith("dynamic") else ""
            print(f"  {quadro.get(f, 0):5d} B  {nome_curto(f)}{marca}{dyn}")
        if rec:
            print(f"  recursao no caminho (contada uma vez): {', '.join(nome_curto(r) for r in rec)}")
        return total, caminho

    print(f"Pilha pelo grafo de chamadas ({elf})")
    p_main, _ = mostrar("main (boot e laco)", "main")
    p_rx, _ = mostrar("recepcao_serial_executar (modo de recepcao)", "recepcao_serial_executar")
    irq0 = {}
    for h in ("USART1_IRQHandler", "DMA2_Stream2_IRQHandler"):
        irq0[h], _ = mostrar(f"{h} (nivel 0)", h)
    p_tick, _ = mostrar("SysTick_Handler (nivel 15)", "SysTick_Handler")

    p_irq = QUADRO_EXCECAO + p_tick + QUADRO_EXCECAO + max(irq0.values() or [0])
    pior = p_main + p_irq
    print("\nPior caso (main + SysTick interrompido por USART1 ou DMA2, cada entrada com "
          f"{QUADRO_EXCECAO} B de quadro de excecao)")
    print(f"  main {p_main} B + interrupcoes {p_irq} B = {pior} B, contra a reserva de "
          f"{args.reserva} B ({100 * pior / args.reserva:.0f} %); folga {args.reserva - pior} B")
    print(f"  so o modo de recepcao: {p_rx} B + {p_irq} B = {p_rx + p_irq} B")

    raizes = [titulo(n) for n in ("main", "USART1_IRQHandler", "DMA2_Stream2_IRQHandler",
                                  "SysTick_Handler") if titulo(n)]
    usadas = alcancaveis(raizes, grafo)
    dinamicas = sorted(nome_curto(f) for f in usadas if tipo.get(f, "").startswith("dynamic"))
    if dinamicas:
        print(f"\nQuadro dinamico (alloca ou VLA; o tamanho acima e o fixo): {', '.join(dinamicas)}")
    # so o que o firmware alcanca; as funcoes da biblioteca de ponto flutuante do gcc
    # (__aeabi_*) nao tem prologo de pilha relevante e ficam de fora
    faltam = {}
    for f in usadas:
        for d in sem_destino.get(f, ()):
            if not d.startswith("__aeabi_"):
                faltam.setdefault(d, set()).add(nome_curto(f))
    if faltam:
        print("\nChamadas que o grafo nao resolve (por ponteiro ou sem simbolo), nas funcoes alcancadas:")
        for d, fs in sorted(faltam.items()):
            print(f"  {d}: chamada de {', '.join(sorted(fs))}")
    return 0 if pior <= args.reserva else 1


if __name__ == "__main__":
    sys.exit(main())
