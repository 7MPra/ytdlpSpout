# =============================================================================
# テスト CMakeLists.txt
# =============================================================================

# Google Test を使用したテスト設定
# 現時点では基本的なユニットテストのみ

# =============================================================================
# M3U8 Parser テスト
# =============================================================================
add_executable(test_m3u8_parser
    test_m3u8_parser.cpp
    ${CMAKE_SOURCE_DIR}/src/hls/M3U8Parser.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/Logger.cpp
)

target_include_directories(test_m3u8_parser PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(test_m3u8_parser PRIVATE
    spdlog::spdlog
)

# テスト登録
add_test(NAME TestM3U8Parser COMMAND test_m3u8_parser)

set_tests_properties(TestM3U8Parser PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

# =============================================================================
# AES Decryptor テスト
# =============================================================================
add_executable(test_aes_decryptor
    test_aes_decryptor.cpp
    ${CMAKE_SOURCE_DIR}/src/hls/AesCbcDecryptor.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/Logger.cpp
)

target_include_directories(test_aes_decryptor PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(test_aes_decryptor PRIVATE
    spdlog::spdlog
    bcrypt
)

# テスト登録
add_test(NAME TestAesDecryptor COMMAND test_aes_decryptor)

set_tests_properties(TestAesDecryptor PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

# =============================================================================
# Decoder テスト
# =============================================================================

# テスト用実行可能ファイル
add_executable(test_decoder
    test_decoder.cpp
)

target_include_directories(test_decoder PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
)

# リンク（メインのライブラリがあれば使用、なければ直接ソースをリンク）
target_link_libraries(test_decoder PRIVATE
    spdlog::spdlog
)

# FFmpegリンク
if(LIBAV_FOUND)
    target_include_directories(test_decoder PRIVATE ${LIBAV_INCLUDE_DIRS})
    target_link_libraries(test_decoder PRIVATE PkgConfig::LIBAV)
elseif(FFMPEG_LIBRARIES)
    target_include_directories(test_decoder PRIVATE ${FFMPEG_INCLUDE_DIR})
    target_link_libraries(test_decoder PRIVATE ${FFMPEG_LIBRARIES})
endif()

# DirectXリンク
target_link_libraries(test_decoder PRIVATE
    ${D3D11_LIBRARY}
    ${DXGI_LIBRARY}
    ole32
)

# テスト登録
add_test(NAME TestDecoder COMMAND test_decoder)

# テストのワーキングディレクトリ設定
set_tests_properties(TestDecoder PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

# =============================================================================
# HLS HTTP Headers テスト
# =============================================================================
add_executable(test_hls_http_headers
    test_hls_http_headers.cpp
)

target_include_directories(test_hls_http_headers PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(test_hls_http_headers PRIVATE
    spdlog::spdlog
)

# FFmpegリンク
if(LIBAV_FOUND)
    target_include_directories(test_hls_http_headers PRIVATE ${LIBAV_INCLUDE_DIRS})
    target_link_libraries(test_hls_http_headers PRIVATE PkgConfig::LIBAV)
elseif(FFMPEG_LIBRARIES)
    target_include_directories(test_hls_http_headers PRIVATE ${FFMPEG_INCLUDE_DIR})
    target_link_libraries(test_hls_http_headers PRIVATE ${FFMPEG_LIBRARIES})
endif()

# DirectXリンク
target_link_libraries(test_hls_http_headers PRIVATE
    ${D3D11_LIBRARY}
    ${DXGI_LIBRARY}
    ole32
)

# テスト登録
add_test(NAME TestHlsHttpHeaders COMMAND test_hls_http_headers)

set_tests_properties(TestHlsHttpHeaders PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

# =============================================================================
# HttpClient テスト
# =============================================================================
add_executable(test_http_client
    test_http_client.cpp
    ${CMAKE_SOURCE_DIR}/src/io/HttpClient.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/Logger.cpp
)

target_include_directories(test_http_client PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(test_http_client PRIVATE
    spdlog::spdlog
    CURL::libcurl
)

# テスト登録
add_test(NAME TestHttpClient COMMAND test_http_client)

set_tests_properties(TestHttpClient PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

# =============================================================================
# SparseFileCache テスト
# =============================================================================
add_executable(test_sparse_file_cache
    test_sparse_file_cache.cpp
    ${CMAKE_SOURCE_DIR}/src/io/SparseFileCache.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/Logger.cpp
)

target_include_directories(test_sparse_file_cache PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(test_sparse_file_cache PRIVATE
    spdlog::spdlog
)

# テスト登録
add_test(NAME TestSparseFileCache COMMAND test_sparse_file_cache)

set_tests_properties(TestSparseFileCache PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

# =============================================================================
# ChunkDownloader テスト
# =============================================================================
add_executable(test_chunk_downloader
    test_chunk_downloader.cpp
    ${CMAKE_SOURCE_DIR}/src/io/ChunkDownloader.cpp
    ${CMAKE_SOURCE_DIR}/src/io/SparseFileCache.cpp
    ${CMAKE_SOURCE_DIR}/src/io/HttpClient.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/Logger.cpp
)

target_include_directories(test_chunk_downloader PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(test_chunk_downloader PRIVATE
    spdlog::spdlog
    CURL::libcurl
    ws2_32
)

# テスト登録
add_test(NAME TestChunkDownloader COMMAND test_chunk_downloader)

set_tests_properties(TestChunkDownloader PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

# =============================================================================
# CustomIOContext テスト
# =============================================================================
add_executable(test_custom_io_context
    test_custom_io_context.cpp
    ${CMAKE_SOURCE_DIR}/src/io/CustomIOContext.cpp
    ${CMAKE_SOURCE_DIR}/src/io/ChunkDownloader.cpp
    ${CMAKE_SOURCE_DIR}/src/io/SparseFileCache.cpp
    ${CMAKE_SOURCE_DIR}/src/io/HttpClient.cpp
    ${CMAKE_SOURCE_DIR}/src/io/PrefetchScheduler.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/Logger.cpp
)

target_include_directories(test_custom_io_context PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(test_custom_io_context PRIVATE
    spdlog::spdlog
    CURL::libcurl
)

# FFmpegリンク
if(LIBAV_FOUND)
    target_include_directories(test_custom_io_context PRIVATE ${LIBAV_INCLUDE_DIRS})
    target_link_libraries(test_custom_io_context PRIVATE PkgConfig::LIBAV)
elseif(FFMPEG_LIBRARIES)
    target_include_directories(test_custom_io_context PRIVATE ${FFMPEG_INCLUDE_DIR})
    target_link_libraries(test_custom_io_context PRIVATE ${FFMPEG_LIBRARIES})
endif()

# テスト登録
add_test(NAME TestCustomIOContext COMMAND test_custom_io_context)

set_tests_properties(TestCustomIOContext PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

# =============================================================================
# VideoDecoder + CustomIOContext 統合テスト
# =============================================================================
add_executable(test_decoder_custom_io
    test_decoder_custom_io.cpp
    ${CMAKE_SOURCE_DIR}/src/decoder/VideoDecoder.cpp
    ${CMAKE_SOURCE_DIR}/src/decoder/HWAccelContext.cpp
    ${CMAKE_SOURCE_DIR}/src/io/CustomIOContext.cpp
    ${CMAKE_SOURCE_DIR}/src/io/ChunkDownloader.cpp
    ${CMAKE_SOURCE_DIR}/src/io/SparseFileCache.cpp
    ${CMAKE_SOURCE_DIR}/src/io/HttpClient.cpp
    ${CMAKE_SOURCE_DIR}/src/io/PrefetchScheduler.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/Logger.cpp
    ${CMAKE_SOURCE_DIR}/src/hls/HlsSliceLoadingManager.cpp
    ${CMAKE_SOURCE_DIR}/src/hls/HlsCustomAVIOContext.cpp
    ${CMAKE_SOURCE_DIR}/src/hls/HlsSegmentCache.cpp
    ${CMAKE_SOURCE_DIR}/src/hls/AesCbcDecryptor.cpp
    ${CMAKE_SOURCE_DIR}/src/hls/M3U8Parser.cpp
)

target_include_directories(test_decoder_custom_io PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(test_decoder_custom_io PRIVATE
    spdlog::spdlog
    CURL::libcurl
    bcrypt
)

# FFmpegリンク
if(LIBAV_FOUND)
    target_include_directories(test_decoder_custom_io PRIVATE ${LIBAV_INCLUDE_DIRS})
    target_link_libraries(test_decoder_custom_io PRIVATE PkgConfig::LIBAV)
elseif(FFMPEG_LIBRARIES)
    target_include_directories(test_decoder_custom_io PRIVATE ${FFMPEG_INCLUDE_DIR})
    target_link_libraries(test_decoder_custom_io PRIVATE ${FFMPEG_LIBRARIES})
endif()

# DirectXリンク
target_link_libraries(test_decoder_custom_io PRIVATE
    ${D3D11_LIBRARY}
    ${DXGI_LIBRARY}
    ole32
)

# テスト登録
add_test(NAME TestDecoderCustomIO COMMAND test_decoder_custom_io)

set_tests_properties(TestDecoderCustomIO PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

# =============================================================================
# YtDlpResolver テスト
# =============================================================================
add_executable(test_ytdlp_resolver
    test_ytdlp_resolver.cpp
    ${CMAKE_SOURCE_DIR}/src/ytdlp/YtDlpResolver.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/Logger.cpp
)

target_include_directories(test_ytdlp_resolver PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(test_ytdlp_resolver PRIVATE
    spdlog::spdlog
    nlohmann_json::nlohmann_json
)

# テスト登録
add_test(NAME TestYtDlpResolver COMMAND test_ytdlp_resolver)

set_tests_properties(TestYtDlpResolver PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

# =============================================================================
# AudioDecoder テスト
# =============================================================================
find_package(doctest CONFIG REQUIRED)

add_executable(test_audio_decoder
    test_audio_decoder.cpp
    ${CMAKE_SOURCE_DIR}/src/audio/AudioDecoder.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/Logger.cpp
)

target_include_directories(test_audio_decoder PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(test_audio_decoder PRIVATE
    spdlog::spdlog
    doctest::doctest
)

# FFmpegリンク
if(LIBAV_FOUND)
    target_include_directories(test_audio_decoder PRIVATE ${LIBAV_INCLUDE_DIRS})
    target_link_libraries(test_audio_decoder PRIVATE PkgConfig::LIBAV)
elseif(FFMPEG_LIBRARIES)
    target_include_directories(test_audio_decoder PRIVATE ${FFMPEG_INCLUDE_DIR})
    target_link_libraries(test_audio_decoder PRIVATE ${FFMPEG_LIBRARIES})
endif()

# テスト登録
add_test(NAME TestAudioDecoder COMMAND test_audio_decoder)

set_tests_properties(TestAudioDecoder PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

# =============================================================================
# BeatMap テスト
# =============================================================================
add_executable(test_beat_map
    test_beat_map.cpp
    ${CMAKE_SOURCE_DIR}/src/audio/BeatMap.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/Logger.cpp
)

target_include_directories(test_beat_map PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(test_beat_map PRIVATE
    spdlog::spdlog
    doctest::doctest
    nlohmann_json::nlohmann_json
)

# テスト登録
add_test(NAME TestBeatMap COMMAND test_beat_map)

set_tests_properties(TestBeatMap PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

# =============================================================================
# BeatDetector テスト
# =============================================================================
add_executable(test_beat_detector
    test_beat_detector.cpp
    ${CMAKE_SOURCE_DIR}/src/audio/BeatDetector.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/Logger.cpp
)

target_include_directories(test_beat_detector PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(test_beat_detector PRIVATE
    spdlog::spdlog
    doctest::doctest
)

# テスト登録
add_test(NAME TestBeatDetector COMMAND test_beat_detector)

set_tests_properties(TestBeatDetector PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

# =============================================================================
# BeatMapGenerator テスト
# =============================================================================
add_executable(test_beat_map_generator
    test_beat_map_generator.cpp
    ${CMAKE_SOURCE_DIR}/src/audio/BeatMapGenerator.cpp
    ${CMAKE_SOURCE_DIR}/src/audio/BeatDetector.cpp
    ${CMAKE_SOURCE_DIR}/src/audio/BeatMap.cpp
    ${CMAKE_SOURCE_DIR}/src/audio/AudioDecoder.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/Logger.cpp
)

target_include_directories(test_beat_map_generator PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(test_beat_map_generator PRIVATE
    spdlog::spdlog
    doctest::doctest
    nlohmann_json::nlohmann_json
)

# FFmpegリンク
if(LIBAV_FOUND)
    target_include_directories(test_beat_map_generator PRIVATE ${LIBAV_INCLUDE_DIRS})
    target_link_libraries(test_beat_map_generator PRIVATE PkgConfig::LIBAV)
elseif(FFMPEG_LIBRARIES)
    target_include_directories(test_beat_map_generator PRIVATE ${FFMPEG_INCLUDE_DIR})
    target_link_libraries(test_beat_map_generator PRIVATE ${FFMPEG_LIBRARIES})
endif()

# テスト登録
add_test(NAME TestBeatMapGenerator COMMAND test_beat_map_generator)

set_tests_properties(TestBeatMapGenerator PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

# =============================================================================
# VideoPlayer ビート機能統合テスト
# =============================================================================

add_executable(test_video_player_beat
    test_video_player_beat.cpp
    ${CMAKE_SOURCE_DIR}/src/player/VideoPlayer.cpp
    ${CMAKE_SOURCE_DIR}/src/player/FrameTimer.cpp
    ${CMAKE_SOURCE_DIR}/src/decoder/VideoDecoder.cpp
    ${CMAKE_SOURCE_DIR}/src/decoder/FrameConverter.cpp
    ${CMAKE_SOURCE_DIR}/src/decoder/HWAccelContext.cpp
    ${CMAKE_SOURCE_DIR}/src/graphics/D3D11Context.cpp
    ${CMAKE_SOURCE_DIR}/src/graphics/SpoutSender.cpp
    ${CMAKE_SOURCE_DIR}/src/graphics/TexturePool.cpp
    ${CMAKE_SOURCE_DIR}/src/audio/BeatMap.cpp
    ${CMAKE_SOURCE_DIR}/src/audio/BeatMapGenerator.cpp
    ${CMAKE_SOURCE_DIR}/src/audio/BeatDetector.cpp
    ${CMAKE_SOURCE_DIR}/src/audio/AudioDecoder.cpp
    ${CMAKE_SOURCE_DIR}/src/io/CustomIOContext.cpp
    ${CMAKE_SOURCE_DIR}/src/io/ChunkDownloader.cpp
    ${CMAKE_SOURCE_DIR}/src/io/SparseFileCache.cpp
    ${CMAKE_SOURCE_DIR}/src/io/HttpClient.cpp
    ${CMAKE_SOURCE_DIR}/src/io/PrefetchScheduler.cpp
    ${CMAKE_SOURCE_DIR}/src/io/SliceLoadingManager.cpp
    ${CMAKE_SOURCE_DIR}/src/hls/HlsSliceLoadingManager.cpp
    ${CMAKE_SOURCE_DIR}/src/hls/HlsCustomAVIOContext.cpp
    ${CMAKE_SOURCE_DIR}/src/hls/HlsSegmentCache.cpp
    ${CMAKE_SOURCE_DIR}/src/hls/M3U8Parser.cpp
    ${CMAKE_SOURCE_DIR}/src/hls/AesCbcDecryptor.cpp
    ${CMAKE_SOURCE_DIR}/src/ytdlp/YtDlpResolver.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/Logger.cpp
)

target_include_directories(test_video_player_beat PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(test_video_player_beat PRIVATE
    spdlog::spdlog
    doctest::doctest
    nlohmann_json::nlohmann_json
    CURL::libcurl
    ${D3D11_LIBRARY}
    ${DXGI_LIBRARY}
    d3dcompiler
    ole32
)

# Spout2リンク
if(TARGET Spout2::Spout)
    target_link_libraries(test_video_player_beat PRIVATE Spout2::Spout)
endif()

# FFmpegリンク
if(LIBAV_FOUND)
    target_include_directories(test_video_player_beat PRIVATE ${LIBAV_INCLUDE_DIRS})
    target_link_libraries(test_video_player_beat PRIVATE PkgConfig::LIBAV)
elseif(FFMPEG_LIBRARIES)
    target_include_directories(test_video_player_beat PRIVATE ${FFMPEG_INCLUDE_DIR})
    target_link_libraries(test_video_player_beat PRIVATE ${FFMPEG_LIBRARIES})
endif()

# テスト登録
add_test(NAME TestVideoPlayerBeat COMMAND test_video_player_beat)

set_tests_properties(TestVideoPlayerBeat PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

# =============================================================================
# C API テスト
# =============================================================================
add_executable(test_c_api
    test_c_api.cpp
    ${CMAKE_SOURCE_DIR}/src/bindings/c_api.cpp
    ${CMAKE_SOURCE_DIR}/src/player/VideoPlayer.cpp
    ${CMAKE_SOURCE_DIR}/src/player/FrameTimer.cpp
    ${CMAKE_SOURCE_DIR}/src/decoder/VideoDecoder.cpp
    ${CMAKE_SOURCE_DIR}/src/decoder/HWAccelContext.cpp
    ${CMAKE_SOURCE_DIR}/src/decoder/FrameConverter.cpp
    ${CMAKE_SOURCE_DIR}/src/graphics/D3D11Context.cpp
    ${CMAKE_SOURCE_DIR}/src/graphics/SpoutSender.cpp
    ${CMAKE_SOURCE_DIR}/src/graphics/TexturePool.cpp
    ${CMAKE_SOURCE_DIR}/src/io/HttpClient.cpp
    ${CMAKE_SOURCE_DIR}/src/io/SparseFileCache.cpp
    ${CMAKE_SOURCE_DIR}/src/io/ChunkDownloader.cpp
    ${CMAKE_SOURCE_DIR}/src/io/PrefetchScheduler.cpp
    ${CMAKE_SOURCE_DIR}/src/io/CustomIOContext.cpp
    ${CMAKE_SOURCE_DIR}/src/io/SliceLoadingManager.cpp
    ${CMAKE_SOURCE_DIR}/src/hls/HlsSliceLoadingManager.cpp
    ${CMAKE_SOURCE_DIR}/src/hls/HlsCustomAVIOContext.cpp
    ${CMAKE_SOURCE_DIR}/src/hls/HlsSegmentCache.cpp
    ${CMAKE_SOURCE_DIR}/src/hls/M3U8Parser.cpp
    ${CMAKE_SOURCE_DIR}/src/hls/AesCbcDecryptor.cpp
    ${CMAKE_SOURCE_DIR}/src/ytdlp/YtDlpResolver.cpp
    ${CMAKE_SOURCE_DIR}/src/audio/AudioDecoder.cpp
    ${CMAKE_SOURCE_DIR}/src/audio/BeatMap.cpp
    ${CMAKE_SOURCE_DIR}/src/audio/BeatDetector.cpp
    ${CMAKE_SOURCE_DIR}/src/audio/BeatMapGenerator.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/Logger.cpp
)

target_include_directories(test_c_api PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_BINARY_DIR}/shaders
)

target_link_libraries(test_c_api PRIVATE
    spdlog::spdlog
    doctest::doctest
    nlohmann_json::nlohmann_json
    CURL::libcurl
    ${D3D11_LIBRARY}
    ${DXGI_LIBRARY}
    d3dcompiler
    ole32
    winmm
)

# Spout2リンク
if(TARGET Spout2::Spout)
    target_link_libraries(test_c_api PRIVATE Spout2::Spout)
elseif(SPOUT_LIBRARY)
    target_link_libraries(test_c_api PRIVATE ${SPOUT_LIBRARY})
endif()

if(SPOUT_INCLUDE_DIR)
    target_include_directories(test_c_api PRIVATE ${SPOUT_INCLUDE_DIR})
endif()

# FFmpegリンク
if(LIBAV_FOUND)
    target_include_directories(test_c_api PRIVATE ${LIBAV_INCLUDE_DIRS})
    target_link_libraries(test_c_api PRIVATE PkgConfig::LIBAV)
elseif(FFMPEG_LIBRARIES)
    target_include_directories(test_c_api PRIVATE ${FFMPEG_INCLUDE_DIR})
    target_link_libraries(test_c_api PRIVATE ${FFMPEG_LIBRARIES})
endif()

# シェーダーへの依存
add_dependencies(test_c_api Shaders)

# テスト登録
add_test(NAME TestCApi COMMAND test_c_api)

set_tests_properties(TestCApi PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

# =============================================================================
# SliceLoadingManager テスト
# =============================================================================
add_executable(test_slice_loading_manager
    test_slice_loading_manager.cpp
    ${CMAKE_SOURCE_DIR}/src/io/SliceLoadingManager.cpp
    ${CMAKE_SOURCE_DIR}/src/io/CustomIOContext.cpp
    ${CMAKE_SOURCE_DIR}/src/io/SparseFileCache.cpp
    ${CMAKE_SOURCE_DIR}/src/io/ChunkDownloader.cpp
    ${CMAKE_SOURCE_DIR}/src/io/PrefetchScheduler.cpp
    ${CMAKE_SOURCE_DIR}/src/io/HttpClient.cpp
    ${CMAKE_SOURCE_DIR}/src/ytdlp/YtDlpResolver.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/Logger.cpp
)

target_include_directories(test_slice_loading_manager PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(test_slice_loading_manager PRIVATE
    spdlog::spdlog
    doctest::doctest
    nlohmann_json::nlohmann_json
    CURL::libcurl
)

# FFmpegリンク
if(LIBAV_FOUND)
    target_include_directories(test_slice_loading_manager PRIVATE ${LIBAV_INCLUDE_DIRS})
    target_link_libraries(test_slice_loading_manager PRIVATE PkgConfig::LIBAV)
elseif(FFMPEG_LIBRARIES)
    target_include_directories(test_slice_loading_manager PRIVATE ${FFMPEG_INCLUDE_DIR})
    target_link_libraries(test_slice_loading_manager PRIVATE ${FFMPEG_LIBRARIES})
endif()

# テスト登録
add_test(NAME TestSliceLoadingManager COMMAND test_slice_loading_manager)

set_tests_properties(TestSliceLoadingManager PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

# =============================================================================
# D3D11 Staging Pool テスト
# =============================================================================
add_executable(test_d3d11_staging_pool
    test_d3d11_staging_pool.cpp
    ${CMAKE_SOURCE_DIR}/src/graphics/D3D11Context.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/Logger.cpp
)

target_include_directories(test_d3d11_staging_pool PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(test_d3d11_staging_pool PRIVATE
    spdlog::spdlog
    ${D3D11_LIBRARY}
    ${DXGI_LIBRARY}
    ${D3DCOMPILER_LIBRARY}
    ole32
)

# テスト登録
add_test(NAME TestD3D11StagingPool COMMAND test_d3d11_staging_pool)

set_tests_properties(TestD3D11StagingPool PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

# =============================================================================
# D3D11 Async Readback テスト
# =============================================================================
add_executable(test_d3d11_async_readback
    test_d3d11_async_readback.cpp
    ${CMAKE_SOURCE_DIR}/src/graphics/D3D11Context.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/Logger.cpp
)

target_include_directories(test_d3d11_async_readback PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(test_d3d11_async_readback PRIVATE
    spdlog::spdlog
    ${D3D11_LIBRARY}
    ${DXGI_LIBRARY}
    ${D3DCOMPILER_LIBRARY}
    ole32
)

# テスト登録
add_test(NAME TestD3D11AsyncReadback COMMAND test_d3d11_async_readback)

set_tests_properties(TestD3D11AsyncReadback PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
# =============================================================================
# NV12 SRV Pool テスト
# =============================================================================
add_executable(test_nv12_srv_pool
    test_nv12_srv_pool.cpp
    ${CMAKE_SOURCE_DIR}/src/graphics/D3D11Context.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/Logger.cpp
)

target_include_directories(test_nv12_srv_pool PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(test_nv12_srv_pool PRIVATE
    spdlog::spdlog
    ${D3D11_LIBRARY}
    ${DXGI_LIBRARY}
    ${D3DCOMPILER_LIBRARY}
    ole32
)

# テスト登録
add_test(NAME TestNV12SRVPool COMMAND test_nv12_srv_pool)

set_tests_properties(TestNV12SRVPool PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

# =============================================================================
# HLS Segment Cache テスト
# =============================================================================
add_executable(test_hls_segment_cache
    test_hls_segment_cache.cpp
    ${CMAKE_SOURCE_DIR}/src/hls/HlsSegmentCache.cpp
    ${CMAKE_SOURCE_DIR}/src/hls/AesCbcDecryptor.cpp
    ${CMAKE_SOURCE_DIR}/src/hls/M3U8Parser.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/Logger.cpp
)

target_include_directories(test_hls_segment_cache PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(test_hls_segment_cache PRIVATE
    spdlog::spdlog
    bcrypt
)

# テスト登録
add_test(NAME TestHlsSegmentCache COMMAND test_hls_segment_cache)

set_tests_properties(TestHlsSegmentCache PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

# =============================================================================
# HLS Custom AVIO Context テスト
# =============================================================================
find_package(GTest CONFIG REQUIRED)

add_executable(test_hls_avio
    test_hls_avio.cpp
    ${CMAKE_SOURCE_DIR}/src/hls/HlsCustomAVIOContext.cpp
    ${CMAKE_SOURCE_DIR}/src/hls/HlsSegmentCache.cpp
    ${CMAKE_SOURCE_DIR}/src/hls/AesCbcDecryptor.cpp
    ${CMAKE_SOURCE_DIR}/src/hls/M3U8Parser.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/Logger.cpp
)

target_include_directories(test_hls_avio PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(test_hls_avio PRIVATE
    spdlog::spdlog
    GTest::gtest
    GTest::gtest_main
    bcrypt
)

# FFmpegリンク
if(LIBAV_FOUND)
    target_include_directories(test_hls_avio PRIVATE ${LIBAV_INCLUDE_DIRS})
    target_link_libraries(test_hls_avio PRIVATE PkgConfig::LIBAV)
elseif(FFMPEG_LIBRARIES)
    target_include_directories(test_hls_avio PRIVATE ${FFMPEG_INCLUDE_DIR})
    target_link_libraries(test_hls_avio PRIVATE ${FFMPEG_LIBRARIES})
endif()

# テスト登録
add_test(NAME TestHlsAVIO COMMAND test_hls_avio)

set_tests_properties(TestHlsAVIO PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

# =============================================================================
# HLS Slice Loading Manager テスト
# =============================================================================
add_executable(test_hls_slice_loading_manager
    test_hls_slice_loading_manager.cpp
    ${CMAKE_SOURCE_DIR}/src/hls/HlsSliceLoadingManager.cpp
    ${CMAKE_SOURCE_DIR}/src/hls/HlsCustomAVIOContext.cpp
    ${CMAKE_SOURCE_DIR}/src/hls/HlsSegmentCache.cpp
    ${CMAKE_SOURCE_DIR}/src/hls/AesCbcDecryptor.cpp
    ${CMAKE_SOURCE_DIR}/src/hls/M3U8Parser.cpp
    ${CMAKE_SOURCE_DIR}/src/io/HttpClient.cpp
    ${CMAKE_SOURCE_DIR}/src/io/ChunkDownloader.cpp
    ${CMAKE_SOURCE_DIR}/src/io/SparseFileCache.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/Logger.cpp
)

target_include_directories(test_hls_slice_loading_manager PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(test_hls_slice_loading_manager PRIVATE
    spdlog::spdlog
    GTest::gtest
    GTest::gtest_main
    CURL::libcurl
    bcrypt
)

# FFmpegリンク
if(LIBAV_FOUND)
    target_include_directories(test_hls_slice_loading_manager PRIVATE ${LIBAV_INCLUDE_DIRS})
    target_link_libraries(test_hls_slice_loading_manager PRIVATE PkgConfig::LIBAV)
elseif(FFMPEG_LIBRARIES)
    target_include_directories(test_hls_slice_loading_manager PRIVATE ${FFMPEG_INCLUDE_DIR})
    target_link_libraries(test_hls_slice_loading_manager PRIVATE ${FFMPEG_LIBRARIES})
endif()

# テスト登録
add_test(NAME TestHlsSliceLoadingManager COMMAND test_hls_slice_loading_manager)

set_tests_properties(TestHlsSliceLoadingManager PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

# =============================================================================
# VideoPlayer HLSスライス読み込み統合テスト
# =============================================================================
add_executable(test_video_player_hls
    test_video_player_hls.cpp
    ${CMAKE_SOURCE_DIR}/src/player/VideoPlayer.cpp
    ${CMAKE_SOURCE_DIR}/src/player/FrameTimer.cpp
    ${CMAKE_SOURCE_DIR}/src/decoder/VideoDecoder.cpp
    ${CMAKE_SOURCE_DIR}/src/decoder/FrameConverter.cpp
    ${CMAKE_SOURCE_DIR}/src/decoder/HWAccelContext.cpp
    ${CMAKE_SOURCE_DIR}/src/graphics/D3D11Context.cpp
    ${CMAKE_SOURCE_DIR}/src/graphics/SpoutSender.cpp
    ${CMAKE_SOURCE_DIR}/src/graphics/TexturePool.cpp
    ${CMAKE_SOURCE_DIR}/src/audio/BeatMap.cpp
    ${CMAKE_SOURCE_DIR}/src/audio/BeatMapGenerator.cpp
    ${CMAKE_SOURCE_DIR}/src/audio/BeatDetector.cpp
    ${CMAKE_SOURCE_DIR}/src/audio/AudioDecoder.cpp
    ${CMAKE_SOURCE_DIR}/src/io/CustomIOContext.cpp
    ${CMAKE_SOURCE_DIR}/src/io/ChunkDownloader.cpp
    ${CMAKE_SOURCE_DIR}/src/io/SparseFileCache.cpp
    ${CMAKE_SOURCE_DIR}/src/io/HttpClient.cpp
    ${CMAKE_SOURCE_DIR}/src/io/PrefetchScheduler.cpp
    ${CMAKE_SOURCE_DIR}/src/io/SliceLoadingManager.cpp
    ${CMAKE_SOURCE_DIR}/src/hls/HlsSliceLoadingManager.cpp
    ${CMAKE_SOURCE_DIR}/src/hls/HlsCustomAVIOContext.cpp
    ${CMAKE_SOURCE_DIR}/src/hls/HlsSegmentCache.cpp
    ${CMAKE_SOURCE_DIR}/src/hls/AesCbcDecryptor.cpp
    ${CMAKE_SOURCE_DIR}/src/hls/M3U8Parser.cpp
    ${CMAKE_SOURCE_DIR}/src/ytdlp/YtDlpResolver.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/Logger.cpp
)

target_include_directories(test_video_player_hls PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(test_video_player_hls PRIVATE
    spdlog::spdlog
    GTest::gtest
    GTest::gtest_main
    nlohmann_json::nlohmann_json
    CURL::libcurl
    bcrypt
    ${D3D11_LIBRARY}
    ${DXGI_LIBRARY}
    d3dcompiler
    ole32
)

# Spout2リンク
if(TARGET Spout2::Spout)
    target_link_libraries(test_video_player_hls PRIVATE Spout2::Spout)
endif()

# FFmpegリンク
if(LIBAV_FOUND)
    target_include_directories(test_video_player_hls PRIVATE ${LIBAV_INCLUDE_DIRS})
    target_link_libraries(test_video_player_hls PRIVATE PkgConfig::LIBAV)
elseif(FFMPEG_LIBRARIES)
    target_include_directories(test_video_player_hls PRIVATE ${FFMPEG_INCLUDE_DIR})
    target_link_libraries(test_video_player_hls PRIVATE ${FFMPEG_LIBRARIES})
endif()

# テスト登録
add_test(NAME TestVideoPlayerHls COMMAND test_video_player_hls)

set_tests_properties(TestVideoPlayerHls PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)