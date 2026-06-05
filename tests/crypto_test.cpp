#include "crypto/Sha1Hmac.h"
#include "crypto/AesCcm.h"
#include <cstdio>
#include <cstring>
using namespace apfpv::crypto;
static void hex(const uint8_t*p,int n){for(int i=0;i<n;i++)printf("%02x",p[i]);printf("\n");}
int main(){
    int fail=0;
    // 1) SHA1("abc") = a9993e364706816aba3e25717850c26c9cd0d89d
    uint8_t h[20]; sha1((const uint8_t*)"abc",3,h);
    uint8_t exp1[20]={0xa9,0x99,0x3e,0x36,0x47,0x06,0x81,0x6a,0xba,0x3e,0x25,0x71,0x78,0x50,0xc2,0x6c,0x9c,0xd0,0xd8,0x9d};
    printf("SHA1(abc): %s", memcmp(h,exp1,20)?"FAIL ":"OK\n"); if(memcmp(h,exp1,20)){fail++;hex(h,20);}
    // 2) HMAC-SHA1 RFC2202 test 1: key=0x0b*20, data="Hi There" -> b617318655057264e28bc0b6fb378c8ef146be00
    uint8_t key[20]; memset(key,0x0b,20); uint8_t m[20];
    hmac_sha1(key,20,(const uint8_t*)"Hi There",8,m);
    uint8_t exp2[20]={0xb6,0x17,0x31,0x86,0x55,0x05,0x72,0x64,0xe2,0x8b,0xc0,0xb6,0xfb,0x37,0x8c,0x8e,0xf1,0x46,0xbe,0x00};
    printf("HMAC-SHA1: %s", memcmp(m,exp2,20)?"FAIL ":"OK\n"); if(memcmp(m,exp2,20)){fail++;hex(m,20);}
    // 3) PBKDF2 WPA2 IEEE test: pass="password", ssid="IEEE", 4096, 32 ->
    //    f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e
    uint8_t pmk[32]; pbkdf2_sha1("password",(const uint8_t*)"IEEE",4,4096,pmk,32);
    uint8_t exp3[32]={0xf4,0x2c,0x6f,0xc5,0x2d,0xf0,0xeb,0xef,0x9e,0xbb,0x4b,0x90,0xb3,0x8a,0x5f,0x90,
                      0x2e,0x83,0xfe,0x1b,0x13,0x5a,0x70,0xe2,0x3a,0xed,0x76,0x2e,0x97,0x10,0xa1,0x2e};
    printf("PBKDF2 WPA2: %s", memcmp(pmk,exp3,32)?"FAIL ":"OK\n"); if(memcmp(pmk,exp3,32)){fail++;hex(pmk,32);}
    // 4) AES-CCM round-trip (RFC 3610 vector 1 structure): encrypt then decrypt
    uint8_t k[16]={0xc0,0xc1,0xc2,0xc3,0xc4,0xc5,0xc6,0xc7,0xc8,0xc9,0xca,0xcb,0xcc,0xcd,0xce,0xcf};
    uint8_t nonce[13]={0x00,0x00,0x00,0x03,0x02,0x01,0x00,0xa0,0xa1,0xa2,0xa3,0xa4,0xa5};
    uint8_t aad[8]={0,1,2,3,4,5,6,7};
    uint8_t pt[23]={8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30};
    uint8_t ct[31]; aes_ccm_encrypt(k,nonce,13,aad,8,pt,23,ct);
    uint8_t dec[23]; bool ok=aes_ccm_decrypt(k,nonce,13,aad,8,ct,31,dec);
    printf("AES-CCM roundtrip: %s", (ok&&!memcmp(dec,pt,23))?"OK\n":"FAIL\n"); if(!(ok&&!memcmp(dec,pt,23)))fail++;
    printf("\n%s\n", fail?"SOME VECTORS FAILED":"ALL CRYPTO VECTORS PASS");
    return fail;
}
