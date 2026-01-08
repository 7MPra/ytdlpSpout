"""
GUIと同じ環境でNativeStreamerWrapperをテストするスクリプト
subprocess を使用してクリーンな環境でテスト
"""
import subprocess
import sys

def run_test(test_name, code):
    """新しいPythonプロセスでテストを実行"""
    print(f"\n[{test_name}]")
    env = {"PYTHONIOENCODING": "utf-8"}
    env.update(__import__("os").environ)
    result = subprocess.run(
        [sys.executable, "-c", code],
        capture_output=True,
        text=True,
        cwd="F:\\ytdlpSpout",
        env=env,
        encoding="utf-8",
        errors="replace"
    )
    print(result.stdout)
    if result.stderr:
        print("STDERR:", result.stderr)
    return result.returncode == 0

# Test 1: DLL単体ロード
run_test("Test 1: DLL単体ロード", '''
import ctypes
import os
os.add_dll_directory("F:/ytdlpSpout/cpp/build/vs2022/bin/Release")
lib = ctypes.CDLL("F:/ytdlpSpout/cpp/build/vs2022/bin/Release/ytdlpspout.dll")
lib.ytdlpspout_version.restype = ctypes.c_char_p
print("SUCCESS: Version =", lib.ytdlpspout_version().decode())
''')

# Test 2: SpoutGL -> DLL
run_test("Test 2: SpoutGL後にDLLロード", '''
import SpoutGL
print("SpoutGL imported")
import ctypes
import os
os.add_dll_directory("F:/ytdlpSpout/cpp/build/vs2022/bin/Release")
try:
    lib = ctypes.CDLL("F:/ytdlpSpout/cpp/build/vs2022/bin/Release/ytdlpspout.dll")
    lib.ytdlpspout_version.restype = ctypes.c_char_p
    print("SUCCESS: Version =", lib.ytdlpspout_version().decode())
except Exception as e:
    print("FAILED:", e)
''')

# Test 3: DLL -> SpoutGL
run_test("Test 3: DLL後にSpoutGLロード", '''
import ctypes
import os
os.add_dll_directory("F:/ytdlpSpout/cpp/build/vs2022/bin/Release")
lib = ctypes.CDLL("F:/ytdlpSpout/cpp/build/vs2022/bin/Release/ytdlpspout.dll")
lib.ytdlpspout_version.restype = ctypes.c_char_p
print("DLL loaded: Version =", lib.ytdlpspout_version().decode())
import SpoutGL
print("SpoutGL imported after DLL - SUCCESS")
''')

# Test 4: SpoutSender作成 -> DLL
run_test("Test 4: SpoutSender作成後にDLLロード", '''
import SpoutGL
sender = SpoutGL.SpoutSender()
sender.setSenderName("TestSender")
print("SpoutSender created")
import ctypes
import os
os.add_dll_directory("F:/ytdlpSpout/cpp/build/vs2022/bin/Release")
try:
    lib = ctypes.CDLL("F:/ytdlpSpout/cpp/build/vs2022/bin/Release/ytdlpspout.dll")
    lib.ytdlpspout_version.restype = ctypes.c_char_p
    print("SUCCESS: Version =", lib.ytdlpspout_version().decode())
except Exception as e:
    print("FAILED:", e)
finally:
    sender.releaseSender()
''')

# Test 5: YtdlpSpoutNative
run_test("Test 5: YtdlpSpoutNative初期化", '''
import sys
sys.path.insert(0, ".")
from python.ytdlpspout_native import YtdlpSpoutNative
native = YtdlpSpoutNative()
print("SUCCESS: Version =", native.version)
''')

# Test 6: SpoutGL + YtdlpSpoutNative
run_test("Test 6: SpoutGL + YtdlpSpoutNative", '''
import SpoutGL
print("SpoutGL imported")
import sys
sys.path.insert(0, ".")
try:
    from python.ytdlpspout_native import YtdlpSpoutNative
    native = YtdlpSpoutNative()
    print("SUCCESS: Version =", native.version)
except Exception as e:
    print("FAILED:", e)
''')

print("\n" + "=" * 60)
print("テスト完了")
print("=" * 60)
