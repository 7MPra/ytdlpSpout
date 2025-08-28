# ytdlpSpout for macOS (Syphon対応版)

このバージョンは、Windows版のSpoutの代わりにmacOSで利用可能なSyphonフレームワークを使用して、YouTubeの動画ストリームをリアルタイムで他のアプリケーションに送信します。

## 動作要件

- macOS
- Python 3.x
- pip (Pythonのパッケージ管理ツール)
- ffmpeg (システムPATHが通っているか、ビルドスクリプトでダウンロードされます)
- Syphon対応アプリケーション (例: Resolume, VDMX, MadMapperなど)

## インストールと実行

1. **リポジトリのクローン**
   ```bash
   git clone https://github.com/7MPra/ytdlpSpout.git
   cd ytdlpSpout
   ```

2. **依存関係のインストール**
   ```bash
   pip3 install -r requirements.txt
   pip3 install syphon-python
   ```

3. **ビルドスクリプトの実行**
   `build_mac.sh`スクリプトを実行して、実行可能ファイルをビルドします。このスクリプトはPyInstallerとffmpegを自動的にセットアップします。
   ```bash
   chmod +x build_mac.sh
   ./build_mac.sh
   ```
   ビルドが成功すると、`dist/ytdlpSpout-mac`に実行可能ファイルが生成されます。

4. **実行**
   ```bash
   ./dist/ytdlpSpout-mac "[YouTube動画のURL]" --sender "[Syphon送信者名]"
   ```
   例:
   ```bash
   ./dist/ytdlpSpout-mac "https://www.youtube.com/watch?v=dQw4w9WgXcQ" --sender "MySyphonStream"
   ```

## 使用方法

基本的な使用方法はCLI版と同様です。詳細は[CLI版のREADME](README.md)を参照してください。

Syphon送信者名は、Syphon対応アプリケーションで表示されるストリームの名前になります。

## 開発者向け

`main.py`と`ytdlpSpout/core.py`がSyphon対応のために変更されています。主な変更点は以下の通りです。

- `SpoutGL`の代わりに`syphon`ライブラリを使用。
- `Streamer`クラス内でSyphonサーバーの初期化とフレーム送信ロジックを実装。
- `build_mac.sh`スクリプトはmacOS環境でのビルドと依存関係の管理を自動化します。

## 貢献

バグ報告や機能提案は、GitHubのIssuesまでお願いします。プルリクエストも歓迎します。


