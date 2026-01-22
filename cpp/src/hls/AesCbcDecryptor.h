// =============================================================================
// AesCbcDecryptor.h - HLSセグメント用AES-128-CBC復号ユーティリティ
// =============================================================================
//
// 機能:
//   - AES-128-CBC復号（Windows BCrypt API使用）
//   - PKCS7パディング除去
//   - HLS用IV生成（メディアシーケンス番号から）
//   - 16進文字列のIV解析
//
// 使用例:
//   AesCbcDecryptor decryptor;
//   if (decryptor.Initialize(keyData)) {
//       auto plaintext = decryptor.Decrypt(ciphertext, iv);
//       decryptor.Close();
//   }
//
// =============================================================================

#pragma once

#include <vector>
#include <string>
#include <optional>
#include <cstdint>

namespace ytdlpspout {
namespace hls {

/// @brief AES-128-CBC復号クラス（Windows BCrypt API使用）
class AesCbcDecryptor {
public:
    // =========================================================================
    // コンストラクタ / デストラクタ
    // =========================================================================
    
    AesCbcDecryptor() = default;
    ~AesCbcDecryptor();
    
    // コピー禁止
    AesCbcDecryptor(const AesCbcDecryptor&) = delete;
    AesCbcDecryptor& operator=(const AesCbcDecryptor&) = delete;
    
    // ムーブ許可
    AesCbcDecryptor(AesCbcDecryptor&& other) noexcept;
    AesCbcDecryptor& operator=(AesCbcDecryptor&& other) noexcept;

    // =========================================================================
    // 初期化 / クローズ
    // =========================================================================
    
    /// @brief 初期化
    /// @param key 16バイトのAES-128キー
    /// @return 成功時true
    bool Initialize(const std::vector<uint8_t>& key);
    
    /// @brief クローズ（リソース解放）
    void Close();
    
    /// @brief 初期化済みかどうか
    /// @return 初期化済みならtrue
    bool IsInitialized() const { return m_hKey != nullptr; }

    // =========================================================================
    // 復号
    // =========================================================================
    
    /// @brief AES-128-CBC復号
    /// @param ciphertext 暗号化データ（16バイトの倍数）
    /// @param iv 16バイトのIV
    /// @return 復号済みデータ（PKCS7パディング除去済み）、失敗時空ベクター
    std::vector<uint8_t> Decrypt(
        const std::vector<uint8_t>& ciphertext,
        const std::vector<uint8_t>& iv
    );

    // =========================================================================
    // 静的ユーティリティ（HLS用）
    // =========================================================================
    
    /// @brief HLS用: メディアシーケンス番号からIVを生成
    /// @param mediaSequence メディアシーケンス番号
    /// @return 16バイトのIV（ビッグエンディアン）
    static std::vector<uint8_t> GenerateIvFromSequence(int64_t mediaSequence);
    
    /// @brief 16進文字列からIVを解析（"0x"プレフィックス対応）
    /// @param hexStr 16進文字列（32文字、または"0x"プレフィックス付き34文字）
    /// @return 16バイトのIV、解析失敗時はnullopt
    static std::optional<std::vector<uint8_t>> ParseHexIv(const std::string& hexStr);

private:
    // =========================================================================
    // 内部ヘルパー
    // =========================================================================
    
    /// @brief PKCS7パディングを除去
    /// @param data パディング付きデータ
    /// @return パディング除去後のデータ、エラー時は空ベクター
    static std::vector<uint8_t> RemovePkcs7Padding(const std::vector<uint8_t>& data);
    
    /// @brief 16進文字を数値に変換
    /// @param c 16進文字（0-9, a-f, A-F）
    /// @return 数値（0-15）、無効な文字は-1
    static int HexCharToInt(char c);

    // =========================================================================
    // メンバ変数
    // =========================================================================
    
    void* m_hAlgorithm = nullptr;     ///< BCryptアルゴリズムハンドル
    void* m_hKey = nullptr;           ///< BCryptキーハンドル
    std::vector<uint8_t> m_keyObject; ///< キーオブジェクトバッファ
};

} // namespace hls
} // namespace ytdlpspout
