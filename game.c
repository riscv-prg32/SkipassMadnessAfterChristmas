/* Skipass Madness After Christmas -- original PRG32 downhill arcade game.
 * Integer-only, portable and deliberately compact for the 128 KiB profile. */
#include "prg32.h"
#include <stdint.h>
#include <stddef.h>

#define W 320
#define H 200
#define ROAD_Y 44
#define ROOM "skipass-madness-roccaraso-v1"
#define RUN_MS 75000u
#define OBJ_N 28
#define OBJ_SPACING 65
#define OBJ_LOOP (OBJ_N * OBJ_SPACING)
#define SKIER_W 16
#define SKIER_H 24
#define SKIER_FRAME_BYTES (SKIER_W * SKIER_H / 2)
#define CI(r,g,b) ((uint8_t)(16 + 36*(r) + 6*(g) + (b)))
enum { TITLE, ATTRACT, PLAY, CRASH, WIN, OVER };
enum { P_SKY=8, P_SKY2, P_FAR, P_MID, P_SNOW, P_SHADE, P_TREE, P_DARK,
       P_RED=232, P_BLUE, P_YELLOW, P_ORANGE, P_ICE, P_SHADOW, P_PANEL, P_WHITE };

typedef struct { int16_t x, y; uint8_t kind, lane; } obstacle_t;
typedef struct {
  int mode, course, x, speed, distance, score, gates, lives, combo;
  uint32_t start_ms, last_ms, mode_ms, input_prev, seed, last_sfx_ms;
  obstacle_t obj[OBJ_N];
  uint8_t joined, music, multiplayer_ready;
} game_t;
static game_t g;

void *memset(void *d, int v, size_t n) { uint8_t *p=d; while(n--) *p++=(uint8_t)v; return d; }
static int clampi(int v,int a,int b){return v<a?a:(v>b?b:v);}
static int absi(int v){return v<0?-v:v;}
static uint32_t rnd(void){g.seed=g.seed*1664525u+1013904223u;return g.seed>>16;}
static uint16_t rgb(int r,int gg,int b){return (uint16_t)(((r&248)<<8)|((gg&252)<<3)|(b>>3));}
static uint16_t cube565(uint8_t c){static const uint8_t r5[6]={0,7,13,19,25,31},g6[6]={0,13,26,38,51,63};int v=c-16;return v<0?0:(uint16_t)((r5[v/36]<<11)|(g6[(v/6)%6]<<5)|r5[v%6]);}
static void box(int x,int y,int w,int h,uint8_t c){if(x<0){w+=x;x=0;}if(y<0){h+=y;y=0;}if(x+w>W)w=W-x;if(y+h>H)h=H-y;if(w>0&&h>0)prg32_gfx_rect_indexed(x,y,w,h,c);}
static int slen(const char*s){int n=0;while(s[n])n++;return n;}
static void txt(int x,int y,const char*s,uint8_t fg,uint8_t bg){prg32_gfx_text8(x,y,s,cube565(fg),cube565(bg));}
static void center(int y,const char*s,uint8_t fg,uint8_t bg){txt((W-slen(s)*8)/2,y,s,fg,bg);}
static void num(char *b,int v,int digits){b[digits]=0;while(digits--){b[digits]=(char)('0'+v%10);v/=10;}}

static void palette(void){
  static const uint8_t p[][4]={{P_SKY,74,150,220},{P_SKY2,164,213,244},{P_FAR,87,119,148},{P_MID,47,78,92},{P_SNOW,238,246,248},{P_SHADE,183,215,226},{P_TREE,25,76,61},{P_DARK,8,23,35},{P_RED,218,44,50},{P_BLUE,26,102,196},{P_YELLOW,255,220,54},{P_ORANGE,242,121,32},{P_ICE,133,224,244},{P_SHADOW,116,158,174},{P_PANEL,10,34,57},{P_WHITE,255,255,255}};
  for(unsigned i=0;i<sizeof(p)/sizeof(p[0]);i++)prg32_palette_set(p[i][0],rgb(p[i][1],p[i][2],p[i][3]));
}

/* Large high-contrast rear-view skier, two packed 4bpp carving frames. */
static uint8_t skier_px[SKIER_FRAME_BYTES*2];
static uint16_t skier_pal[8];
static prg32_indexed_sprite_t skier;
static const char course_name[6][15]={"AREMOGNA","PALLOTTIERI","GRAVARE","PIZZALTO","MONTE PRATELLO","CRETE ROSSE"};

static void music_stop(void){if(g.music){prg32_audio_stop_track();for(int i=0;i<8;i++)prg32_audio_note_off((uint8_t)i);g.music=0;}}
static void pixel4(int frame,int x,int y,int c){int n=frame*SKIER_FRAME_BYTES+y*(SKIER_W/2)+x/2;if(x&1)skier_px[n]=(uint8_t)((skier_px[n]&0xf0)|c);else skier_px[n]=(uint8_t)((skier_px[n]&0x0f)|(c<<4));}
static void sprite_box(int frame,int x,int y,int w,int h,int c){for(int yy=y;yy<y+h;yy++)for(int xx=x;xx<x+w;xx++)pixel4(frame,xx,yy,c);}
static void build_skier(void){
  memset(skier_px,0,sizeof skier_px);
  for(int f=0;f<2;f++){
    sprite_box(f,5,1,6,2,1); sprite_box(f,4,3,8,4,2); /* helmet/head */
    sprite_box(f,3,7,10,8,3); sprite_box(f,6,8,4,6,4); /* jacket */
    sprite_box(f,1,9,3,7,3); sprite_box(f,12,9,3,7,3); /* arms */
    sprite_box(f,4,15,3,6,5); sprite_box(f,9,15,3,6,5); /* trousers */
    if(f){sprite_box(f,2,19,5,3,6);sprite_box(f,10,18,4,4,6);}else{sprite_box(f,4,18,4,4,6);sprite_box(f,9,19,5,3,6);}
    for(int x=0;x<7;x++)pixel4(f,x,23,7);
    for(int x=9;x<16;x++)pixel4(f,x,23,7);
    for(int y=13;y<22;y++){pixel4(f,0,y,1);pixel4(f,15,y,1);} /* poles */
  }
}
static void music_play(int track){music_stop();prg32_audio_play_track((uint8_t)track);g.music=1;}
static void set_mode(int m){g.mode=m;g.mode_ms=prg32_ticks_ms();if(m==TITLE||m==ATTRACT)music_play(0);else if(m==WIN)music_play(1);else if(m==OVER)music_play(2);else music_stop();}
static void place_object(obstacle_t *object,int y,int sequence){
  object->y=(int16_t)y;
  object->lane=(uint8_t)(rnd()%7);
  object->x=(int16_t)(34+object->lane*42);
  object->kind=(uint8_t)(sequence%6==0?2:(sequence%4==0?3:(sequence%3==0?1:0)));
}
static void objects(void){for(int i=0;i<OBJ_N;i++)place_object(&g.obj[i],80+i*OBJ_SPACING+(int)(rnd()%34),i);}
static void begin_run(void){g.x=160;g.speed=3;g.distance=0;g.score=0;g.gates=0;g.lives=3;g.combo=1;g.start_ms=prg32_ticks_ms();g.last_ms=g.start_ms;g.last_sfx_ms=g.start_ms;g.seed=0x5a17u+(uint32_t)g.course*997u;objects();set_mode(PLAY);}
static void impact(int note){prg32_audio_set_channel_pan(6,(int8_t)((g.x-160)*90/160));prg32_audio_note(6,4,(uint8_t)note,180,130);}
static void ski_sound(uint32_t now,int steer){if(now-g.last_sfx_ms>=360){g.last_sfx_ms=now;prg32_audio_set_channel_pan(5,(int8_t)(steer*72));prg32_audio_note(5,3,(uint8_t)(38+g.speed),110,105);}}

static void multiplayer(uint32_t input){
  if(!prg32_multiplayer_available())return;
  if(!g.multiplayer_ready){prg32_multiplayer_init();g.multiplayer_ready=1;}
  if(!g.joined){if(prg32_multiplayer_join(ROOM,PRG32_MP_FLAG_ENABLE)==0)g.joined=1;}
  if(g.joined){prg32_multiplayer_set_input(input);prg32_multiplayer_set_local_state((int16_t)g.x,(int16_t)clampi(g.distance/16,-32768,32767),(uint16_t)g.course,(uint16_t)g.mode);prg32_multiplayer_tick();}
}

static void update_play(uint32_t now,uint32_t in,uint32_t press){
  int steer=0;if(in&PRG32_BTN_LEFT)steer--;if(in&PRG32_BTN_RIGHT)steer++;
  g.x=clampi(g.x+steer*(2+g.speed/3),20,300);
  if(in&PRG32_BTN_UP)g.speed=clampi(g.speed+1,2,9);else if(in&PRG32_BTN_DOWN)g.speed=clampi(g.speed-1,2,9);else if((now&127)==0)g.speed=clampi(g.speed+1,2,7);
  if(press&PRG32_BTN_A){g.x=clampi(g.x+(steer?steer:1)*18,18,302);impact(78);}
  ski_sound(now,steer);
  if(now-g.last_ms>=33){g.last_ms=now;g.distance+=g.speed;g.score+=g.speed*g.combo;}
  for(int i=0;i<OBJ_N;i++){
    int sy=ROAD_Y+g.distance-g.obj[i].y;
    /* The descent brings distant scenery from the horizon toward the skier.
     * Recycle each object after it passes below the playfield. */
    if(sy>205){place_object(&g.obj[i],g.obj[i].y+OBJ_LOOP,i+(g.distance/OBJ_LOOP)*OBJ_N);continue;}
    if(sy>=154&&sy<=186&&absi(g.x-g.obj[i].x)<14){
      if(g.obj[i].kind==2){g.gates++;g.combo=clampi(g.combo+1,1,5);g.score+=250*g.combo;impact(84);g.obj[i].y+=OBJ_LOOP;}
      else if(g.obj[i].kind==3){g.speed=clampi(g.speed-2,2,9);g.score+=75;impact(55);g.obj[i].y+=OBJ_LOOP;}
      else {g.lives--;g.combo=1;impact(g.obj[i].kind?38:48);g.obj[i].y+=OBJ_LOOP;if(g.lives<=0)set_mode(OVER);else set_mode(CRASH);return;}
    }
  }
  if(now-g.start_ms>=RUN_MS||g.distance>=4200)set_mode(WIN);
  multiplayer(in);
}

void skipass_madness_init(void){
  palette();build_skier();skier_pal[0]=0;skier_pal[1]=rgb(18,24,34);skier_pal[2]=rgb(255,214,112);skier_pal[3]=rgb(224,30,48);skier_pal[4]=rgb(250,250,244);skier_pal[5]=rgb(255,210,32);skier_pal[6]=rgb(24,72,186);skier_pal[7]=rgb(18,210,244);
  skier.pixels=skier_px;skier.palette=skier_pal;skier.width=SKIER_W;skier.height=SKIER_H;skier.frame_count=2;skier.palette_count=8;skier.bits_per_pixel=PRG32_SPRITE_BPP_4;skier.transparent_index=0;
  g.seed=0xc0ffeeu;g.course=0;set_mode(TITLE);
}
void skipass_madness_update(void){
  uint32_t now=prg32_ticks_ms(),in=prg32_input_read(),press=in&~g.input_prev;g.input_prev=in;
  if(g.mode==TITLE||g.mode==ATTRACT){if(press&PRG32_BTN_LEFT)g.course=(g.course+5)%6;if(press&PRG32_BTN_RIGHT)g.course=(g.course+1)%6;if(press&(PRG32_BTN_A|PRG32_BTN_B|PRG32_BTN_START))begin_run();else if(g.mode==TITLE&&now-g.mode_ms>12000)set_mode(ATTRACT);}
  else if(g.mode==PLAY)update_play(now,in,press);
  else if(g.mode==CRASH){if(now-g.mode_ms>1100)set_mode(PLAY);}
  else if(g.mode==WIN||g.mode==OVER){if(press&(PRG32_BTN_A|PRG32_BTN_START))begin_run();if(press&PRG32_BTN_B)set_mode(TITLE);}
  if((press&PRG32_BTN_START)&&g.mode==PLAY)set_mode(TITLE);
}

static void mountains(void){
  box(0,0,W,H,P_SKY);box(0,18,W,18,P_SKY2);box(0,36,W,24,P_ICE);box(262,24,22,22,P_YELLOW);
  for(int x=-28;x<W;x+=64){int h=22+((x+91)%18);for(int y=0;y<h;y+=4){box(x+y/2,67-y,64-y,4,y>h-8?P_WHITE:P_FAR);}}
  for(int x=-34;x<W;x+=86){int h=34+((x+77)%15);for(int y=0;y<h;y+=4){box(x+y/2,80-y,86-y,4,y>h-10?P_SNOW:P_MID);}}
  box(0,80,W,120,P_SNOW);box(0,81,W,2,P_SHADE);
  box(8,45,2,35,P_DARK);box(304,38,2,42,P_DARK);for(int x=10;x<304;x+=10)box(x,46-(x-10)/38,10,1,P_DARK);
  box(54,50,13,6,P_ORANGE);box(220,42,13,6,P_BLUE);
}
static void tree(int x,int y,int s){
  box(x-s/2,y+2,s,3,P_SHADOW);box(x-2,y-s/3,4,s/3+4,P_DARK);
  box(x-s/4,y-s,s/2,s/4,P_SNOW);
  box(x-s/3,y-s*3/4,s*2/3,s/4,P_TREE);
  box(x-s*2/5,y-s/2,s*4/5,s/4,P_TREE);
  box(x-s/2,y-s/4,s,s/4,P_TREE);
}
static void gate(int x,int y){int half=10+clampi((y-70)/8,0,8);int h=15+clampi((y-70)/6,0,12);box(x-half-2,y-h,4,h+3,P_BLUE);box(x+half-2,y-h,4,h+3,P_RED);box(x-half,y-h,half*2,4,P_YELLOW);box(x-half+3,y-h+4,half*2-6,5,P_RED);}
static void rock(int x,int y){box(x-6,y-5,12,7,P_MID);box(x-3,y-8,7,3,P_FAR);}
static void bump(int x,int y){box(x-11,y-2,22,4,P_SHADOW);box(x-9,y-5,18,4,P_ICE);box(x-5,y-7,10,3,P_SNOW);}
static void draw_peers(void){if(!g.joined)return;int n=prg32_multiplayer_get_peer_count();for(int i=0;i<n;i++){prg32_player_state_t p;if(prg32_multiplayer_get_peer(i,&p)==0&&(p.flags==PLAY||p.flags==CRASH)&&p.sprite==g.course){int y=170+(g.distance-(int)p.y*16)/18;if(y>80&&y<194){box(p.x-4,y-5,9,9,P_ORANGE);box(p.x-2,y-8,5,4,P_DARK);}}}}
static void hud(void){char b[7];box(0,0,W,18,P_PANEL);txt(4,5,course_name[g.course],CI(5,5,5),CI(0,1,2));num(b,g.score,6);txt(176,5,b,CI(5,5,0),CI(0,1,2));num(b,g.lives,1);txt(288,5,b,CI(5,2,2),CI(0,1,2));}
static void slope(void){mountains();for(int i=0;i<OBJ_N;i++){int y=ROAD_Y+g.distance-g.obj[i].y;if(y<68||y>205)continue;int size=12+clampi((y-70)/5,0,16);if(g.obj[i].kind==2)gate(g.obj[i].x,y);else if(g.obj[i].kind==3)bump(g.obj[i].x,y);else if(g.obj[i].kind==1)rock(g.obj[i].x,y);else tree(g.obj[i].x,y,size);}for(int y=96;y<200;y+=25){int off=(g.distance/2+y*3)%103;box(off,y,24,2,P_SHADE);box(264-off/2,y+9,16,2,P_ICE);}draw_peers();box(g.x-12,188,24,3,P_SHADOW);prg32_sprite_draw_indexed(g.x-SKIER_W/2,162,&skier,(uint32_t)((g.distance/20)&1));hud();}
static void title(void){mountains();box(18,21,284,72,P_PANEL);center(30,"SKIPASS MADNESS",CI(5,5,0),CI(0,1,2));center(43,"AFTER CHRISTMAS",CI(5,2,0),CI(0,1,2));center(63,"ROCCARASO - ABRUZZO",CI(5,5,5),CI(0,1,2));tree(32,151,32);tree(286,155,38);center(112,course_name[g.course],CI(0,2,5),CI(5,5,5));center(130,"LEFT/RIGHT SELECT PISTE",CI(1,1,1),CI(5,5,5));center(145,"A OR START TO SKI",CI(5,0,0),CI(5,5,5));center(174,g.mode==ATTRACT?"ATTRACT MODE - JOIN THE RACE":"NETWORK RACERS ENABLED",CI(0,3,2),CI(5,5,5));}
static void result(const char *head,const char *sub){slope();box(36,55,248,82,P_PANEL);center(67,head,CI(5,5,0),CI(0,1,2));center(86,sub,CI(5,5,5),CI(0,1,2));char b[7];num(b,g.score,6);center(104,b,CI(5,2,0),CI(0,1,2));center(122,"A RETRY   B TITLE",CI(3,5,5),CI(0,1,2));}
void skipass_madness_draw(void){if(g.mode==TITLE||g.mode==ATTRACT)title();else if(g.mode==PLAY||g.mode==CRASH)slope();else if(g.mode==WIN)result("PISTE CONQUERED!",course_name[g.course]);else result("SKIPASS EXPIRED","THE MOUNTAIN WINS");if(g.mode==CRASH){box(70,82,180,34,P_PANEL);center(94,"SNOWBANK!",CI(5,2,0),CI(0,1,2));}}
