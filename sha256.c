/* sha256.c — plain SHA-256 (FIPS 180-4). Portable C89; see sha256.h. */
#include <string.h>
#include "sha256.h"

static const unsigned int K256[64] = {
    0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
    0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
    0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
    0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
    0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
    0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
    0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
    0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
};

#define M32(x)     ((x) & 0xffffffffu)
#define ROR32(x,n) M32((M32(x) >> (n)) | (M32(x) << (32 - (n))))

static void sha_block(pmc_sha256 *c, const unsigned char *p)
{
    unsigned int w[64], a, b, cc, d, e, f, g, h, t1, t2;
    int i;
    for (i = 0; i < 16; i++)
        w[i] = ((unsigned int)p[i*4] << 24) | ((unsigned int)p[i*4+1] << 16) |
               ((unsigned int)p[i*4+2] << 8) | (unsigned int)p[i*4+3];
    for (i = 16; i < 64; i++) {
        unsigned int s0 = ROR32(w[i-15],7) ^ ROR32(w[i-15],18) ^ (M32(w[i-15]) >> 3);
        unsigned int s1 = ROR32(w[i-2],17) ^ ROR32(w[i-2],19) ^ (M32(w[i-2]) >> 10);
        w[i] = M32(w[i-16] + s0 + w[i-7] + s1);
    }
    a=c->h[0]; b=c->h[1]; cc=c->h[2]; d=c->h[3];
    e=c->h[4]; f=c->h[5]; g=c->h[6];  h=c->h[7];
    for (i = 0; i < 64; i++) {
        unsigned int S1 = ROR32(e,6) ^ ROR32(e,11) ^ ROR32(e,25);
        unsigned int ch = (e & f) ^ ((~e) & g);
        unsigned int S0 = ROR32(a,2) ^ ROR32(a,13) ^ ROR32(a,22);
        unsigned int mj = (a & b) ^ (a & cc) ^ (b & cc);
        t1 = M32(h + S1 + ch + K256[i] + w[i]);
        t2 = M32(S0 + mj);
        h=g; g=f; f=e; e=M32(d+t1); d=cc; cc=b; b=a; a=M32(t1+t2);
    }
    c->h[0]=M32(c->h[0]+a); c->h[1]=M32(c->h[1]+b);
    c->h[2]=M32(c->h[2]+cc); c->h[3]=M32(c->h[3]+d);
    c->h[4]=M32(c->h[4]+e); c->h[5]=M32(c->h[5]+f);
    c->h[6]=M32(c->h[6]+g); c->h[7]=M32(c->h[7]+h);
}

void pmc_sha256_init(pmc_sha256 *c)
{
    c->h[0]=0x6a09e667u; c->h[1]=0xbb67ae85u; c->h[2]=0x3c6ef372u; c->h[3]=0xa54ff53au;
    c->h[4]=0x510e527fu; c->h[5]=0x9b05688cu; c->h[6]=0x1f83d9abu; c->h[7]=0x5be0cd19u;
    c->len = 0;
    c->n = 0;
}

void pmc_sha256_update(pmc_sha256 *c, const void *data, size_t len)
{
    const unsigned char *p = (const unsigned char *)data;
    c->len += (unsigned long long)len;
    if (c->n) {
        size_t need = 64 - c->n;
        if (len < need) { memcpy(c->buf + c->n, p, len); c->n += (unsigned int)len; return; }
        memcpy(c->buf + c->n, p, need);
        sha_block(c, c->buf);
        p += need; len -= need; c->n = 0;
    }
    while (len >= 64) { sha_block(c, p); p += 64; len -= 64; }
    if (len) { memcpy(c->buf, p, len); c->n = (unsigned int)len; }
}

void pmc_sha256_final_hex(pmc_sha256 *c, char *hex)
{
    static const char HEXD[] = "0123456789abcdef";
    unsigned long long bits = c->len * 8ull;   /* captured BEFORE the padding update */
    unsigned char pad[72];
    unsigned int padlen, i;

    /* 0x80, then zeros until the buffer is 56 mod 64, then the 64-bit length. */
    padlen = (c->n < 56) ? (56 - c->n) : (120 - c->n);
    memset(pad, 0, sizeof(pad));
    pad[0] = 0x80;
    for (i = 0; i < 8; i++)
        pad[padlen + i] = (unsigned char)((bits >> (56 - 8*i)) & 0xFF);
    pmc_sha256_update(c, pad, (size_t)padlen + 8);

    for (i = 0; i < 8; i++) {
        unsigned int v = c->h[i];
        int j;
        for (j = 0; j < 8; j++)
            hex[i*8 + j] = HEXD[(v >> (28 - 4*j)) & 0xF];
    }
    hex[64] = 0;
}
