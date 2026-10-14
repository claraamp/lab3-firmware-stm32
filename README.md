# lab3-firmware-stm32

Firmware do projeto de **diagnóstico acústico de motores** (ENGG54, Laboratório Integrado III, UFBA), Fase 2: porte embarcado para o STM32F411CEU6.

O protótipo em Python (extração de características, treino e escolha do classificador) fica em [`claraamp/diagnostico-acustico-motores`](https://github.com/claraamp/diagnostico-acustico-motores). Este repositório guarda só o que roda na placa e o diagrama de circuito.

## Estado atual

O firmware tem a base da Fase 2 (Frente 1: placa, serial, LEDs, CMSIS-DSP e DWT) e o **marco de 13/10**: a LDA roda na placa com as características de referência do Python. A cadeia de MFCC ainda não está aqui; por isso as 26 características vêm prontas do Python, e o marco confere o porte do classificador, não o desempenho (os segmentos são os do treino do modelo final).

Ao ligar, a placa:

1. manda `hello frente1`, `fft_init status=0` e uma linha da FFT de teste (seno no bin 32):

   ```
   fft512 ciclos=35677 us=356.8 pico_bin=32 mag=256.0
   ```

   O pico no bin 32 com magnitude 256,0 (= N/2) é o valor teórico esperado.
2. **confere a LDA em todos os segmentos** do `lda_vetores_teste.h`: calcula o escore com o `lda_escore()` do `lda_modelo.h`, compara com o escore do `escores_referencia.csv` e manda uma linha por segmento, um resumo e `MARCO OK` ou `MARCO FALHOU`:

   ```
   marco13 modelo=exp239 json=ee0c3e088366 n_features=26 segmentos=295 clock=100MHz
   seg seg=0 grav=normal ref=-1577.959 placa=... dif=... tol=... dec=normal OK ciclos=...
   ...
   resumo ok=295/295 decisoes_iguais=295/295 dif_max=... (seg ...) dif/tol_max=...% ciclos min=... med=... max=... (med ... us)
   MARCO OK
   ```

   Um segmento passa quando a decisão é a do CSV e `dif` ≤ `tol`. A tolerância de cada segmento vem do gerador: é o limite do arredondamento da soma em float32, o mesmo do `06`, mais o arredondamento do escore de referência para float32. Os `ciclos` são os do `lda_escore()` com a chamada, medidos pelo DWT; o `marco_lda.c` é sempre compilado com `-O2`, inclusive no Debug, para o tempo ser o do firmware final.
3. em laço, **demonstração nos LEDs**: para cada gravação, o segmento de menor |escore| (o mais perto da fronteira). LED verde = normal, vermelho = falha, por 2 s, com uma linha `demo ...` na serial. Se a conferência falhou, os dois LEDs piscam juntos e rápido (isso não é uma decisão) e a serial repete o aviso. O LED da placa (PC13) fica aceso depois da conferência.

### Cabeçalhos do modelo

Os dois vêm do repositório do Python e não se editam a mão:

| Arquivo em `Core/Inc/` | Gerado por (em `diagnostico-acustico-motores`) |
|---|---|
| `lda_modelo.h` | `scripts/pipeline/06_export_lda_header.py` → `reports/modelo_final/lda_modelo.h` |
| `lda_vetores_teste.h` | `scripts/pipeline/07_export_lda_test_vectors.py` → `reports/modelo_final/lda_vetores_teste.h` |

Se o modelo final mudar, regenere os dois e copie os dois. O firmware recusa uma combinação de modelos diferentes: compara o `lda_bias` do `lda_modelo.h` com o `VT_LDA_BIAS` do outro e, se forem diferentes, manda um `ERRO` e pisca os dois LEDs. Com os 295 segmentos, os vetores ocupam cerca de 36 KB de flash; `--por-gravacao K` no gerador reduz.

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
                             marco_lda.c/.h e os cabeçalhos do modelo, lda_modelo.h e lda_vetores_teste.h)
tests/host/                  teste no PC da parte do marco que não usa a HAL
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

## Medições

| Medida | Valor |
|---|---|
| FFT real de 512 pontos (float32), medida com o contador DWT | 35.677 ciclos = 356,8 µs a 100 MHz |
| Flash usada (firmware atual) | 24.592 B de 512 KB (4,7%) |
| RAM usada | 7.216 B de 128 KB (5,5%) |

Um quadro de 320 amostras a 12,8 kHz dura 25 ms; a FFT ocupa cerca de 1,5% desse tempo.

**Orçamento da flash para o áudio:** reservando 64 KB para o resto do firmware (banco de filtros de Mel, DCT, `.h` da LDA, pente harmônico se for adotado e margem), sobram cerca de 434 KB. Em PCM int16 a 12,8 kHz (25.600 B por segundo), isso dá **cerca de 17 s de áudio decimado**. Com 1 s a 51,2 kHz para a demonstração da decimação na placa, sobram cerca de 13 s a 12,8 kHz. A reserva é uma estimativa e deve ser revista quando a matriz de Mel e o pente estiverem definidos.

## Próximos passos

- **Marco de 13/10:** a placa recebe as 26 características de referência de alguns segmentos, calcula o escore com o `.h` da LDA, confere com o `escores_referencia.csv`, acende o LED e manda o resultado pela serial.
- **13 a 19/10:** porte da cadeia de DSP (janela, FFT, MFCC) para C, com verificação bloco a bloco contra as referências em Python, e definição do formato do pacote da serial.
