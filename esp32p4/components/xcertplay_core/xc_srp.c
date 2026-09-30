#include "xc_srp.h"

#include <string.h>

#include "esp_log.h"
#include "psa/crypto.h"
#include "xc_crypto.h"

static const char *TAG = "xc_srp";

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

static void hex_to_bytes(const char *hex, uint8_t *out, size_t out_len)
{
    size_t n = strlen(hex) / 2;
    if (n > out_len) {
        n = out_len;
    }
    for (size_t i = 0; i < n; i++) {
        char a = hex[i * 2], b = hex[i * 2 + 1];
        uint8_t hi = (a <= '9') ? (uint8_t)(a - '0') : (uint8_t)((a | 0x20) - 'a' + 10);
        uint8_t lo = (b <= '9') ? (uint8_t)(b - '0') : (uint8_t)((b | 0x20) - 'a' + 10);
        out[i] = (uint8_t)((hi << 4) | lo);
    }
}

static void sha512_parts(uint8_t out[64], const uint8_t *const *parts, const size_t *lens, int n)
{
    psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
    psa_hash_setup(&op, PSA_ALG_SHA_512);
    for (int i = 0; i < n; i++) {
        psa_hash_update(&op, parts[i], lens[i]);
    }
    size_t olen = 0;
    psa_hash_finish(&op, out, 64, &olen);
}

static void pad384(const uint8_t *in, size_t len, uint8_t out[XC_BN_BYTES])
{
    memset(out, 0, XC_BN_BYTES);
    if (len > XC_BN_BYTES) {
        len = XC_BN_BYTES;
    }
    memcpy(out + XC_BN_BYTES - len, in, len);
}

esp_err_t xc_srp_start(xc_srp_session_t *s, const char *username, const char *password)
{
    if (s == NULL || username == NULL || password == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t n[XC_BN_BYTES];
    hex_to_bytes(SRP_N_HEX, n, sizeof(n));
    uint8_t g[XC_BN_BYTES];
    pad384((const uint8_t[]){ 5 }, 1, g);

    if (xc_random(s->salt, sizeof(s->salt)) != ESP_OK) {
        return ESP_FAIL;
    }

    const uint8_t *ip[] = { (const uint8_t *)username, (const uint8_t *)":", (const uint8_t *)password };
    size_t il[] = { strlen(username), 1, strlen(password) };
    uint8_t inner[64];
    sha512_parts(inner, ip, il, 3);

    const uint8_t *xp[] = { s->salt, inner };
    size_t xl[] = { sizeof(s->salt), sizeof(inner) };
    uint8_t x[64];
    sha512_parts(x, xp, xl, 2);

    if (xc_modexp(g, sizeof(g), x, sizeof(x), n, s->verifier) != ESP_OK) {
        return ESP_FAIL;
    }

    const uint8_t *kp[] = { n, g };
    size_t kl[] = { sizeof(n), sizeof(g) };
    uint8_t k[64];
    sha512_parts(k, kp, kl, 2);

    if (xc_random(s->b, sizeof(s->b)) != ESP_OK) {
        return ESP_FAIL;
    }
    uint8_t gb[XC_BN_BYTES];
    if (xc_modexp(g, sizeof(g), s->b, sizeof(s->b), n, gb) != ESP_OK) {
        return ESP_FAIL;
    }
    uint8_t kpad[XC_BN_BYTES];
    pad384(k, sizeof(k), kpad);
    uint8_t kv[XC_BN_BYTES];
    xc_modmul(kpad, s->verifier, n, kv);
    xc_modadd(kv, gb, n, s->B);

    size_t ulen = strlen(username);
    if (ulen > sizeof(s->identifier)) {
        ulen = sizeof(s->identifier);
    }
    memcpy(s->identifier, username, ulen);
    s->identifier_len = ulen;
    return ESP_OK;
}

esp_err_t xc_srp_verify(xc_srp_session_t *s, const uint8_t *public_a, size_t public_a_len,
                        const uint8_t *client_proof, size_t proof_len,
                        uint8_t session_key[64], uint8_t server_proof[64])
{
    if (s == NULL || public_a == NULL || client_proof == NULL || proof_len != 64) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t n[XC_BN_BYTES];
    hex_to_bytes(SRP_N_HEX, n, sizeof(n));

    uint8_t ap[XC_BN_BYTES];
    pad384(public_a, public_a_len, ap);

    const uint8_t *up[] = { ap, s->B };
    size_t ul[] = { sizeof(ap), sizeof(s->B) };
    uint8_t u[64];
    sha512_parts(u, up, ul, 2);

    uint8_t vu[XC_BN_BYTES];
    if (xc_modexp(s->verifier, sizeof(s->verifier), u, sizeof(u), n, vu) != ESP_OK) {
        return ESP_FAIL;
    }
    uint8_t avu[XC_BN_BYTES];
    xc_modmul(ap, vu, n, avu);
    uint8_t big_s[XC_BN_BYTES];
    if (xc_modexp(avu, sizeof(avu), s->b, sizeof(s->b), n, big_s) != ESP_OK) {
        return ESP_FAIL;
    }

    size_t off = 0;
    while (off < XC_BN_BYTES - 1 && big_s[off] == 0) {
        off++;
    }
    sha512_parts(session_key, (const uint8_t *const[]){ big_s + off },
                 (const size_t[]){ XC_BN_BYTES - off }, 1);

    uint8_t hash_n[64], hash_g[64];
    sha512_parts(hash_n, (const uint8_t *const[]){ n }, (const size_t[]){ sizeof(n) }, 1);
    sha512_parts(hash_g, (const uint8_t *const[]){ (const uint8_t[]){ 5 } }, (const size_t[]){ 1 }, 1);
    uint8_t hash_xor[64];
    for (int i = 0; i < 64; i++) {
        hash_xor[i] = (uint8_t)(hash_n[i] ^ hash_g[i]);
    }
    uint8_t id_hash[64];
    sha512_parts(id_hash, (const uint8_t *const[]){ (const uint8_t *)s->identifier },
                 (const size_t[]){ s->identifier_len }, 1);

    const uint8_t *ep[] = { hash_xor, id_hash, s->salt, ap, s->B, session_key };
    size_t el[] = { 64, 64, sizeof(s->salt), sizeof(ap), sizeof(s->B), 64 };
    uint8_t expected[64];
    sha512_parts(expected, ep, el, 6);
    if (memcmp(expected, client_proof, 64) != 0) {
        ESP_LOGW(TAG, "client proof mismatch");
        return ESP_ERR_INVALID_CRC;
    }

    const uint8_t *sp[] = { ap, client_proof, session_key };
    size_t sl[] = { sizeof(ap), 64, 64 };
    sha512_parts(server_proof, sp, sl, 3);
    return ESP_OK;
}

void xc_srp_free(xc_srp_session_t *s)
{
    if (s != NULL) {
        memset(s, 0, sizeof(*s));
    }
}
