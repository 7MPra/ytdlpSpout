"""
Spout Receiver テスト - Senderの検出とフレーム受信を確認
"""
import sys
import time
import argparse


def test_spout_receiver(sender_name: str = "ytdlpSpoutSender", duration: float = 5.0):
    """
    指定したSender名からフレームを受信してテスト
    """
    try:
        import SpoutGL
    except ImportError:
        print("ERROR: SpoutGL がインストールされていません")
        print("インストール: pip install SpoutGL")
        return False
    
    print(f"=== Spout Receiver テスト ===")
    print(f"Sender名: {sender_name}")
    print(f"テスト時間: {duration}秒")
    print()
    
    # Receiver作成
    receiver = SpoutGL.SpoutReceiver()
    receiver.createOpenGL()
    
    # Sender検出
    print("Sender を検出中...")
    start_time = time.time()
    sender_found = False
    
    while time.time() - start_time < 5.0:
        # 利用可能なSenderを確認
        sender_count = receiver.getSenderCount()
        if sender_count > 0:
            print(f"検出されたSender数: {sender_count}")
            for i in range(sender_count):
                name = receiver.getSenderName(i)
                print(f"  [{i}] {name}")
                if name == sender_name:
                    sender_found = True
            break
        time.sleep(0.1)
    
    if not sender_found:
        print(f"WARNING: 指定したSender '{sender_name}' が見つかりません")
        # 続行して接続を試みる
    
    # Senderに接続
    print(f"\n'{sender_name}' に接続を試みます...")
    
    frame_count = 0
    last_frame_time = 0
    fps_samples = []
    
    test_start = time.time()
    
    try:
        import numpy as np
        HAS_NUMPY = True
    except ImportError:
        HAS_NUMPY = False
        print("WARNING: numpy がないためフレームデータの取得はスキップします")
    
    while time.time() - test_start < duration:
        # フレーム受信を試みる
        if HAS_NUMPY:
            result = receiver.receiveImage(sender_name, 0, True, False)
            if result is not None:
                frame_data, width, height = result
                
                if frame_count == 0:
                    print(f"初回フレーム受信: {width}x{height}")
                
                frame_count += 1
                
                current_time = time.time()
                if last_frame_time > 0:
                    fps = 1.0 / (current_time - last_frame_time)
                    fps_samples.append(fps)
                last_frame_time = current_time
                
                # 1秒ごとに進捗表示
                if frame_count % 30 == 0:
                    avg_fps = sum(fps_samples[-30:]) / len(fps_samples[-30:]) if fps_samples else 0
                    print(f"  受信フレーム数: {frame_count}, 平均FPS: {avg_fps:.1f}")
        else:
            time.sleep(0.033)
    
    # 結果表示
    print()
    print("=== テスト結果 ===")
    print(f"総受信フレーム数: {frame_count}")
    if fps_samples:
        avg_fps = sum(fps_samples) / len(fps_samples)
        print(f"平均FPS: {avg_fps:.1f}")
    
    if frame_count > 0:
        print("結果: SUCCESS - フレームを受信できました")
        return True
    else:
        print("結果: FAILED - フレームを受信できませんでした")
        return False


def list_senders():
    """利用可能なSpout Senderを一覧表示"""
    try:
        import SpoutGL
    except ImportError:
        print("ERROR: SpoutGL がインストールされていません")
        return
    
    print("=== 利用可能な Spout Sender ===")
    
    receiver = SpoutGL.SpoutReceiver()
    receiver.createOpenGL()
    
    sender_count = receiver.getSenderCount()
    print(f"検出されたSender数: {sender_count}")
    
    if sender_count == 0:
        print("Senderが見つかりません")
    else:
        for i in range(sender_count):
            name = receiver.getSenderName(i)
            print(f"  [{i}] {name}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Spout Receiver テスト")
    parser.add_argument("--sender", "-s", default="ytdlpSpoutSender",
                        help="接続するSender名 (default: ytdlpSpoutSender)")
    parser.add_argument("--duration", "-d", type=float, default=5.0,
                        help="テスト時間（秒） (default: 5.0)")
    parser.add_argument("--list", "-l", action="store_true",
                        help="利用可能なSenderを一覧表示")
    
    args = parser.parse_args()
    
    if args.list:
        list_senders()
    else:
        success = test_spout_receiver(args.sender, args.duration)
        sys.exit(0 if success else 1)
