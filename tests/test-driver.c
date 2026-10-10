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
static int feature_count,feature_size,output_count,feature_fail_at,feature_fail_code;
static int discover_enabled;static struct hidraw_devinfo discovered;
static struct input_event events[8192];
static int event_count;
static u8 input_queue[8][64];
static int input_lengths[8];
static int queue_count,queue_pos,queue_enabled,disconnect_after_queue,repeat_status;

static long test_syscall(long n,long a,long b,long c){
    if(n==SYS_clock_gettime){struct timespec32 *t=(void *)b;t->tv_sec=clock_ms/1000;t->tv_nsec=(clock_ms%1000)*1000000;return 0;}
    if(n==SYS_nanosleep){struct timespec32 *t=(void *)a;clock_ms+=t->tv_sec*1000+t->tv_nsec/1000000;return 0;}
    if(n==SYS_open){if(discover_enabled&&!strcmp((const char *)a,"/dev/hidraw0"))return 7;return -2;}
    if(n==SYS_close)return 0;
    if(n==SYS_read){
        if(queue_enabled&&a==7){
            if(queue_pos<queue_count){int length=input_lengths[queue_pos]?input_lengths[queue_pos]:64;memcpy((void *)b,input_queue[queue_pos++],length);clock_ms+=9;return length;}
            if(disconnect_after_queue)return -19;
            if(repeat_status){int length=input_lengths[queue_count-1]?input_lengths[queue_count-1]:64;memcpy((void *)b,input_queue[queue_count-1],length);clock_ms+=9;return length;}
        }
        return -EAGAIN;
    }
    if(n==SYS_ioctl){
        if(b==HIDIOCGRAWINFO&&discover_enabled){memcpy((void *)c,&discovered,sizeof(discovered));return 0;}
        if(b==HIDIOCSFEATURE(20)||b==HIDIOCSFEATURE(64)||b==HIDIOCSFEATURE(65)){
            feature_size=b==HIDIOCSFEATURE(20)?20:(b==HIDIOCSFEATURE(65)?65:64);
            memcpy(feature,(void *)c,feature_size);
            if(feature_count<32)memcpy(feature_history[feature_count],(void *)c,feature_size);
            feature_count++;if(feature_fail_at==feature_count)return feature_fail_code;return feature_size;
        }
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
    active_product=product;active_bus=BUS_USB;reset_ble_input();ble_output_warned=0;defaults();cfg.haptic_intensity=0;gyro_defaults();
    memset(&original,0,sizeof(original));memset(&gos,0,sizeof(gos));memset(&ms,0,sizeof(ms));
    memset(&voice_rt,0,sizeof(voice_rt));voice_busy=voice_rendering=0;
    prev_shortcuts=0;tuning_prev=post_tune_block=0;event_count=0;
    mouse_left_state=mouse_right_state=spinner_primary_state=spinner_secondary_state=0;
    trackball_click_prev=0;trackball_click_button=TRACKBALL_CLICK_NONE;
    numpad_key_down=numpad_click_prev=numpad_rearm=0;
    feature_count=output_count=feature_fail_at=feature_fail_code=discover_enabled=0;memset(input_lengths,0,sizeof(input_lengths));queue_enabled=queue_count=queue_pos=disconnect_after_queue=repeat_status=0;
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
    assert(decode_report(PRODUCT_SC_HEADCRAB,&original,b,64,&out,&size)==REPORT_IGNORED);
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
/* BLE fixtures encode documented chunks, not a second input decoder. */
static const u8 ble_chunks[9][8]={
    {0xff,0x70,0x5e}, {255,128}, {0xaa,0xbb,0xcc},
    {0x39,0x30,0x60,0xa4}, /* stick 12345,-23456 */
    {0xe0,0xb1,0x08,0x52}, /* left pad -20000,21000 */
    {0x30,0x75,0x20,0xd1}, /* right pad 30000,-12000 */
    {101,0,0x36,0xff,0,0x40}, /* acceleration 101,-202,16384 */
    {0x2f,1,0x6c,0xfe,0xf9,1}, /* gyro 303,-404,505 */
    {1,0,2,0,3,0,4,0}
};
static int ble_fixture(u8 *p,u16 mask){
    static const int bytes[]={3,2,3,4,4,4,6,6,8};int i,n=2;
    memset(p,0,54);p[0]=(u8)((mask&0xf0)|4);p[1]=(u8)(mask>>8);
    for(i=0;i<9;i++)if(mask&(0x10<<i)){memcpy(p+n,ble_chunks[i],bytes[i]);n+=bytes[i];}
    return n;
}
static void ble_segment(u8 *seg,const u8 *p,int n,int index){
    int count=n-index*18;if(count>18)count=18;
    memset(seg,0,20);seg[0]=3;seg[1]=(u8)(0x80|index|((index*18+count==n)?0x40:0));
    if(count>0)memcpy(seg+2,p+index*18,count);
}
static int ble_feed_payload(const u8 *p,int n){
    u8 seg[20];int index,result=REPORT_IGNORED;
    for(index=0;index*18<n;index++){ble_segment(seg,p,n,index);result=feed(seg,20);}
    return result;
}
static void reset_ble(void){reset(PRODUCT_SC_BLE);active_bus=BUS_BLUETOOTH;}
static void test_ble_discovery(void){
    int i;u16 products[]={PRODUCT_SC_BLE,PRODUCT_SC_BLE2};
    for(i=0;i<2;i++){
        reset_ble();discover_enabled=1;discovered=(struct hidraw_devinfo){BUS_BLUETOOTH,STEAM_VENDOR,products[i]};
        assert(find_hidraw()==7&&active_bus==BUS_BLUETOOTH&&active_product==products[i]&&original_controller());
        discovered.bustype=BUS_USB;assert(find_hidraw()==-1);
        discovered.bustype=BUS_BLUETOOTH;discovered.vendor=1;assert(find_hidraw()==-1);
    }
    reset(PRODUCT_SC_WIRED);assert(!original_ble());
    active_product=PRODUCT_SC_HEADCRAB;active_bus=BUS_BLUETOOTH;assert(!original_controller());
    active_product=PRODUCT_BT;assert(!original_controller());
    {u8 b[54]={0x42};u16 newer[]={PRODUCT_WIRED,PRODUCT_BT,PRODUCT_PUCK};const u8 *out;int length;
        for(i=0;i<3;i++){
            active_product=newer[i];assert(!original_ble());
            assert(decode_report(active_product,&original,b,54,&out,&length)==REPORT_STATE&&out==b&&length==54);
        }
        active_product=PRODUCT_SC_BLE;active_bus=BUS_USB;
        assert(decode_report(active_product,&original,b,54,&out,&length)==REPORT_IGNORED);
    }
}
static void test_ble_all_chunk_boundaries(void){
    int subset,n,index;u8 p[54],seg[20];
    /* Every nonempty known-mask combination: all one/two/three-segment lengths
     * and chunks crossing both the18-byte and36-byte payload boundaries. */
    for(subset=1;subset<512;subset++){
        reset_ble();n=ble_fixture(p,(u16)(subset<<4));
        for(index=0;index*18<n;index++){
            ble_segment(seg,p,n,index);
            assert(feed(seg,20)==((index+1)*18>=n?REPORT_STATE:REPORT_IGNORED));
            if((index+1)*18<n)assert(!event_count&&!original.connected);
        }
        assert(original.connected);
        if(subset&1)assert(gos.a&&gos.b&&gos.x&&gos.y&&gos.steam&&gos.start&&gos.sel);
        if(subset&2)assert(gos.l2a==32767&&gos.r2a==16447);
        if(subset&8)assert(gos.lx==12345&&gos.ly==23456);
        if(subset&16)assert(original.pad_x==-20000&&original.pad_y==21000);
        if(subset&32)assert(le16s(original.report+24)==30000&&le16s(original.report+26)==-12000);
        if((subset&192)==192)assert(gyro.prev_ax==101&&gyro.prev_gy==404);
        assert_stick_events_in_bounds();
    }
}
static void test_ble_retention_shortcuts(void){
    u8 p[54];int n;u32 samples,sequence;
    reset_ble();n=ble_fixture(p,0x1ff0);assert(ble_feed_payload(p,n)==REPORT_STATE);
    samples=gyro.total_reports;sequence=le32u(original.report+30);event_count=0;
    n=ble_fixture(p,0x10);memset(p+2,0,3);ble_feed_payload(p,n);
    assert(!gos.a&&!gos.steam&&!gos.l3&&!mouse_right_state&&!spinner_primary_state);
    assert(gos.l2a==32767&&gos.lx==12345&&original.pad_x==-20000);
    assert(gyro.total_reports==samples&&le32u(original.report+30)==sequence);
    n=ble_fixture(p,0x80);put16(p+2,-10000);ble_feed_payload(p,n);
    assert(gos.lx==-10000&&original.pad_x==-20000&&gyro.total_reports==samples);
    n=ble_fixture(p,0x200);put16(p+2,1234);ble_feed_payload(p,n);
    assert(le16s(original.report+24)==1234&&original.stick_x==-10000&&gyro.total_reports==samples);
    n=ble_fixture(p,0x80);put16(p+2,0x8000);put16(p+4,0x8000);event_count=0;ble_feed_payload(p,n);
    assert(original.stick_x==-32768&&original.stick_y==-32768);
    assert(has_event(EV_ABS,ABS_X,-32767)&&has_event(EV_ABS,ABS_Y,32767));assert_stick_events_in_bounds();
    active_product=PRODUCT_SC_BLE2;ble_feed_payload(p,n);assert(gos.lx==-32767);
    /* Both-grip shortcuts and edge suppression use the real shared engine. */
    reset_ble();n=ble_fixture(p,0x10);p[2]=0;p[3]=128;p[4]=1;ble_feed_payload(p,n);
    assert(tuning_prev&&!gos.l4&&!gos.r4);
    p[2]=64;ble_feed_payload(p,n);assert(cfg.haptic_intensity==1);
    ble_feed_payload(p,n);assert(cfg.haptic_intensity==1); /* identical full frames are legal */
    p[2]=0;p[4]=1|64;ble_feed_payload(p,n);p[2]=16;ble_feed_payload(p,n);assert(cfg.gyro_enabled);
    p[3]=p[4]=0;ble_feed_payload(p,n);assert(post_tune_block&&!gos.y);
    p[2]=0;ble_feed_payload(p,n);assert(!post_tune_block);
}
static void assert_ble_rejected(const u8 *p,int n){
    struct original_state before=original;struct ble_state before_ble=ble;
    int before_events=event_count;u8 copy[64];assert(n<=64);if(n>0)memcpy(copy,p,n);
    assert(feed(copy,n)==REPORT_IGNORED);
    assert(!memcmp(&before,&original,sizeof(original))&&!memcmp(&before_ble,&ble,sizeof(ble)));
    assert(event_count==before_events);
}
static void test_ble_rejection_and_restart(void){
    u8 p[54],seg[3][20],bad[64];int n,i,j;
    reset_ble();n=ble_fixture(p,0x1ff0);for(i=0;i<3;i++)ble_segment(seg[i],p,n,i);
    for(i=0;i<3;i++)for(j=0;j<20;j++){
        reset_ble();if(i>0)feed(seg[0],20);if(i>1)feed(seg[1],20);assert_ble_rejected(seg[i],j);
    }
    reset_ble();memcpy(bad,seg[0],20);bad[20]=0;assert_ble_rejected(bad,21);
    assert_ble_rejected(seg[1],20);assert_ble_rejected(seg[2],20);
    feed(seg[0],20);assert_ble_rejected(seg[2],20);assert_ble_rejected(seg[1],20);
    feed(seg[0],20);feed(seg[1],20);assert_ble_rejected(seg[1],20);assert_ble_rejected(seg[2],20);
    feed(seg[0],20);clock_ms+=BLE_FRAGMENT_TIMEOUT_MS+1;assert_ble_rejected(seg[1],20);
    assert_ble_rejected(seg[2],20);
    memcpy(bad,seg[0],20);bad[1]|=0x40;assert_ble_rejected(bad,20); /* premature last */
    feed(seg[0],20);feed(seg[1],20);memcpy(bad,seg[2],20);bad[1]&=~0x40;assert_ble_rejected(bad,20);
    memcpy(bad,seg[0],20);bad[1]|=8;assert_ble_rejected(bad,20); /* reserved header */
    memcpy(bad,seg[0],20);bad[2]=4;bad[3]=0x20;assert_ble_rejected(bad,20); /* unknown mask */
    bad[1]=0xc0;bad[3]=0;assert_ble_rejected(bad,20); /* empty mask */
    bad[2]=5;assert_ble_rejected(bad,20); /* status */
    bad[2]=6;assert_ble_rejected(bad,20); /* unknown type */
    /* Empty polling messages leave a valid in-progress frame intact. */
    feed(seg[0],20);memset(bad,0,20);bad[0]=3;assert_ble_rejected(bad,20);
    feed(seg[1],20);assert(feed(seg[2],20)==REPORT_STATE);assert_ble_rejected(seg[2],20);
    /* A new first segment replaces a partial frame without committing its A. */
    reset_ble();feed(seg[0],20);p[2]=0;for(i=0;i<3;i++)ble_segment(seg[i],p,n,i);
    feed(seg[0],20);feed(seg[1],20);assert(!original.connected&&!event_count);
    assert(feed(seg[2],20)==REPORT_STATE&&!gos.a);
}
static void test_ble_gyro_freshness(void){
    u8 p[54];int i,n;u32 samples;int motion;
    reset_ble();n=ble_fixture(p,0xc00);
    for(i=0;i<80;i++)ble_feed_payload(p,n);
    assert(gyro.bias_ready&&gyro.bias_x==303&&gyro.bias_y==404&&gyro.bias_z==505);
    samples=gyro.total_reports;motion=gyro.emitted_x;
    n=ble_fixture(p,0x10);p[2]=p[3]=0;p[4]=16;ble_feed_payload(p,n);
    n=ble_fixture(p,0x200);for(i=0;i<10;i++)ble_feed_payload(p,n);
    assert(gyro.total_reports==samples&&gyro.emitted_x==motion);
    cfg.gyro_enabled=1;gyro_apply_cfg();n=ble_fixture(p,0x800);put16(p+6,2505);
    for(i=0;i<40;i++)ble_feed_payload(p,n);
    assert(gyro.emitted_x!=motion);samples=gyro.total_reports;motion=gyro.emitted_x;
    n=ble_fixture(p,0x400);ble_feed_payload(p,n);assert(gyro.total_reports==samples&&gyro.emitted_x==motion);
    n=ble_fixture(p,0x200);for(i=0;i<30;i++)ble_feed_payload(p,n);
    assert(!ble.gyro_ready&&!gyro.active_stick_mask&&gyro.total_reports==samples&&gyro.emitted_x==motion);
    n=ble_fixture(p,0x800);ble_feed_payload(p,n);assert(gyro.total_reports==samples+1&&gyro.last_dt_us==9000);
    /* Stale stick assistance releases through actual EV_ABS output even while
     * button/pad updates keep the controller stream live. */
    cfg.gyro_stick_mode=1;gyro_apply_cfg();n=ble_fixture(p,0x10);p[2]=p[3]=0;p[4]=64;ble_feed_payload(p,n);
    n=ble_fixture(p,0x800);put16(p+6,2505);
    for(i=0;i<40;i++)ble_feed_payload(p,n);
    assert(gyro.active_stick_mask==1&&(gos.lx||gos.ly));event_count=0;
    n=ble_fixture(p,0x200);for(i=0;i<30;i++)ble_feed_payload(p,n);
    assert(!gyro.active_stick_mask&&!gos.lx&&!gos.ly&&has_event(EV_ABS,ABS_X,0));
    /* A gyro chunk without any acceleration must not calibrate from fake zeros. */
    n=ble_fixture(p,0x800);
    reset_ble();for(i=0;i<80;i++)ble_feed_payload(p,n);assert(!gyro.bias_ready&&!gyro.total_reports);
}
static void test_captured_original_ble_sensors(void){
    /* Real original1106 state segments curated from original-ble-frames.json.
     * Replayed in a controlled order; this does not reproduce capture timing.
     * No identity or status payloads are included. */
    u8 captured[][20]={
        {0x03,0xc0,0x04,0x0c,0x5c,0x02,0x32,0xf9,0x92,0x3f,0xfe,0xff,0xff,0xff,0xfb,0xff,0,0,0,0},
        {0x03,0xc0,0x04,0x04,0x93,0x02,0x95,0xf8,0xc6,0x3f,0,0,0,0,0,0,0,0,0,0},
        {0x03,0xc0,0x04,0x00,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
        {0x03,0xc0,0x04,0x0c,0x80,0x02,0x18,0xf9,0x65,0x3f,0xfb,0xff,0xff,0xff,0xfa,0xff,0,0,0,0}
    };
    struct original_state before;struct ble_state before_ble;u32 gyro_time;int count;
    reset_ble();active_product=PRODUCT_SC_BLE2;
    assert(feed(captured[0],20)==REPORT_STATE);
    assert(gyro.prev_ax==604&&gyro.prev_ay==-1742&&gyro.prev_az==16274);
    /* Raw gyro(-2,-1,-5) becomes(-2,+1,-5) in Orbit's sensor convention. */
    assert(gyro.prev_gx==-2&&gyro.prev_gy==1&&gyro.prev_gz==-5);
    assert(ble.gyro_fresh&&ble.imu_sequence==1&&gyro.fresh_reports==1);
    gyro_time=ble.last_gyro_ms;
    assert(feed(captured[1],20)==REPORT_STATE);
    assert(le16s(original.report+34)==659&&le16s(original.report+36)==-1899&&le16s(original.report+38)==16326);
    assert(le16s(original.report+40)==-2&&le16s(original.report+42)==1&&le16s(original.report+44)==-5);
    assert(!ble.gyro_fresh&&ble.imu_sequence==1&&gyro.fresh_reports==1&&ble.last_gyro_ms==gyro_time);
    before=original;before_ble=ble;count=event_count;
    assert(feed(captured[2],20)==REPORT_IGNORED);
    assert(!memcmp(&before,&original,sizeof(original))&&!memcmp(&before_ble,&ble,sizeof(ble)));
    assert(event_count==count&&gyro.fresh_reports==1);
    assert(feed(captured[3],20)==REPORT_STATE);
    assert(gyro.prev_ax==640&&gyro.prev_ay==-1768&&gyro.prev_az==16229);
    assert(gyro.prev_gx==-5&&gyro.prev_gy==1&&gyro.prev_gz==-6);
    assert(ble.gyro_fresh&&ble.imu_sequence==2&&gyro.fresh_reports==2&&ble.last_gyro_ms>gyro_time);
    /* Actual empty-mask traffic cannot renew the production loop's liveness. */
    reset_ble();active_product=PRODUCT_SC_BLE2;
    memcpy(input_queue[0],captured[0],20);memcpy(input_queue[1],captured[2],20);
    input_lengths[0]=input_lengths[1]=20;queue_count=2;queue_enabled=repeat_status=1;
    assert(run_controller(7,10,11,12,13)==-1&&clock_ms>=102000);
    assert(!original.connected&&!ble.gyro_ready&&gyro.fresh_reports==1);
}
static void test_ble_features(void){
    u8 command[64];int n,i,j,segments;
    for(i=0;i<64;i++)command[i]=(u8)(0xa0+i);
    for(n=1;n<=64;n++){
        reset_ble();assert(send_feature_cmd(7,command,n)==n);segments=(n+17)/18;
        assert(feature_count==segments&&feature_size==20&&!output_count);
        for(i=0;i<segments;i++){
            assert(feature_history[i][0]==3&&feature_history[i][1]==(0x80|i|(i==segments-1?0x40:0)));
            for(j=0;j<18;j++)assert(feature_history[i][j+2]==(i*18+j<n?command[i*18+j]:0));
        }
    }
    reset_ble();assert(send_feature_cmd(7,command,0)<0&&send_feature_cmd(7,command,65)<0&&!feature_count);
    for(i=1;i<=4;i++){
        reset_ble();feature_fail_at=i;feature_fail_code=-EPIPE;
        assert(send_feature_cmd(7,command,64)==-EPIPE&&feature_count==i&&ble_output_warned);
        feature_fail_at=0;assert(send_feature_cmd(7,command,1)==1&&feature[1]==0xc0);
    }
    reset_ble();feature_fail_at=2;feature_fail_code=19;
    assert(send_feature_cmd(7,command,64)==-5&&feature_count==2);
    reset_ble();assert(configure_controller(7)==0&&feature_count==4);
    assert(feature_history[0][2]==0x81);
    assert(!memcmp(feature_history[1]+2,(u8[]){0x87,6,7,7,0,8,7,0},8));
    assert(!memcmp(feature_history[2]+2,(u8[]){0x87,3,0x31,2,0},5));
    assert(!memcmp(feature_history[3]+2,(u8[]){0x87,3,0x30,0x18,0},5));
    reset_ble();feature_fail_at=3;feature_fail_code=-EPIPE;assert(configure_controller(7)<0&&feature_count==3);
    reset_ble();haptic_tone_raw(7,0,-6,500,10,0,0);
    assert(feature_count==1&&feature_size==20&&!memcmp(feature+2,(u8[]){0x8f,8,1,0xe8,3,0xe8,3,5,0,250},10));
    haptic_tone_raw(7,3,0,500,10,0,0);assert(feature_count==1&&!output_count);
    feature_fail_at=2;feature_fail_code=-EPIPE;haptic_tone_raw(7,1,0,500,10,0,0);assert(feature_count==2&&ble_output_warned);
}
static void test_ble_disconnect_loop(void){
    u8 p[54],seg[20];int n;
    reset_ble();n=ble_fixture(p,0x10);p[2]=128;p[3]=p[4]=0;
    ble_segment(seg,p,n,0);memcpy(input_queue[0],seg,20);input_lengths[0]=20;
    memset(input_queue[1],0,20);input_queue[1][0]=3;input_queue[1][1]=0xc0;input_queue[1][2]=5;input_lengths[1]=20;
    queue_enabled=repeat_status=1;queue_count=2;
    assert(run_controller(7,10,11,12,13)==-1&&!gos.a&&!ble.gyro_ready&&!ble_assembly.expected);
    assert(has_event(EV_KEY,BTN_SOUTH,1)&&has_event(EV_KEY,BTN_SOUTH,0));
    /* Reconnect after full cleanup accepts a new state on the same transport. */
    queue_enabled=0;assert(ble_feed_payload(p,n)==REPORT_STATE&&gos.a);
    release_controller_outputs(7,10,11,12,13);assert(!gos.a&&!original.connected);
    reset_ble();memcpy(input_queue[0],seg,20);input_lengths[0]=20;
    queue_count=queue_enabled=disconnect_after_queue=1;
    assert(run_controller(7,10,11,12,13)==-1&&!gos.a&&!original.connected);
}
int main(void){
    test_buttons_axes();test_stick_event_bounds();test_multiplex_and_touch();test_imu();test_shortcuts();
    test_haptics_features();test_newer();test_rejection_disconnect();
    test_ble_discovery();test_ble_all_chunk_boundaries();test_ble_retention_shortcuts();
    test_ble_rejection_and_restart();test_ble_gyro_freshness();test_captured_original_ble_sensors();test_ble_features();test_ble_disconnect_loop();
    puts("PASS: production USB/receiver/BLE/newer input, outputs, shortcuts, IMU, haptics and disconnect loop");return 0;
}
