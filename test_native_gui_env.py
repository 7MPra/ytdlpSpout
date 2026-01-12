"""
GUIと同じ環境でNativeStreamerWrapperをテストするスクリプト
GUI環境を再現して、DLLロードの問題を診断する
"""
import sys
import os
import traceback

# 出力エンコーディング設定
sys.stdout.reconfigure(encoding='utf-8', errors='replace')

# GUIと同じようにSpout関連をインポート
print("=" * 60)
print("GUIと同じ環境でテスト")
print("=" * 60)

# Step 1: GUIと同じインポート順序を再現
print("\n[Step 1] GUIと同じインポートを実行...")
try:
    import customtkinter as ctk
    print("  ✓ customtkinter imported")
except ImportError as e:
    print(f"  ✗ customtkinter: {e}")

try:
    from PIL import Image
    print("  ✓ PIL imported")
except ImportError as e:
    print(f"  ✗ PIL: {e}")

try:
    import SpoutGL
    print("  ✓ SpoutGL imported")
except ImportError as e:
    print(f"  ✗ SpoutGL: {e}")

try:
    from OpenGL import GL
    print("  ✓ OpenGL imported")
except ImportError as e:
    print(f"  ✗ OpenGL: {e}")

try:
    import numpy as np
    print("  ✓ numpy imported")
except ImportError as e:
    print(f"  ✗ numpy: {e}")

# Step 2: Spout Sender作成（GUIと同じ）
print("\n[Step 2] SpoutSender作成...")
try:
    spout_sender = SpoutGL.SpoutSender()
    spout_sender.setSenderName("TestSender")
    print("  ✓ SpoutSender created")
except Exception as e:
    print(f"  ✗ SpoutSender: {e}")
    spout_sender = None

# Step 3: DLLロードテスト（直接）
print("\n[Step 3] DLL直接ロードテスト...")
try:
    import ctypes
    from pathlib import Path
    
    dll_path = Path("F:/ytdlpSpout/cpp/build/vs2022/bin/Release/ytdlpspout.dll")
    dll_dir = str(dll_path.parent)
    
    print(f"  DLL path: {dll_path}")
    print(f"  DLL exists: {dll_path.exists()}")
    
    # PATH設定
    os.environ["PATH"] = dll_dir + os.pathsep + os.environ.get("PATH", "")
    
    # DLL directory追加
    if hasattr(os, 'add_dll_directory'):
        os.add_dll_directory(dll_dir)
    
    # 依存DLLを先にロード
    dep_dlls = [
        "zlib1.dll", "avutil-59.dll", "swresample-5.dll", 
        "avcodec-61.dll", "avformat-61.dll", "swscale-8.dll",
        "fmt.dll", "spdlog.dll", "libcurl.dll", "Spout.dll"
    ]
    
    for dep in dep_dlls:
        dep_path = dll_path.parent / dep
        if dep_path.exists():
            try:
                ctypes.WinDLL(str(dep_path))
                print(f"  ✓ {dep} preloaded")
            except Exception as e:
                print(f"  ✗ {dep}: {e}")
    
    # メインDLLロード
    lib = ctypes.WinDLL(str(dll_path))
    lib.ytdlpspout_version.restype = ctypes.c_char_p
    version = lib.ytdlpspout_version().decode()
    print(f"  ✓ ytdlpspout.dll loaded, version: {version}")
    
except Exception as e:
    print(f"  ✗ DLL load failed: {e}")
    traceback.print_exc()

# Step 4: YtdlpSpoutNativeテスト
print("\n[Step 4] YtdlpSpoutNative初期化テスト...")
try:
    from python.ytdlpspout_native import YtdlpSpoutNative
    native = YtdlpSpoutNative()
    print(f"  ✓ YtdlpSpoutNative created, version: {native.version}")
except Exception as e:
    print(f"  ✗ YtdlpSpoutNative failed: {e}")
    traceback.print_exc()

# Step 5: NativeStreamerWrapperテスト
print("\n[Step 5] NativeStreamerWrapper テスト...")
try:
    from python.native_streamer_wrapper import NativeStreamerWrapper
    import time
    import threading
    
    wrapper = NativeStreamerWrapper(
        video_url="https://www.youtube.com/watch?v=dQw4w9WgXcQ",
        sender_name="TestNative",
        log_cb=lambda msg: print(f"  [LOG] {msg}")
    )
    
    print("  Starting wrapper...")
    wrapper.start()
    
    # 5秒待機
    for i in range(10):
        time.sleep(0.5)
        status = f"playing={wrapper.is_playing}, vod={wrapper.is_vod}, frame={'Yes' if wrapper.latest_frame_bgr is not None else 'No'}"
        print(f"  [{i*0.5:.1f}s] {status}")
        if wrapper.latest_frame_bgr is not None:
            print(f"    Frame shape: {wrapper.latest_frame_bgr.shape}")
            break
    
    print("  Stopping wrapper...")
    wrapper.stop()
    print("  ✓ Test completed")
    
except Exception as e:
    print(f"  ✗ NativeStreamerWrapper failed: {e}")
    traceback.print_exc()

# Cleanup
if spout_sender:
    spout_sender.releaseSender()
    print("\n[Cleanup] SpoutSender released")

print("\n" + "=" * 60)
print("テスト完了")
print("=" * 60)
