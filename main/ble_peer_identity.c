#include "ble_peer_identity.h"
#include "mbedtls/aes.h"

bool ble_peer_rpa_matches(const uint8_t irk[16], const uint8_t address[6])
{
    if ((address[0] & 0xc0) != 0x40) return false;
    uint8_t key[16], input[16] = {0}, output[16];
    for (unsigned i = 0; i < 16; ++i) key[i] = irk[15 - i];
    // Bluetooth ah(): AES(IRK, zero-padding || prand), compare low 24 bits.
    input[13] = address[0];
    input[14] = address[1];
    input[15] = address[2];
    mbedtls_aes_context context;
    mbedtls_aes_init(&context);
    int ret = mbedtls_aes_setkey_enc(&context, key, 128);
    if (ret == 0) ret = mbedtls_aes_crypt_ecb(&context, MBEDTLS_AES_ENCRYPT, input, output);
    mbedtls_aes_free(&context);
    return ret == 0 && output[13] == address[3] &&
           output[14] == address[4] && output[15] == address[5];
}
