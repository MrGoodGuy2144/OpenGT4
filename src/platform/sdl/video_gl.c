/*
 * OpenGL 3.3 video for the SDL backend: window, 2D drawing for the menus
 * (batched textured quads in the virtual screen set by plat_video_set_view,
 * letterboxed into the window at its own resolution) and the frame target
 * the 3D renderer draws into (render3d.c).
 *
 * Every frame is drawn into an offscreen framebuffer the size of the
 * window's drawable and copied to the window at the end, so captures read
 * the same pixels with a visible or a hidden window.
 */
#include "platform/platform.h"
#include "plat_sdl.h"
#include "video_gl.h"
#include "gl.h"
#include "fsr.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void plat_audio_shutdown(void);

static SDL_Window   *window;
static SDL_GLContext context;
static int           vsync, upscale;
static int menu_pacing=1, frame_presented;
static int menu_projection=1;
static void layout_view(void);
static uint64_t menu_deadline;
void video_set_menu_pacing(int enabled)
{
    enabled=enabled!=0;
    if(menu_pacing!=enabled){menu_pacing=enabled;menu_deadline=0;}
}
/* Diagnostic only: synchronize the GPU to measure complete frame work. */
static int profile_frames, profile_draws, profile_quads;
static uint64_t profile_start, profile_total, profile_window;
static unsigned profile_count;

/* P: optional native-resolution filming, independent of the preview window.
   Each record is one real rendered frame; no resampling or duplicated frames.
   The pipe receives bottom-up RGBA, with presentation times in a sidecar. */
static int capture_w,capture_h,capture_attempted,capture_pixel_w,capture_pixel_h;
static const char *capture_pipe,*capture_gate,*capture_times_path;
static FILE *capture_stream,*capture_times;
static unsigned char *capture_pixels;
static uint64_t capture_frame;
int video_export_fixed_step(void)
{return capture_stream && getenv("OPENGT4_CAPTURE_FIXED_STEP")!=NULL;}


/* frame target */
static GLuint fbo, color_rb, depth_rb;
static int    fb_w, fb_h;
static GLuint scene_fbo,scene_color,scene_depth,scene_tex,scene_resolve;
static int scene_w,scene_h,scene_samples,scene_active,output_w,output_h;

/* virtual screen and its letterbox rectangle in framebuffer pixels */
static int ui_panel_on,ui_panel_x,ui_panel_y,ui_panel_w,ui_panel_h;
static int ui_clip_on,ui_clip_x,ui_clip_y,ui_clip_w,ui_clip_h;
static float view_w = 640, view_h = 480, scale_x = 1, scale_y = 1, off_x, off_y;

/* 2D batch */
typedef struct { float x, y, u, v; uint8_t c[4]; } vertex2d;
#define BATCH 16384
static vertex2d batch[BATCH];
static int      nbatch;
static GLuint   batch_tex, white_tex, program2d, vao2d, vbo2d;
static GLuint license_program,license_lut;
static GLint license_uview,license_utex,license_uclut,license_umix[3];
static uint8_t license_cached_lut[256][3];
static int license_lut_valid;
static GLint    u_view2d, u_tex2d, u_kind2d, u_filter2d, u_text2d;
static int batch_kind, ui_filter=0;
static float text_sharpness=0;
static float batch_fixed_alpha=-1;

static void capture_framebuffer(void)
{
    if(!capture_pipe)return;
    if(!capture_attempted){
        FILE *gate=capture_gate?fopen(capture_gate,"rb"):NULL;
        if(capture_gate && !gate)return;
        if(gate)fclose(gate);
        capture_attempted=1;
        capture_stream=fopen(capture_pipe,"wb");
        capture_times=capture_times_path?fopen(capture_times_path,"wb"):NULL;
        capture_pixels=malloc((size_t)fb_w*fb_h*4);
        capture_pixel_w=fb_w;capture_pixel_h=fb_h;
        if(!capture_stream || !capture_times || !capture_pixels){
            fprintf(stderr,"native capture: cannot open output\n");
            if(capture_stream)fclose(capture_stream);
            capture_stream=NULL;return;
        }
        setvbuf(capture_stream,NULL,_IONBF,0);
        fprintf(capture_times,"frame,time_ns,width,height,upscale,ui_filter,text_sharpness\n");
    }
    if(!capture_stream)return;
    if(fb_w!=capture_pixel_w || fb_h!=capture_pixel_h){
        fclose(capture_stream);capture_stream=NULL;
        fprintf(stderr,"native capture: resized output rejected\n");return;
    }
    uint64_t now=plat_time_ns();
    glBindFramebuffer(GL_FRAMEBUFFER,fbo);
    glPixelStorei(GL_PACK_ALIGNMENT,1);
    glReadPixels(0,0,fb_w,fb_h,GL_RGBA,GL_UNSIGNED_BYTE,capture_pixels);
    size_t bytes=(size_t)fb_w*fb_h*4;
    if(fwrite(capture_pixels,1,bytes,capture_stream)!=bytes){
        fclose(capture_stream);capture_stream=NULL;
        fprintf(stderr,"native capture: output closed\n");return;
    }
    fprintf(capture_times,"%llu,%llu,%d,%d,%d,%d,%.2f\n",
        (unsigned long long)capture_frame++,(unsigned long long)now,
        fb_w,fb_h,upscale,ui_filter,text_sharpness);
    fflush(capture_times);
}

struct plat_texture { GLuint id; int w, h; int screen_capture; };

SDL_Window *plat_sdl_window(void) { return window; }

static const char *vs2d =
    "#version 330 core\n"
    "layout(location=0) in vec2 pos; layout(location=1) in vec2 uv; layout(location=2) in vec4 col;\n"
    "uniform vec2 view; out vec2 v_uv; out vec4 v_col;\n"
    "void main(){ gl_Position=vec4(pos.x/view.x*2.0-1.0, 1.0-pos.y/view.y*2.0, 0.0, 1.0); v_uv=uv; v_col=col; }\n";
static const char *fs2d =
    "#version 330 core\n"
    "in vec2 v_uv; in vec4 v_col; uniform sampler2D tex; out vec4 o;\n"
    "uniform int kind,filter_mode; uniform float text_sharpness;\n"
    "vec4 weights(float f){return vec4(-.5*f+f*f-.5*f*f*f,1.-2.5*f*f+1.5*f*f*f,.5*f+2.*f*f-1.5*f*f*f,-.5*f*f+.5*f*f*f);}\n"
    "vec4 sharpSample(vec2 uv){\n"
    " vec2 sz=vec2(textureSize(tex,0)),p=uv*sz-.5,b=floor(p);vec4 wx=weights(fract(p.x)),wy=weights(fract(p.y)),c=vec4(0);\n"
    " for(int y=0;y<4;y++)for(int x=0;x<4;x++){vec4 q=texelFetch(tex,clamp(ivec2(b)+ivec2(x-1,y-1),ivec2(0),ivec2(sz)-1),0);q.rgb*=q.a;c+=q*wx[x]*wy[y];}\n"
    " c.a=clamp(c.a,0.,1.);c.rgb=c.a>.0001?clamp(c.rgb/c.a,0.,1.):vec3(0);return c; }\n"
    "void main(){vec4 c=texture(tex,v_uv);\n"
    /* Glyph atlases are distance fields (core glyph_sdf.c): r holds the
       disc's coverage, a the signed distance to the letter's edge (0.5 on
       it). Text Clarity 0 draws the coverage as the disc does; above it the
       edge is cut at 0.5 with a transition measured on screen (fwidth):
       about 2 px at 0.5, under 1 px at 1, sharp at any window size. */
    " if(kind==1){float a=c.r;\n"
    "  if(text_sharpness>0.){float w=max(fwidth(c.a)*mix(2.,.7,text_sharpness),1e-4);a=mix(a,smoothstep(.5-w,.5+w,c.a),min(1.,text_sharpness*4.));}\n"
    "  c=vec4(1,1,1,a);}\n"
    " else if(kind==0 && filter_mode==1){vec2 ratio=vec2(textureSize(tex,0))*fwidth(v_uv);if(max(ratio.x,ratio.y)<1.)c=sharpSample(v_uv);}\n"
    " else if(kind==0 && filter_mode==2){vec2 sz=vec2(textureSize(tex,0));c=texture(tex,(floor(v_uv*sz)+.5)/sz);}\n"
    " o=c*v_col;}\n";

static int display_aspect;
static float content_width,content_x,side_margin;
static void layout_view(void)
{
    float aspect=display_aspect?16.f/9.f:4.f/3.f;
    if(ui_panel_on){
        /* Each half has its own 640x240 UI space. Applying the full-screen
           4:3 fit here would crop half the HUD horizontally. */
        side_margin=view_w*(aspect/(4.f/3.f)-1)*.5f;
        scale_x=ui_panel_w/(view_w+2*side_margin);scale_y=ui_panel_h/view_h;
        return;
    }
    float height=fminf((float)fb_h,fb_w/aspect),width=height*aspect;
    scale_x=scale_y=height/view_h;
    content_width=width;content_x=(fb_w-width)*.5f;
    side_margin=(width/scale_x-view_w)*.5f;
    off_x=content_x+side_margin*scale_x;off_y=(fb_h-height)*.5f;
    /* Menu render context 00215FE0 +0x30..0xAC maps 640x480 to the
       whole framebuffer. Its 2D widgets have no wide-mode compensation;
       the PS2 display stretches this image. Race HUD has its own layout. */
    if(menu_projection){scale_x=width/view_w;side_margin=0;off_x=content_x;}

}

static int ensure_framebuffer(void)
{
    int w = 0, h = 0;
    SDL_GL_GetDrawableSize(window, &w, &h);
    if(capture_w>0 && capture_h>0){w=capture_w;h=capture_h;}
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (fbo && w == fb_w && h == fb_h) return 1;
    if (!fbo) { glGenFramebuffers(1, &fbo); glGenRenderbuffers(1, &color_rb); glGenRenderbuffers(1, &depth_rb); }
    glBindRenderbuffer(GL_RENDERBUFFER, color_rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, w, h);
    glBindRenderbuffer(GL_RENDERBUFFER, depth_rb);
    glRenderbufferStorage(GL_RENDERBUFFER, gl_reversed_z ? 0x8CAD /* GL_DEPTH32F_STENCIL8 */ : GL_DEPTH24_STENCIL8, w, h);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, color_rb);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, depth_rb);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "OpenGL: incomplete frame target\n");
        return 0;
    }
    fb_w = w;
    fb_h = h;
    layout_view();
    return 1;
}

int plat_video_init(const plat_video_desc *d)
{
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER) != 0) return -1;
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    /* P: optional placement for filming on the user's secondary monitor.
       Ordinary launches retain SDL's centered window. */
    int window_x=SDL_WINDOWPOS_CENTERED,window_y=SDL_WINDOWPOS_CENTERED;
    const char *capture_position=getenv("OPENGT4_WINDOW_POSITION");
    if(capture_position && sscanf(capture_position,"%d,%d",&window_x,&window_y)==2)
        SDL_SetHint(SDL_HINT_WINDOW_NO_ACTIVATION_WHEN_SHOWN,"1");
#ifdef _WIN32
    const char *window_title="OpenGT4";
#else
    const char *window_title="OpenGT4 Steam Deck Edition (experimental) - Based on OpenGT4";
#endif
    window = SDL_CreateWindow(window_title, window_x, window_y, d->width, d->height,
                              SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI |
                              (d->fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
    if (!window) return -1;
    /* 4.3 for the FSR shaders; 3.3 is enough for everything else */
    static const int versions[2][2] = { { 4, 3 }, { 3, 3 } };
    for (int i = 0; i < 2 && !context; i++) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, versions[i][0]);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, versions[i][1]);
        context = SDL_GL_CreateContext(window);
    }
    if (!context || !gl_load()) return -1;
    if (gl_enable_reversed_z()) fprintf(stderr, "OpenGL: reversed float depth\n");
    profile_frames = getenv("OPENGT4_VIDEO_PROFILE") != NULL;
    capture_pipe=getenv("OPENGT4_CAPTURE_PIPE");
    capture_gate=getenv("OPENGT4_CAPTURE_GATE");
    capture_times_path=getenv("OPENGT4_CAPTURE_TIMES");
    const char *capture_size=getenv("OPENGT4_CAPTURE_SIZE");
    if(capture_size && (sscanf(capture_size,"%dx%d",&capture_w,&capture_h)!=2 ||
       capture_w<1 || capture_h<1 || capture_w>4096 || capture_h>4096))capture_w=capture_h=0;
    vsync = d->vsync;
    upscale = d->upscale;
    SDL_GL_SetSwapInterval(vsync ? 1 : 0);

    program2d = gl_program(vs2d, fs2d);
    if (!program2d) return -1;
    u_view2d = glGetUniformLocation(program2d, "view");
    u_tex2d = glGetUniformLocation(program2d, "tex");
    u_kind2d=glGetUniformLocation(program2d,"kind");
    u_filter2d=glGetUniformLocation(program2d,"filter_mode");
    u_text2d=glGetUniformLocation(program2d,"text_sharpness");
    glGenVertexArrays(1, &vao2d);
    glGenBuffers(1, &vbo2d);
    glBindVertexArray(vao2d);
    glBindBuffer(GL_ARRAY_BUFFER, vbo2d);
    glBufferData(GL_ARRAY_BUFFER, sizeof batch, NULL, GL_STREAM_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(vertex2d), (void *)0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(vertex2d), (void *)8);
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(vertex2d), (void *)16);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);
    const uint8_t white[4] = { 255, 255, 255, 255 };
    glGenTextures(1, &white_tex);
    glBindTexture(GL_TEXTURE_2D, white_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    return ensure_framebuffer() ? 0 : -1;
}

void video_flush2d(void)
{
    if (!nbatch) return;
    glUseProgram(program2d);
    glUniform2f(u_view2d, view_w+2*side_margin, view_h);
    glUniform1i(u_tex2d, 0);
    glUniform1i(u_kind2d,batch_kind);
    glUniform1i(u_filter2d,ui_filter);
    glUniform1f(u_text2d,text_sharpness);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, batch_tex ? batch_tex : white_tex);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
    if(batch_fixed_alpha>=0) {
        glBlendColor(0,0,0,batch_fixed_alpha);
        glBlendFuncSeparate(GL_CONSTANT_ALPHA,GL_ONE_MINUS_CONSTANT_ALPHA,GL_ONE,GL_ONE_MINUS_CONSTANT_ALPHA);
    } else glBlendFuncSeparate(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA,GL_ONE,GL_ONE_MINUS_SRC_ALPHA);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    if(ui_panel_on){
        glViewport(ui_panel_x,ui_panel_y,ui_panel_w,ui_panel_h);
        int x=ui_panel_x,y=ui_panel_y,w=ui_panel_w,h=ui_panel_h;
        if(ui_clip_on){
            int right=x+w,bottom=y+h;
            if(ui_clip_x>x)x=ui_clip_x;if(ui_clip_y>y)y=ui_clip_y;
            if(ui_clip_x+ui_clip_w<right)right=ui_clip_x+ui_clip_w;
            if(ui_clip_y+ui_clip_h<bottom)bottom=ui_clip_y+ui_clip_h;
            w=right>x?right-x:0;h=bottom>y?bottom-y:0;
        }
        glEnable(GL_SCISSOR_TEST);glScissor(x,y,w,h);
    }else glViewport((GLint)content_x, (GLint)off_y, (GLsizei)(content_width + .5f), (GLsizei)(view_h * scale_y + .5f));
    glBindVertexArray(vao2d);
    glBindBuffer(GL_ARRAY_BUFFER, vbo2d);
    glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)(nbatch * sizeof(vertex2d)), batch);
    glDrawArrays(GL_TRIANGLES, 0, nbatch);
    if(profile_frames){profile_draws++;profile_quads+=nbatch/6;}
    nbatch = 0;
}

void video_license_image(const plat_texture *t,float x,float y,float w,float h,
                         const uint8_t lut[256][3],const uint8_t mix[3][3])
{
    if(!t)return;
    video_flush2d();
    if(!license_program){
        /* 004A81A8: channel lookup then three GS MODULATE/add passes.
           A: GL capture raster/sample positions still need GS pixel oracle. */
        const char *fs="#version 330 core\n"
            "in vec2 v_uv; uniform sampler2D tex,clut; uniform vec3 mr,mg,mb; out vec4 o;\n"
            "void main(){ivec3 p=ivec3(floor(texture(tex,v_uv).rgb*255.+.5));"
            "int r=int(texelFetch(clut,ivec2(p.r,0),0).r*255.+.5);"
            "int g=int(texelFetch(clut,ivec2(p.g,0),0).g*255.+.5);"
            "int b=int(texelFetch(clut,ivec2(p.b,0),0).b*255.+.5);"
            "ivec3 c=(r*ivec3(mr))/128+(g*ivec3(mg))/128+(b*ivec3(mb))/128;"
            "o=vec4(vec3(min(c,ivec3(255)))/255.,1.);}";
        license_program=gl_program(vs2d,fs);
        if(!license_program)return;
        license_uview=glGetUniformLocation(license_program,"view");license_utex=glGetUniformLocation(license_program,"tex");
        license_uclut=glGetUniformLocation(license_program,"clut");
        license_umix[0]=glGetUniformLocation(license_program,"mr");license_umix[1]=glGetUniformLocation(license_program,"mg");license_umix[2]=glGetUniformLocation(license_program,"mb");
        glGenTextures(1,&license_lut);glBindTexture(GL_TEXTURE_2D,license_lut);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGB8,256,1,0,GL_RGB,GL_UNSIGNED_BYTE,NULL);
    }
    glUseProgram(license_program);
    glUniform2f(license_uview,view_w+2*side_margin,view_h);
    glUniform1i(license_utex,0);
    glUniform1i(license_uclut,1);
    /* Coefficients are bytes represented exactly as floats. */
    for(int i=0;i<3;i++){
        float c[3]={(float)mix[i][0],(float)mix[i][1],(float)mix[i][2]};
        glUniform3fv(license_umix[i],1,c);
    }
    glActiveTexture(GL_TEXTURE0+1);glBindTexture(GL_TEXTURE_2D,license_lut);
    if(!license_lut_valid || memcmp(license_cached_lut,lut,sizeof license_cached_lut)){
        glTexSubImage2D(GL_TEXTURE_2D,0,0,0,256,1,GL_RGB,GL_UNSIGNED_BYTE,lut);
        memcpy(license_cached_lut,lut,sizeof license_cached_lut);license_lut_valid=1;
    }
    glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,t->id);
    glDisable(GL_DEPTH_TEST);glDisable(GL_CULL_FACE);glDisable(GL_BLEND);
    glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
    glViewport((GLint)content_x,(GLint)off_y,(GLsizei)(content_width+.5f),(GLsizei)(view_h*scale_y+.5f));
    const float xs[4]={x,x+w,x,x+w},ys[4]={y,y,y+h,y+h};
    const int order[6]={0,1,2,2,1,3};vertex2d verts[6];
    for(int i=0;i<6;i++){int k=order[i];verts[i]=(vertex2d){xs[k]+side_margin,ys[k],(float)(k&1),k<2?1.f:0.f,{255,255,255,255}};}
    glBindVertexArray(vao2d);glBindBuffer(GL_ARRAY_BUFFER,vbo2d);
    glBufferSubData(GL_ARRAY_BUFFER,0,sizeof verts,verts);glDrawArrays(GL_TRIANGLES,0,6);
    if(profile_frames){profile_draws++;profile_quads++;}
}

static void quad(GLuint tex, int kind, const float x[4], const float y[4], const float u[4], const float v[4], const uint8_t c[4][4])
{
    /* Lists may retain hundreds of faces outside the viewport. Reject their
       quads before a texture change flushes the batch; GL clipping alone
       still submits every invisible glyph and its outline to the driver.
       Use all four vertices so rotated/sheared faces keep their visible ink. */
    float left=x[0],right=x[0],top=y[0],bottom=y[0];
    for(int i=1;i<4;i++){left=fminf(left,x[i]);right=fmaxf(right,x[i]);top=fminf(top,y[i]);bottom=fmaxf(bottom,y[i]);}
    if(right < -side_margin || left > view_w+side_margin || bottom < 0 || top > view_h)return;
    if(batch_fixed_alpha<0 && !(c[0][3]|c[1][3]|c[2][3]|c[3][3]))return;
    if (tex != batch_tex || kind!=batch_kind || nbatch + 6 > BATCH) { video_flush2d(); batch_tex = tex;batch_kind=kind; }
    static const int order[6] = { 0, 1, 2, 2, 1, 3 };
    for (int i = 0; i < 6; i++) {
        vertex2d *b = &batch[nbatch++];
        int k = order[i];
        b->x = x[k]+side_margin; b->y = y[k];
        /* Only full-screen backgrounds extend; text and controls keep their shape. */
        if(kind==0 && fabsf(x[0])<.01f && fabsf(x[2])<.01f && fabsf(x[1]-view_w)<.01f && fabsf(x[3]-view_w)<.01f)
            b->x += (k==0 || k==2)?-side_margin:side_margin;
        b->u = u[k]; b->v = v[k];
        memcpy(b->c, c[k], 4);
    }
}

void plat_video_set_view(float width, float height)
{
    video_flush2d();
    if (width > 0 && height > 0) { view_w = width; view_h = height; }
    layout_view();
}

void plat_set_clip(float x, float y, float w, float h)
{
    video_flush2d();
    if(ui_panel_on){
        ui_clip_on=w>0 && h>0;
        if(ui_clip_on){
            float sx=ui_panel_w/(view_w+2*side_margin),sy=ui_panel_h/view_h;
            ui_clip_x=ui_panel_x+(int)floorf((floorf(x)+side_margin)*sx+.5f);
            ui_clip_y=ui_panel_y+(int)floorf((view_h-floorf(y)-floorf(h+.5f))*sy+.5f);
            ui_clip_w=(int)floorf(floorf(w+.5f)*sx+.5f);
            ui_clip_h=(int)floorf(floorf(h+.5f)*sy+.5f);
            if(x<=0 && x+w>=view_w){ui_clip_x=ui_panel_x;ui_clip_w=ui_panel_w;}
        }
        return;
    }
    if (w <= 0 || h <= 0) { glDisable(GL_SCISSOR_TEST); return; }
    /* same integer rectangle as SDL_RenderSetClipRect, in framebuffer pixels */
    float rx = floorf(x), ry = floorf(y), rw = floorf(w + .5f), rh = floorf(h + .5f);
    int sx = (int)floorf(off_x + rx * scale_x + .5f), sy = (int)floorf(off_y + (view_h - ry - rh) * scale_y + .5f);
    int sw = (int)floorf(rw * scale_x + .5f), sh = (int)floorf(rh * scale_y + .5f);
    if(rx<=0 && rx+rw>=view_w){sx=(int)(content_x+.5f);sw=(int)(content_width+.5f);}
    glEnable(GL_SCISSOR_TEST);
    glScissor(sx, sy, sw > 0 ? sw : 0, sh > 0 ? sh : 0);
}

void plat_video_begin_frame(void)
{
    /* Native UI advances at 60 Hz. A high-refresh display or disabled swap
       interval must not make an idle menu submit unbounded GPU work.
       Sleep before rendering; no spinning and no accumulated catch-up frames. */
    if(menu_pacing || capture_pipe){
        const uint64_t period=1000000000ull/60;
        uint64_t now=plat_time_ns();
        if(menu_deadline && now<menu_deadline){
            SDL_Delay((Uint32)((menu_deadline-now+999999)/1000000));
            now=plat_time_ns();
        }
        if(!menu_deadline || now>menu_deadline+period)menu_deadline=now;
        menu_deadline+=period;
    }
    if(profile_frames){profile_start=plat_time_ns();if(!profile_window)profile_window=profile_start;}
    ensure_framebuffer();
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, fb_w, fb_h);
    glClearColor(0, 0, 0, 1);
    glClearDepth(GL_DEPTH_FAR);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
}

void plat_video_end_frame(void)
{
    video_flush2d();
    capture_framebuffer();
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    int preview_w,preview_h;SDL_GL_GetDrawableSize(window,&preview_w,&preview_h);
    glBlitFramebuffer(0, 0, fb_w, fb_h, 0, 0, preview_w, preview_h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    if(profile_frames){
        glFinish();profile_total+=plat_time_ns()-profile_start;profile_count++;
        if(profile_count==30){
            fprintf(stderr,"video profile: %.3f ms/frame, %.1f draws/frame, %.1f quads/frame, %.1f fps\n",
                    profile_total/1e6/profile_count,(double)profile_draws/profile_count,(double)profile_quads/profile_count, (profile_count-1)*1e9/(profile_start-profile_window));
            profile_count=profile_draws=profile_quads=0;profile_total=profile_window=0;
        }
    }
    /* Offline export consumes the FBO itself. An optional sparse preview
       avoids background-window swap throttling while preserving every
       exported frame. Ordinary gameplay always presents every frame. */
    if(!video_export_fixed_step() || !getenv("OPENGT4_CAPTURE_SPARSE_PREVIEW") || capture_frame%60==0)
        SDL_GL_SwapWindow(window);
    frame_presented=1;
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
}

unsigned char *plat_sdl_read_pixels(int *width, int *height)
{
    video_flush2d();
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    unsigned char *px = malloc((size_t)fb_w * fb_h * 4), *row = malloc((size_t)fb_w * 4);
    if (!px || !row) { free(px); free(row); return NULL; }
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, fb_w, fb_h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    for (int y = 0; y < fb_h / 2; y++) {
        unsigned char *a = px + (size_t)y * fb_w * 4, *b = px + (size_t)(fb_h - 1 - y) * fb_w * 4;
        memcpy(row, a, (size_t)fb_w * 4); memcpy(a, b, (size_t)fb_w * 4); memcpy(b, row, (size_t)fb_w * 4);
    }
    free(row);
    *width = fb_w;
    *height = fb_h;
    return px;
}

/* the game's picture only: the frame target less the side or top bars
   around the display's 4:3 (or 16:9) area */
unsigned char *plat_sdl_read_view(int *width, int *height)
{
    int w, h, x = (int)(content_x + .5f), y = (int)(((float)fb_h - fminf((float)fb_h, content_width * (display_aspect ? 9.f / 16.f : 3.f / 4.f))) * .5f + .5f);
    int cw = (int)(content_width + .5f), ch = fb_h - 2 * y;
    unsigned char *px = plat_sdl_read_pixels(&w, &h), *out;
    if (!px) return NULL;
    if (x < 0 || y < 0 || cw < 1 || ch < 1 || x + cw > w || y + ch > h) { *width = w; *height = h; return px; }
    out = malloc((size_t)cw * ch * 4);
    if (out)
        for (int r = 0; r < ch; r++) memcpy(out + (size_t)r * cw * 4, px + ((size_t)(y + r) * w + x) * 4, (size_t)cw * 4);
    free(px);
    *width = cw;
    *height = ch;
    return out;
}

void plat_video_shutdown(void)
{
    if(capture_stream)fclose(capture_stream);
    if(capture_times)fclose(capture_times);
    free(capture_pixels);capture_pixels=NULL;
    capture_stream=capture_times=NULL;capture_frame=0;capture_attempted=0;
    capture_pipe=capture_gate=capture_times_path=NULL;capture_w=capture_h=0;
    plat_audio_shutdown();
    plat_input_shutdown();
    if(license_lut)glDeleteTextures(1,&license_lut);
    if(license_program)glDeleteProgram(license_program);
    license_lut=license_program=0;license_lut_valid=0;
    if (context) SDL_GL_DeleteContext(context);
    if (window) SDL_DestroyWindow(window);
    context = NULL;
    window = NULL;
    fbo = 0;
    SDL_Quit();
}

void video_set_menu_projection(int enabled)
{video_flush2d();menu_projection=enabled!=0;layout_view();}
void video_set_aspect(int widescreen)
{video_flush2d();display_aspect=widescreen!=0;layout_view();}

/* Widescreen expands horizontal field of view instead of stretching meshes. */
float video_projection_x(void){return view_w/(view_h*(display_aspect?16.f/9.f:4.f/3.f));}

int video_set_upscale(int mode)
{
    if(mode<0 || mode>4 || (mode && !fsr_available()))return 0;
    upscale=mode;return 1;
}
int video_fsr_available(void){return fsr_available();}

void video_begin_scene(void)
{
    video_flush2d();output_w=fb_w;output_h=fb_h;
    static const float ratios[]={1,1.3f,1.5f,1.7f,2};
    int mode=upscale>=0 && upscale<=4?upscale:0;
    if(mode && !fsr_available())mode=0;
    int w=(int)(output_w/ratios[mode]+.5f),h=(int)(output_h/ratios[mode]+.5f);
    GLint max_samples=1;glGetIntegerv(GL_MAX_SAMPLES,&max_samples);int samples=max_samples>=4?4:1;
    if(!scene_fbo){glGenFramebuffers(1,&scene_fbo);glGenFramebuffers(1,&scene_resolve);glGenRenderbuffers(1,&scene_color);glGenRenderbuffers(1,&scene_depth);glGenTextures(1,&scene_tex);}
    if(w!=scene_w || h!=scene_h || samples!=scene_samples) {
        glBindRenderbuffer(GL_RENDERBUFFER,scene_color);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER,samples,GL_RGBA8,w,h);
        glBindRenderbuffer(GL_RENDERBUFFER,scene_depth);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER,samples,gl_reversed_z?0x8CAD:GL_DEPTH24_STENCIL8,w,h);
        glBindFramebuffer(GL_FRAMEBUFFER,scene_fbo);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_RENDERBUFFER,scene_color);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_DEPTH_STENCIL_ATTACHMENT,GL_RENDERBUFFER,scene_depth);
        if(glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE){glBindFramebuffer(GL_FRAMEBUFFER,fbo);return;}
        glBindTexture(GL_TEXTURE_2D,scene_tex);glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,w,h,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
        scene_w=w;scene_h=h;scene_samples=samples;
        printf("scene: %dx%d -> %dx%d, MSAA %dx, %s\n",w,h,output_w,output_h,samples,mode?"FSR 1":"native");
    }
    glBindFramebuffer(GL_FRAMEBUFFER,scene_fbo);glDisable(GL_SCISSOR_TEST);glEnable(GL_MULTISAMPLE);
    glViewport(0,0,w,h);glClearColor(0,0,0,1);glClearDepth(GL_DEPTH_FAR);glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT|GL_STENCIL_BUFFER_BIT);
    fb_w=w;fb_h=h;layout_view();scene_active=1;
}

void video_end_scene(void)
{
    if(!scene_active)return;
    video_flush2d();glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER,scene_resolve);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,scene_tex,0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER,scene_fbo);
    glBlitFramebuffer(0,0,scene_w,scene_h,0,0,scene_w,scene_h,GL_COLOR_BUFFER_BIT,GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER,fbo);
    GLuint resolved=scene_tex;
    if(scene_w!=output_w || scene_h!=output_h) {
        GLuint reconstructed=fsr_resolve(scene_tex,scene_w,scene_h,output_w,output_h);
        if(reconstructed)resolved=reconstructed;
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER,scene_resolve);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,resolved,0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER,fbo);
    int source_w=resolved==scene_tex?scene_w:output_w,source_h=resolved==scene_tex?scene_h:output_h;
    glBlitFramebuffer(0,0,source_w,source_h,0,0,output_w,output_h,GL_COLOR_BUFFER_BIT,GL_LINEAR);
    fb_w=output_w;fb_h=output_h;scene_active=0;layout_view();glBindFramebuffer(GL_FRAMEBUFFER,fbo);
}

void video_ui_quality(int filter,float sharpness)
{ video_flush2d();ui_filter=filter<0?0:filter>2?2:filter;text_sharpness=fminf(1,fmaxf(0,sharpness)); }

int video_set_vsync(int enabled)
{ if(SDL_GL_SetSwapInterval(enabled?1:0)<0)return 0;vsync=enabled!=0;return 1; }

plat_texture *plat_texture_create(int w, int h, const uint8_t *rgba)
{
    plat_texture *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    /* UI is sampled once at drawable resolution. Never run EASU/RCAS on
       glyph atlases, transparency or frame slices. */
    glGenTextures(1, &t->id);
    glBindTexture(GL_TEXTURE_2D, t->id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    t->w = w;
    t->h = h;
    return t;
}

plat_texture *video_grab(plat_texture *t, float x, float y, float w, float h)
{
    video_flush2d();
    int vx, vy, vw, vh;
    video_viewport(x, y, w, h, &vx, &vy, &vw, &vh);
    if (vw <= 0 || vh <= 0) return t;
    if (!t) { t = calloc(1, sizeof *t); if (!t) return NULL; glGenTextures(1, &t->id); }
    glBindTexture(GL_TEXTURE_2D, t->id);
    if (t->w != vw || t->h != vh) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, vw, vh, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        t->w = vw; t->h = vh;
    }
    t->screen_capture=x<=0 && x+w>=view_w;
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, vx, vy, vw, vh);
    return t;
}

void plat_texture_update(plat_texture *t, const uint8_t *rgba)
{
    if (!t || !rgba) return;
    if (batch_tex == t->id) video_flush2d();
    glBindTexture(GL_TEXTURE_2D, t->id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, t->w, t->h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
}

void plat_texture_destroy(plat_texture *t)
{
    if (!t) return;
    if (batch_tex == t->id) video_flush2d();
    glDeleteTextures(1, &t->id);
    free(t);
}

void plat_draw_quad4(const plat_texture *t, float x, float y, float w, float h,
                     float u0, float v0, float u1, float v1, const uint8_t c[4][4])
{
    /* 002182E0 +0xA4..0xF0 draws the retained framebuffer in its own
       dimensions. A capture includes the wide viewport; map every scaled
       piece in that space, not only quads exactly spanning 0..view_w.
       GL adapter A: this does not certify the original GS rasterization. */
    if(t && t->screen_capture && side_margin>0){
        float ratio=(view_w+2*side_margin)/view_w;
        x=view_w*.5f+(x-view_w*.5f)*ratio;w*=ratio;
    }
    const float px[4] = { x, x + w, x, x + w }, py[4] = { y, y, y + h, y + h };
    const float tu[4] = { u0, u1, u0, u1 }, tv[4] = { v0, v0, v1, v1 };
    quad(t ? t->id : 0, 0, px, py, tu, tv, c);
}

void plat_draw_quad(const plat_texture *t, float x, float y, float w, float h,
                    float u0, float v0, float u1, float v1, const uint8_t c[4])
{
    uint8_t cc[4][4];
    for (int i = 0; i < 4; i++) memcpy(cc[i], c, 4);
    plat_draw_quad4(t, x, y, w, h, u0, v0, u1, v1, cc);
}

void plat_draw_solid_quad(const float xy[4][2],const uint8_t c[4])
{
    float x[4],y[4];const float uv[4]={0,0,0,0};uint8_t colors[4][4];
    for(int i=0;i<4;i++){x[i]=xy[i][0];y[i]=xy[i][1];memcpy(colors[i],c,4);}
    quad(0,0,x,y,uv,uv,colors);
}

void plat_draw_quad_blend(const plat_texture *t,float x,float y,float w,float h,
                           float u0,float v0,float u1,float v1,
                           const uint8_t color[4],float factor)
{
    video_flush2d();
    batch_fixed_alpha=fminf(1,fmaxf(0,factor));
    plat_draw_quad(t,x,y,w,h,u0,v0,u1,v1,color);
    video_flush2d();
    batch_fixed_alpha=-1;
}

void plat_draw_glyph(const plat_texture *t, float x, float y, float w, float h,
                     float baseline, float shear, float u0, float v0, float u1, float v1, const uint8_t c[4])
{
    float px[4], py[4];
    const float tu[4] = { u0, u1, u0, u1 }, tv[4] = { v0, v0, v1, v1 };
    uint8_t cc[4][4];
    for (int i = 0; i < 4; i++) {
        py[i] = y + (i / 2) * h;
        px[i] = x + (i % 2) * w + shear * (py[i] - baseline);
        memcpy(cc[i], c, 4);
    }
    quad(t ? t->id : 0, 1, px, py, tu, tv, cc);
}

void plat_draw_rotated(const plat_texture *t, float x, float y, float w, float h,
                       float cx, float cy, float degrees, const uint8_t c[4])
{
    float a = degrees * 0.01745329252f, sn = sinf(a), cs = cosf(a), px[4], py[4];
    const float tu[4] = { 0, 1, 0, 1 }, tv[4] = { 0, 0, 1, 1 };
    uint8_t cc[4][4];
    for (int i = 0; i < 4; i++) {
        float xx = x + (i % 2) * w - cx, yy = y + (i / 2) * h - cy;
        px[i] = cx + cs * xx - sn * yy;
        py[i] = cy + sn * xx + cs * yy;
        memcpy(cc[i], c, 4);
    }
    quad(t ? t->id : 0, 0, px, py, tu, tv, cc);
}

/* ---- for the 3D renderer ---- */

GLuint video_texture_id(const plat_texture *t) { return t ? t->id : 0; }

void video_viewport(float x, float y, float w, float h, int *vx, int *vy, int *vw, int *vh)
{
    *vx = (int)floorf(off_x + x * scale_x + .5f);
    *vy = (int)floorf(off_y + (view_h - y - h) * scale_y + .5f);
    *vw = (int)floorf(w * scale_x + .5f);
    *vh = (int)floorf(h * scale_y + .5f);
    /* Only the actual full-screen viewport uses the content bounds.
       ModelFace (00217058 projection, 004A59B8) can grow its frustum
       beyond the screen: clamping only X compresses the model's width. */
    if(x==0 && w==view_w){*vx=(int)(content_x+.5f);*vw=(int)(content_width+.5f);}
}

void video_ui_panel(float y,float h)
{
    video_flush2d();ui_clip_on=0;
    if(h>0){
        video_viewport(0,y,640,h,&ui_panel_x,&ui_panel_y,&ui_panel_w,&ui_panel_h);
        ui_panel_on=1;
    }else{ui_panel_on=0;glDisable(GL_SCISSOR_TEST);}
    layout_view();
}

plat_texture *video_capture_frame(plat_texture *t)
{return frame_presented?video_grab(t,0,0,view_w,view_h):t;}

float video_view_height(void){return view_h;}
