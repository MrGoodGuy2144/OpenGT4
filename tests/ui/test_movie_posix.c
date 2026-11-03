/* Decoder/process integration with synthetic video; no game data or GPU needed. */
#include "movie.h"
#include "platform/platform.h"
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <errno.h>
struct plat_texture {int unused;};
static struct plat_texture texture;
static int frames;
uint64_t plat_time_ns(void){return SDL_GetTicks64()*1000000ull;}
plat_texture *plat_texture_create(int w,int h,const uint8_t *rgba){if(w!=640 || h!=448 || !rgba)return NULL;frames++;return &texture;}
void plat_texture_update(plat_texture *t,const uint8_t *rgba){(void)t;(void)rgba;frames++;}
void plat_texture_destroy(plat_texture *t){(void)t;}
void plat_draw_quad(const plat_texture *t,float x,float y,float w,float h,float u0,float v0,float u1,float v1,const uint8_t c[4])
{(void)t;(void)x;(void)y;(void)w;(void)h;(void)u0;(void)v0;(void)u1;(void)v1;(void)c;}
int plat_audio_set_music(plat_audio_cb cb,void *user){(void)cb;(void)user;return -1;}
void plat_audio_release_music(void *user){(void)user;}
uint64_t plat_audio_music_position(void){return 0;}
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);return 1;}}while(0)
int main(int argc,char **argv)
{
    CHECK(argc==3);CHECK(SDL_Init(SDL_INIT_TIMER)==0);
    FILE *f=fopen(argv[1],"rb");CHECK(f);CHECK(!fseek(f,0,SEEK_END));long size=ftell(f);CHECK(size>0);rewind(f);
    uint8_t *data=malloc((size_t)size);CHECK(data);CHECK(fread(data,1,(size_t)size,f)==(size_t)size);fclose(f);
    CHECK(!setenv("OPENGT4_FFMPEG_BINARY","/no/such/opengt4-ffmpeg",1));
    CHECK(sdl_movie_open(data,(size_t)size,0,0,0)==NULL);
    CHECK(!setenv("OPENGT4_FFMPEG_BINARY",argv[2],1));
    for(int interlace=0;interlace<2;interlace++){
        sdl_movie *m=sdl_movie_open(data,(size_t)size,interlace,0,0);CHECK(m);
        uint64_t start=SDL_GetTicks64();frames=0;
        while(!sdl_movie_finished(m) && SDL_GetTicks64()-start<10000){sdl_movie_draw(m,0,0,640,448,1);SDL_Delay(5);}
        CHECK(sdl_movie_finished(m));CHECK(frames>0);CHECK(sdl_movie_frame_count(m)>0);sdl_movie_close(m);
    }
    sdl_movie *loop=sdl_movie_open(data,(size_t)size,0,1,0);CHECK(loop);
    /* Leave a full frame queue and a blocked pipe reader, then dispose. */
    uint64_t start=SDL_GetTicks64();while(!sdl_movie_ready(loop) && SDL_GetTicks64()-start<5000)SDL_Delay(5);
    CHECK(sdl_movie_ready(loop));SDL_Delay(100);sdl_movie_close(loop);
    free(data);CHECK(waitpid(-1,NULL,WNOHANG)==-1 && errno==ECHILD);SDL_Quit();return 0;
}
