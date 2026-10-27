cmake_minimum_required(VERSION 3.24)
project(FrameForge VERSION 0.2.0 LANGUAGES CXX)

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

# --- AMD FidelityFX SDK (FSR 3.1 upscaler, D3D12) -----------------------------
# API headers are MIT; signed DLLs are under AMD's binary redistribution licence (docs/license.md).
set(FFXSDK_TAG v2.3.0)
set(FFXSDK_DIR ${CMAKE_BINARY_DIR}/ffxsdk)
set(FFXSDK_FILES
  Kits/FidelityFX/api/include/ffx_api.h
  Kits/FidelityFX/api/include/ffx_api_types.h
  Kits/FidelityFX/api/include/ffx_api_loader.h
  Kits/FidelityFX/api/include/dx12/ffx_api_dx12.h
  Kits/FidelityFX/upscalers/include/ffx_upscale.h
  Kits/FidelityFX/docs/license.md
  Kits/FidelityFX/signedbin/amd_fidelityfx_loader_dx12.dll
  Kits/FidelityFX/signedbin/amd_fidelityfx_upscaler_dx12.dll)
# --- Intel XeSS SR (D3D11 runtime; Intel Simplified Software License permits binary redistribution) ---
set(XESS_COMMIT de0fb9c1c510661c571164e1418ceca8101dab69)
set(XESS_DIR ${CMAKE_BINARY_DIR}/xess)
set(XESS_FILES inc/xess/xess.h inc/xess/xess_d3d11.h LICENSE.txt bin/libxess_dx11.dll)

function(ff_fetch base url_prefix files)
  foreach(f IN LISTS files)
    if(NOT EXISTS ${base}/${f})
      file(DOWNLOAD ${url_prefix}/${f} ${base}/${f} STATUS st TLS_VERIFY ON)
      list(GET st 0 code)
      if(NOT code EQUAL 0)
        file(REMOVE ${base}/${f})
        message(FATAL_ERROR "Download of ${f} failed: ${st}")
      endif()
    endif()
  endforeach()
endfunction()
ff_fetch(${FFXSDK_DIR} https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/${FFXSDK_TAG} "${FFXSDK_FILES}")
ff_fetch(${XESS_DIR} https://raw.githubusercontent.com/intel/xess/${XESS_COMMIT} "${XESS_FILES}")

# --- ONNX Runtime + DirectML (NuGet, MIT / Microsoft redistributable) -----
set(ORT_VER 1.20.1)
set(DML_VER 1.15.4)
set(ORT_DIR ${CMAKE_BINARY_DIR}/ort)
set(DML_DIR ${CMAKE_BINARY_DIR}/dml)
foreach(pkg ORT DML)
  if(pkg STREQUAL ORT)
    set(url https://www.nuget.org/api/v2/package/Microsoft.ML.OnnxRuntime.DirectML/${ORT_VER})
    set(dir ${ORT_DIR})
  else()
    set(url https://www.nuget.org/api/v2/package/Microsoft.AI.DirectML/${DML_VER})
    set(dir ${DML_DIR})
  endif()
  if(NOT EXISTS ${dir}/.done)
    file(DOWNLOAD ${url} ${CMAKE_BINARY_DIR}/${pkg}.zip STATUS st TLS_VERIFY ON)
    list(GET st 0 code)
    if(NOT code EQUAL 0)
      message(FATAL_ERROR "Download of ${url} failed: ${st}")
    endif()
    file(ARCHIVE_EXTRACT INPUT ${CMAKE_BINARY_DIR}/${pkg}.zip DESTINATION ${dir})
    file(REMOVE ${CMAKE_BINARY_DIR}/${pkg}.zip)
    file(WRITE ${dir}/.done "")
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
  ${CMAKE_SOURCE_DIR}/shaders/mv.hlsl
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
  src/core/temporal_fsr3.cpp
  src/core/temporal_xess.cpp
  src/core/depth_ai.cpp
  ${FF_EMBED})
target_include_directories(ffcore PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_include_directories(ffcore PRIVATE ${FFXSDK_DIR}/Kits/FidelityFX ${XESS_DIR}/inc ${ORT_DIR}/build/native/include)
target_compile_definitions(ffcore PRIVATE ORT_API_MANUAL_INIT _WINDOWS)
target_link_libraries(ffcore PUBLIC d3d11 d3d12 dxgi d3dcompiler urlmon bcrypt ${ORT_DIR}/runtimes/win-x64/native/onnxruntime.lib delayimp)

# Runtime DLLs next to the executables (all loaded lazily; the app runs without them).
set(FF_RUNTIME_DLLS
  ${FFXSDK_DIR}/Kits/FidelityFX/signedbin/amd_fidelityfx_loader_dx12.dll
  ${FFXSDK_DIR}/Kits/FidelityFX/signedbin/amd_fidelityfx_upscaler_dx12.dll
  ${XESS_DIR}/bin/libxess_dx11.dll
  ${ORT_DIR}/runtimes/win-x64/native/onnxruntime.dll
  ${DML_DIR}/bin/x64-win/DirectML.dll)

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
target_link_libraries(FrameForge PRIVATE ffcore windowsapp dwmapi winmm d2d1 dwrite)
target_link_options(FrameForge PRIVATE /DELAYLOAD:onnxruntime.dll)
add_custom_command(TARGET FrameForge POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different ${FF_RUNTIME_DLLS} $<TARGET_FILE_DIR:FrameForge>)

# --- Headless tests (WARP) --------------------------------------------------
add_executable(frameforge_tests tests/main.cpp)
target_link_libraries(frameforge_tests PRIVATE ffcore)
target_link_options(frameforge_tests PRIVATE /DELAYLOAD:onnxruntime.dll)
enable_testing()
add_test(NAME headless COMMAND frameforge_tests)
add_custom_command(TARGET frameforge_tests POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different ${FF_RUNTIME_DLLS} $<TARGET_FILE_DIR:frameforge_tests>)
