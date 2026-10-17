# lab3-firmware-stm32

Firmware do projeto de **diagnóstico acústico de motores** (ENGG54, Laboratório Integrado III, UFBA), Fase 2: porte embarcado para o STM32F411CEU6.

O protótipo em Python (extração de características, treino e escolha do classificador) fica em [`claraamp/diagnostico-acustico-motores`](https://github.com/claraamp/diagnostico-acustico-motores). Este repositório guarda só o que roda na placa e o diagrama de circuito.

## Estado atual

O firmware tem a base da Fase 2 (Frente 1: placa, serial, LEDs, CMSIS-DSP e DWT) e o **marco de 13/10**: a LDA roda na placa com as características de referência do Python. A cadeia de MFCC ainda não roda na placa; por isso as 26 características vêm prontas do Python, e o marco confere o porte do classificador, não o desempenho (os segmentos são os do treino do modelo final). O porte do MFCC começou em `Core/Src/mfcc_estagios.c` e é conferido no PC, estágio a estágio, pelo harness (seção "Harness no PC"). Os blocos 1 a 3 (janela, espectro e Mel) já rodam na placa, com a saída conferida contra o Python pela serial (seção "Blocos 1 a 3 na placa").

Ao ligar, a placa:

1. manda `hello frente1`, `fft_init status=0` e uma linha da FFT de teste (seno no bin 32):

   ```
   fft512 ciclos=35677 us=356.8 pico_bin=32 mag=256.0
   ```

   O pico no bin 32 com magnitude 256,0 (= N/2) é o valor teórico esperado.
2. só se o firmware foi compilado com `-DBLOCOS13=ON` (desligada por padrão), roda os **blocos 1 a 3** nos 5 segmentos de referência e manda a saída de cada bloco em binário, para o `receber_blocos13.py` (seção "Blocos 1 a 3 na placa"). Leva cerca de 100 s a 115200 baud, e o marco e os LEDs só começam depois;
3. **confere a LDA em todos os segmentos** do `lda_vetores_teste.h`: calcula o escore com o `lda_escore()` do `lda_modelo.h`, compara com o escore do `escores_referencia.csv` e manda uma linha por segmento, um resumo e `MARCO OK` ou `MARCO FALHOU`:

   ```
   marco13 modelo=exp239 json=ee0c3e088366 n_features=26 segmentos=295 clock=100MHz
   seg seg=0 grav=normal ref=-1577.959 placa=... dif=... tol=... dec=normal OK ciclos=...
   ...
   resumo ok=295/295 decisoes_iguais=295/295 dif_max=... (seg ...) dif/tol_max=...% ciclos min=... med=... max=... (med ... us)
   MARCO OK
   ```

   Um segmento passa quando a decisão é a do CSV e `dif` ≤ `tol`. A tolerância de cada segmento vem do gerador: é o limite do arredondamento da soma em float32, o mesmo do `06`, mais o arredondamento do escore de referência para float32. Os `ciclos` são os do `lda_escore()` com a chamada, medidos pelo DWT; o `marco_lda.c` é sempre compilado com `-O2`, inclusive no Debug, para o tempo ser o do firmware final.
4. em laço, **demonstração nos LEDs**: para cada gravação, o segmento de menor |escore| (o mais perto da fronteira). LED verde = normal, vermelho = falha, por 2 s, com uma linha `demo ...` na serial. Se a conferência falhou, os dois LEDs piscam juntos e rápido (isso não é uma decisão) e a serial repete o aviso. O LED da placa (PC13) fica aceso depois da conferência.

### Cabeçalhos do modelo

Os três vêm do repositório do Python e não se editam a mão:

| Arquivo em `Core/Inc/` | Gerado por (em `diagnostico-acustico-motores`) |
|---|---|
| `lda_modelo.h` | `scripts/pipeline/06_export_lda_header.py` → `reports/modelo_final/lda_modelo.h` |
| `lda_vetores_teste.h` | `scripts/pipeline/07_export_lda_test_vectors.py` → `reports/modelo_final/lda_vetores_teste.h` |
| `clipes_demo.h` | `scripts/pipeline/09_export_clips.py` → `reports/c_reference/clipes_demo.h` (Frente 3C) |

O `clipes_demo.h` só declara os vetores; o `Core/Src/clipes_demo.c` é o único `.c` que os define (com `CLIPES_DEMO_DADOS`), para o áudio entrar na flash uma vez só. O que nenhum código usa sai no link (`--gc-sections`).

Se o modelo final mudar, regenere e copie os três. O firmware recusa uma combinação de modelos diferentes: compara o `lda_bias` do `lda_modelo.h` com o `VT_LDA_BIAS` do outro e, se forem diferentes, manda um `ERRO` e pisca os dois LEDs. Com os 295 segmentos, os vetores ocupam cerca de 34 KB de flash (34.250 B no ELF); `--por-gravacao K` no gerador reduz.

### Teste no PC

A comparação com a referência e a formatação da serial compilam sem a HAL:

```bash
gcc -std=c11 -O2 -Wall -Wextra -Werror -Wdouble-promotion -DMARCO_HOST \
    -ICore/Inc tests/host/test_marco_lda.c Core/Src/marco_lda.c -lm -o build/test_marco_lda
./build/test_marco_lda
```

No PC o gcc não funde multiplicação e soma; no Cortex-M4 funde (`vfma.f32`). Os dois ficam dentro da tolerância, mas os escores não são bit a bit iguais.

## Hardware

| Item | Uso |
|---|---|
| WeAct Black Pill (STM32F411CEU6) | Cortex-M4F a 100 MHz (cristal HSE de 25 MHz), 512 KB de flash, 128 KB de RAM |
| ST-Link V2 | gravação e debug por SWD; também alimenta a placa com 3,3 V |
| Adaptador USB-TTL (3,3 V) | serial com o computador |
| 2 LEDs (verde e vermelho) + 2 resistores de 220 Ω | sinalização local (cerca de 6 mA por LED) |

### Pinos

| Pino | Função | Ligação |
|---|---|---|
| PA9 | USART1_TX | RXD do adaptador |
| PA10 | USART1_RX | TXD do adaptador |
| PB0 | LED_G | 220 Ω → LED verde → GND |
| PB1 | LED_R | 220 Ω → LED vermelho → GND |
| PC13 | LED_PLACA | LED da própria placa (acende em nível baixo) |
| PA13 / PA14 | SWDIO / SWCLK | ST-Link |
| PH0 / PH1 | cristal HSE | já ligado na placa |

Serial: **115200 baud, 8N1**. TX e RX são cruzados entre a placa e o adaptador, e só o GND é comum: não ligue o 3V3 nem o 5V do adaptador na placa. Deixe o USB-C da Black Pill desconectado enquanto o ST-Link estiver alimentando a placa.

O diagrama de circuito está em [`hardware/frente1-circuito/`](hardware/frente1-circuito/) (projeto do KiCad, com o símbolo da Black Pill na biblioteca `frente1.kicad_sym` e o esquema exportado em SVG).

## Estrutura

```
frente1.ioc                  configuração do STM32CubeMX (pinos, clock, USART1)
Core/                        código da aplicação (main.c e periféricos gerados pelo CubeMX;
                             marco_lda.c/.h e os cabeçalhos do modelo, lda_modelo.h e lda_vetores_teste.h;
                             mfcc_estagios.c/.h, os estágios da extração de características
                             portados para C; blocos13.c/.h, o teste dos blocos 1 a 3 na
                             placa; clipes_demo.h/.c, os clipes de áudio da Frente 3C)
tests/host/                  teste no PC da parte do marco que não usa a HAL
tests/host/harness/          harness no PC: confere o porte estágio a estágio contra o Python
tests/host/blocos13/         gerador da entrada, emulador e receptor do teste dos blocos 1 a 3
Drivers/                     HAL do STM32F4, CMSIS e CMSIS-DSP (código-fonte)
cmake/                       toolchain e arquivos gerados pelo CubeMX
CMakeLists.txt               build do projeto e da biblioteca CMSISDSP
CMakePresets.json            presets Debug e Release
STM32F411XX_FLASH.ld         linker script
hardware/frente1-circuito/   diagrama de circuito (KiCad)
```

Ao mexer no código gerado pelo CubeMX (por exemplo, `main.c`), escreva só dentro dos blocos `/* USER CODE BEGIN ... */` e `/* USER CODE END ... */`. O que estiver fora deles é apagado quando o `.ioc` é regenerado. Pelo mesmo motivo, não edite `cmake/stm32cubemx/`: o que é nosso fica no `CMakeLists.txt` da raiz.

## Como compilar e gravar

### Ferramentas

- **STM32CubeCLT**: compilador `arm-none-eabi-gcc`, CMake, Ninja, servidor GDB do ST-Link e `STM32_Programmer_CLI`.
- **STM32CubeMX**: só se for mudar pinos ou clock no `.ioc`.
- Editor: usamos o **VS Code** com a extensão **STM32Cube**. O CubeIDE também funciona, importando o projeto CMake.

### No VS Code

1. Abra a pasta do repositório.
2. Paleta de comandos (Ctrl+Shift+P) → **CMake: Select Configure Preset** → **Debug**.
3. **CMake: Build** (ou F7).
4. Com o ST-Link ligado, F5 e escolha **STM32Cube: STM32 Launch STLink GDB Server**. Ele grava e para no `main()`.

### Pelo terminal

```bash
cmake --preset Debug
cmake --build --preset Debug
STM32_Programmer_CLI -c port=SWD -w build/Debug/frente1.elf -v -rst
```

### Ver a serial (Linux)

```bash
sudo usermod -aG dialout $USER     # uma vez só; depois saia e entre na sessão
picocom -b 115200 /dev/ttyUSB0     # sair: Ctrl+A e depois Ctrl+X
```

## CMSIS-DSP

O pacote do STM32Cube para F4 (V1.28.3) traz o CMSIS-DSP só como código-fonte, sem biblioteca pronta. Por isso ele foi copiado para `Drivers/CMSIS/DSP/` e é compilado como a biblioteca estática `CMSISDSP`, definida no `CMakeLists.txt` da raiz. Quem clonar o repositório não precisa instalar mais nada.

Duas decisões que valem para todo o projeto:

- **A `CMSISDSP` compila com `-O3` mesmo no preset Debug**, para a medição de ciclos ser parecida com a do firmware final. O resto do projeto continua em `-O0` no Debug.
- **As tabelas de FFT estão restritas à FFT real de 512 pontos**, pelos defines `ARM_DSP_CONFIG_TABLES` e companhia, na `CMSISDSP`. A inicialização genérica puxava as tabelas de todos os tamanhos e ocupava 83,8 KB de flash; restrita, o DSP ocupa 10,9 KB. **Com esses defines, qualquer tamanho de FFT diferente de 512 faz a `arm_rfft_fast_init_f32` retornar erro** (`status=-1`). Para usar outro tamanho (por exemplo, a FFT de 4.096 pontos do pente harmônico), acrescente os `ARM_TABLE_*` daquele tamanho no `CMakeLists.txt`.

## Harness no PC (porte da extração de características)

O porte da cadeia de MFCC é conferido no PC, estágio a estágio, antes de ir para a placa. O harness em `tests/host/harness/` compila os estágios de `Core/Src/mfcc_estagios.c` com a **implementação genérica em C da CMSIS-DSP** (o mesmo código-fonte de `Drivers/CMSIS/DSP`, sem `ARM_MATH_CM4` nem intrínsecos do Cortex-M) e os compara com as referências do Python.

**De onde vêm as referências e as tolerâncias.** Do repositório do Python ([`diagnostico-acustico-motores`](https://github.com/claraamp/diagnostico-acustico-motores)), em `reports/c_reference/estagios/`, geradas pelo `scripts/pipeline/08_export_stage_references.py`. São a saída em float64 de cada estágio para um segmento de cada gravação, mais o `tolerancias.txt`. As tolerâncias e a métrica de cada estágio estão no `docs/contrato_numerico.md` de lá, seção "Tolerâncias por estágio (A4)". Não é preciso rodar o Python: as referências são versionadas. Basta clonar os dois repositórios **lado a lado**:

```
Lab3/
├── diagnostico-acustico-motores/
└── lab3-firmware-stm32/
```

### Compilador do PC

O harness usa o compilador do PC, não o `arm-none-eabi-gcc`. O CMake e o Ninja do STM32CubeCLT servem.

- **Linux:** `sudo apt install gcc cmake ninja-build`.
- **Windows:** o CubeCLT não traz compilador para o PC. A opção mais simples é o **WinLibs** (gcc do MinGW-w64): `winget install BrechtSanders.WinLibs.POSIX.UCRT`, e depois abrir um terminal novo. Confira com `gcc --version`. O MSYS2 (`pacman -S mingw-w64-ucrt-x86_64-gcc`) também serve. O harness foi compilado com gcc no Linux e com MinGW-w64; o MSVC não foi testado.

### Compilar e rodar

Na raiz deste repositório:

```bash
cmake -S tests/host/harness -B build/harness -G Ninja
cmake --build build/harness
./build/harness/harness                 # Linux
build\harness\harness.exe               # Windows (PowerShell ou cmd)
```

Sem argumentos, ele lê `../diagnostico-acustico-motores/reports/c_reference/estagios`. Opções:

- `--refs PASTA`: as referências em outro lugar;
- `--modo isolado|encadeado|ambos` (padrão: `ambos`);
- `-v`: uma linha por segmento.

### Como ler a saída

Para cada modo, uma linha por estágio com:

- `erro_abs`: o maior `|c − r|`;
- `erro_rel`: o maior `|c − r|` dividido pelo maior `|r|` do estágio;
- `metrica_A4`: a métrica do contrato para aquele estágio;
- `tol` e `razao`: a tolerância e quanto dela foi usado;
- o pior segmento;
- a situação: `OK`, `FALHOU` ou `nao portado`.

```
modo isolado (cada estagio recebe a referencia do anterior)
estagio   metrica      erro_abs   erro_rel  metrica_A4      tol   razao  pior segmento            situacao
escala    rel_quadro  2.955e-08  4.332e-08   4.332e-08    1e-06    4.3%  normal_seg0029           OK
janela    rel_quadro  1.036e-07  1.246e-07   1.654e-07    5e-06    3.3%  bpfi_1.0mm_seg0147       OK
espectro  rel_quadro  2.739e-04  3.035e-07   3.745e-07    5e-06    7.5%  bpfo_0.3mm_seg0206       OK
mel       rel_quadro          -          -           -    5e-06       -  -                        nao portado
...
```

- **Isolado:** cada estágio recebe a referência do anterior, em float32. Confere um bloco mesmo com a cadeia incompleta.
- **Encadeado:** a cadeia em C desde o `int16`, como na placa, até o primeiro estágio não portado.

O programa termina com `HARNESS OK`, código 0, se todos os estágios portados ficaram dentro da tolerância. Termina com `HARNESS FALHOU`, código 1, se algum passou dela. Termina com código 2 se não conseguiu usar as referências: faltam o `tolerancias.txt` ou o `modelo.txt`, o `tolerancias.txt` tem uma métrica desconhecida (só valem `rel_quadro`, `abs` e `rel`), as referências são de outro modelo, um arquivo não tem a forma esperada do estágio (por exemplo, a janela tem que ser 98 × 320) ou uma referência tem zero onde a métrica do A4 não se aplica (o `dsp.erro_estagio` do Python recusa o mesmo caso).

Hoje estão portados a escala, a janela, o espectro (FFT real de 512 da CMSIS, com o empacotamento de DC e Nyquist), o banco de Mel (bloco 3, esparso) e a média e o desvio (Welford, bloco 6). O escore é conferido com o `lda_modelo.h` do marco de 13/10.

**Um quadro por vez.** Os estágios trabalham com um quadro de 320 amostras de cada vez, e a média e o desvio são acumulados quadro a quadro (`mfcc_resumo_acumular`), porque o segmento inteiro não cabe na RAM da F411 (128 KB): os 98 quadros janelados ocupariam 125 KB, e o espectro de todos, mais 100 KB. Assim, a cadeia usa cerca de 10 KB: 7,3 KB fixos do `mfcc_estagios.c` (a janela, os buffers da FFT e os 479 pesos do Mel) e 2,5 KB de pilha no laço (medidos com `-fstack-usage` no Cortex-M4). O laço da placa está no comentário do `Core/Inc/mfcc_estagios.h`. O áudio pode ficar na flash, no caso de clipes gravados, ou num buffer circular de 320 amostras `int16`, no caso de áudio ao vivo. O harness faz o mesmo laço e só junta os quadros em buffers grandes, no PC, para comparar com a referência do segmento.

**Banco de Mel esparso.** Os 20 triângulos guardam só os pesos não nulos: o filtro `m` começa no bin `l` e tem `r − l` pesos, num vetor único de 479 floats (1,9 KB), em vez da matriz densa 20 × 257 (20 KB). As 22 bordas são constantes copiadas do contrato ("Valores exatos para conferir"); o `test_mel_bordas` (compilado com o harness) as recalcula pela fórmula HTK em float64 e confere, e falha se algum filtro ficar degenerado:

```bash
./build/harness/test_mel_bordas     # MEL BORDAS OK (22 bordas, 479 pesos)
```

### Portar um estágio novo (log e DCT: blocos 4 e 5)

1. Escreva a função em `Core/Src/mfcc_estagios.c` (declarada em `Core/Inc/mfcc_estagios.h`), sem HAL, seguindo a linha do estágio no `docs/contrato_numerico.md`. Ela recebe **um quadro** e devolve o resultado desse quadro (por exemplo, `mfcc_log(const float mel[20], float logmel[20])`), sem buffers do segmento inteiro.
2. No `harness.c`, marque o estágio em `PORTADO[]`.
3. No `harness.c`, chame a função no laço de quadros de `rodar_isolado()`, com a referência do estágio anterior, e no laço de `rodar_encadeado()`, com a saída em C do anterior.
4. Recompile e rode. O estágio passa a ter linha própria, nos dois modos, contra a tolerância do A4.

Se o estágio falhar, a coluna "Onde o porte costuma divergir" do contrato lista as causas mais comuns: Slaney em vez de HTK, `log10`, DCT sem a escala ortonormal e outras.

## Blocos 1 a 3 na placa

O `Core/Src/blocos13.c` roda na placa a janela (escala e Hann), o espectro de potência e o banco de Mel no **primeiro segmento de cada clipe** do `clipes_demo.h`. São, bit a bit, os mesmos 5 segmentos das referências do harness (`normal_seg0029`, `bpfi_0.3mm_seg0088` e os outros), então o teste não precisa de cópia própria do áudio. Para cada segmento, ele:

1. mede com o DWT os ciclos de cada bloco em cada um dos 98 quadros, os três blocos juntos num quadro e o segmento inteiro (uma passada sem a serial, descontando o custo de ler o contador);
2. manda a saída de cada bloco pela serial em binário (float32, quadro a quadro), com o CRC-32 de cada bloco. O protocolo está no comentário do `Core/Inc/blocos13.h`.

O `tests/host/blocos13/receber_blocos13.py` recebe tudo, confere modelo, entrada e CRC, grava cada estágio em `.f32` e compara com as referências em float64 pela métrica e pela **tolerância encadeada** do A4 (a placa roda a cadeia desde o `int16`, como o modo encadeado do harness).

### Rodar

Precisa de Python 3 com `numpy` e `pyserial` (`pip install numpy pyserial`), e dos dois repositórios lado a lado, como no harness.

1. Compile com o teste ligado e grave. A opção `BLOCOS13` vem **desligada** por padrão, porque o envio atrasa o boot em ~100 s, e numa demonstração a placa parece travada:

   ```bash
   cmake --preset Debug -DBLOCOS13=ON
   cmake --build --preset Debug
   ```

   O valor fica guardado no cache do build (`build/Debug`). Depois do teste, volte com `cmake --preset Debug -DBLOCOS13=OFF` antes de gravar o firmware normal.
2. **Feche o picocom** ou qualquer outro programa na porta: só um programa abre a serial por vez, e o binário aparece como lixo num terminal.
3. Rode o receptor e, quando ele pedir, aperte o RESET da placa:

   ```bash
   python3 tests/host/blocos13/receber_blocos13.py --porta /dev/ttyUSB0   # Linux
   python  tests/host/blocos13/receber_blocos13.py --porta COM5           # Windows
   ```

4. Em cerca de 100 s ele termina com `BLOCOS13 OK` (código 0) ou `BLOCOS13 FALHOU` (código 1). A captura bruta, os `.f32` de cada estágio e o `resultado.txt` ficam em `build/blocos13/`.

Para comparar de novo uma captura já gravada, sem a placa: `--arquivo build/blocos13/captura.bin`.

### Sem a placa (testar o receptor)

O `emular_blocos13` (compilado com o harness) roda o mesmo `blocos13_rodar()` no PC e grava a captura que a placa mandaria, com os ciclos zerados:

```bash
./build/harness/emular_blocos13 build/captura_pc.bin
python3 tests/host/blocos13/receber_blocos13.py --arquivo build/captura_pc.bin --saida build/blocos13_pc
```

### Segmentos e entrada

A placa manda o CRC-32 das 12.800 amostras `int16` de cada segmento (`entrada_crc32`), e o receptor confere com o `entrada.i16` da referência. Se os clipes e as referências forem regenerados de modelos ou segmentos diferentes, o receptor recusa a captura, em vez de comparar entradas diferentes. Capturas feitas antes de 17/10, quando a entrada vinha de uma cópia própria (`blocos13_entrada.h`, removido), mandavam o SHA-256 da entrada, e o receptor ainda as aceita.

### Custo

Números do build Debug, em bytes, com KiB entre parênteses. O relatório do linker escreve "KB", mas usa KiB: a flash da F411 é "512 KB" = 524.288 B.

| Build | Flash | RAM |
|---|---|---|
| `-DBLOCOS13=ON` | 322.840 B (315 KiB, 62 %) | 28.352 B (27,7 KiB, 22 %) |
| `-DBLOCOS13=OFF` (padrão) | 60.812 B (59,4 KiB, 12 %) | 7.216 B (7,0 KiB, 5,5 %) |

- **Com o teste ligado:** 256.000 B (250 KiB) da flash são os 5 clipes de 2 s. O teste usa o primeiro segundo de cada um, mas o vetor de cada clipe entra inteiro. Da RAM, 7.840 B guardam os ciclos de todos os quadros para o resumo.
- **O recorte a 51,2 kHz** do `clipes_demo.h` (102.696 B) só entra quando a decimação embarcada o usar.
- **Com o teste desligado** e sem a integração dos clipes, nada do `clipes_demo.h` é usado, e o `--gc-sections` tira tudo do link.
- **A flash varia ~1,6 KB entre máquinas** com versões diferentes da toolchain: o mesmo commit deu 322.800, 322.840 e 324.416 B com o teste ligado.

## Medições

| Medida | Valor |
|---|---|
| FFT real de 512 pontos (float32), medida com o contador DWT | 35.677 ciclos = 356,8 µs a 100 MHz |
| Blocos 1 a 3 por quadro (janela, espectro e Mel), DWT, mediana dos 490 quadros | 56.240 ciclos = 562 µs (janela 10.723, espectro 40.632, Mel 4.830) |
| Blocos 1 a 3 no segmento de 1 s (98 quadros), DWT, mediana dos 5 segmentos | 5.509.208 ciclos = 55,1 ms (5,5 % do tempo real) |
| Flash usada (firmware atual) | 24.592 B de 512 KB (4,7%) |
| RAM usada | 7.216 B de 128 KB (5,5%) |

Um quadro de 320 amostras a 12,8 kHz dura 25 ms; a FFT ocupa cerca de 1,5% desse tempo.

**Orçamento da flash para o áudio:** reservando 64 KB para o resto do firmware (banco de filtros de Mel, DCT, `.h` da LDA, pente harmônico se for adotado e margem), sobram cerca de 434 KB. Em PCM int16 a 12,8 kHz (25.600 B por segundo), isso dá **cerca de 17 s de áudio decimado**. Com 1 s a 51,2 kHz para a demonstração da decimação na placa, sobram cerca de 13 s a 12,8 kHz. A reserva é uma estimativa e deve ser revista quando a matriz de Mel e o pente estiverem definidos.

## Próximos passos

- **Marco de 13/10:** a placa recebe as 26 características de referência de alguns segmentos, calcula o escore com o `.h` da LDA, confere com o `escores_referencia.csv`, acende o LED e manda o resultado pela serial.
- **13 a 19/10:** porte da cadeia de DSP (janela, FFT, MFCC) para C, com verificação bloco a bloco contra as referências em Python, e definição do formato do pacote da serial.
