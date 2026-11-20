// harness stand-in for citro3d/citro2d: a small software rasterizer
typedef struct { struct { void* colorBuf; int width, height; } frameBuf; } C3D_RenderTarget;
#define C3D_DEFAULT_CMDBUF_SIZE 0x40000
#define C2D_DEFAULT_MAX_OBJECTS 4096
#define C3D_FRAME_NONBLOCK 2
enum { GPU_RB_RGBA8, GPU_RB_DEPTH16, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE, GPU_ONE_MINUS_SRC_ALPHA };
#define GX_BUFFER_DIM(w,h) (((h)<<16)|((w)&0xFFFF))
#define GX_TRANSFER_FLIP_VERT(x) 0
#define GX_TRANSFER_OUT_TILED(x) 0
#define GX_TRANSFER_RAW_COPY(x) 0
#define GX_TRANSFER_IN_FORMAT(x) 0
#define GX_TRANSFER_OUT_FORMAT(x) 0
#define GX_TRANSFER_SCALING(x) 0
enum { GX_TRANSFER_FMT_RGBA8, GX_TRANSFER_FMT_RGB8, GX_TRANSFER_SCALE_XY };
static float *CV; static int CVW, CVH, CADD;   // canvas in scene coords
static int TRIS=0;
static bool C3D_Init(int n){(void)n;return 1;} static void C3D_Fini(void){}
static bool C2D_Init(int n){(void)n;return 1;} static void C2D_Fini(void){} static void C2D_Prepare(void){} static void C2D_Flush(void){}
static C3D_RenderTarget* C3D_RenderTargetCreate(int w,int h,int a,int b){(void)a;(void)b; C3D_RenderTarget* t=calloc(1,sizeof *t); t->frameBuf.width=w; t->frameBuf.height=h; t->frameBuf.colorBuf=calloc(h*w*3,sizeof(float)); return t;}
static void C3D_RenderTargetDelete(C3D_RenderTarget* t){free(t);}
static bool C3D_FrameBegin(int f){(void)f;return 1;} static void C3D_FrameEnd(int f){(void)f;}
static void C2D_SceneBegin(C3D_RenderTarget* t){CV=t->frameBuf.colorBuf; CVW=t->frameBuf.height; CVH=t->frameBuf.width;} static void C2D_SceneSize(int w,int h,bool t){(void)w;(void)h;(void)t;}
static void C3D_AlphaBlend(int a,int b,int c,int d,int e,int f){(void)a;(void)b;(void)e;(void)f;(void)c; CADD=(d==GPU_ONE);}
static inline u32 C2D_Color32(u8 r,u8 g,u8 b,u8 a){return r|(g<<8)|(b<<16)|((u32)a<<24);}
static inline u32 C2D_Color32f(float r,float g,float b,float a){return C2D_Color32(r*255,g*255,b*255,a*255);}
static void C2D_TargetClear(C3D_RenderTarget* t,u32 c){C2D_SceneBegin(t); for(int i=0;i<CVW*CVH;i++){CV[i*3]=(c&255)/255.f;CV[i*3+1]=((c>>8)&255)/255.f;CV[i*3+2]=((c>>16)&255)/255.f;}}
static void plot(int x,int y,u32 c){ if(x<0||y<0||x>=CVW||y>=CVH) return; float a=(c>>24)/255.f; float* p=CV+(y*CVW+x)*3; float col[3]={(c&255)/255.f,((c>>8)&255)/255.f,((c>>16)&255)/255.f};
  for(int k=0;k<3;k++){ p[k]= CADD? p[k]+col[k]*a : p[k]*(1-a)+col[k]*a; if(p[k]>1)p[k]=1; } }
static float ed(float ax,float ay,float bx,float by,float px,float py){return (bx-ax)*(py-ay)-(by-ay)*(px-ax);}
static bool C2D_DrawTriangle(float x0,float y0,u32 c0,float x1,float y1,u32 c1,float x2,float y2,u32 c2,float d){(void)c1;(void)c2;(void)d; TRIS++;
  int mnx=floorf(fminf(x0,fminf(x1,x2))), mxx=ceilf(fmaxf(x0,fmaxf(x1,x2))), mny=floorf(fminf(y0,fminf(y1,y2))), mxy=ceilf(fmaxf(y0,fmaxf(y1,y2)));
  float A=ed(x0,y0,x1,y1,x2,y2); if(fabsf(A)<1e-6) return 1;
  for(int y=mny;y<=mxy;y++) for(int x=mnx;x<=mxx;x++){ float px=x+.5f,py=y+.5f; float w0=ed(x1,y1,x2,y2,px,py)/A,w1=ed(x2,y2,x0,y0,px,py)/A,w2=ed(x0,y0,x1,y1,px,py)/A; if(w0>=0&&w1>=0&&w2>=0) plot(x,y,c0);} return 1;}
static bool C2D_DrawLine(float x0,float y0,u32 c0,float x1,float y1,u32 c1,float th,float d){ float dx=x1-x0,dy=y1-y0,l=sqrtf(dx*dx+dy*dy); if(l<1e-4) return 1; float nx=-dy/l*th/2, ny=dx/l*th/2;
  C2D_DrawTriangle(x0+nx,y0+ny,c0,x1+nx,y1+ny,c1,x1-nx,y1-ny,c1,d); C2D_DrawTriangle(x0+nx,y0+ny,c0,x1-nx,y1-ny,c1,x0-nx,y0-ny,c0,d); return 1;}
static bool C2D_DrawRectSolid(float x,float y,float z,float w,float h,u32 c){(void)z; for(int yy=(int)y;yy<(int)(y+h);yy++) for(int xx=(int)x;xx<(int)(x+w);xx++) plot(xx,yy,c); return 1;}
static void C3D_SyncDisplayTransfer(u32* in,u32 indim,u32* out,u32 outdim,u32 fl){(void)fl; CV=(float*)in; CVW=indim>>16; CVH=indim&0xFFFF; (void)outdim; int W=(indim>>16)/2, H=(indim&0xFFFF)/2; u8* o=(u8*)out; // scaled: half of input
  for(int sx=0;sx<W;sx++) for(int sy=0;sy<H;sy++){ float c[3]={0,0,0}; for(int a=0;a<2;a++) for(int b=0;b<2;b++){ float* p=CV+((sy*2+b)*CVW+sx*2+a)*3; for(int k=0;k<3;k++) c[k]+=p[k]/4; }
    u8* q=o+(sx*240+239-sy)*3; q[0]=c[2]*255; q[1]=c[1]*255; q[2]=c[0]*255; } }
static void GSPGPU_InvalidateDataCache(void* p,u32 n){(void)p;(void)n;}

static void C3D_SyncTextureCopy(u32* in,u32 a,u32* out,u32 b,u32 size,u32 f){(void)a;(void)b;(void)f; memcpy(out,in,size);}
#ifndef GX_TRANSFER_RAW_COPY
#define GX_TRANSFER_RAW_COPY(x) ((x)<<3)
#endif
static void GSPGPU_FlushDataCache(void* p,u32 n){(void)p;(void)n;}

/* procedural texture + immediate-mode quad, emulated in software (only the parts PLASMA uses: noise-bent
   sqrt(u^2+v^2) with mirrored repeat, an RGB map table and a colour table). The real noise differs in detail. */
typedef struct { float amp, freq, phase; int noise; int fn; } C3D_ProcTex;
typedef u32 C3D_ProcTexLut[128];
typedef struct { u32 color[256]; u32 diff[256]; } C3D_ProcTexColorLut;
enum { C3D_ProcTex_U = 1, C3D_ProcTex_V = 2, C3D_ProcTex_UV = 3 };
enum { GPU_PT_CLAMP_TO_ZERO, GPU_PT_CLAMP_TO_EDGE, GPU_PT_REPEAT, GPU_PT_MIRRORED_REPEAT, GPU_PT_PULSE };
enum { GPU_PT_U, GPU_PT_U2, GPU_PT_V, GPU_PT_V2, GPU_PT_ADD, GPU_PT_ADD2, GPU_PT_SQRT2, GPU_PT_MIN, GPU_PT_MAX, GPU_PT_RMAX };
enum { GPU_PT_NEAREST, GPU_PT_LINEAR };
typedef enum { GPU_LUT_NOISE = 0, GPU_LUT_RGBMAP = 2, GPU_LUT_ALPHAMAP = 3, GPU_LUT_COLOR = 4, GPU_LUT_COLORDIF = 5 } GPU_PROCTEX_LUTID;
static C3D_ProcTex* FPT; static float FMAP[129]; static u32 FCOL[256]; static int FCOLN = 128;
static void C3D_ProcTexInit(C3D_ProcTex* p, int off, int len) { memset(p, 0, sizeof *p); (void)off; FCOLN = len; }
static inline void C3D_ProcTexClamp(C3D_ProcTex* p, int u, int v) { (void)p; (void)u; (void)v; }
static inline void C3D_ProcTexCombiner(C3D_ProcTex* p, bool s, int rgb, int a) { (void)s; (void)a; p->fn = rgb; }
static inline void C3D_ProcTexFilter(C3D_ProcTex* p, int f) { (void)p; (void)f; }
static inline void C3D_ProcTexNoiseEnable(C3D_ProcTex* p, bool e) { p->noise = e; }
static void C3D_ProcTexNoiseCoefs(C3D_ProcTex* p, int mode, float a, float f, float ph) { (void)mode; p->amp = a; p->freq = f; p->phase = ph; }
static void C3D_ProcTexBind(int tc, C3D_ProcTex* p) { (void)tc; FPT = p; }
static float FMAPSRC[129]; static void ProcTexLut_FromArray(C3D_ProcTexLut* l, const float in[129]) { (void)l; memcpy(FMAPSRC, in, sizeof FMAPSRC); }
static void C3D_ProcTexLutBind(GPU_PROCTEX_LUTID id, C3D_ProcTexLut* l) { (void)l; if (id == GPU_LUT_RGBMAP) memcpy(FMAP, FMAPSRC, sizeof FMAP); }
static void ProcTexColorLut_Write(C3D_ProcTexColorLut* o, const u32* in, int off, int n) { for (int i = 0; i < n; i++) o->color[off + i] = in[i]; }
static void C3D_ProcTexColorLutBind(C3D_ProcTexColorLut* l) { memcpy(FCOL, l->color, sizeof FCOL); }
typedef struct { int x; } C3D_TexEnv; static C3D_TexEnv FENV[6];
enum { C3D_RGB = 1, C3D_Alpha = 2, C3D_Both = 3 }; enum { GPU_PRIMARY_COLOR = 0, GPU_TEXTURE3 = 13, GPU_PREVIOUS = 15 }; enum { GPU_REPLACE = 0 }; enum { GPU_TRIANGLE_STRIP = 1 };
static C3D_TexEnv* C3D_GetTexEnv(int i) { return &FENV[i]; }
static void C3D_TexEnvInit(C3D_TexEnv* e) { (void)e; } static void C3D_TexEnvSrc(C3D_TexEnv* e, int m, int a, int b, int c) { (void)e; (void)m; (void)a; (void)b; (void)c; }
static void C3D_TexEnvFunc(C3D_TexEnv* e, int m, int f) { (void)e; (void)m; (void)f; }
static float FIMM[4][4][4]; static int FIMMN = 0;
static void C3D_ImmDrawBegin(int prim) { (void)prim; FIMMN = 0; }
static void C3D_ImmSendAttrib(float x, float y, float z, float w) { if (FIMMN < 16) { float* a = FIMM[FIMMN / 4][FIMMN % 4]; a[0] = x; a[1] = y; a[2] = z; a[3] = w; } FIMMN++; }
static float fnoise(float x, float y) { return sinf(x * 2.1f + sinf(y * 1.7f)) * cosf(y * 2.3f - sinf(x * 1.3f)); }
static void C3D_ImmDrawEnd(void) {   /* one axis-aligned quad: strip order TL, TR, BL, BR */
	if (!FPT || FIMMN < 16) return;
	float x0 = FIMM[0][0][0], y0 = FIMM[0][0][1], x1 = FIMM[3][0][0], y1 = FIMM[3][0][1];
	float u0 = FIMM[0][2][0], v0 = FIMM[0][2][1], u1 = FIMM[3][2][0], v1 = FIMM[3][2][1];
	for (int y = (int)y0; y < (int)y1; y++) for (int x = (int)x0; x < (int)x1; x++) {
		float tx = (x - x0) / (x1 - x0), ty = (y - y0) / (y1 - y0), u = u0 + (u1 - u0) * tx, v = v0 + (v1 - v0) * ty;
		if (FPT->noise) { float f = FPT->freq * 8; u += FPT->amp * fnoise(u * f + FPT->phase, v * f); v += FPT->amp * fnoise(v * f - FPT->phase, u * f + 3); }
		u = fabsf(u); v = fabsf(v); u = fmodf(u, 2); if (u > 1) u = 2 - u; v = fmodf(v, 2); if (v > 1) v = 2 - v;   /* mirrored repeat */
		float g = sqrtf(u * u + v * v); if (g > 1) g = 1;
		float m = FMAP[(int)(g * 128)]; int ci = (int)(m * (FCOLN - 1)); if (ci < 0) ci = 0; if (ci >= FCOLN) ci = FCOLN - 1;
		plot(x, y, FCOL[ci] | 0xFF000000u);
	}
}
