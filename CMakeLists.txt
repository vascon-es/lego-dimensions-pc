cmake_minimum_required(VERSION 3.24)
project(lego_dimensions_pc LANGUAGES C CXX RC)

# Os stubs HLE usam __attribute__((sysv_abi)) e o salto para o entry point usa asm inline GNU:
# so Clang (clang++ ou clang-cl) serve no Windows.
if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR "This project requires Clang (clang++ / clang-cl). Current compiler: ${CMAKE_CXX_COMPILER_ID}")
endif()
if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Supported target: Windows x64")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# ---- Zydis ---------------------------------------------------------------
set(ZYDIS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_MAN OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_DOXYGEN OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZYDIS_FEATURE_ENCODER OFF CACHE BOOL "" FORCE)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/externals/zydis/CMakeLists.txt")
    add_subdirectory(externals/zydis EXCLUDE_FROM_ALL)
else()
    include(FetchContent)
    FetchContent_Declare(zydis
        GIT_REPOSITORY https://github.com/zyantific/zydis.git
        GIT_TAG v4.1.0
        GIT_SUBMODULES_RECURSE ON)
    FetchContent_MakeAvailable(zydis)
endif()

# ---- Executavel ----------------------------------------------------------
include(src/hle/generated/sources.cmake)

# Optional artwork supplied by the owner, outside the source tree.
set(LD_APP_ICON "" CACHE FILEPATH "Owner-supplied application .ico file")
set(LD_ICON_RESOURCE tools/dist/app.rc)
if(LD_APP_ICON)
    if(NOT EXISTS "${LD_APP_ICON}")
        message(FATAL_ERROR "Application icon does not exist: ${LD_APP_ICON}")
    endif()
    get_filename_component(LD_APP_ICON_RC "${LD_APP_ICON}" ABSOLUTE)
    file(TO_CMAKE_PATH "${LD_APP_ICON_RC}" LD_APP_ICON_RC)
    configure_file(tools/dist/icon.rc.in "${CMAKE_CURRENT_BINARY_DIR}/application.rc" @ONLY)
    set(LD_ICON_RESOURCE "${CMAKE_CURRENT_BINARY_DIR}/application.rc")
endif()

add_executable(lego_dimensions
    src/main.cpp
    ${LD_ICON_RESOURCE}
    src/loader/elf_pie_loader.cpp
    src/patcher/fs_tls_patcher.cpp
    src/runtime/orbis_tls.cpp
    src/runtime/crash_handler.cpp
    src/runtime/hwmon.cpp
    src/hle/hle_log.cpp
    src/hle/hle_perf.cpp
    src/hle/overrides/hle_override.cpp
    src/hle/overrides/guest_format.cpp
    src/hle/overrides/hle_kernel_memory.cpp
    src/hle/overrides/hle_kernel_misc.cpp
    src/hle/overrides/hle_pthread.cpp
    src/hle/overrides/hle_libc.cpp
    src/hle/overrides/hle_libc_stdio.cpp
    src/hle/overrides/hle_filesystem.cpp
    src/hle/overrides/hle_fios2.cpp
    src/hle/overrides/hle_kernel_sync.cpp
    src/hle/overrides/hle_userservice_pad.cpp
    src/hle/overrides/hle_audio.cpp
    src/hle/overrides/hle_usbd.cpp
    src/hle/overrides/hle_videoout.cpp
    src/hle/overrides/hle_gnm.cpp
    src/hle/overrides/hle_toypad.cpp
    src/hle/overrides/hle_toypad_listener.cpp
    src/hle/overrides/hle_toypad_hid.cpp
    src/hle/overrides/hle_pad.cpp
    src/hle/overrides/hle_pad_sdl.cpp
    src/hle/overrides/hle_radar.cpp
    src/hle/overrides/hle_mods.cpp
    src/hle/overrides/hle_anomaly.cpp
    src/hle/overrides/hle_savedata.cpp
    src/hle/overrides/hle_offline.cpp
    src/hle/overrides/hle_avplayer.cpp
    src/render/vulkan_backend.cpp
    ${HLE_GENERATED_SOURCES}
)
target_include_directories(lego_dimensions PRIVATE src src/hle/generated)

# SDL3 (Fase 24.5): so o subsistema de gamepad, para o DualShock 4. Biblioteca estatica ja
# compilada (clang-cl, /MD) em externals/sdl3, copiada do build do shadPS4.
target_include_directories(lego_dimensions PRIVATE externals/sdl3/include)
target_link_libraries(lego_dimensions PRIVATE ${CMAKE_SOURCE_DIR}/externals/sdl3/lib/SDL3-static.lib
    imm32 version setupapi ole32 oleaut32 cfgmgr32 advapi32 shell32 uuid)

# Vulkan: so os cabecalhos sao necessarios para compilar (vulkan-1.dll e carregada em tempo de
# execucao). Usa o Vulkan SDK se estiver instalado; senao, os cabecalhos em externals/.
# O SDK continua opcional para camadas de validacao e necessario para compilar
# o tradutor de shaders. O port usa os headers incluidos quando nao ha SDK.
include(cmake/VulkanHeaders.cmake)
target_link_libraries(lego_dimensions PRIVATE Vulkan::Headers)
target_link_libraries(lego_dimensions PRIVATE Zydis winmm user32 gdi32 xinput dbghelp mfplat mfreadwrite mfuuid ole32)
target_compile_definitions(lego_dimensions PRIVATE
    NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS
    LD_PROJECT_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    target_compile_options(lego_dimensions PRIVATE /W3 -Wno-unused-parameter)
else()
    target_compile_options(lego_dimensions PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
# O codigo do jogo roda na pilha da thread principal: 8 MiB, ja com commit total. O codigo do
# Orbis nao faz stack probing, entao a pilha nao pode depender das guard pages do Windows.
target_link_options(lego_dimensions PRIVATE "/STACK:8388608,8388608")
# o .exe distribuido guarda so o nome do .pdb, nao o caminho da maquina que compilou
target_link_options(lego_dimensions PRIVATE "/PDBALTPATH:%_PDB%")

# ---- Ferramenta: desmontagem dos call sites de imports no eboot ------------
add_executable(callsites tools/callsites/callsites.cpp)
target_include_directories(callsites PRIVATE src)
target_link_libraries(callsites PRIVATE Zydis)
target_compile_definitions(callsites PRIVATE _CRT_SECURE_NO_WARNINGS)

# ---- Ferramenta: extrator de PKG do PS4 para o launcher (extracao do shadPS4 0.7.0, inflate do miniz) ----
add_executable(pkg_extractor
    tools/pkg_extractor/main.cpp
    tools/pkg_extractor/pkg.cpp
    tools/pkg_extractor/pkg_type.cpp
    tools/pkg_extractor/crypto.cpp
    tools/pkg_extractor/miniz/miniz_tinfl.c)
target_include_directories(pkg_extractor PRIVATE tools/pkg_extractor tools/pkg_extractor/miniz)
target_compile_definitions(pkg_extractor PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(pkg_extractor PRIVATE bcrypt)
target_link_options(pkg_extractor PRIVATE "/PDBALTPATH:%_PDB%")
set_property(TARGET pkg_extractor PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded") # sem DLL do runtime, como o launcher

include(CTest)
if(BUILD_TESTING)
    add_executable(shader_metadata_test tools/shader_metadata_test.cpp)
    target_include_directories(shader_metadata_test PRIVATE src)
    add_test(NAME shader_metadata_test COMMAND shader_metadata_test)
    add_executable(region_patch_test tools/region_patch_test.cpp)
    target_include_directories(region_patch_test PRIVATE src)
    add_test(NAME region_patch_test COMMAND region_patch_test)
    add_executable(pkg_io_test tools/pkg_extractor/io_test.cpp)
    target_compile_definitions(pkg_io_test PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
    add_test(NAME pkg_io_test COMMAND pkg_io_test)
endif()
