#!/usr/bin/env python3
"""
orcamento_memoria.py -- flash e RAM do firmware por parte, lidas do .map do linker

Le o frente1.map que o arm-none-eabi-ld grava em todo build (-Wl,-Map) e soma o
tamanho de cada secao de entrada pela parte do firmware a que ela pertence:
clipes, recorte de 51,2 kHz, cadeia (MFCC), decimacao, marco e LDA, pacote da
serial, CMSIS-DSP, HAL, biblioteca C e o resto. Mostra tambem a reserva de heap
e pilha do linker script, a RAM livre e a projecao com o detector de novidade.

Para medir o firmware final, compile o build de medida (orcamento.h), que poe no
link a cadeia, os clipes, o recorte e os buffers da decimacao sem executar nada:

    cmake --preset Debug -DORCAMENTO=ON -DBLOCOS13=OFF
    cmake --build --preset Debug
    python3 tools/orcamento_memoria.py build/Debug/frente1.map

Com --markdown arquivo.md, grava a tabela tambem num arquivo (para o card e o
README). Os totais conferem com o "Memory region" que o linker imprime no fim do
build (--print-memory-usage); se nao conferirem, o script avisa.

So usa a biblioteca padrao do Python; nao precisa do toolchain da ARM.
"""

import argparse
import re
import sys
from collections import defaultdict
from pathlib import Path

FLASH_TOTAL = 512 * 1024
RAM_TOTAL = 128 * 1024
DETECTOR_RAM = 40 * 1024        # card condicional do detector de novidade (~40 KB)

# Secoes de saida do linker script (STM32F411XX_FLASH.ld). A .data ocupa as duas:
# os valores iniciais ficam na flash e sao copiados para a RAM no boot.
SO_FLASH = {".isr_vector", ".text", ".rodata", ".ARM.extab", ".ARM", ".preinit_array",
            ".init_array", ".fini_array"}
FLASH_E_RAM = {".data"}
SO_RAM = {".bss", "._user_heap_stack"}

ORDEM = ["clipes a 12,8 kHz", "recorte a 51,2 kHz", "cadeia (MFCC)",
         "cadeia completa na placa (cadeia_clipes.c)",
         "decimacao (buffers; a funcao esta na CMSIS-DSP)",
         "build de medida (orcamento.c; nao vai para o firmware final)",
         "marco e LDA", "pacote da serial", "teste dos blocos 1 a 3", "CMSIS-DSP",
         "HAL e CubeMX", "aplicacao base", "biblioteca C", "alinhamento", "outros"]


def parte(arquivo: str, secao: str) -> str:
    a = arquivo.replace("\\", "/")
    if "clipes_demo.c" in a:
        return "recorte a 51,2 kHz" if "recorte" in secao else "clipes a 12,8 kHz"
    if "mfcc_estagios.c" in a:
        return "cadeia (MFCC)"
    if "cadeia_clipes.c" in a:
        return "cadeia completa na placa (cadeia_clipes.c)"
    if "orcamento.c" in a:
        # Os buffers da decimacao (decimador*) sao o que o firmware final vai
        # ter; o resto do orcamento.c (o codigo que so toca em tudo e as duas
        # variaveis volatile) existe so no build de medida. A arm_fir_decimate_f32
        # e o init dela vem da libCMSISDSP.a e caem na linha da CMSIS-DSP.
        if "decimador" in secao:
            return "decimacao (buffers; a funcao esta na CMSIS-DSP)"
        return "build de medida (orcamento.c; nao vai para o firmware final)"
    if "marco_lda.c" in a:
        return "marco e LDA"
    if "pacote_serial.c" in a or "texto_fixo.c" in a:
        return "pacote da serial"
    if "blocos13.c" in a:
        return "teste dos blocos 1 a 3"
    if "CMSISDSP" in a or "CMSIS/DSP" in a:
        return "CMSIS-DSP"
    if "STM32_Drivers" in a or "stm32f4xx_hal" in a or "system_stm32f4xx" in a:
        return "HAL e CubeMX"
    if re.search(r"/(main|usart|gpio|stm32f4xx_it|syscalls|sysmem)\.c|startup_", a):
        return "aplicacao base"
    if re.search(r"lib(c|m|g|gcc|nosys|c_nano|g_nano|m_nano)\.a|crt[0-9in]|/libgcc", a):
        return "biblioteca C"
    return "outros"


RE_SAIDA = re.compile(r"^(\.\S+|\S+)(?:\s+0x([0-9a-fA-F]+)\s+0x([0-9a-fA-F]+))?(?:\s+load address\s+0x[0-9a-fA-F]+)?\s*$")
RE_ENTRADA = re.compile(r"^ (\.\S+|COMMON)(?:\s+0x([0-9a-fA-F]+)\s+0x([0-9a-fA-F]+)\s+(.+))?\s*$")
RE_CONTINUA = re.compile(r"^\s+0x([0-9a-fA-F]+)\s+0x([0-9a-fA-F]+)\s+(.+)$")
RE_FILL = re.compile(r"^ \*fill\*\s+0x([0-9a-fA-F]+)\s+0x([0-9a-fA-F]+)")
RE_SIMBOLO = re.compile(r"^\s+0x([0-9a-fA-F]+)\s+(_Min_Heap_Size|_Min_Stack_Size)\s*=")


def ler_map(caminho: Path):
    """Devolve (por_parte, tam_saida, simbolos).

    As secoes de entrada sao contadas pelo ENDERECO, nao so pelo tamanho que o .map
    imprime: as strings que o linker mescla (.rodata.*.str1.4) aparecem com o
    tamanho de antes da mescla e se sobrepoem as seguintes. Cada secao conta do
    seu endereco ate o comeco da proxima (ou o fim da secao de saida), sem repetir
    o que ja foi contado.
    """
    linhas = caminho.read_text(errors="replace").splitlines()
    try:
        inicio = next(i for i, l in enumerate(linhas) if l.startswith("Linker script and memory map"))
    except StopIteration:
        raise SystemExit(f"{caminho}: nao parece um .map do GNU ld (sem 'Linker script and memory map')")

    saidas = {}      # nome -> (endereco, tamanho)
    entradas = defaultdict(list)  # nome da secao de saida -> [(endereco, tamanho, parte)]
    simbolos = {}
    atual = None
    pendente = None

    def guardar(nome_entrada, endereco, tamanho, arquivo):
        if atual is None or tamanho == 0:
            return
        p = "alinhamento" if arquivo == "alinhamento" else parte(arquivo, nome_entrada)
        entradas[atual].append((endereco, tamanho, p))

    i = inicio + 1
    while i < len(linhas):
        l = linhas[i]
        m = RE_SIMBOLO.match(l)
        if m:
            simbolos[m.group(2)] = int(m.group(1), 16)
        if pendente is not None:
            m = RE_CONTINUA.match(l)
            if m and not m.group(3).startswith("load address"):
                guardar(pendente, int(m.group(1), 16), int(m.group(2), 16), m.group(3).strip())
            pendente = None
            i += 1
            continue
        if l and not l[0].isspace():
            m = RE_SAIDA.match(l)
            if m and m.group(1).startswith("."):
                atual = m.group(1)
                if m.group(3):
                    saidas[atual] = (int(m.group(2), 16), int(m.group(3), 16))
                elif i + 1 < len(linhas):
                    m2 = re.match(r"^\s+0x([0-9a-fA-F]+)\s+0x([0-9a-fA-F]+)", linhas[i + 1])
                    if m2:
                        saidas[atual] = (int(m2.group(1), 16), int(m2.group(2), 16))
            else:
                atual = None
            i += 1
            continue
        m = RE_FILL.match(l)
        if m:
            guardar("*fill*", int(m.group(1), 16), int(m.group(2), 16), "alinhamento")
            i += 1
            continue
        m = RE_ENTRADA.match(l)
        if m:
            if m.group(3) is None:
                pendente = m.group(1)
            elif not m.group(4).startswith("load address"):
                guardar(m.group(1), int(m.group(2), 16), int(m.group(3), 16), m.group(4).strip())
        i += 1

    por_parte = defaultdict(lambda: {"flash": 0, "ram": 0})
    for nome, (base, tam) in saidas.items():
        if nome == "._user_heap_stack":   # reserva de heap e pilha, contada a parte
            continue
        if not (nome in SO_FLASH or nome in FLASH_E_RAM or nome in SO_RAM):
            continue
        lista = sorted(entradas.get(nome, []), key=lambda e: (e[0], -e[1]))
        fim_saida = base + tam
        inicios = sorted({e[0] for e in lista})
        cursor = base
        for endereco, tamanho, p in lista:
            k = inicios.index(endereco)
            limite = inicios[k + 1] if k + 1 < len(inicios) else fim_saida
            ini = max(endereco, cursor)
            fim = min(endereco + tamanho, limite, fim_saida)
            efetivo = max(0, fim - ini)
            cursor = max(cursor, ini + efetivo)
            if nome in SO_FLASH or nome in FLASH_E_RAM:
                por_parte[p]["flash"] += efetivo
            if nome in SO_RAM or nome in FLASH_E_RAM:
                por_parte[p]["ram"] += efetivo
    tam_saida = {n: t for n, (_, t) in saidas.items()}
    return por_parte, tam_saida, simbolos


def kib(b):
    return f"{b / 1024:.1f}".replace(".", ",")


def bytes_br(b):
    return f"{b:,}".replace(",", ".")


def montar(caminho: Path):
    por_parte, tam_saida, simbolos = ler_map(caminho)
    heap = simbolos.get("_Min_Heap_Size", 0)
    pilha = simbolos.get("_Min_Stack_Size", 0)

    flash = sum(tam_saida.get(s, 0) for s in SO_FLASH | FLASH_E_RAM)
    ram = sum(tam_saida.get(s, 0) for s in SO_RAM | FLASH_E_RAM)
    ram_heap_pilha = tam_saida.get("._user_heap_stack", 0)
    ram_sem_reserva = ram - ram_heap_pilha

    linhas = []
    linhas.append(f"Orcamento de memoria ({caminho})")
    linhas.append("")
    linhas.append("| Parte | Flash (B) | RAM (B) |")
    linhas.append("|---|---:|---:|")
    soma_f = soma_r = 0
    for p in ORDEM:
        v = por_parte.get(p)
        if not v or (v["flash"] == 0 and v["ram"] == 0):
            continue
        linhas.append(f"| {p} | {bytes_br(v['flash'])} | {bytes_br(v['ram'])} |")
        soma_f += v["flash"]
        soma_r += v["ram"]
    linhas.append(f"| reserva de heap e pilha (linker script: heap {bytes_br(heap)} B, pilha {bytes_br(pilha)} B) "
                  f"| 0 | {bytes_br(ram_heap_pilha)} |")
    linhas.append(f"| **total** | **{bytes_br(flash)}** | **{bytes_br(ram)}** |")
    linhas.append("")

    audio = por_parte["clipes a 12,8 kHz"]["flash"] + por_parte["recorte a 51,2 kHz"]["flash"]
    livre_f = FLASH_TOTAL - flash
    livre_r = RAM_TOTAL - ram
    linhas.append(f"- Flash: {bytes_br(flash)} B de {bytes_br(FLASH_TOTAL)} B "
                  f"({100 * flash / FLASH_TOTAL:.1f} %); livres {bytes_br(livre_f)} B ({kib(livre_f)} KiB).")
    linhas.append(f"- Audio na flash (clipes + recorte): {bytes_br(audio)} B; o resto do firmware: "
                  f"{bytes_br(flash - audio)} B ({kib(flash - audio)} KiB), contra a reserva de 64 KB do orcamento.")
    linhas.append(f"- RAM: {bytes_br(ram)} B de {bytes_br(RAM_TOTAL)} B ({100 * ram / RAM_TOTAL:.1f} %), "
                  f"dos quais {bytes_br(ram_heap_pilha)} B sao a reserva minima de heap e pilha; "
                  f"livres {bytes_br(livre_r)} B ({kib(livre_r)} KiB).")
    sobra = livre_r - DETECTOR_RAM
    linhas.append(f"- Com o detector de novidade (~{bytes_br(DETECTOR_RAM)} B a mais, estimativa do card): "
                  f"sobrariam {bytes_br(sobra)} B ({kib(sobra)} KiB) de RAM."
                  + ("" if sobra > 0 else " NAO CABE."))
    linhas.append(f"- A pilha reservada e so o minimo do linker script ({bytes_br(pilha)} B); a pilha "
                  f"real pode crescer para a RAM livre, sem aviso do linker.")
    return "\n".join(linhas), flash, ram, ram_sem_reserva, por_parte


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    ap.add_argument("map", type=Path, help="o .map do build (ex.: build/Debug/frente1.map)")
    ap.add_argument("--markdown", type=Path, help="grava a tabela tambem neste arquivo")
    args = ap.parse_args()
    if not args.map.is_file():
        print(f"nao achei {args.map}; compile antes (cmake --build --preset Debug)")
        return 2
    texto, flash, ram, _, por_parte = montar(args.map)
    print(texto)
    soma_f = sum(v["flash"] for v in por_parte.values())
    soma_r = sum(v["ram"] for v in por_parte.values())
    _, tam_saida, _ = ler_map(args.map)
    heap_pilha = tam_saida.get("._user_heap_stack", 0)
    if soma_f != flash or soma_r + heap_pilha != ram:
        print(f"\nAVISO: a soma das partes (flash {soma_f}, RAM {soma_r + heap_pilha}) nao bate com os "
              f"totais das secoes ({flash}, {ram}); alguma linha do .map nao foi reconhecida.")
    if "orcamento.c" not in args.map.read_text(errors="replace"):
        print("\nAVISO: este build nao tem o orcamento.c: o recorte de 51,2 kHz e a decimacao ficam "
              "fora do link (a cadeia e os clipes ja entram no build padrao desde a integracao da "
              "cadeia completa). Para o orcamento do firmware final, compile com -DORCAMENTO=ON.")
    if args.markdown:
        args.markdown.parent.mkdir(parents=True, exist_ok=True)
        args.markdown.write_text(texto + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
