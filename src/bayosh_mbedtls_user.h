/* bayo.sh: mbedTLS settings for a PS5 payload (own sockets, hardware entropy, no files). */
#undef MBEDTLS_NET_C
#undef MBEDTLS_TIMING_C
#undef MBEDTLS_FS_IO
#undef MBEDTLS_PSA_ITS_FILE_C
#undef MBEDTLS_PSA_CRYPTO_STORAGE_C
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT
#undef MBEDTLS_SELF_TEST
#undef MBEDTLS_DEBUG_C
