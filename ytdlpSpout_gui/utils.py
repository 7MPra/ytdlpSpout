"""GUI用ユーティリティ関数"""

from urllib.parse import parse_qs, urlencode, urlparse, urlunparse


def clean_playlist_url(url: str) -> str:
    """
    プレイリストURLから単体動画URLに変換
    プレイリストパラメータ（list, playlist）を除去
    """
    try:
        # URLをパース
        parsed = urlparse(url)
        query_params = parse_qs(parsed.query)
        
        # プレイリストパラメータを除去
        playlist_params = ['list', 'playlist', 'pl']
        for param in playlist_params:
            if param in query_params:
                del query_params[param]
        
        # パラメータを再構築
        new_query = urlencode(query_params, doseq=True)
        
        # URLを再構築
        new_parsed = parsed._replace(query=new_query)
        cleaned_url = urlunparse(new_parsed)
        
        # 元のURLと異なる場合はログ出力
        if cleaned_url != url:
            print(f"プレイリストURL変換: {url} -> {cleaned_url}")
        
        return cleaned_url
        
    except Exception as e:
        print(f"URL変換エラー: {e}")
        return url  # エラー時は元のURLをそのまま返す
