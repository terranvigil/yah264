/* Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * reconcmp -- does the bitstream decode to what the encoder built?
 *
 * WHY THIS EXISTS. The conformance gate answers exactly this question and
 * cannot reach the threaded path: --dump-recon forces the serial streaming
 * loop, because per-GOP encoders cannot produce one continuous self-consistent
 * recon stream. So every recon-match result we have describes serial encoding,
 * and a defect that only appears with threads on is invisible to all 476 of
 * them. One did: the RCP_LAG=1 default emitted, on ABR with threads > 1 and
 * bframes >= 2, a stream whose B frames do not decode to the encoder's own
 * reconstruction. 46 of 60 frames wrong, and every gate passed.
 *
 * The recon CALLBACK has no such restriction. It fires once per emitted frame
 * on any path, with the frame's display index, which is all a comparison needs.
 * This tool drives the library directly, captures recon by display index,
 * writes the bitstream beside it, and the caller decodes and diffs.
 *
 *   reconcmp <in.y4m> <out.264> <recon.yuv> <frames> <threads> <bitrate-kbps>
 *
 * Then decode out.264 to raw yuv420p and compare against recon.yuv frame by
 * frame. Any differing frame is a conformance break: the encoder predicted from
 * a picture the decoder cannot reproduce, and the error accumulates from there.
 *
 * 8-bit 4:2:0 only, which is what the defect needed. Widen when something wants
 * it rather than in advance.
 */
#include <yah264.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FILE *rf; static int W,H,wrote;
static long fsz(void){ return (long)W*H*3/2; }

static void on_recon(void *ud, const yah264_picture_t *r, int disp)
{
    (void)ud;
    /* write recon frames indexed by DISPLAY order into a sparse file */
    fseek(rf, (long)disp * fsz(), SEEK_SET);
    for (int y=0;y<H;y++) fwrite(r->plane[0]+(size_t)y*r->stride[0],1,W,rf);
    for (int c=1;c<3;c++)
        for (int y=0;y<H/2;y++) fwrite(r->plane[c]+(size_t)y*r->stride[c],1,W/2,rf);
    if (disp+1>wrote) wrote=disp+1;
}

int main(int argc,char**argv)
{
    const char *y4m=argv[1], *out=argv[2], *reconp=argv[3];
    int frames=atoi(argv[4]), threads=atoi(argv[5]), bitrate=atoi(argv[6]);
    FILE *f=fopen(y4m,"rb"); if(!f){perror("in");return 1;}
    char hdr[512]; if(!fgets(hdr,sizeof hdr,f)) return 1;
    char *p=strstr(hdr," W"); W=p?atoi(p+2):0;
    p=strstr(hdr," H"); H=p?atoi(p+2):0;
    if(W<=0||H<=0){fprintf(stderr,"bad y4m header\n");return 1;}

    yah264_param_t pm; yah264_param_default(&pm);
    yah264_param_apply_preset(&pm,"medium");
    pm.width=W; pm.height=H; pm.csp=YAH264_CSP_I420;
    pm.timebase.fps_num=30; pm.timebase.fps_den=1;
    pm.rc.method=YAH264_RC_ABR; pm.rc.bitrate=bitrate;
    /* RECONCMP_CRF=<n> switches to CRF, which is not cosmetic: the staircase
     * clamp requires !abr_on, so an ABR-only gate cannot reach the staircase
     * path at all and every stair defect is invisible to it. */
    { const char *cq=getenv("RECONCMP_CRF");
      if(cq&&*cq){ pm.rc.method=YAH264_RC_CRF; pm.rc.rf=atof(cq); } }
    pm.bframes=3; pm.threads=threads;
    /* RECONCMP_DIRECT=temporal: arm the B direct mode from the environment, so
     * the threaded recon gate can reach a path the CLI-only flag otherwise hides. */
    { const char *d=getenv("RECONCMP_DIRECT");
      if(d&&!strcmp(d,"temporal")) pm.direct=YAH264_DIRECT_TEMPORAL;
      else if(d&&!strcmp(d,"spatial")) pm.direct=YAH264_DIRECT_SPATIAL; }

    yah264_encoder_t *e=yah264_encoder_open(&pm);
    if(!e){fprintf(stderr,"open failed\n");return 1;}
    rf=fopen(reconp,"wb+"); if(!rf){perror("recon");return 1;}
    yah264_encoder_set_recon_cb(e,on_recon,NULL);

    FILE *of=fopen(out,"wb");
    {   /* parameter sets first, or nothing decodes -- the same omission that
         * shipped in the ffmpeg wrapper. */
        yah264_nal_t *hn=NULL; int hc=0;
        if(yah264_encoder_headers(e,&hn,&hc)==0)
            for(int k=0;k<hc;k++) fwrite(hn[k].payload,1,hn[k].size,of);
    }
    unsigned char *buf=malloc(fsz());
    char fr[64]; int n=0;
    while(n<frames && fgets(fr,sizeof fr,f) && fread(buf,1,fsz(),f)==(size_t)fsz()){
        yah264_picture_t pic; memset(&pic,0,sizeof pic);
        pic.csp=YAH264_CSP_I420; pic.width=W; pic.height=H; pic.pts=n;
        pic.plane[0]=buf; pic.plane[1]=buf+(size_t)W*H; pic.plane[2]=buf+(size_t)W*H*5/4;
        pic.stride[0]=W; pic.stride[1]=W/2; pic.stride[2]=W/2;
        yah264_nal_t *nal=NULL; int cnt=0;
        if(yah264_encoder_encode(e,&nal,&cnt,&pic)<0){fprintf(stderr,"encode fail @%d\n",n);break;}
        for(int k=0;k<cnt;k++) fwrite(nal[k].payload,1,nal[k].size,of);
        n++;
    }
    for(;;){ yah264_nal_t *nal=NULL; int cnt=0;
        int r=yah264_encoder_encode(e,&nal,&cnt,NULL);
        if(r<0||(r==0&&cnt==0)) break;
        for(int k=0;k<cnt;k++) fwrite(nal[k].payload,1,nal[k].size,of); }
    fprintf(stderr,"  fed %d frames, recon frames written %d, %dx%d\n",n,wrote,W,H);
    yah264_encoder_close(e); fclose(of); fclose(rf); free(buf); fclose(f);
    return 0;
}
