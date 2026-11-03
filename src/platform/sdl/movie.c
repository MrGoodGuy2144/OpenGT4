#include "movie.h"
#include "platform/platform.h"
#include "pss_audio.h"
#include <SDL.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <errno.h>
extern char **environ;
#endif
#define FRAME_BYTES (640*448*4)
#define QUEUE 6
struct sdl_movie {
#ifdef _WIN32
    HANDLE process,pipe;
    wchar_t path[MAX_PATH];
#else
    pid_t process;
    int pipe;
    char path[1024];
#endif
    SDL_Thread *thread;
    SDL_mutex *lock;
    SDL_cond *ready;
    int stop,ended,head,count,rate;
    uint8_t *frames[QUEUE],*display;
    plat_texture *texture;
    uint64_t start,number;
    int16_t *pcm;
    size_t audio_frames,audio_pos;
    int audio_enabled,audio_attempted,audio_playing;
};
/* The movie's sound is the device's music source, like the Arcade track:
   one output device and mixer for the whole frontend. This runs on the
   platform's music producer thread, never the device callback. */
static void movie_audio_render(int16_t *out,int frames,void *user)
{
    sdl_movie *m=user;size_t count=(size_t)frames;
    size_t take=m->audio_frames-m->audio_pos;if(take>count)take=count;
    memset(out,0,count*4);
    memcpy(out,m->pcm+m->audio_pos*2,take*4);m->audio_pos+=take;
}
void sdl_movie_audio(sdl_movie *m,int enabled)
{
    if(!m)return;
    m->audio_enabled=enabled;
    if(!enabled && m->audio_playing){plat_audio_release_music(m);m->audio_playing=0;}
}
static void start_audio(sdl_movie *m)
{
    if(!m->audio_enabled || !m->pcm || m->audio_attempted)return;
    m->audio_attempted=1;
    m->audio_playing=plat_audio_set_music(movie_audio_render,m)==0;
    fprintf(stderr,m->audio_playing?"movie: sound playing on the music output\n":"movie: audio unavailable\n");
}
static uint64_t playback_ns(sdl_movie *m)
{
    /* The device's consumed frames keep the pictures on the sound. */
    if(m->audio_playing)return plat_audio_music_position()*1000000000ull/48000;
    return m->start?plat_time_ns()-m->start:0;
}
uint64_t sdl_movie_frame_count(const sdl_movie *m){return m?m->number:0;}
/* When frame n is due: 30000/1001 or 60000/1001 frames per second. */
static uint64_t frame_ns(const sdl_movie *m,uint64_t n){return n*1001000000ull/(unsigned)m->rate;}
static int decode(void *ptr)
{
    sdl_movie *m=ptr;
    for(;;) {
        SDL_LockMutex(m->lock);
        while(m->count==QUEUE && !m->stop)SDL_CondWait(m->ready,m->lock);
        int stop=m->stop,slot=(m->head+m->count)%QUEUE;
        SDL_UnlockMutex(m->lock);
        if(stop)break;
        size_t offset=0;
        while(offset<FRAME_BYTES) {
#ifdef _WIN32
            DWORD got=0;
            if(!ReadFile(m->pipe,m->frames[slot]+offset,(DWORD)(FRAME_BYTES-offset),&got,NULL) || !got)break;
#else
            ssize_t got=read(m->pipe,m->frames[slot]+offset,FRAME_BYTES-offset);
            if(got<0 && errno==EINTR)continue;
            if(got<=0)break;
#endif
            offset+=(size_t)got;
        }
        if(offset!=FRAME_BYTES)break;
        SDL_LockMutex(m->lock);m->count++;SDL_UnlockMutex(m->lock);
    }
    SDL_LockMutex(m->lock);m->ended=1;SDL_UnlockMutex(m->lock);
    return 0;
}
void sdl_movie_release_texture(sdl_movie *m){if(m){plat_texture_destroy(m->texture);m->texture=NULL;}}
int sdl_movie_ready(sdl_movie *m){if(!m)return 0;SDL_LockMutex(m->lock);int ready=m->count>0;SDL_UnlockMutex(m->lock);return ready;}
int sdl_movie_finished(sdl_movie *m){if(!m)return 0;SDL_LockMutex(m->lock);int done=m->ended && !m->count;SDL_UnlockMutex(m->lock);return done && (!m->number || playback_ns(m)>=frame_ns(m,m->number));}
void sdl_movie_close(sdl_movie *m)
{
    if(!m)return;
    sdl_movie_audio(m,0);
    if(m->lock){SDL_LockMutex(m->lock);m->stop=1;if(m->ready)SDL_CondSignal(m->ready);SDL_UnlockMutex(m->lock);}
#ifdef _WIN32
    if(m->process)TerminateProcess(m->process,0); /* releases a blocked pipe read */
    if(m->thread)SDL_WaitThread(m->thread,NULL);
    if(m->pipe)CloseHandle(m->pipe);
    if(m->process){WaitForSingleObject(m->process,2000);CloseHandle(m->process);}
    if(m->path[0])DeleteFileW(m->path);
#else
    /* Killing the child releases a blocked reader even when the queue is full. */
    if(m->process>0)kill(m->process,SIGKILL);
    if(m->thread)SDL_WaitThread(m->thread,NULL);
    if(m->pipe>=0)close(m->pipe);
    if(m->process>0)while(waitpid(m->process,NULL,0)<0 && errno==EINTR){}
    if(m->path[0])unlink(m->path);
#endif
    if(m->ready)SDL_DestroyCond(m->ready);
    if(m->lock)SDL_DestroyMutex(m->lock);
    for(int i=0;i<QUEUE;i++)free(m->frames[i]);
    free(m->pcm);free(m->display);plat_texture_destroy(m->texture);free(m);
}
sdl_movie *sdl_movie_open(const uint8_t *data,size_t size,int interlace,int repeat,int audio_track)
{
    if(!data || !size || size>512u*1024*1024)return NULL;
    sdl_movie *m=calloc(1,sizeof *m);if(!m)return NULL;
#ifndef _WIN32
    m->pipe=-1;
#endif
    m->rate=interlace?60:30;
    if(audio_track>0 && !repeat) {
        if(gt4_pss_audio(data,size,(unsigned)audio_track,&m->pcm,&m->audio_frames))
            fprintf(stderr,"movie: audio track %d, stereo 48000 Hz, %zu frames\n",audio_track,m->audio_frames);
        else fprintf(stderr,"movie: unsupported or missing audio track %d\n",audio_track);
    }
#ifdef _WIN32
    wchar_t temp[MAX_PATH],exe[MAX_PATH],cmd[2048];
    if(!GetTempPathW(MAX_PATH,temp) || !GetTempFileNameW(temp,L"og4",0,m->path))goto fail;
    FILE *f=_wfopen(m->path,L"wb");if(!f)goto fail;
    size_t written=fwrite(data,1,size,f);fclose(f);if(written!=size)goto fail;
    /* FFmpeg: beside the executable first (a release ships it there), then
       the one found when building, then whatever PATH holds */
    {
        DWORD n=GetModuleFileNameW(NULL,exe,MAX_PATH);
        wchar_t *cut=n && n<MAX_PATH?wcsrchr(exe,L'\\'):NULL;
        if(cut){wcscpy(cut+1,L"ffmpeg.exe");}
        if(!cut || GetFileAttributesW(exe)==INVALID_FILE_ATTRIBUTES){
#ifdef OPENGT4_FFMPEG
            MultiByteToWideChar(CP_UTF8,0,OPENGT4_FFMPEG,-1,exe,MAX_PATH);
            if(GetFileAttributesW(exe)==INVALID_FILE_ATTRIBUTES)
#endif
            {
                wchar_t *part=NULL;
                if(!SearchPathW(NULL,L"ffmpeg.exe",NULL,MAX_PATH,exe,&part))goto fail;
            }
        }
    }
    /* No shell: only a generated temporary path is passed to FFmpeg.
       Interlaced movies store each frame as two stacked 224-line fields,
       top first, 1/59.94 s apart. `il` interleaves them back into lines and
       yadif makes one picture per field (59.94 fps), as the PS2 shows them
       field by field; static detail stays still instead of flickering. */
    /* The PS2's IPU turns YCbCr into RGB with no range expansion: Y is
       shown as it is (MPEG-2's 16..235 stays 16..235, the grey blacks and
       the soft whites of the PCSX2 print: the Arcade's Mode Selection
       background at 5%/99% of 32/238, ours 8/253 with FFmpeg's default
       expansion). in_range=full keeps the samples unexpanded. */
    const wchar_t *filter=interlace?
        L"il=l=i:c=i,setfield=tff,yadif=mode=send_field:parity=tff,scale=640:448:in_range=full:out_range=full,format=rgba,fps=60000/1001":
        L"scale=640:448:in_range=full:out_range=full,format=rgba,fps=30000/1001";
    swprintf(cmd,2048,L"\"%ls\" -v error -nostdin -threads 1%ls -i \"%ls\" -an -sn -vf %ls -pix_fmt rgba -f rawvideo pipe:1",
             exe,repeat?L" -stream_loop -1":L"",m->path,filter);
    SECURITY_ATTRIBUTES sa={sizeof sa,NULL,TRUE};HANDLE write_pipe=NULL;
    if(!CreatePipe(&m->pipe,&write_pipe,&sa,0))goto fail;
    SetHandleInformation(m->pipe,HANDLE_FLAG_INHERIT,0);
    HANDLE nul=CreateFileW(L"NUL",GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,&sa,OPEN_EXISTING,0,NULL);
    STARTUPINFOW si={sizeof si};PROCESS_INFORMATION pi={0};
    si.dwFlags=STARTF_USESTDHANDLES;si.hStdOutput=write_pipe;si.hStdError=nul;si.hStdInput=nul;
    BOOL ok=CreateProcessW(exe,cmd,NULL,NULL,TRUE,CREATE_NO_WINDOW,NULL,NULL,&si,&pi);
    CloseHandle(write_pipe);CloseHandle(nul);
    if(!ok)goto fail;
    m->process=pi.hProcess;CloseHandle(pi.hThread);
#else
    const char *temp=getenv("TMPDIR");if(!temp || !*temp)temp="/tmp";
    if(snprintf(m->path,sizeof m->path,"%s/opengt4-movie-XXXXXX",temp)>=(int)sizeof m->path){m->path[0]=0;goto fail;}
    int fd=mkstemp(m->path);
    if(fd<0){m->path[0]=0;goto fail;}
    size_t offset=0;
    while(offset<size){
        ssize_t n=write(fd,data+offset,size-offset);
        if(n<0 && errno==EINTR)continue;
        if(n<=0){close(fd);goto fail;}
        offset+=(size_t)n;
    }
    if(close(fd))goto fail;
    const char *exe=getenv("OPENGT4_FFMPEG_BINARY");
    if(!exe || !*exe)exe="ffmpeg";
    const char *filter=interlace?
        "il=l=i:c=i,setfield=tff,yadif=mode=send_field:parity=tff,scale=640:448:in_range=full:out_range=full,format=rgba,fps=60000/1001":
        "scale=640:448:in_range=full:out_range=full,format=rgba,fps=30000/1001";
    char *args[32];int argc=0;
#define ARG(value) args[argc++]=(char *)(value)
    ARG(exe);ARG("-v");ARG("error");ARG("-nostdin");ARG("-threads");ARG("1");
    if(repeat){ARG("-stream_loop");ARG("-1");}
    ARG("-i");ARG(m->path);ARG("-an");ARG("-sn");ARG("-vf");ARG(filter);
    ARG("-pix_fmt");ARG("rgba");ARG("-f");ARG("rawvideo");ARG("pipe:1");args[argc]=NULL;
#undef ARG
    int pipes[2];if(pipe(pipes))goto fail;
    /* SDL audio/loader threads may be alive. Spawn without invoking a shell
       or running allocation code in a forked copy of the application. */
    fcntl(pipes[0],F_SETFD,FD_CLOEXEC);fcntl(pipes[1],F_SETFD,FD_CLOEXEC);
    posix_spawn_file_actions_t actions;
    if(posix_spawn_file_actions_init(&actions)){close(pipes[0]);close(pipes[1]);goto fail;}
    int err=posix_spawn_file_actions_adddup2(&actions,pipes[1],STDOUT_FILENO);
    if(!err)err=posix_spawn_file_actions_addopen(&actions,STDIN_FILENO,"/dev/null",O_RDONLY,0);
    if(!err)err=posix_spawn_file_actions_addclose(&actions,pipes[0]);
    if(!err)err=posix_spawn_file_actions_addclose(&actions,pipes[1]);
    if(!err)err=posix_spawnp(&m->process,exe,&actions,NULL,args,environ);
    posix_spawn_file_actions_destroy(&actions);close(pipes[1]);
    if(err){close(pipes[0]);m->process=0;fprintf(stderr,"movie: %s: %s\n",exe,strerror(err));goto fail;}
    m->pipe=pipes[0];
#endif
    m->lock=SDL_CreateMutex();m->ready=SDL_CreateCond();
    if(!m->lock || !m->ready)goto fail;
    for(int i=0;i<QUEUE;i++){m->frames[i]=malloc(FRAME_BYTES);if(!m->frames[i])goto fail;}
    m->display=malloc(FRAME_BYTES);if(!m->display)goto fail;
    m->thread=SDL_CreateThread(decode,"menu movie",m);if(!m->thread)goto fail;
    return m;
fail:
    fprintf(stderr,"movie: could not start decoder\n");sdl_movie_close(m);return NULL;
}
int sdl_movie_draw(sdl_movie *m,float x,float y,float w,float h,float opacity)
{
    if(!m)return 0;
    uint64_t now=plat_time_ns();int update=0;
    if(!m->start && sdl_movie_ready(m))start_audio(m);
    SDL_LockMutex(m->lock);
    while(m->count && (!m->start || playback_ns(m)>=frame_ns(m,m->number))) {
        if(!m->start)m->start=now;
        memcpy(m->display,m->frames[m->head],FRAME_BYTES);
        m->head=(m->head+1)%QUEUE;m->count--;m->number++;update=1;
        /* After a slow decode, hold timing instead of rapidly replaying a backlog. */
        if(!m->audio_playing && now-m->start>frame_ns(m,m->number)+100000000ull)
            m->start=now-frame_ns(m,m->number-1);
        SDL_CondSignal(m->ready);
    }
    SDL_UnlockMutex(m->lock);
    if(update){if(m->texture)plat_texture_update(m->texture,m->display);else m->texture=plat_texture_create(640,448,m->display);}
    if(!m->texture)return 0;
    uint8_t tint[4]={255,255,255,(uint8_t)(255*opacity)};
    plat_draw_quad(m->texture,x,y,w,h,0,0,1,1,tint);return 1;
}
