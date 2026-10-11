/*
SurfOS Undertow: the configuration of Mbed TLS 3.6.7
----------------------
File: tls_config.h  Date: 10/11/26
----------------------
The whole configuration (MBEDTLS_CONFIG_FILE) of every Mbed TLS translation
unit and of transport.c: nothing comes from the vendor's default
mbedtls_config.h. What Undertow needs is an HTTPS client and nothing more:

  TLS 1.2 and 1.3, client side only, with SNI and ALPN; ECDHE key exchange
  with X25519, P-256 and P-384 (transport.c offers these groups and no
  others); the AEAD ciphers ChaCha20-Poly1305 and AES-GCM; certificate
  chains with RSA (PKCS#1 v1.5 and PSS) and ECDSA signatures.

  P-521 and SHA-512 are here for certificates only: one root of the bundle
  has a P-521 key and some are signed with SHA-512. SHA-1 is here only so
  that the three roots signed with it can be read at all (a root's own
  signature is never checked); transport.c's certificate profile and the
  TLS signature algorithms accept SHA-256, SHA-384 and SHA-512, never SHA-1.

  Not here: the server side (the host tests add it, tests/host/undertow/
  tls_host_config.h), DTLS, renegotiation, session tickets and resumption,
  early data, PSK, CBC suites, RSA key exchange, finite-field DH; OS sockets,
  OS entropy, files, threading, assembly and SIMD (AES-NI, PadLock).

The platform hooks are in tls_platform.c: memory, snprintf, the strong
entropy of getentropy() (RDRAND), the UTC clock certificates are checked
against, the monotonic clock and gmtime_r. mbedtls_time_t is 64 bits.
*/

#ifndef UNDERTOW_TLS_CONFIG_H
#define UNDERTOW_TLS_CONFIG_H

/**** the platform: SurfOS's user space through tls_platform.c ****/
#define MBEDTLS_HAVE_TIME
#define MBEDTLS_HAVE_TIME_DATE
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_PLATFORM_MEMORY
#define MBEDTLS_PLATFORM_NO_STD_FUNCTIONS
#define MBEDTLS_PLATFORM_STD_MEM_HDR      "tls_platform.h"
#define MBEDTLS_PLATFORM_CALLOC_MACRO     ut_tls_calloc
#define MBEDTLS_PLATFORM_FREE_MACRO       ut_tls_free
#define MBEDTLS_PLATFORM_SNPRINTF_MACRO   ut_tls_snprintf
#define MBEDTLS_PLATFORM_VSNPRINTF_MACRO  ut_tls_vsnprintf
#define MBEDTLS_PLATFORM_TIME_TYPE_MACRO  long long
#define MBEDTLS_PLATFORM_TIME_MACRO       ut_tls_time
#define MBEDTLS_PLATFORM_MS_TIME_ALT
#define MBEDTLS_PLATFORM_GMTIME_R_ALT
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT
#define MBEDTLS_DEPRECATED_REMOVED

/**** symmetric cryptography and hashes ****/
#define MBEDTLS_AES_C
#define MBEDTLS_AES_ROM_TABLES
#define MBEDTLS_GCM_C
#define MBEDTLS_CHACHA20_C
#define MBEDTLS_POLY1305_C
#define MBEDTLS_CHACHAPOLY_C
#define MBEDTLS_CIPHER_C
#define MBEDTLS_MD_C
#define MBEDTLS_SHA1_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA384_C
#define MBEDTLS_SHA512_C
#define MBEDTLS_HKDF_C

/**** randomness: a CTR_DRBG seeded from the hardware (mbedtls_hardware_poll) ****/
#define MBEDTLS_ENTROPY_C
#define MBEDTLS_CTR_DRBG_C

/**** public keys ****/
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_ECP_C
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED
#define MBEDTLS_ECP_DP_SECP521R1_ENABLED
#define MBEDTLS_ECP_DP_CURVE25519_ENABLED
#define MBEDTLS_ECP_NIST_OPTIM
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_RSA_C
#define MBEDTLS_PKCS1_V15
#define MBEDTLS_PKCS1_V21
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_OID_C
#define MBEDTLS_BASE64_C
#define MBEDTLS_PEM_PARSE_C

/* PSA Crypto: TLS 1.3's key exchange and key schedule use it; its algorithms follow from the
   options above (config_adjust_psa_from_legacy.h), and TLS 1.2 keeps the classic API */
#define MBEDTLS_PSA_CRYPTO_C

/**** certificates ****/
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C
#define MBEDTLS_X509_RSASSA_PSS_SUPPORT
#define MBEDTLS_X509_REMOVE_INFO

/**** TLS: a client of TLS 1.2 and 1.3 ****/
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_SSL_PROTO_TLS1_3
#define MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_EPHEMERAL_ENABLED
#define MBEDTLS_SSL_TLS1_3_COMPATIBILITY_MODE
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
#define MBEDTLS_SSL_EXTENDED_MASTER_SECRET
#define MBEDTLS_SSL_KEEP_PEER_CERTIFICATE
#define MBEDTLS_SSL_SERVER_NAME_INDICATION
#define MBEDTLS_SSL_ALPN
#define MBEDTLS_SSL_ALL_ALERT_MESSAGES

/* ChaCha20-Poly1305 first: without AES instructions it is the faster cipher here, and its
   software is free of AES's table lookups */
#define MBEDTLS_SSL_CIPHERSUITES \
    MBEDTLS_TLS1_3_CHACHA20_POLY1305_SHA256, \
    MBEDTLS_TLS1_3_AES_128_GCM_SHA256, \
    MBEDTLS_TLS1_3_AES_256_GCM_SHA384, \
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256, \
    MBEDTLS_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256, \
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256, \
    MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256, \
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384, \
    MBEDTLS_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384

#endif
