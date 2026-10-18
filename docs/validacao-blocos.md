# Validação bloco a bloco da cadeia em C

Card "Validar cada bloco da cadeia em C contra a referência em Python" (2º parcial, 26/10). Validação feita por quem não portou nenhum bloco, para não repetir o entendimento de quem portou.

## Resultado

**No PC, todos os blocos ficam dentro das tolerâncias do A4 nos 295 segmentos da partição**, nos dois modos. A maior margem usada é 11,3% da tolerância (escore, modo encadeado). A cadeia em C, do `int16` ao escore da LDA, dá a **mesma decisão do Python nos 295 segmentos**. A maior diferença de escore contra o `escores_referencia.csv` é 0,0016, contra um menor |escore| de 1.297,2.

**Na placa**, o teste do segmento completo está pronto e conferido com o emulador, mas **ainda não rodou na placa** (seção "Placa").

| Item do card | Situação |
|---|---|
| A4 confirmado no contrato | Feito: `docs/contrato_numerico.md`, seção "Tolerâncias por estágio (A4)", com os valores em `dsp.ESTAGIOS_PORTE` |
| Bloco a bloco no harness, com maior erro absoluto e relativo | Feito, nos 295 segmentos (tabelas abaixo) |
| Mesmo teste na placa (FFT da CMSIS otimizada e segmento completo) | Blocos 1 a 3: teste da Clara (`blocos13`). Segmento completo: teste pronto, **falta rodar na placa** |
| Escore final contra o `escores_referencia.csv` | Feito: 295/295 decisões iguais, diferença máxima 0,0016 |
| Decimação (bloco 0) | Não entrou: ainda não há decimação no firmware |
| Tabela de erros no repositório | Este arquivo |

## Como a validação é independente

- **Mais segmentos que os do porte.** O harness versionado usa 5 segmentos, um por gravação, os mesmos com que os blocos foram portados e ajustados. Aqui entram **os 295 segmentos da partição**. As referências por estágio de todos eles foram geradas com o próprio `08_export_stage_references.py`, com `--segmentos`; o 08 confere o escore de cada um contra o CSV antes de gravar.
- **Outra conta de erro.** O C só grava as saídas (`tests/host/validacao/exportar_saidas_c.c`). A comparação é do `tests/host/validacao/validar_blocos.py`, que usa o `dsp.erro_estagio` e o `dsp.ESTAGIOS_PORTE` do repositório do Python, e não a conta de erro do harness. Os dois chegam aos mesmos números, o que também confere o harness.
- **O validador reprova quando deve.** Com o `mfcc_estagios.c` estragado de propósito, ele dá FALHOU:
  - dividir por 32768: escala, janela, espectro, Mel, MFCC e média;
  - `log10f` no lugar do `logf`: do log-Mel ao escore, com 236/295 decisões iguais;
  - desvio amostral (`ddof = 1`): desvio e escore.

  Com a captura da placa adulterada, também reprova: um desvio 0,1% maior, e um escore com o sinal trocado (decisão diferente).

## Tabelas (PC, 295 segmentos)

CMSIS-DSP genérica em C, compilada com o gcc no PC, sem fusão de multiplicação e soma (FMA). **Erro abs.** é `max|c − r|`; **Erro rel.** é `max|c − r| / max|r|` do estágio. Os dois são o pior caso entre os 295 segmentos. A **Métrica A4** e a tolerância são as do contrato. "media" e "desvio" são os 13 coeficientes de cada; "escore" é o da LDA final (`exp239`).

### Modo isolado

Cada estágio recebe a referência do anterior, arredondada para float32: mede o erro do próprio bloco.

| Estágio | Métrica | Erro abs. | Erro rel. | Métrica A4 | Tolerância | % da tol. | Pior segmento | Situação |
|---|---|---:|---:|---:|---:|---:|---|---|
| escala | rel_quadro | 3,0 × 10⁻⁸ | 5,1 × 10⁻⁸ | 5,1 × 10⁻⁸ | 1 × 10⁻⁶ | 5,1% | bpfo_0.3mm_seg0202 | OK |
| janela | rel_quadro | 1,0 × 10⁻⁷ | 1,6 × 10⁻⁷ | 1,7 × 10⁻⁷ | 5 × 10⁻⁶ | 3,5% | bpfi_1.0mm_seg0119 | OK |
| espectro | rel_quadro | 5,2 × 10⁻⁴ | 3,9 × 10⁻⁷ | 4,8 × 10⁻⁷ | 5 × 10⁻⁶ | 9,7% | normal_seg0003 | OK |
| mel | rel_quadro | 1,7 × 10⁻³ | 3,9 × 10⁻⁷ | 4,8 × 10⁻⁷ | 5 × 10⁻⁶ | 9,6% | normal_seg0009 | OK |
| logmel | abs | 5,1 × 10⁻⁷ | 6,2 × 10⁻⁸ | 5,1 × 10⁻⁷ | 1 × 10⁻⁵ | 5,1% | bpfi_1.0mm_seg0144 | OK |
| mfcc | abs | 3,6 × 10⁻⁶ | 3,4 × 10⁻⁷ | 3,6 × 10⁻⁶ | 5 × 10⁻⁵ | 7,2% | bpfi_1.0mm_seg0124 | OK |
| media | abs | 5,1 × 10⁻⁶ | 5,9 × 10⁻⁷ | 5,1 × 10⁻⁶ | 1 × 10⁻⁴ | 5,1% | bpfi_1.0mm_seg0138 | OK |
| desvio | rel | 1,2 × 10⁻⁶ | 1,7 × 10⁻⁶ | 2,1 × 10⁻⁶ | 5 × 10⁻⁵ | 4,1% | bpfi_1.0mm_seg0138 | OK |
| escore | rel | 6,0 × 10⁻⁴ | 4,2 × 10⁻⁷ | 4,2 × 10⁻⁷ | 5 × 10⁻⁶ | 8,4% | bpfi_1.0mm_seg0150 | OK |

### Modo encadeado

A cadeia em C desde o `int16`, com as mesmas chamadas da placa: o erro de cada estágio inclui o de todos os anteriores.

| Estágio | Métrica | Erro abs. | Erro rel. | Métrica A4 | Tolerância | % da tol. | Pior segmento | Situação |
|---|---|---:|---:|---:|---:|---:|---|---|
| escala | rel_quadro | 3,0 × 10⁻⁸ | 5,1 × 10⁻⁸ | 5,1 × 10⁻⁸ | 1 × 10⁻⁶ | 5,1% | bpfo_0.3mm_seg0202 | OK |
| janela | rel_quadro | 1,0 × 10⁻⁷ | 1,6 × 10⁻⁷ | 1,7 × 10⁻⁷ | 5 × 10⁻⁶ | 3,5% | bpfi_1.0mm_seg0119 | OK |
| espectro | rel_quadro | 5,2 × 10⁻⁴ | 3,9 × 10⁻⁷ | 4,7 × 10⁻⁷ | 5 × 10⁻⁶ | 9,3% | bpfi_1.0mm_seg0164 | OK |
| mel | rel_quadro | 1,4 × 10⁻³ | 3,7 × 10⁻⁷ | 5,5 × 10⁻⁷ | 5 × 10⁻⁶ | 10,9% | bpfo_0.3mm_seg0220 | OK |
| logmel | abs | 2,0 × 10⁻⁵ | 2,3 × 10⁻⁶ | 2,0 × 10⁻⁵ | 2 × 10⁻⁴ | 9,8% | bpfi_1.0mm_seg0163 | OK |
| mfcc | abs | 7,0 × 10⁻⁶ | 6,7 × 10⁻⁷ | 7,0 × 10⁻⁶ | 1 × 10⁻⁴ | 7,0% | bpfi_1.0mm_seg0163 | OK |
| media | abs | 5,1 × 10⁻⁶ | 5,9 × 10⁻⁷ | 5,1 × 10⁻⁶ | 1 × 10⁻⁴ | 5,1% | bpfi_1.0mm_seg0138 | OK |
| desvio | rel | 1,3 × 10⁻⁶ | 1,7 × 10⁻⁶ | 2,2 × 10⁻⁶ | 5 × 10⁻⁵ | 4,3% | bpfi_1.0mm_seg0138 | OK |
| escore | rel | 1,6 × 10⁻³ | 1,1 × 10⁻⁶ | 1,1 × 10⁻⁶ | 1 × 10⁻⁵ | 11,3% | bpfi_1.0mm_seg0167 | OK |

O `mfcc_segmento()`, a chamada única que a placa vai usar, dá bit a bit as mesmas 26 características que o laço estágio a estágio nos 295 segmentos.

**Com FMA** (`-ffp-contract=fast -march=native`, como o Cortex-M4 faz com `vfma.f32`), todos os estágios continuam OK. O pior caso passa a ser o Mel encadeado (10,9%), e o escore encadeado cai para 10,4%.

### Leitura

- **As margens nos 295 segmentos são maiores que nos 5 do harness**, mas continuam folgadas. Nos 5, o pior caso era 7,8% (Mel encadeado) e o escore encadeado ficava em 3,4%. Nos 295, o pior caso é 11,3% (escore encadeado). Nenhum bloco chega perto do limite: a tolerância é 10 vezes o maior erro da emulação em float32 do `exp253`, e os erros medidos ficam pelo menos 8 vezes abaixo dela.
- **O erro absoluto do espectro e do Mel parece grande (até 1,7 × 10⁻³), mas é relativo à escala.** Esses estágios vão até ~10⁴ nos quadros fortes, e o erro relativo fica em 4 × 10⁻⁷, perto do arredondamento do float32. Por isso o A4 os mede por quadro (`rel_quadro`).
- **O salto do isolado para o encadeado está no log-Mel** (de 5,1 × 10⁻⁷ para 2,0 × 10⁻⁵), como o contrato prevê: o erro relativo do espectro nas bandas de pouca energia vira erro absoluto no log. A tolerância encadeada do log-Mel já conta com isso (20 vezes a isolada).

## Escore final contra o `escores_referencia.csv`

A cadeia em C inteira (`int16` → MFCC → média e desvio → LDA), nos 295 segmentos:

| Medida | Valor |
|---|---|
| Decisões iguais às do CSV | **295/295** |
| Diferença máxima do escore | 0,0016 (`bpfi_1.0mm_seg0167`) |
| Diferença mediana | 0,0003 |
| Erro relativo máximo | 1,1 × 10⁻⁶ (11,3% da tolerância encadeada do escore) |
| Menor \|escore\| do CSV | 1.297,2: cerca de 800 mil vezes a maior diferença |

O card esperava uma diferença de "~0,0007, como no Marco". A cadeia completa chega a 0,0016 no pior segmento. Isso é esperado e está dentro do A4: no Marco, as 26 características vinham prontas do Python, e o erro era só o da LDA em float32 (0,0005). Aqui, o erro do MFCC em float32 também entra.

## Placa

### Blocos 1 a 3 (janela, espectro e Mel): FFT da CMSIS otimizada

O teste `blocos13` da Clara (PR #3) roda os três blocos na placa nos 5 segmentos de referência e compara cada quadro com a tolerância encadeada do A4. O resultado relatado no PR é 15/15 estágios OK, com pior caso de 7,1% (espectro, `bpfi_1.0mm_seg0147`). **Não refiz esse teste na placa**; a captura anexada ao PR não abria quando revisei. Para refazer, siga a seção "Blocos 1 a 3 na placa" do README.

### Segmento completo (blocos 1 a 6 e escore): falta rodar

O `Core/Src/validacao_placa.c` (opção `VALIDACAO_PLACA` do CMake, desligada por padrão) roda, ao ligar, o `mfcc_segmento()` e o `lda_escore()` nos **10 segmentos dos clipes** do `clipes_demo.h`. Ele mede os ciclos de cada segmento com o DWT e manda as 26 características e o escore pela serial, em hexadecimal do float32, sem perder bits. O `validar_blocos.py --placa` compara:

- a média e o desvio com as referências por estágio do Python, pela tolerância **encadeada** do A4;
- o escore com o `escores_referencia.csv` e a decisão com a do CSV.

O teste leva menos de 1 s. Ele usa a CMSIS otimizada do Cortex-M4 e a FMA do compilador da placa, que o PC não reproduz.

**Conferido sem a placa:** o emulador (`emular_validacao_placa`) roda o mesmo código no PC e dá 10/10 OK. Os maiores erros são 2,4% (média), 2,5% (desvio) e 5,8% (escore) da tolerância, e as decisões são todas iguais. Isso confere o protocolo e o validador, não a placa.

**Para rodar na placa:**

```bash
cmake --preset Debug -DVALIDACAO_PLACA=ON
cmake --build --preset Debug          # e gravar
picocom -b 115200 /dev/ttyUSB0 --logfile build/validacao_placa.txt   # apertar o RESET, esperar "val FIM", sair com Ctrl-A Ctrl-X
python3 tests/host/validacao/validar_blocos.py --refs REFS_295 --saidas build/saidas_c \
    --placa build/validacao_placa.txt --md build/validacao_placa.md
cmake --preset Debug -DVALIDACAO_PLACA=OFF   # antes de gravar o firmware normal
```

Com o teste ligado, o firmware Debug ocupa 322.672 B de flash (62%; 256 KB são os clipes) e 18.384 B de RAM.

**Resultado na placa:** _pendente; colar aqui a tabela do `--md`._

## Decimação (bloco 0)

Não entrou nesta semana: o firmware ainda não decima (os clipes já estão a 12,8 kHz). Quando entrar, a comparação deve ser contra a saída em float da decimação do Python, não contra o `int16` requantizado, como diz o `clipes_demo.h` (`RECORTE_ESCORE_REF`).

## Como reproduzir

Com os dois repositórios lado a lado, `numpy` e `scipy` (as versões do `requirements.txt` do Python), e os `.bin` do PCM decimado em `data/processed/pcm_decimated/12800/` do repositório do Python:

```bash
# 1. Referências por estágio dos 295 segmentos (no repositório do Python, fora da pasta versionada)
cd ../diagnostico-acustico-motores
python3 scripts/pipeline/08_export_stage_references.py --out /tmp/refs295 --segmentos \
    $(python3 -c "import csv; print(' '.join(l['segmento'] for l in csv.DictReader(open('reports/modelo_final/escores_referencia.csv'))))")

# 2. Saídas do C (no repositório do firmware)
cd ../lab3-firmware-stm32
cmake -S tests/host/harness -B build/harness -G Ninja && cmake --build build/harness
./build/harness/exportar_saidas_c /tmp/refs295 build/saidas_c

# 3. Comparação e tabelas
python3 tests/host/validacao/validar_blocos.py --refs /tmp/refs295 --saidas build/saidas_c --md build/validacao.md
```

As referências dos 295 segmentos ocupam 180 MB e não são versionadas. Elas saem iguais às 5 versionadas da entrada até o espectro. Do Mel em diante, diferem em até 10⁻¹³ relativo, por causa da ordem da soma em float64 da biblioteca de álgebra linear de cada máquina. Isso fica 7 ordens de grandeza abaixo das tolerâncias.

**Versões usadas nesta validação:** firmware `219829c` (main, depois do PR #4); Python `c158f2a` (main, depois do PR #26); modelo `exp239` (JSON `ee0c3e088366`); numpy 2.5.3 e scipy 1.18.1; gcc do PC sem FMA, salvo onde indicado.
