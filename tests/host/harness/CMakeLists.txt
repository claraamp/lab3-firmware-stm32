# Harness no PC (Frente 3D): os estagios portados em Core/Src/mfcc_estagios.c,
# com a implementacao generica em C da CMSIS-DSP (sem ARM_MATH_CM4 nem
# intrinsecos do Cortex-M), conferidos contra as referencias do Python.
#
#   cmake -S tests/host/harness -B build/harness
#   cmake --build build/harness
#   build/harness/harness --refs ../diagnostico-acustico-motores/reports/c_reference/estagios
#
# Projeto separado do firmware: usa o compilador do PC, nao o arm-none-eabi.
cmake_minimum_required(VERSION 3.20)
project(harness_estagios C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(RAIZ ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
set(DSP ${RAIZ}/Drivers/CMSIS/DSP)

# So as funcoes que os estagios usam (FFT real de 512, produto e modulo ao
# quadrado) e o decimador da decimacao embarcada, com as mesmas tabelas restritas
# do firmware (CMakeLists.txt da raiz).
add_library(cmsisdsp_host STATIC
    ${DSP}/Source/TransformFunctions/arm_rfft_fast_f32.c
    ${DSP}/Source/TransformFunctions/arm_rfft_fast_init_f32.c
    ${DSP}/Source/TransformFunctions/arm_cfft_f32.c
    ${DSP}/Source/TransformFunctions/arm_cfft_init_f32.c
    ${DSP}/Source/TransformFunctions/arm_cfft_radix8_f32.c
    ${DSP}/Source/TransformFunctions/arm_bitreversal2.c
    ${DSP}/Source/CommonTables/arm_common_tables.c
    ${DSP}/Source/CommonTables/arm_const_structs.c
    ${DSP}/Source/ComplexMathFunctions/arm_cmplx_mag_squared_f32.c
    ${DSP}/Source/BasicMathFunctions/arm_mult_f32.c
    ${DSP}/Source/BasicMathFunctions/arm_dot_prod_f32.c
    ${DSP}/Source/FilteringFunctions/arm_fir_decimate_f32.c
    ${DSP}/Source/FilteringFunctions/arm_fir_decimate_init_f32.c
)
target_include_directories(cmsisdsp_host
    PUBLIC  ${DSP}/Include
    PRIVATE ${DSP}/PrivateInclude
)
target_compile_definitions(cmsisdsp_host PUBLIC
    ARM_DSP_CONFIG_TABLES
    ARM_FFT_ALLOW_TABLES
    ARM_TABLE_TWIDDLECOEF_F32_256
    ARM_TABLE_BITREVIDX_FLT_256
    ARM_TABLE_TWIDDLECOEF_RFFT_F32_512
    # como no firmware (CMakeLists.txt da raiz): o mesmo caminho da CMSIS
    ARM_MATH_LOOPUNROLL
)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    # no PC, o arm_math_types.h nao pode incluir o cmsis_compiler.h (intrinsecos do
    # Cortex-M); esta macro e a que a propria CMSIS-DSP usa nos builds de host
    target_compile_definitions(cmsisdsp_host PUBLIC __GNUC_PYTHON__)
endif()

add_executable(harness
    harness.c
    ${RAIZ}/Core/Src/mfcc_estagios.c
)
target_include_directories(harness PRIVATE ${RAIZ}/Core/Inc)
target_link_libraries(harness PRIVATE cmsisdsp_host)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(harness PRIVATE -Wall -Wextra -Wdouble-promotion)
    target_link_libraries(harness PRIVATE m)
elseif(MSVC)
    target_compile_options(harness PRIVATE /W3)
    target_compile_definitions(harness PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# Testes das tabelas (bordas do Mel em tests/host/test_mel_bordas.c; matriz da DCT,
# log e Welford em tests/host/test_log_dct.c) e emulador do teste dos blocos 1 a 3
# na placa (tests/host/blocos13/): usam os mesmos estagios e a mesma CMSIS-DSP
# generica do harness.
add_executable(test_mel_bordas
    ${RAIZ}/tests/host/test_mel_bordas.c
    ${RAIZ}/Core/Src/mfcc_estagios.c
)
add_executable(test_log_dct
    ${RAIZ}/tests/host/test_log_dct.c
    ${RAIZ}/Core/Src/mfcc_estagios.c
)
add_executable(emular_blocos13
    ${RAIZ}/tests/host/blocos13/emular_blocos13.c
    ${RAIZ}/Core/Src/blocos13.c
    ${RAIZ}/Core/Src/mfcc_estagios.c
    ${RAIZ}/Core/Src/clipes_demo.c
    ${RAIZ}/Core/Src/crc32.c
)
target_compile_definitions(emular_blocos13 PRIVATE BLOCOS13_HOST)

# Validacao bloco a bloco (tests/host/validacao/): o exportador grava a saida de
# cada estagio do C, sem comparar (a comparacao e do validar_blocos.py, com o
# dsp.erro_estagio do Python), e o emulador gera a saida do teste do segmento
# completo na placa (Core/Src/validacao_placa.c) sem a placa.
add_executable(exportar_saidas_c
    ${RAIZ}/tests/host/validacao/exportar_saidas_c.c
    ${RAIZ}/Core/Src/mfcc_estagios.c
)
add_executable(emular_validacao_placa
    ${RAIZ}/tests/host/validacao/emular_validacao_placa.c
    ${RAIZ}/Core/Src/validacao_placa.c
    ${RAIZ}/Core/Src/mfcc_estagios.c
    ${RAIZ}/Core/Src/clipes_demo.c
)
target_compile_definitions(emular_validacao_placa PRIVATE VALIDACAO_HOST)

# Cadeia completa do firmware normal (Core/Src/cadeia_clipes.c): a verificacao que a
# placa faz ao ligar, os pacotes $SEG de origem C e a comparacao que tem que reprovar.
add_executable(test_cadeia_clipes
    ${RAIZ}/tests/host/test_cadeia_clipes.c
    ${RAIZ}/Core/Src/cadeia_clipes.c
    ${RAIZ}/Core/Src/mfcc_estagios.c
    ${RAIZ}/Core/Src/clipes_demo.c
    ${RAIZ}/Core/Src/pacote_serial.c
    ${RAIZ}/Core/Src/texto_fixo.c
)
target_compile_definitions(test_cadeia_clipes PRIVATE CADEIA_HOST)

# Decimacao embarcada (Core/Src/decimacao.c): a verificacao que a placa faz ao
# ligar, a fase do decimador, os criterios que tem que reprovar e as saidas em float
# para o tests/host/decimacao/comparar_decimacao.py.
add_executable(test_decimacao
    ${RAIZ}/tests/host/test_decimacao.c
    ${RAIZ}/Core/Src/decimacao.c
    ${RAIZ}/Core/Src/cadeia_clipes.c
    ${RAIZ}/Core/Src/mfcc_estagios.c
    ${RAIZ}/Core/Src/clipes_demo.c
    ${RAIZ}/Core/Src/pacote_serial.c
    ${RAIZ}/Core/Src/texto_fixo.c
    ${RAIZ}/Core/Src/crc32.c
)
target_compile_definitions(test_decimacao PRIVATE DECIMACAO_HOST CADEIA_HOST)

# Uma gravacao inteira a 51,2 kHz pelo decimador do firmware, para o
# tests/host/decimacao/tolerancias_gravacoes.py medir os 295 segmentos.
add_executable(decimar_gravacao
    ${RAIZ}/tests/host/decimacao/decimar_gravacao.c
    ${RAIZ}/Core/Src/decimacao.c
    ${RAIZ}/Core/Src/cadeia_clipes.c
    ${RAIZ}/Core/Src/mfcc_estagios.c
    ${RAIZ}/Core/Src/clipes_demo.c
    ${RAIZ}/Core/Src/pacote_serial.c
    ${RAIZ}/Core/Src/texto_fixo.c
    ${RAIZ}/Core/Src/crc32.c
)
target_compile_definitions(decimar_gravacao PRIVATE DECIMACAO_HOST CADEIA_HOST)

# CRC-32 comum (Core/Src/crc32.c): o vetor 123456789, o CRC continuado e o das
# amostras int16 em little-endian, contra valores do zlib do Python.
add_executable(test_crc32
    ${RAIZ}/tests/host/test_crc32.c
    ${RAIZ}/Core/Src/crc32.c
)

# Recepcao de segmentos pela serial (Core/Src/recepcao.c): o parser do quadro em
# fluxo, com os 10 segmentos dos clipes bit a bit contra o mfcc_segmento(), e os
# erros (crc, cabecalho, truncado, silencio).
add_executable(test_recepcao
    ${RAIZ}/tests/host/test_recepcao.c
    ${RAIZ}/Core/Src/recepcao.c
    ${RAIZ}/Core/Src/crc32.c
    ${RAIZ}/Core/Src/mfcc_estagios.c
    ${RAIZ}/Core/Src/clipes_demo.c
)
foreach(alvo test_mel_bordas test_log_dct emular_blocos13 exportar_saidas_c emular_validacao_placa
        test_cadeia_clipes test_decimacao decimar_gravacao test_crc32 test_recepcao)
    target_include_directories(${alvo} PRIVATE ${RAIZ}/Core/Inc)
    target_link_libraries(${alvo} PRIVATE cmsisdsp_host)
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${alvo} PRIVATE -Wall -Wextra -Wdouble-promotion)
        target_link_libraries(${alvo} PRIVATE m)
    elseif(MSVC)
        target_compile_options(${alvo} PRIVATE /W3)
        target_compile_definitions(${alvo} PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
endforeach()

# Modo de recepcao da placa num pseudo-terminal (tests/host/recepcao/emular_placa.c):
# o Core/Src/recepcao_serial.c de verdade, contra uma HAL de mentira (stub/), para o
# enviar_segmento.py conversar com ele sem a placa. So no Linux e no macOS.
if(UNIX)
    find_package(Threads REQUIRED)
    add_executable(emular_placa
        ${RAIZ}/tests/host/recepcao/emular_placa.c
        ${RAIZ}/Core/Src/recepcao_serial.c
        ${RAIZ}/Core/Src/recepcao.c
        ${RAIZ}/Core/Src/crc32.c
        ${RAIZ}/Core/Src/cadeia_clipes.c
        ${RAIZ}/Core/Src/mfcc_estagios.c
        ${RAIZ}/Core/Src/clipes_demo.c
        ${RAIZ}/Core/Src/pacote_serial.c
        ${RAIZ}/Core/Src/texto_fixo.c
    )
    # o stub/ vem antes do Core/Inc: o main.h e o usart.h de mentira
    target_include_directories(emular_placa PRIVATE ${RAIZ}/tests/host/recepcao/stub ${RAIZ}/Core/Inc)
    target_compile_definitions(emular_placa PRIVATE CADEIA_HOST)
    target_link_libraries(emular_placa PRIVATE cmsisdsp_host m Threads::Threads)
    target_compile_options(emular_placa PRIVATE -Wall -Wextra -Wdouble-promotion)
endif()
