/* nv12_flip — NV12 主平面直接 page flip（硬體 YUV / CSC）
 * 3 個 GBM NV12 緩衝，每幀填色條（BT.709 limited）並 flip。
 * 用法：sudo nv12_flip [card] [hz] [frames]
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <gbm.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
#define W 1920
#define H 1080
#define NB 3
static volatile int flipped;
static void on_event(int fd,unsigned s,unsigned a,unsigned b,void*d){(void)fd;(void)s;(void)a;(void)b;(void)d;flipped=1;}
static double now_ms(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1000.0+t.tv_nsec/1e6;}
static drmModeConnector *pick(int fd,drmModeRes **ro){
    drmModeRes *r=drmModeGetResources(fd); if(!r)return NULL; *ro=r;
    for(int i=0;i<r->count_connectors;i++){drmModeConnector*c=drmModeGetConnector(fd,r->connectors[i]);if(!c)continue;
        if(c->connection==DRM_MODE_CONNECTED&&c->count_modes>0)return c; drmModeFreeConnector(c);} return NULL;}
typedef struct { struct gbm_bo *bo; uint32_t fb; uint8_t *base; uint32_t ys,yo,cs,co; } VB;
static void fill(VB *v,int off){
    static const int bars[8][3]={{235,235,235},{235,235,0},{0,235,235},{0,235,0},
                                 {235,0,235},{235,0,0},{0,0,235},{0,0,0}};
    static uint8_t yrow[W], uvrow[W];
    for(int x=0;x<W;x++){
        int bi=((x+off)%W)*8/W; if(bi>7)bi=7;
        int r=bars[bi][0],g=bars[bi][1],b=bars[bi][2];
        yrow[x]=(uint8_t)(16+(int)(0.1826*r+0.6142*g+0.0620*b));
        uvrow[x&~1]=(uint8_t)(128+(int)(-0.1006*r-0.3386*g+0.4392*b));
        uvrow[x|1]  =(uint8_t)(128+(int)(0.4392*r-0.3989*g-0.0403*b));
    }
    for(int y=0;y<H;y++) memcpy(v->base+v->yo+(size_t)y*v->ys, yrow, W);
    for(int y=0;y<H/2;y++) memcpy(v->base+v->co+(size_t)y*v->cs, uvrow, W);
}
int main(int argc,char**argv){
    setvbuf(stdout,NULL,_IONBF,0);
    const char*card=argc>1?argv[1]:"/dev/dri/card2";
    int hz=argc>2?atoi(argv[2]):120, frames=argc>3?atoi(argv[3]):600;
    int fd=open(card,O_RDWR|O_CLOEXEC); if(fd<0){perror("open");return 1;}
    if(drmSetMaster(fd)) fprintf(stderr,"drmSetMaster: %s\n",strerror(errno));
    drmModeRes *res=NULL; drmModeConnector*conn=pick(fd,&res); if(!conn){fprintf(stderr,"no conn\n");return 1;}
    drmModeEncoder*enc=drmModeGetEncoder(fd,conn->encoder_id);
    uint32_t crtc=enc?enc->crtc_id:0; if(!crtc) for(int i=0;i<res->count_crtcs;i++){crtc=res->crtcs[i];break;}
    drmModeModeInfo mode=conn->modes[0]; int best=-1000;
    for(int i=0;i<conn->count_modes;i++){drmModeModeInfo*m=&conn->modes[i];
        if(m->hdisplay!=W||m->vdisplay!=H||(m->flags&DRM_MODE_FLAG_INTERLACE))continue;
        int sc=hz?-abs((int)m->vrefresh-hz):(int)m->vrefresh; if(sc>best){best=sc;mode=*m;}}
    printf("NV12 flip: %dx%d@%d crtc=%u\n",mode.hdisplay,mode.vdisplay,mode.vrefresh,crtc);
    struct gbm_device*g=gbm_create_device(fd); if(!g){perror("gbm");return 1;}
    VB V[NB];
    for(int i=0;i<NB;i++){
        V[i].bo=gbm_bo_create(g,W,H,GBM_FORMAT_NV12,GBM_BO_USE_SCANOUT|GBM_BO_USE_LINEAR|GBM_BO_USE_WRITE);
        if(!V[i].bo){perror("gbm_bo_create");return 1;}
        V[i].ys=gbm_bo_get_stride_for_plane(V[i].bo,0); V[i].yo=gbm_bo_get_offset(V[i].bo,0);
        V[i].cs=gbm_bo_get_stride_for_plane(V[i].bo,1); V[i].co=gbm_bo_get_offset(V[i].bo,1);
        size_t sz=(size_t)V[i].co+(size_t)V[i].cs*(H/2);
        int dfd=gbm_bo_get_fd(V[i].bo);
        V[i].base=mmap(NULL,sz,PROT_READ|PROT_WRITE,MAP_SHARED,dfd,0);
        if(V[i].base==MAP_FAILED){perror("mmap");return 1;}
        fill(&V[i],i*80);
        uint32_t h[4]={gbm_bo_get_handle(V[i].bo).u32,gbm_bo_get_handle(V[i].bo).u32,0,0};
        uint32_t p[4]={V[i].ys,V[i].cs,0,0},o[4]={V[i].yo,V[i].co,0,0};
        if(drmModeAddFB2(fd,W,H,DRM_FORMAT_NV12,h,p,o,&V[i].fb,0)){perror("AddFB2");return 1;}
    }
    if(drmModeSetCrtc(fd,crtc,V[0].fb,0,0,&conn->connector_id,1,&mode)){perror("SetCrtc");return 1;}
    int cur=0,pending=-1,drops=0; double t0=now_ms(),last=t0,cpu=0;
    double exp=1000.0/mode.vrefresh;
    for(int f=1;f<=frames;f++){
        int b=(cur+1)%NB;
        double c0=now_ms(); fill(&V[b],(f*6)%W); cpu+=now_ms()-c0;
        if(pending>=0){
            flipped=0; struct pollfd pf={fd,POLLIN,0}; int gd=0;
            while(!flipped&&gd++<200){ if(poll(&pf,1,100)>0){drmEventContext ev={0};ev.version=2;ev.page_flip_handler=on_event;drmHandleEvent(fd,&ev);} }
            cur=pending; pending=-1;
            double now=now_ms(),gap=now-last; if(gap>exp*1.5)drops+=(int)(gap/exp+0.5)-1; last=now;
        }
        int frc=drmModePageFlip(fd,crtc,V[b].fb,DRM_MODE_PAGE_FLIP_EVENT,NULL);
        if(frc){perror("PageFlip");break;}
        pending=b;
    }
    double dt=now_ms()-t0;
    printf("%d 幀 / %.1f ms → %.1f fps ｜ 填充 %.3f ms/幀 ｜ 漏拍 %d\n",
           frames,dt,frames*1000.0/dt,cpu/frames,drops);
    return 0;
}
