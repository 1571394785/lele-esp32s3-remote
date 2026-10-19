"""Verify actual RPA resolver against Bluetooth ah vector using Windows AES."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
header=r'''
#pragma once
#include <stdint.h>
#include <string.h>
#include <windows.h>
#include <bcrypt.h>
typedef struct {uint8_t key[16];} mbedtls_aes_context;
#define MBEDTLS_AES_ENCRYPT 1
static inline void mbedtls_aes_init(mbedtls_aes_context*c){memset(c,0,sizeof(*c));}
static inline void mbedtls_aes_free(mbedtls_aes_context*c){memset(c,0,sizeof(*c));}
static inline int mbedtls_aes_setkey_enc(mbedtls_aes_context*c,const uint8_t*k,unsigned bits){if(bits!=128)return -1;memcpy(c->key,k,16);return 0;}
static inline int mbedtls_aes_crypt_ecb(mbedtls_aes_context*c,int mode,const uint8_t*in,uint8_t*out){
 (void)mode;BCRYPT_ALG_HANDLE alg=NULL;BCRYPT_KEY_HANDLE key=NULL;ULONG written=0;
 NTSTATUS status=BCryptOpenAlgorithmProvider(&alg,BCRYPT_AES_ALGORITHM,NULL,0);
 if(status>=0)status=BCryptSetProperty(alg,BCRYPT_CHAINING_MODE,(PUCHAR)BCRYPT_CHAIN_MODE_ECB,sizeof(BCRYPT_CHAIN_MODE_ECB),0);
 if(status>=0)status=BCryptGenerateSymmetricKey(alg,&key,NULL,0,c->key,16,0);
 if(status>=0)status=BCryptEncrypt(key,(PUCHAR)in,16,NULL,NULL,0,out,16,&written,0);
 if(key)BCryptDestroyKey(key);
 if(alg)BCryptCloseAlgorithmProvider(alg,0);
 return status>=0 && written==16 ? 0 : -1;
}
'''
harness=r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "ble_peer_identity.h"
int main(void){
 // Bluetooth Core sample: IRK ec0234a357c8ad05341010a60a397d9b,
 // prand 708194, hash 0dfbaa. ESP-IDF SMP keys are little-endian.
 uint8_t irk[16]={0x9b,0x7d,0x39,0x0a,0xa6,0x10,0x10,0x34,0x05,0xad,0xc8,0x57,0xa3,0x34,0x02,0xec};
 uint8_t addr[6]={0x70,0x81,0x94,0x0d,0xfb,0xaa};
 assert(ble_peer_rpa_matches(irk,addr));
 // Reject every single-bit change in the address or identity key.
 for(unsigned bit=0;bit<48;++bit){addr[bit/8]^=1u<<(bit%8);assert(!ble_peer_rpa_matches(irk,addr));addr[bit/8]^=1u<<(bit%8);}
 for(unsigned bit=0;bit<128;++bit){irk[bit/8]^=1u<<(bit%8);assert(!ble_peer_rpa_matches(irk,addr));irk[bit/8]^=1u<<(bit%8);}
 addr[0]=0xd0;assert(!ble_peer_rpa_matches(irk,addr));
 puts("PASS: Bluetooth ah sample, ESP-IDF key byte order, 176 mismatches and non-RPA rejection (Windows AES)");
}
'''
with tempfile.TemporaryDirectory(prefix='ble_identity_test_') as directory:
 t=Path(directory);(t/'mbedtls').mkdir();(t/'mbedtls/aes.h').write_text(header)
 (t/'test.c').write_text(harness)
 subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-I',str(t),'-I',str(root/'main'),str(t/'test.c'),str(root/'main/ble_peer_identity.c'),'-lbcrypt','-o',str(t/'test.exe')],check=True)
 subprocess.run([str(t/'test.exe')],check=True)
