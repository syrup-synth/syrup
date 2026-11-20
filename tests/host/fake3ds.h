#define SYRUP_HOST 1
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <pthread.h>
typedef int8_t s8;
#include <sys/stat.h>
typedef pthread_mutex_t LightLock;
typedef struct { pthread_mutex_t m; pthread_cond_t c; int sig; } LightEvent;
typedef pthread_t* Thread; typedef void (*ThreadFunc)(void*); typedef int32_t s32;
enum{RESET_ONESHOT=0}; typedef int ResetType;
#define CUR_THREAD_HANDLE 0xFFFF8000
#define U64_MAX 0xFFFFFFFFFFFFFFFFULL
static void LightLock_Init(LightLock* l){pthread_mutex_init(l,NULL);} static void LightLock_Lock(LightLock* l){pthread_mutex_lock(l);} static void LightLock_Unlock(LightLock* l){pthread_mutex_unlock(l);} static int LightLock_TryLock(LightLock* l){return pthread_mutex_trylock(l)!=0;}
static void LightEvent_Init(LightEvent* e,ResetType t){(void)t;pthread_mutex_init(&e->m,NULL);pthread_cond_init(&e->c,NULL);e->sig=0;}
static void LightEvent_Signal(LightEvent* e){pthread_mutex_lock(&e->m);e->sig=1;pthread_cond_signal(&e->c);pthread_mutex_unlock(&e->m);}
static void LightEvent_Wait(LightEvent* e){pthread_mutex_lock(&e->m);while(!e->sig)pthread_cond_wait(&e->c,&e->m);e->sig=0;pthread_mutex_unlock(&e->m);}
static int APT_SetAppCpuTimeLimit(int p){(void)p;return 0;} static int svcGetThreadPriority(s32* p,int h){(void)h;*p=0x30;return 0;}
typedef struct{ThreadFunc f;void*a;}TA; static void* tramp(void* x){TA* t=x; t->f(t->a); return NULL;}
/* FAKE_SYNC_WORKER (default): no thread on core 1, so Syrup rebuilds wavetables inline on the main loop and every
   run renders the same audio whatever the machine's speed. Tests of the core-1 worker / effects hand-over turn it off. */
static int FAKE_SYNC_WORKER = 1;
static Thread threadCreate(ThreadFunc f,void*a,size_t st,int pr,int core,bool det){(void)st;(void)pr;(void)det; if(core==1&&FAKE_SYNC_WORKER) return NULL; pthread_t* t=malloc(sizeof(pthread_t)); TA* ta=malloc(sizeof(TA)); ta->f=f;ta->a=a; pthread_create(t,NULL,tramp,ta); return t;}
static int threadJoin(Thread t,unsigned long long to){(void)to;pthread_join(*t,NULL);return 0;} static void threadFree(Thread t){free(t);}

typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64; typedef int16_t s16; typedef int32_t Result;
#define R_SUCCEEDED(r) ((r)>=0)
enum{GFX_TOP,GFX_BOTTOM}; enum{GFX_LEFT};
enum{KEY_A=1,KEY_B=2,KEY_SELECT=4,KEY_START=8,KEY_DRIGHT=16,KEY_DLEFT=32,KEY_DUP=64,KEY_DDOWN=128,KEY_R=256,KEY_L=512,KEY_X=1024,KEY_Y=2048,KEY_TOUCH=1<<20};
typedef struct{u16 px,py;}touchPosition; typedef struct{short dx,dy;}circlePosition;
typedef struct{u8* gfx; u16 asciiOffset; u16 numChars;}ConsoleFont; typedef struct{ConsoleFont font;}PrintConsole;
enum{NDSP_OUTPUT_STEREO=1, NDSP_OUTPUT_SURROUND=2, NDSP_CLIP_SOFT=1}; enum{NDSP_INTERP_POLYPHASE=0, NDSP_INTERP_LINEAR=1, NDSP_INTERP_NONE=2}; enum{NDSP_FORMAT_MONO_PCM16=5, NDSP_FORMAT_MONO_ADPCM=9};
typedef struct{ u16 index; s16 history0, history1; } ndspAdpcmData;
typedef struct{ union{const void* data_vaddr; s16* data_pcm16;}; u32 nsamples; ndspAdpcmData* adpcm_data; u32 offset; bool looping; u8 status; }ndspWaveBuf;
// harness state
static u8 FONT[2048]; static PrintConsole CON; static u8 TOPFB[400*240*3], BOTFB[320*240*3];
static int FR=0, FRMAX=0; static u32 HD=0, HDPREV=0; static int TOUCH=0, TX=0, TY=0; static short CX=0, CY=0;
static PrintConsole* consoleGetDefault(void){CON.font.gfx=FONT;return &CON;}
static void gfxInitDefault(void){} static void gfxSetDoubleBuffering(int a,bool b){(void)a;(void)b;} static void gfxExit(void){}
static u8* gfxGetFramebuffer(int s,int e,void*a,void*b){(void)e;(void)a;(void)b;return s==GFX_TOP?TOPFB:BOTFB;}
static void gfxFlushBuffers(void){} static void gfxSwapBuffers(void){}
static void* linearAlloc(size_t n){return malloc(n);} static void linearFree(void*p){free(p);} static void DSP_FlushDataCache(const void*p,u32 n){(void)p;(void)n;}
static u64 osGetTime(void){return (u64)(FR*1000/60);}
static void hidScanInput(void){} static u32 hidKeysHeld(void){return HD|(TOUCH?KEY_TOUCH:0);} static u32 hidKeysDown(void){return HD&~HDPREV;}
static void hidTouchRead(touchPosition* t){t->px=TX;t->py=TY;} static void hidCircleRead(circlePosition* c){c->dx=CX;c->dy=CY;}
// ndsp emulation
typedef struct{ ndspWaveBuf* wb; ndspWaveBuf* q[8]; int nq; double pos; float rate, mix[12]; int bq; float b0,b1,b2,a1,a2,z1,z2; int cq; int cp[5], x1,x2,y1,y2; } Ch;
enum{NDSP_WBUF_FREE=0,NDSP_WBUF_QUEUED=1,NDSP_WBUF_PLAYING=2,NDSP_WBUF_DONE=3};
static Ch CH[24];
static Result ndspInit(void){return 0;} static void ndspExit(void){} static void ndspSetOutputMode(int m){(void)m;} static void ndspSetMasterVol(float v){(void)v;}
static void ndspChnReset(int c){memset(&CH[c],0,sizeof(Ch));} static void ndspChnSetInterp(int c,int i){(void)c;(void)i;} static void ndspChnSetFormat(int c,u16 f){(void)c;(void)f;}
static void ndspChnSetRate(int c,float r){CH[c].rate=r;} static void ndspChnSetMix(int c,float m[12]){memcpy(CH[c].mix,m,48);}
static void ndspChnWaveBufClear(int c){CH[c].wb=NULL;CH[c].nq=0;CH[c].pos=0;} static void ndspChnWaveBufAdd(int c,ndspWaveBuf* w){ if(!w->looping && CH[c].wb && !CH[c].wb->looping){ if(CH[c].nq<8){w->status=NDSP_WBUF_QUEUED;CH[c].q[CH[c].nq++]=w;} return;} w->status=NDSP_WBUF_PLAYING; CH[c].wb=w;CH[c].pos=0;}
static void ndspChnIirBiquadSetEnable(int c,bool e){CH[c].bq=e;}
static bool ndspChnIirBiquadSetParamsLowPassFilter(int c,float f0,float Q){ CH[c].cq=0; float w=2*3.14159265f*f0/32728.0f, al=sinf(w)/(2*Q), cw=cosf(w), a0=1+al;
 CH[c].b0=(1-cw)/2/a0; CH[c].b1=(1-cw)/a0; CH[c].b2=(1-cw)/2/a0; CH[c].a1=-2*cw/a0; CH[c].a2=(1-al)/a0; return true; }
static void rbj(int c,int type,float f0,float Q,float gdb){ float A=powf(10,gdb/40), w=2*3.14159265f*f0/32728.0f, al=sinf(w)/(2*Q), cw=cosf(w), b0,b1,b2,a0,a1,a2;
 if(type==1){b0=(1+cw)/2;b1=-(1+cw);b2=(1+cw)/2;a0=1+al;a1=-2*cw;a2=1-al;}
 else if(type==2){b0=al;b1=0;b2=-al;a0=1+al;a1=-2*cw;a2=1-al;}
 else if(type==3){b0=1;b1=-2*cw;b2=1;a0=1+al;a1=-2*cw;a2=1-al;}
 else {b0=1+al*A;b1=-2*cw;b2=1-al*A;a0=1+al/A;a1=-2*cw;a2=1-al/A;}
 CH[c].b0=b0/a0;CH[c].b1=b1/a0;CH[c].b2=b2/a0;CH[c].a1=a1/a0;CH[c].a2=a2/a0; CH[c].cq=0; }
static bool ndspChnIirBiquadSetParamsHighPassFilter(int c,float f,float q){rbj(c,1,f,q,0);return true;}
static bool ndspChnIirBiquadSetParamsBandPassFilter(int c,float f,float q){rbj(c,2,f,q,0);return true;}
static bool ndspChnIirBiquadSetParamsNotchFilter(int c,float f,float q){rbj(c,3,f,q,0);return true;}
static bool ndspChnIirBiquadSetParamsPeakingEqualizer(int c,float f,float q,float g){rbj(c,4,f,q,20*log10f(g>0?g:1));return true;}
#define BIT(n) (1u<<(n))
#define CSND_TIMER(n) (0x3FEC3FC / ((u32)(n)))
#define SOUND_CHANNEL(n) ((u32)(n) & 0x1F)
enum{SOUND_FORMAT_16BIT=1<<12, SOUND_REPEAT=1<<10, SOUND_LINEAR_INTERP=1<<6, SOUND_ENABLE=1<<14};
static u32 csndChannels=0xFFFFFF00;
static const void* PHYS[4096]; static int NPHYS=1;
static u32 osConvertVirtToPhys(const void* p){ for(int i=1;i<NPHYS;i++) if(PHYS[i]==p) return i; PHYS[NPHYS]=p; return NPHYS++; }
typedef struct{ const s16* d; int n; double pos; u32 timer; float l,r,cl,cr; int on, fmt, duty, rep, ap, ai, a0p, a0i, last; const u8* ad; u32 lfsr; } CsCh; static CsCh CS[32];
typedef struct{ s16* b; int n, pos, on; } CapU; static CapU CAPU[2]; static int CAPUSED[2];
enum{SOUND_FORMAT_ADPCM=2<<12, SOUND_FORMAT_PSG=3<<12, CAPTURE_REPEAT=0, CAPTURE_FORMAT_16BIT=0, CAPTURE_ENABLE=1<<15};
typedef int CSND_DutyCycle;
static const int IMS[89]={7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,80,88,97,107,118,130,143,157,173,190,209,230,253,279,307,337,371,408,449,494,544,598,658,724,796,876,963,1060,1166,1282,1411,1552,1707,1878,2066,2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,5894,6484,7132,7845,8630,9493,10442,11487,12635,13899,15289,16818,18500,20350,22385,24623,27086,29794,32767};
static const int IMI[16]={-1,-1,-1,-1,2,4,6,8,-1,-1,-1,-1,2,4,6,8};
static float csNext(CsCh* h){
  int adv; double inc=(0x3FEC3FC/(double)h->timer)/32728.0;
  if(h->fmt==3){ if(h->duty<16){ double ph=h->pos-floor(h->pos/8)*8; h->pos+=inc; int hi=(int)ph < ((h->duty==7)?0:h->duty+1); return hi?0.5f:-0.5f; }
    h->pos+=inc; while(h->pos>=1){ h->pos-=1; u32 b=h->lfsr&1; h->lfsr>>=1; if(b) h->lfsr^=0x6000; } return (h->lfsr&1)?0.5f:-0.5f; }
  if(h->fmt==2){ int i=(int)h->pos; float x=h->last/32768.0f; h->pos+=inc; adv=(int)h->pos-i;
    while(adv-->0){ int p=++h->ap; if(p>=h->n){ if(!h->rep){h->on=0;return 0;} h->ap=0; p=0; h->last=h->a0p; h->ai=h->a0i; }
      int b=h->ad[p>>1]; int code=(p&1)?b>>4:b&15; int st=IMS[h->ai], dq=st>>3; if(code&4)dq+=st; if(code&2)dq+=st>>1; if(code&1)dq+=st>>2;
      h->last+=(code&8)?-dq:dq; if(h->last>32767)h->last=32767; if(h->last<-32768)h->last=-32768; h->ai+=IMI[code]; if(h->ai<0)h->ai=0; if(h->ai>88)h->ai=88; }
    return x; }
  int i=(int)h->pos; float u=h->pos-i; float x=(h->d[i%h->n]*(1-u)+h->d[(i+1)%h->n]*u)/32768.0f; h->pos+=inc;
  if(h->pos>=h->n){ if(!h->rep){h->on=0;} else h->pos-=h->n*floor(h->pos/h->n);} return x; }
static Result csndInit(void){return 0;} static void csndExit(void){} static Result csndExecCmds(bool w){(void)w;return 0;}
static void CSND_SetChnRegs(u32 flags,u32 pa0,u32 pa1,u32 size,u32 vol,u32 cap){(void)pa1; int c=flags&0x1F; CsCh* h=&CS[c]; int ap=h->a0p, ai=h->a0i; h->fmt=(flags>>12)&3; h->rep=((flags>>10)&3)==1; h->d=PHYS[pa0]; h->ad=PHYS[pa0];
 h->n=h->fmt==2?size*2:size/2; h->pos=0; h->timer=flags>>16; h->l=(vol&0xffff)/32768.0f; h->r=(vol>>16)/32768.0f; h->cl=(cap&0xffff)/32768.0f; h->cr=(cap>>16)/32768.0f; h->on=1; h->ap=-1; h->last=ap; h->ai=ai; }
static void CSND_SetChnRegsPSG(u32 flags,u32 vol,u32 cap,CSND_DutyCycle d){ int c=flags&0x1F; CsCh* h=&CS[c]; h->fmt=3; h->duty=d; h->d=NULL; h->pos=0; h->timer=flags>>16; h->l=(vol&0xffff)/32768.0f; h->r=(vol>>16)/32768.0f; h->cl=(cap&0xffff)/32768.0f; h->cr=(cap>>16)/32768.0f; h->on=1; }
static void CSND_SetChnRegsNoise(u32 flags,u32 vol,u32 cap){ CSND_SetChnRegsPSG(flags,vol,cap,99); CS[flags&0x1F].lfsr=0x7FFF; }
static void CSND_SetAdpcmState(u32 c,int blk,int smp,int idx){ if(!blk){CS[c].a0p=smp;CS[c].a0i=idx;} }
static void CSND_SetAdpcmReload(u32 c,bool r){(void)c;(void)r;}
static void CSND_SetDuty(u32 c,CSND_DutyCycle d){CS[c].duty=d;}
static Result CSND_AcquireCapUnit(u32* u){ for(int i=0;i<2;i++) if(!CAPUSED[i]){CAPUSED[i]=1;*u=i;return 0;} return -1; }
static Result CSND_ReleaseCapUnit(u32 u){ CAPUSED[u]=0; CAPU[u].on=0; return 0; }
static void CSND_SetCapRegs(u32 u,u32 flags,u32 addr,u32 size){(void)flags; CAPU[u].b=(s16*)PHYS[addr]; CAPU[u].n=size/2; CAPU[u].pos=0; }
static void CSND_CapSetTimer(u32 u,u32 t){(void)u;(void)t;} static void CSND_CapEnable(u32 u,bool e){CAPU[u].on=e;}
static void CSND_SetPlayState(u32 c,u32 v){ CS[c].on=v; }
static void CSND_SetTimer(u32 c,u32 t){ CS[c].timer=t; }
static void CSND_SetVol(u32 c,u32 vol,u32 cap){ CS[c].l=(vol&0xffff)/32768.0f; CS[c].r=(vol>>16)/32768.0f; CS[c].cl=(cap&0xffff)/32768.0f; CS[c].cr=(cap>>16)/32768.0f; }
static s16 OUT[32728*160*2]; static int ON=0;
typedef void (*ndspCallback)(void*); typedef void (*ndspAuxCallback)(void*,int,void*[4]);
static ndspCallback NCB; static ndspAuxCallback AUXCB[2]; static int AUXEN[2], AUXDEAD=0;
static float AXS[2][2][160], RET[2][2][160]; static int FPOS=0;
static void ndspSetCallback(ndspCallback c,void* d){(void)d;NCB=c;}
static void ndspAuxSetCallback(int i,ndspAuxCallback c,void* d){(void)d;AUXCB[i]=c;}
static void ndspAuxSetEnable(int i,bool e){AUXEN[i]=e;} static void ndspAuxSetFrontBypass(int i,bool b){(void)i;(void)b;} static void ndspAuxSetVolume(int i,float v){(void)i;(void)v;}
static void ndspSetClippingMode(int m){(void)m;} static void ndspSurroundSetDepth(u16 d){(void)d;}
static void ndspChnIirMonoSetEnable(int c,bool e){(void)c;(void)e;} static bool ndspChnIirMonoSetParamsLowPassFilter(int c,float f){(void)c;(void)f;return 1;} static bool ndspChnIirMonoSetParamsHighPassFilter(int c,float f){(void)c;(void)f;return 1;}
typedef struct{s16 x,y,z;} accelVector; static void HIDUSER_EnableAccelerometer(void){} static void HIDUSER_DisableAccelerometer(void){}
static float TILTX=0; static void hidAccelRead(accelVector* a){a->x=(s16)(TILTX*512);a->y=362;a->z=362;}
enum{MICU_ENCODING_PCM16_SIGNED=3, MICU_SAMPLE_RATE_16360=1};
static u32 MICN; static Result micInit(u8* b,u32 n){(void)b;MICN=n;return 0;} static void micExit(void){} static u32 micGetSampleDataSize(void){return MICN-4;} static u32 micGetLastSampleOffset(void){return 0;}
static Result MICU_StartSampling(int a,int b,u32 c,u32 d,bool e){(void)a;(void)b;(void)c;(void)d;(void)e;return 0;} static Result MICU_StopSampling(void){return 0;}
static void* memalign(size_t a,size_t n){(void)a;return malloc(n);}
#define R_FAILED(r) ((r)<0)
/* simulated clock: the tick follows the audio rendered so far, so tests are repeatable and run faster than real time.
   A test can take the clock over with SIMTICK_SET. */
static int SIMTICK_SET=0; static unsigned long long SIMTICK=0;
static unsigned long long svcGetSystemTick(void){ if(SIMTICK_SET) return SIMTICK; return (unsigned long long)ON*268111856ULL/32728ULL; }
enum{SOUND_ONE_SHOT=2<<10};
static void osSetSpeedupEnable(bool e){(void)e;} static Result CSND_FlushDataCache(const void* p,u32 n){(void)p;(void)n;return 0;}
enum{GSPLCD_SCREEN_TOP=1,GSPLCD_SCREEN_BOTTOM=2};
static Result gspLcdInit(void){return 0;} static void gspLcdExit(void){} static u32 BRIGHT[3]={0,3,3};
static Result GSPLCD_GetBrightness(u32 s,u32* b){*b=BRIGHT[s];return 0;} static Result GSPLCD_SetBrightness(u32 s,u32 b){BRIGHT[s]=b;return 0;}
typedef struct{s16 x,z,y;} angularRate; static Result HIDUSER_EnableGyroscope(void){return 0;} static Result HIDUSER_DisableGyroscope(void){return 0;}
static float TWISTV=0; static void hidGyroRead(angularRate* r){r->x=0;r->y=0;r->z=(s16)(TWISTV*1800);}
static bool gFakeHP = true;   /* headphones in: patch levels are measured without SPEAKER EQ AUTO */
static Result DSP_GetHeadphoneStatus(bool* h){*h=gFakeHP;return 0;} static Result DSP_InvalidateDataCache(const void*p,u32 n){(void)p;(void)n;return 0;}
static ndspWaveBuf* CAPWB=NULL; static void ndspSetCapture(ndspWaveBuf* w){CAPWB=w;}
typedef u32 Handle; enum{PORT_CAM1=1,SELECT_OUT1=1,SELECT_NONE=0,SIZE_QQVGA=2,OUTPUT_RGB_565=1,CONTEXT_A=1};
static Result camInit(void){return 0;} static void camExit(void){} static Result CAMU_StopCapture(u32 p){(void)p;return 0;} static Result CAMU_Activate(u32 s){(void)s;return 0;}
static Result CAMU_SetSize(u32 a,int b,int c){(void)a;(void)b;(void)c;return 0;} static Result CAMU_SetOutputFormat(u32 a,int b,int c){(void)a;(void)b;(void)c;return 0;}
static Result CAMU_SetNoiseFilter(u32 a,bool b){(void)a;(void)b;return 0;} static Result CAMU_SetAutoExposure(u32 a,bool b){(void)a;(void)b;return 0;} static Result CAMU_SetAutoWhiteBalance(u32 a,bool b){(void)a;(void)b;return 0;}
static Result CAMU_SetTrimming(u32 a,bool b){(void)a;(void)b;return 0;} static Result CAMU_GetMaxBytes(u32* m,s16 w,s16 h){(void)w;(void)h;*m=1024;return 0;} static Result CAMU_SetTransferBytes(u32 a,u32 b,s16 w,s16 h){(void)a;(void)b;(void)w;(void)h;return 0;}
static Result CAMU_ClearBuffer(u32 p){(void)p;return 0;} static Result CAMU_StartCapture(u32 p){(void)p;return 0;} static u16* CAMDST; static Result CAMU_SetReceiving(Handle* e,void* d,u32 p,u32 s,s16 t){(void)p;(void)s;(void)t;CAMDST=(u16*)d;*e=7;return 0;}
static int CAMT; static Result svcWaitSynchronization(Handle h,long long t){(void)t; if(h==7&&CAMDST&&(++CAMT%4==0)){ for(int y=0;y<120;y++)for(int x=0;x<160;x++){ float v=0.5f+0.5f*sinf(x*0.08f+CAMT*0.05f)*cosf(y*0.05f+CAMT*0.02f); int g=(int)(v*63); CAMDST[y*160+x]=(u16)(((g>>1)<<11)|(g<<5)|(g>>1)); } return 0;} return 1;} static Result svcCloseHandle(Handle h){(void)h;return 0;} static void svcSleepThread(long long n){(void)n;}
typedef struct{int x;} udsBindContext; typedef struct{int x;} udsNetworkStruct; typedef struct{udsNetworkStruct network;} udsNetworkScanInfo; typedef struct{u8 total_nodes;} udsConnectionStatus;
#define UDS_BROADCAST_NETWORKNODEID 0xFFFF
#define UDS_DEFAULT_RECVBUFSIZE 0x2E30
enum{UDS_SENDFLAG_Default=1,UDSCONTYPE_Client=1};
static Result udsInit(size_t a,const char* b){(void)a;(void)b;return -1;} static void udsExit(void){} static void udsGenerateDefaultNetworkStruct(udsNetworkStruct* n,u32 a,u8 b,u8 c){(void)n;(void)a;(void)b;(void)c;}
static Result udsCreateNetwork(const udsNetworkStruct* n,const void* p,size_t ps,udsBindContext* c,u8 d,u32 r){(void)n;(void)p;(void)ps;(void)c;(void)d;(void)r;return -1;}
static Result udsScanBeacons(void* o,size_t m,udsNetworkScanInfo** n,size_t* t,u32 a,u8 b,const u8* c,bool d){(void)o;(void)m;(void)n;(void)a;(void)b;(void)c;(void)d;*t=0;return -1;}
static Result udsConnectNetwork(const udsNetworkStruct* n,const void* p,size_t ps,udsBindContext* c,u16 r,int t,u8 d,u32 rb){(void)n;(void)p;(void)ps;(void)c;(void)r;(void)t;(void)d;(void)rb;return -1;}
static Result udsDestroyNetwork(void){return 0;} static Result udsDisconnectNetwork(void){return 0;} static Result udsUnbind(udsBindContext* c){(void)c;return 0;}
static Result udsSendTo(u16 a,u8 b,u8 c,const void* d,size_t e){(void)a;(void)b;(void)c;(void)d;(void)e;return 0;}
static Result udsPullPacket(const udsBindContext* c,void* b,size_t s,size_t* a,u16* n){(void)c;(void)b;(void)s;(void)a;(void)n;return -1;}
static Result udsGetConnectionStatus(udsConnectionStatus* o){o->total_nodes=0;return 0;}
static Result iruInit(u32* m,u32 s){(void)m;(void)s;return -1;} static void iruExit(void){} static Result iruSendData(u8* b,u32 s,bool w){(void)b;(void)s;(void)w;return 0;}
static Result iruRecvData(u8* b,u32 s,u8 f,u32* c,bool w){(void)b;(void)s;(void)f;(void)w;*c=0;return -1;}
static void dspFrameEnd(void){
  if(CAPWB){ int o=CAPWB->offset; for(int i=0;i<160;i++){ int k=(ON-160+i); if(k<0)k=0; ((s16*)CAPWB->data_pcm16)[(o+i)*2]=OUT[k*2]; ((s16*)CAPWB->data_pcm16)[(o+i)*2+1]=OUT[k*2+1]; } CAPWB->offset+=160; if(CAPWB->offset>=CAPWB->nsamples) CAPWB->offset=0; }
  for(int b=0;b<2;b++){ if(!AUXEN[b]||!AUXCB[b]||AUXDEAD){ memset(RET[b],0,sizeof RET[b]); continue; }
    static s32 buf[4][160]; for(int c=0;c<2;c++) for(int i=0;i<160;i++) buf[c][i]=(s32)(AXS[b][c][i]*32768); memset(buf[2],0,640); memset(buf[3],0,640);
    void* p[4]={buf[0],buf[1],buf[2],buf[3]}; AUXCB[b](NULL,160,p); for(int c=0;c<2;c++) for(int i=0;i<160;i++) RET[b][c][i]=buf[c][i]/32768.0f; }
  memset(AXS,0,sizeof AXS); if(NCB) NCB(NULL); }
static void renderFrame(void){ int n=32728/60; for(int s=0;s<n;s++){ float L=0,R=0; for(int c=0;c<24;c++){ Ch* h=&CH[c]; if(!h->wb) continue; const s16* d=h->wb->data_vaddr; int N=h->wb->nsamples;
  int i=(int)h->pos; float u=h->pos-i; float x=(d[i%N]*(1-u)+d[(i+1)%N]*u)/32768.0f; h->pos+=h->rate/32728.0; if(h->pos>=N){ if(h->wb->status==NDSP_WBUF_PLAYING && !h->wb->looping && (h->nq||c==21||c==22)){ h->wb->status=NDSP_WBUF_DONE; if(h->nq){ h->wb=h->q[0]; memmove(h->q,h->q+1,--h->nq*sizeof(void*)); h->wb->status=NDSP_WBUF_PLAYING; h->pos=0;} else { h->wb=NULL; continue; } } else h->pos-=N*floor(h->pos/N);}
  if(h->bq && h->cq){ int xi=(int)(x*32767); long long acc=(long long)h->cp[4]*xi+(long long)h->cp[3]*h->x1+(long long)h->cp[2]*h->x2+(long long)h->cp[1]*h->y1+(long long)h->cp[0]*h->y2;
    long long y=acc>>14; if(y>32767)y=32767; if(y<-32768)y=-32768; h->x2=h->x1; h->x1=xi; h->y2=h->y1; h->y1=(int)y; x=(float)y/32768.0f; }
  else if(h->bq){ float y=h->b0*x+h->z1; h->z1=h->b1*x-h->a1*y+h->z2; h->z2=h->b2*x-h->a2*y; x=y; }
  L+=x*h->mix[0]; R+=x*h->mix[1]; AXS[0][0][FPOS]+=x*h->mix[4]; AXS[0][1][FPOS]+=x*h->mix[5]; AXS[1][0][FPOS]+=x*h->mix[8]; AXS[1][1][FPOS]+=x*h->mix[9]; }
  L+=RET[0][0][FPOS]+RET[1][0][FPOS]; R+=RET[0][1][FPOS]+RET[1][1][FPOS]; if(++FPOS==160){FPOS=0; dspFrameEnd();}
  float CL=0,CR=0;
  for(int c=0;c<32;c++){ CsCh* h=&CS[c]; if(!h->on||(!h->d&&h->fmt!=3)) continue; float x=csNext(h); L+=x*h->l; R+=x*h->r; CL+=x*h->cl; CR+=x*h->cr; }
  for(int u=0;u<2;u++) if(CAPU[u].on&&CAPU[u].b){ float v=u?CR:CL; if(v>1)v=1; if(v<-1)v=-1; CAPU[u].b[CAPU[u].pos]=(s16)(v*32767); if(++CAPU[u].pos>=CAPU[u].n) CAPU[u].pos=0; }
  if(L>1)L=1; if(L<-1)L=-1; if(R>1)R=1; if(R<-1)R=-1; if(ON<32728*160){OUT[ON*2]=L*32767;OUT[ON*2+1]=R*32767;ON++;} } }
static void gspWaitForVBlank(void){ renderFrame(); }
static void SCRIPT(int fr);
static bool aptMainLoop(void){ HDPREV=HD; SCRIPT(FR); FR++; return FR<FRMAX; }
static Result IRU_SetBitRate(u8 v){(void)v;return 0;}
static Result socInit(u32* m,u32 n){(void)m;(void)n;return 0;} static Result socExit(void){return 0;}
#ifndef FAKE_EXTRA2
#define FAKE_EXTRA2
typedef int64_t s64;
#define SYSCLOCK_ARM11 268111856u
typedef int APT_HookType; typedef struct { int x; } aptHookCookie;
enum { APTHOOK_ONSUSPEND, APTHOOK_ONRESTORE, APTHOOK_ONSLEEP, APTHOOK_ONWAKEUP, APTHOOK_ONEXIT };
typedef void (*aptHookFn)(APT_HookType, void*);
static inline void aptHook(aptHookCookie* c, aptHookFn f, void* p) { (void)c; (void)f; (void)p; }
static inline void aptUnhook(aptHookCookie* c) { (void)c; }
#endif
/* custom biquad as the DSP runs it: Q14 coefficients (as libctru packs them), 16-bit samples, saturating output */
static int fakeQ14(float v){ float q=v*16384.0f; if(q>32767)q=32767; if(q<-32768)q=-32768; return (int)lrintf(q); }
static inline bool ndspChnIirBiquadSetParamsCustomFilter(int id,float a0,float a1,float a2,float b0,float b1,float b2){
	Ch* h=&CH[id]; h->cp[0]=fakeQ14(-a2/a0); h->cp[1]=fakeQ14(-a1/a0); h->cp[2]=fakeQ14(b2/a0); h->cp[3]=fakeQ14(b1/a0); h->cp[4]=fakeQ14(b0/a0);
	if(!h->cq){ h->x1=h->x2=h->y1=h->y2=0; } h->cq=1; return true; }
static inline void ndspChnSetAdpcmCoefs(int id,u16* c){(void)id;(void)c;}
static inline int LightEvent_WaitTimeout(LightEvent* e, s64 t){   /* real timed wait: 0 = signalled, 1 = timed out */
	struct timespec ts; clock_gettime(CLOCK_REALTIME,&ts); ts.tv_nsec+=t%1000000000LL; ts.tv_sec+=t/1000000000LL+ts.tv_nsec/1000000000L; ts.tv_nsec%=1000000000L;
	pthread_mutex_lock(&e->m); int r=0; while(!e->sig){ if(pthread_cond_timedwait(&e->c,&e->m,&ts)){ r=1; break; } } if(!r) e->sig=0; pthread_mutex_unlock(&e->m); return r; }
#ifndef FS_WRITE_FLUSH
#define FS_WRITE_FLUSH 1
#define MEDIATYPE_SD 1
static Result FSFILE_Write(Handle h,u32*w,u64 o,const void*d,u32 n,u32 f){(void)h;(void)o;(void)d;(void)f;*w=n;return 0;}
static Result amInit(void){return -1;} static void amExit(void){}
static Result AM_StartCiaInstall(int m,Handle*h){(void)m;(void)h;return -1;} static Result AM_FinishCiaInstall(Handle h){(void)h;return 0;} static Result AM_CancelCIAInstall(Handle h){(void)h;return 0;}
#endif
#ifndef FAKE_CORE2_STUBS
#define FAKE_CORE2_STUBS
static u32 FAKE_CPU_LIMIT=30; static Result APT_GetAppCpuTimeLimit(u32*p){*p=FAKE_CPU_LIMIT;return 0;} static bool envIsHomebrew(void){return 1;}
static Result svcCreateThread(Handle*h,void(*f)(void*),u32 a,u32*st,s32 pr,s32 id){(void)h;(void)f;(void)a;(void)st;(void)pr;(void)id;return -1;} static void svcExitThread(void){}
#endif
/* software keyboard (host test: always types TEST NAME) */
typedef struct { int dummy; } SwkbdState;
typedef enum { SWKBD_BUTTON_NONE = -1, SWKBD_BUTTON_LEFT = 0, SWKBD_BUTTON_MIDDLE, SWKBD_BUTTON_RIGHT, SWKBD_BUTTON_CONFIRM = SWKBD_BUTTON_RIGHT } SwkbdButton;
enum { SWKBD_TYPE_NORMAL = 0 }; enum { SWKBD_NOTEMPTY_NOTBLANK = 3 };
static inline void swkbdInit(SwkbdState* s, int t, int b, int m) { (void)s; (void)t; (void)b; (void)m; }
static inline void swkbdSetHintText(SwkbdState* s, const char* t) { (void)s; (void)t; }
static inline void swkbdSetInitialText(SwkbdState* s, const char* t) { (void)s; (void)t; }
static inline void swkbdSetValidation(SwkbdState* s, int a, int b, int c) { (void)s; (void)a; (void)b; (void)c; }
static inline SwkbdButton swkbdInputText(SwkbdState* s, char* o, size_t n) { (void)s; snprintf(o, n, "test name \xc3\xa9"); return SWKBD_BUTTON_CONFIRM; }
/* model queries and DSP health */
static int FAKE_NEW3DS = 0; static Result APT_CheckNew3DS(bool* o) { *o = FAKE_NEW3DS; return 0; }
static u32 ndspGetDroppedFrames(void) { return 0; } static u32 ndspGetFrameCount(void) { return 0; }
#define NDSP_SAMPLE_RATE (16756991u / 512.0)
int __system_argc = 0; char** __system_argv = NULL;   /* launch arguments (none in the simulator) */
