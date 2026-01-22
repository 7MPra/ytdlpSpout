import ctypes
import os
import sys
from pathlib import Path

print("--- Minimal DLL Test ---")

# 1. 権限テスト
debug_file_path = "F:/ytdlpSpout/dll_debug_test.txt"
print(f"Testing write permission to: {debug_file_path}")
try:
    with open(debug_file_path, "w") as f:
        f.write("Write test success")
    print("Write permission OK")
    os.remove(debug_file_path)
except Exception as e:
    print(f"Write permission FAILED: {e}")

# 2. DLLロードテスト
dll_path = Path("F:/ytdlpSpout/python/ytdlpspout.dll")
print(f"Loading DLL from: {dll_path}")
print(f"DLL exists: {dll_path.exists()}")
print(f"DLL size: {dll_path.stat().st_size} bytes")

try:
    # 依存関係解決のためにディレクトリを追加
    if hasattr(os, 'add_dll_directory'):
        os.add_dll_directory(str(dll_path.parent))
        # Releaseビルドのbinも念のため追加（依存DLLがあるかも）
        build_bin = Path("F:/ytdlpSpout/cpp/build/bin/Release")
        if build_bin.exists():
             os.add_dll_directory(str(build_bin))

    lib = ctypes.cdll.LoadLibrary(str(dll_path))
    print(f"DLL loaded successfully: {lib}")
except Exception as e:
    print(f"DLL Load FAILED: {e}")
    sys.exit(1)

# 3. 関数呼び出しテスト
try:
    print("Calling ytdlpspout_create...")
    # 関数定義
    lib.ytdlpspout_create.restype = ctypes.c_void_p
    lib.ytdlpspout_create.argtypes = []
    
    handle = lib.ytdlpspout_create()
    print(f"ytdlpspout_create returned handle: {handle}")
    
    if handle:
        # 解放
        lib.ytdlpspout_destroy.argtypes = [ctypes.c_void_p]
        lib.ytdlpspout_destroy(handle)
        print("Handle destroyed")
    else:
        print("Handle is NULL")

except Exception as e:
    print(f"Function call FAILED: {e}")

print("--- Check log files now ---")
