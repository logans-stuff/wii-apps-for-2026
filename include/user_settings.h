/* wolfSSL settings for the Sonoran Wii app.
 * The app is a TLS 1.3 client only. It talks to Cloudflare. */
#ifndef SONORAN_USER_SETTINGS_H
#define SONORAN_USER_SETTINGS_H

#ifdef __BIG_ENDIAN__
    #define BIG_ENDIAN_ORDER
#endif

/* Platform */
#define SINGLE_THREADED
#define NO_FILESYSTEM
#define NO_WRITEV
#define NO_DEV_RANDOM
#define WOLFSSL_USER_IO
#define WOLFSSL_NO_SOCK
#define WOLFSSL_IGNORE_FILE_WARN

/* Seed from the app (see source/seed.c). */
extern int sonoran_seed(unsigned char *out, unsigned int sz);
#define CUSTOM_RAND_GENERATE_SEED sonoran_seed

/* Protocol */
#define WOLFSSL_TLS13
#define NO_OLD_TLS
#define HAVE_TLS_EXTENSIONS
#define HAVE_SUPPORTED_CURVES
#define HAVE_SNI
#define HAVE_HKDF
#define HAVE_AEAD
#define NO_DH
#define NO_PSK
#define NO_SESSION_CACHE
#define WOLFSSL_NO_TLS12_ON_SERVER

/* Cipher suites: AES-GCM and ChaCha20-Poly1305 */
#define HAVE_AESGCM
#define GCM_TABLE_4BIT
#define WOLFSSL_AES_128
#define WOLFSSL_AES_256
#define HAVE_CHACHA
#define HAVE_POLY1305
#define WOLFSSL_SHA384
#define WOLFSSL_SHA512

/* Key exchange and signatures */
#define HAVE_ECC
#define ECC_TIMING_RESISTANT
#define HAVE_ECC384
#define HAVE_ECC521
#define ECC_SHAMIR
#define HAVE_CURVE25519
#define WC_RSA_PSS
#define WC_RSA_BLINDING
#define WOLFSSL_KEY_GEN      /* RSA key for the Discord QR login */
#define WOLFSSL_SP_MATH_ALL
#define WOLFSSL_HAVE_SP_RSA
#define WOLFSSL_HAVE_SP_ECC
#define SP_INT_BITS 4096
#define TFM_TIMING_RESISTANT

/* Certificates */
#define WOLFSSL_BASE64_ENCODE
#define WOLFSSL_ALT_CERT_CHAINS

/* Off */
#define NO_DSA
#define NO_RC4
#define NO_MD4
#define NO_DES3
#define NO_PWDBASED
#define NO_OLD_RNGNAME
#define WOLFSSL_NO_SHAKE128
#define WOLFSSL_NO_SHAKE256

#endif
