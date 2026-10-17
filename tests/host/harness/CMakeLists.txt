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
# quadrado), com as mesmas tabelas restritas do firmware (CMakeLists.txt da raiz).
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
