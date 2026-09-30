idf_component_register(
    SRCS "xc_crypto.c" "xc_tlv8.c" "xc_selftest.c" "xc_bigint.c" "xc_srp.c" "xc_identity.c"
    INCLUDE_DIRS "include"
    REQUIRES mbedtls nvs_flash espressif__libsodium
)
