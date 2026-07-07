#!/usr/bin/env python3
"""
配布用パッケージを作成するスクリプト
- C++ DLLをビルド（オプション）
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
# yt-dlpの最新リリースURL
YT_DLP_URL = "https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp.exe"
DIST_DIR = "dist_package"
BIN_DIR = "bin"
CPP_BUILD_DIR = Path("cpp/build")
PYTHON_DIR = Path("python")

# 配布に必要なC++依存DLL（cpp/build/bin/Release から収集）。
# gtest.dll / gtest_main.dll（テストフレームワーク用）や、fmtd.dll等のDebugビルド
# 成果物は配布に含めない（そもそもReleaseビルド出力には含まれない）。
DIST_REQUIRED_DLLS = [
    "ytdlpspout.dll",
    "avcodec-61.dll",
    "avformat-61.dll",
    "avutil-59.dll",
    "swscale-8.dll",
    "swresample-5.dll",
    "libcurl.dll",
    "spdlog.dll",
    "fmt.dll",
    "zlib1.dll",
    "Spout.dll",
]


def build_cpp_dll(skip_if_exists=False):
    """C++ DLL (ytdlpspout.dll) をビルド"""
    print("C++ DLLをビルド中...")
    
    dll_path = CPP_BUILD_DIR / "bin" / "Release" / "ytdlpspout.dll"
    
    if skip_if_exists and dll_path.exists():
        print(f"C++ DLLは既に存在します: {dll_path}")
    else:
        # CMakeビルド
        if not CPP_BUILD_DIR.exists():
            print("エラー: C++ビルドディレクトリが存在しません。先にCMakeを実行してください。")
            print("  cd cpp && mkdir build && cd build && cmake .. && cmake --build . --config Release")
            return False
        
        cmd = ["cmake", "--build", str(CPP_BUILD_DIR), "--config", "Release", "--target", "ytdlpspout"]
        result = subprocess.run(cmd, capture_output=True, text=True)
        if result.returncode != 0:
            print(f"C++ DLLビルドエラー: {result.stderr}")
            return False
        
        print("C++ DLLビルド完了")
    
    # DLLをpythonディレクトリにコピー
    if dll_path.exists():
        PYTHON_DIR.mkdir(exist_ok=True)
        shutil.copy2(dll_path, PYTHON_DIR / "ytdlpspout.dll")
        print(f"DLLをコピー: {PYTHON_DIR / 'ytdlpspout.dll'}")
    else:
        print(f"警告: DLLが見つかりません: {dll_path}")
        return False
    
    return True


def copy_cpp_dependency_dlls():
    """配布に必要なC++依存DLLを cpp/build/bin/Release から配布ディレクトリへコピー

    PyInstallerのonefileビルド（ytdlpSpout.spec）は python/*.dll を拾って
    exe自体に埋め込むが、python/ フォルダには開発中に残ったDebugビルドや
    テスト用DLL（fmtd.dll, gtest*.dll等）が混在し得るため、常に最新かつ
    Releaseビルドのみを含む cpp/build/bin/Release から明示的にコピーし、
    配布フォルダ直下（exeと同じ階層）にも配置しておく。
    """
    print("C++依存DLLを配布ディレクトリにコピー中...")

    release_dir = CPP_BUILD_DIR / "bin" / "Release"
    if not release_dir.exists():
        print(f"警告: C++ Releaseビルドディレクトリが見つかりません: {release_dir.resolve()}")
        return False

    dist_path = Path(DIST_DIR)
    dist_path.mkdir(parents=True, exist_ok=True)

    missing = []
    for dll_name in DIST_REQUIRED_DLLS:
        src = release_dir / dll_name
        if src.exists():
            shutil.copy2(src, dist_path / dll_name)
            print(f"DLLを配置: {dist_path / dll_name}")
        else:
            missing.append(dll_name)

    if missing:
        print(f"警告: 以下のDLLが {release_dir} に見つかりませんでした: {missing}")
        print("  C++ Releaseビルドが完了しているか確認してください。")

    return len(missing) == 0


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

def download_yt_dlp():
    """yt-dlp.exeをダウンロード"""
    print("yt-dlpをダウンロード中...")
    
    bin_dir = Path(DIST_DIR) / BIN_DIR
    bin_dir.mkdir(parents=True, exist_ok=True)
    
    yt_dlp_path = bin_dir / "yt-dlp.exe"
    
    try:
        urllib.request.urlretrieve(YT_DLP_URL, yt_dlp_path)
        print(f"yt-dlp.exeを配置: {yt_dlp_path}")
    except Exception as e:
        print(f"yt-dlpダウンロードエラー: {e}")
        raise

def build_exe():
    """PyInstallerでexeをビルド

    リポジトリ直下の ytdlpSpout.spec は、1つのspecファイルから
    GUI版（ytdlpSpoutGUI.exe）・CLI版（ytdlpSpoutCLI.exe）の両方を
    一括ビルドする（gui.pyを共通のエントリーポイントとして使用し、
    console=False/Trueの違いでexeを分ける）。
    """
    print("PyInstallerでexeをビルド中...")

    # 既存のdist, buildを削除
    for dir_name in ["dist", "build"]:
        if os.path.exists(dir_name):
            shutil.rmtree(dir_name)

    spec_path = Path("ytdlpSpout.spec")
    if not spec_path.exists():
        print(f"エラー: specファイルが見つかりません: {spec_path.resolve()}")
        return False

    print("GUI版・CLI版をビルド中...")
    cmd = [
        sys.executable, "-m", "PyInstaller",
        "--noconfirm",
        str(spec_path)
    ]

    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        print(f"PyInstallerエラー: {result.stderr}")
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
    
    # LICENSEファイルをコピー
    license_src = Path("LICENSE")
    if license_src.exists():
        shutil.copy2(license_src, dist_path / "LICENSE.txt")
        print(f"ライセンスファイルを配置: {dist_path / 'LICENSE.txt'}")
    else:
        print("警告: LICENSEファイルが見つかりません")

    # C++依存DLL（ytdlpspout.dll, ffmpeg, Spout等）をコピー
    copy_cpp_dependency_dlls()

    # READMEを作成
    readme_content = """# ytdlpSpout - YouTube to Spout Streamer

## 含まれるファイル
- ytdlpSpoutGUI.exe (GUI版アプリケーション)
- ytdlpSpoutCLI.exe (コマンドライン版)
- ytdlpspout.dll / avcodec-61.dll / avformat-61.dll / avutil-59.dll / swscale-8.dll /
  swresample-5.dll / libcurl.dll / spdlog.dll / fmt.dll / zlib1.dll / Spout.dll
  (C++ネイティブバックエンドとその依存ライブラリ。exeと同じフォルダに配置してください)
- bin/ffmpeg.exe (動画処理用)
- bin/yt-dlp.exe (動画ダウンロード用)
- bin/*.dll (ffmpeg依存ライブラリ)
- LICENSE.txt (MITライセンス)

## 特徴
- C++ ネイティブバックエンドによる高速な動画処理
- D3D11 ハードウェアアクセラレーションによる4K対応
- GPUベースのNV12→RGBA変換による低遅延処理
- スライスローディングによる効率的なストリーミング

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

## ライセンス
このソフトウェアはMITライセンスの下で配布されています。
詳細はLICENSE.txtファイルをご確認ください。

## システム要件
- Windows 10/11 64bit
- DirectX 11対応GPU（ハードウェアアクセラレーション用）
- Visual C++ Redistributable 2019以降

## 注意事項
- binフォルダ・DLLファイル・exeはすべて同じディレクトリに配置してください
- Spout対応アプリケーション（OBS Studio等）で受信できます
- yt-dlpはexeに埋め込まれているため、別途インストール不要です

## トラブルシューティング
- ffmpegやyt-dlpが見つからない場合は、binフォルダの配置を確認してください
- システムにffmpegやyt-dlpがインストールされている場合は、そちらが使用されることがあります
- 4K動画の再生が遅い場合は、GPU性能を確認してください

## 再配布について
このソフトウェアはMITライセンスの下で配布されており、自由に再配布できます。
再配布時は以下を遵守してください：
1. LICENSE.txtファイルを含めること
2. 著作権表示を保持すること
3. 改変した場合は改変内容を明記することを推奨します

## プロジェクト情報
- Original project: https://github.com/7MPra/ytdlpSpout
- License: MIT License
- ffmpeg: 外部バイナリとして同梱（GPL/LGPL、外部実行のため影響なし）
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
        # 0. C++ DLLをビルド
        if not build_cpp_dll(skip_if_exists=True):
            print("C++ DLLビルドに失敗しました")
            print("既存のDLLを使用するか、手動でビルドしてください")
            # DLLがなくてもPythonバックエンドで動作可能なので続行
        
        # 1. exeをビルド
        if not build_exe():
            print("exeビルドに失敗しました")
            return 1
        
        # 2. 配布ディレクトリを作成
        create_distribution()
        
        # 3. ffmpegをダウンロード・配置
        download_ffmpeg()
        
        # 4. yt-dlpをダウンロード・配置
        download_yt_dlp()
        
        # 5. ZIP圧縮
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