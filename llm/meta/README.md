# llm/ ドキュメントディレクトリ

このディレクトリはytdlpSpoutプロジェクトの技術ドキュメントを格納しています。

## ディレクトリ構造

| ディレクトリ | 説明 |
|-------------|------|
| `architecture/` | システム全体のアーキテクチャ設計 |
| `specs/` | 機能別の技術仕様書 |
| `optimization/` | パフォーマンス最適化ドキュメント |
| `meta/` | メタ情報（本README等） |
| `logs/` | 開発ログ、決定記録 |
| `issues/` | 課題追跡 |

## ドキュメント一覧

### アーキテクチャ (`architecture/`)
- [slice-loading-architecture.md](../architecture/slice-loading-architecture.md) - スライス読み込みアーキテクチャの全体設計

### 技術仕様 (`specs/`)
- [cpp-dll-python-ffi.md](../specs/cpp-dll-python-ffi.md) - C++ DLL / Python FFI連携仕様
- [hls-m3u8-parser.md](../specs/hls-m3u8-parser.md) - HLS m3u8パーサー実装仕様
- [niconico-compatibility.md](../specs/niconico-compatibility.md) - ニコニコ動画対応仕様

### 最適化 (`optimization/`)
- [d3d11-graphics-optimization.md](../optimization/d3d11-graphics-optimization.md) - D3D11グラフィックス最適化

## 関連コード

各ドキュメントは以下のコードと対応しています：

| ドキュメント | 関連コード |
|-------------|-----------|
| slice-loading-architecture | `cpp/src/io/` |
| cpp-dll-python-ffi | `cpp/src/bindings/`, `python/ytdlpspout_native.py` |
| hls-m3u8-parser | `cpp/src/hls/` |
| niconico-compatibility | `ytdlpSpout/core.py` |
| d3d11-graphics-optimization | `cpp/src/graphics/` |
