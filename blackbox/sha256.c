#include "sha256.h"
#include <string.h>
static const uint32_t K[64]={
0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
#define R(x,n) (((x)>>(n))|((x)<<(32-(n))))
static void blk(bb_sha256_ctx*c,const uint8_t*p){uint32_t w[64],a,b,d,e,f,g,h,cc,t1,t2;int i;
 for(i=0;i<16;i++)w[i]=(uint32_t)p[4*i]<<24|(uint32_t)p[4*i+1]<<16|(uint32_t)p[4*i+2]<<8|p[4*i+3];
 for(;i<64;i++){uint32_t s0=R(w[i-15],7)^R(w[i-15],18)^(w[i-15]>>3),s1=R(w[i-2],17)^R(w[i-2],19)^(w[i-2]>>10);w[i]=w[i-16]+s0+w[i-7]+s1;}
 a=c->h[0];b=c->h[1];cc=c->h[2];d=c->h[3];e=c->h[4];f=c->h[5];g=c->h[6];h=c->h[7];
 for(i=0;i<64;i++){t1=h+(R(e,6)^R(e,11)^R(e,25))+((e&f)^(~e&g))+K[i]+w[i];t2=(R(a,2)^R(a,13)^R(a,22))+((a&b)^(a&cc)^(b&cc));
  h=g;g=f;f=e;e=d+t1;d=cc;cc=b;b=a;a=t1+t2;}
 c->h[0]+=a;c->h[1]+=b;c->h[2]+=cc;c->h[3]+=d;c->h[4]+=e;c->h[5]+=f;c->h[6]+=g;c->h[7]+=h;}
void bb_sha256_init(bb_sha256_ctx*c){static const uint32_t I[8]={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};memcpy(c->h,I,32);c->len=0;c->n=0;}
void bb_sha256_update(bb_sha256_ctx*c,const void*d,size_t l){const uint8_t*p=d;c->len+=l;
 if(c->n){size_t k=64-c->n;if(k>l)k=l;memcpy(c->buf+c->n,p,k);c->n+=k;p+=k;l-=k;if(c->n==64){blk(c,c->buf);c->n=0;}}
 while(l>=64){blk(c,p);p+=64;l-=64;} if(l){memcpy(c->buf,p,l);c->n=l;}}
void bb_sha256_final(bb_sha256_ctx*c,uint8_t o[32]){uint64_t bits=c->len*8;uint8_t pad=0x80,z=0,L[8];int i;
 bb_sha256_update(c,&pad,1);while(c->n!=56)bb_sha256_update(c,&z,1);
 for(i=0;i<8;i++)L[i]=(uint8_t)(bits>>(56-8*i));
 bb_sha256_update(c,L,8);
 for(i=0;i<8;i++){o[4*i]=c->h[i]>>24;o[4*i+1]=c->h[i]>>16;o[4*i+2]=c->h[i]>>8;o[4*i+3]=c->h[i];}}
void bb_sha256(const void*d,size_t l,uint8_t o[32]){bb_sha256_ctx c;bb_sha256_init(&c);bb_sha256_update(&c,d,l);bb_sha256_final(&c,o);}
void bb_hmac_sha256(const uint8_t*key,size_t kl,const void*m,size_t ml,uint8_t o[32]){
 uint8_t k[64]={0},ip[64],op[64],ih[32];int i;bb_sha256_ctx c;
 if(kl>64)bb_sha256(key,kl,k);else memcpy(k,key,kl);
 for(i=0;i<64;i++){ip[i]=k[i]^0x36;op[i]=k[i]^0x5c;}
 bb_sha256_init(&c);bb_sha256_update(&c,ip,64);bb_sha256_update(&c,m,ml);bb_sha256_final(&c,ih);
 bb_sha256_init(&c);bb_sha256_update(&c,op,64);bb_sha256_update(&c,ih,32);bb_sha256_final(&c,o);}
