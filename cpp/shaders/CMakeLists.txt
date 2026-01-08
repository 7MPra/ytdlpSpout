# =============================================================================
# シェーダーコンパイル設定
# =============================================================================
#
# HLSLシェーダーをコンパイルし、Cヘッダーファイル（バイトコード配列）として出力
#

# Windows SDKからfxc.exeを検索
find_program(FXC fxc.exe
  HINTS
    "$ENV{WindowsSdkDir}/bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64"
    "$ENV{WindowsSdkDir}/bin/x64"
    "C:/Program Files (x86)/Windows Kits/10/bin/10.0.22621.0/x64"
    "C:/Program Files (x86)/Windows Kits/10/bin/10.0.22000.0/x64"
    "C:/Program Files (x86)/Windows Kits/10/bin/10.0.19041.0/x64"
)

if(NOT FXC)
  message(WARNING "fxc.exe not found. Shader compilation will be skipped. Please install Windows SDK.")
  # フォールバック: プリコンパイル済みシェーダーを使用
  set(SHADER_COMPILATION_AVAILABLE FALSE)
else()
  message(STATUS "Found fxc.exe: ${FXC}")
  set(SHADER_COMPILATION_AVAILABLE TRUE)
endif()

set(SHADER_OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR})

if(SHADER_COMPILATION_AVAILABLE)
  # ==========================================================================
  # 頂点シェーダー: Passthrough_VS.hlsl
  # ==========================================================================
  add_custom_command(
    OUTPUT ${SHADER_OUTPUT_DIR}/Passthrough_VS.h
    COMMAND ${FXC}
      /T vs_5_0
      /E main
      /Fh ${SHADER_OUTPUT_DIR}/Passthrough_VS.h
      /Vn g_Passthrough_VS
      /nologo
      $<$<CONFIG:Debug>:/Zi>
      $<$<CONFIG:Debug>:/Od>
      $<$<CONFIG:Release>:/O3>
      ${CMAKE_CURRENT_SOURCE_DIR}/Passthrough_VS.hlsl
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/Passthrough_VS.hlsl
    COMMENT "Compiling Passthrough_VS.hlsl -> Passthrough_VS.h"
    VERBATIM
  )

  # ==========================================================================
  # ピクセルシェーダー: NV12ToRGBA_PS.hlsl
  # ==========================================================================
  add_custom_command(
    OUTPUT ${SHADER_OUTPUT_DIR}/NV12ToRGBA_PS.h
    COMMAND ${FXC}
      /T ps_5_0
      /E main
      /Fh ${SHADER_OUTPUT_DIR}/NV12ToRGBA_PS.h
      /Vn g_NV12ToRGBA_PS
      /nologo
      $<$<CONFIG:Debug>:/Zi>
      $<$<CONFIG:Debug>:/Od>
      $<$<CONFIG:Release>:/O3>
      ${CMAKE_CURRENT_SOURCE_DIR}/NV12ToRGBA_PS.hlsl
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/NV12ToRGBA_PS.hlsl
    COMMENT "Compiling NV12ToRGBA_PS.hlsl -> NV12ToRGBA_PS.h"
    VERBATIM
  )

  # ==========================================================================
  # シェーダーターゲット
  # ==========================================================================
  add_custom_target(Shaders ALL
    DEPENDS
      ${SHADER_OUTPUT_DIR}/Passthrough_VS.h
      ${SHADER_OUTPUT_DIR}/NV12ToRGBA_PS.h
    COMMENT "Building HLSL shaders"
  )
else()
  # シェーダーコンパイルが利用できない場合のダミーターゲット
  add_custom_target(Shaders ALL
    COMMENT "Shader compilation skipped (fxc.exe not found)"
  )
endif()

# シェーダー出力ディレクトリをエクスポート
set(SHADER_INCLUDE_DIR ${SHADER_OUTPUT_DIR} PARENT_SCOPE)
