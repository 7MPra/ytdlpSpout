# =============================================================================
# ytdlpSpout C++ Backend - ルートCMake設定
# =============================================================================
#
# ビルド手順:
#   1. vcpkgをセットアップ: $env:VCPKG_ROOT = "C:\vcpkg"
#   2. cmake --preset windows-x64-debug  (または windows-x64-release)
#   3. cmake --build build/windows-x64-debug
#
# =============================================================================

cmake_minimum_required(VERSION 3.25)

# vcpkg toolchainの設定（CMakePresets.jsonで指定推奨）
if(DEFINED ENV{VCPKG_ROOT} AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
      CACHE STRING "Vcpkg toolchain file")
endif()

project(ytdlpSpoutCpp
  VERSION 0.1.0
  LANGUAGES CXX
  DESCRIPTION "C++ backend for ytdlpSpout - Video streaming to Spout"
)

# =============================================================================
# C++標準設定
# =============================================================================
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 出力ディレクトリ設定
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# =============================================================================
# Windows固有設定
# =============================================================================
if(WIN32)
  add_compile_definitions(
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
    _CRT_SECURE_NO_WARNINGS
  )
  
  # MSVC固有の警告設定
  if(MSVC)
    add_compile_options(
      /W4           # 警告レベル4
      /WX-          # 警告をエラーとして扱わない（開発中）
      /MP           # マルチプロセスビルド
      /utf-8        # ソースファイルをUTF-8として扱う
    )
  endif()
endif()

# =============================================================================
# 依存パッケージの検索
# =============================================================================

# FFmpeg (PkgConfigまたはvcpkg)
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(LIBAV QUIET IMPORTED_TARGET
    libavcodec
    libavformat
    libavutil
    libswresample
    libswscale
  )
endif()

# PkgConfigで見つからない場合、find_packageを試行
if(NOT LIBAV_FOUND)
  # vcpkgでFFmpegを見つける（手動パス設定のフォールバック）
  find_path(FFMPEG_INCLUDE_DIR
    NAMES libavcodec/avcodec.h
    HINTS ${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/include
  )
  
  find_library(AVCODEC_LIBRARY avcodec
    HINTS ${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib
  )
  find_library(AVFORMAT_LIBRARY avformat
    HINTS ${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib
  )
  find_library(AVUTIL_LIBRARY avutil
    HINTS ${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib
  )
  find_library(SWRESAMPLE_LIBRARY swresample
    HINTS ${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib
  )
  find_library(SWSCALE_LIBRARY swscale
    HINTS ${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib
  )
  
  if(AVCODEC_LIBRARY AND AVFORMAT_LIBRARY AND AVUTIL_LIBRARY)
    set(FFMPEG_FOUND TRUE)
    set(FFMPEG_LIBRARIES
      ${AVCODEC_LIBRARY}
      ${AVFORMAT_LIBRARY}
      ${AVUTIL_LIBRARY}
      ${SWRESAMPLE_LIBRARY}
      ${SWSCALE_LIBRARY}
    )
    message(STATUS "FFmpeg found via manual search")
  else()
    message(FATAL_ERROR "FFmpeg not found. Please install via vcpkg: vcpkg install ffmpeg:x64-windows")
  endif()
endif()

# =============================================================================
# Spout機能の有効/無効オプション
# DLL用途ではSpoutを無効化（Python SpoutGLと競合するため）
# =============================================================================
option(YTDLPSPOUT_ENABLE_SPOUT "Enable Spout integration (disable for DLL to avoid SpoutGL conflict)" ON)

if(YTDLPSPOUT_ENABLE_SPOUT)
  # Spout2
  find_package(Spout2 CONFIG QUIET)
  if(NOT Spout2_FOUND)
    # フォールバック: 手動検索 (SpoutLibraryを探す)
    find_path(SPOUT_INCLUDE_DIR
      NAMES SpoutLibrary.h
      HINTS ${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/include
    )
    find_library(SPOUT_LIBRARY SpoutLibrary
      HINTS ${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib
    )
    if(SPOUT_LIBRARY)
      set(SPOUT_FOUND TRUE)
      message(STATUS "Spout2 (SpoutLibrary) found via manual search")
    else()
      message(FATAL_ERROR "Spout2 not found. Please install via vcpkg: vcpkg install spout2:x64-windows")
    endif()
  endif()
else()
  message(STATUS "Spout integration DISABLED (YTDLPSPOUT_ENABLE_SPOUT=OFF)")
  set(Spout2_FOUND FALSE)
  set(SPOUT_FOUND FALSE)
endif()

# spdlog
find_package(spdlog CONFIG REQUIRED)

# CURL
find_package(CURL CONFIG REQUIRED)

# CLI11 (シングルヘッダー版を使用 - vcpkgライブラリのABI互換性問題を回避)
set(CLI11_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/CLI11")

# nlohmann-json
find_package(nlohmann_json CONFIG REQUIRED)

# =============================================================================
# DirectX11（Windows SDK）
# =============================================================================
if(WIN32)
  set(D3D11_LIBRARY d3d11)
  set(DXGI_LIBRARY dxgi)
  set(D3DCOMPILER_LIBRARY d3dcompiler)
  set(WINMM_LIBRARY winmm)  # タイマー関数用
endif()

# =============================================================================
# シェーダーサブディレクトリ
# =============================================================================
add_subdirectory(shaders)

# =============================================================================
# ソースファイル収集
# =============================================================================
set(SOURCES
  # エントリーポイント
  src/main.cpp
  src/Application.cpp
  src/Application.h
  
  # ユーティリティ
  src/utils/Logger.cpp
  src/utils/Logger.h
  src/utils/ComPtr.h
  src/utils/ErrorHandling.h
  
  # グラフィックス
  src/graphics/D3D11Context.cpp
  src/graphics/D3D11Context.h
  src/graphics/SpoutSender.cpp
  src/graphics/SpoutSender.h
  src/graphics/TexturePool.cpp
  src/graphics/TexturePool.h
  
  # デコーダー
  src/decoder/VideoDecoder.cpp
  src/decoder/VideoDecoder.h
  src/decoder/HWAccelContext.cpp
  src/decoder/HWAccelContext.h
  src/decoder/FrameConverter.cpp
  src/decoder/FrameConverter.h
  
  # プレイヤー
  src/player/VideoPlayer.cpp
  src/player/VideoPlayer.h
  src/player/FrameTimer.cpp
  src/player/FrameTimer.h
  
  # I/O
  src/io/HttpClient.cpp
  src/io/HttpClient.h
  src/io/SparseFileCache.cpp
  src/io/SparseFileCache.h
  src/io/ChunkDownloader.cpp
  src/io/ChunkDownloader.h
  src/io/PrefetchScheduler.cpp
  src/io/PrefetchScheduler.h
  src/io/CustomIOContext.cpp
  src/io/CustomIOContext.h
  src/io/SliceLoadingManager.cpp
  src/io/SliceLoadingManager.h
  
  # yt-dlp連携
  src/ytdlp/YtDlpResolver.cpp
  src/ytdlp/YtDlpResolver.h
  
  # 音声処理
  src/audio/AudioDecoder.cpp
  src/audio/AudioDecoder.h
  src/audio/BeatMap.cpp
  src/audio/BeatMap.h
  src/audio/BeatDetector.cpp
  src/audio/BeatDetector.h
  src/audio/BeatMapGenerator.cpp
  src/audio/BeatMapGenerator.h
)

# =============================================================================
# ライブラリ用ソースファイル（main.cpp, Application.cpp除外、C API追加）
# =============================================================================
set(LIB_SOURCES
  # ユーティリティ
  src/utils/Logger.cpp
  src/utils/Logger.h
  src/utils/ComPtr.h
  src/utils/ErrorHandling.h
  
  # グラフィックス
  src/graphics/D3D11Context.cpp
  src/graphics/D3D11Context.h
  src/graphics/TexturePool.cpp
  src/graphics/TexturePool.h
  
  # デコーダー
  src/decoder/VideoDecoder.cpp
  src/decoder/VideoDecoder.h
  src/decoder/HWAccelContext.cpp
  src/decoder/HWAccelContext.h
  src/decoder/FrameConverter.cpp
  src/decoder/FrameConverter.h
  
  # プレイヤー
  src/player/VideoPlayer.cpp
  src/player/VideoPlayer.h
  src/player/FrameTimer.cpp
  src/player/FrameTimer.h
  
  # I/O
  src/io/HttpClient.cpp
  src/io/HttpClient.h
  src/io/SparseFileCache.cpp
  src/io/SparseFileCache.h
  src/io/ChunkDownloader.cpp
  src/io/ChunkDownloader.h
  src/io/PrefetchScheduler.cpp
  src/io/PrefetchScheduler.h
  src/io/CustomIOContext.cpp
  src/io/CustomIOContext.h
  src/io/SliceLoadingManager.cpp
  src/io/SliceLoadingManager.h
  
  # yt-dlp連携
  src/ytdlp/YtDlpResolver.cpp
  src/ytdlp/YtDlpResolver.h
  
  # 音声処理
  src/audio/AudioDecoder.cpp
  src/audio/AudioDecoder.h
  src/audio/BeatMap.cpp
  src/audio/BeatMap.h
  src/audio/BeatDetector.cpp
  src/audio/BeatDetector.h
  src/audio/BeatMapGenerator.cpp
  src/audio/BeatMapGenerator.h
  
  # C APIバインディング
  src/bindings/c_api.cpp
)

# Spout有効時のみSpoutSenderを追加
if(YTDLPSPOUT_ENABLE_SPOUT)
  list(APPEND LIB_SOURCES
    src/graphics/SpoutSender.cpp
    src/graphics/SpoutSender.h
  )
endif()

# =============================================================================
# 実行可能ターゲット
# =============================================================================
add_executable(ytdlpSpoutCLI ${SOURCES})

# シェーダーへの依存
add_dependencies(ytdlpSpoutCLI Shaders)

target_include_directories(ytdlpSpoutCLI PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_BINARY_DIR}/shaders  # コンパイル済みシェーダー
)

# FFmpegのインクルードディレクトリ
if(LIBAV_FOUND)
  target_include_directories(ytdlpSpoutCLI PRIVATE ${LIBAV_INCLUDE_DIRS})
elseif(FFMPEG_INCLUDE_DIR)
  target_include_directories(ytdlpSpoutCLI PRIVATE ${FFMPEG_INCLUDE_DIR})
endif()

# Spout2のインクルードディレクトリ
if(SPOUT_INCLUDE_DIR)
  target_include_directories(ytdlpSpoutCLI PRIVATE ${SPOUT_INCLUDE_DIR})
endif()

# リンク設定
target_link_libraries(ytdlpSpoutCLI PRIVATE
  spdlog::spdlog
  CURL::libcurl
  nlohmann_json::nlohmann_json
  ${D3D11_LIBRARY}
  ${DXGI_LIBRARY}
  ${D3DCOMPILER_LIBRARY}
  winmm
  ole32
)

# CLI11インクルードディレクトリ（シングルヘッダー版）
if(CLI11_INCLUDE_DIR)
  target_include_directories(ytdlpSpoutCLI PRIVATE ${CLI11_INCLUDE_DIR})
endif()

# FFmpegリンク
if(LIBAV_FOUND)
  target_link_libraries(ytdlpSpoutCLI PRIVATE PkgConfig::LIBAV)
elseif(FFMPEG_LIBRARIES)
  target_link_libraries(ytdlpSpoutCLI PRIVATE ${FFMPEG_LIBRARIES})
endif()

# Spout2リンク
if(Spout2_FOUND)
  target_link_libraries(ytdlpSpoutCLI PRIVATE Spout2::Spout)
elseif(SPOUT_LIBRARY)
  target_link_libraries(ytdlpSpoutCLI PRIVATE ${SPOUT_LIBRARY})
endif()

# =============================================================================
# DLLターゲット（Python FFI用）
# =============================================================================
option(BUILD_SHARED_LIB "Build shared library (DLL)" ON)
if(BUILD_SHARED_LIB)
  add_library(ytdlpspout SHARED ${LIB_SOURCES})
  
  # DLLビルドフラグ
  target_compile_definitions(ytdlpspout PRIVATE YTDLPSPOUT_BUILD_DLL)
  
  # シェーダーへの依存
  add_dependencies(ytdlpspout Shaders)
  
  target_include_directories(ytdlpspout PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_BINARY_DIR}/shaders
  )
  
  # 公開インクルードディレクトリ
  target_include_directories(ytdlpspout PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
  )
  
  # FFmpegのインクルードディレクトリ
  if(LIBAV_FOUND)
    target_include_directories(ytdlpspout PRIVATE ${LIBAV_INCLUDE_DIRS})
  elseif(FFMPEG_INCLUDE_DIR)
    target_include_directories(ytdlpspout PRIVATE ${FFMPEG_INCLUDE_DIR})
  endif()
  
  # Spout2のインクルードディレクトリ
  if(SPOUT_INCLUDE_DIR)
    target_include_directories(ytdlpspout PRIVATE ${SPOUT_INCLUDE_DIR})
  endif()
  
  # Spout有効時のコンパイル定義
  if(YTDLPSPOUT_ENABLE_SPOUT)
    target_compile_definitions(ytdlpspout PRIVATE YTDLPSPOUT_ENABLE_SPOUT=1)
  endif()
  
  # リンク設定（静的リンクを可能な限り使用）
  target_link_libraries(ytdlpspout PRIVATE
    spdlog::spdlog
    CURL::libcurl
    nlohmann_json::nlohmann_json
    ${D3D11_LIBRARY}
    ${DXGI_LIBRARY}
    ${D3DCOMPILER_LIBRARY}
    winmm
    ole32
  )
  
  # FFmpegリンク
  if(LIBAV_FOUND)
    target_link_libraries(ytdlpspout PRIVATE PkgConfig::LIBAV)
  elseif(FFMPEG_LIBRARIES)
    target_link_libraries(ytdlpspout PRIVATE ${FFMPEG_LIBRARIES})
  endif()
  
  # Spout2リンク
  if(Spout2_FOUND)
    target_link_libraries(ytdlpspout PRIVATE Spout2::Spout)
  elseif(SPOUT_LIBRARY)
    target_link_libraries(ytdlpspout PRIVATE ${SPOUT_LIBRARY})
  endif()
  
  # DLLの出力設定
  set_target_properties(ytdlpspout PROPERTIES
    OUTPUT_NAME "ytdlpspout"
    VERSION ${PROJECT_VERSION}
    SOVERSION 0
  )
  
  # インストール設定（DLL）
  install(TARGETS ytdlpspout
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
  )
  
  # ヘッダーファイルのインストール
  install(FILES include/ytdlpspout/ytdlpspout.h
    DESTINATION include/ytdlpspout
  )
endif()

# =============================================================================
# テスト設定
# =============================================================================
option(BUILD_TESTING "Build tests" OFF)  # 一時的に無効化
if(BUILD_TESTING)
  enable_testing()
  add_subdirectory(tests)
endif()

# =============================================================================
# インストール設定
# =============================================================================
install(TARGETS ytdlpSpoutCLI
  RUNTIME DESTINATION bin
)

# =============================================================================
# ビルド情報出力
# =============================================================================
message(STATUS "=== ytdlpSpout C++ Backend Configuration ===")
message(STATUS "  C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "  Build Type: ${CMAKE_BUILD_TYPE}")
if(LIBAV_FOUND OR FFMPEG_FOUND)
  message(STATUS "  FFmpeg found: TRUE")
else()
  message(STATUS "  FFmpeg found: FALSE")
endif()
if(Spout2_FOUND OR SPOUT_FOUND)
  message(STATUS "  Spout2 found: TRUE")
else()
  message(STATUS "  Spout2 found: FALSE")
endif()
message(STATUS "  spdlog found: TRUE")
message(STATUS "  CURL found: TRUE")
if(CLI11_INCLUDE_DIR)
  message(STATUS "  CLI11 found: TRUE (header-only)")
else()
  message(STATUS "  CLI11 found: FALSE")
endif()
message(STATUS "============================================")
