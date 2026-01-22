// =============================================================================
// test_aes_decryptor.cpp - AES-128-CBC復号ユーティリティのユニットテスト
// =============================================================================

#include <iostream>
#include <cassert>
#include <cstring>
#include "hls/AesCbcDecryptor.h"
#include "utils/Logger.h"

using namespace ytdlpspout;
using namespace ytdlpspout::hls;

// =============================================================================
// テストユーティリティ
// =============================================================================

static int s_testsPassed = 0;
static int s_testsFailed = 0;

#define TEST_CASE(name) \
    std::cout << "  Testing: " << name << "..." << std::flush; \
    try {

#define TEST_END() \
        std::cout << " PASSED" << std::endl; \
        s_testsPassed++; \
    } catch (const std::exception& e) { \
        std::cout << " FAILED: " << e.what() << std::endl; \
        s_testsFailed++; \
    }

#define ASSERT_TRUE(expr) \
    if (!(expr)) throw std::runtime_error("Assertion failed: " #expr)

#define ASSERT_FALSE(expr) \
    if (expr) throw std::runtime_error("Assertion failed: !" #expr)

#define ASSERT_EQ(a, b) \
    if ((a) != (b)) throw std::runtime_error("Assertion failed: " #a " != " #b)

#define ASSERT_NE(a, b) \
    if ((a) == (b)) throw std::runtime_error("Assertion failed: " #a " == " #b)

// =============================================================================
// テスト: Initialize / Close
// =============================================================================

void TestInitializeClose()
{
    TEST_CASE("Initialize and Close")
    {
        AesCbcDecryptor decryptor;
        
        // 16バイトのキー
        std::vector<uint8_t> key(16, 0x00);
        key[0] = 0x2b; key[1] = 0x7e; key[2] = 0x15; key[3] = 0x16;
        key[4] = 0x28; key[5] = 0xae; key[6] = 0xd2; key[7] = 0xa6;
        key[8] = 0xab; key[9] = 0xf7; key[10] = 0x15; key[11] = 0x88;
        key[12] = 0x09; key[13] = 0xcf; key[14] = 0x4f; key[15] = 0x3c;
        
        ASSERT_TRUE(decryptor.Initialize(key));
        decryptor.Close();
    }
    TEST_END()
}

void TestInitializeInvalidKeyLength()
{
    TEST_CASE("Initialize with invalid key length")
    {
        AesCbcDecryptor decryptor;
        
        // 8バイトのキー（不正）
        std::vector<uint8_t> shortKey(8, 0x00);
        ASSERT_FALSE(decryptor.Initialize(shortKey));
        
        // 32バイトのキー（AES-256、今回はAES-128のみ対応）
        std::vector<uint8_t> longKey(32, 0x00);
        ASSERT_FALSE(decryptor.Initialize(longKey));
    }
    TEST_END()
}

// =============================================================================
// テスト: AES-128-CBC復号
// =============================================================================

void TestDecryptBasic()
{
    TEST_CASE("Basic AES-128-CBC decryption")
    {
        AesCbcDecryptor decryptor;
        
        // NIST AES-128-CBC テストベクター
        // Key: 2b7e151628aed2a6abf7158809cf4f3c
        // IV:  000102030405060708090a0b0c0d0e0f
        // Plaintext:  6bc1bee22e409f96e93d7e117393172a (16バイト)
        // Ciphertext: 7649abac8119b246cee98e9b12e9197d
        
        std::vector<uint8_t> key = {
            0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6,
            0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c
        };
        
        std::vector<uint8_t> iv = {
            0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
            0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f
        };
        
        // NIST テストベクター（パディング付き）
        // 平文 16バイト + PKCS7パディング 16バイト = 32バイト
        // Ciphertext: 7649abac8119b246cee98e9b12e9197d8964e0b149c10b7b682e6e39aaeb731c
        std::vector<uint8_t> ciphertext = {
            0x76, 0x49, 0xab, 0xac, 0x81, 0x19, 0xb2, 0x46,
            0xce, 0xe9, 0x8e, 0x9b, 0x12, 0xe9, 0x19, 0x7d,
            0x89, 0x64, 0xe0, 0xb1, 0x49, 0xc1, 0x0b, 0x7b,
            0x68, 0x2e, 0x6e, 0x39, 0xaa, 0xeb, 0x73, 0x1c
        };
        
        std::vector<uint8_t> expectedPlaintext = {
            0x6b, 0xc1, 0xbe, 0xe2, 0x2e, 0x40, 0x9f, 0x96,
            0xe9, 0x3d, 0x7e, 0x11, 0x73, 0x93, 0x17, 0x2a
        };
        
        ASSERT_TRUE(decryptor.Initialize(key));
        
        auto result = decryptor.Decrypt(ciphertext, iv);
        ASSERT_EQ(result.size(), expectedPlaintext.size());
        
        for (size_t i = 0; i < result.size(); ++i) {
            ASSERT_EQ(result[i], expectedPlaintext[i]);
        }
        
        decryptor.Close();
    }
    TEST_END()
}

void TestDecryptPkcs7Padding()
{
    TEST_CASE("PKCS7 padding removal")
    {
        AesCbcDecryptor decryptor;
        
        // 5バイトの平文 -> 11バイトパディング
        // 平文: "Hello" (0x48, 0x65, 0x6c, 0x6c, 0x6f)
        // パディング後: "Hello" + 11 * 0x0b
        // 暗号化後: (事前計算した暗号文)
        
        std::vector<uint8_t> key(16, 0x00);  // オールゼロキー
        std::vector<uint8_t> iv(16, 0x00);   // オールゼロIV
        
        // "Hello" + PKCS7(11) をAES-128-CBCで暗号化した結果
        // Ciphertext: 042dbe01027a650c746a5dc65db6be11
        std::vector<uint8_t> ciphertext = {
            0x04, 0x2d, 0xbe, 0x01, 0x02, 0x7a, 0x65, 0x0c,
            0x74, 0x6a, 0x5d, 0xc6, 0x5d, 0xb6, 0xbe, 0x11
        };
        
        std::vector<uint8_t> expectedPlaintext = {
            0x48, 0x65, 0x6c, 0x6c, 0x6f  // "Hello"
        };
        
        ASSERT_TRUE(decryptor.Initialize(key));
        
        auto result = decryptor.Decrypt(ciphertext, iv);
        ASSERT_EQ(result.size(), expectedPlaintext.size());
        
        for (size_t i = 0; i < result.size(); ++i) {
            ASSERT_EQ(result[i], expectedPlaintext[i]);
        }
        
        decryptor.Close();
    }
    TEST_END()
}

void TestDecryptInvalidIvLength()
{
    TEST_CASE("Decrypt with invalid IV length")
    {
        AesCbcDecryptor decryptor;
        std::vector<uint8_t> key(16, 0x00);
        std::vector<uint8_t> ciphertext(16, 0x00);
        std::vector<uint8_t> shortIv(8, 0x00);  // 不正なIV長
        
        ASSERT_TRUE(decryptor.Initialize(key));
        
        auto result = decryptor.Decrypt(ciphertext, shortIv);
        ASSERT_TRUE(result.empty());  // エラー時は空ベクター
        
        decryptor.Close();
    }
    TEST_END()
}

void TestDecryptEmptyCiphertext()
{
    TEST_CASE("Decrypt empty ciphertext")
    {
        AesCbcDecryptor decryptor;
        std::vector<uint8_t> key(16, 0x00);
        std::vector<uint8_t> iv(16, 0x00);
        std::vector<uint8_t> emptyCiphertext;
        
        ASSERT_TRUE(decryptor.Initialize(key));
        
        auto result = decryptor.Decrypt(emptyCiphertext, iv);
        ASSERT_TRUE(result.empty());
        
        decryptor.Close();
    }
    TEST_END()
}

void TestDecryptNonBlockAlignedCiphertext()
{
    TEST_CASE("Decrypt non-block-aligned ciphertext")
    {
        AesCbcDecryptor decryptor;
        std::vector<uint8_t> key(16, 0x00);
        std::vector<uint8_t> iv(16, 0x00);
        std::vector<uint8_t> invalidCiphertext(15, 0x00);  // 16の倍数でない
        
        ASSERT_TRUE(decryptor.Initialize(key));
        
        auto result = decryptor.Decrypt(invalidCiphertext, iv);
        ASSERT_TRUE(result.empty());  // エラー時は空ベクター
        
        decryptor.Close();
    }
    TEST_END()
}

// =============================================================================
// テスト: IV生成（HLS仕様）
// =============================================================================

void TestGenerateIvFromSequence()
{
    TEST_CASE("GenerateIvFromSequence basic")
    {
        // mediaSequence = 0 -> IV = all zeros
        auto iv0 = AesCbcDecryptor::GenerateIvFromSequence(0);
        ASSERT_EQ(iv0.size(), (size_t)16);
        for (size_t i = 0; i < 16; ++i) {
            ASSERT_EQ(iv0[i], 0);
        }
        
        // mediaSequence = 1 -> IV = [0,0,...,0,1]
        auto iv1 = AesCbcDecryptor::GenerateIvFromSequence(1);
        ASSERT_EQ(iv1.size(), (size_t)16);
        for (size_t i = 0; i < 15; ++i) {
            ASSERT_EQ(iv1[i], 0);
        }
        ASSERT_EQ(iv1[15], 1);
        
        // mediaSequence = 256 -> IV = [0,0,...,1,0]
        auto iv256 = AesCbcDecryptor::GenerateIvFromSequence(256);
        ASSERT_EQ(iv256.size(), (size_t)16);
        for (size_t i = 0; i < 14; ++i) {
            ASSERT_EQ(iv256[i], 0);
        }
        ASSERT_EQ(iv256[14], 1);
        ASSERT_EQ(iv256[15], 0);
    }
    TEST_END()
}

void TestGenerateIvFromSequenceLargeValue()
{
    TEST_CASE("GenerateIvFromSequence large value")
    {
        // mediaSequence = 0x0102030405060708
        int64_t seq = 0x0102030405060708LL;
        auto iv = AesCbcDecryptor::GenerateIvFromSequence(seq);
        
        ASSERT_EQ(iv.size(), (size_t)16);
        
        // ビッグエンディアンで16バイトに配置
        // 上位8バイトは0、下位8バイトがシーケンス番号
        for (size_t i = 0; i < 8; ++i) {
            ASSERT_EQ(iv[i], 0);
        }
        ASSERT_EQ(iv[8], 0x01);
        ASSERT_EQ(iv[9], 0x02);
        ASSERT_EQ(iv[10], 0x03);
        ASSERT_EQ(iv[11], 0x04);
        ASSERT_EQ(iv[12], 0x05);
        ASSERT_EQ(iv[13], 0x06);
        ASSERT_EQ(iv[14], 0x07);
        ASSERT_EQ(iv[15], 0x08);
    }
    TEST_END()
}

// =============================================================================
// テスト: 16進文字列のIV解析
// =============================================================================

void TestParseHexIvWithPrefix()
{
    TEST_CASE("ParseHexIv with 0x prefix")
    {
        // "0x"プレフィックス付き
        auto result = AesCbcDecryptor::ParseHexIv("0x00000000000000000000000000000001");
        ASSERT_TRUE(result.has_value());
        ASSERT_EQ(result->size(), (size_t)16);
        
        for (size_t i = 0; i < 15; ++i) {
            ASSERT_EQ((*result)[i], 0);
        }
        ASSERT_EQ((*result)[15], 1);
    }
    TEST_END()
}

void TestParseHexIvWithoutPrefix()
{
    TEST_CASE("ParseHexIv without 0x prefix")
    {
        // プレフィックスなし
        auto result = AesCbcDecryptor::ParseHexIv("000102030405060708090a0b0c0d0e0f");
        ASSERT_TRUE(result.has_value());
        ASSERT_EQ(result->size(), (size_t)16);
        
        for (size_t i = 0; i < 16; ++i) {
            ASSERT_EQ((*result)[i], (uint8_t)i);
        }
    }
    TEST_END()
}

void TestParseHexIvUppercase()
{
    TEST_CASE("ParseHexIv uppercase")
    {
        auto result = AesCbcDecryptor::ParseHexIv("0xABCDEF0123456789ABCDEF0123456789");
        ASSERT_TRUE(result.has_value());
        ASSERT_EQ(result->size(), (size_t)16);
        
        ASSERT_EQ((*result)[0], 0xAB);
        ASSERT_EQ((*result)[1], 0xCD);
        ASSERT_EQ((*result)[2], 0xEF);
    }
    TEST_END()
}

void TestParseHexIvInvalidLength()
{
    TEST_CASE("ParseHexIv invalid length")
    {
        // 短すぎる
        auto result1 = AesCbcDecryptor::ParseHexIv("0x01020304");
        ASSERT_FALSE(result1.has_value());
        
        // 長すぎる
        auto result2 = AesCbcDecryptor::ParseHexIv("0x0102030405060708090a0b0c0d0e0f1011");
        ASSERT_FALSE(result2.has_value());
    }
    TEST_END()
}

void TestParseHexIvInvalidCharacters()
{
    TEST_CASE("ParseHexIv invalid characters")
    {
        // 不正な文字
        auto result = AesCbcDecryptor::ParseHexIv("0x0000000000000000000000000000ZZZZ");
        ASSERT_FALSE(result.has_value());
    }
    TEST_END()
}

void TestParseHexIvEmpty()
{
    TEST_CASE("ParseHexIv empty string")
    {
        auto result = AesCbcDecryptor::ParseHexIv("");
        ASSERT_FALSE(result.has_value());
    }
    TEST_END()
}

// =============================================================================
// テスト: 連続復号（同一キーでの複数回復号）
// =============================================================================

void TestMultipleDecrypts()
{
    TEST_CASE("Multiple decrypts with same key")
    {
        AesCbcDecryptor decryptor;
        std::vector<uint8_t> key(16, 0x00);
        
        ASSERT_TRUE(decryptor.Initialize(key));
        
        // 複数回の復号が正しく動作することを確認
        std::vector<uint8_t> iv1(16, 0x00);
        std::vector<uint8_t> iv2(16, 0x01);
        
        // 同じ暗号文でも異なるIVで復号
        // "Hello" with zero key/IV: 042dbe01027a650c746a5dc65db6be11
        std::vector<uint8_t> ciphertext = {
            0x04, 0x2d, 0xbe, 0x01, 0x02, 0x7a, 0x65, 0x0c,
            0x74, 0x6a, 0x5d, 0xc6, 0x5d, 0xb6, 0xbe, 0x11
        };
        
        auto result1 = decryptor.Decrypt(ciphertext, iv1);
        ASSERT_FALSE(result1.empty());
        ASSERT_EQ(result1.size(), (size_t)5);  // "Hello" = 5 bytes
        
        // 異なるIVで復号するとパディングが変わるので、結果も異なる
        // ただしパディングエラーになる可能性があるのでemptyチェックはしない
        auto result2 = decryptor.Decrypt(ciphertext, iv2);
        // 異なるIVなので結果は異なる（パディングエラーで空になる可能性もある）
        ASSERT_TRUE(result1 != result2);
        
        decryptor.Close();
    }
    TEST_END()
}

// =============================================================================
// メイン
// =============================================================================

int main()
{
    // ロガー初期化（テスト用に最小限）
    Logger::Initialize(true, "logs/test_aes_decryptor.log", LogLevel::Warn);
    
    std::cout << "========================================" << std::endl;
    std::cout << "AES-128-CBC Decryptor Tests" << std::endl;
    std::cout << "========================================" << std::endl;
    
    // Initialize/Close
    std::cout << "\n[Initialize/Close Tests]" << std::endl;
    TestInitializeClose();
    TestInitializeInvalidKeyLength();
    
    // Decryption
    std::cout << "\n[Decryption Tests]" << std::endl;
    TestDecryptBasic();
    TestDecryptPkcs7Padding();
    TestDecryptInvalidIvLength();
    TestDecryptEmptyCiphertext();
    TestDecryptNonBlockAlignedCiphertext();
    
    // IV Generation
    std::cout << "\n[IV Generation Tests]" << std::endl;
    TestGenerateIvFromSequence();
    TestGenerateIvFromSequenceLargeValue();
    
    // Hex IV Parsing
    std::cout << "\n[Hex IV Parsing Tests]" << std::endl;
    TestParseHexIvWithPrefix();
    TestParseHexIvWithoutPrefix();
    TestParseHexIvUppercase();
    TestParseHexIvInvalidLength();
    TestParseHexIvInvalidCharacters();
    TestParseHexIvEmpty();
    
    // Multiple Decrypts
    std::cout << "\n[Multiple Decrypts Tests]" << std::endl;
    TestMultipleDecrypts();
    
    std::cout << "\n========================================" << std::endl;
    std::cout << "Results: " << s_testsPassed << " passed, " 
              << s_testsFailed << " failed" << std::endl;
    std::cout << "========================================" << std::endl;
    
    Logger::Shutdown();
    
    return s_testsFailed > 0 ? 1 : 0;
}
