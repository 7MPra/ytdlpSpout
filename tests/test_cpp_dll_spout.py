"""
C++ DLL 直接テスト - Spout送信が動作するか確認
"""
import sys
import time
import os

# DLLパスを追加
dll_paths = [
    r"F:\ytdlpSpout\cpp\build\vs2022\bin\Release",
    r"F:\ytdlpSpout\cpp\build\vs2022\bin\Debug",
]
for dll_path in dll_paths:
    if os.path.exists(dll_path):
        os.environ["PATH"] = dll_path + os.pathsep + os.environ.get("PATH", "")
        if hasattr(os, 'add_dll_directory'):
            try:
                os.add_dll_directory(dll_path)
            except OSError:
                pass

# プロジェクトルートをパスに追加
sys.path.insert(0, r"F:\ytdlpSpout")

from python.ytdlpspout_native import YtdlpSpoutNative


def test_cpp_dll_spout(video_path: str, duration: float = 5.0):
    """
    C++ DLLを直接使ってSpout送信をテスト
    """
    print(f"=== C++ DLL Spout 送信テスト ===")
    print(f"動画: {video_path}")
    print(f"テスト時間: {duration}秒")
    print()
    
    if not os.path.exists(video_path):
        print(f"ERROR: ファイルが見つかりません: {video_path}")
        return False
    
    # DLL初期化
    print("DLL初期化中...")
    native = YtdlpSpoutNative()
    print(f"DLLバージョン: {native.version}")
    
    # 再生開始
    print("再生開始...")
    sender_name = "TestSpoutSender"
    
    try:
        native.start(
            input_file=video_path,
            sender_name=sender_name,
            loop=False,
            verbose=True
        )
    except Exception as e:
        print(f"ERROR: 再生開始失敗: {e}")
        return False
    
    # 動画情報取得
    info = native.get_video_info()
    if info:
        print(f"動画情報: {info['width']}x{info['height']} @ {info['fps']}fps, duration={info['duration']:.1f}s")
    
    print(f"\nSpout Sender名: {sender_name}")
    print("フレーム処理中...")
    
    # フレーム処理ループ
    start_time = time.time()
    frame_count = 0
    
    while time.time() - start_time < duration:
        if not native.is_playing:
            print("再生終了")
            break
        
        if native.process_frame():
            frame_count += 1
            if frame_count % 30 == 0:
                pos = native.position
                print(f"  フレーム数: {frame_count}, 位置: {pos:.2f}秒")
        else:
            print("process_frame() が False を返しました")
            break
    
    # 停止
    print("\n停止中...")
    native.stop()
    
    print()
    print("=== テスト結果 ===")
    print(f"処理フレーム数: {frame_count}")
    print(f"別ウィンドウでSpout受信アプリを開いて '{sender_name}' からの映像を確認してください")
    
    return frame_count > 0


if __name__ == "__main__":
    import argparse
    
    parser = argparse.ArgumentParser(description="C++ DLL Spout送信テスト")
    parser.add_argument("video", nargs="?", default=r"data\test.mp4",
                        help="テスト用動画ファイル")
    parser.add_argument("--duration", "-d", type=float, default=5.0,
                        help="テスト時間（秒）")
    
    args = parser.parse_args()
    
    success = test_cpp_dll_spout(args.video, args.duration)
    sys.exit(0 if success else 1)
