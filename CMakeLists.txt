cmake_minimum_required(VERSION 3.20)
project(eragon_recomp C)

set(CMAKE_C_STANDARD 11)
file(GLOB RECOMP_SRC CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/out/*.c)

add_executable(eragon
    runtime/runtime.c
    runtime/kernel.c
    runtime/vu0.c
    runtime/hw.c
    runtime/iop.c
    runtime/iop_servers.c
    runtime/iop_mc.c
    runtime/iop_sfiop.c
    runtime/iop_audio.c
    runtime/iop_file.c
    runtime/iop_bink.c
    runtime/iop_pad.c
    runtime/gs.c
    runtime/gif.c
    runtime/vif.c
    runtime/vu1.c
    runtime/window.c
    runtime/profiler.c
    runtime/gs_gpu.c
    ${CMAKE_BINARY_DIR}/gs_gpu_hlsl.h
    ${RECOMP_SRC})
target_include_directories(eragon PRIVATE runtime out ${CMAKE_BINARY_DIR})

# The GS compute shader is compiled at startup from source embedded as a C string.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/gs_gpu_hlsl.h
    COMMAND Python3::Interpreter -I ${CMAKE_SOURCE_DIR}/tools/embed_text.py ${CMAKE_SOURCE_DIR}/runtime/gs_gpu.hlsl
            ${CMAKE_BINARY_DIR}/gs_gpu_hlsl.h g_gs_hlsl
    DEPENDS ${CMAKE_SOURCE_DIR}/runtime/gs_gpu.hlsl ${CMAKE_SOURCE_DIR}/tools/embed_text.py
    COMMENT "Embedding gs_gpu.hlsl")
# The EE FPU has no Inf/NaN and flushes denormals; emulate that in every FPU op.
target_compile_definitions(eragon PRIVATE PS2_FPU_CLAMP)

if(MSVC)
    # Generated files are large; /bigobj and fast optimisation keep build times sane.
    target_compile_options(eragon PRIVATE /O1 /bigobj /wd4102 /wd4146)
else()
    target_compile_options(eragon PRIVATE -O1 -fno-strict-aliasing -Wno-unused-label)
    target_link_libraries(eragon PRIVATE m)
endif()

if(MSVC)
    # Hand-written runtime: full optimisation (the generated code stays at /O1 for build time).
    file(GLOB RUNTIME_SRC ${CMAKE_SOURCE_DIR}/runtime/*.c)
    set_source_files_properties(${RUNTIME_SRC} PROPERTIES COMPILE_OPTIONS "/O2;/fp:fast;/Ob2;/Zi")
    # Debug info for the hand-written runtime so PS2_PROFILE can name functions.
    target_link_options(eragon PRIVATE /DEBUG /OPT:REF /OPT:ICF)
endif()

if(WIN32)
    target_link_libraries(eragon PRIVATE user32 gdi32 winmm dbghelp d3d11 d3dcompiler dxguid)
endif()
