/*
 * Fixed-size (3072-bit / 384-byte) modular arithmetic for SRP-6a pairing.
 *
 * Delegates to mbedTLS's bignum, which on the ESP32-P4 uses the hardware MPI
 * (RSA accelerator, CONFIG_MBEDTLS_HARDWARE_MPI), so a 3072-bit modexp is fast
 * enough to answer an iPhone pair-setup within its timeout.
 */
#include "xc_bigint.h"

#include <string.h>

#include "esp_log.h"

#define MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS
#include "mbedtls/private/bignum.h"

esp_err_t xc_modexp(const uint8_t *base, size_t base_len,
                    const uint8_t *exp, size_t exp_len,
                    const uint8_t mod[XC_BN_BYTES],
                    uint8_t out[XC_BN_BYTES])
{
    if (base == NULL || exp == NULL || mod == NULL || out == NULL || base_len == 0 || exp_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    mbedtls_mpi A, E, N, R;
    mbedtls_mpi_init(&A);
    mbedtls_mpi_init(&E);
    mbedtls_mpi_init(&N);
    mbedtls_mpi_init(&R);

    int rc = mbedtls_mpi_read_binary(&A, base, base_len);
    if (rc == 0) rc = mbedtls_mpi_read_binary(&E, exp, exp_len);
    if (rc == 0) rc = mbedtls_mpi_read_binary(&N, mod, XC_BN_BYTES);
    if (rc == 0) rc = mbedtls_mpi_exp_mod(&R, &A, &E, &N, NULL);
    if (rc == 0) rc = mbedtls_mpi_write_binary(&R, out, XC_BN_BYTES);

    mbedtls_mpi_free(&A);
    mbedtls_mpi_free(&E);
    mbedtls_mpi_free(&N);
    mbedtls_mpi_free(&R);
    if (rc != 0) {
        ESP_LOGE("xc_bigint", "modexp failed rc=%d", rc);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t binop(const uint8_t a[XC_BN_BYTES], const uint8_t b[XC_BN_BYTES],
                       const uint8_t mod[XC_BN_BYTES], uint8_t out[XC_BN_BYTES], int op)
{
    mbedtls_mpi A, B, N, R;
    mbedtls_mpi_init(&A);
    mbedtls_mpi_init(&B);
    mbedtls_mpi_init(&N);
    mbedtls_mpi_init(&R);

    int rc = mbedtls_mpi_read_binary(&A, a, XC_BN_BYTES);
    if (rc == 0) rc = mbedtls_mpi_read_binary(&B, b, XC_BN_BYTES);
    if (rc == 0) rc = mbedtls_mpi_read_binary(&N, mod, XC_BN_BYTES);
    if (rc == 0) rc = (op == 0) ? mbedtls_mpi_mul_mpi(&R, &A, &B) : mbedtls_mpi_add_mpi(&R, &A, &B);
    if (rc == 0) rc = mbedtls_mpi_mod_mpi(&R, &R, &N);
    if (rc == 0) rc = mbedtls_mpi_write_binary(&R, out, XC_BN_BYTES);

    mbedtls_mpi_free(&A);
    mbedtls_mpi_free(&B);
    mbedtls_mpi_free(&N);
    mbedtls_mpi_free(&R);
    return rc == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t xc_modmul(const uint8_t a[XC_BN_BYTES], const uint8_t b[XC_BN_BYTES],
                    const uint8_t mod[XC_BN_BYTES], uint8_t out[XC_BN_BYTES])
{
    return binop(a, b, mod, out, 0);
}

esp_err_t xc_modadd(const uint8_t a[XC_BN_BYTES], const uint8_t b[XC_BN_BYTES],
                    const uint8_t mod[XC_BN_BYTES], uint8_t out[XC_BN_BYTES])
{
    return binop(a, b, mod, out, 1);
}

/* ---- self test -------------------------------------------------------- */

static const char *SRP_N_HEX =
    "FFFFFFFFFFFFFFFFC90FDAA22168C234C4C6628B80DC1CD129024E088A67CC74"
    "020BBEA63B139B22514A08798E3404DDEF9519B3CD3A431B302B0A6DF25F1437"
    "4FE1356D6D51C245E485B576625E7EC6F44C42E9A637ED6B0BFF5CB6F406B7ED"
    "EE386BFB5A899FA5AE9F24117C4B1FE649286651ECE45B3DC2007CB8A163BF05"
    "98DA48361C55D39A69163FA8FD24CF5F83655D23DCA3AD961C62F356208552BB"
    "9ED529077096966D670C354E4ABC9804F1746C08CA18217C32905E462E36CE3B"
    "E39E772C180E86039B2783A2EC07A28FB5C55DF06F4C52C9DE2BCBF6955817183"
    "995497CEA956AE515D2261898FA051015728E5A8AAAC42DAD33170D04507A33A"
    "85521ABDF1CBA64ECFB850458DBEF0A8AEA71575D060C7DB3970F85A6E1E4C7AB"
    "F5AE8CDB0933D71E8C94E04A25619DCEE3D2261AD2EE6BF12FFA06D98A0864D8"
    "7602733EC86A64521F2B18177B200CBBE117577A615D6C770988C0BAD946E208"
    "E24FA074E5AB3143DB5BFCE0FD108E4B82D120A93AD2CAFFFFFFFFFFFFFFFF";

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

static void hex_to_bytes(const char *hex, uint8_t *out, size_t out_len)
{
    size_t n = strlen(hex) / 2;
    if (n > out_len) {
        n = out_len;
    }
    for (size_t i = 0; i < n; i++) {
        out[i] = (uint8_t)((hexval(hex[i * 2]) << 4) | hexval(hex[i * 2 + 1]));
    }
}

static bool hex_eq(const uint8_t *got, const char *want_hex)
{
    static const char *d = "0123456789abcdef";
    for (size_t i = 0; want_hex[i]; i += 2) {
        uint8_t b = (uint8_t)((hexval(want_hex[i]) << 4) | hexval(want_hex[i + 1]));
        if (d[got[i / 2] >> 4] != want_hex[i] || d[got[i / 2] & 0xf] != want_hex[i + 1]) {
            (void)b;
            return false;
        }
    }
    return true;
}

bool xc_bigint_selftest(void)
{
    uint8_t n[XC_BN_BYTES];
    hex_to_bytes(SRP_N_HEX, n, sizeof(n));
    uint8_t g[1] = { 5 };

    uint8_t x[32];
    for (int i = 0; i < 32; i++) x[i] = (uint8_t)i;
    uint8_t out[XC_BN_BYTES];
    if (xc_modexp(g, 1, x, sizeof(x), n, out) != ESP_OK) return false;
    if (!hex_eq(out,
                "203e5bc55133e8d2917aac4b7d78b82c725d9085efc46dd33c449ebfb09816be"
                "e2ae6e218c9873a99a1e01bd0e7ca47edca05889319bfebb30bdea7701cba20c"
                "6419a59b7c6ee0f80c59fcb9ec9e43da57885775e6d343288c7bdca766361e51"
                "1c8d7f1ac4c521c2fbdd22e145fb83fcf0722949ab5520de4963661c5d27658a"
                "010cb50514a6d8ff10a8448117545c4a9cd12002b1bf0f81775f0426ad609d2e"
                "aaee21736fc6d766a9e8977a4b467c1a67a4de79e6c185b555028633cd028b39"
                "17fc6f0d8e07871b2d531ea6e4f9f5bbed5cf6aaee6f1f429126c6bac9644df2"
                "64bf61fcf5a8c5a42c99d42f1bf87c89488649991be2bcdce6c1d87a6a463a79"
                "98150a20b33e7fbbcaa366e8a77c5529421dadf8132ef97e09d30eadae3149b8"
                "f15025354b7c6148fbd754c9ca6caead032f4e322fc93049805f5ea415bf535d"
                "eab62f603340cc2d9bd99874c9bdf2c6a07d1963bfb79ee527fc252db63696b0"
                "0ec434bdc69e9158ba02ac9dbe0ff85e5263c57f178dd4d529eaa23940ed2e47")) {
        return false;
    }

    uint8_t e[48];
    for (int i = 0; i < 48; i++) e[i] = (uint8_t)(i + 1);
    uint8_t base[48];
    memset(base, 0x11, sizeof(base));
    if (xc_modexp(base, sizeof(base), e, sizeof(e), n, out) != ESP_OK) return false;
    return hex_eq(out,
                  "fb304ff49439609c4b207b07835ee1448c241c45cf9b8725012a97539e46fc3c"
                  "389e40866c789aa18ea4d0cd502ee4187594b1cf10f7ff6fec70538c61942099"
                  "940585ad842b9c5ef40544ec2230bb835fe32a9ec50a13c607ecc24bd3393cb6"
                  "bf68f729f160fdfc2bd58d061840389634b9ca607ea9a6450f0bf3277c195131"
                  "934df2262ed32e727246c5e899e950458169df523611f4249c886ca6a0f9fe4b"
                  "b1e82f89a589777818fce7169df2a5cf4a43deacce482092618358b3174a0294"
                  "5641380605770cefe08429629d8205f1c60823e88c8a28a24f8df0a6730ae32c"
                  "d0b6046cd41a6fbfe044fb6e34737bbee7a33c79190941833bd8dce2a8092fcb"
                  "2b123db753fd1d1cfc2ec6537c6d1304006c73289add3093c85c014fcf887bca"
                  "c956882c32ab7b2e1989675331a18d313cfa4cc058156e1c87afea9e567ecf93"
                  "8bfd30fbe3943c3d1a2b88886ee2b47e6f9876c06b43d53ed385e11b17d77cd4"
                  "91d7bf7aa80751031dc8a1a7b85a26f6a9a94c933039a5d02cbb27f6bdda4820");
}
