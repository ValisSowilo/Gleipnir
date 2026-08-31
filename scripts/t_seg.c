#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#define SEGWIN 8192
#define AFMIN 32
#define AGAP 6
#define B_MODEL 0
#define B_ALPHA 3
#define BTYPE(t) ((t)&3)
static const uint64_t ALPHA_OPS =
    (1ULL<<0x08)|(1ULL<<0x09)|(1ULL<<0x0a)|(1ULL<<0x0c)|(1ULL<<0x0d)|(1ULL<<0x0e)|
    (1ULL<<0x10)|(1ULL<<0x11)|(1ULL<<0x12)|(1ULL<<0x13)|(1ULL<<0x14)|(1ULL<<0x15)|
    (1ULL<<0x16)|(1ULL<<0x17)|(1ULL<<0x18)|(1ULL<<0x1a)|0xFFFFFFFF00000000ULL;
static int alpha_align_of(const uint8_t *d, size_t n, size_t base, int *ok) {
    *ok = 0;
    if (n < 512) return 0;
    int hit[4]={0,0,0,0}, tot[4]={0,0,0,0};
    for (int a=0;a<4;a++){
        size_t st=(size_t)((a-(int)(base&3))&3);
        for(size_t i=st;i+4<=n;i+=4){ if(ALPHA_OPS>>(d[i+3]>>2)&1) hit[a]++; tot[a]++; }
    }
    int b=0; double f[4];
    for(int a=0;a<4;a++){ f[a]=tot[a]?(double)hit[a]/tot[a]:0.0; if(f[a]>f[b]) b=a; }
    double second=0.0;
    for(int a=0;a<4;a++) if(a!=b&&f[a]>second) second=f[a];
    if(f[b]>AFMIN/100.0 && f[b]-second>AGAP/100.0) *ok=1;
    return b;
}
static int classify(const uint8_t *d, size_t n, size_t base) {
    int aok, aal = alpha_align_of(d,n,base,&aok);
    if (aok) return B_ALPHA | (aal<<2);
    return B_MODEL;   /* other paths irrelevant for this test */
}
typedef struct { uint8_t type; uint32_t len; } Blk;
int main(int argc,char**argv){
    FILE*f=fopen(argv[1],"rb"); fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
    uint8_t*d=malloc(n); fread(d,1,n,f); fclose(f);
    Blk*out=malloc(sizeof(Blk)*65536); int nb=0; size_t i=0;
    while(i<(size_t)n && nb<65536){
        size_t w=((size_t)n-i<SEGWIN)?(size_t)n-i:SEGWIN;
        int t=classify(d+i,w,i);
        size_t start=i; i+=w;
        while(i<(size_t)n){
            size_t w2=((size_t)n-i<SEGWIN)?(size_t)n-i:SEGWIN;
            if(classify(d+i,w2,i)!=t) break;
            i+=w2;
        }
        if(nb&&out[nb-1].type==t) out[nb-1].len+=(uint32_t)(i-start);
        else { out[nb].type=(uint8_t)t; out[nb].len=(uint32_t)(i-start); nb++; }
    }
    unsigned long long ab=0; int ac=0, byal[4]={0,0,0,0};
    for(int q=0;q<nb;q++) if(BTYPE(out[q].type)==B_ALPHA){
        ac++; ab+=out[q].len; byal[(out[q].type>>2)&3]++;
    }
    printf("%-12s nb=%d  alpha %d blocks %lluKB  blocks-by-align 0:%d 1:%d 2:%d 3:%d\n",
           argv[1],nb,ac,ab>>10,byal[0],byal[1],byal[2],byal[3]);
    return 0;
}
