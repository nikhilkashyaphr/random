/* Wire-format detector.
 *
 * Guessing a format from a filename loses afternoons. This measures instead.
 *
 * THE DISCRIMINATOR: spectral flatness.
 *   Decoded with the CORRECT format, a real capture has structure — its energy
 *   concentrates in some part of the band. Decoded with the WRONG format the
 *   bytes are re-partitioned into samples that mean nothing, and the result is
 *   close to white noise.
 *   Flatness = geometric mean / arithmetic mean of the power spectrum:
 *   ~1.0 = noise-like (wrong), << 1.0 = structured (right).
 *
 * A first attempt used "are the top 16 bits of each int32 just sign extension"
 * and got Cs16 data wrong: read as int32, the high half is the NEXT sample,
 * not sign extension. Spectral flatness has no such blind spot.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <complex.h>

#define NFFT 4096
static void fft(double complex*a,int n){
    for(int i=1,j=0;i<n;i++){int b=n>>1;for(;j&b;b>>=1)j^=b;j^=b;
        if(i<j){double complex t=a[i];a[i]=a[j];a[j]=t;}}
    for(int len=2;len<=n;len<<=1){double ang=-2*M_PI/len;
        double complex wl=cos(ang)+I*sin(ang);
        for(int i=0;i<n;i+=len){double complex w=1;
            for(int k=0;k<len/2;k++){double complex u=a[i+k],v=a[i+k+len/2]*w;
                a[i+k]=u+v;a[i+k+len/2]=u-v;w*=wl;}}}
}
static double flatness(const double complex*x,int n){
    double complex*a=malloc(sizeof(double complex)*NFFT);
    int m=n<NFFT?n:NFFT;
    for(int i=0;i<NFFT;i++){
        double w = i<m ? 0.5-0.5*cos(2*M_PI*i/(m-1)) : 0.0;   /* Hann */
        a[i]= i<m ? x[i]*w : 0;
    }
    fft(a,NFFT);
    double la=0,ar=0; int c=0;
    for(int i=0;i<NFFT;i++){
        double p=creal(a[i])*creal(a[i])+cimag(a[i])*cimag(a[i]);
        if(p<1e-20)p=1e-20;
        la+=log(p); ar+=p; c++;
    }
    free(a);
    if(c==0||ar<=0||!isfinite(la)||!isfinite(ar)) return 1.0;
    double r = exp(la/c)/(ar/c);
    return isfinite(r) ? r : 1.0;   /* inf/NaN: reject, do not rank as "best" */
}
typedef struct { const char*label; double flat; int bytes_per_samp; } cand_t;

int main(int argc,char**argv){
    if(argc<2){fprintf(stderr,"usage: %s <file.bin> [channels]\n",argv[0]);return 2;}
    int ch=argc>2?atoi(argv[2]):1; if(ch<1)ch=1;
    FILE*f=fopen(argv[1],"rb"); if(!f){perror("open");return 1;}
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
    size_t want = sz<(1<<21)?(size_t)sz:(1<<21);
    uint8_t*b=malloc(want); size_t got=fread(b,1,want,f); fclose(f);

    printf("file       : %s\nsize       : %ld bytes", argv[1], sz);
    if(sz%4096==0) printf("   (%ld x 4096 B — matches roce-extractor payloads)",sz/4096);
    printf("\nanalysed   : %zu bytes   channels: %d\n\n", got, ch);

    int N=NFFT;
    double complex*x=malloc(sizeof(double complex)*N);
    cand_t c[6]; int nc=0;

    /* Complex Int 16 — take channel 0 of `ch` interleaved streams */
    {const int16_t*s=(const int16_t*)b; size_t n=got/2;
     for(int i=0;i<N;i++){size_t k=(size_t)i*2*ch; 
        x[i]= k+1<n ? s[k]+I*(double)s[k+1] : 0;}
     c[nc++]=(cand_t){"Complex Int 16",flatness(x,N),4*ch};}
    /* Complex Int 32 */
    {const int32_t*s=(const int32_t*)b; size_t n=got/4;
     for(int i=0;i<N;i++){size_t k=(size_t)i*2*ch;
        x[i]= k+1<n ? s[k]+I*(double)s[k+1] : 0;}
     c[nc++]=(cand_t){"Complex Int 32",flatness(x,N),8*ch};}
    /* Short 16 (real) */
    {const int16_t*s=(const int16_t*)b; size_t n=got/2;
     for(int i=0;i<N;i++){size_t k=(size_t)i*ch; x[i]= k<n ? s[k] : 0;}
     c[nc++]=(cand_t){"Short 16 (real)",flatness(x,N),2*ch};}
    /* Int 32 (real) */
    {const int32_t*s=(const int32_t*)b; size_t n=got/4;
     for(int i=0;i<N;i++){size_t k=(size_t)i*ch; x[i]= k<n ? s[k] : 0;}
     c[nc++]=(cand_t){"Int 32 (real)",flatness(x,N),4*ch};}
    /* Complex Float 32 */
    {const float*s=(const float*)b; size_t n=got/4;
     for(int i=0;i<N;i++){size_t k=(size_t)i*2*ch;
        x[i]= k+1<n ? s[k]+I*(double)s[k+1] : 0;}
     c[nc++]=(cand_t){"Complex Float 32",flatness(x,N),8*ch};}
    /* Complex Int 8 */
    {const int8_t*s=(const int8_t*)b; size_t n=got;
     for(int i=0;i<N;i++){size_t k=(size_t)i*2*ch;
        x[i]= k+1<n ? s[k]+I*(double)s[k+1] : 0;}
     c[nc++]=(cand_t){"Complex Int 8",flatness(x,N),2*ch};}

    printf("SPECTRAL FLATNESS, dB  (more negative = more structure = more likely)\n");
    int best=0; for(int i=1;i<nc;i++) if(c[i].flat<c[best].flat) best=i;
    for(int i=0;i<nc;i++)
        printf("  %-20s %9.1f dB%s\n", c[i].label,
               10.0*log10(c[i].flat < 1e-300 ? 1e-300 : c[i].flat),
               i==best?"   <-- best":"");

    /* 14-bit MSB-aligned fingerprint */
    const int16_t*s16=(const int16_t*)b; size_t n16=got/2, lsb0=0;
    for(size_t i=0;i<n16;i++) if((s16[i]&3)==0) lsb0++;
    double lsb=n16?100.0*lsb0/n16:0;

    printf("\nFINGERPRINTS\n");
    printf("  int16 samples with bits[1:0]==00 : %6.2f%%%s\n", lsb,
           lsb>95?"   <- 14-bit MSB-aligned, RFSoC packing":"");

    printf("\nVERDICT\n  Wire format : \"%s\"\n", c[best].label);
    printf("  Bytes/sample: %d  (with %d channel%s)\n",
           c[best].bytes_per_samp, ch, ch==1?"":"s");
    if(sz % c[best].bytes_per_samp)
        printf("  NOTE: file size is not a whole number of samples in this format.\n");
    if(lsb>95)
        printf("  Full scale is 8191<<2 = 32764, not 32767.\n");
    free(x);free(b);
    return 0;
}
