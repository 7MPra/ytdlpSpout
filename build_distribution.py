#!/usr/bin/env python3
"""
配布用パッケージを作成するスクリプト
- PyInstallerでexeをビルド
- ffmpegバイナリをダウンロード・配置
- 配布用フォルダを作成
"""

import os
import sys
import subprocess
import urllib.request
import zipfile
import shutil
from pathlib import Path
from datetime import datetime

# 設定
# BtbN/FFmpeg-Buildsの最新リリースURL（latestタグを使用）
FFMPEG_URL = "https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-master-latest-win64-gpl-shared.zip"
DIST_DIR = "dist_package"
BIN_DIR = "bin"

def download_ffmpeg():
    """ffmpegをダウンロードして展開"""
    print("ffmpegをダウンロード中...")
    
    # 一時ディレクトリ作成
    temp_dir = Path("temp_ffmpeg")
    temp_dir.mkdir(exist_ok=True)
    
    try:
        # ffmpegをダウンロード
        zip_path = temp_dir / "ffmpeg.zip"
        urllib.request.urlretrieve(FFMPEG_URL, zip_path)
        print(f"ダウンロード完了: {zip_path}")
        
        # 展開
        with zipfile.ZipFile(zip_path, 'r') as zip_ref:
            zip_ref.extractall(temp_dir)
        
        # ffmpeg.exeを探す
        ffmpeg_exe = None
        for root, dirs, files in os.walk(temp_dir):
            if "ffmpeg.exe" in files:
                ffmpeg_exe = Path(root) / "ffmpeg.exe"
                break
        
        if not ffmpeg_exe:
            raise FileNotFoundError("ffmpeg.exeが見つかりません")
        
        # binディレクトリに配置
        bin_dir = Path(DIST_DIR) / BIN_DIR
        bin_dir.mkdir(parents=True, exist_ok=True)
        
        shutil.copy2(ffmpeg_exe, bin_dir / "ffmpeg.exe")
        print(f"ffmpeg.exeを配置: {bin_dir / 'ffmpeg.exe'}")
        
        # 必要なDLLも探してコピー
        dll_files = []
        for root, dirs, files in os.walk(temp_dir):
            for file in files:
                if file.endswith('.dll') and 'bin' in root:
                    dll_files.append(Path(root) / file)
        
        for dll in dll_files:
            shutil.copy2(dll, bin_dir / dll.name)
            print(f"DLLを配置: {bin_dir / dll.name}")
        
    finally:
        # 一時ディレクトリを削除
        shutil.rmtree(temp_dir, ignore_errors=True)

def build_exe():
    """PyInstallerでexeをビルド"""
    print("PyInstallerでexeをビルド中...")
    
    # 既存のdist, buildを削除
    for dir_name in ["dist", "build"]:
        if os.path.exists(dir_name):
            shutil.rmtree(dir_name)
    
    # GUI版をビルド
    print("GUI版をビルド中...")
    cmd_gui = [
        sys.executable, "-m", "PyInstaller",
        "--noconfirm",
        "ytdlpSpoutGUI.spec"
    ]
    
    result = subprocess.run(cmd_gui, capture_output=True, text=True)
    if result.returncode != 0:
        print(f"GUI版PyInstallerエラー: {result.stderr}")
        return False
    
    # CLI版をビルド
    print("CLI版をビルド中...")
    cmd_cli = [
        sys.executable, "-m", "PyInstaller",
        "--noconfirm",
        "ytdlpSpoutCLI.spec"
    ]
    
    result = subprocess.run(cmd_cli, capture_output=True, text=True)
    if result.returncode != 0:
        print(f"CLI版PyInstallerエラー: {result.stderr}")
        return False
    
    print("exeビルド完了")
    return True

def create_distribution():
    """配布用パッケージを作成"""
    print("配布用パッケージを作成中...")
    
    # 配布ディレクトリを作成
    dist_path = Path(DIST_DIR)
    if dist_path.exists():
        shutil.rmtree(dist_path)
    dist_path.mkdir()
    
    # GUI版exeをコピー
    gui_exe_src = Path("dist") / "ytdlpSpoutGUI.exe"
    if gui_exe_src.exists():
        shutil.copy2(gui_exe_src, dist_path / "ytdlpSpoutGUI.exe")
        print(f"GUI版exeを配置: {dist_path / 'ytdlpSpoutGUI.exe'}")
    else:
        print("警告: GUI版exeファイルが見つかりません")
    
    # CLI版exeをコピー
    cli_exe_src = Path("dist") / "ytdlpSpoutCLI.exe"
    if cli_exe_src.exists():
        shutil.copy2(cli_exe_src, dist_path / "ytdlpSpoutCLI.exe")
        print(f"CLI版exeを配置: {dist_path / 'ytdlpSpoutCLI.exe'}")
    else:
        print("警告: CLI版exeファイルが見つかりません")
    
    # READMEを作成
    readme_content = """# ytdlpSpout - YouTube to Spout Streamer

## 含まれるファイル
- ytdlpSpoutGUI.exe (GUI版アプリケーション)
- ytdlpSpoutCLI.exe (コマンドライン版)
- bin/ffmpeg.exe (動画処理用)
- bin/*.dll (ffmpeg依存ライブラリ)

## GUI版の使用方法
1. ytdlpSpoutGUI.exe を実行
2. YouTube URLを入力
3. Startボタンをクリック

## CLI版の使用方法
コマンドプロンプトから以下のように実行：

基本的な使用:
ytdlpSpoutCLI.exe "https://www.youtube.com/watch?v=dQw4w9WgXcQ"

オプション付き:
ytdlpSpoutCLI.exe -s "MySpoutSender" -w 1280 --height 720 "https://youtu.be/dQw4w9WgXcQ"
ytdlpSpoutCLI.exe --max-width 1920 --max-height 1080 --loop "https://youtu.be/dQw4w9WgXcQ"
ytdlpSpoutCLI.exe --verbose "https://youtu.be/dQw4w9WgXcQ"

ヘルプ表示:
ytdlpSpoutCLI.exe --help

## 注意事項
- binフォルダとexeは同じディレクトリに配置してください
- Spout対応アプリケーション（OBS Studio等）で受信できます
- yt-dlpはexeに埋め込まれているため、別途インストール不要です

## トラブルシューティング
- ffmpegが見つからない場合は、binフォルダの配置を確認してください
- システムにffmpegがインストールされている場合は、そちらが使用されます
"""
    
    with open(dist_path / "README.txt", "w", encoding="utf-8") as f:
        f.write(readme_content)
    
    print(f"配布パッケージ作成完了: {dist_path}")

def create_zip_package():
    """配布パッケージをZIP圧縮"""
    print("配布パッケージをZIP圧縮中...")
    
    # タイムスタンプ付きファイル名を生成
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    zip_filename = f"ytdlpSpout_v{timestamp}.zip"
    
    dist_path = Path(DIST_DIR)
    if not dist_path.exists():
        print(f"エラー: 配布ディレクトリが見つかりません: {dist_path}")
        return None
    
    try:
        with zipfile.ZipFile(zip_filename, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as zipf:
            # 配布ディレクトリ内の全ファイルを再帰的に追加
            for root, dirs, files in os.walk(dist_path):
                for file in files:
                    file_path = Path(root) / file
                    # ZIP内でのパスを相対パスに設定（dist_packageを除く）
                    arcname = file_path.relative_to(dist_path)
                    zipf.write(file_path, arcname)
                    print(f"  追加: {arcname}")
        
        zip_size = Path(zip_filename).stat().st_size / (1024 * 1024)  # MB
        print(f"ZIP作成完了: {zip_filename} ({zip_size:.1f} MB)")
        return zip_filename
        
    except Exception as e:
        print(f"ZIP作成エラー: {e}")
        return None


def main():
    """メイン処理"""
    print("=== ytdlpSpout 配布パッケージ作成 ===")
    
    try:
        # 1. exeをビルド
        if not build_exe():
            print("exeビルドに失敗しました")
            return 1
        
        # 2. 配布ディレクトリを作成
        create_distribution()
        
        # 3. ffmpegをダウンロード・配置
        download_ffmpeg()
        
        # 4. ZIP圧縮
        zip_file = create_zip_package()
        
        print("\n=== 完了 ===")
        print(f"配布パッケージ: {DIST_DIR}/")
        if zip_file:
            print(f"ZIP配布ファイル: {zip_file}")
            print("このZIPファイルを配布してください")
        else:
            print("ZIP作成に失敗しました。手動でフォルダを圧縮してください")
        
        return 0
        
    except Exception as e:
        print(f"エラー: {e}")
        return 1

if __name__ == "__main__":
    sys.exit(main())