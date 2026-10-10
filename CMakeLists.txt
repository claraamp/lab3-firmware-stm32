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
)

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
