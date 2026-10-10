/* Exercise the real decoder, processing engine and loop; stub only OS calls. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define SC_ORBIT_TEST
#define main driver_main
#include "../SC-Orbit-driver.c"
#undef main

static u32 clock_ms;
static u8 feature[65],feature_history[32][65],output[128];
static int feature_count,feature_size,output_count;
static struct input_event events[8192];
static int event_count;
static u8 input_queue[8][64];
static int queue_count,queue_pos,queue_enabled,disconnect_after_queue,repeat_status;

static long test_syscall(long n,long a,long b,long c){
    if(n==SYS_clock_gettime){struct timespec32 *t=(void *)b;t->tv_sec=clock_ms/1000;t->tv_nsec=(clock_ms%1000)*1000000;return 0;}
    if(n==SYS_nanosleep){struct timespec32 *t=(void *)a;clock_ms+=t->tv_sec*1000+t->tv_nsec/1000000;return 0;}
    if(n==SYS_open)return -2;
    if(n==SYS_close)return 0;
    if(n==SYS_read){
        if(queue_enabled&&a==7){
            if(queue_pos<queue_count){memcpy((void *)b,input_queue[queue_pos++],64);clock_ms+=9;return 64;}
            if(disconnect_after_queue)return -19;
            if(repeat_status){memcpy((void *)b,input_queue[1],64);clock_ms+=9;return 64;}
        }
        return -EAGAIN;
    }
    if(n==SYS_ioctl){
        if(b==HIDIOCSFEATURE(64)||b==HIDIOCSFEATURE(65)){feature_size=(b==HIDIOCSFEATURE(65))?65:64;memcpy(feature,(void *)c,feature_size);if(feature_count<32)memcpy(feature_history[feature_count],(void *)c,feature_size);feature_count++;return feature_size;}
        return -1;
    }
    if(n==SYS_write){
        if(a==7){assert(c<=128);memcpy(output,(void *)b,c);output_count++;}
        if(a>=10&&a<=13){int count=(int)c/(int)sizeof(events[0]);assert(event_count+count<8192);memcpy(events+event_count,(void *)b,c);event_count+=count;}
        return c;
    }
    return -1;
}
static void reset(u16 product){
    active_product=product;defaults();cfg.haptic_intensity=0;gyro_defaults();
    memset(&original,0,sizeof(original));memset(&gos,0,sizeof(gos));memset(&ms,0,sizeof(ms));
    memset(&voice_rt,0,sizeof(voice_rt));voice_busy=voice_rendering=0;
    prev_shortcuts=0;tuning_prev=post_tune_block=0;event_count=0;
    mouse_left_state=mouse_right_state=spinner_primary_state=spinner_secondary_state=0;
    trackball_click_prev=0;trackball_click_button=TRACKBALL_CLICK_NONE;
    numpad_key_down=numpad_click_prev=numpad_rearm=0;
    feature_count=output_count=0;queue_enabled=queue_count=queue_pos=disconnect_after_queue=repeat_status=0;
    clock_ms=100000;
}
static void packet(u8 *b,u32 seq){memset(b,0,64);b[0]=1;b[2]=1;b[3]=60;put32(b+4,seq);}
static int feed(u8 *b,int n){clock_ms+=9;return process_input_report(7,10,11,12,13,b,n);}
static int has_event(u16 type,u16 code,int value){int i;for(i=0;i<event_count;i++)if(events[i].type==type&&events[i].code==code&&events[i].value==value)return 1;return 0;}
static void test_buttons_axes(void){
    u8 b[64];reset(PRODUCT_SC_WIRED);packet(b,1);
    b[8]=0xff;b[9]=0x70; /* face, shoulders/triggers, ordinary menu keys */
    b[11]=255;b[12]=128;put16(b+16,12345);put16(b+18,-23456);
    assert(feed(b,64)==REPORT_STATE);
    assert(gos.a&&gos.b&&gos.x&&gos.y&&gos.start&&gos.sel&&gos.steam);
    assert(gos.l1&&gos.r1&&gos.l2c&&gos.r2c);
    assert(gos.l2a==32767&&gos.r2a==16447&&gos.lx==12345&&gos.ly==23456);
    assert(!gos.rx&&!gos.ry&&!tuning_prev);
    assert(has_event(EV_KEY,BTN_MODE,1)&&has_event(EV_ABS,ABS_Z,32767));
    packet(b,2);b[9]=128;b[10]=64;put16(b+18,0x8000);feed(b,64);
    assert(gos.l4&&gos.l3&&!gos.r4&&gos.ly==32767);
    packet(b,3);b[10]=1;feed(b,64);assert(gos.r4&&!gos.l4);
    packet(b,4);b[9]=1|2|4|8;feed(b,64);assert(gos.hatx==1&&gos.haty==1);
}
static void assert_stick_events_in_bounds(void){
    int i;
    for(i=0;i<event_count;i++){
        if(events[i].type==EV_ABS&&(events[i].code==ABS_X||events[i].code==ABS_Y||
           events[i].code==ABS_RX||events[i].code==ABS_RY))
            assert(events[i].value>=-32767&&events[i].value<=32767);
    }
}
static void test_stick_event_bounds(void){
    u8 b[64];int endpoint;s16 raw;
    reset(PRODUCT_SC_WIRED);
    for(endpoint=0;endpoint<2;endpoint++){
        raw=endpoint?32767:-32768;packet(b,endpoint+1);
        put16(b+16,(u16)raw);put16(b+18,(u16)raw);event_count=0;feed(b,64);
        /* Signed raw values survive decoding; only emitted axes are clamped. */
        assert(le16s(original.report+10)==raw&&le16s(original.report+12)==raw);
        assert(has_event(EV_ABS,ABS_X,endpoint?32767:-32767));
        assert(has_event(EV_ABS,ABS_Y,endpoint?-32767:32767));
        assert_stick_events_in_bounds();
    }
    reset(PRODUCT_WIRED);
    for(endpoint=0;endpoint<2;endpoint++){
        raw=endpoint?32767:-32768;memset(b,0,sizeof(b));b[0]=0x42;
        put16(b+10,(u16)raw);put16(b+12,(u16)raw);
        put16(b+14,(u16)raw);put16(b+16,(u16)raw);event_count=0;feed(b,54);
        assert(has_event(EV_ABS,ABS_X,endpoint?32767:-32767));
        assert(has_event(EV_ABS,ABS_Y,endpoint?-32767:32767));
        assert(has_event(EV_ABS,ABS_RX,endpoint?32767:-32767));
        assert(has_event(EV_ABS,ABS_RY,endpoint?-32767:32767));
        assert_stick_events_in_bounds();
    }
}
static void test_multiplex_and_touch(void){
    u8 b[64];reset(PRODUCT_SC_WIRELESS);packet(b,1);b[10]=128;put16(b+16,12000);put16(b+18,-13000);feed(b,64);
    assert(gos.lx==12000&&original.report[5]&2);
    packet(b,2);b[10]=128|8|2|16|4;put16(b+16,-20000);put16(b+18,21000);put16(b+20,1234);put16(b+22,-5678);feed(b,64);
    assert(gos.lx==12000&&gos.ly==13000);
    assert(le16s(original.report+18)==-20000&&le16s(original.report+20)==21000);
    assert((original.report[5]&6)==6&&(original.report[4]&96)==96);
    assert(spinner_primary_state&&has_event(EV_KEY,BTN_TRIGGER,1));
    packet(b,3);b[10]=128;put16(b+16,16000);feed(b,64);
    assert(gos.lx==16000&&le16s(original.report+18)==-20000);
    packet(b,4);b[10]=8;put16(b+16,7000);feed(b,64);assert(!gos.lx&&!gos.ly);
    packet(b,5);feed(b,64);assert(!le16s(original.report+18)&&!(original.report[5]&2));
    assert(!spinner_primary_state&&!mouse_left_state&&!mouse_right_state);
}
static void test_imu(void){
    u8 b[64];int i;s32 dt;reset(PRODUCT_SC_WIRED);packet(b,1);
    put16(b+28,101);put16(b+30,-202);put16(b+32,16384);
    put16(b+34,303);put16(b+36,-404);put16(b+38,505);feed(b,64);
    assert(gyro.prev_ax==101&&gyro.prev_ay==-202&&gyro.prev_az==16384);
    assert(gyro.prev_gx==303&&gyro.prev_gy==404&&gyro.prev_gz==505);
    assert(gyro.last_dt_us==9000);assert(!gyro_fresh_dt(original.report,54,&dt));
    clock_ms+=7;put32(b+4,2);feed(b,64);assert(gyro.last_dt_us==16000);
    for(i=3;i<180;i++){put32(b+4,i);feed(b,64);}
    assert(gyro.bias_ready&&gyro.bias_x==303&&gyro.bias_y==404&&gyro.bias_z==505);
    cfg.gyro_enabled=1;gyro_apply_cfg();b[10]=16;
    put16(b+38,2505);
    for(i=180;i<220;i++){put32(b+4,i);feed(b,64);}
    assert(has_event(EV_REL,REL_X,1)||gyro.emitted_x!=0);
    put16(b+36,0x8000);put32(b+4,181);feed(b,64);assert(gyro.prev_gy==32767);
}
static void test_shortcuts(void){
    u8 b[64];int speed;reset(PRODUCT_SC_WIRED);speed=cfg.spinner_ipod_speed;
    packet(b,1);b[9]=128;b[10]=1;feed(b,64);assert(tuning_prev&&!gos.l4&&!gos.r4&&!cfg.gyro_enabled);
    b[8]=4;feed(b,64);assert(cfg.spinner_ipod_speed==speed+1);
    feed(b,64);assert(cfg.spinner_ipod_speed==speed+1); /* edge, not repeats */
    b[8]=0;b[10]|=64;feed(b,64);assert(!cfg.gyro_stick_mode);
    b[8]=16;feed(b,64);assert(cfg.gyro_enabled&&cfg.spinner_direction==1);
    b[8]=0;feed(b,64);b[8]=128;feed(b,64);assert(cfg.gyro_stick_mode==1&&!cfg.pads_swapped);
    b[8]=0;feed(b,64);b[8]=64;feed(b,64);assert(cfg.gyro_inversion==0);
    b[8]=128;b[9]=0;b[10]=0;feed(b,64);assert(post_tune_block&&!gos.a);
    packet(b,2);feed(b,64);assert(!post_tune_block);
    b[8]=128;feed(b,64);assert(gos.a&&!tuning_prev);
    /* A stick direction must also be released after leaving settings. */
    packet(b,3);b[9]=128;b[10]=1;put16(b+16,24000);feed(b,64);
    b[9]=b[10]=0;feed(b,64);assert(post_tune_block&&!gos.lx);
    put16(b+16,0);feed(b,64);assert(!post_tune_block);
}
static void test_haptics_features(void){
    u8 expected[10]={0x8f,8,1,0xe8,3,0xe8,3,5,0,0xfa};
    reset(PRODUCT_SC_WIRED);haptic_tone_raw(7,0,-6,500,10,0,0);
    assert(feature_count==1&&feature_size==65&&feature[0]==0&&!memcmp(feature+1,expected,10)&&!output_count);
    haptic_tone_raw(7,1,31,500,10,0,0);assert(feature[3]==0&&feature[10]==6);
    haptic_tone_raw(7,3,0,500,10,0,0);assert(feature_count==2);
    voice_stop_channel(7,1);assert(feature_count==3&&feature[8]==0&&feature[9]==0);
    configure_controller(7);
    assert(feature_history[3][0]==0&&feature_history[3][1]==0x81&&feature_history[3][2]==0);
    assert(!memcmp(feature_history[4]+1,(u8[]){0x87,6,7,7,0,8,7,0},8));
    assert(feature_count==6&&feature[1]==0x87&&feature[3]==0x30&&feature[4]==0x18);
    reset(PRODUCT_WIRED);haptic_tone_raw(7,3,-6,500,10,42,2);
    assert(!feature_count&&output_count==1&&!memcmp(output,(u8[]){0x83,3,250,244,1,10,0,42,0,2},10));
    configure_controller(7);assert(feature[0]==1&&feature_size==64&&feature_count==3);
}
static void test_newer(void){
    static const u8 real[]={0x42,0xad,0,0,0,0x20,0,0,0,0,0xe2,0xfd,0x37,0xfa,0x75,1,7,6,0,0,0,0,0,0,0,0,0,0,0x28,0x78,0x81,0xbd,0xae,0,0x40,0xf2,0x9e,0x3e,0,0,0,0,0,0,0,0,0xe3,0x22,0x90,0xfb,0x15,0x0d,0xa1,0x85};
    const u8 *out;int n,i;struct gamepad_out_state before;u8 b[54];
    reset(PRODUCT_WIRED);assert(sizeof(real)==54);
    assert(decode_report(active_product,&original,real,54,&out,&n)==REPORT_STATE&&out==real&&n==54);
    process_report(7,10,11,12,13,real,54);before=gos;
    reset(PRODUCT_WIRED);process_input_report(7,10,11,12,13,real,54);assert(!memcmp(&before,&gos,sizeof(gos)));
    assert(gos.ly==0&&gos.ry==0); /* captured centred axes below existing deadzone */
    for(i=0;i<3;i++){
        reset(PRODUCT_WIRED);memcpy(b,real,54);b[0]=(u8[]){0x42,0x45,0x47}[i];b[2]=1;b[4]=1;
        assert(feed(b,54)==REPORT_STATE&&gos.a&&gos.steam);
        b[2]=16|8;b[4]=0;feed(b,54);assert(tuning_prev&&cfg.spinner_direction==-1); /* newer ...+Y unchanged */
    }
}
static void test_rejection_disconnect(void){
    u8 b[128];int n,old_events;const u8 *out;int size;
    reset(PRODUCT_SC_WIRELESS);packet(b,1);b[8]=128;b[10]=16|4;put16(b+20,30000);feed(b,64);assert(gos.a&&mouse_right_state);
    old_events=event_count;
    for(n=0;n<64;n++)assert(feed(b,n)==REPORT_IGNORED);
    assert(feed(b,65)==REPORT_IGNORED);assert(event_count==old_events&&gos.a);
    b[2]=4;b[3]=11;assert(feed(b,64)==REPORT_IGNORED&&gos.a);
    b[2]=3;b[3]=1;b[4]=99;assert(feed(b,64)==REPORT_IGNORED&&gos.a);
    b[2]=1;b[3]=59;assert(feed(b,64)==REPORT_IGNORED);
    b[3]=60;b[1]=1;assert(feed(b,64)==REPORT_IGNORED);
    b[1]=0;b[2]=3;b[3]=1;b[4]=1;assert(feed(b,64)==REPORT_DISCONNECTED);
    assert(!gos.a&&!mouse_right_state&&!spinner_primary_state&&!original.stick_x&&!tuning_prev);
    assert(has_event(EV_KEY,BTN_SOUTH,0)&&has_event(EV_KEY,BTN_RIGHT,0));
    b[4]=2;assert(feed(b,64)==REPORT_CONNECTED);assert(feed(b,64)==REPORT_IGNORED);
    packet(b,2);b[8]=128;assert(feed(b,64)==REPORT_STATE&&gos.a);
    /* Exact boundaries and unrelated payloads never escape the decoder. */
    {struct original_state state={0};int kind;
        for(kind=0;kind<256;kind++){packet(b,2);b[2]=(u8)kind;b[3]=60;
            assert(decode_report(PRODUCT_SC_WIRED,&state,b,64,&out,&size)==(kind==1?REPORT_STATE:REPORT_IGNORED));}
        packet(b,3);put16(b+16,0x8000);put16(b+18,0x7fff);b[11]=1;b[12]=0;
        assert(decode_report(PRODUCT_SC_WIRED,&state,b,64,&out,&size)==REPORT_STATE);
        assert(le16s(out+10)==-32768&&le16s(out+12)==32767&&le16u(out+6)==128&&le16u(out+8)==0);
    }
    assert(decode_report(PRODUCT_SC_ALT,&original,b,64,&out,&size)==REPORT_IGNORED);
    assert(decode_report(PRODUCT_SC_VIRTUAL,&original,b,64,&out,&size)==REPORT_IGNORED);
    assert(decode_report(PRODUCT_SC_BLE,&original,b,64,&out,&size)==REPORT_IGNORED);
    /* Loop watchdog: receiver persists but no new state; outputs release. */
    reset(PRODUCT_SC_WIRELESS);queue_enabled=repeat_status=1;queue_count=2;
    packet(input_queue[0],1);input_queue[0][8]=128;
    packet(input_queue[1],2);input_queue[1][2]=4;input_queue[1][3]=11;
    assert(run_controller(7,10,11,12,13)==-1&&clock_ms>=102000&&!gos.a);
    /* Immediate USB loss follows the same production release path. */
    reset(PRODUCT_SC_WIRED);queue_enabled=disconnect_after_queue=1;queue_count=1;
    packet(input_queue[0],1);input_queue[0][8]=128;
    assert(run_controller(7,10,11,12,13)==-1&&!gos.a);
}
int main(void){
    test_buttons_axes();test_stick_event_bounds();test_multiplex_and_touch();test_imu();test_shortcuts();
    test_haptics_features();test_newer();test_rejection_disconnect();
    puts("PASS: production legacy/newer decoder, outputs, shortcuts, IMU, haptics and disconnect loop");return 0;
}
