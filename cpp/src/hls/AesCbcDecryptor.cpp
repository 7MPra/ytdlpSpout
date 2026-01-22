// =============================================================================
// AesCbcDecryptor.cpp - HLSセグメント用AES-128-CBC復号ユーティリティ実装
// =============================================================================

#include "hls/AesCbcDecryptor.h"
#include "utils/Logger.h"

#include <windows.h>
#include <bcrypt.h>

#pragma comment(lib, "bcrypt.lib")

namespace ytdlpspout {
namespace hls {

// =============================================================================
// 定数
// =============================================================================

constexpr size_t AES_BLOCK_SIZE = 16;
constexpr size_t AES_128_KEY_SIZE = 16;

// =============================================================================
// デストラクタ / ムーブ
// =============================================================================

AesCbcDecryptor::~AesCbcDecryptor()
{
    Close();
}

AesCbcDecryptor::AesCbcDecryptor(AesCbcDecryptor&& other) noexcept
    : m_hAlgorithm(other.m_hAlgorithm)
    , m_hKey(other.m_hKey)
    , m_keyObject(std::move(other.m_keyObject))
{
    other.m_hAlgorithm = nullptr;
    other.m_hKey = nullptr;
}

AesCbcDecryptor& AesCbcDecryptor::operator=(AesCbcDecryptor&& other) noexcept
{
    if (this != &other) {
        Close();
        m_hAlgorithm = other.m_hAlgorithm;
        m_hKey = other.m_hKey;
        m_keyObject = std::move(other.m_keyObject);
        other.m_hAlgorithm = nullptr;
        other.m_hKey = nullptr;
    }
    return *this;
}

// =============================================================================
// 初期化 / クローズ
// =============================================================================

bool AesCbcDecryptor::Initialize(const std::vector<uint8_t>& key)
{
    // キー長チェック（AES-128のみ対応）
    if (key.size() != AES_128_KEY_SIZE) {
        LOG_ERROR("AesCbcDecryptor: Invalid key length: {} (expected {})", 
                  key.size(), AES_128_KEY_SIZE);
        return false;
    }

    // 既存のリソースをクリーンアップ
    Close();

    BCRYPT_ALG_HANDLE hAlgorithm = nullptr;
    BCRYPT_KEY_HANDLE hKey = nullptr;
    NTSTATUS status;

    // AESアルゴリズムプロバイダーを開く
    status = BCryptOpenAlgorithmProvider(
        &hAlgorithm,
        BCRYPT_AES_ALGORITHM,
        nullptr,
        0
    );
    if (!BCRYPT_SUCCESS(status)) {
        LOG_ERROR("AesCbcDecryptor: BCryptOpenAlgorithmProvider failed: 0x{:08x}", 
                  static_cast<unsigned int>(status));
        return false;
    }

    // CBCモードを設定
    status = BCryptSetProperty(
        hAlgorithm,
        BCRYPT_CHAINING_MODE,
        (PUCHAR)BCRYPT_CHAIN_MODE_CBC,
        sizeof(BCRYPT_CHAIN_MODE_CBC),
        0
    );
    if (!BCRYPT_SUCCESS(status)) {
        LOG_ERROR("AesCbcDecryptor: BCryptSetProperty (CBC mode) failed: 0x{:08x}", 
                  static_cast<unsigned int>(status));
        BCryptCloseAlgorithmProvider(hAlgorithm, 0);
        return false;
    }

    // キーオブジェクトのサイズを取得
    DWORD keyObjectSize = 0;
    DWORD resultSize = 0;
    status = BCryptGetProperty(
        hAlgorithm,
        BCRYPT_OBJECT_LENGTH,
        (PUCHAR)&keyObjectSize,
        sizeof(DWORD),
        &resultSize,
        0
    );
    if (!BCRYPT_SUCCESS(status)) {
        LOG_ERROR("AesCbcDecryptor: BCryptGetProperty (OBJECT_LENGTH) failed: 0x{:08x}", 
                  static_cast<unsigned int>(status));
        BCryptCloseAlgorithmProvider(hAlgorithm, 0);
        return false;
    }

    // キーオブジェクトバッファを確保
    m_keyObject.resize(keyObjectSize);

    // 対称キーを生成
    status = BCryptGenerateSymmetricKey(
        hAlgorithm,
        &hKey,
        m_keyObject.data(),
        static_cast<ULONG>(m_keyObject.size()),
        const_cast<PUCHAR>(key.data()),
        static_cast<ULONG>(key.size()),
        0
    );
    if (!BCRYPT_SUCCESS(status)) {
        LOG_ERROR("AesCbcDecryptor: BCryptGenerateSymmetricKey failed: 0x{:08x}", 
                  static_cast<unsigned int>(status));
        BCryptCloseAlgorithmProvider(hAlgorithm, 0);
        m_keyObject.clear();
        return false;
    }

    m_hAlgorithm = hAlgorithm;
    m_hKey = hKey;

    LOG_DEBUG("AesCbcDecryptor: Initialized successfully");
    return true;
}

void AesCbcDecryptor::Close()
{
    if (m_hKey != nullptr) {
        BCryptDestroyKey(static_cast<BCRYPT_KEY_HANDLE>(m_hKey));
        m_hKey = nullptr;
    }

    if (m_hAlgorithm != nullptr) {
        BCryptCloseAlgorithmProvider(static_cast<BCRYPT_ALG_HANDLE>(m_hAlgorithm), 0);
        m_hAlgorithm = nullptr;
    }

    m_keyObject.clear();
}

// =============================================================================
// 復号
// =============================================================================

std::vector<uint8_t> AesCbcDecryptor::Decrypt(
    const std::vector<uint8_t>& ciphertext,
    const std::vector<uint8_t>& iv)
{
    // 初期化チェック
    if (!IsInitialized()) {
        LOG_ERROR("AesCbcDecryptor: Not initialized");
        return {};
    }

    // IV長チェック
    if (iv.size() != AES_BLOCK_SIZE) {
        LOG_ERROR("AesCbcDecryptor: Invalid IV length: {} (expected {})", 
                  iv.size(), AES_BLOCK_SIZE);
        return {};
    }

    // 空の暗号文チェック
    if (ciphertext.empty()) {
        LOG_WARN("AesCbcDecryptor: Empty ciphertext");
        return {};
    }

    // ブロックアライメントチェック
    if (ciphertext.size() % AES_BLOCK_SIZE != 0) {
        LOG_ERROR("AesCbcDecryptor: Ciphertext size {} is not a multiple of block size {}", 
                  ciphertext.size(), AES_BLOCK_SIZE);
        return {};
    }

    // IVのコピー（BCryptDecryptがIVを変更する可能性があるため）
    std::vector<uint8_t> ivCopy = iv;

    // 出力バッファサイズを取得
    ULONG decryptedSize = 0;
    NTSTATUS status = BCryptDecrypt(
        static_cast<BCRYPT_KEY_HANDLE>(m_hKey),
        const_cast<PUCHAR>(ciphertext.data()),
        static_cast<ULONG>(ciphertext.size()),
        nullptr,
        ivCopy.data(),
        static_cast<ULONG>(ivCopy.size()),
        nullptr,
        0,
        &decryptedSize,
        0
    );
    if (!BCRYPT_SUCCESS(status)) {
        LOG_ERROR("AesCbcDecryptor: BCryptDecrypt (size query) failed: 0x{:08x}", 
                  static_cast<unsigned int>(status));
        return {};
    }

    // 復号バッファを確保
    std::vector<uint8_t> decrypted(decryptedSize);

    // IVを再コピー（BCryptDecryptが変更する可能性があるため）
    ivCopy = iv;

    // 復号実行
    status = BCryptDecrypt(
        static_cast<BCRYPT_KEY_HANDLE>(m_hKey),
        const_cast<PUCHAR>(ciphertext.data()),
        static_cast<ULONG>(ciphertext.size()),
        nullptr,
        ivCopy.data(),
        static_cast<ULONG>(ivCopy.size()),
        decrypted.data(),
        static_cast<ULONG>(decrypted.size()),
        &decryptedSize,
        0
    );
    if (!BCRYPT_SUCCESS(status)) {
        LOG_ERROR("AesCbcDecryptor: BCryptDecrypt failed: 0x{:08x}", 
                  static_cast<unsigned int>(status));
        return {};
    }

    decrypted.resize(decryptedSize);

    // PKCS7パディングを除去
    return RemovePkcs7Padding(decrypted);
}

// =============================================================================
// 静的ユーティリティ
// =============================================================================

std::vector<uint8_t> AesCbcDecryptor::GenerateIvFromSequence(int64_t mediaSequence)
{
    // HLS仕様: IVはメディアシーケンス番号をビッグエンディアン16バイトに変換
    std::vector<uint8_t> iv(16, 0);
    
    // ビッグエンディアンで配置（上位8バイトは0、下位8バイトにシーケンス番号）
    uint64_t seq = static_cast<uint64_t>(mediaSequence);
    iv[8]  = static_cast<uint8_t>((seq >> 56) & 0xFF);
    iv[9]  = static_cast<uint8_t>((seq >> 48) & 0xFF);
    iv[10] = static_cast<uint8_t>((seq >> 40) & 0xFF);
    iv[11] = static_cast<uint8_t>((seq >> 32) & 0xFF);
    iv[12] = static_cast<uint8_t>((seq >> 24) & 0xFF);
    iv[13] = static_cast<uint8_t>((seq >> 16) & 0xFF);
    iv[14] = static_cast<uint8_t>((seq >> 8) & 0xFF);
    iv[15] = static_cast<uint8_t>(seq & 0xFF);
    
    return iv;
}

std::optional<std::vector<uint8_t>> AesCbcDecryptor::ParseHexIv(const std::string& hexStr)
{
    if (hexStr.empty()) {
        return std::nullopt;
    }

    std::string hex = hexStr;
    
    // "0x"プレフィックスを除去
    if (hex.size() >= 2 && hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')) {
        hex = hex.substr(2);
    }

    // 長さチェック（16バイト = 32文字）
    if (hex.size() != 32) {
        LOG_WARN("AesCbcDecryptor: Invalid hex IV length: {} (expected 32)", hex.size());
        return std::nullopt;
    }

    std::vector<uint8_t> iv;
    iv.reserve(16);

    for (size_t i = 0; i < 32; i += 2) {
        int high = HexCharToInt(hex[i]);
        int low = HexCharToInt(hex[i + 1]);
        
        if (high < 0 || low < 0) {
            LOG_WARN("AesCbcDecryptor: Invalid hex character in IV");
            return std::nullopt;
        }
        
        iv.push_back(static_cast<uint8_t>((high << 4) | low));
    }

    return iv;
}

// =============================================================================
// 内部ヘルパー
// =============================================================================

std::vector<uint8_t> AesCbcDecryptor::RemovePkcs7Padding(const std::vector<uint8_t>& data)
{
    if (data.empty()) {
        return {};
    }

    // 最後のバイトがパディング長
    uint8_t paddingLen = data.back();
    
    // パディング長の妥当性チェック
    if (paddingLen == 0 || paddingLen > AES_BLOCK_SIZE || paddingLen > data.size()) {
        LOG_WARN("AesCbcDecryptor: Invalid PKCS7 padding length: {}", paddingLen);
        return {};
    }

    // パディングバイトの検証
    for (size_t i = data.size() - paddingLen; i < data.size(); ++i) {
        if (data[i] != paddingLen) {
            LOG_WARN("AesCbcDecryptor: Invalid PKCS7 padding byte at position {}", i);
            return {};
        }
    }

    // パディングを除去
    return std::vector<uint8_t>(data.begin(), data.end() - paddingLen);
}

int AesCbcDecryptor::HexCharToInt(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return 10 + (c - 'a');
    }
    if (c >= 'A' && c <= 'F') {
        return 10 + (c - 'A');
    }
    return -1;
}

} // namespace hls
} // namespace ytdlpspout
