# 可插拔加密驱动设计

csilk 允许开发人员替换其内部的加密和唯一标识符算法。这对于利用硬件加速加密、与系统级库集成或使用本地化算法（如 SM 系列）很有用。加密驱动 **MUST** 实现 `csilk_crypto_driver_t` 中的所有必需字段 — 部分实现 **MUST** 在注册时被拒绝。驱动查找 **MUST** 通过固定大小注册表哈希表为 O(1)。SHA-256 哈希 **SHOULD** 在 ≤ 1µs 内完成，输入 ≤ 256 字节。UUID v4 生成 **MUST** 使用加密安全随机数生成器 (CSPRNG)。

加密子系统有两个独立的可插拔接口：

| 接口 | 目的 | 头文件 |
|------|------|--------|
| `csilk_crypto_driver_t` | 哈希、HMAC、UUID 原语 | `csilk.h` |
| `csilk_cipher_driver_t` | 对称/非对称加密、签名 | `csilk/drivers/cipher.h` |

---

## 1. 加密原语驱动 (`csilk_crypto_driver_t`)

### 接口定义

在 `csilk.h` 定义：

```c
typedef struct {
  void (*sha256)(const uint8_t* data, size_t len, uint8_t out[32]);
  void (*hmac_sha256)(const uint8_t* key, size_t key_len, const uint8_t* data,
                     size_t data_len, uint8_t out[32]);
  void (*generate_uuid)(char buf[37]);
} csilk_crypto_driver_t;
```

### 集成生命周期

1. **初始化**: 创建 `csilk_crypto_driver_t` 的静态或分配实例。
2. **注册**: 在启动服务器之前调用 `csilk_server_set_crypto_driver(server, &my_driver)`。
3. **传播**: 服务器自动将驱动附加到每个 `csilk_ctx_t`。
4. **执行**: 中间件（如 JWT 和 Session）使用上下文中提供的驱动。

### 使用示例

```c
#include <openssl/hmac.h>
#include "csilk/csilk.h"

void openssl_hmac(const uint8_t* key, size_t key_len, const uint8_t* data,
                  size_t data_len, uint8_t out[32]) {
    unsigned int len = 32;
    HMAC(EVP_sha256(), key, key_len, data, data_len, out, &len);
}

static csilk_crypto_driver_t openssl_driver = {
    .hmac_sha256 = openssl_hmac,
};

int main() {
    csilk_server_t* server = csilk_server_new(router);
    csilk_server_set_crypto_driver(server, &openssl_driver);
    csilk_server_run(server, 8080);
}
```

---

## 2. 加密驱动 (`csilk_cipher_driver_t`)

### 接口定义

在 `csilk/drivers/cipher.h` 定义：

```c
#define CSILK_AES256_KEY_SIZE   32
#define CSILK_GCM_IV_SIZE       12
#define CSILK_GCM_TAG_SIZE      16
#define CSILK_RSA_KEY_SIZE      256
#define CSILK_RSA_SIGNATURE_SIZE 256

typedef struct {
    int (*symmetric_encrypt)(const uint8_t* key, size_t key_len,
                            const uint8_t* plaintext, size_t plaintext_len,
                            const uint8_t* iv, size_t iv_len,
                            uint8_t* ciphertext, size_t* ciphertext_len,
                            uint8_t* tag, size_t tag_len);
    int (*symmetric_decrypt)(const uint8_t* key, size_t key_len,
                            const uint8_t* ciphertext, size_t ciphertext_len,
                            const uint8_t* iv, size_t iv_len,
                            const uint8_t* tag, size_t tag_len,
                            uint8_t* plaintext, size_t* plaintext_len);
    int (*generate_keypair)(char* public_key, size_t* pub_len,
                           char* private_key, size_t* priv_len);
    int (*asymmetric_encrypt)(const char* public_key, size_t pub_len,
                             const uint8_t* plaintext, size_t plaintext_len,
                             uint8_t* ciphertext, size_t* ciphertext_len);
    int (*asymmetric_decrypt)(const char* private_key, size_t priv_len,
                             const uint8_t* ciphertext, size_t ciphertext_len,
                             uint8_t* plaintext, size_t* plaintext_len);
    int (*sign)(const char* private_key, size_t priv_len,
               const uint8_t* data, size_t data_len,
               uint8_t* signature, size_t* sig_len);
    int (*verify)(const char* public_key, size_t pub_len,
                 const uint8_t* data, size_t data_len,
                 const uint8_t* signature, size_t sig_len);
} csilk_cipher_driver_t;
```

### 算法

| 操作 | 算法 | 参数 |
|------|------|------|
| **对称加密/解密** | AES-256-GCM | 32 字节密钥，12 字节 IV，16 字节标签 |
| **密钥生成** | RSA-2048 | 输出 PEM 编码的密钥对 |
| **非对称加密/解密** | RSA-OAEP (SHA-256 MGF1) | 最大明文约 190 字节 |
| **签名/验证** | RSA-PSS (SHA-256) | 256 字节签名 |

### 集成生命周期

与加密原语驱动相同：

1. **初始化**: 创建具有填充函数指针的 `csilk_cipher_driver_t` 实例。
2. **注册**: 调用 `csilk_server_set_cipher_driver(server, &my_driver)`。传递 NULL 以恢复默认。
3. **传播**: 服务器在连接接受时将驱动复制到每个 `csilk_ctx_t`。
4. **执行**: 使用内部包装器（`_csilk_symmetric_encrypt` 等），它们委托给驱动或回退到默认。

### 使用示例

```c
#include "csilk/csilk.h"
#include "csilk/core/internal.h"  // umbrella → includes crypto_dispatch.h

void my_handler(csilk_ctx_t* c) {
    uint8_t key[32], iv[12];
    memset(key, 0x2A, 32);
    memset(iv, 0x3B, 12);
    const uint8_t pt[] = "secret data";
    uint8_t ct[64], tag[16], dec[64];
    size_t ct_len = sizeof(ct), dec_len = sizeof(dec);

    // 加密（使用默认 AES-256-GCM 或自定义驱动如果已设置）
    _csilk_symmetric_encrypt(c, key, sizeof(key), pt, strlen((char*)pt),
                             iv, sizeof(iv), ct, &ct_len, tag, sizeof(tag));

    // 解密
    _csilk_symmetric_decrypt(c, key, sizeof(key), ct, ct_len,
                             iv, sizeof(iv), tag, sizeof(tag), dec, &dec_len);
}
```

### 自定义驱动示例

```c
static int my_aes_encrypt(const uint8_t* key, size_t key_len,
                          const uint8_t* pt, size_t pt_len,
                          const uint8_t* iv, size_t iv_len,
                          uint8_t* ct, size_t* ct_len,
                          uint8_t* tag, size_t tag_len) {
    // 自定义 AES-256-GCM 实现（硬件加速等）
    return my_hw_aes_gcm_encrypt(key, key_len, pt, pt_len, iv, iv_len, ct, ct_len, tag);
}

static csilk_cipher_driver_t hw_driver = {
    .symmetric_encrypt = my_aes_encrypt,
    .symmetric_decrypt = my_aes_decrypt,
    // NULL 条目回退到默认 OpenSSL 实现
};

int main() {
    csilk_server_set_cipher_driver(server, &hw_driver);
}
```

### 默认实现

内置默认实现（`csilk_default_cipher_driver` 在 `src/crypto/cipher.c`）使用：

- **OpenSSL EVP API** 进行所有算法
- **AES-256-GCM** 通过 `EVP_aes_256_gcm()`
- **RSA-2048** 密钥生成通过 `EVP_PKEY_keygen()`
- **RSA-OAEP** 通过 `EVP_PKEY_encrypt_init()` 带 `RSA_PKCS1_OAEP_PADDING`
- **RSA-PSS** 通过 `EVP_DigestSign()` 带 `RSA_PKCS1_PSS_PADDING`

### 内部委托机制

`crypto_dispatch.h` 中的内部包装器（通过 `internal.h` 包含，通过 `utils.c` 实现）遵循与加密原语驱动相同的模式：

```c
int _csilk_symmetric_encrypt(csilk_ctx_t* c, ...) {
    csilk_cipher_driver_t* d = resolve_cipher(c);
    if (d && d->symmetric_encrypt) return d->symmetric_encrypt(...);
    return -1;
}
```

`resolve_cipher()` 辅助函数返回上下文的加密驱动（如果已设置），否则返回内置的 `csilk_default_cipher_driver`。