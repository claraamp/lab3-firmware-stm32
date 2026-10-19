cmake_minimum_required(VERSION 3.22)

#
# This file is generated only once,
# and is not re-generated if converter is called multiple times.
#
# User is free to modify the file as much as necessary
#

# Setup compiler settings
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)


# Define the build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

# Set the project name
set(CMAKE_PROJECT_NAME frente1)

# Enable compile command to ease indexing with e.g. clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# Core project settings
project(${CMAKE_PROJECT_NAME})
message("Build type: " ${CMAKE_BUILD_TYPE})

# Enable CMake support for ASM and C languages
enable_language(C ASM)

# Create an executable object type
add_executable(${CMAKE_PROJECT_NAME})

# Add STM32CubeMX generated sources
add_subdirectory(cmake/stm32cubemx)

# CMSIS-DSP (copied from STM32Cube_FW_F4_V1.28.3), built from the per-module
# aggregate sources. Kept out of cmake/stm32cubemx because CubeMX regenerates it.
set(CMSISDSP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/Drivers/CMSIS/DSP)
add_library(CMSISDSP STATIC
    ${CMSISDSP_DIR}/Source/BasicMathFunctions/BasicMathFunctions.c
    ${CMSISDSP_DIR}/Source/CommonTables/CommonTables.c
    ${CMSISDSP_DIR}/Source/ComplexMathFunctions/ComplexMathFunctions.c
    ${CMSISDSP_DIR}/Source/FastMathFunctions/FastMathFunctions.c
    ${CMSISDSP_DIR}/Source/FilteringFunctions/FilteringFunctions.c
    ${CMSISDSP_DIR}/Source/StatisticsFunctions/StatisticsFunctions.c
    ${CMSISDSP_DIR}/Source/SupportFunctions/SupportFunctions.c
    ${CMSISDSP_DIR}/Source/TransformFunctions/TransformFunctions.c
)
target_include_directories(CMSISDSP
    PUBLIC  ${CMSISDSP_DIR}/Include
    PRIVATE ${CMSISDSP_DIR}/PrivateInclude
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/Drivers/CMSIS/Include
)
# Always optimized, even in Debug, so DWT cycle counts are realistic
target_compile_options(CMSISDSP PRIVATE -O3)
# Only keep the tables needed by a 512-point real FFT (complex 256-point core)
# and by arm_sin_f32; any other arm_rfft_fast_init_f32 length now returns
# ARM_MATH_ARGUMENT_ERROR
target_compile_definitions(CMSISDSP PUBLIC
    ARM_DSP_CONFIG_TABLES
    ARM_FFT_ALLOW_TABLES
    ARM_TABLE_TWIDDLECOEF_F32_256
    ARM_TABLE_BITREVIDX_FLT_256
    ARM_TABLE_TWIDDLECOEF_RFFT_F32_512
    ARM_FAST_ALLOW_TABLES
    ARM_TABLE_SIN_F32
)

# Link directories setup
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined library search paths
)

# Add sources to executable
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user sources here
    Core/Src/marco_lda.c
    Core/Src/mfcc_estagios.c
    # pacote da serial (formato v1) e o formatador de ponto fixo que ele e o marco usam
    Core/Src/pacote_serial.c
    Core/Src/texto_fixo.c
    # clipes de audio da Frente 3C (clipes_demo.h, gerado no repositorio do Python);
    # o que nenhum .c usa sai no link pelo --gc-sections
    Core/Src/clipes_demo.c
)
# Marco de 13/10 (LDA na placa) e estagios do MFCC: sempre otimizados, inclusive
# no Debug, para o tempo medido pelo DWT ser o do firmware final, como o CMSIS-DSP
set_source_files_properties(Core/Src/marco_lda.c Core/Src/mfcc_estagios.c
    PROPERTIES COMPILE_OPTIONS "-O2")

# Teste dos blocos 1 a 3 na placa (janela, espectro e Mel; Core/Src/blocos13.c):
# ao ligar, roda o primeiro segmento de cada clipe do clipes_demo.h (os 5 das
# referencias do harness), mede os ciclos e manda a saida de cada bloco pela serial
# para o receber_blocos13.py. Le o audio dos clipes, sem copia propria.
# DESLIGADO por padrao: o envio leva ~100 s e atrasa o marco e os LEDs, o que numa
# demonstracao parece placa travada. Para rodar o teste: cmake --preset Debug -DBLOCOS13=ON
# (o valor fica no cache do build; volte com -DBLOCOS13=OFF).
option(BLOCOS13 "Teste dos blocos 1 a 3 na placa ao ligar (~100 s pela serial)" OFF)
if(BLOCOS13)
    target_sources(${CMAKE_PROJECT_NAME} PRIVATE Core/Src/blocos13.c)
    set_source_files_properties(Core/Src/blocos13.c PROPERTIES COMPILE_OPTIONS "-O2")
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE FIRMWARE_BLOCOS13)
endif()

# Validacao do segmento completo na placa (Core/Src/validacao_placa.c): ao ligar,
# roda o mfcc_segmento() e o lda_escore() nos 10 segmentos dos clipes e manda as 26
# caracteristicas e o escore pela serial, para o validar_blocos.py --placa comparar
# com o Python. Leva menos de 1 s, mas fica DESLIGADO por padrao, como o BLOCOS13:
# cmake --preset Debug -DVALIDACAO_PLACA=ON
option(VALIDACAO_PLACA "Validacao do segmento completo na placa ao ligar" OFF)
if(VALIDACAO_PLACA)
    target_sources(${CMAKE_PROJECT_NAME} PRIVATE Core/Src/validacao_placa.c)
    set_source_files_properties(Core/Src/validacao_placa.c PROPERTIES COMPILE_OPTIONS "-O2")
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE FIRMWARE_VALIDACAO_PLACA)
    if(BLOCOS13)
        # os dois testes rodam no boot; o binario do blocos13 sai antes e o
        # validar_blocos.py nao acha a validacao no meio dele
        message(WARNING "VALIDACAO_PLACA e BLOCOS13 ligados juntos (BLOCOS13 pode ter "
                        "ficado no cache): para a validacao, use -DBLOCOS13=OFF")
    endif()
endif()

# Build de medida do orcamento de flash e RAM (Core/Src/orcamento.c): poe no link
# tudo o que o firmware final vai ter e que hoje o --gc-sections descarta (cadeia
# completa, LDA, os 5 clipes, o recorte de 51,2 kHz e os buffers da decimacao),
# SEM EXECUTAR nada disso, para medir pelo .map com o tools/orcamento_memoria.py.
# DESLIGADO por padrao. Para medir: cmake --preset Debug -DORCAMENTO=ON (o valor
# fica no cache do build, como o BLOCOS13; volte com -DORCAMENTO=OFF).
option(ORCAMENTO "Build de medida: cadeia, clipes e decimacao no link, sem executar" OFF)
if(ORCAMENTO)
    target_sources(${CMAKE_PROJECT_NAME} PRIVATE Core/Src/orcamento.c)
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE FIRMWARE_ORCAMENTO)
endif()

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined include paths
)

# Add project symbols (macros)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined symbols
)

# Remove wrong libob.a library dependency when using cpp files
list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

# Add linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32cubemx

    # Add user defined libraries
    CMSISDSP
)
