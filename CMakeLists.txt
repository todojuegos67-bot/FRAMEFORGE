cmake_minimum_required(VERSION 3.24)
project(FrameForge VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(NOT WIN32)
  message(FATAL_ERROR "FrameForge only builds on Windows (MSVC).")
endif()

add_compile_options(/utf-8 /W3 /permissive- /EHsc)
add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)

include(FetchContent)
FetchContent_Declare(imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG v1.91.9
  GIT_SHALLOW TRUE
  SOURCE_SUBDIR none)
FetchContent_MakeAvailable(imgui)

# --- AMD FidelityFX FSR 1 headers (MIT) ------------------------------------
set(FFX_DIR ${CMAKE_BINARY_DIR}/ffx)
foreach(f ffx_a.h ffx_fsr1.h)
  if(NOT EXISTS ${FFX_DIR}/${f})
    file(DOWNLOAD https://raw.githubusercontent.com/GPUOpen-Effects/FidelityFX-FSR/v1.0.2/ffx-fsr/${f}
         ${FFX_DIR}/${f} STATUS st TLS_VERIFY ON)
    list(GET st 0 code)
    if(NOT code EQUAL 0)
      file(REMOVE ${FFX_DIR}/${f})
      message(FATAL_ERROR "Download of ${f} failed: ${st}")
    endif()
  endif()
endforeach()

# --- Embed shaders (and FFX headers) into the binary ------------------------
set(FF_SHADERS
  ${CMAKE_SOURCE_DIR}/shaders/common.hlsli
  ${CMAKE_SOURCE_DIR}/shaders/luma.hlsl
  ${CMAKE_SOURCE_DIR}/shaders/match.hlsl
  ${CMAKE_SOURCE_DIR}/shaders/median.hlsl
  ${CMAKE_SOURCE_DIR}/shaders/consist.hlsl
  ${CMAKE_SOURCE_DIR}/shaders/hud.hlsl
  ${CMAKE_SOURCE_DIR}/shaders/interp.hlsl
  ${CMAKE_SOURCE_DIR}/shaders/blit.hlsl
  ${CMAKE_SOURCE_DIR}/shaders/bilinear.hlsl
  ${CMAKE_SOURCE_DIR}/shaders/fsr_easu.hlsl
  ${CMAKE_SOURCE_DIR}/shaders/fsr_rcas.hlsl
  ${FFX_DIR}/ffx_a.h
  ${FFX_DIR}/ffx_fsr1.h)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${FF_SHADERS})
set(FF_EMBED ${CMAKE_BINARY_DIR}/generated/embedded.cpp)
set(content "#include \"core/embedded.h\"\nnamespace ff::embedded {\n")
set(table "")
set(idx 0)
foreach(path IN LISTS FF_SHADERS)
  get_filename_component(name ${path} NAME)
  file(READ ${path} hex HEX)
  string(LENGTH "${hex}" hl)
  math(EXPR n "${hl} / 2")
  string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," arr "${hex}")
  string(APPEND content "static const unsigned char d${idx}[] = {${arr}0x00};\n")
  string(APPEND table "  {\"${name}\", d${idx}, ${n}},\n")
  math(EXPR idx "${idx} + 1")
endforeach()
string(APPEND content "const File files[] = {\n${table}};\nconst size_t fileCount = ${idx};\n}\n")
file(WRITE ${FF_EMBED}.tmp "${content}")
file(COPY_FILE ${FF_EMBED}.tmp ${FF_EMBED} ONLY_IF_DIFFERENT)

# --- Core library (GPU pipeline, pacing, config) ----------------------------
add_library(ffcore STATIC
  src/core/gpu.cpp
  src/core/log.cpp
  src/core/config.cpp
  src/core/framegen.cpp
  src/core/upscale.cpp
  src/core/pacer.cpp
  ${FF_EMBED})
target_include_directories(ffcore PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(ffcore PUBLIC d3d11 dxgi d3dcompiler)

# --- Application ------------------------------------------------------------
add_executable(FrameForge WIN32
  src/app/main.cpp
  src/app/capture.cpp
  src/app/overlay.cpp
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp)
target_include_directories(FrameForge PRIVATE ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(FrameForge PRIVATE ffcore windowsapp dwmapi winmm)

# --- Headless tests (WARP) --------------------------------------------------
add_executable(frameforge_tests tests/main.cpp)
target_link_libraries(frameforge_tests PRIVATE ffcore)
enable_testing()
add_test(NAME headless COMMAND frameforge_tests)
