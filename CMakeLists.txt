cmake_minimum_required(VERSION 3.25)
project(lmport LANGUAGES C CXX VERSION 1.7.4)
if (WIN32)
    enable_language(RC)  # the exe icon (packaging/windows/lmcoop.rc)
endif ()

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(AURORA_ENABLE_DVD ON CACHE BOOL "" FORCE)
set(AURORA_ENABLE_CARD ON CACHE BOOL "" FORCE)
# Local fixes to Aurora, kept as patches so the submodule stays pristine.
# The applied set is recorded in a stamp (hash of every patch, in order); when the set changes,
# the submodule is reset to pristine and the whole series re-applied, since later patches may
# touch the same lines as earlier ones (a per-patch reverse check can't tell what's applied).
file(GLOB AURORA_PATCHES ${CMAKE_SOURCE_DIR}/patches/aurora/*.patch)
list(SORT AURORA_PATCHES)
set(_patch_ids "")
foreach (_patch ${AURORA_PATCHES})
    file(SHA1 ${_patch} _sha)
    get_filename_component(_name ${_patch} NAME)
    string(APPEND _patch_ids "${_name}:${_sha}\n")
endforeach ()
set(_aurora_dir ${CMAKE_SOURCE_DIR}/extern/aurora)
execute_process(COMMAND git rev-parse --git-dir WORKING_DIRECTORY ${_aurora_dir}
                OUTPUT_VARIABLE _aurora_git OUTPUT_STRIP_TRAILING_WHITESPACE)
get_filename_component(_aurora_git ${_aurora_git} ABSOLUTE BASE_DIR ${_aurora_dir})
set(_stamp ${_aurora_git}/lm_patches_applied)
set(_stamped "")
if (EXISTS ${_stamp})
    file(READ ${_stamp} _stamped)
endif ()
if (NOT _stamped STREQUAL _patch_ids)
    # The patches are LF; Git for Windows' system-wide core.autocrlf=true would check the
    # submodule out with CRLF. Force LF and rewrite every file before applying.
    execute_process(COMMAND git config core.autocrlf false WORKING_DIRECTORY ${_aurora_dir})
    execute_process(COMMAND git checkout -q -- . WORKING_DIRECTORY ${_aurora_dir})
    execute_process(COMMAND git clean -fdq WORKING_DIRECTORY ${_aurora_dir})
    # checkout-index -f does not rewrite files git considers unchanged (stat-clean CRLF copies
    # from an earlier autocrlf checkout stay CRLF and the patches then fail): drop the index so
    # every file is written again.
    execute_process(COMMAND git rm -r -q --cached . WORKING_DIRECTORY ${_aurora_dir} OUTPUT_QUIET ERROR_QUIET)
    execute_process(COMMAND git reset -q --hard WORKING_DIRECTORY ${_aurora_dir})
    execute_process(COMMAND git checkout-index -f -a WORKING_DIRECTORY ${_aurora_dir})
    foreach (_patch ${AURORA_PATCHES})
        execute_process(COMMAND git apply ${_patch} WORKING_DIRECTORY ${_aurora_dir} RESULT_VARIABLE _applied)
        if (NOT _applied EQUAL 0)
            file(REMOVE ${_stamp})
            message(FATAL_ERROR "Failed to apply Aurora patch ${_patch}")
        endif ()
        message(STATUS "Applied Aurora patch ${_patch}")
    endforeach ()
    file(WRITE ${_stamp} "${_patch_ids}")
endif ()
add_subdirectory(extern/aurora EXCLUDE_FROM_ALL)

# ---- Internet play (docs/ONLINE_INTERNET.md) ---------------------------------------------------
# libdatachannel (MPL-2.0: WebRTC data channels, ICE with STUN/TURN, DTLS) on MbedTLS (Apache-2.0),
# plus nlohmann/json (MIT) for the matchmaking messages. All three are built from source and linked
# statically; nothing is installed. MbedTLS needs DTLS-SRTP switched on (the lmcoop config header).
include(FetchContent)
# Kept out of `cmake --install` (the release package): with install rules skipped, CMake still lists
# each fetched directory's cmake_install.cmake in the top-level one but never writes it (the install step
# then fails), so empty ones are written below.
set(_lm_skip_install ${CMAKE_SKIP_INSTALL_RULES})
set(CMAKE_SKIP_INSTALL_RULES ON)
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(mbedtls GIT_REPOSITORY https://github.com/Mbed-TLS/mbedtls.git GIT_TAG mbedtls-3.6.7 GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(mbedtls)
foreach (_t mbedcrypto mbedx509 mbedtls)
    target_compile_definitions(${_t} PUBLIC "MBEDTLS_USER_CONFIG_FILE=\"${CMAKE_SOURCE_DIR}/packaging/mbedtls/lmcoop_config.h\"")
endforeach ()
add_library(lm_mbedtls INTERFACE)
target_link_libraries(lm_mbedtls INTERFACE mbedtls mbedx509 mbedcrypto)
add_library(MbedTLS::MbedTLS ALIAS lm_mbedtls)  # the name libdatachannel looks for
set(USE_MBEDTLS ON CACHE BOOL "" FORCE)
set(NO_MEDIA ON CACHE BOOL "" FORCE)
set(NO_EXAMPLES ON CACHE BOOL "" FORCE)
set(NO_TESTS ON CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
# The json submodule is skipped: its test data has paths too long for Windows checkouts.
FetchContent_Declare(datachannel GIT_REPOSITORY https://github.com/paullouisageneau/libdatachannel.git
    GIT_TAG v0.24.6 GIT_SHALLOW TRUE GIT_SUBMODULES "deps/libjuice deps/plog deps/usrsctp")
FetchContent_MakeAvailable(datachannel)
FetchContent_Declare(nlohmann_json URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d)
FetchContent_MakeAvailable(nlohmann_json)
set(CMAKE_SKIP_INSTALL_RULES ${_lm_skip_install})
foreach (_d mbedtls datachannel nlohmann_json)
    file(WRITE ${FETCHCONTENT_BASE_DIR}/${_d}-build/cmake_install.cmake "# fetched dependency: not installed
")
endforeach ()


# ---- Shared game/library compile environment -------------------------------
add_library(lm_game_headers INTERFACE)
target_include_directories(lm_game_headers INTERFACE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/libs/JSystem/include
    ${CMAKE_SOURCE_DIR}/extern/aurora/include/dolphin
    ${CMAKE_SOURCE_DIR}/extern/aurora/include
    ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(lm_game_headers INTERFACE TARGET_PC=1 AVOID_UB=1 VERSION=0 WIDESCREEN_SUPPORT=1)
# Game and JSystem headers assume global.h is the first include of every translation unit.
if (CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    target_compile_options(lm_game_headers INTERFACE "$<$<COMPILE_LANGUAGE:C,CXX>:/FIglobal.h>")
else ()
    target_compile_options(lm_game_headers INTERFACE "$<$<COMPILE_LANGUAGE:C,CXX>:-includeglobal.h>")
endif ()
set(GAME_LIBS lm_game_headers aurora::core aurora::gx aurora::gd aurora::si aurora::vi aurora::pad
    aurora::mtx aurora::os aurora::dvd aurora::card)

# ---- JSystem (ported base from Dusklight, CC0) ------------------------------
set(JSYSTEM_LIBRARIES)
foreach (_jsys JKernel JSupport JMath JUtility JGadget J2DGraph)
    file(GLOB _jsys_src CONFIGURE_DEPENDS libs/JSystem/src/${_jsys}/*.cpp)
    add_library(JSystem_${_jsys} STATIC ${_jsys_src})
    target_link_libraries(JSystem_${_jsys} PUBLIC ${GAME_LIBS})
    list(APPEND JSYSTEM_LIBRARIES JSystem_${_jsys})
endforeach ()

# Platform layer (window, main loop, frame pacing, interpolation, settings).
# Decompiled game code from decomp/ is added to this target unit-by-unit as
# each unit is made to compile against Aurora (see docs/ROADMAP.md, Phase 1).
file(GLOB PORT_SOURCES CONFIGURE_DEPENDS src/port/*.cpp src/port/os/*.cpp src/helpers/*.cpp)
file(GLOB_RECURSE GAME_SOURCES CONFIGURE_DEPENDS src/game/*.cpp)
add_executable(lmport ${PORT_SOURCES} ${GAME_SOURCES})
target_include_directories(lmport PRIVATE src)
target_link_libraries(lmport PRIVATE aurora::main ${GAME_LIBS} ${JSYSTEM_LIBRARIES})
if (WIN32)
    target_link_libraries(lmport PRIVATE dbghelp winhttp bcrypt)
endif ()

if (WIN32)
    # Ship runtime DLLs (SDL3, Dawn, nod, libpng, zlib) next to the executable.
    # dxcompiler/dxil are loaded dynamically by Dawn's D3D12 backend, so they are
    # not part of TARGET_RUNTIME_DLLS and must be copied explicitly.
    add_custom_command(TARGET lmport POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:lmport> $<TARGET_FILE_DIR:lmport>
        COMMAND_EXPAND_LISTS)
    file(GLOB _dawn_extra_dlls "${CMAKE_BINARY_DIR}/_deps/dawn_prebuilt-src/bin/dx*.dll")
    if (_dawn_extra_dlls)
        add_custom_command(TARGET lmport POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_dawn_extra_dlls} $<TARGET_FILE_DIR:lmport>)
    endif ()
endif ()

# ---- Recompiled build (docs/RECOMPILER.md, docs/SETUP.md) ---------------------
# The recompiler (src/recompiler) translates the user's own main.dol into C++. It is
# linked into the shipped executable (first-run setup) and into the lmrecompile tool the
# dev build uses. Nothing generated is ever committed.
add_library(lm_recompiler STATIC src/recompiler/recompiler.cpp src/recompiler/recompiler.h)
target_include_directories(lm_recompiler PUBLIC src)
add_executable(lmrecompile tools/recomp/lmrecompile.cpp)
target_link_libraries(lmrecompile PRIVATE lm_recompiler)

# Runtime shared by both executables: memory/CPU helpers, scheduler, HLE SDK, frame
# driver, audio, menu. module_rt.cpp belongs to the game module, not the runtime.
file(GLOB RECOMP_RUNTIME_SOURCES CONFIGURE_DEPENDS src/recomp/*.cpp)
list(REMOVE_ITEM RECOMP_RUNTIME_SOURCES ${CMAKE_SOURCE_DIR}/src/recomp/module_rt.cpp)
list(APPEND RECOMP_RUNTIME_SOURCES src/port/crash_handler.cpp src/port/options.cpp src/port/dol.cpp
    src/port/settings.cpp src/port/install.cpp src/port/update.cpp src/port/recovery.cpp src/port/crash_report.cpp)
# The program's version (F1 > Game, the updater's comparison with the latest GitHub release, the
# game module's cache key; docs/SETUP.md "Updates"). Only this file sees it, so a version bump
# rebuilds one file.
set_source_files_properties(src/port/update.cpp PROPERTIES COMPILE_DEFINITIONS "LM_VERSION=\"${PROJECT_VERSION}\"")

# The commit this build comes from, for crash reports (src/port/crash_report.cpp).
execute_process(COMMAND git rev-parse --short=10 HEAD WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE LM_GIT_SHA OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
set(LM_STEAM_SDK_DIR "" CACHE PATH "Steamworks SDK 'sdk' folder: builds the optional Steam transport (Windows)")
function(lm_runtime_target tgt)
    target_include_directories(${tgt} PRIVATE src
        ${CMAKE_SOURCE_DIR}/extern/aurora/include/dolphin ${CMAKE_SOURCE_DIR}/extern/aurora/include)
    target_compile_definitions(${tgt} PRIVATE TARGET_PC=1)
    if (LM_GIT_SHA)
        target_compile_definitions(${tgt} PRIVATE "LM_GIT_SHA=\"${LM_GIT_SHA}\"")
    endif ()
    target_link_libraries(${tgt} PRIVATE aurora::main aurora::core aurora::gx aurora::vi aurora::pad
        aurora::si aurora::os aurora::dvd aurora::card
        LibDataChannel::LibDataChannelStatic nlohmann_json::nlohmann_json)
    if (LM_STEAM_SDK_DIR AND WIN32)  # optional Steam transport (src/recomp/online_steam.cpp, docs/ONLINE_STEAM.md)
        target_include_directories(${tgt} PRIVATE ${LM_STEAM_SDK_DIR}/public)
        target_compile_definitions(${tgt} PRIVATE LM_STEAM=1)
        target_link_libraries(${tgt} PRIVATE ${LM_STEAM_SDK_DIR}/redistributable_bin/win64/steam_api64.lib)
        add_custom_command(TARGET ${tgt} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${LM_STEAM_SDK_DIR}/redistributable_bin/win64/steam_api64.dll $<TARGET_FILE_DIR:${tgt}>)
    endif ()
    # Shipped shader/pipeline precache list (tools/pipelines/sweep.py; docs/RENDERING.md).
    add_custom_command(TARGET ${tgt} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/data/pipelines.bin $<TARGET_FILE_DIR:${tgt}>)
    if (WIN32)
        target_link_libraries(${tgt} PRIVATE dbghelp winhttp bcrypt)  # winhttp/bcrypt: updater
        # longPathAware (docs/SETUP.md "Long paths"): merged into the default manifest by the linker step.
        target_sources(${tgt} PRIVATE ${CMAKE_SOURCE_DIR}/packaging/windows/longpath.manifest)
        add_custom_command(TARGET ${tgt} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:${tgt}> $<TARGET_FILE_DIR:${tgt}>
            COMMAND_EXPAND_LISTS)
        if (_dawn_extra_dlls)
            add_custom_command(TARGET ${tgt} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_dawn_extra_dlls} $<TARGET_FILE_DIR:${tgt}>)
        endif ()
    elseif (APPLE)
        # macOS (docs/MACOS.md): same reason as the game module's -ffp-contract=off
        # (src/setup/module_builder.cpp): no fused multiply-adds the PowerPC would not do, in
        # the HLE math (PSMTX/PSVEC) and in lmrecomp's generated code.
        target_compile_options(${tgt} PRIVATE -ffp-contract=off)
    elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux")
        # Portable Linux binary (docs/LINUX.md): SDL3, Dawn and nod are linked statically by
        # Aurora; the C++ runtime is too, and src/port/glibc_compat.c covers the few symbols
        # a newer build host's glibc would otherwise require, so the executable only needs
        # glibc 2.34+ (libc, libm, libz). SDL3 and Dawn dlopen X11/Wayland, PulseAudio/
        # PipeWire/ALSA and the Vulkan loader at run time.
        target_sources(${tgt} PRIVATE ${CMAKE_SOURCE_DIR}/src/port/glibc_compat.c)
        # libgcc_s stays shared: the static unwinder of a newer GCC needs _dl_find_object (glibc 2.35).
        target_link_options(${tgt} PRIVATE -static-libstdc++)
        target_link_libraries(${tgt} PRIVATE ${CMAKE_DL_LIBS})
    endif ()
endfunction()

# LuigisMansionRecomp: what end users run. It contains the runtime, the recompiler and
# the setup UI; the game itself is a module DLL that setup compiles from the user's disc
# with the bundled clang + lld (docs/SETUP.md).
file(GLOB SETUP_SOURCES CONFIGURE_DEPENDS src/setup/*.cpp src/setup/*.h)
add_executable(LuigisMansionRecomp ${RECOMP_RUNTIME_SOURCES} ${SETUP_SOURCES})
if (WIN32)
    set_target_properties(LuigisMansionRecomp PROPERTIES OUTPUT_NAME LmCoop)  # the file players run: LmCoop.exe (Linux/macOS keep the old name)
endif ()
lm_runtime_target(LuigisMansionRecomp)
target_link_libraries(LuigisMansionRecomp PRIVATE lm_recompiler)  # nod comes with aurora::dvd
if (WIN32)
    # A windowed program: no console window opens with the game (output: src/port/recovery.h
    # setupConsole). main() stays the entry point. lmrecomp (dev build) keeps its console.
    set_target_properties(LuigisMansionRecomp PROPERTIES WIN32_EXECUTABLE TRUE)
    target_sources(LuigisMansionRecomp PRIVATE packaging/windows/lmcoop.rc)
    target_link_options(LuigisMansionRecomp PRIVATE "LINKER:/ENTRY:mainCRTStartup")
endif ()
# Setup's inputs are embedded in the executable (src/setup/embedded.cpp): symbol maps,
# hooks, and the game module's freestanding sources.
add_executable(lmembed tools/recomp/lmembed.cpp)
set(LM_EMBEDDED_FILES
    kSymbolsUs=decomp/config/GLME01/symbols.txt kSplitsUs=decomp/config/GLME01/splits.txt
    kSymbolsPal=decomp/config/GLMP01_00/symbols.txt kVersionMapPal=tools/recomp/GLMP01_map.txt
    kHooks=tools/recomp/hooks.txt kCtxHeader=src/recomp/ctx.h kModuleAbiHeader=src/recomp/module_abi.h
    kModuleRt=src/recomp/module_rt.cpp kRecompilerSource=src/recompiler/recompiler.cpp
    kLogo=assets/lm_coop_logo.png kJoinLoading=assets/join-loading.png)
set(_embed_deps)
foreach (_e ${LM_EMBEDDED_FILES})
    string(REGEX REPLACE "^[^=]*=" "${CMAKE_SOURCE_DIR}/" _f ${_e})
    list(APPEND _embed_deps ${_f})
endforeach ()
add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/embedded_data.cpp
    COMMAND lmembed ${CMAKE_BINARY_DIR}/embedded_data.cpp setup::embedded::data ${LM_EMBEDDED_FILES}
    DEPENDS lmembed ${_embed_deps}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Embedding setup data"
    VERBATIM)
target_sources(LuigisMansionRecomp PRIVATE ${CMAKE_BINARY_DIR}/embedded_data.cpp)
target_link_libraries(LuigisMansionRecomp PRIVATE PNG::PNG)  # the LmCoop logo (src/setup/logo.cpp)
# Dev builds use the LLVM that built them when no bundled llvm/ is next to the exe.
# The module linker is lld-link on Windows, ld.lld on Linux (src/setup/module_builder.cpp).
if (WIN32)
    set(LM_MODULE_LINKER lld-link)
elseif (APPLE)
    set(LM_MODULE_LINKER ld64.lld)
else ()
    set(LM_MODULE_LINKER ld.lld)
endif ()
get_filename_component(_cxx_dir ${CMAKE_CXX_COMPILER} DIRECTORY)
if (EXISTS ${_cxx_dir}/clang${CMAKE_EXECUTABLE_SUFFIX} AND EXISTS ${_cxx_dir}/${LM_MODULE_LINKER}${CMAKE_EXECUTABLE_SUFFIX})
    target_compile_definitions(LuigisMansionRecomp PRIVATE "LM_DEV_LLVM_DIR=\"${_cxx_dir}\"")
elseif (APPLE)
    # macOS: Apple's clang comes without lld. Homebrew's llvm and lld (separate kegs) build the
    # module in dev builds; findToolchain takes clang and ld64.lld from different folders here.
    find_program(LM_DEV_CLANG clang HINTS /opt/homebrew/opt/llvm/bin /usr/local/opt/llvm/bin NO_DEFAULT_PATH)
    find_program(LM_DEV_LD64_LLD ld64.lld HINTS /opt/homebrew/opt/lld/bin /opt/homebrew/opt/llvm/bin
        /usr/local/opt/lld/bin /usr/local/opt/llvm/bin)
    if (LM_DEV_CLANG AND LM_DEV_LD64_LLD)
        get_filename_component(_dev_clang_dir ${LM_DEV_CLANG} DIRECTORY)
        get_filename_component(_dev_lld_dir ${LM_DEV_LD64_LLD} DIRECTORY)
        target_compile_definitions(LuigisMansionRecomp PRIVATE "LM_DEV_LLVM_DIR=\"${_dev_clang_dir}\""
            "LM_DEV_LLD_DIR=\"${_dev_lld_dir}\"")
        set(_cxx_dir ${_dev_clang_dir})
    else ()
        message(WARNING "No clang + ld64.lld found (brew install llvm lld): the game module cannot be built")
    endif ()
endif ()

# Packaging: `cmake --install build/<preset> --prefix <dir>` lays out a release folder.
# LM_LLVM_DIR (an LLVM bin directory) supplies the bundled clang and lld-link / ld.lld.
set(LM_LLVM_DIR "${_cxx_dir}" CACHE PATH "LLVM bin directory whose clang and lld-link (Windows) / ld.lld (Linux) are bundled on install")
install(TARGETS LuigisMansionRecomp RUNTIME DESTINATION .)
# shortcut: a copy under the old name, only so installs older than 1.3.2 (whose updater insists on finding
# it in the package) can still update; drop it once those are gone.
if (WIN32)
    install(PROGRAMS $<TARGET_FILE:LuigisMansionRecomp> DESTINATION . RENAME LuigisMansionRecomp.exe)
endif ()
install(FILES NOTICE.md DESTINATION .)
if (LM_STEAM_SDK_DIR AND WIN32)
    install(FILES ${LM_STEAM_SDK_DIR}/redistributable_bin/win64/steam_api64.dll DESTINATION .)
endif ()
install(DIRECTORY packaging/licenses/ DESTINATION licenses)
install(FILES data/pipelines.bin DESTINATION .)
install(FILES packaging/LLVM-LICENSE.TXT DESTINATION llvm)
if (WIN32)
    install(FILES packaging/windows/README.txt DESTINATION .)
    install(FILES packaging/windows/LmCoop-Monitor.cmd packaging/windows/LmCoop-Monitor.ps1 DESTINATION .)
    install(CODE "
        file(GLOB _dlls \"$<TARGET_FILE_DIR:LuigisMansionRecomp>/*.dll\")
        file(INSTALL \${_dlls} DESTINATION \"\${CMAKE_INSTALL_PREFIX}\")")
    install(PROGRAMS ${LM_LLVM_DIR}/clang${CMAKE_EXECUTABLE_SUFFIX} ${LM_LLVM_DIR}/lld-link${CMAKE_EXECUTABLE_SUFFIX}
        DESTINATION llvm/bin OPTIONAL)
else ()
    # Linux (docs/LINUX.md): clang and ld.lld are usually symlinks (clang -> clang-21,
    # ld.lld -> lld); copy the real binaries under the names the setup looks for (lld picks
    # its flavour from its name). Use a self-contained LLVM (the official release tarball),
    # not a distribution's clang that needs libLLVM/libclang-cpp next to it.
    if (APPLE)
        set(_lm_linker_name ld64.lld)  # macOS: Mach-O lld (docs/MACOS.md)
    else ()
        set(_lm_linker_name ld.lld)
    endif ()
    install(CODE "
        foreach (_pair \"clang;clang\" \"${_lm_linker_name};${_lm_linker_name}\")
            list(GET _pair 0 _src)
            list(GET _pair 1 _dst)
            set(_from \"${LM_LLVM_DIR}/\${_src}\")
            if (NOT EXISTS \"\${_from}\")
                message(WARNING \"LM_LLVM_DIR has no \${_src}; the package will lack the bundled compiler\")
                continue()
            endif ()
            file(REAL_PATH \"\${_from}\" _real)
            file(MAKE_DIRECTORY \"\${CMAKE_INSTALL_PREFIX}/llvm/bin\")
            file(COPY_FILE \"\${_real}\" \"\${CMAKE_INSTALL_PREFIX}/llvm/bin/\${_dst}\")
            file(CHMOD \"\${CMAKE_INSTALL_PREFIX}/llvm/bin/\${_dst}\" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
                 GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
            message(STATUS \"Bundled \${_real} as llvm/bin/\${_dst}\")
        endforeach ()")
endif ()
if (APPLE)
    install(FILES packaging/macos/README.txt DESTINATION .)
endif ()
if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # The official LLVM lld needs libxml2.so.2 (only for lld-link manifests): ship a stub
    # in llvm/lib, found through lld's RUNPATH $ORIGIN/../lib (packaging/linux/libxml2_stub.c).
    add_library(lm_libxml2_stub SHARED packaging/linux/libxml2_stub.c)
    set_target_properties(lm_libxml2_stub PROPERTIES OUTPUT_NAME xml2 SOVERSION 2
        LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/llvm_stub)
    target_link_options(lm_libxml2_stub PRIVATE
        -Wl,--version-script=${CMAKE_SOURCE_DIR}/packaging/linux/libxml2_stub.map)
    add_dependencies(LuigisMansionRecomp lm_libxml2_stub)
    install(FILES $<TARGET_FILE:lm_libxml2_stub> DESTINATION llvm/lib)
    install(PROGRAMS packaging/linux/LuigisMansionRecompiled.sh DESTINATION .)
    install(FILES packaging/linux/README.txt DESTINATION .)
endif ()

# lmrecomp: developer build with the game recompiled at build time and linked in
# statically (debuggable, no setup). LM_DOL is the user's own main.dol; LM_DOL_VERSION
# selects the symbol map (GLME01, or GLMP01 for the European release).
set(LM_DOL "${CMAKE_SOURCE_DIR}/Luigis Mansion US/sys/main.dol" CACHE FILEPATH
    "Path to your Luigi's Mansion main.dol, extracted from your own disc")
set(LM_DOL_VERSION "GLME01" CACHE STRING "Release of LM_DOL: GLME01 (USA) or GLMP01 (Europe)")
if (EXISTS ${LM_DOL})
    set(LM_RECOMP_DIR ${CMAKE_BINARY_DIR}/recomp_gen)
    if (LM_DOL_VERSION STREQUAL "GLMP01")
        set(_recomp_cfg --symbols ${CMAKE_SOURCE_DIR}/decomp/config/GLMP01_00/symbols.txt
            --version-map ${CMAKE_SOURCE_DIR}/tools/recomp/GLMP01_map.txt)
        set(_recomp_deps ${CMAKE_SOURCE_DIR}/decomp/config/GLMP01_00/symbols.txt
            ${CMAKE_SOURCE_DIR}/tools/recomp/GLMP01_map.txt)
    else ()
        set(_recomp_cfg --symbols ${CMAKE_SOURCE_DIR}/decomp/config/GLME01/symbols.txt
            --splits ${CMAKE_SOURCE_DIR}/decomp/config/GLME01/splits.txt)
        set(_recomp_deps ${CMAKE_SOURCE_DIR}/decomp/config/GLME01/symbols.txt
            ${CMAKE_SOURCE_DIR}/decomp/config/GLME01/splits.txt)
    endif ()
    # Fixed set of outputs (lmrc::kChunks = 32 in src/recompiler/recompiler.h).
    set(_recomp_files ${LM_RECOMP_DIR}/func_table.cpp)
    foreach (_i RANGE 31)
        string(LENGTH "${_i}" _len)
        if (_len EQUAL 1)
            set(_i "00${_i}")
        else ()
            set(_i "0${_i}")
        endif ()
        list(APPEND _recomp_files ${LM_RECOMP_DIR}/funcs_${_i}.cpp)
    endforeach ()
    add_custom_command(
        OUTPUT ${_recomp_files} ${LM_RECOMP_DIR}/funcs.h ${LM_RECOMP_DIR}/summary.txt
        COMMAND lmrecompile --dol ${LM_DOL} --version ${LM_DOL_VERSION} ${_recomp_cfg}
            --hooks ${CMAKE_SOURCE_DIR}/tools/recomp/hooks.txt --out ${LM_RECOMP_DIR}
        DEPENDS lmrecompile ${LM_DOL} ${_recomp_deps} ${CMAKE_SOURCE_DIR}/tools/recomp/hooks.txt
        COMMENT "Recompiling ${LM_DOL}"
        VERBATIM)

    add_executable(lmrecomp ${RECOMP_RUNTIME_SOURCES} ${_recomp_files})
    lm_runtime_target(lmrecomp)
    target_include_directories(lmrecomp PRIVATE ${LM_RECOMP_DIR})
    target_compile_definitions(lmrecomp PRIVATE LM_STATIC_MODULE=1)
    if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        execute_process(COMMAND ${CMAKE_CXX_COMPILER} --print-resource-dir
            OUTPUT_VARIABLE _clang_resource OUTPUT_STRIP_TRAILING_WHITESPACE)
        # The crash-test script points this at a newer runtime; VS clang 19 ASan cannot hook a ucrt prologue.
        set(LM_ASAN_RUNTIME_DIR "${_clang_resource}/lib/windows" CACHE PATH "clang_rt ASan/fuzzer library directory")
        function(lm_address_sanitizer tgt)
            target_compile_definitions(${tgt} PRIVATE LM_ASAN=1)
            if (WIN32)
                target_compile_options(${tgt} PRIVATE /fsanitize=address /Oy-)
                # This toolchain has Clang ASan but no MSVC STL annotation library.
                target_compile_definitions(${tgt} PRIVATE _DISABLE_VECTOR_ANNOTATION _DISABLE_STRING_ANNOTATION)
                target_link_directories(${tgt} PRIVATE "${LM_ASAN_RUNTIME_DIR}")
                target_link_options(${tgt} PRIVATE /INCREMENTAL:NO)
                target_link_libraries(${tgt} PRIVATE clang_rt.asan_dynamic-x86_64.lib clang_rt.asan_dynamic_runtime_thunk-x86_64.lib)
                target_link_options(${tgt} PRIVATE /wholearchive:clang_rt.asan_dynamic_runtime_thunk-x86_64.lib)
                file(GLOB _asan_dll "${LM_ASAN_RUNTIME_DIR}/*asan_dynamic-x86_64.dll")
                add_custom_command(TARGET ${tgt} POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    ${_asan_dll} $<TARGET_FILE_DIR:${tgt}> COMMAND_EXPAND_LISTS)
            else ()
                target_compile_options(${tgt} PRIVATE -fsanitize=address -fno-omit-frame-pointer)
                target_link_options(${tgt} PRIVATE -fsanitize=address)
            endif ()
        endfunction()
        add_executable(lmrecomp_asan EXCLUDE_FROM_ALL ${RECOMP_RUNTIME_SOURCES} ${_recomp_files})
        set_target_properties(lmrecomp_asan PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/crash-asan")
        lm_runtime_target(lmrecomp_asan)
        target_include_directories(lmrecomp_asan PRIVATE ${LM_RECOMP_DIR})
        target_compile_definitions(lmrecomp_asan PRIVATE LM_STATIC_MODULE=1)
        lm_address_sanitizer(lmrecomp_asan)

        add_executable(lm_recompiler_fuzz EXCLUDE_FROM_ALL
            src/recompiler/recompiler.cpp tools/recomp/recompiler_fuzz.cpp)
        set_target_properties(lm_recompiler_fuzz PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/crash-fuzz")
        target_include_directories(lm_recompiler_fuzz PRIVATE src)
        lm_address_sanitizer(lm_recompiler_fuzz)
        if (WIN32)
            set_property(TARGET lm_recompiler_fuzz PROPERTY MSVC_RUNTIME_LIBRARY MultiThreaded)
            target_compile_options(lm_recompiler_fuzz PRIVATE /clang:-fsanitize=fuzzer)
            target_link_libraries(lm_recompiler_fuzz PRIVATE clang_rt.fuzzer-x86_64.lib)
        else ()
            target_compile_options(lm_recompiler_fuzz PRIVATE -fsanitize=fuzzer)
            target_link_options(lm_recompiler_fuzz PRIVATE -fsanitize=fuzzer)
        endif ()
    endif ()
    # Generated code: one huge function per guest function, lots of gotos.
    if (CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        set_source_files_properties(${_recomp_files} PROPERTIES COMPILE_OPTIONS "/clang:-Wno-unused-label;/clang:-Wno-unused-variable")
    else ()
        set_source_files_properties(${_recomp_files} PROPERTIES COMPILE_OPTIONS "-Wno-unused-label;-Wno-unused-variable")
    endif ()
else ()
    message(STATUS "LM_DOL not found (${LM_DOL}); skipping the lmrecomp developer build")
endif ()

# Transport-level end-to-end test of the internet path (server/test/e2e.test.js). Not built by default:
#   powershell -File tools/build.ps1 -Target lm_rtc_test
add_executable(lm_rtc_test EXCLUDE_FROM_ALL tools/recomp/online_rtc_test.cpp src/recomp/online_rtc.cpp src/recomp/online_udp.cpp)
target_include_directories(lm_rtc_test PRIVATE src)
target_link_libraries(lm_rtc_test PRIVATE LibDataChannel::LibDataChannelStatic nlohmann_json::nlohmann_json)
if (WIN32)
    target_link_libraries(lm_rtc_test PRIVATE ws2_32 crypt32)
endif ()
