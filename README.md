# lab3-firmware-stm32

Firmware do projeto de **diagnóstico acústico de motores** (ENGG54, Laboratório Integrado III, UFBA), Fase 2: porte embarcado para o STM32F411CEU6.

O protótipo em Python (extração de características, treino e escolha do classificador) fica em [`claraamp/diagnostico-acustico-motores`](https://github.com/claraamp/diagnostico-acustico-motores). Este repositório guarda só o que roda na placa e o diagrama de circuito.

## Estado atual

O firmware tem a base da Fase 2 (Frente 1: placa, serial, LEDs, CMSIS-DSP e DWT), o **marco de 13/10** (a LDA na placa com as características de referência do Python) e, desde 20/10, a **cadeia completa no firmware normal**: os clipes de áudio da flash (`clipes_demo.h`) passam pelo `mfcc_segmento()` e pelo `lda_escore()` na placa, do `int16` à decisão, e a demonstração nos LEDs e o pacote `$SEG` (origem `C`) saem dessa inferência (seção "Cadeia completa na placa"). Também desde 20/10, a **decimação embarcada** (bloco 0): o recorte de 1 s a 51,2 kHz passa pelo `arm_fir_decimate_f32` na placa, é comparado com o `int16` decimado do Python e segue pela mesma cadeia até o escore (seção "Decimação na placa"). Desde 22/10, a **primeira inferência de ponta a ponta** roda no laço normal: a cada volta da demonstração, o recorte de 51,2 kHz passa pela decimação e pela cadeia até o escore, acende o LED da decisão e sai na serial como `$SEG` de origem `D` (seção "Primeira inferência de ponta a ponta"). Desde 30/10, a **recepção de segmentos pela serial**: um `RECEBER` do PC troca a demonstração pelo modo de recepção, e a placa passa pela mesma cadeia qualquer segmento de 1 s a 12,8 kHz mandado pelo adaptador USB-serial, com o `$SEG` de origem `S`; os 10 segmentos dos clipes mandados pelo PC dão, na placa, o escore bit a bit igual ao do clipe da flash (seção "Recepção de segmentos pela serial"). O porte do MFCC está em `Core/Src/mfcc_estagios.c` e foi conferido no PC, estágio a estágio, pelo harness (seção "Harness no PC"), e na placa pela validação bloco a bloco ([`docs/validacao-blocos.md`](docs/validacao-blocos.md)). Os segmentos dos clipes são os do treino do modelo final: a demonstração mostra a fidelidade do porte, não o desempenho.

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
   $CAB,1,exp239,ee0c3e088366,26,100*3B
   seg seg=0 grav=normal ref=-1577.959 placa=... dif=... tol=... dec=normal OK ciclos=...
   ...
   resumo ok=295/295 decisoes_iguais=295/295 dif_max=... (seg ...) dif/tol_max=...% ciclos min=... med=... max=... (med ... us)
   MARCO OK
   ```

   Um segmento passa quando a decisão é a do CSV e `dif` ≤ `tol`. A tolerância de cada segmento vem do gerador: é o limite do arredondamento da soma em float32, o mesmo do `06`, mais o arredondamento do escore de referência para float32. Os `ciclos` são os do `lda_escore()` com a chamada, medidos pelo DWT; o `marco_lda.c` é sempre compilado com `-O2`, inclusive no Debug, para o tempo ser o do firmware final.
4. **roda a cadeia completa nos 10 segmentos dos clipes** (`cadeia_verificar()`, ~1,3 s): para cada um, o `mfcc_segmento()` e o `lda_escore()` do `int16` ao escore, conferido com o `escores_referencia.csv` pela tolerância encadeada do escore no A4 (1e-5 relativo), e os ciclos de cada bloco pelo DWT. Termina com o resumo, a mediana de cada bloco e `CADEIA OK` ou `CADEIA FALHOU`, e manda o `$CAB` de novo (seção "Cadeia completa na placa");
5. **decima o recorte de 51,2 kHz** (`decimacao_verificar()`): confere a fase do `arm_fir_decimate_f32` por um impulso, decima o segmento 29 do clipe normal passando cada passo de quadro pela cadeia até o escore, compara a saída com o `int16` do Python e mede os ciclos. Termina com `DECIMACAO OK duracao=... ms` ou `DECIMACAO FALHOU` (seção "Decimação na placa");
6. em laço, **demonstração nos LEDs com a cadeia**: cada volta começa pela **inferência de ponta a ponta** (o recorte de 51,2 kHz decimado e passado pela cadeia até o escore, com uma linha `ponta ...` e o `$SEG` de origem `D`; seção "Primeira inferência de ponta a ponta") e segue pelos 10 segmentos dos clipes em ordem, cada um processado de novo na hora (`mfcc_segmento()` → `lda_escore()`). LED verde = normal, vermelho = falha, por 2 s, com uma linha `demo ...` e o pacote `$SEG` de origem `C` na serial (seção "Pacote da serial"). Os `ciclos` da linha `demo` e do pacote vão do `int16` ao escore. Se o marco, a cadeia **ou** a decimação falharam, os dois LEDs piscam juntos e rápido (isso não é uma decisão) e a serial repete o aviso. A trava nas três conferências é de propósito: se só o marco falhar (a LDA ou os cabeçalhos do modelo incoerentes), a demonstração não roda, mesmo com a `CADEIA OK`, porque ela usa o mesmo `lda_escore()`; e se só a decimação falhar, o firmware tem um erro de porte que só a serial mostraria, e numa demonstração ninguém olha a serial. O mesmo padrão aparece num segmento cujo pacote sai com erro (escore não finito, `disparou = 4`). O LED da placa (PC13) fica aceso depois da conferência.

   A demonstração com as características prontas (`marco_lda_passo()`, origem `R`) foi removida em 20/10; o marco ficou só como conferência no boot.

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
    -ICore/Inc tests/host/test_marco_lda.c Core/Src/marco_lda.c Core/Src/texto_fixo.c \
    Core/Src/pacote_serial.c -lm -o build/test_marco_lda
./build/test_marco_lda
```

O pacote da serial tem o seu próprio teste (seção "Pacote da serial"), e a cadeia completa também (seção "Cadeia completa na placa").

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
                             placa; clipes_demo.h/.c, os clipes de áudio da Frente 3C;
                             validacao_placa.c/.h, o teste do segmento completo na placa;
                             cadeia_clipes.c/.h, a cadeia completa do firmware normal
                             (conferência no boot, ciclos por bloco e o laço dos LEDs);
                             decimacao.c/.h, a decimação embarcada do recorte de 51,2 kHz;
                             pacote_serial.c/.h, o formato do pacote da serial;
                             crc32.c/.h, o CRC-32 do zlib, comum ao blocos13, à
                             decimação e à recepção pela serial;
                             recepcao.c/.h, o parser do quadro da recepção pela serial;
                             recepcao_serial.c/.h, o modo de recepção na placa (DMA e respostas);
                             texto_fixo.c/.h, float em texto sem o %f)
tests/host/                  testes no PC da parte do marco e do pacote que não usa a HAL,
                             testes das tabelas (test_mel_bordas.c, test_log_dct.c), do
                             CRC-32 comum (test_crc32.c) e do parser da recepção (test_recepcao.c)
tests/host/pacote/           leitor dos pacotes da serial (referência para o painel)
tests/host/harness/          harness no PC: confere o porte estágio a estágio contra o Python
tests/host/blocos13/         emulador e receptor do teste dos blocos 1 a 3
tests/host/validacao/        validação independente bloco a bloco (exportador do C e comparador em Python)
tests/host/recepcao/         script mínimo do PC para a recepção pela serial e o emulador da placa (stub/)
tests/host/decimacao/        a decimação em C contra o Python em float64: o recorte (comparar_decimacao.py)
                             e as gravações inteiras (decimar_gravacao.c e tolerancias_gravacoes.py)
docs/validacao-blocos.md     tabela de erros por bloco (PC, 295 segmentos) e o teste na placa
tools/                       orcamento_memoria.py: flash e RAM por parte, lidas do .map
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
- **A `CMSISDSP` compila com `ARM_MATH_LOOPUNROLL`** (desde a revisão do PR #9, 20/10): os laços são desenrolados, e o `arm_fir_decimate_f32` calcula 4 saídas por vez, com 4 acumuladores. Sem ele, o FIR da decimação levava ~9,1 ciclos por coeficiente. Nas funções que a cadeia usa (`arm_mult_f32`, `arm_cmplx_mag_squared_f32`, `arm_dot_prod_f32`) e no decimador, a ordem das contas é a mesma, e as saídas são bit a bit as mesmas (conferido no harness pela Amanda e nos testes de host); só os ciclos mudam. O harness do PC usa o mesmo define.
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
mel       rel_quadro  7.942e-04  2.051e-07   3.110e-07    5e-06    6.2%  bpfi_1.0mm_seg0147       OK
logmel    abs         4.266e-07  5.164e-08   4.266e-07    1e-05    4.3%  bpfi_1.0mm_seg0147       OK
mfcc      abs         2.372e-06  2.449e-07   2.372e-06    5e-05    4.7%  bpfo_1.0mm_seg0265       OK
...
```

- **Isolado:** cada estágio recebe a referência do anterior, em float32. Confere um bloco mesmo com a cadeia incompleta.
- **Encadeado:** a cadeia em C desde o `int16`, como na placa, até o escore. No fim, o harness também confere que o `mfcc_segmento()`, a chamada única que a placa usa, dá bit a bit as mesmas 26 características do laço estágio a estágio.

O programa termina com `HARNESS OK`, código 0, se todos os estágios portados ficaram dentro da tolerância. Termina com `HARNESS FALHOU`, código 1, se algum passou dela. Termina com código 2 se não conseguiu usar as referências: faltam o `tolerancias.txt` ou o `modelo.txt`, o `tolerancias.txt` tem uma métrica desconhecida (só valem `rel_quadro`, `abs` e `rel`), as referências são de outro modelo, um arquivo não tem a forma esperada do estágio (por exemplo, a janela tem que ser 98 × 320) ou uma referência tem zero onde a métrica do A4 não se aplica (o `dsp.erro_estagio` do Python recusa o mesmo caso).

Todos os estágios estão portados: a escala, a janela, o espectro (FFT real de 512 da CMSIS, com o empacotamento de DC e Nyquist), o banco de Mel (bloco 3, esparso), o log (bloco 4), a DCT (bloco 5) e a média e o desvio (Welford, bloco 6). O escore é conferido com o `lda_modelo.h` do marco de 13/10. Nos 5 segmentos, o pior estágio usa 7,8 % da tolerância, e o escore encadeado, do `int16` à LDA, erra 3,4 × 10⁻⁷ relativo (3,4 % da tolerância de 10⁻⁵):

| Estágio | Isolado | Encadeado | Tolerância (isolado / encadeado) |
|---|---:|---:|---:|
| logmel (abs) | 4,3 × 10⁻⁷ | 5,9 × 10⁻⁶ | 1 × 10⁻⁵ / 2 × 10⁻⁴ |
| mfcc (abs) | 2,4 × 10⁻⁶ | 4,0 × 10⁻⁶ | 5 × 10⁻⁵ / 1 × 10⁻⁴ |
| media (abs) | 2,1 × 10⁻⁶ | 1,9 × 10⁻⁶ | 1 × 10⁻⁴ |
| desvio (rel) | 3,0 × 10⁻⁷ | 3,5 × 10⁻⁷ | 5 × 10⁻⁵ |
| escore (rel) | 2,3 × 10⁻⁷ | 3,4 × 10⁻⁷ | 5 × 10⁻⁶ / 1 × 10⁻⁵ |

**Log (bloco 4).** `mfcc_log(mel, logmel)`: `logf(mel + 1e-10f)`, com o épsilon **somado**, não como piso, como no contrato. Usa o `logf` da biblioteca C, e não a `arm_vlog_f32` da CMSIS, que é uma aproximação polinomial. Pode ser chamado em lugar.

**DCT (bloco 5).** A CMSIS não tem DCT-II (a `arm_dct4_f32` é DCT-IV). `mfcc_dct(logmel, coefs)` multiplica pela matriz 13 × 20 da DCT-II ortonormal, calculada no `mfcc_estagios_init()` (1.040 B de RAM), com 13 `arm_dot_prod_f32`. O ângulo `π·k·(2n + 1)/40` é reduzido em inteiros (período 80, simetrias em 40 e 20) antes do `cosf`. Sem a redução, o ângulo chega a 36,7 rad e o arredondamento dele em float32 erra o coeficiente em até 6 × 10⁻⁷. No harness, o erro do escore encadeado sobe de 3,4 % para 15 % da tolerância.

**Média e desvio (bloco 6).** Welford, quadro a quadro, já estava portado. A escolha está no contrato ("Média e desvio em float32", `exp242`): a soma dos quadrados erra o desvio cerca de 100 vezes mais e passaria da tolerância do A4.

**O harness pega os erros clássicos destes blocos.** Testado com o código estragado de propósito: `log10f` no lugar do `logf`, a DCT sem a escala ortonormal, a DCT sem o c0 e o desvio amostral (`ddof = 1`) reprovam com erro de 100 a 10⁷ vezes a tolerância. O épsilon como piso (`max(mel, 1e-10)`) passaria, porque nenhum quadro das referências tem Mel tão pequeno; quem o pega é o `test_log_dct`.

**Um quadro por vez.** Os estágios trabalham com um quadro de 320 amostras de cada vez, e a média e o desvio são acumulados quadro a quadro (`mfcc_resumo_acumular`), porque o segmento inteiro não cabe na RAM da F411 (128 KB): os 98 quadros janelados ocupariam 125 KB, e o espectro de todos, mais 100 KB. Assim, a cadeia inteira usa cerca de 11 KB de RAM, **todos estáticos** (10,8 KB no `mfcc_estagios.c`: a janela, os buffers da FFT, os 479 pesos do Mel, a matriz da DCT e os buffers de um quadro do `mfcc_quadro_coefs()`), e por isso entram na conta de RAM que o linker imprime. A pilha do `mfcc_segmento()` fica em ~0,3 KB (medida com `-fstack-usage` no Cortex-M4). Os buffers de um quadro são estáticos de propósito: o linker só reserva 4 KB de pilha (`_Min_Stack_Size`; era 1 KB até 20/10), não avisa se a pilha real passar disso, e 2,4 KB na pilha comeriam boa parte dessa reserva. Com a cadeia e o `lda_escore()` ligados ao firmware, a RAM vai a ~18 KB e o código a mais na flash é de cerca de 2 KB, dos quais ~0,6 KB são o `logf`. As funções não são reentrantes (buffers estáticos): chame a cadeia de um só contexto. O laço da placa está no comentário do `Core/Inc/mfcc_estagios.h`. O áudio pode ficar na flash, no caso de clipes gravados, ou num buffer circular de 320 amostras `int16`, no caso de áudio ao vivo. O harness faz o mesmo laço e só junta os quadros em buffers grandes, no PC, para comparar com a referência do segmento.

**Banco de Mel esparso.** Os 20 triângulos guardam só os pesos não nulos: o filtro `m` começa no bin `l` e tem `r − l` pesos, num vetor único de 479 floats (1,9 KB), em vez da matriz densa 20 × 257 (20 KB). As 22 bordas são constantes copiadas do contrato ("Valores exatos para conferir"); o `test_mel_bordas` (compilado com o harness) as recalcula pela fórmula HTK em float64 e confere, e falha se algum filtro ficar degenerado:

```bash
./build/harness/test_mel_bordas     # MEL BORDAS OK (22 bordas, 479 pesos)
./build/harness/test_log_dct        # LOG DCT OK (matriz da DCT, épsilon somado, ddof = 0)
./build/harness/test_crc32          # TEST CRC32 OK (o CRC-32 comum contra o zlib)
```

O `test_log_dct` confere o que as referências não exercitam: a matriz da DCT contra a fórmula em float64 (a menos de 10⁻⁷) e com linhas ortonormais, o épsilon do log somado (com `mel = 1e-10`, a soma dá `ln(2e-10)` e o piso daria `ln(1e-10)`) e o desvio populacional num caso conhecido.

**Na placa:**

```c
float x[2 * MFCC_N_COEFS];          // 13 médias e 13 desvios, a ordem do modelo
if (mfcc_segmento(pcm, x) == 0) {   // as 12.800 amostras int16 de 1 s
    escore = lda_escore(x);         // lda_modelo.h
}
```

O `mfcc_segmento()` devolve −3 (e zeros) se o `mfcc_estagios_init()` não rodou ou falhou: sem ele, as tabelas ficam zeradas e o escore sairia igual ao bias da LDA, sem nenhum aviso.

O `mfcc_segmento()` é o laço do comentário do `mfcc_estagios.h`, e o `mfcc_quadro_coefs()` faz um quadro (int16 → 13 coeficientes). Para medir cada bloco com o DWT, como no `blocos13`, chame as funções de cada estágio separadas.

### Portar um estágio novo

Para um estágio que entre depois (o pente harmônico, se for adotado, por exemplo):

1. Escreva a função em `Core/Src/mfcc_estagios.c` (declarada em `Core/Inc/mfcc_estagios.h`), sem HAL, seguindo a linha do estágio no `docs/contrato_numerico.md`. Ela recebe **um quadro** e devolve o resultado desse quadro, sem buffers do segmento inteiro.
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

## Pacote da serial

Formato **v1**, definido em 17/10 a partir da decisão "O que o painel mostra no lugar de grau de confiança" (Registro de Decisões). Código em `Core/Src/pacote_serial.c`; leitor de referência para o painel em `tests/host/pacote/ler_pacotes.py`.

**Texto, uma linha por segmento de 1 s, no estilo das sentenças NMEA:** começa com `$`, campos separados por vírgula, e termina com `*` mais o checksum e `\r\n`. O checksum é o XOR de todos os bytes entre o `$` e o `*`, em 2 dígitos hexadecimais maiúsculos. Texto porque se lê direto no picocom e basta para a demonstração e o 2º parcial; o checksum porque o painel precisa descartar a linha que chegar corrompida em vez de mostrar um número errado. As outras linhas da serial (`hello`, `fft512`, `marco13`, `seg ...`, `demo ...`) não começam com `$`, e o painel as ignora.

A 115200 baud, um `$SEG` típico (~45 B) leva cerca de 4 ms, contra 1 s de áudio por segmento.

### `$CAB`: cabeçalho, uma vez ao ligar

```
$CAB,1,exp239,ee0c3e088366,26,100*3B
```

| # | Campo | Exemplo | Significado |
|---|---|---|---|
| 1 | versão | `1` | versão do formato; o painel recusa outra |
| 2 | modelo | `exp239` | id do modelo da LDA (`VT_MODELO_ID`) |
| 3 | json | `ee0c3e088366` | 12 primeiros dígitos do SHA-256 do `lda_final.json` |
| 4 | n_features | `26` | número de características |
| 5 | clock | `100` | clock da CPU em MHz, para converter os ciclos em tempo |

O painel confere o modelo antes de mostrar os segmentos. Se a placa reiniciar, o `$CAB` vem de novo.

### `$SEG`: um segmento

```
$SEG,1,7,R,2,1043,F,1,231.475,0,,,412*69
```

| # | Campo | Valores | Significado |
|---|---|---|---|
| 1 | versão | `1` | versão do formato |
| 2 | n | 0, 1, 2… | contador desde o boot; um salto mostra pacote perdido, e voltar a 0 mostra que a placa reiniciou |
| 3 | origem | `R`, `C`, `D`, `S`, `A` | `R`: características de referência do Python (marco de 13/10); `C`: clipe na flash (`clipes_demo.h`); `D`: recorte de 51,2 kHz da flash, decimado na placa (22/10); `S`: segmento mandado pelo PC no modo de recepção (30/10; seção "Recepção de segmentos pela serial"); `A`: áudio ao vivo (reservado) |
| 4 | clipe | inteiro ou vazio | `R`: índice da gravação (`vt_gravacao`); `C` e `D`: índice do clipe; `S`: grupo do quadro; `A`: vazio |
| 5 | segmento | inteiro | `R`: id do segmento no conjunto (`vt_segmento`); `C` e `D`: segmento dentro do clipe (na `D`, 0: o recorte é o primeiro segmento do clipe `normal`); `S`: id do quadro |
| 6 | decisão | `N`, `F` | normal ou falha (LDA **ou** novidade) |
| 7 | disparou | `0`–`3` ou `4` | máscara: 0 = nenhum, 1 = LDA, 2 = novidade, 3 = ambos; **4 = erro**, sempre sozinho (ver "Erro" abaixo) |
| 8 | escore da LDA | 3 casas | valor cru, **falha se ≥ 0**. Não é confiança e nunca vai em % |
| 9 | código de novidade | `0`–`4` | 0 = não adotado (também no D4); 1 = pente, série BPFO; 2 = pente, série BPFI; 3 = D2 (Mahalanobis² no MFCC); 4 = D3 (Mahalanobis² no MFCC + pente) |
| 10 | pontuação de novidade | 3 casas ou vazio | vazio com código 0. Em dB no pente, Mahalanobis² no D2/D3 |
| 11 | limiar de novidade | 3 casas ou vazio | vazio com código 0. **Falha se pontuação > limiar** (estritamente maior) |
| 12 | ciclos | inteiro | ciclos do DWT do processamento do segmento. Na origem `D`, do `int16` a 51,2 kHz ao escore (decimação, cadeia e LDA); na origem `S`, os quadros do MFCC, o resumo e a LDA, sem a espera pelos bytes; na origem `C`, do `int16` a 12,8 kHz ao escore (`mfcc_segmento()` e `lda_escore()`); na origem `R` (marco), só os do `lda_escore()` |

**Sentidos das comparações.** A LDA dá falha com escore **≥ 0** e o detector de novidade com pontuação **> limiar**, os mesmos sentidos do Python. A placa decide pelo `float`. **No escore da LDA, o sinal do texto é sempre o da decisão:** todo escore negativo sai com `-`, inclusive o que arredonda a zero (−0,0004 sai `-0.000`, decisão `N`). O `-0.0f` não é negativo (`-0.0f < 0` é falso), então sai `0.000` e é falha, como no `lda_e_falha()`. Assim, o painel pode ler a decisão da LDA pelo sinal do texto, sem exceção na fronteira; em Python, use o sinal da string, porque `float("-0.000") >= 0` é verdadeiro. O formatador do marco (`texto_fixo()`) continua sem `-0.000`; o `-` é posto só no `pacote_linha()`. Na novidade (campos reservados), quando a pontuação e o limiar arredondam para o mesmo valor, vale o campo de decisão.

**Erro (disparou = 4).** Se o escore da LDA, ou a pontuação ou o limiar de novidade com detector adotado, não for finito (NaN ou infinito), o pacote sai com `disparou = 4`, sozinho, e decisão `F`, e a placa pisca os dois LEDs juntos em vez de acender um deles. Num detector de falha, uma conta que deu errado não pode aparecer como "normal": `NaN >= 0` é falso, e sem esse bit um escore NaN sairia `N` com o LED verde. O texto do valor mostra o problema (`nan`, `inf` ou `-inf`). Hoje a cadeia não gera NaN com entrada válida (o log tem o épsilon e o Welford divide por n ≥ 1); o caso provável é futuro, a pontuação do pente com fundo zero (infinito menos infinito). Um `inf` **sem** o bit de erro também pode aparecer: é um valor finito grande demais para o texto (≥ ~4 × 10⁶ com 3 casas), e o sinal dele continua valendo. O painel mostra "erro" para o 4.

**Pente (D1/D1b), que tem duas séries.** Vai a série com a maior diferença pontuação − limiar (em dB), e o código diz qual (1 ou 2). Em empate, vai a BPFO. O painel mostra a pontuação e o limiar, não a diferença.

**O que não vai no pacote:** a razão pontuação ÷ limiar (seria lida como força da detecção) e a consistência entre segmentos ("k de N segmentos recentes indicaram falha"), que o painel calcula a partir da sequência de `$SEG`.

**Os campos de novidade estão reservados, não prometidos.** Enquanto nenhum detector for adotado, o código é 0, e a pontuação e o limiar vão vazios. Adotar um detector não muda o formato; mudar o formato exige subir a versão (`PACOTE_VERSAO`).

**A origem `D` (22/10) não muda a versão:** é um valor novo de um campo que já existia, com os mesmos campos na mesma ordem. O `ler_pacotes.py` foi atualizado junto e, no resumo, conta os segmentos e as decisões por origem. Um leitor da versão 1 anterior a esta mudança recusa o pacote `D` como "origem desconhecida": hoje isso não quebra nada, porque o único leitor é o `ler_pacotes.py`, mas a regra para o painel é aceitar as quatro origens (`R`, `C`, `D`, `A`) e, se aparecer uma origem que ele não conhece, descartar só aquele pacote, sem tratar como pacote corrompido (revisão da Júlia no PR da primeira inferência).

**O pacote `D` e o primeiro `C` de cada volta têm o mesmo clipe e segmento (0, 0):** são o mesmo segundo de áudio por dois caminhos, o `D` a partir do recorte de 51,2 kHz e o `C` a partir do clipe já decimado pelo Python. Isso serve para o painel comparar as duas decisões, mas a chave de um segmento no painel é **origem, clipe e segmento**, nunca só clipe e segmento; senão o par parece um pacote repetido. Pacote repetido de verdade se vê pelo contador `n`.

**Limites.** Com 3 casas, cada número vai até cerca de ±4 × 10⁶; acima disso, sai `inf` (`texto_fixo.h`). Esse limite é o de caber em `uint32_t`, não o de precisão: o `float` tem ~7 dígitos significativos, então acima de ~10.000 a terceira casa já não é confiável. O escore da LDA fica em ±1.600. O id do modelo não pode ter vírgula nem `*`. A maior linha possível cabe em `PACOTE_TAM_MAX` (128 B).

### Testar no PC

```bash
gcc -std=c11 -O2 -Wall -Wextra -Werror -Wdouble-promotion -DMARCO_HOST \
    -ICore/Inc tests/host/test_pacote_serial.c Core/Src/pacote_serial.c \
    Core/Src/texto_fixo.c Core/Src/marco_lda.c -lm -o build/test_pacote_serial
./build/test_pacote_serial > build/pacotes_exemplo.txt
python3 tests/host/pacote/ler_pacotes.py --arquivo build/pacotes_exemplo.txt --resumo
python3 tests/host/pacote/ler_pacotes.py --autoteste
```

O teste em C confere as linhas exatas (com o checksum), os sentidos das comparações, a escolha da série do pente, a fronteira, o tamanho máximo e o buffer pequeno, e que o pacote de cada um dos 295 segmentos do marco tem a mesma decisão do LED e do CSV. Na saída padrão, ele escreve um `$CAB` e um `$SEG` por segmento, e o leitor em Python confere o mesmo formato: os dois lados precisam concordar. O `--autoteste` confere que o leitor aceita as linhas certas e recusa as erradas (checksum, campos, versão, decisão incoerente).

### Ler da placa

```bash
python3 tests/host/pacote/ler_pacotes.py --porta /dev/ttyUSB0 --segundos 30 --salvar build/captura_pacotes.txt
```

Aperte o RESET da placa logo depois, para pegar também o `$CAB`. Com `--csv arquivo.csv`, os segmentos são gravados em CSV. Feche o picocom antes: só um programa abre a serial por vez.

## Cadeia completa na placa

Card "Portar a cadeia de DSP para C — integração da cadeia completa na placa". O `Core/Src/cadeia_clipes.c` liga a cadeia validada ao firmware normal, sem opção de CMake:

```
clipes[c].pcm → mfcc_segmento() → lda_escore() → decisão → LEDs e $SEG (origem C)
```

**No boot**, depois do marco, o `cadeia_verificar()` roda a cadeia nos 10 segmentos dos clipes e manda uma linha por segmento:

```
cadeia inicio modelo=exp239 json=ee0c3e088366 segmentos=10 origem=C clock=100MHz
cadeia seg=29 clipe=normal k=0 ref=-1331.768 placa=-1331.768 dif=0.000122 tol=0.013318 dec=normal OK ciclos=... blocos=janela:...,espectro:...,mel:...,log:...,dct:...,resumo:...,final:...,lda:... soma=... iguais=1
...
cadeia resumo ok=10/10 decisoes_iguais=10/10 iguais_blocos=10/10 dif_max=... (seg ...) dif/tol_max=...% ciclos min=... med=... max=... (med ... ms, max ... ms)
cadeia blocos_mediana janela=... espectro=... mel=... log=... dct=... resumo=... final=... lda=... soma=... (... ms)
CADEIA OK
$CAB,1,exp239,ee0c3e088366,26,100*3B
```

- **`ciclos`:** o `mfcc_segmento()` e o `lda_escore()` juntos, do `int16` ao escore, numa chamada, descontado o custo de ler o DWT. É o tempo do firmware.
- **`blocos`:** uma segunda passada no mesmo segmento, com os estágios chamados um a um, na ordem do `mfcc_quadro_coefs()`, e cada um medido em cada um dos 98 quadros (somados no segmento). `janela` é a escala e a Hann; `resumo` é o Welford quadro a quadro; `final` é a média e o desvio no fim do segmento; `lda` é o escore. A `soma` fica ~0,5 % abaixo de `ciclos`, porque as medidas não incluem o laço dos quadros nem as chamadas entre os estágios (o `mfcc_quadro_coefs()` e a volta do laço do `mfcc_segmento()`). `iguais=1` diz que essa passada deu bit a bit as mesmas 26 características que o `mfcc_segmento()`; ele aparece no resumo, mas não reprova. A passada de medida custa ~2,9 KB de RAM (os buffers de um quadro, o resumo e os ciclos dos 10 segmentos) e ~0,6 s de boot. Fica ligada em todo boot, sem opção de CMake, porque os ciclos por bloco são o insumo da caracterização de tempo de novembro e a RAM tem folga (~74 KiB livres desde a decimação, 20/10); se a RAM apertar (por exemplo, com o detector de novidade), ela é a primeira candidata a ir para uma opção como o `VALIDACAO_PLACA`.
- **Passa** quando o escore fica a até `1e-5 × |ref|` do `escores_referencia.csv` (tolerância encadeada do escore no A4) e a decisão é a do CSV. Se algum segmento falha, o laço dos LEDs mostra o padrão de erro.
- **O `$CAB` vem de novo** logo antes dos `$SEG` de origem `C`, para a demonstração não depender do `$CAB` do marco.

**No laço**, o `cadeia_passo()` mostra os 10 segmentos em ordem, 2 s cada, rodando a cadeia de novo em cada um (é uma inferência de verdade, não um valor guardado), com a linha `demo ...` e o `$SEG` de origem `C`, clipe = índice do clipe e segmento = índice dentro do clipe:

```
demo seg=88 clipe=bpfi_0.3mm k=0 ref=1361.836 placa=1361.836 dif=... tol=0.013618 dec=falha OK ciclos=...
$SEG,1,2,C,1,0,F,1,1361.836,0,,,...*HH
```

**Testar no PC** (compilado com o harness; com a CMSIS-DSP genérica, os ciclos viram nanossegundos do PC):

```bash
cmake -S tests/host/harness -B build/harness -G Ninja && cmake --build build/harness
./build/harness/test_cadeia_clipes > build/cadeia_pc.txt      # TEST CADEIA OK, na saída de erro
python3 tests/host/pacote/ler_pacotes.py --arquivo build/cadeia_pc.txt --resumo
```

O teste confere os 10 segmentos, a passada bloco a bloco igual à do `mfcc_segmento()`, o pacote de origem `C` e que a comparação reprova quando deve (escore a 2 tolerâncias, sinal trocado, NaN, empate em zero).

**Na placa** (build padrão, sem opção):

```bash
cmake --preset Debug -DBLOCOS13=OFF -DVALIDACAO_PLACA=OFF -DORCAMENTO=OFF   # limpa o que ficou no cache
cmake --build --preset Debug          # e gravar
picocom -b 115200 /dev/ttyUSB0 --logfile build/cadeia_placa.txt   # RESET; esperar uns 3 ciclos da demo
python3 tests/host/pacote/ler_pacotes.py --arquivo build/cadeia_placa.txt --resumo
```

### Resultado na placa

Captura de 19/10 (build Debug padrão), versionada em [`docs/cadeia-placa-19-10.log`](docs/cadeia-placa-19-10.log), do `hello frente1` ao último `$SEG`. O resumo dos pacotes sai de novo com:

```bash
python3 tests/host/pacote/ler_pacotes.py --arquivo docs/cadeia-placa-19-10.log --resumo
```

As linhas da cadeia na captura:

```
cadeia resumo ok=10/10 decisoes_iguais=10/10 iguais_blocos=10/10 dif_max=0.000854 (seg 148) dif/tol_max=6.0% ciclos min=6144384 med=6145466 max=6147497 (med 61.45 ms, max 61.47 ms)
cadeia blocos_mediana janela=1051171 espectro=3992168 mel=472774 log=296495 dct=254779 resumo=49196 final=646 lda=245 soma=6117474 (61.17 ms)
CADEIA OK
```

| Bloco | Ciclos por segmento de 1 s (mediana dos 10) | ms a 100 MHz | % do segmento |
|---|---:|---:|---:|
| janela (escala e Hann) | 1.051.171 | 10,51 | 17,1 % |
| espectro (FFT e potência) | 3.992.168 | 39,92 | 65,0 % |
| Mel | 472.774 | 4,73 | 7,7 % |
| log | 296.495 | 2,96 | 4,8 % |
| DCT | 254.779 | 2,55 | 4,1 % |
| resumo (Welford, 98 quadros) | 49.196 | 0,49 | 0,8 % |
| média e desvio (fim do segmento) | 646 | 0,006 | < 0,1 % |
| LDA | 245 | 0,002 | < 0,1 % |
| soma dos blocos | 6.117.474 | 61,17 | 99,5 % |
| **segmento inteiro (`ciclos`, uma chamada)** | **6.145.466** | **61,45** | 100 % |

Estes ciclos são da CMSIS-DSP **sem** `ARM_MATH_LOOPUNROLL`. Com ele (desde a revisão do PR #9), os escores e as decisões são os mesmos (`CADEIA OK` 10/10, pior caso 6,0 %) e só os ciclos mudam. Captura de 20/10, em [`docs/decimacao-placa-20-10-revisao.log`](docs/decimacao-placa-20-10-revisao.log):

| Bloco | Sem `ARM_MATH_LOOPUNROLL` (19/10) | Com (20/10) | Diferença |
|---|---:|---:|---:|
| janela (escala e Hann; `arm_mult_f32`) | 1.051.171 | 976.569 | −7,1 % |
| espectro (FFT e potência) | 3.992.168 | 3.950.910 | −1,0 % |
| Mel (`arm_dot_prod_f32`) | 472.774 | 361.998 | −23,4 % |
| log | 296.495 | 296.205 | −0,1 % |
| DCT (`arm_dot_prod_f32`) | 254.779 | 196.612 | −22,8 % |
| resumo (Welford) | 49.196 | 49.052 | −0,3 % |
| média e desvio | 646 | 645 | 0 % |
| LDA | 245 | 242 | −1,2 % |
| **segmento inteiro (`ciclos`, uma chamada)** | **6.145.466** (61,45 ms) | **5.862.057** (58,62 ms) | **−4,6 %** |

A FFT quase não muda porque o `arm_cfft_f32` não tem a opção; o ganho vem do Mel, da DCT e da janela. O segmento inteiro fica em 5,9 % do tempo real.

**No firmware final (22/10).** Com a primeira inferência de ponta a ponta no laço, a captura de [`docs/primeira-inferencia-22-10.log`](docs/primeira-inferencia-22-10.log) remede a cadeia no build que vai para o relatório. Os escores e as decisões são os mesmos (`CADEIA OK` 10/10, pior caso 6,0 %, `iguais_blocos=10/10`), e o segmento inteiro fica 0,1 % abaixo do de 20/10 (o bloco que mais muda é o resumo, −1,3 %, 640 ciclos), dentro da variação entre builds:

| Bloco | Com `ARM_MATH_LOOPUNROLL` (20/10) | Firmware final (22/10) | ms a 100 MHz (22/10) |
|---|---:|---:|---:|
| janela | 976.569 | 976.689 | 9,77 |
| espectro | 3.950.910 | 3.944.931 | 39,45 |
| Mel | 361.998 | 362.546 | 3,63 |
| log | 296.205 | 296.749 | 2,97 |
| DCT | 196.612 | 196.611 | 1,97 |
| resumo (Welford) | 49.052 | 48.412 | 0,48 |
| média e desvio | 645 | 650 | 0,007 |
| LDA | 242 | 241 | 0,002 |
| soma dos blocos | 5.832.233 | 5.826.829 | 58,27 |
| **segmento inteiro (`ciclos`, uma chamada)** | **5.862.057** (58,62 ms) | **5.855.300** (58,55 ms) | 5,9 % do tempo real |

- **Escore:** 10/10 dentro da tolerância e com as decisões do CSV; pior caso 6,0 % da tolerância (`bpfi_1.0mm_seg0148`, diferença de 0,0009). A passada de medida deu bit a bit as mesmas 26 características nos 10 segmentos (`iguais_blocos=10/10`).
- **Tempo:** 61,45 ms por segundo de áudio, ou **6,1 % do tempo real**, com variação de 0,05 % entre segmentos. É o mesmo número da validação do segmento completo (6,145 M ciclos, 19/10).
- **A FFT domina:** espectro e janela somam 82 % do segmento. Os blocos 1 a 3 batem com o `blocos13` (10.723, 40.632 e 4.830 ciclos por quadro, × 98).
- **Blocos 4 a 6 medidos:** 6,01 ms juntos (antes, ~6,4 ms por diferença). O log é o mais caro deles: 1.960 `logf` por segmento, ~151 ciclos cada. A DCT custa ~2.600 ciclos por quadro (13 produtos de 20 termos).
- **Demonstração:** 39 pacotes `$SEG` de origem `C` seguidos, com as decisões do CSV; o `ler_pacotes.py` deu 0 descartados e 0 perdidos. O marco continua `MARCO OK` (295/295) no mesmo boot.

## Decimação na placa

Card "Decimação embarcada (bloco 0) e medida de ciclos". O `Core/Src/decimacao.c` fecha a cadeia desde a taxa da gravação original, sem opção de CMake, quadro a quadro, como o áudio ao vivo vai rodar:

```
recorte_51200 (int16, 51,2 kHz) → arm_fir_decimate_f32 (147 coeficientes, fator 4, blocos de 512)
  → 128 saídas requantizadas para int16 por bloco → buffer de um quadro (320 amostras)
  → mfcc_quadro_coefs() e mfcc_resumo_acumular() a cada passo de 128 → lda_escore()
```

O recorte é o do `clipes_demo.h`: o segmento 29 (o primeiro do clipe `normal`) com as margens do decimador causal de fase 0, 75 amostras antes e 73 depois (`docs/contrato_numerico.md` do Python, seção "Decimação"). A última chamada do decimador tem 148 amostras: sem ela, as 37 últimas saídas se perdem sem erro. As 37 primeiras saídas ainda têm os zeros do estado inicial e são descartadas; as 12.800 seguintes são o segmento.

**A cadeia quadro a quadro** (revisão da Amanda no PR #9). Cada bloco de 512 amostras de entrada dá 128 saídas, exatamente um passo de quadro. As saídas entram num buffer de 320 amostras (640 B), com a sobreposição de 192 entre quadros. São as mesmas chamadas do `mfcc_segmento()`, na mesma ordem, então o escore sai bit a bit igual ao do segundo inteiro (o `test_decimacao` confere). As 64 últimas amostras não entram em quadro, como no contrato (98 quadros). Não há buffer do segundo inteiro: era de 25.600 B.

**A conta em LSB.** O Python divide a entrada por 32767 e multiplica a saída por 32767; as duas escalas se cancelam. Por isso a entrada vai para o FIR como `(float)int16`, sem a divisão por amostra, e os coeficientes ficam como estão, numa cópia em RAM (588 B, para tirar as leituras da flash). `Σ b[k] x[n − k]` já sai em LSB. A requantização é a do contrato (meio-para-par, **saturada** em −32768…32767 antes do cast): na placa, pelo `vcvtr.s32.f32`, que arredonda pelo modo do FPSCR (meio-para-par por padrão) numa instrução; no PC, pelo `lrintf`. Antes da revisão, a requantização usava o `lrintf` da newlib, que é em software (~30 ciclos por amostra).

**No boot**, depois da cadeia, o `decimacao_verificar()` manda (aqui, a saída do PC):

```
decim inicio recorte=seg29 clipe=normal fs=51200->12800 taps=147 fator=4 bloco=512 amostras=51348 descartar=37
decim fase=0 OK
decim saida n=12800/12837 saturadas=0 quadros=98 max_lsb=0.500092 (j=6240) tol=0.555330 pico=5533 dif_int16_max=1 n_dif=4 iguais=1 crc32=165F58B2 crc32_ref=D10BB565 OK
decim ciclos total=... (... ms, ...% do tempo real) decimacao=... (... ms, ...%) fir=... (... ms) resto=... (... ms) cadeia=... (... ms)
decim escore ref=-1331.768 placa=-1331.770 dif=0.002319 tol=0.013318 dec=normal igual_clipe=0 OK
DECIMACAO OK duracao=... ms
```

Os valores de `saida` e `escore` da placa podem diferir um pouco dos do PC, porque ela funde multiplicação e soma (`vfma.f32`).

- **`fase`:** um impulso pelo `arm_fir_decimate_f32`. Com `x = [1, 0, 0, …]`, a fase 0 dá `y[m] = b[4m]` (o teste do contrato). O recorte só vale para a fase 0; com outra, a verificação para ali.
- **`saida`:** a referência é o `clipe_normal` (os 12.800 primeiros `int16`), que é a saída do `02` do Python: em float64, o recorte decimado e requantizado dá ele bit a bit (conferido pelo `comparar_decimacao.py`). Em float32 não precisa dar, porque amostras perto de um empate (x,5 LSB) podem arredondar para o outro lado. Passa quando:
  - `n=12800/12837`, `saturadas=0` e `quadros=98`;
  - antes de requantizar, `max_lsb` = max |y − ref| ≤ `tol` = 0,5 + 10⁻⁵ × `pico` (ver "Tolerâncias" abaixo: na placa, essa conferência é frouxa);
  - depois de requantizar, nenhuma amostra a mais de 1 LSB do Python (`dif_int16_max` ≤ 1);
  - `iguais=1`: uma segunda passada, sem a cadeia e sem medir, deu o mesmo CRC-32 da passada medida.

  `n_dif` (quantas amostras diferem de 1 LSB) vai na linha **só como informação**: são empates, e o número cresce com a amplitude. O `crc32` é o do segmento decimado na placa, e o `crc32_ref`, o do `int16` do Python (`zlib.crc32` das amostras em little-endian); os dois só são iguais quando `n_dif=0`.
- **`ciclos`:** uma passada só, pelo DWT: `total` (do `int16` a 51,2 kHz ao escore), `cadeia` (os 98 quadros, o fim do resumo e a LDA), `decimacao` (o total menos a cadeia), e, dentro dela, `fir` (as 101 chamadas do `arm_fir_decimate_f32`) e `resto` (a conversão, a requantização, o CRC e o buffer do quadro).
- **`escore`:** o escore da cadeia quadro a quadro, contra o `RECORTE_ESCORE_REF_INT16` do `clipes_demo.h` (o escore do recorte requantizado), pela tolerância encadeada do escore no A4 (1e-5 relativo). `igual_clipe=1` quando o segmento decimado é bit a bit o `clipe_normal` (`n_dif=0`); senão o escore muda um pouco (no PC, 0,002, de uma tolerância de 0,013).
- **`duracao`:** a verificação inteira, com as duas decimações, a cadeia e a serial.

**Tolerâncias.** O A4 não tinha tolerância para a decimação. As daqui seguem a regra dele: 10 vezes o maior erro medido, para cima na sequência 1, 2, 5.

- **Erro do float32, antes de requantizar** (`DECIM_TOL_REL_FLOAT` = 10⁻⁵ do pico). É relativo ao pico do segmento, e não em LSB, porque o erro do float32 cresce com a amplitude. A primeira versão fixou 5 × 10⁻⁶ com o segmento 29 só (4,6 × 10⁻⁷). Na revisão do PR #9, a Júlia mediu os 295 segmentos: 9,3 × 10⁻⁷ no pior caso, o que pela regra dá 10⁻⁵. Com a conta em LSB, o segmento 29 fica em 5,1 × 10⁻⁷ (`comparar_decimacao.py`). Os 295 segmentos com a conta atual: ver "Medida nos 295 segmentos", abaixo.
- **Depois de requantizar:** no máximo 1 LSB do Python. Não há limite para quantas amostras diferem: a primeira versão tinha 20, mas a Júlia mostrou que ele reprova 23 dos 295 segmentos sem erro de porte, todos das gravações fortes (até 31 empates num segmento).
- **O que a placa confere de verdade.** Na placa não há o float64, só o `int16` do Python, que já tem até 0,5 LSB de arredondamento. Por isso o critério de lá é 0,5 + 10⁻⁵ × pico, e um erro de até ~0,5 LSB passa: ~10⁻⁴ do pico de 5.533, ~20 vezes acima dos 10⁻⁵. **Os 10⁻⁵ só são conferidos no PC**, pelo `comparar_decimacao.py` (o segmento 29) e pelo `tolerancias_gravacoes.py` (os 295). Na placa, quem pega um erro de porte fino é o escore, conferido pelo A4.

**Memória.** A decimação ocupa ~7 KB de RAM: o estado do decimador (147 + 512 − 1 floats), um bloco de entrada (512 floats), as saídas de um bloco (128 floats), a cópia dos coeficientes (147 floats), o buffer do quadro (320 `int16`) e o texto da serial. O recorte em float inteiro (205 KB) não caberia na RAM.

**Testar no PC** (compilado com o harness):

```bash
cmake -S tests/host/harness -B build/harness -G Ninja && cmake --build build/harness
./build/harness/test_decimacao build/decimacao_c.f32 > build/decimacao_pc.txt   # TEST DECIMACAO OK, na saída de erro
python3 tests/host/decimacao/comparar_decimacao.py build/decimacao_c.f32 \
    --repo-python ../diagnostico-acustico-motores                             # COMPARACAO OK
```

O teste confere:
- a fase e a verificação inteira;
- que a cadeia quadro a quadro dá o escore bit a bit do `mfcc_segmento()` no mesmo `int16`, e que o CRC acumulado é o das 12.800 saídas;
- a requantização (meio-para-par e saturação);
- que os critérios reprovam quando devem: sem o último bloco, amostra saturada, quadros a menos, 2 LSB, erro em float acima da tolerância, NaN, passadas diferentes, uma referência errada de 2 LSB numa amostra;
- e que **não** reprovam por amostras demais a 1 LSB.

O `comparar_decimacao.py` compara as saídas em float com a decimação em float64 do contrato, com os coeficientes do `dsp.taps_decimacao` (com `--repo-python`) ou os do `clipes_demo.h` (sem), e confere que o float64 requantizado dá o `clipe_normal` bit a bit.

### Medida nos 295 segmentos

O `tests/host/decimacao/decimar_gravacao.c` decima uma gravação inteira a 51,2 kHz com o mesmo `decimacao_bloco()` da placa (CMSIS-DSP genérica), com os 75 zeros antes, como o `lfilter` do Python. O `tests/host/decimacao/tolerancias_gravacoes.py` roda ele nas 5 gravações e compara cada segmento da partição com o `dsp.resample_clip` em float64. Precisa das gravações (`data/processed/pcm_raw/*.bin`) e do venv do repositório do Python:

```bash
~/diagnostico-acustico-motores/.venv/bin/python tests/host/decimacao/tolerancias_gravacoes.py \
    --repo-python ~/diagnostico-acustico-motores --recorte build/decimacao_c.f32   # TOLERANCIAS OK
```

Com `--recorte`, confere também que o segmento 29 da gravação inteira dá **bit a bit** as saídas do recorte (o alinhamento). O script foi testado com gravações sintéticas no lugar das reais: o alinhamento deu bit a bit, e os erros ficaram na ordem dos medidos pela Júlia.

Resultado em 20/10, nas gravações reais, com a conta em LSB (saída do script):

```
recorte (segmento 29) contra a gravacao inteira: bit a bit igual
```

| Gravação | Pico (LSB) | Erro relativo máx. | Diferença máx. depois de requantizar (LSB) | Amostras diferentes (máx.) |
|---|---:|---:|---:|---:|
| normal | 4.634–8.038 | 8,67 × 10⁻⁷ | 1 | 7 |
| bpfi_0.3mm | 12.711–15.813 | 7,56 × 10⁻⁷ | 1 | 17 |
| bpfi_1.0mm | 27.432–31.268 | 6,69 × 10⁻⁷ | 1 | 28 |
| bpfo_0.3mm | 4.697–6.843 | 7,59 × 10⁻⁷ | 1 | 6 |
| bpfo_1.0mm | 25.271–28.028 | 6,93 × 10⁻⁷ | 1 | 26 |

- **Erro do float32:** 8,67 × 10⁻⁷ do pico no pior caso (segmento 57). Pela regra do A4, a tolerância é **10⁻⁵**, a do `decimacao.h`, e o pior caso usa 8,7 % dela. Antes da conta em LSB, a Júlia mediu 9,3 × 10⁻⁷.
- **Depois de requantizar:** no máximo 1 LSB em todos os 295; até 28 amostras a 1 LSB num segmento, nas gravações fortes (o limite antigo de 20 reprovaria esses segmentos sem erro de porte); nenhuma saturada.
- **A conferência da placa** (|y − `int16`| ≤ 0,5 + 10⁻⁵ × pico) passaria em todos os 295.
- **TOLERANCIAS OK.**

### Resultado na placa

Captura de 20/10 (build Debug padrão, depois da revisão do PR #9), versionada em [`docs/decimacao-placa-20-10-revisao.log`](docs/decimacao-placa-20-10-revisao.log), do `hello frente1` ao nono `$SEG`. No mesmo boot: `MARCO OK` (295/295), `CADEIA OK` (10/10) e:

```
decim fase=0 OK
decim saida n=12800/12837 saturadas=0 quadros=98 max_lsb=0.500092 (j=6240) tol=0.555330 pico=5533 dif_int16_max=1 n_dif=7 iguais=1 crc32=E4DD7E31 crc32_ref=D10BB565 OK
decim ciclos total=17453346 (174.53 ms, 17.5% do tempo real) decimacao=11602669 (116.03 ms, 11.6%) fir=10210468 (102.10 ms) resto=1392201 (13.92 ms) cadeia=5850677 (58.51 ms)
decim escore ref=-1331.768 placa=-1331.769 dif=0.001099 tol=0.013318 dec=normal igual_clipe=0 OK
DECIMACAO OK duracao=350.41 ms
```

| Etapa (1 s de áudio) | Antes da revisão | Depois | ms a 100 MHz (depois) | % do tempo real (depois) |
|---|---:|---:|---:|---:|
| FIR (`arm_fir_decimate_f32`, 101 chamadas) | 17.214.391 | 10.210.468 | 102,10 | 10,2 % |
| conversão, requantização e o resto | 2.265.403 | 1.392.201 | 13,92 | 1,4 % |
| **decimação** | **19.479.794** | **11.602.669** | **116,03** | **11,6 %** |
| cadeia (MFCC e LDA) | 6.205.237 | 5.850.677 | 58,51 | 5,9 % |
| **do `int16` a 51,2 kHz ao escore** | **25.685.031** | **17.453.346** | **174,53** | **17,5 %** |

- **Saída:** 7 das 12.800 amostras diferem de 1 LSB do `int16` do Python (4 no PC, sem FMA), todas empates. Antes de requantizar, o pior caso é 0,500092 LSB, contra 0,555330 da conferência frouxa da placa. Sem saturação, 98 quadros, e as duas passadas deram o mesmo CRC.
- **Escore:** a 8,3 % da tolerância encadeada do A4 (0,001099 de 0,013318), com a decisão do CSV (normal). Foi 2,7 % na primeira captura: a conta em LSB muda o arredondamento de algumas saídas, e a mudança chega ao escore, bem dentro da tolerância.
- **FIR:** 5,4 ciclos por coeficiente (10.210.468 / (12.837 × 147)), contra 9,1 antes. A estimativa da Amanda na revisão era ~5 ciclos.
- **O resto:** ~27 ciclos por amostra de entrada, contra ~44 antes. Inclui a conversão `int16 → float`, o `vcvtr` e a saturação, o buffer do quadro e o CRC-32 acumulado, que só existe por causa da verificação.
- **A cadeia dentro da decimação** (5.850.677) bate com a cadeia dos clipes no mesmo boot (5.862.057, mediana dos 10): a diferença é a medida, que aqui não inclui o laço dos quadros.
- **A verificação inteira** leva 350 ms no boot: as duas decimações, a cadeia e a serial.
- **Viabilidade:** do `int16` a 51,2 kHz ao escore, 17,5 % do tempo real.

### Primeira captura (antes da revisão do PR #9)

Captura de 20/10, versionada em [`docs/decimacao-placa-20-10.log`](docs/decimacao-placa-20-10.log). Era o firmware com o buffer do segundo inteiro, a divisão por 32767 na entrada, o `lrintf` da newlib e a CMSIS-DSP sem `ARM_MATH_LOOPUNROLL`:

```
decim saida n=12800/12837 saturadas=0 max_lsb=0.500061 (j=4543) tol=0.527665 pico=5533 dif_int16_max=1 n_dif=2/20 iguais=1 crc32=6487E0C4 crc32_ref=D10BB565 OK
decim ciclos total=19479794 (194.80 ms) fir=17214391 (172.14 ms) conversao_e_quantizacao=2265403 (22.65 ms) tempo_real=19.5%
decim escore ref=-1331.768 placa=-1331.768 dif=0.000366 tol=0.013318 dec=normal igual_clipe=0 ciclos_cadeia=6205237 (62.05 ms) OK
decim cadeia_completa ciclos=25685031 (256.85 ms, 25.7% do tempo real) decimacao=19479794 cadeia=6205237
DECIMACAO OK
```

- A decimação levou 194,8 ms por segundo de áudio (19,5 % do tempo real), e o `int16` a 51,2 kHz até o escore, 256,9 ms (25,7 %).
- O FIR levou ~9,1 ciclos por coeficiente: sem `ARM_MATH_LOOPUNROLL`, o `arm_fir_decimate_f32` usava o laço com um acumulador só, e cada `vfma.f32` esperava o anterior.
- A conversão e a requantização somaram ~44 ciclos por amostra de entrada: a divisão `int16 / 32767` (51.348 vezes) e o `lrintf` em software (12.800 vezes).
- A cadeia no mesmo boot ficou 1,2 % acima da captura de 19/10 (6.217.706 contra 6.145.466 ciclos), sem mudança no código dela; provavelmente pela posição do código na flash (não confirmado). Para o relatório, vale citar a variação de ~1 a 2 % entre builds.

## Primeira inferência de ponta a ponta

Card "Primeira inferência on-device funcionando" (meta física da Fase 2: cadeia portada, com a primeira inferência executando no microcontrolador). Junta as duas pontas que já rodavam separadas: a cadeia no laço, sobre os clipes de 12,8 kHz (desde 19/10), e a decimação, só no boot (desde 20/10). Agora o **laço normal do firmware** faz o caminho inteiro, sem opção de CMake:

```
recorte_51200 (int16, 51,2 kHz, flash) → decimação (FIR de 147, fator 4) → cadeia quadro a quadro
  → lda_escore() → decisão → LED (2 s) e $SEG de origem D
```

Cada volta da demonstração começa por esse caminho e segue pelos 10 segmentos dos clipes (origem `C`), com um contador `n` só para as duas origens. É uma inferência de verdade a cada volta, não um valor guardado: o `decimacao_passo()` chama o mesmo `decimacao_executar()` do boot, que passa o recorte de novo pelo `arm_fir_decimate_f32` e pela cadeia. Uma volta leva cerca de 25 s (11 segmentos de 2,3 s), e o primeiro `$SEG` depois do boot é o de origem `D`.

Na serial, a cada volta (a linha `ponta` numa linha só):

```
ponta seg=29 clipe=normal fs=51200->12800 ref=-1331.768 placa=-1331.769 dif=... tol=0.013318 dec=normal OK
      ciclos=... (... ms, ...% do tempo real) decimacao=... cadeia=... crc32=... igual_boot=1
$SEG,1,0,D,0,0,N,0,-1331.769,0,,,...*HH
```

- **`OK`** pede três coisas: as contagens da decimação certas (12.800 saídas, sem saturar, 98 quadros); o escore dentro da tolerância encadeada do A4 contra `RECORTE_ESCORE_REF_INT16`, com a decisão da referência (normal); e **`igual_boot=1`**, o mesmo `int16` (CRC-32) e o mesmo escore, bit a bit, da passada medida no boot. Sem `OK`, a linha e o pacote saem do mesmo jeito, mas os LEDs piscam o erro em vez de mostrar uma decisão.
- **`ciclos`** vão do `int16` a 51,2 kHz ao escore, os mesmos do campo 12 do `$SEG`; `decimacao` e `cadeia` são as duas partes, como na linha `decim ciclos` do boot.
- **O recorte é um segmento do treino do modelo final**, como os dos clipes: a inferência mostra a fidelidade do porte, não o desempenho.

**Fora deste card:** o painel no computador (23/11) e o áudio contínuo de vários segundos a 51,2 kHz, que a flash não comporta (o recorte de 1 s já ocupa 103 KB).

### Testar no PC

O `test_decimacao` (seção "Decimação na placa") roda duas inferências seguidas pelo `decimacao_inferir()`, depois da verificação, e confere que dão o mesmo `int16` e o mesmo escore, bit a bit, da passada do boot, que o `$SEG` sai com origem `D` e a decisão normal, e que a linha `ponta` diz `OK` (e `FALHOU` sem ele). O `test_pacote_serial` confere a linha exata de origem `D`, e o `ler_pacotes.py --autoteste` a aceita.

### Capturar na placa

Grave o build padrão (Debug) e capture a serial do RESET até pelo menos duas voltas (~60 s), num arquivo novo:

```bash
rm -f build/primeira_inferencia.txt
python3 tests/host/pacote/ler_pacotes.py --porta /dev/ttyUSB0 --segundos 60 --salvar build/primeira_inferencia.txt
python3 tests/host/pacote/ler_pacotes.py --arquivo build/primeira_inferencia.txt --resumo
```

Aperte o RESET logo depois de abrir, para pegar o boot inteiro (`MARCO OK`, `CADEIA OK`, `DECIMACAO OK` e os `$CAB`). O resumo tem que dar 0 descartados e 0 perdidos, e a linha "por origem" mostra os pacotes `D` (todos `N`) e `C`.

### Resultado na placa

Captura de 22/10 (build Debug padrão, sem `BLOCOS13`, `VALIDACAO_PLACA` nem `ORCAMENTO`), versionada em [`docs/primeira-inferencia-22-10.log`](docs/primeira-inferencia-22-10.log): um boot, do `hello frente1` a três voltas do laço (60 s). No mesmo boot, `MARCO OK` (295/295), `CADEIA OK` (10/10) e `DECIMACAO OK`. O resumo dos pacotes sai de novo com:

```bash
python3 tests/host/pacote/ler_pacotes.py --arquivo docs/primeira-inferencia-22-10.log --resumo
```

```
resumo: 25 pacotes certos, 0 descartados, 0 perdidos, 0 reinicios; 23 segmentos; modelo exp239
por origem: C (clipe) 20: 4 N, 16 F; D (decimado) 3: 3 N, 0 F
```

As três inferências de ponta a ponta:

```
ponta seg=29 clipe=normal fs=51200->12800 ref=-1331.768 placa=-1331.769 dif=0.001099 tol=0.013318 dec=normal OK ciclos=17446485 (174.46 ms, 17.4% do tempo real) decimacao=11602884 cadeia=5843601 crc32=E4DD7E31 igual_boot=1
ponta seg=29 clipe=normal fs=51200->12800 ref=-1331.768 placa=-1331.769 dif=0.001099 tol=0.013318 dec=normal OK ciclos=17446414 (174.46 ms, 17.4% do tempo real) decimacao=11602704 cadeia=5843710 crc32=E4DD7E31 igual_boot=1
ponta seg=29 clipe=normal fs=51200->12800 ref=-1331.768 placa=-1331.769 dif=0.001099 tol=0.013318 dec=normal OK ciclos=17446451 (174.46 ms, 17.4% do tempo real) decimacao=11602724 cadeia=5843727 crc32=E4DD7E31 igual_boot=1
```

| Etapa (1 s de áudio, inferência no laço) | Ciclos (1ª volta) | ms a 100 MHz | % do tempo real |
|---|---:|---:|---:|
| decimação (FIR, conversão e requantização) | 11.602.884 | 116,03 | 11,6 % |
| cadeia (MFCC e LDA, quadro a quadro) | 5.843.601 | 58,44 | 5,8 % |
| **do `int16` a 51,2 kHz ao escore** | **17.446.485** | **174,46** | **17,4 %** |

- **Fidelidade:** as três inferências deram o mesmo `int16` (CRC `E4DD7E31`) e o mesmo escore da passada do boot, bit a bit (`igual_boot=1`). O escore fica a 8,3 % da tolerância encadeada do A4 (0,001099 de 0,013318), com a decisão da referência (normal), como no boot de 20/10.
- **Pacotes:** os 23 `$SEG` (3 `D` e 20 `C`) saíram numa sequência só, `n` de 0 a 22, sem perda nem pacote descartado; os 3 `D` são normais e os `C` têm as decisões do CSV (4 normais, 16 falhas).
- **Tempo:** 174,46 ms do `int16` a 51,2 kHz ao escore, com variação de 71 ciclos entre as voltas (< 0,001 %). É o mesmo número da passada medida no boot (`decim ciclos total=17446493`). A cadeia dentro da decimação (5.843.601) fica 0,2 % abaixo da cadeia dos clipes no mesmo boot (5.855.300): a medida aqui não inclui o laço dos quadros.
- **Meta física da Fase 2:** cadeia portada, com a inferência executando no microcontrolador do áudio na taxa original até a decisão nos LEDs e na serial.

## Recepção de segmentos pela serial

Card "Recepção de segmentos pela serial no firmware (nova origem no $SEG)". A flash só guarda os 10 segmentos dos clipes, todos do treino. Com a placa recebendo segmentos do PC pelo adaptador USB-serial, a cadeia da placa roda nos 295 segmentos do Jung e nos 40 do HUSTmotor, que estão fora do treino. O formato abaixo foi decidido em 29/10 (Registro de Decisões, "Recepção de segmentos pela serial: formato do quadro, origem S e modo de recepção"), com a revisão da Amanda e da Júlia.

**Estado (30/10):** o parser do quadro (`recepcao.c/.h`), o modo de recepção na placa (`recepcao_serial.c/.h`), a origem `S` e as linhas `$PED` e `$ERR` no `pacote_serial`, o script mínimo do PC (`tests/host/recepcao/enviar_segmento.py`) e um emulador da placa no PC estão prontos e testados no PC, e testados na placa em 30/10 (115.200 bit/s) e em 31/10 (921.600 bit/s, teste à parte; seção "Resultado na placa").

### O diálogo

1. O PC manda `RECEBER\n` (ASCII) a cada 1 s e ignora as linhas da demonstração até chegar um `$PED`. A placa confere a UART entre um segmento e outro da demonstração. Ao entrar no modo, manda uma linha `recepcao inicio`, uma linha `ref` por segmento dos clipes da flash (abaixo), o `$CAB` e o primeiro `$PED`. Sai do modo só por reset. Se as verificações do boot falharam, a placa recusa o modo (`recepcao recusada: ...`) e mantém o padrão de erro nos LEDs. Um `RECEBER` que ainda chegue depois do `$PED` é ignorado: esperando um quadro, a placa procura a mágica `QSEG` e descarta o que vem antes.
2. Para cada `$PED`, o PC manda **um** quadro. Para cada quadro, a placa responde com exatamente um `$SEG` (origem `S`) ou um `$ERR`, e depois um `$PED`. O PC não manda nada antes do `$PED`.
3. Se vier `$ERR`, ou se nenhuma resposta chegar no tempo de transmissão mais 2 s, o PC reenvia o mesmo segmento. São no máximo 3 tentativas por segmento, contando as duas causas; depois, o segmento entra no relatório como "não processado", com contagem: a matriz dos 335 não pode encolher sem ninguém perceber. As regras completas do script da rodada estão em "Regras do PC na rodada", abaixo.

A 115.200 bit/s (o padrão), um quadro leva 2,22 s e os 335 segmentos, cerca de 13 min. 921.600 bit/s (925.926 reais com o APB2 a 100 MHz: a HAL grava BRR = 0x6C, USARTDIV 6,75, +0,47 %) é uma opção, testada em 31/10 (seção "Resultado na placa"); o formato não depende do baud.

### O quadro (PC → placa)

25.618 B, tudo little-endian:

| Offset | Bytes | Campo | Valor |
|---|---|---|---|
| 0 | 4 | mágica | `QSEG` (ASCII) |
| 4 | 1 | versão do quadro | 1 |
| 5 | 1 | formato | 1 = `int16` a 12,8 kHz (2 reservado para 51,2 kHz, ainda não aceito) |
| 6 | 2 | grupo (`uint16`) | vai no campo `clipe` do `$SEG`: 1 = Jung, 2 = HUSTmotor, 9 = clipes da flash (ver "Regras do PC na rodada") |
| 8 | 4 | id (`uint32`, até 2³¹ − 1) | vai no campo `segmento` do `$SEG` |
| 12 | 2 | n_amostras (`uint16`) | 12800 |
| 14 | 4 | CRC-32 (zlib) | sobre os bytes 4..13 e o corpo |
| 18 | 25.600 | corpo | 12.800 amostras `int16` |

O CRC é o `zlib.crc32` do Python (o `crc32.c` da placa) e cobre também o cabeçalho, então um id ou um tamanho corrompido também é pego. No Python:

```python
corpo = amostras.astype("<i2").tobytes()          # 12800 amostras
cab = struct.pack("<BBHIH", 1, 1, grupo, id_seg, 12800)
quadro = b"QSEG" + cab + struct.pack("<I", zlib.crc32(cab + corpo)) + corpo
```

### As respostas (placa → PC)

```
recepcao inicio modelo=exp239 json=ee0c3e088366 quadro=25618 anel=2048 baud=115200 silencio_ms=200
ref seg=29 clipe=normal k=0 escore=-1331.768 escore_hex=0xC4A67892   (uma por segmento dos clipes)
$CAB,1,exp239,ee0c3e088366,26,100*3B
$PED,1,1*51                                    pronta; o 3º campo conta os pedidos desde a entrada no modo
rx grupo=2 id=117 bytes=25618 ms=2231 ignorados=0 a_mais=0 erros_uart=0 ciclos=... escore_hex=0x...
$SEG,1,812,S,2,117,N,0,-1.234,0,,,1043210*71   segmento recebido e processado
rx erro=crc grupo=2 id=118 bytes=25618 descartados=0 ignorados=0 erros_uart=0
$ERR,1,2,118,crc*0C                            erro de recepção: grupo, id e motivo
$ERR,1,,,truncado*60                           erro antes de o cabeçalho chegar: grupo e id vazios
```

(Os escores `ref` acima são do emulador no PC; na placa, a conta com `vfma.f32` dá outros bits.)

- **`$SEG` de origem `S`:** os mesmos campos do de origem `C`. Os `ciclos` somam só o processamento (os quadros do MFCC, o resumo e a LDA), sem a espera pelos bytes.
- **`$ERR,1,<grupo>,<id>,<motivo>*HH`:** os motivos são `versao`, `formato`, `tamanho`, `crc` e `truncado`. O `magica`, previsto em 29/10, não é mais emitido (30/10): com a busca pela mágica, um quadro com a mágica corrompida é ignorado inteiro, a placa não responde, e o PC reenvia pelo timeout dele. Um erro de recepção não é resultado de segmento: não gasta o `n` do `$SEG` e não entra na contagem do painel. Se o cabeçalho chegou, grupo e id repetem o que veio nele (mesmo que outro campo esteja errado); se o quadro truncou antes, os dois vão vazios. O erro de conta (`disparou` = 4, decisão F) continua no `$SEG`, como antes.
- **As linhas `ref` e `rx`:** a placa manda, ao entrar no modo, o escore de cada segmento dos clipes da flash pela mesma conta da demonstração (`mfcc_segmento` e `lda_escore`), em hexadecimal; e, antes de cada `$SEG`, a linha `rx`, com o escore do segmento recebido em hexadecimal (o `$SEG` só tem 3 casas). Mandar do PC um segmento dos clipes e comparar os dois hexadecimais é o teste bit a bit do card. A `rx` traz também o tempo de recepção (`ms`, da mágica ao último byte), os bytes ignorados antes da mágica, os que vieram a mais e os erros de linha da UART.
- O `ler_pacotes.py` só lê as linhas que começam com `$CAB,` ou `$SEG,`, então `$PED` e `$ERR` passam pela versão 1 do pacote sem mudança. Só a origem `S` pede o leitor atualizado, como foi com a `D`.

### Erros e silêncio

Esperando um quadro, a placa procura a mágica `QSEG` e ignora o que vem antes, sem timeout. Depois dela, o cabeçalho é conferido assim que os 18 bytes chegam: versão, formato e n_amostras errados viram erro na hora, sem esperar o corpo. O CRC só é conferido no fim, e **a decisão só sai depois dele**: um quadro com CRC errado nunca vira `$SEG`. **200 ms sem byte** no meio de um quadro contam como `truncado` (bem acima da latência de um adaptador USB-serial, de poucos ms a ~16 ms). Todo erro segue o mesmo caminho: a placa para o DMA, descarta tudo até a linha ficar 200 ms em silêncio, manda o `$ERR`, recomeça a recepção do zero e manda um `$PED` novo. Esperando o primeiro byte de um quadro, não há timeout.

### Recepção em fluxo

A placa não guarda o segmento. Um DMA circular só (`HAL_UART_Receive_DMA`, DMA2 Stream2, anel de 2 KB) recebe o quadro inteiro, cabeçalho e corpo, sem rearme no meio: um segundo `receive` para o corpo abriria uma janela de ~21 µs a 921.600 bit/s em que bytes se perderiam (apontado pela Júlia). O anel é armado na posição 0 antes de cada `$PED` e parado quando o quadro completa ou dá erro. O laço lê a posição do DMA (o `NDTR`) e entrega ao parser o que chegou desde a última leitura, em dois pedaços quando o anel deu a volta. Por isso o `HAL_UARTEx_ReceiveToIdle_DMA` da decisão de 29/10 não é usado: o truncado é medido pelo tempo desde o último byte, e o evento de IDLE não serviria para nada além de uma interrupção a mais por rajada.

Durante a demonstração, os 64 primeiros bytes do mesmo anel recebem o que chega (um DMA circular de 64 B), para o `RECEBER` não se perder nos `HAL_Delay` nem dar overrun. **Um erro de linha** (ruído, quadro, overrun) faz a HAL abortar a recepção por DMA e chamar o `HAL_UART_ErrorCallback()`, que o `recepcao_serial.c` define: o laço arma o DMA de novo sem zerar o parser, e os bytes perdidos viram `crc` ou `truncado`.

O `recepcao.c` recebe os bytes como chegam, em pedaços de qualquer tamanho, e faz o que o `decimacao_executar()` faz com as saídas do decimador: cada byte do anel é lido **uma vez só**, para uma cópia de 64 B, e o CRC e as amostras saem da cópia. Lendo o anel duas vezes (uma para o CRC, outra para as amostras), um anel sobrescrito entre as duas leituras daria CRC certo com amostras erradas; o emulador abaixo, sem o ritmo da serial, achou exatamente isso na primeira versão. Cada amostra vai para um quadro de 320 do MFCC (640 B), com a sobreposição de 192, e a média e o desvio são acumulados por Welford. São as mesmas chamadas, na mesma ordem, do `mfcc_segmento()`, então as 26 características saem **bit a bit** iguais às do segundo inteiro. Uma amostra pode chegar dividida entre dois pedaços (ou na volta do anel); o parser guarda o primeiro byte até o segundo chegar. O estado do parser ocupa 856 B e o anel, 2 KB: com os buffers de linha, a RAM do firmware sobe cerca de 3,2 KB (de 31,7 KB para 34,9 KB, pelo relatório do linker), contra 25,6 KB de um buffer do quadro inteiro (o PR #9 acabou de tirar um buffer igual). A flash sobe cerca de 4,8 KB (o parser, o modo, as linhas e o caminho de recepção por DMA da HAL). Processar um quadro do MFCC leva ~0,6 ms, e 128 amostras novas levam 22 ms para chegar a 115.200 bit/s (2,8 ms a 921.600), então o parser nunca fica para trás.

O tempo entra de fora (`agora_ms`: `HAL_GetTick()` na placa, um relógio de mentira no teste), por isso o timeout e o silêncio também são testados no PC.

### Testar no PC

```bash
cmake -S tests/host/harness -B build/harness -G Ninja && cmake --build build/harness
./build/harness/test_recepcao     # TEST RECEPCAO OK, na saída de erro
```

O `test_recepcao` monta o quadro de cada um dos 10 segmentos dos clipes e confere:

- entregue em cinco padrões de pedaços (inteiro, byte a byte, e tamanhos que cortam o cabeçalho e as amostras ao meio), dá as 26 características e o escore da LDA bit a bit iguais aos do `mfcc_segmento()`, com a decisão do CSV;
- bytes a mais depois do quadro só são contados;
- um bit trocado no corpo, no grupo, no id, no próprio CRC ou no último byte vira `crc` só no fim, sem características;
- mágica, versão, formato (0 e o 2, reservado) e n_amostras errados viram erro no 18º byte, e o resto é descartado;
- `truncado` aos 200 ms sem byte, no cabeçalho e no corpo, e não a 199 ms; bytes chegando a cada 199 ms não truncam; sem nenhum byte, não há timeout; e o tempo funciona na volta do contador de ms;
- depois de um erro, um byte atrasado recomeça a espera do silêncio.

Testado também com o parser estragado de propósito: amostras em big-endian, o CRC sem conferir ou sem o cabeçalho, o timeout com `>` no lugar de `>=`, o quadro do MFCC sem a sobreposição, o tamanho sem conferir e um byte que não quebra o silêncio reprovam. Um 99º quadro do MFCC passaria, mas é inalcançável: com 12.800 amostras, ele nunca enche.

O teste também confere que o que vem antes da mágica é ignorado: um `RECEBER\n`, falsos começos (`Q`, `QS`, `QSE`) e um zero, cortados em todos os pontos, e o quadro de trás sai bit a bit; e que um quadro com a mágica corrompida é ignorado inteiro, sem erro e sem timeout.

### A placa no PC (emulador)

O `emular_placa` compila o `recepcao_serial.c` **de verdade** (com o parser, a cadeia, a LDA e o pacote) contra uma HAL de mentira (`tests/host/recepcao/stub/`): a USART1 é um pseudo-terminal, e o DMA circular é uma thread que escreve no anel os bytes que chegam, no ritmo de 115.200 bit/s, e conta o `NDTR` para baixo. O script do PC conversa com ele como com a placa. Não testa a HAL nem o tempo da serial real; testa o diálogo, o anel e a volta dele, o rearme depois de um erro de linha, o parser e as respostas. Só no Linux e no macOS.

```bash
./build/harness/emular_placa &          # imprime a porta: /dev/pts/N
python3 tests/host/recepcao/enviar_segmento.py --porta /dev/pts/N --todos
python3 tests/host/recepcao/enviar_segmento.py --porta /dev/pts/N --seg 88 --estragar crc --ja-no-modo
```

Resultado em 30/10: os 10 segmentos dos clipes com o escore bit a bit igual ao da linha `ref`; `crc`, `versao`, `formato`, `tamanho` e `truncado` com o `$ERR` certo; a mágica corrompida ignorada e o quadro certo de trás aceito. Com `--baud 0` (os bytes de uma vez, o anel sobrescrito antes de o laço ler), os quadros corrompidos voltam como `$ERR` e nenhum `$SEG` sai com escore errado. Com `--erro-uart 5000`, o quadro do erro de linha volta como `$ERR truncado` e o seguinte passa.

### O script do PC (mínimo)

O `tests/host/recepcao/enviar_segmento.py` (precisa do `pyserial`) lê os 10 segmentos dos clipes do próprio `Core/Inc/clipes_demo.h`, pede o modo, manda os segmentos com o grupo 9 e o id do segmento, e confere cada resposta: o `$SEG` com origem `S`, grupo e id certos e o escore bit a bit igual ao da linha `ref`; ou, com `--estragar`, o `$ERR` com o motivo certo. É o script do teste do firmware; o da rodada dos 295 e dos 40 é o da Amanda.

```bash
python3 tests/host/recepcao/enviar_segmento.py --autoteste                      # sem a placa
python3 tests/host/recepcao/enviar_segmento.py --porta /dev/ttyUSB0 --todos --erros --seg 88   # com a placa
```

O `--erros` manda, na mesma sessão, os seis quadros estragados (`crc`, `versao`, `formato`, `tamanho`, `truncado` e `magica`) e o quadro certo no fim, para mostrar que a placa se recuperou. A placa fica no modo até o reset, e as linhas `ref` só saem ao entrar nele. Ao abrir a porta, o script escuta 3 s: se a placa fala (a demonstração), ele pede o modo; se está calada (já no modo, de uma execução anterior), manda direto, sem a conferência bit a bit. Se chegar uma linha da demonstração no meio dos quadros (a placa reiniciou, por exemplo por um novo "Run and Debug" no VS Code), o script para e avisa, em vez de tomar o `$SEG` da demonstração por resposta.

### Regras do PC na rodada

O que o script da rodada dos 295 do Jung e dos 40 do HUSTmotor segue (revisão da Amanda no PR). O `enviar_segmento.py` é só o script do teste do firmware e não implementa todas.

1. **Modelo.** Antes do primeiro quadro, o PC confere o `modelo=` e o `json=` da linha `recepcao inicio` (os mesmos do `$CAB`) contra o modelo do Python. Se não baterem, não manda nada.
2. **Tentativas.** No máximo 3 por segmento, contando `$ERR` e timeout (o tempo de transmissão mais 2 s sem resposta). Depois da terceira, o segmento entra no relatório como "não processado", com contagem.
3. **Duplicatas.** Se a resposta se perde na linha, o reenvio por timeout faz a placa processar o mesmo quadro de novo, com dois `$SEG` para o mesmo id e `n` diferentes. Vale o primeiro; a duplicata é contada no relatório (e o escore tem de ser o mesmo, bit a bit).
4. **Grupos e ids.** Grupo 1 = Jung, com o id do segmento no conjunto (`vt_segmento`); grupo 2 = HUSTmotor, com o índice do segmento na lista `segmentos` do `data/processed/hustmotor/divisao_rotacao.json` (versionado no repositório do Python), **contado a partir de 0**, como o índice de uma lista em Python: os 40 segmentos vão de 0 (`h_5hz_a1`) a 39 (`bf_30hz_a5`). Os ids do HUST são texto e o quadro só leva `uint32`; o relatório da rodada guarda os dois, o índice e o id em texto; grupo 9 = clipes da flash, com o id do segmento (29, 30, 88, ...), que é o que o `enviar_segmento.py` usa.
5. **Reinício.** Se aparecer uma linha da demonstração (`demo`, `ponta`, um `$SEG` de origem `C` ou `D`) ou um `hello` no meio da rodada, a placa reiniciou: o PC pede o modo de novo (`RECEBER`) e retoma do segmento pendente.
6. **A linha `rx` faz parte do protocolo.** O `$SEG` só tem 3 casas, então o script compara o escore pelo `escore_hex` da `rx`. Os campos `grupo`, `id`, `ms` e `escore_hex` da `rx` de um segmento, e `erro`, `grupo` e `id` da `rx` de erro, não mudam sem aviso, como os campos do `$SEG`; campos novos só entram no fim da linha.

### Resultado na placa

Captura de 30/10 (build Debug padrão, 115.200 bit/s), versionada em [`docs/recepcao-placa-30-10.log`](docs/recepcao-placa-30-10.log): a saída do `enviar_segmento.py --todos --erros --seg 88 --salvar`, com as linhas `recepcao inicio`, `ref`, `rx`, `$SEG`, `$ERR` e `$PED` da placa.

- **Bit a bit, 10/10:** os 10 segmentos dos clipes mandados pelo PC deram o escore com os mesmos bits do que a placa calcula para o mesmo clipe da flash (`escore_hex` da `rx` contra o da `ref`), com as decisões do CSV (2 N, 8 F). A recepção em fluxo dá, na placa, a mesma conta do `mfcc_segmento()`, inclusive com o `vfma.f32`. Os bits diferem dos do PC (emulador) em 6 dos 10 escores, pela fusão de multiplicação e soma: por isso a referência é calculada pela própria placa.
- **Erros, 5/5:** `crc`, `versao`, `formato`, `tamanho` e `truncado` voltaram com o `$ERR` e o motivo certos; os de cabeçalho no 18º byte (25.600 B descartados), o truncado aos 12.809 bytes (a metade que o script mandou). A mágica corrompida foi ignorada, sem resposta, e o quadro certo mandado depois dos seis passou bit a bit.
- **Recepção:** 2.219 a 2.220 ms por segmento, da mágica ao último byte (teórico: 25.614 B a 115.200 bit/s = 2.223 ms), com `ignorados=0`, `a_mais=0` e `erros_uart=0` em todas as linhas `rx`.
- **Processamento:** ~5.843.000 ciclos (58,4 ms, 5,8 % do tempo real) por segmento, os quadros do MFCC, o resumo e a LDA. É o mesmo número da cadeia dentro da decimação (5.843.848 no boot de 31/10, com este firmware), que também soma só as chamadas de cada quadro, e fica 0,2 % abaixo do `ciclos` da demonstração no mesmo boot (5.855.322, mediana dos 10), que mede o `mfcc_segmento()` inteiro, com o laço dos quadros. (O build só com o CRC-32, de 30/10, deu ~5.932.000 na demonstração: a variação de ~1 a 2 % entre builds já registrada na seção "Decimação na placa".) **Fica fora do `ciclos`** o trabalho byte a byte da recepção: o CRC (tabela de meio byte), a cópia do anel e a montagem das amostras, e o deslocamento do quadro do MFCC. A Amanda estima ~3 ms por quadro, sem medir; **a medir na caracterização de novembro**, com o DWT em torno do `consumir()` inteiro, para registrar o custo total da recepção.
- **921.600 bit/s (31/10, teste à parte):** com o baud da USART1 mudado no `.ioc` numa branch descartável, o mesmo teste passou inteiro: **10/10 bit a bit** com os mesmos `escore_hex`, os 5/5 erros com o `$ERR` certo, a mágica ignorada e a recuperação. Recepção de **306 a 307 ms** por segmento, contra 276,6 ms teóricos (925.926 bit/s reais: com o APB2 a 100 MHz, a HAL grava BRR = 0x6C, USARTDIV 6,75; conta corrigida pela Amanda na revisão do PR); os ~11 % a mais são intervalos entre os bytes no lado do PC e do adaptador, não perda: o quadro chegou inteiro em todos. A rodada dos 335 segmentos cairia de ~13 min para ~2 min. O firmware deste PR **continua a 115.200 bit/s**, como decidido em 29/10: mudar o padrão muda o picocom, os scripts e o painel, e fica para o grupo decidir. Na revisão, a Júlia e a Amanda sugeriram que, se o padrão subir, entre junto um contador de voltas do anel (pelas interrupções de meia transferência e de transferência completa do DMA), mostrado na linha `rx`: a 921.600 bit/s o anel de 2 KB dá ~22 ms de folga, e uma volta perdida hoje só aparece como `crc` ou `truncado`. Captura em [`docs/recepcao-placa-921600-31-10.log`](docs/recepcao-placa-921600-31-10.log).

**Memória** (`tools/orcamento_memoria.py`, que agora separa a recepção e o CRC-32), contra o mesmo código sem as três chamadas no `main.c` (o que o `--gc-sections` deixa de fora sem elas é o custo do modo):

| Parte | Flash (B) | RAM (B) |
|---|---:|---:|
| recepção pela serial (`recepcao.c` e `recepcao_serial.c`): o anel do DMA (2 KB), o estado do parser (856 B) e os buffers de linha | +2.567 | +3.164 |
| textos das linhas novas (`recepcao inicio`, `ref`, `rx`; o linker os junta na seção de strings do `marco_lda.c`) | +380 | 0 |
| HAL e CubeMX (o caminho de recepção por DMA: `HAL_UART_Receive_DMA`, o abort, as interrupções) | +1.632 | 0 |
| pacote da serial (`$PED` e `$ERR`) | +152 | 0 |
| CRC-32 comum (o `crc32_bytes()` passa a ser usado) | +64 | 0 |
| aplicação base, alinhamento e reserva da pilha | +21 | +4 |
| **total** (relatório do linker: 444.924 → 449.740 B de flash, 31.744 → 34.912 B de RAM) | **+4.816** | **+3.168** |

Sobram 72,8 KiB de flash e 93,9 KiB de RAM.

## Validação bloco a bloco

A validação independente da cadeia em C (card "Validar cada bloco da cadeia em C contra a referência em Python") está em [`docs/validacao-blocos.md`](docs/validacao-blocos.md): a tabela de erros de cada bloco nos 295 segmentos da partição, nos dois modos do A4, o escore final contra o `escores_referencia.csv` e o teste do segmento completo na placa (opção `VALIDACAO_PLACA` do CMake, desligada por padrão). O C só grava as saídas (`tests/host/validacao/exportar_saidas_c.c`); a comparação é do `tests/host/validacao/validar_blocos.py`, com o `dsp.erro_estagio` do repositório do Python. Como reproduzir está no próprio documento.

## Medições

| Medida | Valor |
|---|---|
| FFT real de 512 pontos (float32), medida com o contador DWT | 35.677 ciclos = 356,8 µs a 100 MHz |
| Blocos 1 a 3 por quadro (janela, espectro e Mel), DWT, mediana dos 490 quadros | 56.240 ciclos = 562 µs (janela 10.723, espectro 40.632, Mel 4.830) |
| Blocos 1 a 3 no segmento de 1 s (98 quadros), DWT, mediana dos 5 segmentos | 5.509.208 ciclos = 55,1 ms (5,5 % do tempo real) |
| Flash usada (firmware até 19/10, Debug, sem o teste dos blocos 1 a 3) | 60.812 B de 512 KB (11,6 %) |
| RAM usada (firmware até 19/10, Debug, sem o teste dos blocos 1 a 3) | 7.216 B de 128 KB (5,5 %) |
| Flash e RAM com a cadeia (`mfcc_segmento()`) e o `lda_escore()` ligados, sem o teste dos blocos 1 a 3 | ~62,7 KB de flash sem contar o áudio (os 5 clipes de 2 s somam 256 KB) e 18.000 B de RAM (13,7 %) |
| Firmware final medido no build de medida: cadeia completa, LDA, 5 clipes, recorte de 51,2 kHz e buffers da decimação no link (seção "Orçamento medido") | 432.460 B de flash (82,5 %) e 29.728 B de RAM (22,7 %), em 20/10 |
| Build padrão com a cadeia completa ligada: clipes, cadeia, LDA e o `cadeia_clipes.c`, sem o recorte nem a decimação | 328.472 B de flash (62,7 %) e 24.520 B de RAM (18,7 %), com a pilha de 4 KB (20/10); 256.215 B da flash são os clipes. O build capturado na placa em 19/10, com a pilha de 1 KB, tinha 328.480 B e 21.896 B |
| Segmento de 1 s na placa, do `int16` ao escore (`cadeia resumo`, mediana dos 10) | 6.145.466 ciclos = 61,45 ms (6,1 % do tempo real); blocos na seção "Cadeia completa na placa" |
| Decimação de 1 s de áudio na placa (51,2 → 12,8 kHz, `decim ciclos`), depois da revisão do PR #9 (20/10) | 11.602.669 ciclos = 116,0 ms (11,6 % do tempo real), dos quais 102,1 ms no FIR; do `int16` a 51,2 kHz ao escore, 174,5 ms (17,5 %). Seção "Decimação na placa" |
| Segmento de 1 s na placa, do `int16` ao escore, com `ARM_MATH_LOOPUNROLL` (20/10) | 5.862.057 ciclos = 58,62 ms (5,9 % do tempo real); blocos na seção "Cadeia completa na placa" |
| Segmento de 1 s na placa, do `int16` ao escore, firmware final (22/10) | 5.855.300 ciclos = 58,55 ms (5,9 % do tempo real), mediana dos 10; blocos na seção "Cadeia completa na placa" |
| Primeira inferência de ponta a ponta no laço, do `int16` a 51,2 kHz ao escore (22/10) | 17.446.485 ciclos = 174,46 ms (17,4 % do tempo real): decimação 116,03 ms, cadeia 58,44 ms. Seção "Primeira inferência de ponta a ponta" |
| Decimação de 1 s de áudio na placa, primeira captura (antes da revisão do PR #9, 20/10) | 19.479.794 ciclos = 194,8 ms (19,5 % do tempo real), dos quais 172,1 ms no FIR; do `int16` a 51,2 kHz ao escore, 256,9 ms (25,7 %) |
| Build padrão com a decimação, depois da revisão do PR #9 (cadeia quadro a quadro, sem o buffer do segundo inteiro, `ARM_MATH_LOOPUNROLL`) | 439.376 B de flash (83,8 %) e 31.536 B de RAM (24,1 %), relatório do linker com o STM32CubeCLT (o build gravado na placa); a decimação ocupa 7.016 B de RAM. Antes da revisão: 436.600 B e 55.744 B |
| Recepção de um segmento pela serial a 921.600 bit/s (31/10, teste com o `.ioc` mudado; o firmware continua a 115.200) | 306 a 307 ms (teórico 276,6 ms, a 925.926 bit/s reais), 10/10 bit a bit, sem erros de linha. Seção "Recepção de segmentos pela serial" |
| Recepção de um segmento pela serial a 115.200 bit/s (30/10) | 2.219 a 2.220 ms (teórico 2.223 ms), sem bytes perdidos; processamento de ~5.843.000 ciclos = 58,4 ms (5,8 % do tempo real). Seção "Recepção de segmentos pela serial" |
| Build padrão com o modo de recepção (30/10) | 449.740 B de flash (85,8 %) e 34.912 B de RAM (26,6 %), relatório do linker (gcc 13.2.1); +4.816 B e +3.168 B sobre o build sem as chamadas, dos quais 2.567 B de flash e 3.164 B de RAM são o `recepcao.c` e o `recepcao_serial.c` |
| Build padrão com a primeira inferência de ponta a ponta no laço (22/10) | 440.676 B de flash (84,1 %) e 31.648 B de RAM (24,1 %), relatório do linker (gcc 13.2.1); +1.300 B e +112 B sobre o build do PR #9. No `tools/orcamento_memoria.py`, a decimação passa a 6.108 B de flash e 7.125 B de RAM |

As linhas "firmware até 19/10" são o build padrão antes da integração da cadeia completa: ele ainda não chamava o `mfcc_segmento()` (só o teste dos blocos 1 a 3, desligado por padrão, chama o `mfcc_estagios_init()`), e o `--gc-sections` tira a cadeia do link. Por isso a RAM não mostra os ~11 KB estáticos da cadeia; eles entram quando a integração na placa chamar o `mfcc_segmento()`. A medida é a mesma de antes (build Debug, relatório do linker). A flash era 24.592 B em 13/10, antes do marco; a diferença, ~36 KB, são os vetores de teste do marco (`lda_vetores_teste.h`, 34.250 B) e o código do marco e da LDA.

Um quadro de 320 amostras a 12,8 kHz dura 25 ms; a FFT ocupa cerca de 1,5% desse tempo.

**Orçamento da flash para o áudio:** reservando 64 KB para o resto do firmware (banco de filtros de Mel, DCT, `.h` da LDA, pente harmônico se for adotado e margem), sobram cerca de 434 KB. Em PCM int16 a 12,8 kHz (25.600 B por segundo), isso dá **cerca de 17 s de áudio decimado**. Com 1 s a 51,2 kHz para a demonstração da decimação na placa, sobram cerca de 13 s a 12,8 kHz. A reserva é uma estimativa e deve ser revista quando a matriz de Mel e o pente estiverem definidos. A medida com tudo no link (18/10) está logo abaixo.

### Orçamento medido (build de medida)

**Desde 20/10 o build padrão já mostra o orçamento do firmware final:** a cadeia roda nos clipes e a decimação roda no recorte, então nada do que o firmware final vai ter fica fora do link. O build de medida continua valendo (mede o mesmo, mais o `orcamento.c`), e os buffers da decimação saíram dele para não contarem duas vezes. Antes, o build padrão não mostrava o orçamento: o `--gc-sections` tirava do link tudo o que ninguém chamava ainda (a cadeia, os clipes, o recorte de 51,2 kHz e a decimação). O **build de medida** (`-DORCAMENTO=ON`, `Core/Src/orcamento.c`) põe essas partes no link **sem executar nenhuma delas**: a chamada fica atrás de uma variável `volatile` que vale sempre 0. Assim, o `.map` mostra o tamanho real de cada parte, e a placa se comporta como no build padrão. Os buffers da decimação têm os tamanhos que o `clipes_demo.h` descreve (estado da CMSIS com 147 + 512 − 1 floats, um bloco de entrada e as saídas de um bloco).

```bash
cmake --preset Debug -DORCAMENTO=ON -DBLOCOS13=OFF
cmake --build --preset Debug
python3 tools/orcamento_memoria.py build/Debug/frente1.map
```

O valor fica no cache do build, como o do `BLOCOS13`; volte com `-DORCAMENTO=OFF`. O script lê o `.map` e soma as seções de entrada pela parte do firmware a que elas pertencem, contando pelo endereço, porque as strings mescladas pelo linker aparecem no `.map` com o tamanho de antes da mescla. Ele só usa a biblioteca padrão do Python. O total pode diferir em alguns bytes do relatório do linker, que conta também os vãos de alinhamento entre seções.

Resultado em 20/10 (build Debug com gcc 13.2.1, branch da cadeia completa, depois do pacote da serial e da pilha de 4 KB). É a saída do `tools/orcamento_memoria.py`:

| Parte | Flash (B) | RAM (B) |
|---|---:|---:|
| clipes a 12,8 kHz | 256.215 | 0 |
| recorte a 51,2 kHz | 103.284 | 0 |
| cadeia (MFCC) | 1.432 | 10.788 |
| cadeia completa na placa (cadeia_clipes.c) | 3.443 | 3.446 |
| decimacao (buffers; a funcao esta na CMSIS-DSP) | 0 | 5.204 |
| build de medida (orcamento.c; nao vai para o firmware final) | 516 | 8 |
| marco e LDA | 36.460 | 0 |
| pacote da serial | 1.398 | 0 |
| CMSIS-DSP | 11.460 | 0 |
| HAL e CubeMX | 6.025 | 13 |
| aplicacao base | 2.196 | 5.220 |
| biblioteca C | 9.856 | 408 |
| alinhamento | 31 | 5 |
| outros | 144 | 28 |
| reserva de heap e pilha (linker script: heap 512 B, pilha 4.096 B) | 0 | 4.608 |
| **total** | **432.460** | **29.728** |

- **Flash:** 432.460 B (82,5 %); sobram 91.828 B (89,7 KiB), ~3,6 s a mais de áudio a 12,8 kHz ou o teto para as tabelas da FFT de 4.096 pontos, se o pente for adotado. O áudio ocupa 359.499 B. O resto do firmware ocupa 72.961 B, **~9 KB acima da reserva de 64 KB** do orçamento. A maior folga para recuperar são os 34.250 B dos vetores de teste do marco (`lda_vetores_teste.h`), que podem sair do firmware final ou encolher com `--por-gravacao K`; os 516 B do `orcamento.c` não vão para o firmware final.
- **RAM:** 29.728 B (22,7 %); sobram 101.344 B (99,0 KiB). Com os ~40 KB do detector de novidade, ainda sobrariam ~59 KiB. Dos 5.220 B da aplicação base, 3.072 B são os buffers da FFT de teste da Frente 1 (`fft_in` e `fft_mag` no `main.c`), que também podem sair do firmware final.
- **Pilha (4 KB desde 20/10):** a cadeia sozinha usa ~0,3 KB, mas a verificação que roda em todo boot chega a ~1,05 KB no caminho mais fundo: `main` → `cadeia_verificar_com()` → `cadeia_linha()` → `vsnprintf` da newlib nano, mais uma interrupção com o contexto da FPU empilhado (estimativa da Amanda na revisão do PR #8, conferida com o `-fstack-usage` e os prólogos das funções da biblioteca no `.elf`). Isso passava da reserva de 1 KB. Na placa não dava erro, porque a reserva não limita a pilha (ela cresce para a RAM livre); ela só faz o linker recusar um firmware em que não sobre aquele espaço, e o `_sbrk()` não deixar o heap entrar nele. Com 4 KB (no `.ioc` e nos três `.ld`), a conta do linker volta a valer, com folga.
- **Histórico:** em 18/10, antes do pacote da serial e da cadeia completa, o mesmo build dava 427.152 B de flash e 23.216 B de RAM (pilha de 1 KB).

## Próximos passos

- **Marco de 13/10:** a placa recebe as 26 características de referência de alguns segmentos, calcula o escore com o `.h` da LDA, confere com o `escores_referencia.csv`, acende o LED e manda o resultado pela serial.
- **13 a 19/10:** porte da cadeia de DSP (janela, FFT, MFCC) para C, com verificação bloco a bloco contra as referências em Python, e definição do formato do pacote da serial.
- **19/10:** cadeia completa no firmware normal (clipes → MFCC → LDA → LEDs e `$SEG` de origem `C`), com os ciclos de cada bloco na placa.
- **20/10:** decimação embarcada (bloco 0) sobre o recorte de 51,2 kHz, passada pela cadeia quadro a quadro e comparada com o Python, com os ciclos pelo DWT. Na revisão do PR #9: `ARM_MATH_LOOPUNROLL` na CMSIS-DSP, a conta em LSB com o `vcvtr` e as tolerâncias medidas nos 295 segmentos.
- **22/10:** primeira inferência de ponta a ponta no laço normal: o recorte de 51,2 kHz decimado e passado pela cadeia até o escore, a cada volta da demonstração, com o LED da decisão e o `$SEG` de origem `D`.
- **29 a 31/10:** recepção de segmentos pela serial (seção "Recepção de segmentos pela serial"): o formato do quadro (29/10), o parser em fluxo, o modo de recepção, o script mínimo, o emulador no PC e o teste na placa a 115.200 bit/s (30/10) e o teste a 921.600 bit/s (31/10), para rodar os 295 segmentos do Jung e os 40 do HUSTmotor na placa.
- **Depois:** caracterização completa de tempo e memória (novembro), o detector de novidade se for adotado (03/11) e o painel no computador (23/11).
