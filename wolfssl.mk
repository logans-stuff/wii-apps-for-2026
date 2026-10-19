# wolfSSL sources that the app needs. Shared by both Makefiles.
WOLF := third_party/wolfssl
WOLF_SRC := $(addprefix $(WOLF)/src/, internal.c keys.c ssl.c tls.c tls13.c wolfio.c) \
  $(addprefix $(WOLF)/wolfcrypt/src/, aes.c asn.c coding.c chacha.c chacha20_poly1305.c \
    curve25519.c ecc.c error.c fe_operations.c hash.c hmac.c kdf.c logging.c memory.c \
    poly1305.c random.c rsa.c sha.c sha256.c sha512.c signature.c sp_int.c sp_c32.c sp_c64.c wc_encrypt.c \
    wc_port.c wolfmath.c md5.c)
CJSON_SRC := third_party/cJSON/cJSON.c
WOLF_CFLAGS := -DWOLFSSL_USER_SETTINGS -Iinclude -I$(WOLF) -Ithird_party/cJSON
