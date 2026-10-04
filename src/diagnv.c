/* diagnv — on-device DIAG NV 550 (IMEI) client for ZTE MU5250 (SDX75), via libdiag.
 * Replicates vendor zte_nv init: Init -> register_callback -> ioctl(41) query owner ->
 * ioctl(7) switch logging mode 6 (become owner) -> logging_mode=6 -> ioctl(41) requery
 * -> send command -> receive via registered callback.
 *   diagnv read  <idx>
 *   diagnv write <idx> <256hex> --arm [--force]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <dlfcn.h>

#define PKT 138
#define PAYLOAD 128

static volatile int g_got = 0;
static unsigned char g_resp[512];
static int g_resp_len = 0;

static int deframe(const unsigned char *in, int n, unsigned char *out, int outcap) {
    unsigned char tmp[512]; int t = 0, i = 0;
    for (; i < n; i++) {
        unsigned char c = in[i];
        if (c == 0x7e) { if (t == 0) continue; else break; }
        if (c == 0x7d && i + 1 < n) { i++; c = in[i] ^ 0x20; }
        if (t < (int)sizeof(tmp)) tmp[t++] = c;
    }
    if (t < 3) return -1;
    uint16_t crc = 0xFFFF;
    for (int k = 0; k < t - 2; k++) { crc ^= tmp[k]; for (int b=0;b<8;b++) crc = (crc&1)?(crc>>1)^0x8408:(crc>>1); }
    crc ^= 0xFFFF;
    uint16_t got = tmp[t-2] | (tmp[t-1] << 8);
    if (crc != got) { fprintf(stderr,"FCS mismatch calc=%04x got=%04x len=%d\n",crc,got,t); return -2; }
    int plen = t - 2; if (plen > outcap) plen = outcap;
    memcpy(out, tmp, plen); return plen;
}

static void cb(const unsigned char *data, int len) {
    fprintf(stderr, "CB len=%d b0=%02x\n", len, (data&&len>0)?data[0]:0);
    if (!data || len <= 0) return;
    int p = deframe(data, len, g_resp, sizeof(g_resp));
    if (p > 0) { g_resp_len = p; g_got = 1; }
}

typedef int (*init_fn)(int);
typedef int (*reg_fn)(void*, int);
typedef int (*send_fn)(int, unsigned char*, int);
typedef int (*ioctl_fn)(int, int, void*, int);

static send_fn p_send;

static int do_exchange(unsigned char *req, int reqhdr, unsigned char *outpayload) {
    g_got = 0; g_resp_len = 0;
    for (int attempt = 0; attempt < 4 && !g_got; attempt++) {
        int r = p_send(0, req, PKT);
        if (attempt == 0) fprintf(stderr, "send ret=%d\n", r);
        for (int w = 0; w < 60 && !g_got; w++) usleep(10000);
    }
    if (!g_got) { fprintf(stderr, "no response\n"); return -1; }
    if (g_resp_len < reqhdr + PAYLOAD) { fprintf(stderr, "short resp %d\n", g_resp_len); return -1; }
    if (memcmp(g_resp, req, reqhdr) != 0) { fprintf(stderr, "echo mismatch\n"); return -1; }
    memcpy(outpayload, g_resp + reqhdr, PAYLOAD);
    return 0;
}

static int hx(int c){if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;if(c>='A'&&c<='F')return c-'A'+10;return -1;}

/* send an arbitrary (unframed) DIAG command, capture deframed response */
static int do_raw(unsigned char *req, int len, const char *tag) {
    g_got = 0; g_resp_len = 0;
    for (int attempt = 0; attempt < 4 && !g_got; attempt++) {
        int r = p_send(0, req, len);
        if (attempt == 0) fprintf(stderr, "%s send ret=%d\n", tag, r);
        for (int w = 0; w < 60 && !g_got; w++) usleep(10000);
    }
    if (!g_got) { fprintf(stderr, "%s no response\n", tag); return -1; }
    fprintf(stderr, "%s resp_len=%d bytes=", tag, g_resp_len);
    for (int i = 0; i < g_resp_len && i < 24; i++) fprintf(stderr, "%02x", g_resp[i]);
    fprintf(stderr, "\n");
    return 0;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 3) { fprintf(stderr,"usage: diagnv read <idx> | write <idx> <256hex> --arm [--force]\n"); return 2; }
    int do_write = !strcmp(argv[1], "write");
    int do_sec   = !strcmp(argv[1], "sec");   /* sec spc <6d> | sec pw <16hex> | sec both <6d> <16hex> */
    int index = do_sec ? 0 : atoi(argv[2]);
    unsigned char want[PAYLOAD]; int armed=0, force=0;
    const char *spc=NULL, *pw=NULL;
    if (do_write) {
        if (argc<4 || (int)strlen(argv[3])!=PAYLOAD*2){fprintf(stderr,"need %d hex\n",PAYLOAD*2);return 2;}
        for(int i=0;i<PAYLOAD;i++){int h=hx(argv[3][2*i]),l=hx(argv[3][2*i+1]);if(h<0||l<0){fprintf(stderr,"bad hex\n");return 2;}want[i]=(h<<4)|l;}
        for(int i=4;i<argc;i++){
            if(!strcmp(argv[i],"--arm"))armed=1;
            else if(!strcmp(argv[i],"--force"))force=1;
            else if(!strcmp(argv[i],"--spc")&&i+1<argc)spc=argv[++i];
            else if(!strcmp(argv[i],"--pw")&&i+1<argc)pw=argv[++i];
        }
    } else if (do_sec) {
        if(!strcmp(argv[2],"spc")&&argc>=4)spc=argv[3];
        else if(!strcmp(argv[2],"pw")&&argc>=4)pw=argv[3];
        else if(!strcmp(argv[2],"both")&&argc>=5){spc=argv[3];pw=argv[4];}
        else {fprintf(stderr,"usage: diagnv sec spc <6d> | pw <16hex> | both <6d> <16hex>\n");return 2;}
    }

    void *lib = dlopen("/usr/lib/libdiag.so.1", RTLD_NOW);
    if (!lib){fprintf(stderr,"dlopen %s\n",dlerror());return 1;}
    init_fn  p_init = (init_fn) dlsym(lib,"Diag_LSM_Init");
    reg_fn   p_reg  = (reg_fn)  dlsym(lib,"diag_register_callback");
    p_send          = (send_fn) dlsym(lib,"diag_callback_send_data");
    ioctl_fn p_ioc  = (ioctl_fn)dlsym(lib,"diag_lsm_comm_ioctl");
    int     *p_fd   = (int*)    dlsym(lib,"diag_fd");
    int     *p_lm   = (int*)    dlsym(lib,"logging_mode");
    if(!p_init||!p_reg||!p_send||!p_ioc||!p_fd){fprintf(stderr,"dlsym missing\n");return 1;}
    if(!p_init(0)){fprintf(stderr,"Init failed\n");return 1;}
    p_reg((void*)cb, 0);
    fprintf(stderr,"logging_mode init=%d fd=%d\n", p_lm?*p_lm:-1, *p_fd);

    unsigned char qstruct[24]={0x02,0,0,0, 0,0,0,0, 0xea,0xff,0xff,0xff, 0x01,0,0,0, 0,0,0,0, 0,0,0,0};
    unsigned char sstruct[24]={0x06,0,0,0, 0x02,0,0,0, 0,0,0,0, 0x01,0,0,0, 0xea,0xff,0xff,0xff, 0x01,0,0,0};
    int r1=p_ioc(*p_fd, 41, qstruct, 24);
    fprintf(stderr,"ioctl(41) query ret=%d owner=%d\n", r1, *(int*)(qstruct+8));
    int r2=p_ioc(*p_fd, 7, sstruct, 24);
    fprintf(stderr,"ioctl(7) switch ret=%d\n", r2);
    if(p_lm)*p_lm=6;
    unsigned char q2[24]={0x02,0,0,0, 0,0,0,0, 0xea,0xff,0xff,0xff, 0x01,0,0,0, 0,0,0,0, 0,0,0,0};
    int r3=p_ioc(*p_fd, 41, q2, 24);
    fprintf(stderr,"ioctl(41) requery ret=%d owner=%d mypid=%d\n", r3, *(int*)(q2+8), getpid());

    /* optional security unlock before NV access, in the SAME diag session */
    if (spc) {
        unsigned char s[16]; s[0]=0x25; int n=strlen(spc);
        for(int i=0;i<6;i++) s[1+i]=(i<n)?spc[i]:'0';
        do_raw(s,7,"SPC");
        fprintf(stderr,"SPC status=%02x\n", g_resp_len>=2?g_resp[1]:0xff);
    }
    if (pw) {
        unsigned char s[16]; s[0]=0x41;
        for(int i=0;i<8;i++){int h=hx(pw[2*i]),l=hx(pw[2*i+1]);s[1+i]=((h<0?0:h)<<4)|(l<0?0:l);}
        do_raw(s,9,"PW");
        fprintf(stderr,"PW status=%02x\n", g_resp_len>=2?g_resp[1]:0xff);
    }
    if (do_sec) { fflush(stdout); _exit(0); }

    unsigned char req[PKT]; memset(req,0,sizeof(req));
    req[0]=0x4b;req[1]=0x30;req[2]=0x01;req[3]=0x00;req[4]=0x26;req[5]=0x02;req[6]=(unsigned char)index;req[7]=0x00;
    unsigned char cur[PAYLOAD];
    if(do_exchange(req,8,cur)!=0){ fflush(stdout); _exit(1); }
    printf("RECORD "); for(int i=0;i<PAYLOAD;i++) printf("%02x",cur[i]); printf("\n");

    if(do_write){
        if(!armed){fprintf(stderr,"refusing write without --arm\n"); fflush(stdout); _exit(3);}
        if(!memcmp(cur,want,PAYLOAD)&&!force) fprintf(stderr,"note: same-value write\n");
        unsigned char wr[PKT]; memset(wr,0,sizeof(wr));
        wr[0]=0x4b;wr[1]=0x30;wr[2]=0x02;wr[3]=0x00;wr[4]=0x26;wr[5]=0x02;wr[6]=(unsigned char)index;wr[7]=0x00;
        memcpy(wr+8,want,PAYLOAD);
        unsigned char tmp[PAYLOAD];
        fprintf(stderr,"WRITING index %d\n",index);
        if(do_exchange(wr,8,tmp)!=0){fprintf(stderr,"write failed\n"); fflush(stdout); _exit(1);}
        fprintf(stderr,"WRITE resp_len=%d status=%02x%02x full_tail=", g_resp_len, g_resp_len>=2?g_resp[g_resp_len-2]:0, g_resp_len>=1?g_resp[g_resp_len-1]:0);
        for(int i=(g_resp_len>16?g_resp_len-16:0);i<g_resp_len;i++) fprintf(stderr,"%02x",g_resp[i]);
        fprintf(stderr,"\n");
        unsigned char after[PAYLOAD];
        if(do_exchange(req,8,after)!=0){fprintf(stderr,"readback failed\n"); fflush(stdout); _exit(1);}
        printf("AFTER  "); for(int i=0;i<PAYLOAD;i++) printf("%02x",after[i]); printf("\n");
        printf(!memcmp(after,want,PAYLOAD)?"VERIFY OK\n":"VERIFY FAIL\n");
    }
    fflush(stdout);
    _exit(0);
}
