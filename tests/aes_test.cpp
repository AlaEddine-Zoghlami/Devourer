#include "crypto/AesCcm.h"
#include <cstdio>
#include <cstring>
using namespace apfpv::crypto;
int main(){
    // FIPS-197 AES-128 vector: key=000102..0f, pt=00112233..ff, ct=69c4e0d86a7b0430d8cdb78070b4c55a
    uint8_t key[16],pt[16],ct[16],out[16];
    for(int i=0;i<16;i++){key[i]=i;pt[i]=i*0x11;}
    uint8_t expct[16]={0x69,0xc4,0xe0,0xd8,0x6a,0x7b,0x04,0x30,0xd8,0xcd,0xb7,0x80,0x70,0xb4,0xc5,0x5a};
    aes128_ecb_encrypt(key,pt,ct);
    printf("AES enc: %s\n", memcmp(ct,expct,16)?"FAIL":"OK");
    aes128_ecb_decrypt(key,expct,out);
    printf("AES dec: %s\n", memcmp(out,pt,16)?"FAIL":"OK");
    // RFC3394 unwrap of wrap(KEK=000..f, key=00112233..) test vector:
    uint8_t kek[16]; for(int i=0;i<16;i++)kek[i]=i;
    uint8_t wrapped[24]={0x1F,0xA6,0x8B,0x0A,0x81,0x12,0xB4,0x47,0xAE,0xF3,0x4B,0xD8,0xFB,0x5A,0x7B,0x82,0x9D,0x3E,0x86,0x23,0x71,0xD2,0xCF,0xE5};
    uint8_t uw[16]; bool ok=aes_key_unwrap(kek,wrapped,24,uw);
    uint8_t expkey[16]; for(int i=0;i<16;i++)expkey[i]=i*0x11;
    printf("RFC3394 unwrap: %s\n", (ok&&!memcmp(uw,expkey,16))?"OK":"FAIL");
    return 0;
}
