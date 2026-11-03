/*
 * OpenGT4 viewer: opens the user's GT4 disc image and shows the game's UI
 * screens with the native UI renderer. First step towards the menu; there is
 * no game logic yet.
 *
 *   opengt4 <disc.iso> [project] [screen] [--shot out.png]
 *   arrows = focus, Enter = activate, Escape = cancel, F10 = quit.
 *   F1/F2 = debug screen browsing, PageUp/PageDown = debug project browsing.
 */
#include "platform/platform.h"
#include "plat_sdl.h"
#include "movie.h"
#include "showroom.h"
#include "mot1.h"
#include "race_view.h"
#include "photo_travel.h"
#include "race_game.h"
#include "race_hud.h"
#include "race_run.h"
#include "boot_notice.h"
#include "loading_screen.h"
#include "menu.h"
#include "gtmode_game.h"
#include "career_vehicle.h"
#include "music.h"
#include "pregrid_audio.h"
#include "ui.h"
#include "ui_motion.h"
#include "render3d.h"
#include "vol.h"
#include "video_settings.h"
#include "launcher.h"
#include "save_ui.h"
#include "save.h"
#include "replay_file.h"
#include "career_event.h"
#include "career_license_disc.h"
#include "video_gl.h"

#include "miniz.h"

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

static const char *projects[] = { "arcade", "gtmode", "boot", "option", "setting", "event", "quick", "save", "quick-arcade", "quick-tt", "quick-event", "quick-championship", "labomode", "quick-license", "photo_shoot", "quick-mission", "logger", "slide", "quick-freerun", "quick-practice", "quick-mt", "photo_save" };
static photo_travel *presentation_photo;
static int photo_project(int i){return i>=0 && i<(int)(sizeof projects/sizeof projects[0]) && !strcmp(projects[i],"photo_shoot");}
/* Arcade and Time Trial are distinct authored projects (arcade.ad sequences,
   quick-arcade.mproject ArcadeRoot / quick-tt.mproject TimeTrialRoot). */
static int race_quick_project(int project){return project==8 || project==9 || project==10 || project==11 || project==13 || project==15 || project==18 || project==19 || project==20;}
static int free_mode;   /* the race's GT Mode Free Run (1), Practice (2), Photo Drive (3) or Power & Speed (4): their quick menus */
static const char *race_quick_route(int time_trial){return (free_mode==1 || free_mode==3)?"quick-freerun:FreeRunRoot":free_mode==2?"quick-practice:PracticeRoot":free_mode==4?"quick-mt:QuickRoot":time_trial?"quick-tt:TimeTrialRoot":"quick-arcade:ArcadeRoot";}

#define NPROJECTS (int)(sizeof projects / sizeof projects[0])
static struct {
    int active,rng_ready,result_applied,has_distances,championship,retired;
    int mission;   /* a driving mission (0011BB60): the game's car, quick-mission */
    unsigned round,rounds;int32_t points[6];
    uint32_t grand_prize;char series[32];
    gt4_race_desc championship_grid;uint8_t bodies[6][0x4b8];
    uint32_t rng,award,a_distance,b_distance;
    uint8_t best,row[151];
    char race[32];
} career_race;
static struct {
    int active,result_applied;
    char code[32];
    gt4_career_license_attempts visit;
    uint32_t best_time;
    uint32_t last_time;   /* the last attempt's time (race+0x8C before the menu clears it) */
} career_license;

static uint64_t presentation_clock;
static video_settings *app_video_settings;
static save_ui save_screen;
static int present_profile_picker(menu *m);
static gt4_replay_file replay_to_save;   /* the race the memory card's save screen writes */
static ui_project *dialog_ui;
static const char *dialog_shot;
static int dialog_shot_scripted;
static const char *loading_shot;
static void screenshot(const char *path);
static int present_dialog(menu *,const char *,const char *,int,const char *,const char *);
static int present_event(menu *,void *);
static char *scripted_keys;
static float scripted_interval;
static showroom *presentation_cars;
static showroom *split_selection_cars[2];
static const char *presentation_disc;
static race_game *presentation_race;
static int presentation_pregrid;
static uint64_t pregrid_clock;
static int draw_pregrid_face(void *user,float x,float y,float w,float h,float opacity)
{
    (void)user;
    if(!presentation_pregrid || x!=0 || y!=0 || w!=640 || h!=480)return 0;
    uint64_t now=plat_time_ns();
    race_game_pregrid_update(presentation_race,pregrid_clock?(now-pregrid_clock)/1e9:1.0/60);
    pregrid_clock=now;
    video_begin_scene();int ready=race_game_draw_pregrid(presentation_race);video_end_scene();
    if(ready && opacity<1){uint8_t black[4]={0,0,0,(uint8_t)(255*(1-opacity))};plat_draw_quad(NULL,0,0,640,480,0,0,1,1,black);}
    return ready;
}
/* Draws the persistent movie layer under pages without their own movie. */
static void (*presentation_movie_layer)(menu *m);

/* ModelFace assets belong to UI packages, independent of race/showroom state. */
typedef struct {r3d_mesh *gpu;float ground_pivot[3];} ui_mesh;
static void *ui_mesh_create(const uint8_t *bytes,size_t size)
{
    gt4_model *model=gt4_model_decode_part(bytes,size,0,0);
    if(!model)return NULL;
    /* Static ModelFace conversion does not execute the original VU anchor
       adjustment. Keep an otherwise detached ground-shadow quad under its
       model. Preserve the authored quad size and small directional offsets. */
    float base_lo[3]={1e9f,1e9f,1e9f},base_hi[3]={-1e9f,-1e9f,-1e9f};
    float body_lo[3]={1e9f,1e9f,1e9f},body_hi[3]={-1e9f,-1e9f,-1e9f};
    int base_count=0,body_count=0;
    for(int i=0;i<model->count;i++){
        gt4_triangle *t=&model->triangles[i];
        int ground=fabsf(t->v[0].p[1])<1e-5f && fabsf(t->v[1].p[1])<1e-5f && fabsf(t->v[2].p[1])<1e-5f;
        float *lo=ground?base_lo:body_lo,*hi=ground?base_hi:body_hi;
        if(ground)base_count++;else body_count++;
        for(int v=0;v<3;v++)for(int k=0;k<3;k++){lo[k]=fminf(lo[k],t->v[v].p[k]);hi[k]=fmaxf(hi[k],t->v[v].p[k]);}
    }
    if(base_count==2 && body_count>0){
        float shift[3]={0};
        for(int k=0;k<3;k+=2){float delta=(base_lo[k]+base_hi[k]-body_lo[k]-body_hi[k])*.5f;
            if(fabsf(delta)>(body_hi[k]-body_lo[k])*.25f)shift[k]=delta;}
        for(int i=0;i<model->count;i++){gt4_triangle *t=&model->triangles[i];
            if(fabsf(t->v[0].p[1])<1e-5f && fabsf(t->v[1].p[1])<1e-5f && fabsf(t->v[2].p[1])<1e-5f)
                for(int v=0;v<3;v++)for(int k=0;k<3;k+=2)t->v[v].p[k]-=shift[k];}
    }
    ui_mesh *mesh=calloc(1,sizeof *mesh);
    if(mesh){
        mesh->gpu=r3d_mesh_create(model);
        if(body_count>0)for(int k=0;k<3;k+=2)
            mesh->ground_pivot[k]=(body_lo[k]+body_hi[k])*.5f;
        if(!mesh->gpu){free(mesh);mesh=NULL;}
    }
    gt4_model_free(model);
    return mesh;
}
static void ui_mesh_destroy(void *opaque)
{ui_mesh *mesh=opaque;if(mesh){r3d_mesh_destroy(mesh->gpu);free(mesh);}}
static void ui_mesh_draw(void *mesh,const mp_node *node,const float angles[3],float x,float y,float w,float h,const tex1_image *environment)
{
    if(w<=0 || h<=0)return;
    ui_mesh *icon=mesh;
    double camera[3]={0,7.2,14},tilt[3]={-25,0,0};
    mp_numbers(node,"camera_trans",camera,3);
    mp_numbers(node,"camera_rotate",tilt,3);
    const float rad=0.01745329252f;
    float a=(float)angles[0]*rad,b=(float)angles[1]*rad,c=(float)angles[2]*rad;
    float ca=cosf(a),sa=sinf(a),cb=cosf(b),sb=sinf(b),cc=cosf(c),sc=sinf(c);
    r3d_view v={0};
    /* ModelFace's authored composition: Rz * Rx * Ry. The tilt stays
       fixed while heading turns in model space. */
    float rotation[9]={cc*cb-sc*sa*sb,-sc*ca,cc*sb+sc*sa*cb,
                      sc*cb+cc*sa*sb,cc*ca,sc*sb-cc*sa*cb,
                      -ca*sb,sa,ca*cb};
    memcpy(v.rot,rotation,sizeof rotation);
    for(int i=0;i<3;i++)v.cam[i]=(float)camera[i];
    float pitch=(float)tilt[0]*rad,yaw=(float)tilt[1]*rad;
    v.r[0]=cosf(yaw);v.r[2]=-sinf(yaw);
    v.u[0]=sinf(yaw)*sinf(pitch);v.u[1]=cosf(pitch);v.u[2]=cosf(yaw)*sinf(pitch);
    v.f[0]=-sinf(yaw)*cosf(pitch);v.f[1]=sinf(pitch);v.f[2]=-cosf(yaw)*cosf(pitch);
    float fov=(float)mp_number(node,"view_angle",30);
    if(fov<1 || fov>170)fov=30;
    /* 00217058 +0x2B0..0x3A8: the widget's rectangle in the framebuffer
       (00215FE0: x * 640/640, y * 448/480, D_006184D0 mode 0 640x448),
       the projection centred on it with one focal length for both axes,
       half its height over tan(view_angle / 2) (004A59B8). Back in the
       640x480 units: x * 0.5 * h * 448/480 / tan, y * 0.5 * h / tan.
       The viewport is the rectangle grown k times about its centre, so
       the model may leave it as the PS2's full-screen frustum lets it. */
    const float k=2;
    float t=tanf(fov*rad*.5f);
    v.focal=320*.5f*h*(448.f/480.f)/(k*w*t);
    v.focal_y=224*.5f/(k*t);
    /* the Dutch flag car is the size of the others (2.38 x 0.71 x 1.19
       against 2.69 x 0.71 x 1.14): no framing of its own */
    const char *model_path=mp_string(node,"model_set_path");
    if(model_path && !strcmp(model_path,"image/gtmode/map/car_old_timer.mdl")){
        /* This mesh's body is offset from the set origin (~0.498 on Z).
           Turn about its own ground center, retaining its authored rest
           placement. Other map assets keep their original origin. */
        double rest[3]={0};mp_numbers(node,"model_rotate",rest,3);
        float ra=(float)rest[0]*rad,rb=(float)rest[1]*rad,rc=(float)rest[2]*rad;
        float xa=cosf(ra),xs=sinf(ra),ya=cosf(rb),ys=sinf(rb),za=cosf(rc),zs=sinf(rc);
        float rest_matrix[9]={za*ya-zs*xs*ys,-zs*xa,za*ys+zs*xs*ya,
                             zs*ya+za*xs*ys,za*xa,zs*ys-za*xs*ya,
                             -xa*ys,xs,xa*ya};
        ui_model_pivot_offset(v.rot,rest_matrix,icon->ground_pivot,v.pos);
        if(getenv("OPENGT4_ICON_PIVOT_TRACE")){
            float center[3];for(int i=0;i<3;i++){
                center[i]=v.pos[i];for(int k=0;k<3;k++)center[i]+=v.rot[i*3+k]*icon->ground_pivot[k];
            }
            fprintf(stderr,"historic pivot: yaw=%.3f center=%.6f,%.6f,%.6f\n",angles[1],center[0],center[1],center[2]);
        }
    }
    r3d_ui_light light={0};
    double direction[4]={0,.629198,.777245,0},ambient[4]={50,50,50,255};
    double diffuse[4]={255,255,255,255},specular[4]={127,127,127,255};
    mp_numbers(node,"l0_pos",direction,4);mp_numbers(node,"l0_amb",ambient,4);
    mp_numbers(node,"l0_dif",diffuse,4);mp_numbers(node,"l0_spe",specular,4);
    for(int i=0;i<3;i++) {
        light.direction[i]=(float)direction[i];light.ambient[i]=(float)ambient[i]/255;
        light.diffuse[i]=(float)diffuse[i]/255;light.specular[i]=(float)specular[i]/255;
    }
    /* quick-event.mproject PrizeRoot.model supplies two lights and env0;
       the former one-light/no-environment path lost the trophy's metal. */
    r3d_lights lights={1};
    memcpy(lights.direction[0],light.direction,sizeof light.direction);memcpy(lights.ambient[0],light.ambient,sizeof light.ambient);
    memcpy(lights.diffuse[0],light.diffuse,sizeof light.diffuse);memcpy(lights.specular[0],light.specular,sizeof light.specular);
    for(int l=1;l<4;l++){
        char key[32];snprintf(key,sizeof key,"l%d_enable",l);if(!mp_number(node,key,0))continue;
        int dst=lights.count++;double values[4]={0};
        const char *suffix[]={"pos","amb","dif","spe"};float *out[]={lights.direction[dst],lights.ambient[dst],lights.diffuse[dst],lights.specular[dst]};
        for(int prop=0;prop<4;prop++){memset(values,0,sizeof values);snprintf(key,sizeof key,"l%d_%s",l,suffix[prop]);mp_numbers(node,key,values,4);
            for(int axis=0;axis<3;axis++)out[prop][axis]=(float)values[axis]/(prop?255.f:1.f);}
    }
    r3d_begin_ui_environment(x+w*.5f-k*w,y+h*.5f-k*h,2*k*w,2*k*h,environment);
    r3d_draw_lit(icon->gpu,&v,&lights);r3d_end();
}

static int is_popup(const char *name);
static void movie_layer(menu *m);
static void dialog_background(menu *m);
/* Is page `name` on top, or right under a popup on top (SettingPopup over
   CarRoot keeps the showroom visible)? */
static int page_below_popup(menu *m, const char *name)
{
    ui_project *p = menu_ui(m);
    int depth = menu_page_depth(m);
    const char *top = ui_root_name(p, menu_active_root(m));
    if (top && !strcmp(top, name)) return 1;
    return depth > 1 && is_popup(top) && !strcmp(ui_root_name(p, menu_page_at(m, depth - 2)), name);
}
/* A popup page (SettingPopup, the confirm dialogs) is pushed over the page
   it belongs to and shows it underneath; other pushed pages (GameRoot over
   TopRoot) show only the movie layer below them (movie_layer). */
static int is_popup(const char *name)
{
    return name && (strstr(name, "Popup") || strstr(name, "Dialog"));
}
/* cursor.ad's ProgressDialog over the page while a script holds it open
   (labomode's "Now loading" around a card read) */
static void draw_pages(menu *m, int *drawn, int *skipped);
static int progress_root=-1,progress_open;
static void present_progress(menu *m,int state,const char *message,int button,float progress)
{
    (void)m;
    if(!dialog_ui)return;
    if(progress_root<0)for(int i=0;i<ui_root_count(dialog_ui);i++)if(!strcmp(ui_root_name(dialog_ui,i),"ProgressDialog"))progress_root=i;
    if(progress_root<0)return;
    ui_widget *page=ui_root_widget(dialog_ui,progress_root),*pane=ui_widget_child(page,"Dialog");
    ui_widget *bar=ui_widget_child(ui_widget_child(pane,"Progress"),"Bar");
    ui_widget *fill=ui_widget_child(bar,"progress"),*bg=ui_widget_child(bar,"bg");
    if(state==1){
        if(pane)pane->y=120;   /* open(): Dialog.y = 120 */
        ui_widget *text=ui_widget_child(ui_widget_child(pane,"Message"),"message");
        if(text){free(text->text);text->text=strdup(message?message:"");}
        ui_widget *ok=ui_widget_child(ui_widget_child(pane,"Button"),"OK");
        if(ok)ok->visible=button!=0;
        if(fill)fill->progress=1;   /* the PC reads the file at once: the bar full (P) */
        progress_open=1;
    }else if(state==0){
        if(progress_open && getenv("OPENGT4_PROGRESS_SHOT")){   /* tests: the dialog as it closes */
            video_set_menu_pacing(1);video_set_menu_projection(1);plat_video_begin_frame();draw_pages(m,NULL,NULL);
            screenshot(getenv("OPENGT4_PROGRESS_SHOT"));plat_video_end_frame();
        }
        progress_open=0;
    }
    else if(fill && progress>=0)fill->progress=fminf(1,progress);   /* ProgressFace draws its fraction */
}

static void draw_pages(menu *m, int *drawn, int *skipped)
{
    ui_project *p = menu_ui(m);
    ui_set_scene_renderer(p,draw_pregrid_face,NULL);
    int depth = menu_page_depth(m);
    if (depth > 1 && is_popup(ui_root_name(p, menu_active_root(m)))) ui_draw_under(p, menu_page_at(m, depth - 2));
    else movie_layer(m);
    ui_draw_root(p, menu_active_root(m), drawn, skipped);
    if(progress_open && dialog_ui && progress_root>=0)ui_draw_root(dialog_ui,progress_root,NULL,NULL);
}

/* GT Mode's CarFaces (dealers, garage, car wash...), drawn in place by the
   UI: action_type "normal" puts just the car over the page with the
   widget's own camera and lights (l0..l2 and world_ambient, as ModelFace
   reads them). An explicit CarModel pose also uses the authored frame;
   the remaining advanced showroom path uses the studio. */
static void draw_car_face(void *user,const ui_widget *w,float x,float y,float wd,float h,float opacity)
{
    (void)opacity;
    menu *m=user;
    showroom *cars=presentation_cars;int slot=0;
    /* arcade.ad SplitRoot.set_car/move_car: independent P1/P2 CarFaces,
       each with its own resident model, paint and animation clock. */
    if(!strcmp(ui_root_name(menu_ui(m),w->root),"SplitRoot"))
        for(const ui_widget *p=w->parent;p;p=p->parent){
            int port=p->name && !strcmp(p->name,"P1")?0:p->name && !strcmp(p->name,"P2")?1:-1;
            if(port<0)continue;
            if(!split_selection_cars[port] && w->car_code)split_selection_cars[port]=showroom_open(presentation_disc);
            cars=split_selection_cars[port];slot=port+1;break;
        }
    static uint64_t last[3];
    uint64_t now=plat_time_ns();
    float dt=last[slot]?(float)((now-last[slot])/1e9):0;last[slot]=now;
    if(!cars || !w->car_code)return;
    showroom_set_wheel(cars,w->car_wheel);
    showroom_set_wing(cars,w->car_wing);
    showroom_set_selection(cars,w->car_code,w->car_color_index);
    if(!w->car_normal && (!w->car_pose_set || !strcmp(ui_root_name(menu_ui(m),w->root),"BuyCarRoot"))) {
        /* the studio fills the view: its own scene target (MSAA, FSR) */
        video_begin_scene();showroom_draw(cars,w->car_code,dt);video_end_scene();
    } else {
        /* over the page as drawn so far: straight into the frame (the scene
           target is cleared opaque and would cover the page's background) */
        showroom_face f;memset(&f,0,sizeof f);
        f.x=x;f.y=y;f.w=wd;f.h=h;
        f.crew_data=w->car_crew_data;f.crew_size=w->car_crew_size;f.crew_serial=w->car_crew_serial;f.crew_motion=w->car_crew_motion;f.crew_time=w->car_crew_time;
        f.crew_loop=w->car_crew_loop;
        f.models=w->car_models;f.model_count=w->car_model_count;f.motions=w->car_motions;f.motion_count=w->car_motion_count;
        memcpy(f.service_geometry,w->car_service_geometry,sizeof f.service_geometry);f.service_geometry_valid=w->car_service_geometry_valid;f.additional_scale=w->car_additional_scale;
        f.pose_valid=w->car_pose_set && strcmp(ui_root_name(menu_ui(m),w->root),"BuyCarRoot");
        /* func_00157378: advanced owner copies x/y, negates z and adds
           exactly 180 degrees to the pose yaw before scene projection. */
        float yaw=w->model_angles[1];if(!w->car_normal)yaw=yaw+180.0f;
        f.car_angle=yaw*.017453292519943295f;
        memcpy(f.car_position,w->car_position,sizeof f.car_position);
        if(!w->car_normal)f.car_position[2]=-f.car_position[2];
        double trans[3]={0,.98,9},rot[3]={-3,0,0},angle[1]={25};
        mp_numbers(w->src,"camera_trans",trans,3);mp_numbers(w->src,"camera_rotate",rot,3);mp_numbers(w->src,"view_angle",angle,1);
        for(int i=0;i<3;i++){f.eye[i]=(float)trans[i];f.rotate[i]=(float)rot[i];}
        f.view_angle=(float)angle[0];
        if(w->car_camera){float fov=0;
            float time=w->car_camera_loop?gt4_mot1_wrap(w->car_camera,w->car_camera_time):w->car_camera_time;
            f.look_valid=gt4_mot1_camera(w->car_camera,time,f.look,&fov);
            if(f.look_valid)f.view_angle=fov*57.29577951308232f;
            /* 001570A4..001570D8: the service camera also drives body lift. */
            gt4_mot1_value(w->car_camera,"lift",time,&f.service_lift);
        }
        /* LightSet ctor 00210830 +0x9c..0x12c: world RGB 0,
           position (0,1,0,0), ambient 0x3e999999, diffuse/specular
           0.5; only light 0 enabled. Preserve normalized defaults before
           overriding them with authored byte-valued attributes. */
        double world[4],default_pos[4],default_amb[4],default_dif[4],default_spe[4];
        ui_light_defaults(world,default_pos,default_amb,default_dif,default_spe);
        mp_numbers(w->src,"world_ambient",world,4);
        for(int l=0;l<3;l++) {
            char key[16];double on[1]={l==0},pos[4],amb[4],dif[4],spe[4];
            memcpy(pos,default_pos,sizeof pos);memcpy(amb,default_amb,sizeof amb);
            memcpy(dif,default_dif,sizeof dif);memcpy(spe,default_spe,sizeof spe);
            if(l){snprintf(key,sizeof key,"l%d_enable",l);mp_numbers(w->src,key,on,1);}
            if(!on[0])continue;
            snprintf(key,sizeof key,"l%d_pos",l);mp_numbers(w->src,key,pos,4);
            snprintf(key,sizeof key,"l%d_amb",l);mp_numbers(w->src,key,amb,4);
            snprintf(key,sizeof key,"l%d_dif",l);mp_numbers(w->src,key,dif,4);
            snprintf(key,sizeof key,"l%d_spe",l);mp_numbers(w->src,key,spe,4);
            int n=f.lights.count++;
            for(int k=0;k<3;k++) {
                f.lights.direction[n][k]=(float)pos[k];
                f.lights.ambient[n][k]=(float)(amb[k]+(n==0?world[k]:0))/255;
                f.lights.diffuse[n][k]=(float)dif[k]/255;f.lights.specular[n][k]=(float)spe[k]/255;
            }
        }
        f.lights.specular_scale=.18f;   /* as the arcade CarFace path */
        if(w->car_camera && w->car_motion_count){
            showroom_service_channels(&f,w->car_camera,w->car_camera_time);
            f.service_opacity*=w->car_model_opacity; /* 00217058: CarModel+4C. */
        }
        showroom_draw_face(cars,w->car_code,dt,&f);
    }
}

static void draw_arcade_car_face(void *user,const ui_widget *w,float x,float y,float wd,float h,float opacity)
{
    menu *m=user;
    /* CarRoot's full showroom is already drawn by the main scene path.
       SplitRoot's two authored normal CarFaces draw inside their panels. */
    if(!strcmp(ui_root_name(menu_ui(m),w->root),"SplitRoot"))draw_car_face(user,w,x,y,wd,h,opacity);
}

/* Script sync waits keep presenting actors, without re-entering the VM. */
static int music_faded;
static void fade_menu_music(float seconds){music_faded=1;plat_audio_fade_music(seconds);}

/* While the loading overlay reveals the destination, its authored fade and
   widget actors must advance too. Keep the cursor at the outgoing selection. */
static void animate_loading_destination(menu *m,float dt)
{
    ui_project *p=menu_ui(m);
    int paused=ui_cursor_pause(p,1);
    menu_animate(m,dt>1.0f/30.0f?1.0f/30.0f:dt);
    ui_cursor_pause(p,paused);
}

static void update_present_pcm(menu *m);
static void present_wait(menu *m, float seconds, void *user)
{
    (void)user;
    /* Loading readiness follows the original fade actors, independently of
       asset-read latency; no additional fixed sleep is needed. */
    uint64_t start=plat_time_ns();
    int trace=getenv("OPENGT4_PRESENT_TRACE") && seconds>=1 && menu_active_root(m)>=0 &&
        !strcmp(ui_root_name(menu_ui(m),menu_active_root(m)),"PresentRoot");
    static unsigned ceremony_wait;unsigned trace_id=trace?++ceremony_wait:0;int captured=0;
    if(trace){printf("award Sync %u begin %.3f loading=%d\n",trace_id,seconds,loading_screen_active());fflush(stdout);}
    if(!presentation_clock) presentation_clock=start;
    /* Sync(1/60) is one field on the PS2. Wait to a running deadline, not
       from now: with vsync a frame lasts almost exactly 1/60 s, and timing
       from each call's start made every other Sync present two frames
       (17/33 ms, the Tune Shop dialog's entrance stuttering). A stale
       deadline (no Sync for a while) restarts from now. */
    static uint64_t deadline;
    uint64_t span=(uint64_t)(seconds*1e9);
    if(!deadline || start>deadline+UINT64_C(50000000) || deadline>start+UINT64_C(1000000000))deadline=start;
    deadline+=span;
    const uint64_t slack=UINT64_C(2000000);
    do {
        SDL_PumpEvents();
        if (SDL_HasEvent(SDL_QUIT)) {ui_fade(menu_ui(m),0,0,0);break;}
        uint64_t now=plat_time_ns();
        float dt=(float)fmin((now-presentation_clock)/1e9,1.0/30.0);
        if(loading_screen_revealing())animate_loading_destination(m,dt);
        else if(!loading_screen_active())menu_animate(m,dt);
        presentation_clock=now;
        update_present_pcm(m);
        video_set_menu_pacing(1);video_set_menu_projection(1);
        plat_video_begin_frame();
        if(loading_screen_active()){if(loading_screen_revealing())draw_pages(m,NULL,NULL);loading_screen_draw();if(loading_shot && loading_screen_ready()){screenshot(loading_shot);loading_shot=NULL;}plat_video_end_frame();plat_sleep_ms(1);continue;}
        if(page_below_popup(m,"CarRoot")){video_begin_scene();showroom_draw(presentation_cars,menu_selected_car(m),dt);showroom_set_color(presentation_cars,menu_car_color(m));video_end_scene();}
        draw_pages(m,NULL,NULL);
        if(trace && !captured && (plat_time_ns()-start)/1e9>.1){
            const char *prefix=getenv("OPENGT4_PRESENT_CAPTURE");
            if(prefix){char path[512];snprintf(path,sizeof path,"%s-%u.png",prefix,trace_id);screenshot(path);}
            captured=1;
        }
        if(trace && captured==1 && (plat_time_ns()-start)/1e9>.75){
            const char *prefix=getenv("OPENGT4_PRESENT_CAPTURE");
            if(prefix){char path[512];snprintf(path,sizeof path,"%s-%u-late.png",prefix,trace_id);screenshot(path);}
            captured=2;
        }
        plat_video_end_frame();
        plat_sleep_ms(1);
    } while (plat_time_ns()+slack<deadline ||
             (loading_screen_active() && !loading_screen_revealing() && !loading_screen_ready()));
    if(trace){printf("award Sync %u end\n",trace_id);fflush(stdout);}
}

static int load_car_face(menu *m,const ui_widget *w,void *user)
{
    (void)user;if(!presentation_cars)return 0;
    uint64_t start=plat_time_ns();int state;
    while((state=showroom_prepare(presentation_cars,w->car_code))==0){
        if(plat_time_ns()-start>UINT64_C(30000000000) || SDL_HasEvent(SDL_QUIT))return 0;
        present_wait(m,1.f/60,NULL);
    }
    return state==1;
}

/* the cars' ratings the Arcade picks its opponents by (the physics' setup) */
static gt4_car_ratings *app_car_ratings;
static gt4_career_vehicle *app_car_factory;
static void prepare_menu_audio(menu *m,void *user);
static int rate_cars(void *ctx,const uint64_t *keys,int n,float *out){return gt4_car_ratings_get(ctx,keys,n,out);}

/* CPU/script and GL asset preparation share this presentation-thread pump.
   It paints only the independent loader and never re-enters the menu VM. */
static void pump_loading_work(void *user)
{
    (void)user;
    if(!loading_screen_active() || loading_screen_revealing())return;
    uint64_t now=plat_time_ns();
    if(presentation_clock && now-presentation_clock<UINT64_C(16000000))return;
    if(getenv("OPENGT4_LOADING_TRACE"))fprintf(stderr,"loading work frame: gap=%.3f ms\n",presentation_clock?(now-presentation_clock)/1e6:0);
    SDL_PumpEvents();video_set_menu_pacing(1);video_set_menu_projection(1);
    plat_video_begin_frame();loading_screen_draw();plat_video_end_frame();
    presentation_clock=plat_time_ns();
}

static menu *load(gt4_vol *v, int k)
{
    char err[256] = "";
    menu *m = menu_open_presented(v,k==7?"boot":projects[k],err,sizeof err,pump_loading_work,NULL);
    if (!m) fprintf(stderr, "project %s: %s\n", projects[k], err);
    if(m && k==7)save_ui_attach(&save_screen,m);
    if(m){ui_set_capture_renderer(menu_ui(m),video_grab);menu_set_profile_picker(m,present_profile_picker);}
    menu_set_presenter(m,present_wait,NULL);
    menu_set_music_fader(m,fade_menu_music);
    menu_set_audio_preparer(m,prepare_menu_audio,NULL);
    menu_set_dialog_presenter(m,present_dialog);menu_set_progress_presenter(m,present_progress);
    menu_set_event_presenter(m,present_event,NULL);
    if(app_car_ratings)menu_set_car_rater(m,rate_cars,app_car_ratings);
    if(m && k==3 && app_video_settings)video_settings_attach(app_video_settings,m);
    /* every project's ModelFace: the map's icons, the event prize's trophy */
    if(m)ui_set_model_renderer(menu_ui(m),(ui_model_renderer){ui_mesh_create,ui_mesh_draw,ui_mesh_destroy});
    if(m && !strcmp(projects[k],"gtmode")) {
        ui_set_car_renderer(menu_ui(m),draw_car_face,m);
        menu_set_car_loader(m,load_car_face,NULL);
    }
    if(m && !strcmp(projects[k],"arcade"))ui_set_car_renderer(menu_ui(m),draw_arcade_car_face,m);
    if(m)ui_prepare_dialogs(menu_ui(m),pump_loading_work,NULL);
    return m;
}

static void switch_to(gt4_vol *vol, menu **cache, menu **mn, int *proj, char *next)
{
    char *requested_root=strchr(next,':');if(requested_root)*requested_root++=0;
    for (int i = 0; i < NPROJECTS; i++) if (!strcmp(next, projects[i])) {
        if(getenv("OPENGT4_MENU_TRACE"))printf("project route: %s -> %s\n",projects[*proj],projects[i]);
        char photo_course[32]="",photo_car[96]="";int photo_color=0;
        if(photo_project(i) && !menu_photo_request(*mn,photo_course,photo_car,&photo_color)) {
            menu_confirm(*mn,"Photo Travel","Choose a car in your garage before entering Photo Travel.");
            ui_fade(menu_ui(*mn),0,0,0);return;
        }
        int loading_boundary=(i==1 && *proj!=1) || (*proj==1 && i==0) || race_quick_project(i) || race_quick_project(*proj) || photo_project(i) || photo_project(*proj);
        int own_loading=loading_boundary && !loading_screen_active();
        if(own_loading){
            plat_audio_set_music(NULL,NULL);music_faded=1;
            loading_screen_begin();
        }
        /* Give cached project loads the same visible, stationary loading
           cursor as page loads. This minimum delay is presentation only. */
        ui_cursor_icon(menu_ui(*mn),"load");
        ui_cursor_hold(menu_ui(*mn),1);
        present_wait(*mn,menu_loading_duration(*mn),NULL);
        ui_cursor_hold(menu_ui(*mn),0);
        ui_cursor_icon(menu_ui(*mn),NULL);
        if (!cache[i]) cache[i] = load(vol, i);
        if(i==0 && race_quick_project(*proj))menu_arcade_return(cache[i]);
        if (!cache[i]) {if(own_loading)loading_screen_end();return;}
        if(photo_project(i) && !photo_travel_load_from(presentation_photo,cache[i],*mn,photo_course,photo_car,photo_color,pump_loading_work,NULL)) {
            fprintf(stderr,"Photo Travel: scene/car load failed: %s / %s\n",photo_course,photo_car);
            ui_fade(menu_ui(*mn),0,0,0);if(own_loading)loading_screen_end();return;
        }
        if((*proj==10 || *proj==11) && i==1 && career_race.active &&
           !menu_commit_career_distances(cache[1],*mn,career_race.a_distance,career_race.b_distance,&career_race.has_distances)){
            fprintf(stderr,"cannot commit career mileage before return\n");return;
        }
        if(*proj==11 && i==1 && career_race.active && career_race.championship){
            /* 00119618..19664 / 00119BD8: Exit after a completed stage
               advances before suspension; final stage and Retire clear it. */
            uint8_t live[0x90];uint64_t keys[6];int32_t paints[6];
            if(!menu_championship_context_get(*mn,live)){fprintf(stderr,"cannot read championship context\n");return;}
            for(unsigned j=0;j<6;j++){
                memcpy(keys+j,career_race.bodies[j]+(j?8:0),8);
                paints[j]=(int8_t)career_race.bodies[j][0x18];
            }
            int finished_final=career_race.result_applied && career_race.round+1==career_race.rounds;
            unsigned round=career_race.round+(career_race.result_applied && !finished_final);
            gt4_career_championship_context(live,!career_race.retired && !finished_final,
                round,career_race.rounds-1,career_race.series,keys,paints,career_race.points);
            if(!menu_championship_context_set(*mn,live)){fprintf(stderr,"cannot store championship context\n");return;}
            menu_save_refresh(cache[1]);
        }
        if(*proj==13 && i==1 && career_license.active){
            if(!menu_license_leave(cache[1],career_license.code,&career_license.visit)){fprintf(stderr,"cannot record licence visit\n");return;}
            career_license.active=0;
            menu_license_return_prepare(cache[1]);
            menu_save_refresh(*mn);
        }
        if(*proj==4 && !menu_setting_end(*mn)){fprintf(stderr,"cannot finish setting\n");return;}
        if(*proj==10 || *proj==11 || *proj==13 || *proj==15)menu_save_capture(*mn);
        if(i==4)menu_save_capture(*mn);
        int career_return=((*proj==10 || *proj==11 || *proj==15) && i==1 && career_race.active) || (*proj==13 && i==1);
        if((i==10 || i==11 || i==15) && career_race.active)menu_quick_event_context(cache[i],career_race.race);
        if(i==13 && career_license.active)menu_quick_event_context(cache[i],career_license.code);
        int new_career=i==7?menu_take_new_career(*mn):0;
        char labo_arg[32]="";
        if(i==12)menu_take_labo_arg(*mn,labo_arg,sizeof labo_arg);
        if(i==7)save_screen.enter_gtmode=menu_take_gtmode_save_entry(*mn);
        menu_set_origin(cache[i], projects[*proj]);
        if(i==4)menu_setting_context(cache[i],*mn);
        if(i==17)menu_jukebox_context(cache[i],*mn);
        if(i==18||i==19)menu_setting_context(cache[i],*mn);   /* FreeRun/Practice exits read major_menu_project/page */
        if(i==12 && !strcmp(labo_arg,"PhotoRoot"))menu_setting_context(cache[i],*mn);   /* labomode.ad PhotoRoot exit reads major_menu_project/page */
        if(i==4 && !menu_setting_begin(cache[i])){fprintf(stderr,"cannot open setting\n");if(own_loading)loading_screen_end();return;}
        /* Project changes share the cursor just like pages and common dialogs:
           retain its outgoing position before the destination takes focus. */
        ui_cursor_transfer(menu_ui(cache[i]),menu_ui(*mn));
        *mn = cache[i]; *proj = i;
        int target=menu_active_root(*mn)<0?0:menu_active_root(*mn);
        if(requested_root)for(int k=0;k<ui_root_count(menu_ui(*mn));k++)
            if(!strcmp(ui_root_name(menu_ui(*mn),k),requested_root)){target=k;break;}
        /* Clear a cached outgoing fade before authored onStart/panIn runs;
           never replace the game's transition after it has been selected. */
        ui_fade(menu_ui(*mn),0,0,0);
        if(career_return){if(!menu_enter_career_return(*mn))fprintf(stderr,"career reward entry failed\n");}
        else if(i==12 && labo_arg[0]){if(!menu_open_labo(*mn,labo_arg))fprintf(stderr,"labomode: %s failed\n",labo_arg);}
        else if(!requested_root && race_quick_project(i)){
            if(!menu_enter_quick_start(*mn))fprintf(stderr,"career quick selector failed\n");
        }
        else menu_enter(*mn,target);
        if(new_career)save_ui_new_career(&save_screen);
        /* PresentRoot.onInitialize queues get_paper/get_car (gtmode.ad
           L14466/L14473). Finish the loading overlay before dispatching:
           their authored Syncs are part of the visible ceremony. */
        if(own_loading){ui_prepare_root(menu_ui(*mn),menu_active_root(*mn),pump_loading_work,NULL);loading_screen_end();plat_video_set_view(640,480);}
        if(!loading_screen_active())menu_tick(*mn);
        presentation_clock=plat_time_ns();
        return;
    }
    fprintf(stderr, "menu: unsupported project '%s'\n", next);
}

static void switch_requested(gt4_vol *vol, menu **cache, menu **mn, int *proj)
{
    char next[64];
    if (!menu_take_project(*mn, next, sizeof next)) return;
    switch_to(vol, cache, mn, proj, next);
}

/* The race the quick menu (quick-arcade's ArcadeRoot) shows before and
   after it: the grid from pole, the player last (the Arcade's grid), each
   car's power and weight as the game computes them. */
/* the session's best time (00112E78 opens it at 0x157529FF, 00112620
   lowers it after each race to the player's best lap) */
static uint32_t session_best=0x157529ffu;
static int32_t machine_best_speed;   /* Power & Speed: MQuickWork +0x49C */
static char machine_race[32];       /* its race_code (mt400_testcourse ...): quick-mt.ad raceLabel */
/* logger.ad analyze_start: the last race replayed a frame at a time
   (00122FA8's thread), 12 ms of steps a frame */
static race_game *logger_game;
static int logger_analysis_begin(void *user){(void)user;char msg[160];int ok=race_game_logger_analyze_begin(logger_game,msg,sizeof msg);if(!ok)fprintf(stderr,"logger: %s\n",msg);return ok;}
static int logger_analysis_step(void *user,float *progress){(void)user;return race_game_logger_analyze_step(logger_game,0.012,progress);}
static void logger_analysis_end(void *user,int ok)
{
    (void)user;race_game_logger_analyze_end(logger_game,1,ok);
    menu_logger_source src;
    if(ok && race_game_logger_source(logger_game,&src)){
        if(career_license.active && career_license.result_applied && (src.time[0]==0 || src.time[0]==0x157529ffu))src.time[0]=career_license.last_time;
        menu_set_logger_source(&src);
    }
}
static void session_best_update(race_game *g,menu *mn,const char *course,const char *car)
{
    if(career_license.active){menu_set_quick_best(career_license.best_time);return;}
    {   /* 00115CB4..00115D74 (Power & Speed): with car 0 through, the
           session's best time or top speed (+0x498 / +0x49C, km/h x 100:
           m/s x 3600 + 5, /10), then the record (001161A0) */
        int test;uint32_t t;float v;
        if(free_mode==4 && race_game_machine_result(g,&test,&t,&v)){
            int unit=test==1?(strcmp(course,"lasvegas")?0:3):test==2?1:2;
            if(unit==2){int32_t k=(int32_t)((v*60.f*60.f+5.f)/10.f);if(k>machine_best_speed)machine_best_speed=k;menu_set_quick_best_speed(machine_best_speed);}
            else if(t<session_best){session_best=t;menu_set_quick_best(session_best);}
            menu_machine_record_insert(mn,unit,car,unit==2?0x157529ffu:t,unit==2?v:0);
            return;
        }
    }
    uint32_t best=race_game_best_lap(g);
    /* 00113320 (single race) / 00117898 (Time Trial): a new session best
       goes into the course's ranking. */
    if(best<session_best && !career_race.active && !race_game_split(g) && !race_game_replaying(g))menu_course_record_insert(mn,course,car,best);
    if(best<session_best)session_best=best;
    menu_set_quick_best(session_best);menu_set_quick_ghost(race_game_has_ghost(g));
}
static void quick_race_set(race_game *g,const char *course,const char *car,int color,
                           const char *const *others,const int *colors,int n,int tire)
{
    menu_quick_race q;memset(&q,0,sizeof q);
    session_best=0x157529ffu;menu_set_quick_best(session_best);machine_best_speed=0;menu_set_quick_best_speed(0);
    snprintf(q.course,sizeof q.course,"%s",course);
    q.entries=n+1;q.player=n;q.tire=tire;q.dry=1;
    if(free_mode==4)snprintf(q.race,sizeof q.race,"%s",machine_race);
    race_game_gear_range(g,&q.gear,&q.gear_min,&q.gear_max);   /* 00111B50 */
    for(int k=0;k<=n;k++) {
        int e=k<n?k+1:0,power=0,weight=0;
        snprintf(q.grid[k].car,sizeof q.grid[k].car,"%s",k<n?others[k]:car);
        q.grid[k].color=k<n?(colors?colors[k]:0):color;
        if(app_car_ratings && race_game_car_id(g,e))gt4_car_ratings_spec(app_car_ratings,race_game_car_id(g,e),&power,&weight);
        q.grid[k].power=power;q.grid[k].weight=weight;
    }
    menu_set_quick_race(&q);
}

static int career_race_result(race_game *game,menu *career,menu *quick)
{
    if(career_license.active){
        if(career_license.result_applied || race_game_replaying(game))return 1;
        int32_t state;uint32_t elapsed;
        if(!race_game_license_result(game,&state,&elapsed))return 0;
        time_t clock_time=time(NULL);struct tm date;uint32_t packed=0;
#ifdef _WIN32
        int clock_ok=!localtime_s(&date,&clock_time);
#else
        int clock_ok=localtime_r(&clock_time,&date)!=NULL;
#endif
        if(clock_ok)packed=(uint32_t)(date.tm_year+1900)|((uint32_t)(date.tm_mon+1)<<16)|((uint32_t)date.tm_mday<<24);
        /* Original 004320B8 uses the console clock, not calendar.today.
           PC local clock is the host replacement for that hardware call. */
        if(!menu_license_attempt(career,career_license.code,&career_license.visit,state,elapsed,packed))return 0;
        career_license.last_time=elapsed;
        career_license.best_time=menu_license_best(career,career_license.code);menu_save_refresh(quick);menu_set_quick_best(career_license.best_time);
        fprintf(stderr,"licence attempt %s: state %d time %u best %u\n",career_license.code,state,elapsed,career_license.best_time);
        career_license.result_applied=1;return 1;
    }
    if(!career_race.active || career_race.result_applied || race_game_replaying(game))return 1;
    gt4_race_hud hud;int position=race_game_event_position(game);
    if(position<0 || !race_game_player_hud(game,&hud))return 0;
    uint32_t prize=hud.finished?gt4_career_race_prize(career_race.row,(uint64_t)position):0;
    if(career_race.mission){
        /* 0011BB60: through (car +0x5B6) -> 0011C328 (record, prize total,
           calendar with the race time car +0x71C) and 0011C1D8 (the grid's
           names and times); otherwise 0011C2E0. The game's car: no wear. */
        menu_save_capture(quick);menu_save_refresh(career);
        if(!menu_apply_mission_result(career,career_race.race,hud.finished,position,hud.best_lap,hud.aspec_points,prize,&career_race.award))return 0;
        menu_quick_race q;if(!race_game_quick_results(game,&q))return 0;
        snprintf(q.race,sizeof q.race,"%s",career_race.race);q.finished=1;q.prize=career_race.award;
        q.can_replay=race_game_can_replay(game);menu_set_quick_race(&q);
        menu_save_refresh(quick);menu_quick_event_result(quick,1,career_race.award);
        fprintf(stderr,"mission %s: through %d position %d time %u prize %u\n",career_race.race,hud.finished,position,hud.best_lap,prize);
        career_race.result_applied=1;return 1;
    }
    /* 00114498 must execute before replay destroys the completed state.
       Restore the latest paid profile before applying another retry. */
    menu_save_capture(quick);menu_save_refresh(career);
    uint8_t wear[32];if(!race_game_player_wear(game,wear) || !menu_apply_car_wear(career,wear))return 0;
    if(!race_game_player_distances(game,&career_race.a_distance,&career_race.b_distance))return 0;
    career_race.has_distances=1;
    if(career_race.championship){
        uint8_t order[6];int n=race_game_finish_order(game,order);if(n!=6)return 0;
        int32_t before[6];memcpy(before,career_race.points,sizeof before);
        const gt4_career_race_lookup_data *rules=gt4_game_race_lookup_data();
        gt4_career_championship_points(career_race.points,order,6,hud.finished,(const int8_t *)rules->driver_points);
        int16_t units;memcpy(&units,career_race.row+0x2c,2);
        if(!menu_apply_championship_result(career,career_race.race,career_race.series,hud.finished,
            career_race.round+1==career_race.rounds,position,hud.best_lap,race_game_driver_mode(game),hud.aspec_points,prize,
            career_race.points,units,&career_race.award,&career_race.grand_prize))return 0;
        menu_quick_race q;if(!race_game_quick_results(game,&q))return 0;
        q.championship=1;q.session_number=(int)career_race.round;q.final_session=career_race.round+1==career_race.rounds;
        q.series_rank=gt4_career_championship_rank(career_race.points);q.grand_prize=career_race.grand_prize;
        for(unsigned i=0;i<6;i++){unsigned driver=order[i];
            q.total_points[i]=career_race.points[driver];q.round_points[i]=(int32_t)((uint32_t)career_race.points[driver]-(uint32_t)before[driver]);if(!driver)q.player=(int)i;}
        snprintf(q.race,sizeof q.race,"%s",career_race.race);q.finished=1;q.prize=career_race.award;q.can_replay=race_game_can_replay(game);menu_set_quick_race(&q);
    }else{
        if(!menu_apply_career_result(career,career_race.race,hud.finished,position,hud.best_lap,race_game_driver_mode(game),hud.aspec_points,prize,&career_race.best,&career_race.award))return 0;
        menu_quick_race q;if(!race_game_quick_results(game,&q))return 0;
        snprintf(q.race,sizeof q.race,"%s",career_race.race);q.finished=1;q.prize=career_race.award;
        q.can_replay=race_game_can_replay(game);menu_set_quick_race(&q);
    }
    menu_save_refresh(quick);menu_quick_event_result(quick,1,career_race.award);
    career_race.result_applied=1;
    return 1;
}

static int input_name(const char *name, menu_input *out)
{
    static const char *names[] = {"up", "down", "left", "right", "accept", "back", "triangle", "circle", "square"};
    for (int i = 0; i < (int)(sizeof names/sizeof names[0]); i++) if (!strcmp(name, names[i])) { *out = (menu_input)i; return 1; }
    return 0;
}

/* Saves the current frame (before present) as a PNG. */
static void screenshot(const char *path)
{
    int w = 0, h = 0;
    unsigned char *px = plat_sdl_read_pixels(&w, &h);
    if (px) {
        size_t len = 0;
        void *png = tdefl_write_image_to_png_file_in_memory(px, w, h, 4, &len);
        FILE *f = png ? fopen(path, "wb") : NULL;
        if (f) { fwrite(png, 1, len, f); fclose(f); printf("saved %s\n", path); }
        mz_free(png);
    }
    free(px);
}

/* Consume before dispatch: a script may enter a nested modal event loop
   during this input and must receive the following queued input there. */
static char *next_scripted_key(void)
{
    char *key=scripted_keys;
    if(!key || !*key)return NULL;
    char *next=strchr(key,',');
    if(next)*next++=0;
    scripted_keys=next;return key;
}
static int present_event(menu *m,void *user)
{
    (void)user;
    present_wait(m,scripted_interval>0?scripted_interval:1.0f/60,NULL);
    if(dialog_shot && !dialog_shot_scripted){
        video_set_menu_pacing(1);video_set_menu_projection(1);
        plat_video_begin_frame();draw_pages(m,NULL,NULL);screenshot(dialog_shot);
        plat_video_end_frame();dialog_shot=NULL;
    }
    plat_key_event ev;
    while(plat_sdl_poll(&ev)){
        const Uint8 *held=SDL_GetKeyboardState(NULL);
        menu_set_pad_held(m,plat_sdl_menu_buttons()|(held[SDL_SCANCODE_Q]?MENU_PAD_L1:0)|(held[SDL_SCANCODE_E]?MENU_PAD_R1:0));
        menu_input input;int valid=1;
        switch(ev.key){
        case SDLK_UP:input=MENU_UP;break;case SDLK_DOWN:input=MENU_DOWN;break;
        case SDLK_LEFT:input=MENU_LEFT;break;case SDLK_RIGHT:input=MENU_RIGHT;break;
        case SDLK_RETURN:case SDLK_KP_ENTER:input=MENU_ACCEPT;break;
        case SDLK_ESCAPE:input=ev.pad?MENU_CIRCLE:MENU_BACK;break;case SDLK_F4:input=MENU_TRIANGLE;break;
        case SDLK_INSERT:if(ev.pad)input=MENU_SQUARE;else valid=0;break;default:valid=0;break;
        }
        if(valid){menu_send_input(m,input);break;}
    }
    if(plat_should_quit())return 0;
    char *key=next_scripted_key();
    if(key){
        menu_input input;
        if(!strcmp(key,"shot") && dialog_shot){   /* tests: --dialog-shot taken here, inside the modal */
            video_set_menu_pacing(1);video_set_menu_projection(1);
            plat_video_begin_frame();draw_pages(m,NULL,NULL);screenshot(dialog_shot);
            plat_video_end_frame();dialog_shot=NULL;
        }else if(strcmp(key,"wait") && input_name(key,&input)){
            menu_send_input(m,input);
            printf("modal input %s -> %s focus=%s\n",key,
                ui_root_name(menu_ui(m),menu_active_root(m)),menu_focus_name(m));
        }
    }else if(plat_sdl_get_dialog_answer()>=0){
        menu_send_input(m,plat_sdl_get_dialog_answer()?MENU_ACCEPT:MENU_BACK);
    }
    menu_tick(m);return !plat_should_quit();
}

static void dialog_text(ui_widget *w,const char *text)
{if(w){free(w->text);w->text=strdup(text?text:"");}}

/* Synchronous to the script, but rendered inside the game without VM reentry. */
static int present_dialog(menu *m,const char *title,const char *message,int type,const char *yes,const char *no)
{
    /* cursor.adc cases 0/2: OK; 1/4: Yes; 3: No initially. */
    int confirm=type==1 || type==3 || type==4;
    if(!dialog_ui)return -1;
    int root=-1;for(int i=0;i<ui_root_count(dialog_ui);i++)if(!strcmp(ui_root_name(dialog_ui,i),"ConfirmDialog"))root=i;
    if(root<0)return -1;
    ui_widget *page=ui_root_widget(dialog_ui,root),*pane=ui_widget_child(page,"Dialog");
    ui_widget *heading=ui_widget_child(pane,"Title"),*body=ui_widget_child(pane,"Message"),*buttons=ui_widget_child(pane,"Buttons");
    ui_widget *single=ui_widget_child(buttons,"OK"),*pair=ui_widget_child(buttons,"YesNo");
    ui_widget *positive=ui_widget_child(pair,"Yes"),*negative=ui_widget_child(pair,"No");
    dialog_text(ui_widget_child(heading,"title"),title);
    ui_widget_child(heading,"bgE")->visible=0;ui_widget_child(heading,"titleE")->visible=0;
    ui_widget_child(body,"caution")->visible=0;
    dialog_text(ui_widget_child(body,"message"),message);
    ui_widget *message_face=ui_widget_child(body,"message");
    message_face->w=300;message_face->h=50;message_face->scale_x=message_face->scale_y=1;
    ui_text_wrap(dialog_ui,message_face,300);ui_text_adjust_scale(dialog_ui,message_face);
    float message_size[2];ui_text_size(dialog_ui,message_face,message_size);
    body->h=fmaxf(82,message_size[1]+32);buttons->y=body->y+body->h;
    pane->h=buttons->y+buttons->h;pane->y=(480-pane->h)*.5f;
    ui_widget *dialog_bg=ui_widget_child(pane,"bg");dialog_bg->h=pane->h;
    single->visible=!confirm;pair->visible=confirm;
    dialog_text(ui_widget_child(positive,"label"),yes && *yes?yes:"OK");
    dialog_text(ui_widget_child(negative,"label"),no && *no?no:"Cancel");
    /* Common dialog uses bit 2 for focus, represented by native FBox focus. */
    int selected=type==3?0:1,forced=plat_sdl_get_dialog_answer(),result=-1;
    if(forced>=0)selected=confirm?forced:1;
    /* cursor.adc open_motion/close_motion; native MMoveActor 002C1960
       and MFadeActor 0020C478. Run actors and cursor on the same original
       update clock, including the speed ratio and unsnapped panel tail. */
    float resting_y=pane->y,motion_time=0;
    float speed=menu_cursor_speed(m);
    float move_ratio=0.20000000298023224f*speed;
    float fade_period=0.5f/speed,fade_step=1.0f/(fade_period*60.0f);
    double motion_ticks=0;
    int closing=0;
    pane->y=480;page->opacity=0;
    positive->focused=confirm && selected;negative->focused=confirm && !selected;single->focused=!confirm;
    ui_cursor_transfer(dialog_ui,menu_ui(m));
    /* Popup actors do not inherit the outgoing-page adapter pause. */
    ui_fade(dialog_ui,0,0,0);ui_cursor_pause(dialog_ui,0);
    ui_cursor_select(dialog_ui,confirm?(selected?positive:negative):single);
    ui_cursor_speed(dialog_ui,speed);
    uint64_t last=plat_time_ns();
    printf("in-game dialog: %s / %s\n",title,message);
    do {
        plat_key_event ev;
        while(plat_sdl_poll(&ev)) {
            if(closing || motion_time<.5f)continue;
            if((ev.key==SDLK_ESCAPE && !ev.pad) || ev.key==SDLK_F4 || (ev.pad && ev.key==SDLK_INSERT)){menu_play_effect(m,"cancel");result=0;break;}
            if(ev.key==SDLK_LEFT || ev.key==SDLK_RIGHT || ev.key==SDLK_UP || ev.key==SDLK_DOWN)if(confirm){selected=!selected;menu_play_effect(m,"cursor");}
            if(ev.key==SDLK_RETURN || ev.key==SDLK_KP_ENTER || (ev.pad && ev.key==SDLK_ESCAPE)){menu_play_effect(m,"ok");result=selected;break;}
        }
        if(plat_should_quit())return 0;
        uint64_t now=plat_time_ns();float dt=(float)((now-last)/1e9);last=now;
        dt=fminf(dt,.1f);
        if(result>=0 && !closing){closing=1;motion_time=0;ui_cursor_hold(dialog_ui,1);}
        positive->focused=confirm && selected;negative->focused=confirm && !selected;single->focused=!confirm;
        ui_cursor_select(dialog_ui,confirm?(selected?positive:negative):single);
        motion_ticks+=(double)dt*60.0;
        while(motion_ticks>=1.0-1e-6) {
            motion_ticks-=1;motion_time+=1.0f/60.0f;
            float destination=closing?(result?-pane->h:480):resting_y;
            pane->y=gt4_move_value(pane->y,destination,move_ratio);
            page->opacity=gt4_fade_value(page->opacity,closing?0:1,closing?-fade_step:fade_step);
            menu_animate(m,1.0f/60.0f);ui_animate(dialog_ui,root,1.0f/60.0f);
        }
        video_set_menu_pacing(1);video_set_menu_projection(1);
        uint64_t draw_start=plat_time_ns();
        plat_video_begin_frame();
        if(page_below_popup(m,"CarRoot")){video_begin_scene();showroom_draw(presentation_cars,menu_selected_car(m),dt);video_end_scene();}
        dialog_background(m);
        ui_draw_root(dialog_ui,root,NULL,NULL);
        if(dialog_shot && !closing && motion_time>=.5f){screenshot(dialog_shot);dialog_shot=NULL;}
        plat_video_end_frame();
        if(getenv("OPENGT4_DIALOG_TRACE"))fprintf(stderr,"dialog frame: %.3f ms\n",(plat_time_ns()-draw_start)/1e6);
        if(forced>=0 && !closing && motion_time>=.5f){menu_play_effect(m,"ok");result=selected;}
        SDL_Delay(1);
    }while(!closing || motion_time<.5f);
    printf("dialog motion: entry=480 exit=%.1f result=%d close=%.3fs\n",result?-pane->h:480,result,motion_time);
    pane->y=resting_y;page->opacity=1;
    ui_cursor_transfer(menu_ui(m),dialog_ui);
    ui_cursor_hold(menu_ui(m),0);
    presentation_clock=plat_time_ns();
    return result;
}

/* P: profile chooser replaces a PS2 card slot without switching the
   active player. Uses the existing in-game dialog widgets and pad input. */
static int present_profile_picker(menu *m)
{
 int slots[GT4_SAVE_SLOTS],count=0;gt4_save_info info[GT4_SAVE_SLOTS];
 for(int i=1;i<=GT4_SAVE_SLOTS;i++)if(gt4_save_info_get(i,&info[count])>0)slots[count++]=i;
 if(!count){present_dialog(m,"Player Profiles","No saved profiles found. Create a profile from Save Games.",0,"OK",NULL);return 0;}
 int root=-1;for(int i=0;i<ui_root_count(dialog_ui);i++)if(!strcmp(ui_root_name(dialog_ui,i),"ConfirmDialog"))root=i;
 if(root<0)return 0;
 ui_widget *page=ui_root_widget(dialog_ui,root),*pane=ui_widget_child(page,"Dialog");
 ui_widget *body=ui_widget_child(pane,"Message"),*buttons=ui_widget_child(pane,"Buttons"),*bg=ui_widget_child(pane,"bg");
 ui_widget *group=ui_widget_child(pane,"Profiles");
 if(!group){group=ui_widget_from_text(dialog_ui,root,"Composite {name string {\"Profiles\"} geometry rectangle {0 45 320 300}}");ui_widget_append(pane,group);
  ui_widget *sample=ui_widget_child(ui_widget_child(buttons,"YesNo"),"Yes");
  for(int i=0;i<GT4_SAVE_SLOTS;i++){ui_widget *row=ui_widget_copy(dialog_ui,sample);row->x=30;row->y=i*32;row->w=260;row->h=24;
   for(int k=0;k<row->nkids;k++){row->kids[k]->w=260;row->kids[k]->h=24;}
   ui_widget_append(group,row);
  }
 }
 float old_y=pane->y,old_h=pane->h,old_bg_h=bg->h;int bv=body->visible,bt=buttons->visible;
 ui_widget *message=ui_widget_child(body,"message"),*caution=ui_widget_child(body,"caution"),*title=ui_widget_child(pane,"Title");
 int mv=message->visible,cv=caution->visible;
 message->visible=caution->visible=0;ui_widget_child(title,"bgE")->visible=0;ui_widget_child(title,"titleE")->visible=0;

 pane->h=60+count*32;pane->y=(480-pane->h)*.5f;bg->h=pane->h;body->visible=1;buttons->visible=0;group->visible=1;page->opacity=1;
 dialog_text(ui_widget_child(ui_widget_child(pane,"Title"),"title"),"Select Player Profile");
 for(int i=0;i<GT4_SAVE_SLOTS;i++){group->kids[i]->visible=i<count;group->kids[i]->focused=0;
  if(i<count){char label[160];snprintf(label,sizeof label,"%d: %s",slots[i],info[i].name);dialog_text(ui_widget_child(group->kids[i],"label"),label);}
 }
 int selected=0,result=-1;ui_fade(dialog_ui,0,0,0);ui_cursor_transfer(dialog_ui,menu_ui(m));ui_cursor_hold(dialog_ui,0);ui_cursor_pause(dialog_ui,0);
 uint64_t last=plat_time_ns();
 while(result<0 && !plat_should_quit()){
  char *key=next_scripted_key();menu_input input;int capture=0;
  if(key && !strcmp(key,"shot"))capture=1;
  else if(key && input_name(key,&input)){
   if(input==MENU_BACK)result=0;
   if(input==MENU_UP || input==MENU_DOWN)selected=(selected+(input==MENU_UP?count-1:1))%count;
   if(input==MENU_ACCEPT)result=slots[selected];
  }
  plat_key_event ev;while(plat_sdl_poll(&ev)){
   if((ev.key==SDLK_ESCAPE && !ev.pad) || ev.key==SDLK_F4 || (ev.pad && ev.key==SDLK_INSERT))result=0;
   if(ev.key==SDLK_UP || ev.key==SDLK_DOWN){selected=(selected+(ev.key==SDLK_UP?count-1:1))%count;menu_play_effect(m,"cursor");}
   if(ev.key==SDLK_RETURN || ev.key==SDLK_KP_ENTER || (ev.pad && ev.key==SDLK_ESCAPE))result=slots[selected];
  }
  for(int i=0;i<count;i++)group->kids[i]->focused=i==selected;
  ui_cursor_select(dialog_ui,group->kids[selected]);uint64_t now=plat_time_ns();
  ui_animate(dialog_ui,root,fminf(.1f,(float)((now-last)/1e9)));last=now;
  video_set_menu_pacing(1);video_set_menu_projection(1);plat_video_begin_frame();ui_draw_under(menu_ui(m),menu_active_root(m));ui_draw_root(dialog_ui,root,NULL,NULL);
  if(capture && dialog_shot){screenshot(dialog_shot);dialog_shot=NULL;}plat_video_end_frame();
 }
 group->visible=0;for(int i=0;i<count;i++)group->kids[i]->focused=0;
 pane->y=old_y;pane->h=old_h;bg->h=old_bg_h;body->visible=bv;buttons->visible=bt;message->visible=mv;caution->visible=cv;
 ui_cursor_transfer(menu_ui(m),dialog_ui);ui_cursor_hold(menu_ui(m),0);presentation_clock=plat_time_ns();
 printf("local profile selected: %d\n",result>0?result:0);menu_play_effect(m,result>0?"ok":"cancel");return result>0?result:0;
}

/* Semantic state for headless screenshot/integration checks. */
static void report_choices(const ui_widget *w)
{
    if(!w)return;
    if(w->name && (!strcmp(w->name,"NoticeBase") || (w->parent && w->parent->name && !strcmp(w->parent->name,"Notice"))))
        printf("notice %s visible=%d opacity=%.3f rect=%.1f,%.1f,%.1f,%.1f text=%s\n",w->name,w->visible,w->opacity,w->x,w->y,w->w,w->h,w->text?w->text:"");
    if(!strcmp(w->type,"ModelFace"))
        printf("model %s/%s mode=%d yaw=%.3f speed=%.3f\n",w->parent?w->parent->name:"",w->name,
               w->model_target_mode,w->model_angles[1],w->model_velocity[1]);
    if(!strcmp(w->type,"CarFace") && w->car_code)
        printf("car pose %s motion=%d explicit=%d yaw=%.3f speed=%.3f\n",
               w->car_code,w->car_motion,w->car_pose_set,w->model_angles[1],w->car_spin);
    if(!strcmp(w->type,"OptionMenu")) {
        ui_widget *item=ui_option_item(w,w->focus_index);
        printf("choice %s/%s index=%d item=%s\n",w->parent?w->parent->name:"",
               w->name,w->focus_index,item?item->name:"<none>");
        return;
    }
    for(int i=0;i<w->nkids;i++)report_choices(w->kids[i]);
}
/* tests: OPENGT4_UI_TREE prints the page's widgets (layout checks) */
static void report_tree(const ui_widget *w,int depth)
{
    if(!w || depth>14)return;
    printf("tree %*s%s %s vis=%d ins=%d geo=%d rect=%.0f,%.0f,%.0f,%.0f%s%s\n",depth*2,"",w->type,w->name?w->name:"-",w->visible,w->insensitive,w->has_geometry,
           w->x,w->y,w->w,w->h,w->text?" text=":"",w->text?w->text:"");
    for(int i=0;i<w->nkids;i++)report_tree(w->kids[i],depth+1);
}

/* Slow disc reads, decoder startup and disposal never run in draw_movie.
   Each worker owns a separate volume/file cursor. Requests coalesce when
   the player moves through several cards before decoding finishes. */
typedef struct {
    gt4_vol *volume;
    SDL_Thread *thread;
    SDL_mutex *lock; SDL_cond *wake;
    sdl_movie *decoder,*pending,*retired,*previous;
    uint64_t switched;
    char name[128],request[128];
    unsigned generation,completed,displayed,finish_notified;
    int stop,interlace,repeat,audio_track,seen,drawn,muted,failed,unavailable;
} movie_view;
static int movie_worker(void *arg)
{
    movie_view *v=arg;unsigned handled=0;
    for(;;) {
        SDL_LockMutex(v->lock);
        while(!v->stop && handled==v->generation && !v->retired)SDL_CondWait(v->wake,v->lock);
        sdl_movie *old=v->retired;v->retired=NULL;
        if(v->stop){SDL_UnlockMutex(v->lock);sdl_movie_close(old);break;}
        unsigned gen=v->generation;char name[128];strcpy(name,v->request);int interlace=v->interlace,repeat=v->repeat,audio_track=v->audio_track;
        SDL_UnlockMutex(v->lock);sdl_movie_close(old);
        if(gen==handled)continue;
        sdl_movie *next=NULL;
        if(*name && v->volume) {
            char path[160];size_t size=0;snprintf(path,sizeof path,"/mpeg/main/%s.pss",name);
            uint8_t *data=gt4_vol_read(v->volume,path,&size);
            if(data){next=sdl_movie_open(data,size,interlace,repeat,audio_track);free(data);}
        }
        if(!next)fprintf(stderr,"movie: cannot load %s; skipping unavailable movie\n",name);
        SDL_LockMutex(v->lock);
        handled=gen;
        if(gen==v->generation){old=v->pending;v->pending=next;v->completed=gen;v->failed=next==NULL;next=NULL;}
        else old=NULL;
        SDL_UnlockMutex(v->lock);sdl_movie_close(old);sdl_movie_close(next);
    }
    return 0;
}
static movie_view *layer_movie;
/* The PS2 plays menu movies on one layer of the render context: a movie
   started by a page (TopRoot's start_game) keeps playing under pages pushed
   over it that have no MovieFace, seen through their translucent parts
   (GameRoot's mode selection). Only while the page playing it is still in
   the stack: CourseRoot's course preview must not stay behind MakerRoot,
   which replaced it (startPage). */
#include "menu_movie_layer.h"
static int draw_movie(void *user,const char *name,float x,float y,float w,float h,float opacity,int interlace,int repeat,int audio_track);
static int draw_layer_movie(void *user,const char *name,float x,float y,float w,float h,float opacity,int interlace,int repeat,int audio_track)
{
    movie_view *v=user;int muted=v->muted;v->muted=1;
    int result=draw_movie(user,name,x,y,w,h,opacity,interlace,repeat,audio_track);
    v->muted=muted;return result;
}
static void movie_layer(menu *m)
{
    /* Pump the requested movie every frame, including decoder promotion and
       retirement. Comparing only its requested name used to stop pumping as
       soon as TopRoot was requested again: the CourseRoot decoder then stayed
       visible forever underneath GameRoot. */
    if(layer_movie)menu_movie_layer_draw(m,draw_layer_movie,layer_movie);
}

static void dialog_background(menu *m)
{
    ui_set_scene_renderer(menu_ui(m),draw_pregrid_face,NULL);
    menu_dialog_background_draw(m,draw_layer_movie,layer_movie);
}
static int draw_movie(void *user,const char *name,float x,float y,float w,float h,float opacity,int interlace,int repeat,int audio_track)
{
    movie_view *v=user;v->seen=1;
    if(!v->thread || strlen(name)>=100 || strchr(name,'/') || strchr(name,'\\')){v->unavailable=1;v->generation=1;return 0;}
    SDL_LockMutex(v->lock);
    if(strcmp(v->name,name) || v->interlace!=interlace || v->repeat!=repeat || v->audio_track!=audio_track) {
        sdl_movie_audio(v->decoder,0);sdl_movie_audio(v->previous,0);
        snprintf(v->name,sizeof v->name,"%s",name);strcpy(v->request,v->name);
        v->interlace=interlace;v->repeat=repeat;v->audio_track=audio_track;v->generation++;SDL_CondSignal(v->wake);
    }
    v->unavailable=v->completed==v->generation && v->failed;
    if(v->completed==v->generation && v->pending && (sdl_movie_ready(v->pending) || sdl_movie_finished(v->pending)) && !v->retired && !v->previous) {
        v->previous=v->decoder;v->decoder=v->pending;v->pending=NULL;v->displayed=v->generation;v->switched=plat_time_ns();
        sdl_movie_audio(v->decoder,!v->muted && audio_track>0);
    }
    SDL_UnlockMutex(v->lock);
    /* Only a visible MovieFace may enable movie audio. */
    sdl_movie_audio(v->decoder,!v->muted && audio_track>0);
    float blend=v->previous?(float)((plat_time_ns()-v->switched)/180000000.0):1;
    if(blend>1)blend=1;
    if(v->previous)sdl_movie_draw(v->previous,x,y,w,h,opacity);
    int drawn=sdl_movie_draw(v->decoder,x,y,w,h,opacity*blend);
    v->drawn=drawn && v->displayed==v->generation && blend>=1;
    if(v->previous && blend>=1){SDL_LockMutex(v->lock);if(!v->retired){sdl_movie_release_texture(v->previous);v->retired=v->previous;v->previous=NULL;SDL_CondSignal(v->wake);}SDL_UnlockMutex(v->lock);}
    return drawn;
}
typedef struct {
    const char *disc;char name[64];double tempo;gt4_music *music;unsigned sequence,count;int played;
    gt4_music **playlist;SDL_atomic_t *track_ready,*track_frames;
    SDL_atomic_t frames;
    SDL_atomic_t ready,volume,current_sequence;SDL_Thread *thread;
} music_job;
static void render_menu_music(int16_t *out,int frames,void *user)
{
    music_job *job=user;
    if(job->playlist) {
        unsigned sequence=(unsigned)SDL_AtomicGet(&job->current_sequence);
        gt4_music *tracks[256]={0};
        unsigned available[256]={0};
        for(unsigned i=0;i<job->count;i++)
            if(SDL_AtomicGet(&job->track_ready[i])){tracks[i]=job->playlist[i];available[i]=(unsigned)SDL_AtomicGet(&job->track_frames[i]);}
        if(!strcmp(job->name,"map"))sequence=gt4_music_render_playlist_available(out,frames,tracks,available,job->count,sequence);
        else gt4_music_render_available(out,frames,tracks[sequence],available[sequence]);
        SDL_AtomicSet(&job->current_sequence,(int)sequence);
    } else gt4_music_render_available(out,frames,job->music,(unsigned)SDL_AtomicGet(&job->frames));
    int volume=SDL_AtomicGet(&job->volume);
    if(volume!=120)for(int i=0;i<frames*2;i++)out[i]=(int16_t)((int)out[i]*volume/120);
}
typedef struct {music_job *job;unsigned sequence;int first;} music_prefix;
static void publish_music_prefix(void *user,unsigned frames)
{
    music_prefix *prefix=user;music_job *j=prefix->job;
    if(j->playlist){
        SDL_AtomicSet(&j->track_frames[prefix->sequence],(int)frames);
        if(frames>=4096){SDL_AtomicSet(&j->track_ready[prefix->sequence],1);if(prefix->first)SDL_AtomicSet(&j->ready,1);}
    }else {SDL_AtomicSet(&j->frames,(int)frames);if(frames>=4096)SDL_AtomicSet(&j->ready,1);}
}
static int prepare_music(void *arg)
{
    music_job *j=arg;const char *err=NULL;gt4_vol *v=gt4_vol_open(j->disc,&err);
    if(v){unsigned count=gt4_music_sequence_count(v,j->name);
        j->count=count;
        j->sequence=count?j->sequence%count:0;
        if(count>1 && count<=256) {
            j->playlist=calloc(count,sizeof *j->playlist);
            j->track_ready=calloc(count,sizeof *j->track_ready);
            j->track_frames=calloc(count,sizeof *j->track_frames);
            if(j->playlist && j->track_ready && j->track_frames) {
                /* Publish the first track immediately; prefetch the others
                   without delaying startup or allocating in the callback. */
                unsigned first=j->sequence;
                SDL_AtomicSet(&j->current_sequence,(int)first);
                for(unsigned offset=0;offset<count;offset++) {
                    unsigned i=(first+offset)%count;
                    gt4_music *track=gt4_music_open_sequence(v,j->name,i);
                    j->playlist[i]=track;if(!offset)j->music=track;
                    if(!track || !gt4_music_prepare_begin(track,j->tempo)) {
                        gt4_music_close(track);
                        j->playlist[i]=NULL;if(!offset)j->music=NULL;
                        fprintf(stderr,"music: cannot prepare %s sequence=%u\n",j->name,i);
                        continue;
                    }
                    j->playlist[i]=track;
                }
                /* Round robin: every possible next menu selection receives
                   its startup buffer before any whole song is rendered. */
                int more;
                do{more=0;for(unsigned offset=0;offset<count;offset++){
                    unsigned i=(first+offset)%count;music_prefix prefix={j,i,!offset};
                    int pending=gt4_music_prepare_step(j->playlist[i],publish_music_prefix,&prefix);more|=pending;
                    if(!pending && j->playlist[i] && SDL_AtomicGet(&j->track_frames[i])){
                        SDL_AtomicSet(&j->track_ready[i],1);if(!offset)SDL_AtomicSet(&j->ready,1);
                    }
                }}while(more);
            } else {
                free(j->playlist);j->playlist=NULL;free(j->track_ready);j->track_ready=NULL;free(j->track_frames);j->track_frames=NULL;
            }
        } else {
            j->music=gt4_music_open_sequence(v,j->name,j->sequence);
            music_prefix prefix={j,0,1};
            if(j->music && !gt4_music_prepare_stream(j->music,j->tempo,publish_music_prefix,&prefix)){gt4_music_close(j->music);j->music=NULL;}
        }
        if(!j->playlist)SDL_AtomicSet(&j->current_sequence,(int)j->sequence);
        gt4_vol_close(v);}
    SDL_AtomicSet(&j->ready,1);return 0;
}

/* The disc requests BGM before the page's authored panIn/Sync. Begin IO
   there, rather than after the synchronous reveal has already completed. */
static music_job *preparing_jobs;
static int *preparing_count;
static const char *preparing_disc;
static double preparing_tempo;
static pregrid_audio *preparing_pcm;
/* startStream is synchronous in the script, but decoding is a PC worker.
   Pump publication inside Sync as well as the outer event loop, otherwise
   get_car waits 4+1+3 seconds before its PCM can begin (L14542..14640). */
static void update_present_pcm(menu *m)
{
    if(!preparing_pcm || !m)return;
    int volume=0;const char *group=menu_pcm_request(m,&volume);
    if(!group)return;
    /* Consume the outgoing BGM fade before publishing the new stream.
       The outer MIDI bookkeeping may still name its old owner. */
    if(*group && music_faded){plat_audio_set_music(NULL,NULL);music_faded=0;}
    int sequence,repeat;unsigned serial;
    menu_pcm_parameters(m,&sequence,&repeat,&serial);
    pregrid_audio_update_gated(preparing_pcm,group,volume,(uint32_t)plat_time_ns(),sequence,repeat,serial,!loading_screen_active());
    pregrid_audio_pause(preparing_pcm,menu_pcm_paused(m));
    menu_pcm_state(m,pregrid_audio_playing(preparing_pcm));
}

static void prepare_menu_audio(menu *m,void *user)
{
    (void)user;
    int volume=0;const char *group=menu_pcm_request(m,&volume);
    if(group && *group){int sequence,repeat;unsigned serial;
        menu_pcm_parameters(m,&sequence,&repeat,&serial);
        pregrid_audio_update_gated(preparing_pcm,group,volume,(uint32_t)plat_time_ns(),sequence,repeat,serial,0);
    }
    const char *name=menu_music_name(m);
    if(!preparing_jobs || !name || !*name)return;
    for(int i=0;i<*preparing_count;i++)if(!strcmp(preparing_jobs[i].name,name))return;
    if(*preparing_count>=32)return;
    music_job *job=&preparing_jobs[(*preparing_count)++];
    job->disc=preparing_disc;job->tempo=preparing_tempo;
    snprintf(job->name,sizeof job->name,"%s",name);SDL_AtomicSet(&job->volume,120);
    job->thread=SDL_CreateThread(prepare_music,"menu music preparation",job);
    if(!job->thread)SDL_AtomicSet(&job->ready,1);
}

int main(int argc, char **argv)
{
    launcher_startup startup={0};
    int prepared=launcher_bootstrap(&argc,&argv,&startup);
    if(prepared<=0)return prepared<0?1:0;
    if(argc==4 && !strcmp(argv[2],"--launcher-gallery"))return launcher_capture_gallery(argv[1],argv[3]);
    const char *err = NULL;
    const char *shot = NULL;
    double shot_delay=0,input_interval=0;
    const char *quick_input=NULL;   /* --quick-input: inputs for the quick menu (tests), 1.5 s after it opens */
    int exit_code = 0;
    double music_tempo=1.0; /* authored sequence timing, with source-rate conversion */
    int music_playing=0,music_with_shot=0;
    int color_preview=0,color_applied=0;
    int preview_courses=0;
    int intro_override=-1,first_run_override=0;
    int persistent_video=1,settings_preview=0;
    for(int i=1;i<argc;i++)if(!strcmp(argv[i],"--shot") || !strcmp(argv[i],"--no-config"))persistent_video=0;
    const char *config_file=NULL;
    for(int i=1;i<argc;i++)if(!strcmp(argv[i],"--config-file") && i+1<argc){config_file=argv[++i];persistent_video=1;}
    video_settings settings;video_settings_init(&settings,persistent_video,config_file);
    char game_options_path[1100]="";
    if(settings.persist && settings.path[0])snprintf(game_options_path,sizeof game_options_path,"%s.game_options",settings.path);
    menu_options_storage(game_options_path[0]?game_options_path:NULL);
    char save_directory[1100]="";const char *player_name=NULL;
    for(int i=1;i<argc;i++){if(!strcmp(argv[i],"--save-dir") && i+1<argc)snprintf(save_directory,sizeof save_directory,"%s",argv[++i]);else if(!strcmp(argv[i],"--player-name") && i+1<argc)player_name=argv[++i];}
    if(!save_directory[0] && persistent_video){char *pref=SDL_GetPrefPath("OpenGT4","OpenGT4");if(pref){snprintf(save_directory,sizeof save_directory,"%ssaves",pref);SDL_free(pref);}}
    gt4_save_init(save_directory[0]?save_directory:NULL);
    app_video_settings=&settings;
    app_car_ratings=gt4_car_ratings_open(argv[1]);
    app_car_factory=gt4_career_vehicle_open(argv[1]);
    gt4_game_set_car_factory(gt4_career_vehicle_create_seeded,app_car_factory);
    gt4_game_set_auto_gears(gt4_career_vehicle_auto_gears,app_car_factory);
    gt4_game_set_stats_reader(gt4_career_vehicle_stats,app_car_factory);
    gt4_game_set_service_reader(gt4_career_vehicle_service_geometry,app_car_factory);
    gt4_game_set_setting_reader(gt4_career_vehicle_setting,app_car_factory);
    int window_w=settings.current.video.width,window_h=settings.current.video.height;
    int window_full=settings.current.video.fullscreen,upscale=settings.current.video.upscale,video_vsync=settings.current.video.vsync;
    int race_preview=0,race_active=0,race_loading=0,race_drive=0,loading_preview=0,race_return=0;
    int pre_race_preview=0,quick_options=0,quick_options_project=-1,race_time_trial=0;
    int logger_track=-1;   /* the Logger's Load Track waiting for labomode's best lap */
    int logger_load_asked=0;   /* its Load Ghost not yet sent: the quick menu must be on screen */
    int split_preview=0;const char *split_car_option=NULL;
    const char *race_course_option=NULL,*race_car_option=NULL;
    char race_car[96]={0},race_course[96]={0};
    const char *input_sequence = NULL;
    int selection_steps = 0, positional = 1;
    for (int i = 1; i < argc; i++) {
        if(!strcmp(argv[i],"--loading-shot") && i+1<argc){loading_shot=argv[++i];continue;}
        if(!strcmp(argv[i],"--loading-logo") && i+1<argc){int logo=atoi(argv[++i]);if(logo<0 || logo>1)return 2;loading_screen_preview_variant(logo);continue;}
        if(!strcmp(argv[i],"--loading-preview")){loading_preview=1;continue;}
        if(!strcmp(argv[i],"--save-dir") || !strcmp(argv[i],"--player-name")){if(++i==argc)return 2;continue;}
        if(!strcmp(argv[i],"--config-file")){if(++i==argc || strlen(argv[i])>=sizeof settings.path)return 2;continue;}
        if(!strcmp(argv[i],"--no-config"))continue;
        if(!strcmp(argv[i],"--video-settings")){settings_preview=1;continue;}
        if(!strcmp(argv[i],"--ui-filter") && i+1<argc){
            const char *mode=argv[++i];
            if(!strcmp(mode,"smooth"))settings.current.filter=0;
            else if(!strcmp(mode,"sharp"))settings.current.filter=1;
            else if(!strcmp(mode,"nearest"))settings.current.filter=2;
            else return 2;
            continue;
        }
        if(!strcmp(argv[i],"--text-sharpness") && i+1<argc){
            char *end;long n=strtol(argv[++i],&end,10);if(!*argv[i] || *end || n<0 || n>100)return 2;
            settings.current.text=(int)n;continue;
        }
        if(!strcmp(argv[i],"--vsync") && i+1<argc){
            const char *mode=argv[++i];if(!strcmp(mode,"on"))video_vsync=1;else if(!strcmp(mode,"off"))video_vsync=0;else return 2;continue;
        }
        if(!strcmp(argv[i],"--first-run")){first_run_override=1;continue;}
        if(!strcmp(argv[i],"--intro")){intro_override=1;continue;}
        if(!strcmp(argv[i],"--skip-intro")){intro_override=0;continue;}
        if(!strcmp(argv[i],"--race-preview")){race_preview=1;continue;}
        if(!strcmp(argv[i],"--split-screen")){split_preview=race_preview=race_drive=1;pre_race_preview=0;continue;}
        if(!strcmp(argv[i],"--race-car-2p")){if(++i==argc || !*argv[i] || strlen(argv[i])>80)return 2;split_car_option=argv[i];continue;}
        if(!strcmp(argv[i],"--drive")){race_preview=race_drive=1;continue;}   /* the race itself, not the grid preview */
        if(!strcmp(argv[i],"--time-trial")){race_time_trial=race_preview=race_drive=1;pre_race_preview=!getenv("OPENGT4_RACE_SNAPSHOT");continue;}   /* a capture: straight to its moment */
        if(!strcmp(argv[i],"--pre-race")){pre_race_preview=race_preview=race_drive=1;continue;}
        if(!strcmp(argv[i],"--race-course") || !strcmp(argv[i],"--race-car")) {
            int is_course=!strcmp(argv[i],"--race-course");
            if(++i==argc || !*argv[i] || strlen(argv[i])>80){fprintf(stderr,"race option needs a label (up to 80 characters)\n");return 2;}
            if(is_course)race_course_option=argv[i];else race_car_option=argv[i];
            race_preview=1;continue;
        }
        if(!strcmp(argv[i],"--preview-courses")){preview_courses=1;continue;}
        if(!strcmp(argv[i],"--fullscreen")){window_full=1;continue;}
        if(!strcmp(argv[i],"--upscale") && i+1<argc){
            i++;if(!strcmp(argv[i],"off"))upscale=0;
            else if(!strcmp(argv[i],"ultra-quality"))upscale=1;
            else if(!strcmp(argv[i],"fsr") || !strcmp(argv[i],"quality"))upscale=2;
            else if(!strcmp(argv[i],"balanced"))upscale=3;
            else if(!strcmp(argv[i],"performance"))upscale=4;else return 2;
            continue;
        }
        if(!strcmp(argv[i],"--window") && i+1<argc){
            /* window size in pixels: the GPU draws at this resolution */
            if(sscanf(argv[++i],"%dx%d",&window_w,&window_h)!=2 || window_w<320 || window_h<240 || window_w>7680 || window_h>4320)return 2;
            continue;
        }
        if(!strcmp(argv[i],"--dialog-shot") && i+1<argc){dialog_shot=argv[++i];continue;}
        if(!strcmp(argv[i],"--dialog-shot-scripted") && i+1<argc){dialog_shot=argv[++i];dialog_shot_scripted=1;continue;}   /* taken at the input "shot" */
        if(!strcmp(argv[i],"--with-music")){music_with_shot=1;continue;}
        if (!strcmp(argv[i], "--quick-input") || !strcmp(argv[i], "--input-interval") || !strcmp(argv[i], "--color") || !strcmp(argv[i], "--shot-delay") || !strcmp(argv[i], "--music-tempo") || !strcmp(argv[i], "--shot") || !strcmp(argv[i], "--select") || !strcmp(argv[i], "--input") || !strcmp(argv[i], "--dialog-answer")) {
            const char *option = argv[i];
            if (++i == argc) { fprintf(stderr, "%s needs a value\n", option); return 2; }
            if(!strcmp(option,"--quick-input")){quick_input=argv[i];continue;}
            if(!strcmp(option,"--input-interval")){char *end;input_interval=strtod(argv[i],&end);if(!*argv[i] || *end || !(input_interval>=0 && input_interval<=2))return 2;}
            else if(!strcmp(option,"--color")){char *end;long n=strtol(argv[i],&end,10);if(!*argv[i] || *end || n<0 || n>127)return 2;color_preview=(int)n;}
            else if (!strcmp(option,"--shot-delay")) {
                char *end;shot_delay=strtod(argv[i],&end);
                if(!*argv[i] || *end || !(shot_delay>=0 && shot_delay<=300))return 2;
            }
            else if (!strcmp(option, "--music-tempo")) {
                char *end; music_tempo=strtod(argv[i],&end);
                if(!*argv[i] || *end || !(music_tempo>=0.5 && music_tempo<=1.5)) {
                    fprintf(stderr,"--music-tempo expects 0.5..1.5\n");return 2;
                }
            }
            else if (!strcmp(option, "--shot")) shot = argv[i];
            else if (!strcmp(option, "--input")) input_sequence = argv[i];
            else if (!strcmp(option, "--dialog-answer")) {
                if (strcmp(argv[i], "yes") && strcmp(argv[i], "no")) { fprintf(stderr, "--dialog-answer needs yes or no\n"); return 2; }
                plat_sdl_dialog_answer(!strcmp(argv[i], "yes"));
            }
            else {
                char *end;
                long value = strtol(argv[i], &end, 10);
                if (!*argv[i] || *end || value < 0 || value > 1000) {
                    fprintf(stderr, "--select expects 0..1000 preview steps\n"); return 2;
                }
                selection_steps = (int)value;
            }
        } else argv[positional++] = argv[i];
    }
    argc = positional;
    if (argc < 2) {
        fprintf(stderr, "usage: %s <disc.iso|GT4.VOL> [project] [screen]\n", argv[0]);
        return 2;
    }
    gt4_vol *vol = gt4_vol_open(argv[1], &err);
    if(vol && !gt4_game_load_disc(argv[1]))fprintf(stderr,"career: original calendar, price or oil constants unavailable for this disc\n");
    {
        /* The local HD cache of the menu images (docs/UI_HD.md), built by
           gt4hdui; OPENGT4_UI_HD=0 turns it off, or names another folder. */
        const char *hd=getenv("OPENGT4_UI_HD"),*base=getenv("LOCALAPPDATA");char dir[512]="";
        if(hd && strcmp(hd,"0"))snprintf(dir,sizeof dir,"%s",hd);
        else if(!hd && base)snprintf(dir,sizeof dir,"%s/OpenGT4/ui_hd",base);
        ui_set_hd_cache(dir);
    }
    if (!vol) { fprintf(stderr, "cannot open the game volume: %s\n", err); return 1; }

    int startup_intro=!race_preview && (intro_override==1 ||
        (intro_override<0 && (argc==2 || startup.preferences.file[0]) && !shot && !input_sequence && !selection_steps && !preview_courses));
    int proj = startup_intro ? 2 : 0, root = 0;
    if (argc > 2)
        for (int k = 0; k < NPROJECTS; k++)
            if (!strcmp(argv[2], projects[k])) proj = k;
    int show_opening=intro_override==1 || menu_option_number(NULL,"opening_after_autoload",1)!=0;
    if(startup_intro)proj=show_opening?2:0;
    int first_run=first_run_override || (startup_intro && !gt4_save_active());
    if(first_run){proj=7;startup_intro=0;}
    if(settings_preview){proj=3;startup_intro=0;first_run=0;}

    plat_video_desc desc = { window_w, window_h, window_full, video_vsync, upscale };
    settings.current.video=desc;
    if (plat_video_init(&desc) != 0) {
        fprintf(stderr, "cannot open a window: %s\n", SDL_GetError());
        return 1;
    }
    video_set_aspect(settings.current.aspect);race_hud_set_widescreen(settings.current.aspect);   /* D_006184F0 */
    plat_video_set_view(640, 480);
    video_ui_quality(settings.current.filter,settings.current.text/100.f);
    if(!video_set_upscale(upscale)){settings.current.video.upscale=0;fprintf(stderr,"FSR unavailable; using native resolution.\n");}
    if(!shot){plat_audio_desc audio={48000,2};plat_audio_init(&audio,NULL,NULL);}

    loading_screen_open(vol,argv[1]);
    presentation_disc=argv[1];
    showroom *cars=showroom_open(argv[1]);presentation_cars=cars;
    race_view *race=race_view_open(argv[1]);
    presentation_photo=photo_travel_open(argv[1]);
    /* the race itself (the physics with the player's controls): driving 1
       while its models load, 2 racing */
    race_game *game=race_game_open(argv[1]);int driving=0;
    int photo_return_project=-1;
    presentation_race=game;
    /* driving: 1 the race loading (the quick menu shown), 3 loaded, 2
       racing; its settings kept to race again from the quick menu */
    int race_start_pending=0,race_color=0,race_others_n=0,race_other_colors[5]={0};
    int theater=0;   /* 1 user replay, 2 title attract tape */
    double attract_seconds=0;int attract_cancel=0;
    char race_others[5][32];const char *race_other_ptr[5];
    for(int i=0;i<5;i++)race_other_ptr[i]=race_others[i];
    movie_view movie={0};movie.muted=shot!=NULL;movie.volume=gt4_vol_open_movies(argv[1],&err);
    layer_movie=&movie;presentation_movie_layer=movie_layer;
    movie.lock=SDL_CreateMutex();movie.wake=SDL_CreateCond();
    if(movie.lock && movie.wake)movie.thread=SDL_CreateThread(movie_worker,"movie loader",&movie);
    /* The legal notice and "SCEA Presents" come before the boot project. */
    gt4_boot_notices notices={0};plat_texture *notice_tex[GT4_NOTICE_COUNT]={0};
    int notice=GT4_NOTICE_COUNT;uint64_t notice_start=0;
    if(startup_intro) {
        gt4_boot_notices_open(&notices,argv[1],vol);
        for(int i=0;i<GT4_NOTICE_COUNT;i++) {
            const tex1_image *im=gt4_boot_notice_image(&notices,i);
            if(im)notice_tex[i]=plat_texture_create(im->width,im->height,im->rgba);
            else fprintf(stderr,"boot notice %d unavailable\n",i);
        }
        notice=0;
    }
    /* Tracks are requested from the authored RootWindow.bgm, not project IDs.
       Each preparation worker owns its disc reader. A cached track keeps its
       playback position across keep/popups and loads without blocking input. */
    music_job music_load[32]={0};int music_count=0;
    /* RootWindow.bgm is requested by menu_enter before its reveal
       (0022F6A0 page entry; original mSound playback 004616E8).
       A: the PC backend pre-renders complete sequences. Prefetch MAP,
       the GT Mode authored group, while boot/project loading is running
       rather than beginning that expensive work after the loading overlay. */
    music_job *map_prefetch=&music_load[music_count++];
    map_prefetch->disc=argv[1];map_prefetch->tempo=music_tempo;
    snprintf(map_prefetch->name,sizeof map_prefetch->name,"map");
    SDL_AtomicSet(&map_prefetch->volume,120);
    map_prefetch->thread=SDL_CreateThread(prepare_music,"menu music preparation",map_prefetch);
    if(!map_prefetch->thread)SDL_AtomicSet(&map_prefetch->ready,1);
    music_job *playing_music=NULL;char desired_music[64]="";
    pregrid_audio *pregrid_music=pregrid_audio_create(argv[1]);
    preparing_jobs=music_load;preparing_count=&music_count;preparing_disc=argv[1];
    preparing_tempo=music_tempo;preparing_pcm=pregrid_music;
    char dialog_error[256]="";dialog_ui=ui_project_load(vol,"cursor",dialog_error,sizeof dialog_error);
    if(!dialog_ui)fprintf(stderr,"dialog: %s\n",dialog_error);
    else ui_prepare_dialogs(dialog_ui,NULL,NULL);
    menu *cache[NPROJECTS] = {0};
    menu *mn = cache[proj] = load(vol, proj);
    ui_project *p = mn ? menu_ui(mn) : NULL;
    if(p && startup_intro && show_opening)
        for(int i=0;i<ui_root_count(p);i++)
            if(!strcmp(ui_root_name(p,i),"MovieRoot")){root=i;break;}
    if(proj==7){root=save_screen.root;if(first_run)save_ui_first_run(&save_screen);}
    if (p && argc > 3 && !startup_intro)
        for (int i = 0; i < ui_root_count(p); i++)
            if (!strcmp(ui_root_name(p, i), argv[3])) root = i;
    if(p && settings_preview)
        for(int i=0;i<ui_root_count(p);i++)if(!strcmp(ui_root_name(p,i),"ScreenRoot"))root=i;

    if (mn) {
        if(proj==4 && !menu_setting_begin(mn)){fprintf(stderr,"Setting: no valid garage car\n");exit_code=1;goto done;}
        if(photo_project(proj)) {
            char course[32],car[96];int color=0;
            const char *test_course=getenv("OPENGT4_PHOTO_COURSE"),*test_car=getenv("OPENGT4_PHOTO_CAR");
            if(test_course && test_car)menu_photo_set_selection(mn,test_course,test_car,0);
            if(!menu_photo_request(mn,course,car,&color) ||
               !photo_travel_load(presentation_photo,mn,course,car,color,pump_loading_work,NULL)) {
                fprintf(stderr,"Photo Travel: no valid destination/garage car\n");exit_code=1;goto done;
            }
        }
        menu_preview_courses(mn,preview_courses);
        if(preview_courses)printf("Catalog preview: disc catalog, save unlocks not loaded.\n");
        if(getenv("OPENGT4_TEST_CAR"))menu_test_car(mn,getenv("OPENGT4_TEST_CAR"));   /* tests: a new car ridden */
        if(getenv("OPENGT4_MENU_PUSH"))menu_test_push(mn,getenv("OPENGT4_MENU_PUSH"));   /* tests: a page entered as from a dealer */
        if(!(proj==12 && getenv("OPENGT4_LABO_ARG")))menu_enter(mn, root);
        if(getenv("OPENGT4_TEST_MONEY")){menu_test_money(mn,strtoll(getenv("OPENGT4_TEST_MONEY"),NULL,10));menu_save_capture(mn);}   /* tests: credits */
        if(proj==12 && getenv("OPENGT4_LABO_SAVE")){   /* tests: a saved replay offered to the Save Replay screen */
            static gt4_replay_file offered;
            if(gt4_replay_load(getenv("OPENGT4_LABO_SAVE"),&offered))menu_set_replay_to_save(&offered);
        }
        if(proj==12 && getenv("OPENGT4_LABO_ARG"))menu_open_labo(mn,getenv("OPENGT4_LABO_ARG"));   /* tests: labomode at a page or a card mode */
        if(proj==1 && getenv("OPENGT4_MISSION"))menu_request_mission(mn,getenv("OPENGT4_MISSION"));   /* tests: a mission, its licence check passed */
        menu_tick(mn);
        if(proj==7 && player_name)save_ui_name(&save_screen,player_name);
        while (selection_steps-- > 0) menu_send_input(mn, MENU_DOWN);
        if (input_sequence) {
            char *seq = strdup(input_sequence);
            scripted_keys=seq;scripted_interval=(float)input_interval;
            char *key;
            while ((key=next_scripted_key())!=NULL) {
                menu_input in;
                if(!strcmp(key,"profiles")){present_profile_picker(mn);continue;}
                if (!strcmp(key, "wait")) {
                    /* the menu keeps running while the test waits, as it does
                       between a player's presses (its watchers, e.g.
                       labomode.ad onCheck filling the replay list) */
                    ui_set_movie_renderer(menu_ui(mn),draw_movie,&movie);
                    uint64_t until=plat_time_ns()+(uint64_t)((input_interval>0?input_interval:1.0)*1e9);
                    do{present_wait(mn,1.0f/60,NULL);menu_tick(mn);}while(plat_time_ns()<until && !plat_should_quit());
                    continue;
                }
                /* tests: "l1+", "r1+" or "l1r1+" before an input hold those buttons through it */
                unsigned held=0;const char *named=key;
                if(!strncmp(named,"l1r1+",5)){held=MENU_PAD_L1|MENU_PAD_R1;named+=5;}
                else if(!strncmp(named,"l1+",3)){held=MENU_PAD_L1;named+=3;}
                else if(!strncmp(named,"r1+",3)){held=MENU_PAD_R1;named+=3;}
                if (!input_name(named, &in)) { fprintf(stderr, "unknown input '%s'\n", key); free(seq); scripted_keys=NULL; exit_code = 2; goto done; }
                menu_set_pad_held(mn, held);
                menu_send_input(mn, in);
                {   /* a tap: the key's release too (photo_shoot.ad lets a grab go on it) */
                    static const int released[]={65362,65364,65361,65363,65293,65307,65421,65307,65379};
                    menu_send_key(mn, released[in], 1);
                }
                menu_set_pad_held(mn, 0);
                menu_tick(mn);
                switch_requested(vol, cache, &mn, &proj);
                printf("input %s -> %s/%s focus=%s\n", key, projects[proj],
                       ui_root_name(menu_ui(mn), menu_active_root(mn)), menu_focus_name(mn));
                if(input_interval>0){ui_set_movie_renderer(menu_ui(mn),draw_movie,&movie);present_wait(mn,(float)input_interval,NULL);}
            }
            scripted_keys=NULL;scripted_interval=0;
            free(seq);
        }
        p = menu_ui(mn); root = menu_active_root(mn);
        if (shot) for (int i=0;i<60;i++) menu_animate(mn,1.0f/60);
    }

    int last_root = -1, last_proj = -1, was_car_page = 0;
    int movie_page_root=-1,movie_page_project=-1;
    if(race_preview){
        const char *car=race_car_option?race_car_option:menu_selected_car(mn);
        const char *course=race_course_option?race_course_option:menu_selected_course(mn);
        snprintf(race_car,sizeof race_car,"%s",car && *car?car:"ka_01");
        snprintf(race_course,sizeof race_course,"%s",course && *course?course:"tsukuba");
        if(race_drive){
            static const char *const others[]={"_1100_standard_type11_49","z_act_70","today_g_85","midget2_d_type_98","s500_63"};
            char msg[256];
            race_game_set_time_trial(game,race_time_trial);
            gt4_race_desc split={0};split.tire_2p=1;
            race_game_set_split(game,split_preview?&split:NULL);
            const char *second[1]={split_car_option?split_car_option:race_car};
            if(getenv("OPENGT4_REPLAY_PLAY")){   /* tests: a saved replay watched at once */
                gt4_replay_file r;
                if(!game || !race_game_read_replay(game,getenv("OPENGT4_REPLAY_PLAY"),&r,msg,sizeof msg)){fprintf(stderr,"replay: cannot read %s\n",getenv("OPENGT4_REPLAY_PLAY"));exit_code=1;goto done;}
                race_game_replay_import(game,&r);gt4_replay_free(&r);
                if(!race_game_replay(game,msg,sizeof msg)){fprintf(stderr,"replay: %s\n",msg);exit_code=1;goto done;}
            }else
            if(!game || !race_game_start(game,race_course,race_car,color_preview,split_preview?second:others,NULL,split_preview?1:race_time_trial?0:5,msg,sizeof msg)){fprintf(stderr,"race: %s\n",msg);exit_code=1;goto done;}
            if(split_preview)pre_race_preview=0;
            driving=1;race_start_pending=!pre_race_preview;
            race_color=color_preview;race_others_n=race_time_trial?0:5;
            for(int i=0;i<5;i++)snprintf(race_others[i],sizeof race_others[i],"%s",others[i]);
            if(pre_race_preview){char quick[64];snprintf(quick,sizeof quick,"%s",race_quick_route(race_time_trial));
                quick_race_set(game,race_course,race_car,race_color,race_other_ptr,race_other_colors,race_others_n,0);
                if(getenv("OPENGT4_REPLAY_IMPORT")){   /* tests: the quick menu after a race, its replay a saved one */
                    gt4_replay_file r;
                    if(gt4_replay_load(getenv("OPENGT4_REPLAY_IMPORT"),&r)){race_game_replay_import(game,&r);gt4_replay_free(&r);}
                }
                switch_to(vol,cache,&mn,&proj,quick);p=menu_ui(mn);root=menu_active_root(mn);
                if(getenv("OPENGT4_REPLAY_IMPORT")){menu_set_quick_completed(mn,1);menu_enter(mn,root);menu_tick(mn);}}
        }else if(!race_view_request(race,race_course,race_car,color_preview)){exit_code=1;goto done;}
        else race_loading=1;
    }
    if(loading_preview || driving==1 || race_loading)loading_screen_begin();
    uint64_t shot_wait=0,shot_started=0;
    presentation_clock = plat_time_ns();
    int returning_quick_options=0;
    while (!plat_should_quit()) {
        plat_key_event ev;
        if(driving==2 && race_game_take_retry(game)){
            char error[256];
            if(!race_game_restart(game,error,sizeof error)){fprintf(stderr,"race retry: %s\n",error);exit_code=1;goto done;}
            career_license.result_applied=0;career_race.result_applied=0;
            loading_screen_begin();driving=1;race_start_pending=1;
        }
        /* out of the race when the pause menu chose EXIT: back to the
           quick menu (its grid, race again or leave) */
        if(driving==2 && race_game_demonstrating(game) && (race_game_quit(game) || race_game_over(game))){
            /* 0011C518 / 0011D020: teacher playback never enters the
               attempt-result/award path. Keep the visit's best and icons. */
            char msg[256];loading_screen_begin();
            if(!race_game_end_demo(game,msg,sizeof msg)){fprintf(stderr,"demonstration return: %s\n",msg);exit_code=1;goto done;}
            driving=1;race_start_pending=0;last_root=-1;
            fprintf(stderr,"demonstration: returned to examination menu\n");
            menu_enter_quick_start(mn);menu_tick(mn);presentation_clock=plat_time_ns();
        }
        if(theater && driving==2 && (race_game_quit(game) || race_game_over(game) || (theater==2 && attract_seconds>=90))){
            /* the saved replay over or left: back to the Replay Theater (a
               playlist goes on to its next one unless the replay was left) */
            if(theater==1 && race_game_quit(game))menu_stop_replay_queue();
            if(theater==2)menu_attract_finish(mn,attract_cancel || race_game_quit(game));
            loading_screen_begin();present_wait(mn,menu_loading_duration(mn),NULL);
            race_game_stop(game);driving=0;theater=0;last_root=-1;
            if(mn){menu_enter(mn,menu_active_root(mn)<0?0:menu_active_root(mn));menu_tick(mn);ui_fade(menu_ui(mn),1,0,.2f);}
            loading_screen_end();plat_video_set_view(640,480);presentation_clock=plat_time_ns();
        }
        if(game && race_quick_project(proj) && logger_track>=0 && !logger_load_asked && !menu_pending_command(mn) && ui_fade_remaining(menu_ui(mn))<=0 && menu_active_root(mn)>=0 && !loading_screen_active()){
            /* 0010FEE8: replace only the selected bank from the loaded lap;
               cancel retains both banks and returns to the Logger. */
            char name[64],msg[160];int track=logger_track;logger_track=-1;
            if(menu_take_loaded_ghost(name,sizeof name)){
                gt4_replay_file r;int ok=gt4_replay_load(name,&r);
                if(ok){
                    loading_screen_begin();ok=race_game_logger_load(game,&r,track,msg,sizeof msg);
                    if(ok){menu_logger_source src;if(race_game_logger_source(game,&src))menu_set_logger_source(&src);}
                    loading_screen_end();
                }
                gt4_replay_free(&r);
                if(!ok)present_dialog(mn,"Logger","Cannot load this recording. Choose a best lap from the same course.",0,NULL,NULL);
            }
            char logger[]="logger:LoggerRoot";switch_to(vol,cache,&mn,&proj,logger);
            p=menu_ui(mn);root=menu_active_root(mn);
        }
        if(game && race_quick_project(proj)){   /* a best lap the memory card's screen loaded: the Time Trial's ghost */
            char name[64];
            if(menu_take_loaded_ghost(name,sizeof name)){
                gt4_replay_file r;int ok=gt4_replay_load(name,&r);
                if(ok)ok=race_game_ghost_import(game,&r);
                if(ok){menu_set_quick_best(r.best_lap<session_best?(session_best=r.best_lap):session_best);menu_set_quick_ghost(1);fprintf(stderr,"ghost: %s loaded (%zu steps)\n",name,r.steps);}
                gt4_replay_free(&r);
                if(!ok)present_dialog(mn,"Load Ghost","This ghost was driven on another course or with another car.",0,NULL,NULL);
            }
        }
        if(!driving && game && (proj!=12 || menu_in_replay_theater(mn))){   /* a replay the memory card's screen or the Replay Theater loaded */
            char name[64];int attract=menu_take_attract_replay(name,sizeof name);
            if(attract || menu_take_loaded_replay(name,sizeof name)){
                gt4_replay_file r;char msg[256];
                if(race_game_read_replay(game,name,&r,msg,sizeof msg) && race_game_replay_import(game,&r)){
                    snprintf(race_course,sizeof race_course,"%s",r.course);snprintf(race_car,sizeof race_car,"%s",r.car);
                    {const char *rm=menu_option_text(mn,"replay_mode","NORMAL");
                     race_game_set_replay_options(game,menu_option_number(mn,"replay_display_enable",0)!=0,rm && !strcmp(rm,"DIVE"));}
                    if(race_game_replay(game,msg,sizeof msg)){ui_fade(menu_ui(mn),0,0,0);loading_screen_begin();driving=1;race_start_pending=1;theater=attract?2:1;attract_seconds=0;attract_cancel=0;fprintf(stderr,"%s: %s loaded\n",attract?"attract replay":"theater replay",name);}
                    else {fprintf(stderr,"replay: %s\n",msg);if(attract)menu_attract_finish(mn,1);else present_dialog(mn,"Replay Theater","This replay could not be played.",0,NULL,NULL);}
                }else {if(attract)menu_attract_finish(mn,1);else present_dialog(mn,"Replay Theater","This replay could not be read.",0,NULL,NULL);}
                gt4_replay_free(&r);
            }
        }
        if(driving==2 && (race_game_quit(game) || (race_game_split(game) && race_game_over(game)))){
            if(!career_race_result(game,cache[1],mn)){fprintf(stderr,"career result could not be recorded\n");exit_code=1;goto done;}
            loading_screen_begin();present_wait(mn,menu_loading_duration(mn),NULL);
            if(race_quick_project(proj)){session_best_update(game,mn,race_course,race_car);menu_set_quick_completed(mn,1);race_game_return_pregrid(game);driving=4;pregrid_clock=0;}
            else {int split=race_game_split(game);
                if(split && !race_game_replaying(game) && !race_game_quit(game) && race_game_can_replay(game)){
                    /* Battle2P uses both command recorders; common result -> replay.
                       A: activation ordering still requires an original world capture. */
                    char msg[256];
                    if(race_game_replay(game,msg,sizeof msg)){driving=1;race_start_pending=1;}
                    else {fprintf(stderr,"2P replay: %s\n",msg);race_game_stop(game);driving=0;}
                }else{race_game_stop(game);driving=0;}
                if(split && mn && !driving){menu_enter(mn,menu_active_root(mn)<0?0:menu_active_root(mn));menu_tick(mn);ui_fade(menu_ui(mn),1,0,.2f);}}
            last_root=-1;
            if(mn && race_quick_project(proj)){menu_enter_quick_start(mn);menu_tick(mn);ui_fade(menu_ui(mn),1,0,.2f);}
            loading_screen_end();plat_video_set_view(640,480);presentation_clock=plat_time_ns();
        }
        if(mn){   /* the pad's L1 and R1 for the menus: Q and E held (P: a PC key choice) */
            const Uint8 *k=SDL_GetKeyboardState(NULL);
            menu_set_pad_held(mn,plat_sdl_menu_buttons()|(k[SDL_SCANCODE_Q]?MENU_PAD_L1:0)|(k[SDL_SCANCODE_E]?MENU_PAD_R1:0));
        }
        while (plat_sdl_poll(&ev)) {
            if (ev.key == SDLK_F10) goto done;
            if(ev.mouse){
                if(driving==2)race_game_cinematic_mouse(game,ev.mouse_x,ev.mouse_y,ev.wheel);
                continue;
            }
            if(notice<GT4_NOTICE_COUNT)continue;   /* the original screens take no input */
            if(driving==2){
                if(!ev.pad && ev.key==SDLK_F9){race_game_cinematic_toggle(game);continue;}
                if(!ev.pad && race_game_cinematic(game) && ev.key!=SDLK_F5 && ev.key!=SDLK_F3){race_game_cinematic_key(game,ev.key);continue;}
                /* 0010BA28/0010BE20: a pad press leaves the attract cycle. */
                if(theater==2){attract_cancel=1;race_game_key(game,SDLK_ESCAPE);continue;}
                /* P: F5 opens Photo Drive from its live session or a replay. Rendering borrows
                   the resident race; driving=5 prevents any solver frame. */
                if(ev.key==SDLK_F5 && race_game_photo_view(game)){
                    if(race_game_cinematic(game))race_game_cinematic_toggle(game);
                    if(!cache[14])cache[14]=load(vol,14);
                    float camera_size[4],camera_distance;if(race_game_photo_camera_data(game,camera_size,&camera_distance))photo_travel_drive_setup(presentation_photo,camera_size,camera_distance);
                    if(cache[14] && photo_travel_drive(presentation_photo,cache[14],race_game_photo_view(game))){
                        photo_return_project=proj;mn=cache[14];proj=14;p=menu_ui(mn);root=menu_active_root(mn);
                        driving=5;race_game_photo_pause(game,1);plat_video_set_view(640,480);
                    }
                    continue;
                }
                /* P: F6 stores the complete native 2P recording. The same
                   file is selectable in Replay Theater. */
                if(ev.key==SDLK_F6 && race_game_split(game) && race_game_can_replay(game)){
                    gt4_replay_file r;char name[64];
                    if(race_game_replay_export(game,&r,GT4_REPLAY_FULL) && gt4_replay_store(&r,name,sizeof name))fprintf(stderr,"2P replay saved: %s\n",name);
                    continue;
                }
                if(ev.pad)continue; /* race_game owns controller edges */
                if(ev.key==SDLK_F3)race_game_inspect_toggle(game);
                /* Escape, the arrows and Enter go to the pause menu (RacePause) */
                race_game_key(game,ev.key);
                continue;
            }
            if(theater==2 && driving==1){
                menu_attract_finish(mn,1);race_game_stop(game);driving=0;
                race_start_pending=0;theater=0;loading_screen_end();
                plat_video_set_view(640,480);presentation_clock=plat_time_ns();continue;
            }
            if(driving==1 && loading_screen_active()){
                if(ev.key==SDLK_ESCAPE){
                    if(race_game_demonstrating(game)){
                        char msg[256];
                        if(!race_game_end_demo(game,msg,sizeof msg)){fprintf(stderr,"demonstration return: %s\n",msg);exit_code=1;goto done;}
                        driving=1;race_start_pending=0;menu_enter(mn,menu_active_root(mn)<0?0:menu_active_root(mn));menu_tick(mn);
                    }else {race_game_stop(game);driving=0;race_start_pending=0;loading_screen_end();}
                    plat_video_set_view(640,480);presentation_clock=plat_time_ns();
                }
                continue;
            }
            if(race_loading){if(ev.key==SDLK_ESCAPE)race_loading=-1;continue;}
            if(race_active){
                if(ev.key==SDLK_ESCAPE){loading_screen_begin();present_wait(mn,menu_loading_duration(mn),NULL);race_view_clear(race);loading_screen_end();race_active=0;last_root=-1;plat_video_set_view(640,480);presentation_clock=plat_time_ns();}
                continue;
            }
            if(driving==5 && ev.key==SDLK_ESCAPE && !strcmp(ui_root_name(menu_ui(mn),menu_active_root(mn)),"ShootRoot")){
                photo_travel_drive_end(presentation_photo);proj=photo_return_project;photo_return_project=-1;
                mn=cache[proj];p=menu_ui(mn);root=menu_active_root(mn);driving=2;
                race_game_photo_pause(game,0);plat_video_set_view(640,448);continue;
            }
            if(driving==5 && proj==14 && (ev.key==SDLK_F5 || ev.key==SDLK_RETURN)){
                if(!cache[21])cache[21]=load(vol,21);
                if(cache[21] && photo_travel_drive_preview(presentation_photo,cache[21])){proj=21;mn=cache[21];p=menu_ui(mn);root=menu_active_root(mn);}continue;
            }
            if(driving==5 && proj==21 && ev.key==SDLK_ESCAPE){
                photo_travel_drive_editor(presentation_photo);proj=14;mn=cache[14];p=menu_ui(mn);root=menu_active_root(mn);continue;
            }
            /* The PhotoPause editor polls its own controls. Hidden PhotoTravel
               widgets must not receive directional or tool events. */
            if(driving==5 && proj==14)continue;
            if(proj==7 && save_ui_event(&save_screen,ev.key,ev.text))continue;
            if(photo_project(proj) && ev.key==SDLK_F5){photo_travel_shoot(presentation_photo);continue;}
            if (mn) {
                int car_page=!strcmp(ui_root_name(menu_ui(mn),menu_active_root(mn)),"CarRoot");
                if(car_page && ev.key==SDLK_F5){
                    const char *car=menu_selected_car(mn),*course=menu_selected_course(mn);
                    if(car && *car && course && *course){
                        snprintf(race_car,sizeof race_car,"%s",car);snprintf(race_course,sizeof race_course,"%s",course);
                        if(race_view_request(race,race_course,race_car,showroom_color_index(cars))){loading_screen_begin();race_loading=1;}
                    }else present_dialog(mn,"Info","Escolha uma pista e um carro primeiro.",0,NULL,NULL);
                    continue;
                }
                if (ev.key == SDLK_UP) menu_send_input(mn, MENU_UP);
                if (ev.key == SDLK_DOWN) menu_send_input(mn, MENU_DOWN);
                if (ev.key == SDLK_LEFT) menu_send_input(mn, MENU_LEFT);
                if (ev.key == SDLK_RIGHT) menu_send_input(mn, MENU_RIGHT);
                if (ev.key == SDLK_RETURN || ev.key == SDLK_KP_ENTER) menu_send_input(mn, MENU_ACCEPT);
                if (ev.key == SDLK_ESCAPE) menu_send_input(mn, ev.pad?MENU_CIRCLE:MENU_BACK);
                if (ev.pad && ev.key == SDLK_INSERT) menu_send_input(mn, MENU_SQUARE);
                /* P: F4 maps the original pad Triangle event for Home/Setting. */
                if (ev.key == SDLK_F4) menu_send_input(mn, MENU_TRIANGLE);
                /* L1, L2, R1, R2, Square, Select: the scripts' own keys */
                if (ev.ps2key) menu_send_key(mn, ev.ps2key, ev.release);
            }
            if (p && (ev.key == SDLK_F1 || ev.key == SDLK_F2)) {
                root = (root + (ev.key == SDLK_F2 ? 1 : ui_root_count(p) - 1)) % ui_root_count(p);
                menu_enter(mn, root);
            }
            if (ev.key == SDLK_PAGEDOWN || ev.key == SDLK_PAGEUP) {
                proj = (proj + (ev.key == SDLK_PAGEDOWN ? 1 : NPROJECTS - 1)) % NPROJECTS;
                if (!cache[proj]) cache[proj] = load(vol, proj);
                mn = cache[proj];
                p = mn ? menu_ui(mn) : NULL;
                root = 0;
                if (mn) menu_enter(mn, root);
            }
        }
        if(mn && menu_take_quit(mn))goto done;
        /* The driving options' OK (doGame -> finish, single race / time
           trial): the race on the chosen course with the chosen car and
           paint, against the AI. */
        if(mn && !race_loading && !race_active && !driving && menu_take_race_request(mn)) {
            if(proj==1 && menu_race_is_mission(mn)){
                /* 0011BB60: the mission from RACE/RACES (0011BEC0), RaceMission,
                   then quick-mission (0011BF60) */
                gt4_career_license_exam mission;char code[32],msg[256]="Cannot read mission request.";
                int ok=menu_get_mission_request(mn,code) && gt4_career_mission_prepare(&mission,argv[1],gt4_game_race_lookup_data(),code,msg,sizeof msg);
                if(ok){race_time_trial=0;free_mode=0;race_game_set_time_trial(game,0);mission.desc.manual=menu_driving_manual(mn);ok=race_game_start_prepared(game,&mission.desc,msg,sizeof msg);}
                if(ok){menu_quick_race q;ok=race_game_quick_grid(game,&q);
                    if(ok){
                        career_license.active=0;career_race.active=1;career_race.mission=1;career_race.championship=0;
                        career_race.result_applied=0;career_race.has_distances=0;career_race.retired=0;career_race.round=0;career_race.rounds=0;
                        career_race.best=15;career_race.award=0;memcpy(career_race.row,mission.row,sizeof career_race.row);
                        snprintf(career_race.race,sizeof career_race.race,"%s",code);snprintf(q.race,sizeof q.race,"%s",code);
                        snprintf(race_course,sizeof race_course,"%s",q.course);snprintf(race_car,sizeof race_car,"%s",q.grid[q.player].car);
                        race_color=q.grid[q.player].color;race_others_n=q.entries-1;
                        menu_save_capture(mn);menu_set_quick_race(&q);loading_screen_begin();driving=1;race_start_pending=0;
                        char route[]="quick-mission:MissionRoot";switch_to(vol,cache,&mn,&proj,route);p=menu_ui(mn);root=menu_active_root(mn);
                    }else snprintf(msg,sizeof msg,"Cannot read the mission's grid.");
                }
                if(!ok){fprintf(stderr,"mission: %s\n",msg);present_dialog(mn,"Info",msg,0,NULL,NULL);}
            }else if(proj==1 && menu_race_is_license(mn)){
                gt4_career_license_exam exam;char code[32],msg[256]="Cannot read licence request.";
                int ok=menu_get_license_request(mn,code) && gt4_career_license_prepare(&exam,argv[1],gt4_game_race_lookup_data(),code,msg,sizeof msg);
                if(ok){race_time_trial=0;free_mode=0;race_game_set_time_trial(game,0);exam.desc.manual=menu_driving_manual(mn);ok=race_game_start_prepared(game,&exam.desc,msg,sizeof msg);}
                if(ok){menu_quick_race q;ok=race_game_quick_grid(game,&q) && menu_license_begin(mn,code,&career_license.visit);
                    if(ok){
                        career_race.active=0;career_license.active=1;career_license.result_applied=0;snprintf(career_license.code,sizeof career_license.code,"%s",code);
                        career_license.best_time=menu_license_best(mn,code);snprintf(q.race,sizeof q.race,"%s",code);menu_set_quick_race(&q);menu_set_quick_best(career_license.best_time);
                        snprintf(race_course,sizeof race_course,"%s",q.course);snprintf(race_car,sizeof race_car,"%s",q.grid[0].car);race_color=q.grid[0].color;race_others_n=0;
                        menu_save_capture(mn);loading_screen_begin();driving=1;race_start_pending=0;
                        char route[]="quick-license:LicenseRoot";switch_to(vol,cache,&mn,&proj,route);p=menu_ui(mn);root=menu_active_root(mn);
                    }
                }
                if(!ok){
                    /* PC integration errors after race construction may not
                       supply text; never open an empty information dialog. */
                    if(!*msg)snprintf(msg,sizeof msg,"Cannot read the licence's prepared car or record.");
                    fprintf(stderr,"licence: %s\n",msg);present_dialog(mn,"Info",msg,0,NULL,NULL);
                }
            }else if(proj==1 && !menu_race_is_free(mn)){
                menu_career_request request;gt4_career_event event;char msg[256];
                int ok=menu_get_career_request(mn,&request,msg,sizeof msg);
                char first_round[32];const char *event_label=request.race;unsigned rounds=0,restored_round=0;
                uint8_t suspended[0x90]={0};int resume=0;
                if(ok && request.championship){
                    ok=menu_championship_context_get(mn,suspended);resume=ok && suspended[0x28]!=0;
                    if(resume){memcpy(request.race,suspended+8,32);request.race[31]=0;restored_round=suspended[0x29];}
                    if(ok)ok=gt4_career_championship_label(request.race,restored_round,first_round,&rounds);event_label=first_round;
                    if(!ok)snprintf(msg,sizeof msg,"Invalid original championship label.");}
                if(ok && !career_race.rng_ready){career_race.rng_ready=gt4_career_event_rng_load(argv[1],&career_race.rng);ok=career_race.rng_ready;if(!ok)snprintf(msg,sizeof msg,"Cannot read original event RNG state.");}
                if(ok){if(resume)ok=gt4_career_championship_restore(&event,argv[1],gt4_game_race_lookup_data(),suspended,request.player_body,&career_race.rng,msg,sizeof msg);
                    else ok=gt4_career_event_prepare(&event,argv[1],gt4_game_race_lookup_data(),event_label,request.player_body,request.championship,&career_race.rng,msg,sizeof msg);}
                if(ok){
                    race_time_trial=0;free_mode=0;race_game_set_time_trial(game,0);
                    event.desc.manual=menu_driving_manual(mn);
                    ok=race_game_start_prepared(game,&event.desc,msg,sizeof msg);
                    /* 00114250 +F4 / 00119918 +174: licence availability
                       becomes options+970, read by the pit menu 003C12A8. */
                    if(ok)race_game_set_pit_mode_change(game,menu_career_aspec_available(mn,event_label));
                }
                if(ok){
                    menu_quick_race q;ok=race_game_quick_grid(game,&q);
                    if(ok){
                        career_race.active=1;career_race.mission=0;career_race.result_applied=0;career_race.has_distances=0;
                        career_race.championship=request.championship;career_race.retired=0;career_race.round=restored_round;career_race.rounds=rounds;
                        career_race.grand_prize=0;memset(career_race.points,0,sizeof career_race.points);
                        snprintf(career_race.series,sizeof career_race.series,"%s",request.race);
                        if(request.championship){
                            if(!race_game_championship_grid(game,&career_race.championship_grid,career_race.bodies)){exit_code=1;goto done;}
                            career_race.championship_grid.event_rng=&career_race.rng;
                            q.championship=1;q.session_number=(int)restored_round;q.final_session=restored_round+1==rounds;
                            if(resume)for(unsigned j=0;j<6;j++)memcpy(career_race.points+j,suspended+0x3c+j*16,4);
                            memcpy(q.total_points,career_race.points,sizeof career_race.points);
                        }
                        /* 00113CB4..13CB8: best position belongs to the
                           event sequence, persists over its retries. */
                        career_race.best=15;career_race.award=0;
                        memcpy(career_race.row,event.race_row,sizeof career_race.row);
                        snprintf(career_race.race,sizeof career_race.race,"%s",event_label);
                        snprintf(q.race,sizeof q.race,"%s",event_label);
                        snprintf(race_course,sizeof race_course,"%s",q.course);
                        snprintf(race_car,sizeof race_car,"%s",q.grid[q.player].car);
                        race_color=q.grid[q.player].color;race_others_n=q.entries-1;
                        menu_save_capture(mn);menu_set_quick_race(&q);
                        loading_screen_begin();driving=1;race_start_pending=0;
                        char quick[64];snprintf(quick,sizeof quick,"%s",request.championship?"quick-championship:ChampionshipRoot":"quick-event:RaceEventRoot");switch_to(vol,cache,&mn,&proj,quick);
                        p=menu_ui(mn);root=menu_active_root(mn);
                    }else snprintf(msg,sizeof msg,"Cannot read the prepared grid.");
                }
                if(!ok){fprintf(stderr,"career event: %s\n",msg);present_dialog(mn,"Info",msg,0,NULL,NULL);}
            }else{
            career_race.active=0;career_license.active=0;
            /* GT Mode's Free Run (00117668: world "freerun", quick-freerun)
               and Practice (00118670: quick-practice): the riding car on the
               course chosen, through the Arcade's own build */
            int free=proj==1?menu_race_is_free(mn):0;
            int split=free?0:menu_race_is_split(mn);
            const char *car=menu_split_car(mn,0),*course=menu_selected_course(mn);
            uint8_t split_bodies[2][0x4b8];int split_owned[2]={0},split_colors[2]={0};
            char free_course[32],free_car[96];int free_laps=0;
            if(free){
                if(!menu_free_request(mn,free_course,free_car,&split_colors[0],split_bodies[0],&free_laps)){present_dialog(mn,"Info","Cannot read the riding car.",0,NULL,NULL);continue;}
                split_owned[0]=1;car=free_car;course=free_course;
            }else{
                for(int i=0;i<(split?2:1);i++)split_owned[i]=menu_split_owned(mn,i,split_bodies[i],&split_colors[i]);
                if(split_owned[0]<0 || split_owned[1]<0){present_dialog(mn,"Info","Cannot read the selected garage car.",0,NULL,NULL);continue;}
            }
            /* the opponents the scripts chose (set_enemy_cars:
               MCarData.PickUpEnemyCars, game.setEntryCarCode) */
            const char *others[5];int other_colors[5];
            int nothers=menu_race_entries(mn,others,other_colors,5);
            if(split){others[0]=menu_split_car(mn,1);other_colors[0]=split_colors[1];nothers=1;}
            if(split && (!others[0] || !*others[0])){fprintf(stderr,"split battle: missing second car selection\n");continue;}
            fprintf(stderr,"race: %s on %s against",car?car:"-",course?course:"-");
            for(int i=0;i<nothers;i++)fprintf(stderr," %s/%d",others[i],other_colors[i]);
            fputc('\n',stderr);
            if(car && *car && course && *course && game) {
                char msg[256],quick[64];
                race_time_trial=free?free==1:menu_race_is_time_trial(mn);
                free_mode=free;
                if(race_time_trial || free==3 || free==4)nothers=0; /* quick-tt.ad L103; Photo Drive 0011B1CC: one entry. */
                snprintf(quick,sizeof quick,"%s",race_quick_route(race_time_trial));
                race_game_set_world_mode(game,free==1?11:free==2?12:free==4?15:0);
                if(free==4)snprintf(machine_race,sizeof machine_race,"%s",menu_game_text(mn,"race_code"));   /* 00115A90 -> 003BDD60: Power & Speed, mode 15 */
                int tire=(int)menu_game_number(mn,"tire_idx",0);
                snprintf(race_car,sizeof race_car,"%s",car);snprintf(race_course,sizeof race_course,"%s",course);
                race_color=(split || split_owned[0])?split_colors[0]:showroom_color_index(cars);race_others_n=nothers;
                for(int i=0;i<nothers;i++){snprintf(race_others[i],sizeof race_others[i],"%s",others[i]);race_other_colors[i]=other_colors[i];}
                gt4_race_desc split_options={0};
                if(split){
                    for(int i=0;i<2;i++)if(split_owned[i])split_options.owned_car[i]=split_bodies[i];
                    split_options.tire_2p=(int)menu_game_number(mn,"tire_idx_2p",0)+1;
                    split_options.manual_2p=(int)menu_game_number(mn,"manual_2p",0);
                    split_options.assist_asm_2p=(int)menu_game_number(mn,"assist_asm_2p",0);
                    split_options.assist_tcs_2p=(int)menu_game_number(mn,"assist_tcs_2p",0);
                    split_options.split_boost=(int)menu_option_number(mn,"split_boost_level",0);
                    split_options.split_handicap=(int)menu_option_number(mn,"split_handicap",0);
                    split_options.split_tire_damage=(int)menu_option_number(mn,"split_tire_damage",0);
                }
                race_game_set_split(game,split?&split_options:NULL);
                race_game_set_arcade_owned(game,!split && split_owned[0]?split_bodies[0]:NULL);
                race_game_set_laps(game,free==2 || free==4?free_laps:(int)menu_option_number(mn,split?"split_race_laps":"race_laps",2));
                race_game_set_time_trial(game,race_time_trial);
                /* GranTurismo4Option: unit_velocity KMPH (0) or MPH (1), 0x005A6978 */
                race_game_set_unit(game,strcmp(menu_option_text(mn,"unit_velocity","MPH"),"KMPH")?1:0);
                race_game_set_power_unit(game,menu_option_text(mn,"unit_power","HP"));
                /* the driving options' apply_setting: game.manual, assist_asm, assist_tcs, tire_idx */
                race_game_set_driving(game,tire,proj==1?menu_driving_manual(mn):(int)menu_game_number(mn,"manual",0),
                    (int)menu_game_number(mn,"assist_asm",menu_option_number(mn,"assist_asm",1)),
                    (int)menu_game_number(mn,"assist_tcs",menu_option_number(mn,"assist_tcs",1)));
                race_game_quick_clear(game);   /* 00111B50: a new race, no QuickTune */
                int started;
                if(free==3){
                    /* 0011AC54/0011ACB4: Photo Drive has the GT world/controller
                       (003EDE50), its own RACE row and one equipped garage car.
                       0011B288: its authored quick page is quick-freerun. */
                    gt4_career_event photo={0};uint8_t active[0x178];
                    started=menu_photo_drive_rules(mn,photo.race_row) && gt4_career_active_setup(split_bodies[0],active);
                    if(!career_race.rng_ready)career_race.rng_ready=gt4_career_event_rng_load(argv[1],&career_race.rng);
                    started=started && career_race.rng_ready;
                    if(started){
                        memcpy(photo.player_body,split_bodies[0],sizeof photo.player_body);
                        photo.desc.cars=1;memcpy(&photo.desc.car[0],active,4);
                        snprintf(photo.desc.course,sizeof photo.desc.course,"%s",race_course);
                        photo.desc.owned_car[0]=photo.player_body;photo.desc.event_row=photo.race_row;
                        photo.desc.event_rng=&career_race.rng;
                        photo.desc.manual=menu_driving_manual(mn);
                        photo.desc.assist_asm=(int)menu_game_number(mn,"assist_asm",0);
                        photo.desc.assist_tcs=(int)menu_game_number(mn,"assist_tcs",0);
                        started=race_game_start_prepared(game,&photo.desc,msg,sizeof msg);
                    }else snprintf(msg,sizeof msg,"Cannot read Photo Drive rules or equipped car.");
                }else started=race_game_start(game,race_course,race_car,race_color,race_other_ptr,race_other_colors,nothers,msg,sizeof msg);
                if(started) {
                    /* the race sequence: its quick menu first (the grid), the race on its Start */
                    loading_screen_begin();
                    driving=1;race_start_pending=split;
                    if(!split){
                    quick_race_set(game,race_course,race_car,race_color,race_other_ptr,race_other_colors,nothers,tire);
                    switch_to(vol,cache,&mn,&proj,quick);
                    p=menu_ui(mn);root=menu_active_root(mn);
                    }
                }
                else {fprintf(stderr,"race: %s\n",msg);present_dialog(mn,"Info","Nao foi possivel montar esta corrida. Veja o log.",0,NULL,NULL);}
            }
            }
        }
        /* --quick-input: one input a second, including its Options round trip. */
        {   /* tests: OPENGT4_LOGGER_INPUT=<keys> pressed in the Logger, one a second and a half */
            static const char *keys;static uint64_t since,last;
            if(!keys)keys=getenv("OPENGT4_LOGGER_INPUT");
            if(keys && *keys && mn && (proj==16 || (proj==12 && logger_track>=0))){
                uint64_t t=plat_time_ns();if(!since)since=t;
                if(t-since>3000000000ull && t-last>1500000000ull){
                    char key[16];size_t n=strcspn(keys,",");menu_input in;
                    snprintf(key,sizeof key,"%.*s",(int)(n<15?n:15),keys);
                    keys+=n;if(*keys==',')keys++;
                    if(input_name(key,&in)){menu_send_input(mn,in);menu_tick(mn);printf("logger input %s -> %s\n",key,menu_focus_name(mn));}
                    last=t;
                }
            }
        }
        {
            static uint64_t quick_since,quick_last;
            if(quick_input && *quick_input && mn && (race_quick_project(proj) || quick_options || proj==12) && driving!=2) {
                uint64_t t=plat_time_ns();
                if(!quick_since)quick_since=t;
                if(t-quick_since>1500000000ull && t-quick_last>1000000000ull) {
                    char key[16];size_t n=strcspn(quick_input,",");menu_input in;
                    snprintf(key,sizeof key,"%.*s",(int)(n<15?n:15),quick_input);
                    quick_input+=n;if(*quick_input==',')quick_input++;
                    if(input_name(key,&in)){menu_send_input(mn,in);menu_tick(mn);printf("quick input %s -> %s\n",key,menu_focus_name(mn));}
                    quick_last=t;
                }
            }
        }
        if(logger_load_asked && mn && race_quick_project(proj) && ui_fade_remaining(menu_ui(mn))<=0 && menu_active_root(mn)>=0)
            {logger_load_asked=0;menu_test_quick_command(mn,"COMMAND_LOAD_GHOST");}   /* the quick menu's own Load Ghost (0010FEE8: labomode's best laps) */
        {   /* tests: OPENGT4_ACTIVATE=<widget>[,<widget>...] chosen in turn, 4 s apart once a page is up */
            static uint64_t since;static const char *next;
            if(!next)next=getenv("OPENGT4_ACTIVATE");
            if(next && *next && mn && menu_active_root(mn)>=0 && !loading_screen_active()){
                uint64_t t=plat_time_ns();if(!since)since=t;
                if(t-since>4000000000ull){
                    char name[64];size_t n=strcspn(next,",");snprintf(name,sizeof name,"%.*s",(int)(n<63?n:63),next);
                    next+=n;if(*next==',')next++;since=t;
                    fprintf(stderr,"activate %s: %s\n",name,menu_test_activate(mn,name)?"ok":"not found");
                }
            }
        }
        {   /* tests: a quick menu command once the race is over and its menu back */
            static int sent;
            if(!sent && mn && race_quick_project(proj) && driving==4 && getenv("OPENGT4_QUICK_COMMAND") && ui_fade_remaining(menu_ui(mn))<=0)
                {sent=1;menu_test_quick_command(mn,getenv("OPENGT4_QUICK_COMMAND"));}
        }
        /* The quick menu's command: Start races (the race loaded, or again) */
        {
            char command[32];
            if(mn && menu_take_quick_command(mn,command,sizeof command)) {
                fprintf(stderr,"quick menu: %s\n",command);
                int spectator=!strcmp(command,"COMMAND_SPECTATOR") && career_race.active && !career_race.mission && !career_license.active;
                if(!strcmp(command,"COMMAND_START") || spectator) {
                    /* quick-event.ad L363: Spectator is COMMAND_SPECTATOR (14 of
                       D_006187B0), the race B-Spec; Start is A-Spec. The race
                       built for the menu is rebuilt when that changes. */
                    int rebuild=game && race_game_set_bspec(game,spectator);
                    if(career_race.active && !career_race.mission && career_race.result_applied && !career_race.championship){
                        if(!menu_commit_career_distances(cache[1],mn,career_race.a_distance,career_race.b_distance,&career_race.has_distances)){exit_code=1;goto done;}
                        /* 00113DC8/00113EC8 save the live wear before another
                           attempt; the native restart constructs a new race
                           and must receive this updated owned body. */
                        uint8_t body[0x4b8];
                        if(!menu_riding_car_body(cache[1],body) || !race_game_setting(game,body)){exit_code=1;goto done;}
                    }
                    if(career_license.active)career_license.result_applied=0;
                    if(career_race.active){if(career_race.championship && career_race.result_applied)continue;career_race.result_applied=0;career_race.award=0;menu_quick_event_result(mn,0,0);}
                    /* 00111498 -> 00111DC0: the Arcade's QuickTune into the
                       player's entry; the race built for the grid is rebuilt
                       to take it (the PS2 writes the live entry before its
                       cars start) */
                    /* 003D6350 reads the current option on GT Start;
                       Options may have changed it after the grid loaded. */
                    if((career_race.active || career_license.active || free_mode) &&
                       race_game_set_transmission(game,menu_driving_manual(mn)))rebuild=1;
                    menu_quick_tune_values tune;
                    if(!career_race.active && !career_license.active && game && race_car[0] && menu_quick_tune(mn,&tune) &&
                       race_game_quick_tune(game,tune.tuned,tune.power,tune.weight,tune.front_tire,tune.rear_tire,tune.gear,tune.transmission,tune.assist) &&
                       (driving==3 || driving==1)){
                        char msg[256];
                        fprintf(stderr,"quick tune: tuned %d power %+d%% weight %+d%% tyres %d/%d gear %d manual %d aids %d\n",
                            tune.tuned,tune.power,tune.weight,tune.front_tire,tune.rear_tire,tune.gear,tune.transmission,tune.assist);
                        if(race_game_restart(game,msg,sizeof msg)){loading_screen_begin();driving=1;race_start_pending=1;}
                        else fprintf(stderr,"race: %s\n",msg);
                        continue;
                    }
                    if(driving==3 && race_game_license_active(game)){
                        char msg[256];
                        if(race_game_restart(game,msg,sizeof msg)){loading_screen_begin();driving=1;race_start_pending=1;}
                        else fprintf(stderr,"race: %s\n",msg);
                    }
                    else if(driving==3 && !rebuild){driving=2;plat_video_set_view(640,448);presentation_clock=plat_time_ns();}
                    else if(driving==1 && !rebuild)race_start_pending=1;
                    else if((!driving || driving==4 || rebuild) && game && race_car[0]) {
                        char msg[256];
                        if(race_game_restart(game,msg,sizeof msg)){loading_screen_begin();driving=1;race_start_pending=1;}
                        else fprintf(stderr,"race: %s\n",msg);
                    }
                }
                else if(!strcmp(command,"COMMAND_NEXT_RACE") && career_license.active){
                    char next[32],msg[256];gt4_career_license_exam exam;
                    if(!gt4_career_license_next(career_license.code,next)){
                        char destination[]="gtmode";switch_to(vol,cache,&mn,&proj,destination);p=menu_ui(mn);root=menu_active_root(mn);
                    }else{
                        /* 0011D020 -> 00110800 -> 0010FCD0: leave the old
                           visit before constructing the next examination. */
                        menu_save_capture(mn);menu_save_refresh(cache[1]);
                        int ok=gt4_career_license_prepare(&exam,argv[1],gt4_game_race_lookup_data(),next,msg,sizeof msg);
                        if(ok){exam.desc.manual=menu_driving_manual(cache[1]);ok=race_game_start_prepared(game,&exam.desc,msg,sizeof msg);}
                        menu_quick_race q;
                        if(ok)ok=race_game_quick_grid(game,&q);
                        if(ok)ok=menu_license_leave(cache[1],career_license.code,&career_license.visit);
                        if(ok){career_license.active=0;ok=menu_license_begin(cache[1],next,&career_license.visit);}
                        if(ok)career_license.active=1;
                        if(ok){
                            career_license.result_applied=0;snprintf(career_license.code,sizeof career_license.code,"%s",next);
                            career_license.best_time=menu_license_best(cache[1],next);snprintf(q.race,sizeof q.race,"%s",next);menu_set_quick_race(&q);menu_set_quick_best(career_license.best_time);
                            snprintf(race_course,sizeof race_course,"%s",q.course);snprintf(race_car,sizeof race_car,"%s",q.grid[0].car);race_color=q.grid[0].color;
                            menu_save_refresh(mn);menu_quick_event_context(mn,next);menu_enter_quick_start(mn);
                            loading_screen_begin();driving=1;race_start_pending=0;
                        }else{fprintf(stderr,"licence next exam failed: %s\n",msg);present_dialog(mn,"Info","Cannot load the next licence examination.",0,NULL,NULL);}
                    }
                }
                else if(!strcmp(command,"COMMAND_NEXT_RACE") && career_race.active && career_race.championship){
                    char msg[256],label[32];gt4_career_event next;unsigned rounds;
                    if(!menu_commit_career_distances(cache[1],mn,career_race.a_distance,career_race.b_distance,&career_race.has_distances)){exit_code=1;goto done;}
                    int ok=career_race.result_applied && gt4_career_championship_label(career_race.series,career_race.round+1,label,&rounds);
                    menu_save_capture(mn);menu_save_refresh(cache[1]);
                    /* 003EF088 keeps entrants; only the player's owned body
                       receives the wear saved after the preceding attempt. */
                    menu_career_request equipped;
                    if(ok)ok=menu_get_career_request(cache[1],&equipped,msg,sizeof msg);
                    if(ok){memcpy(career_race.bodies[0],equipped.player_body,0x4b8);
                        ok=gt4_career_championship_round(&next,argv[1],gt4_game_race_lookup_data(),label,&career_race.championship_grid,msg,sizeof msg);}
                    if(ok){next.desc.manual=menu_driving_manual(mn);ok=race_game_start_prepared(game,&next.desc,msg,sizeof msg);}
                    if(ok)race_game_set_pit_mode_change(game,menu_career_aspec_available(cache[1],label));
                    if(ok){menu_quick_race q;ok=race_game_quick_grid(game,&q);if(ok){
                        career_race.round++;career_race.result_applied=0;career_race.award=0;career_race.grand_prize=0;
                        memcpy(career_race.row,next.race_row,151);snprintf(career_race.race,sizeof career_race.race,"%s",label);
                        q.championship=1;q.session_number=(int)career_race.round;q.final_session=career_race.round+1==career_race.rounds;
                        for(unsigned i=0;i<6;i++)q.total_points[i]=career_race.points[i];
                        snprintf(q.race,sizeof q.race,"%s",label);snprintf(race_course,sizeof race_course,"%s",q.course);
                        menu_set_quick_race(&q);menu_quick_event_context(mn,label);menu_quick_event_result(mn,0,0);
                        menu_enter_quick_start(mn);loading_screen_begin();driving=1;race_start_pending=0;
                    }}
                    if(!ok){fprintf(stderr,"championship next round failed\n");present_dialog(mn,"Info","Cannot load the next championship round.",0,NULL,NULL);}
                }
                else if(!strcmp(command,"COMMAND_RETIRE_CHAMPIONSHIP") && career_race.active && career_race.championship){
                    career_race.retired=1;
                    char destination[]="gtmode";switch_to(vol,cache,&mn,&proj,destination);p=menu_ui(mn);root=menu_active_root(mn);
                }
                else if(!strcmp(command,"COMMAND_DEMONSTRATION") && race_game_license_active(game) && (driving==3 || driving==4 || !driving)){
                    char msg[256];
                    if(race_game_license_demo(game,NULL,msg,sizeof msg)){
                        loading_screen_begin();driving=1;race_start_pending=1;
                    }else {fprintf(stderr,"demonstration: %s\n",msg);present_dialog(mn,"Demonstration","Cannot read the licence demonstration from the disc.",0,NULL,NULL);}
                }
                else if(!strcmp(command,"COMMAND_REPLAY") && (!driving || driving==4) && game && race_game_can_replay(game)) {
                    /* the last race again from its commands (race_game_replay) */
                    char msg[256];
                    {const char *rm=menu_option_text(mn,"replay_mode","NORMAL");
                     race_game_set_replay_options(game,menu_option_number(mn,"replay_display_enable",0)!=0,rm && !strcmp(rm,"DIVE"));}
                    if(race_game_replay(game,msg,sizeof msg)){loading_screen_begin();driving=1;race_start_pending=1;}
                    else fprintf(stderr,"replay: %s\n",msg);
                }
                else if(!strcmp(command,"COMMAND_SAVE_REPLAY") && game && race_game_can_replay(game)) {
                    /* the race lent to the memory card's save screen (its files
                       the save folder's, replay_file.h, the PC layer) */
                    gt4_replay_file r;char msg[160];
                    int ok=race_game_replay_export(game,&r,GT4_REPLAY_FULL);
                    if(ok){   /* the names a list shows, the quick menu's own (CourseName, CAR_NAME) */
                        const char *cn=ui_translate(menu_ui(mn),menu_active_root(mn)<0?0:menu_active_root(mn),"CourseName",r.course),*car=menu_car_name(mn,r.car);
                        snprintf(r.course_name,sizeof r.course_name,"%s",cn && *cn && strcmp(cn,r.course)?cn:"");
                        snprintf(r.car_name,sizeof r.car_name,"%s",car?car:"");
                    }
                    if(ok){   /* labomode's SaveReplayRoot (MODE_SAVE_FULL_REPLAY), back to this menu */
                        replay_to_save=r;menu_set_replay_to_save(&replay_to_save);
                        menu_memcard_return(projects[proj]);
                        char labo[]="labomode";switch_to(vol,cache,&mn,&proj,labo);
                        if(proj==12 && !menu_open_labo(mn,"MODE_SAVE_FULL_REPLAY"))fprintf(stderr,"labomode: save screen failed\n");
                        p=menu_ui(mn);root=menu_active_root(mn);
                    }else{snprintf(msg,sizeof msg,"The replay could not be saved.");present_dialog(mn,"Save Replay",msg,0,NULL,NULL);}
                }
                else if((!strcmp(command,"COMMAND_SAVE_GHOST") || !strcmp(command,"COMMAND_LOAD_GHOST")) && game) {
                    /* quick-tt.ad SaveGhost / LoadGhost: labomode's SaveReplayRoot /
                       LoadReplayRoot in the best lap's modes, back to this menu;
                       a loaded one is taken as the ghost below */
                    int save=!strcmp(command,"COMMAND_SAVE_GHOST"),ready=1;
                    if(save){
                        gt4_replay_file r;
                        ready=race_game_ghost_export(game,&r);
                        if(ready){
                            const char *cn=ui_translate(menu_ui(mn),menu_active_root(mn)<0?0:menu_active_root(mn),"CourseName",r.course),*car=menu_car_name(mn,r.car);
                            snprintf(r.course_name,sizeof r.course_name,"%s",cn && *cn && strcmp(cn,r.course)?cn:"");
                            snprintf(r.car_name,sizeof r.car_name,"%s",car?car:"");
                            replay_to_save=r;menu_set_replay_to_save(&replay_to_save);
                        }else present_dialog(mn,"Save Ghost","There is no lap to save.",0,NULL,NULL);
                    }
                    if(ready){
                    menu_memcard_return(projects[proj]);
                    char labo[]="labomode";switch_to(vol,cache,&mn,&proj,labo);
                    if(proj==12 && !menu_open_labo(mn,save?"MODE_SAVE_BESTLAP_REPLAY":"MODE_LOAD_BESTLAP_REPLAY"))fprintf(stderr,"labomode: ghost screen failed\n");
                    p=menu_ui(mn);root=menu_active_root(mn);
                    }
                }
                else if(!strcmp(command,"COMMAND_SETTINGS")){
                    quick_options_project=proj;char setting[]="setting";quick_options=2;switch_to(vol,cache,&mn,&proj,setting);
                    p=menu_ui(mn);root=menu_active_root(mn);
                }
                else if(!strcmp(command,"COMMAND_LOGGER") && game){
                    /* quick-tt.ad L483 / quick-license.ad: the Logger
                       project over the last race; its exit returns here
                       (logger.ad exit: setReturnToLastMenuProject) */
                    menu_logger_source src;char amsg[160];
                    if(getenv("OPENGT4_LOGGER_ANALYZE") && !race_game_logger_analyze(game,1,amsg,sizeof amsg))fprintf(stderr,"logger: %s\n",amsg);   /* tests: the analysis by replay */
                    if(race_game_logger_source(game,&src)){
                        if(career_license.active && career_license.result_applied && (src.time[0]==0 || src.time[0]==0x157529ffu))src.time[0]=career_license.last_time;
                        menu_set_logger_source(&src);}
                    else menu_set_logger_source(NULL);
                    logger_game=game;menu_set_logger_analyzer(NULL,logger_analysis_begin,logger_analysis_step,logger_analysis_end);
                    char logger[]="logger:LoggerRoot";switch_to(vol,cache,&mn,&proj,logger);
                    p=menu_ui(mn);root=menu_active_root(mn);
                }
                else if(!strcmp(command,"COMMAND_OPTION")){
                    /* quick-arcade.ad L472 returns to the native race caller;
                       the option project must finish back at this quick menu. */
                    quick_options_project=proj;char option[]="option";quick_options=1;switch_to(vol,cache,&mn,&proj,option);
                    p=menu_ui(mn);root=menu_active_root(mn);
                }
            }
        }
        if(quick_options && (returning_quick_options || (proj!=3 && proj!=4))){
            returning_quick_options=0;
            if(quick_options==2 && !race_game_license_active(game)){
                uint8_t body[0x4b8];char msg[256];
                if(menu_riding_car_body(mn,body) && race_game_setting(game,body)){
                    /* Preserve the completed attempt/replay; the next Start
                       rebuilds it. A pre-grid scene must use the new sheet. */
                    if(race_game_finished(game)<0){
                        if(!race_game_restart(game,msg,sizeof msg)){fprintf(stderr,"cannot apply garage setting: %s\n",msg);exit_code=1;goto done;}
                        int32_t stats[3];if(gt4_career_vehicle_stats(app_car_factory,body,stats))
                            menu_update_quick_player((int32_t)((float)stats[0]/100.f+.5f),stats[2]);
                        loading_screen_begin();driving=1;race_start_pending=0;
                    }
                }
            }
            /* quick-championship.ad getStartPage L22..39 and quick-event:
               Option returns through the selector (AfterRoot if finished),
               keeping the cursorPosition saved by the original button. */
            char quick[64];snprintf(quick,sizeof quick,"%s",quick_options_project>=0?projects[quick_options_project]:career_license.active?"quick-license":career_race.active?(career_race.mission?"quick-mission":career_race.championship?"quick-championship":"quick-event"):race_quick_route(race_time_trial));quick_options=0;quick_options_project=-1;pregrid_clock=0;
            switch_to(vol,cache,&mn,&proj,quick);
            p=menu_ui(mn);root=menu_active_root(mn);
        }
        if(driving && !race_quick_project(proj) && proj!=12 && proj!=16 && !quick_options && !race_start_pending && driving!=2 && driving!=5) {   /* the quick menu left (Exit): no race */
            if(career_race.active && career_race.has_distances){
                if(!menu_commit_career_distances(cache[1],mn,career_race.a_distance,career_race.b_distance,&career_race.has_distances)){exit_code=1;goto done;}
            }
            career_race.active=0;race_game_stop(game);driving=0;loading_screen_end();
        }
        uint64_t now=plat_time_ns();
        double elapsed=(now-presentation_clock)/1e9;
        presentation_clock=now;
        /* P: opt-in offline film export. Every rendered frame advances one
           existing 60 Hz step; no physics formulas, parameters or tick rate
           change. Export can take longer than its playback on a busy GPU. */
        if(video_export_fixed_step())elapsed=1.0/60.0;
        float notice_level=0;
        if(notice<GT4_NOTICE_COUNT) {
            if(!notice_start)notice_start=now;
            notice_level=gt4_boot_notice_level(notice,(now-notice_start)/1e9);
            while(notice<GT4_NOTICE_COUNT && (notice_level<0 || !notice_tex[notice])) {
                if(++notice<GT4_NOTICE_COUNT){notice_start=now;notice_level=gt4_boot_notice_level(notice,0);}
            }
            if(notice==GT4_NOTICE_COUNT){plat_video_set_view(640,480);elapsed=0;last_root=-1;}
        }
        int notice_active=notice<GT4_NOTICE_COUNT;
        if(driving==1 && (!loading_screen_active() || loading_screen_ready())){
            int ready=race_game_poll(game);
            if(ready>0){
                if(race_start_pending){driving=2;race_start_pending=0;plat_video_set_view(640,448);}
                else driving=3;
            }
            else if(ready<0){
                if(theater==2){menu_attract_finish(mn,1);theater=0;}
                driving=0;fprintf(stderr,"cannot load course %s\n",race_course);
                present_dialog(mn,"Info","Nao foi possivel carregar esta pista. Veja o log.",0,NULL,NULL);
            }
            if(ready){loading_screen_end();plat_video_set_view(640,driving==2?448:480);last_root=-1;elapsed=0;presentation_clock=plat_time_ns();}
        }
        if(race_loading && (!loading_screen_active() || loading_screen_ready())){
            int ready=race_view_poll(race,race_loading<0);
            if(ready){
                loading_screen_end();plat_video_set_view(640,ready>0 && race_loading>0?448:480);
                if(ready>0 && race_loading>0){race_active=1;plat_video_set_view(640,448);}
                else if(ready<0 && race_loading>0){
                    fprintf(stderr,"cannot load course %s\n",race_course);
                    if(shot){exit_code=1;goto done;}
                    present_dialog(mn,"Info","Nao foi possivel carregar esta pista. Veja o log.",0,NULL,NULL);
                }
                race_loading=0;last_root=-1;elapsed=0;presentation_clock=plat_time_ns();
            }
        }
        if(mn && loading_screen_revealing() && !race_active && !race_loading && driving!=2 && !notice_active)
            animate_loading_destination(mn,(float)elapsed);
        if (mn && !loading_screen_active() && !race_active && !race_loading && driving!=2 && !notice_active) {
            if(proj==16){
                /* the Logger's graph reads the sticks (001206A0: left x/y the
                   cursor and the scroll, right x/y the zooms). P: the arrows
                   are the left stick, Page Up/Down and Home/End the right */
                float axes[4]={0};plat_pad_state pad;
                if(plat_input_poll(0,&pad))for(int k=0;k<4;k++)axes[k]=pad.axes[k];
                const Uint8 *k=SDL_GetKeyboardState(NULL);
                axes[0]+=(float)(k[SDL_SCANCODE_RIGHT]-k[SDL_SCANCODE_LEFT]);
                axes[1]+=(float)(k[SDL_SCANCODE_DOWN]-k[SDL_SCANCODE_UP]);
                axes[2]+=(float)(k[SDL_SCANCODE_PAGEUP]-k[SDL_SCANCODE_PAGEDOWN]);
                axes[3]+=(float)(k[SDL_SCANCODE_HOME]-k[SDL_SCANCODE_END]);
                menu_set_sticks(mn,axes);
            }
            /* Asset/device stalls must not spend six native cursor steps in
               one displayed frame. The 60 Hz motion itself is unchanged. */
            menu_advance(mn,fmin(elapsed,1.0/30.0));
            char film_path[1200];
            if(proj==12 && menu_take_photo_film(mn,film_path,sizeof film_path)){
                int preview=0;while(preview<NPROJECTS && strcmp(projects[preview],"photo_save"))preview++;
                if(preview<NPROJECTS && !cache[preview])cache[preview]=load(vol,preview);
                if(preview<NPROJECTS && cache[preview] && photo_travel_develop(presentation_photo,cache[preview],film_path,pump_loading_work,NULL)){
                    photo_return_project=12;proj=preview;mn=cache[preview];driving=6;plat_video_set_view(640,480);
                }else {fprintf(stderr,"Photo film: cannot develop %s\n",film_path);menu_open_labo(mn,"PhotoRoot");}
            }else if(driving==6){
                char destination[64];if(menu_take_project(mn,destination,sizeof destination)){
                    photo_travel_drive_end(presentation_photo);proj=photo_return_project;photo_return_project=-1;mn=cache[proj];driving=0;menu_open_labo(mn,"PhotoRoot");
                }
            }else if(driving==5){
                /* PhotoShoot Exit uses the authored confirmation/sequence.
                   Restore the held replay instead of entering its major menu. */
                char destination[64];
                if(menu_take_project(mn,destination,sizeof destination)){
                    if(proj==21){
                        photo_travel_drive_editor(presentation_photo);proj=14;mn=cache[14];
                    }else {
                    photo_travel_drive_end(presentation_photo);proj=photo_return_project;photo_return_project=-1;
                    mn=cache[proj];driving=2;race_game_photo_pause(game,0);plat_video_set_view(640,448);
                    presentation_clock=plat_time_ns();
                    }
                }
            }else if(quick_options && (proj==3 || proj==4)){
                /* 0010FE40 quick loop: Option returns to its native caller.
                   Consume the authored major-menu route without opening it. */
                char destination[64];
                if(menu_take_project(mn,destination,sizeof destination))returning_quick_options=1;
            }else switch_requested(vol, cache, &mn, &proj);
            p = menu_ui(mn); root = menu_active_root(mn);
            if(race_quick_project(proj) && game){
                /* 0010FEE8: the Logger left with a track to load: labomode's
                   best laps for this course, then the Logger again */
                int track=menu_take_logger_load();
                if(track>=0){
                    /* the quick menu's own Load Ghost, once it is back */
                    logger_track=track;logger_load_asked=1;
                }
            }
        }
        /* Music requests belong to the destination page; MAP belongs to GT Mode. */
        const char *page=p ? ui_root_name(p,root) : "";
        int pcm_volume=0;const char *pcm_group=menu_pcm_request(mn,&pcm_volume);
        int wants_pcm=(!shot || music_with_shot) && !notice_active && !race_active && driving!=2 &&
                      !loading_screen_active() && pcm_group && *pcm_group && strcmp(page,"MovieRoot");
        int prepare_pcm=(!shot || music_with_shot) && !notice_active && !race_active && driving!=2 &&
                        pcm_group && *pcm_group && strcmp(page,"MovieRoot");
        if(!prepare_pcm)pregrid_audio_update(pregrid_music,NULL,0,0);
        if(!strcmp(page,"MovieRoot") && (root!=movie_page_root || proj!=movie_page_project)) {
            if(movie.lock){SDL_LockMutex(movie.lock);movie.name[0]=0;SDL_UnlockMutex(movie.lock);}
        }
        movie_page_root=root;movie_page_project=proj;
        {int car_page=!race_active && driving!=2 && !strcmp(page,"CarRoot");if(car_page && !was_car_page)showroom_hide(cars);was_car_page=car_page;}
        /* A project's mSound stops its music when the project goes
           (destructor 0023E078 -> 0023F890 -> 002C3BA0/00460B90): the
           option project, with no bgm on its pages, is silent (PCSX2). */
        {static int music_project=-1;if(proj!=music_project){if(music_project>=0)desired_music[0]=0;music_project=proj;}}
        const char *request=mn?menu_music_name(mn):NULL;
        if(request)snprintf(desired_music,sizeof desired_music,"%s",request);
        int music_top=proj==0 && !strcmp(page,"TopRoot");
        /* TopRoot authors bgm="keep", not a named title playlist. MAP must
           not leak across GT Mode exit; a title-specific source is unverified. */
        if(music_top)desired_music[0]=0;
        if(music_faded){
            plat_audio_set_music(NULL,NULL);music_playing=0;playing_music=NULL;music_faded=0;
            printf("music: stopped after authored fade\n");
        }

        int wants_music=(!shot || music_with_shot) && !wants_pcm && !race_quick_project(proj) && !loading_screen_active() && !notice_active && strcmp(page,"MovieRoot") && !race_active && driving!=2 && *desired_music;
        music_job *requested_music=NULL;
        /* Prepare the authored destination during loading; only publishing
           the audio waits for wants_music. No disc work in the callback. */
        int prepare_requested=(!shot || music_with_shot) && !wants_pcm &&
            !race_quick_project(proj) && !notice_active && strcmp(page,"MovieRoot") &&
            !race_active && driving!=2 && *desired_music;
        if(prepare_requested) {
            for(int i=0;i<music_count;i++)if(!strcmp(music_load[i].name,desired_music))requested_music=&music_load[i];
            if(requested_music && requested_music!=playing_music && requested_music->played && SDL_AtomicGet(&requested_music->ready)){
                /* func_00461068/00460FE0 saves current sequence + 1;
                   func_004616E8 reduces it modulo the disc group count. */
                if(requested_music->playlist){
                    requested_music->sequence=gt4_music_next_sequence((unsigned)SDL_AtomicGet(&requested_music->current_sequence))%requested_music->count;
                    SDL_AtomicSet(&requested_music->current_sequence,(int)requested_music->sequence);
                    requested_music->music=SDL_AtomicGet(&requested_music->track_ready[requested_music->sequence])?requested_music->playlist[requested_music->sequence]:NULL;
                } else if(requested_music->count>1){
                    if(requested_music->thread)SDL_WaitThread(requested_music->thread,NULL);
                    requested_music->sequence=gt4_music_next_sequence(requested_music->sequence);
                    gt4_music_close(requested_music->music);requested_music->music=NULL;
                    SDL_AtomicSet(&requested_music->frames,0);
                    SDL_AtomicSet(&requested_music->ready,0);
                    requested_music->thread=SDL_CreateThread(prepare_music,"menu music preparation",requested_music);
                    if(!requested_music->thread)SDL_AtomicSet(&requested_music->ready,1);
                }
                requested_music->played=0;
            }
            if(!requested_music && music_count<32) {
                requested_music=&music_load[music_count++];requested_music->disc=argv[1];
                requested_music->tempo=music_tempo;
                snprintf(requested_music->name,sizeof requested_music->name,"%s",desired_music);
                SDL_AtomicSet(&requested_music->volume,120);
                requested_music->thread=SDL_CreateThread(prepare_music,"menu music preparation",requested_music);
                if(!requested_music->thread)SDL_AtomicSet(&requested_music->ready,1);
            }
        }
        if(!wants_music)requested_music=NULL;
        if(playing_music && playing_music!=requested_music) {
            plat_audio_release_music(playing_music);music_playing=0;playing_music=NULL;
            printf("music: stopped\n");
        }
        if(requested_music) {
            if(SDL_AtomicGet(&requested_music->ready) && requested_music->playlist && !music_playing) {
                unsigned current=(unsigned)SDL_AtomicGet(&requested_music->current_sequence);
                requested_music->music=SDL_AtomicGet(&requested_music->track_ready[current])?requested_music->playlist[current]:NULL;
            }
            int volume=(int)menu_option_number(mn,"menu_bgm_volume_raw",120);
            if(volume<0)volume=0;if(volume>120)volume=120;
            SDL_AtomicSet(&requested_music->volume,volume);
            if(!music_playing && SDL_AtomicGet(&requested_music->ready) && requested_music->music) {
                gt4_music_rewind(requested_music->music);
                music_playing=plat_audio_set_music(render_menu_music,requested_music)==0;
                if(music_playing){requested_music->played=1;playing_music=requested_music;printf("music: %s sequence=%u, 48000 Hz, tempo %.3f\n",requested_music->name,requested_music->sequence,music_tempo);fflush(stdout);}
                presentation_clock=plat_time_ns();
            }
        }
        if(prepare_pcm){int sequence,repeat;unsigned serial;menu_pcm_parameters(mn,&sequence,&repeat,&serial);
            pregrid_audio_update_gated(pregrid_music,pcm_group,pcm_volume,(uint32_t)plat_time_ns(),sequence,repeat,serial,wants_pcm);}
        pregrid_audio_pause(pregrid_music,menu_pcm_paused(mn));
        menu_pcm_state(mn,wants_pcm && pregrid_audio_playing(pregrid_music));
        /* ResultLicense/ResultArcade advance per VBlank (003DF978 +B0),
           including failure colour decay. The mission shares this clock. */
        video_set_menu_pacing((!race_active && driving!=2) || (race_active && (race_game_license_active(game) || race_game_mission_active(game))));
        video_set_menu_projection(!race_active && driving!=2);
        presentation_pregrid=(driving==3 || driving==4) && race_quick_project(proj);
        if(!presentation_pregrid)pregrid_clock=0;
        plat_video_begin_frame();
        int car_ready=1;
        int scene_layer=race_active || driving==2 || (!notice_active && mn && page_below_popup(mn,"CarRoot"));
        if(scene_layer)video_begin_scene();
        if(driving==2){
            {   /* the replay has its own volumes (replay_bgm/se_volume_raw) */
                int rp=race_game_replaying(game);
                race_game_set_audio(game,menu_option_number(mn,rp?"replay_bgm_volume_raw":"race_bgm_volume_raw",120),
                                    menu_option_number(mn,rp?"replay_bgm":"race_bgm",1),
                                    menu_option_number(mn,rp?"replay_se_volume_raw":"race_se_volume_raw",120));
            }
            race_game_update(game,elapsed);car_ready=race_game_draw(game);
            if(theater==2)attract_seconds+=elapsed; /* 00434590 +0x10: 90-second demo budget. */
            {static int told;if(race_game_finished(game)>=0 && !told){told=1;printf("race: the player finished\n");}}
            {   /* tests: OPENGT4_RACE_REPLAY_AFTER=<seconds> replays the race from then
                   (with OPENGT4_REPLAY_SAVE, saved and read back first) */
                static double raced;const char *after=getenv("OPENGT4_RACE_REPLAY_AFTER");
                if(after && !race_game_replaying(game) && (raced+=elapsed)>=atof(after)) {
                    char msg[256];raced=-1e9;
                    if(getenv("OPENGT4_REPLAY_SAVE")){
                        gt4_replay_file r,back;char name[64];
                        if(race_game_replay_export(game,&r,GT4_REPLAY_FULL) && gt4_replay_store(&r,name,sizeof name) && gt4_replay_load(name,&back)){
                            fprintf(stderr,"replay: saved %s, %zu steps read back\n",name,back.steps);
                            race_game_replay_import(game,&back);gt4_replay_free(&back);
                        }else fprintf(stderr,"replay: save failed\n");
                    }
                    if(race_game_replay(game,msg,sizeof msg)){loading_screen_begin();driving=1;race_start_pending=1;}
                    else fprintf(stderr,"replay: %s\n",msg);
                }
            }
            if(driving==2 && !race_game_demonstrating(game) && race_game_over(game) && race_quick_project(proj)) {
                if(!career_race_result(game,cache[1],mn)){fprintf(stderr,"career result could not be recorded\n");exit_code=1;goto done;}
                /* the results over (cb_raceexit): back to the quick menu */
                session_best_update(game,mn,race_course,race_car);race_return=1;
            }
        }else if(race_active){
            const uint8_t *keys=SDL_GetKeyboardState(NULL);float dt=(float)fmin(elapsed,.05);
            race_view_camera(race,(keys[SDL_SCANCODE_RIGHT]-keys[SDL_SCANCODE_LEFT])*dt,(keys[SDL_SCANCODE_DOWN]-keys[SDL_SCANCODE_UP])*dt*3);
            car_ready=race_view_draw(race);
        }else if(notice_active) {
            /* Centered in the 640x448 frame and faded from black, as the
               boot executable (0x01000A48) and CORE (0x001091B8) draw them. */
            const tex1_image *im=gt4_boot_notice_image(&notices,notice);
            uint8_t v=(uint8_t)(255*notice_level+.5f),tint[4]={v,v,v,255};
            plat_video_set_view(640,448);
            plat_draw_quad(notice_tex[notice],(float)((640-im->width)>>1),(float)((448-im->height)>>1),
                           (float)im->width,(float)im->height,0,0,1,1,tint);
        }else if(mn && page_below_popup(mn,"CarRoot")) {
            car_ready=showroom_draw(cars,menu_selected_car(mn),(float)elapsed);
            if(!color_preview)showroom_set_color(cars,menu_car_color(mn));   /* the script's colour chips */
            if(car_ready && color_preview && !color_applied){showroom_color(cars,color_preview);color_applied=1;car_ready=0;}
        }
        if(scene_layer)video_end_scene();
        if(driving==2 && race_game_wants_thumbnail(game)){   /* the race's picture for a saved replay, before its HUD */
            int tw,th;unsigned char *px=plat_sdl_read_view(&tw,&th);
            if(px){race_game_set_thumbnail(game,px,tw,th);free(px);}
        }
        if(driving==2)race_game_draw_hud(game,elapsed);
        int drawn = 0, skipped = 0;
        movie.seen=movie.drawn=0;
        if (p && !race_active && driving!=2 && !notice_active) {ui_set_movie_renderer(p,draw_movie,&movie);if(driving==5 && proj==14)photo_travel_editor_draw(presentation_photo);else draw_pages(mn,&drawn,&skipped);}
        if((photo_project(proj) || driving==6 || (driving==5 && proj==21)) && !notice_active)photo_travel_feedback(presentation_photo);
        {   /* P: opt-in filming input, the same shutter path as F5. A real
               Photo Travel render/preview; no replacement image or UI. */
            static uint64_t photo_film_started;static int photo_film_shot,photo_film_focused;
            const char *after=getenv("OPENGT4_PHOTO_SHOOT_AFTER");
            if(after && photo_project(proj) && !loading_screen_active() && !photo_film_shot){
                uint64_t now=plat_time_ns();if(!photo_film_started)photo_film_started=now;
                /* P: photographer's framing for this recording only. Aim at
                   the body, then let the ordinary centre-depth AF sample it
                   on the next frame before pressing the shutter. */
                if(!photo_film_focused && (now-photo_film_started)/1e9>=atof(after)-1){
                    gt4_photo_mode *s=menu_photo_state(mn);
                    float dx=s->car[0]-s->eye[0],dy=s->car[1]+.65f-s->eye[1],dz=s->car[2]-s->eye[2];
                    s->yaw=atan2f(dx,-dz)*57.2957763671875f;
                    s->pitch=-atan2f(dy,sqrtf(dx*dx+dz*dz))*57.2957763671875f;
                    s->manual_focus=0;s->focus_requested=1;photo_film_focused=1;
                }
                if((now-photo_film_started)/1e9>=atof(after)){
                    photo_travel_shoot(presentation_photo);photo_film_shot=1;
                }
            }
        }
        if(presentation_pregrid && p && !race_active && driving!=2 && !notice_active)
            race_game_draw_pregrid_overlay(presentation_race);
        /* A: cached PC decoder outlives its page. Original project teardown
           stops sound (0023E078 -> 0023F890); never leak the movie bus into
           race/replay. Resume is handled by draw_movie above. */
        if(!movie.seen && movie.lock){
            SDL_LockMutex(movie.lock);
            sdl_movie_audio(movie.decoder,0);sdl_movie_audio(movie.previous,0);
            /* Request a fresh decoder on re-entry; the stopped decoder's
               audio_attempted flag cannot start its sound a second time. */
            movie.name[0]=0;
            SDL_UnlockMutex(movie.lock);
        }
        if(loading_screen_active()){loading_screen_draw();if(loading_shot && loading_screen_ready()){screenshot(loading_shot);loading_shot=NULL;}}
        if(loading_screen_revealing() && getenv("OPENGT4_LOADING_REVEAL_SHOT")){
            static unsigned reveal_frames;
            if(++reveal_frames==5)screenshot(getenv("OPENGT4_LOADING_REVEAL_SHOT"));
        }
        if(p && !race_active && driving!=2 && movie.seen && movie.finish_notified!=movie.generation &&
           (movie.unavailable || (movie.decoder && movie.displayed==movie.generation && sdl_movie_finished(movie.decoder)))) {
            movie.finish_notified=movie.generation;
            printf("movie: %s finished after %llu frames\n",movie.name,(unsigned long long)sdl_movie_frame_count(movie.decoder));
            sdl_movie_audio(movie.decoder,0);
            if(menu_movie_finished(mn)) {
                menu_tick(mn);
                switch_requested(vol,cache,&mn,&proj);
            }
        }
        /* Retain the last frame/decoder while changing pages; worker replaces it. */
        if(shot && ((!loading_preview && loading_screen_active()) || (loading_preview && !loading_screen_ready()) || race_loading || (driving==1 && race_start_pending) || (movie.seen && !movie.drawn) || !car_ready)) {
            if(!shot_wait)shot_wait=plat_time_ns();
            if(plat_time_ns()-shot_wait<((race_preview || !strncmp(movie.name,"opening",7))?30000000000ull:5000000000ull)){plat_video_end_frame();SDL_Delay(10);continue;}
            fprintf(stderr,"presentation: timed out waiting for requested asset\n");exit_code=1;
        } else if(shot) shot_wait=0; /* each movie transition gets a fresh loading deadline */
        int shot_due=shot!=NULL;
        if(shot && shot_delay>0) {
            if(!shot_started)shot_started=plat_time_ns();
            /* Capture delay must not skip the result/retry/menu transitions
               below. Only the screenshot waits, not the application's flow. */
            shot_due=(plat_time_ns()-shot_started)/1e9>=shot_delay;
        }
        if (shot_due) {
            printf("video settings: filter=%d text=%d vsync=%d fullscreen=%d size=%dx%d upscaling=%d aspect=%d\n",
                settings.current.filter,settings.current.text,settings.current.video.vsync,
                settings.current.video.fullscreen,settings.current.video.width,settings.current.video.height,settings.current.video.upscale,settings.current.aspect);
            if(race_active)printf("race preview: course=%s car=%s\n",race_course,race_car);
            if(!strcmp(page,"CarRoot"))printf("car color=%s\n",showroom_caption(cars));
            if(movie.decoder)printf("movie frames=%llu\n",(unsigned long long)sdl_movie_frame_count(movie.decoder));
            printf("%s / %s: drawn %d, not yet drawn %d\n", projects[proj],
                   p ? ui_root_name(p, root) : "?", drawn, skipped);
            const char *car=menu_selected_car(mn);
            if(car)printf("selected car=%s\n",car);
            const char *course=menu_selected_course(mn);
            if(course)printf("selected course=%s\n",course);
            if(p)report_choices(ui_root_widget(p,root));
            if(p && getenv("OPENGT4_UI_TREE"))report_tree(ui_root_widget(p,root),0);
            screenshot(shot);
            goto done;
        }
        plat_video_end_frame();
        /* the results over: the race's replay plays by itself from its
           opening, then the quick menu (as the game goes on) */
        if(race_return && career_license.active && race_game_license_retry(game)){
            char msg[256];race_return=0;career_license.result_applied=0;
            if(race_game_restart(game,msg,sizeof msg)){loading_screen_begin();driving=1;race_start_pending=1;}
            else {fprintf(stderr,"licence retry: %s\n",msg);exit_code=1;goto done;}
        }
        /* 0011BB60 returns RaceMission to 0011BF60's QuickWork loop;
           replay is an explicit menu command, not Arcade's automatic replay. */
        if(race_return && !career_license.active && !career_race.mission && !race_game_replaying(game) && race_game_can_replay(game)) {
            char msg[256];race_return=0;
            {const char *rm=menu_option_text(mn,"replay_mode","NORMAL");
             race_game_set_replay_options(game,menu_option_number(mn,"replay_display_enable",0)!=0,rm && !strcmp(rm,"DIVE"));}
            if(race_game_replay(game,msg,sizeof msg)){loading_screen_begin();driving=1;race_start_pending=1;}
            else {fprintf(stderr,"replay: %s\n",msg);race_return=1;}
        }
        if(race_return){
            loading_screen_begin();present_wait(mn,menu_loading_duration(mn),NULL);
            race_game_return_pregrid(game);driving=4;pregrid_clock=0;race_return=0;last_root=-1;
            menu_set_quick_completed(mn,1);
            if(career_license.active){
                /* 0010FBD8 rebuilds quick-license after the examination.
                   Re-run its authored BestTime/ranking/button initialization. */
                menu_quick_event_context(mn,career_license.code);
                menu_enter_quick_start(mn);
            }else menu_enter_quick_start(mn);
            /* quick-event.ad L899 queues doAction: dispatch only after the
               loading overlay has finished, from the normal menu loop. */
            ui_prepare_root(menu_ui(mn),menu_active_root(mn),pump_loading_work,NULL);
            loading_screen_end();plat_video_set_view(640,480);presentation_clock=plat_time_ns();
            ui_fade(menu_ui(mn),1,0,.2f);
        }
        /* the window keeps the title "OpenGT4"; the page goes to the log */
        if(!(driving==2 || (driving==1 && race_start_pending)) && !race_active && !race_loading && (root != last_root || proj != last_proj)) {
            char title[256];
            snprintf(title, sizeof title, "OpenGT4%s - %s / %s (%d/%d)  drawn %d, not yet drawn %d",
                     !strcmp(page,"CarRoot")?" [F5: carro na pista]":preview_courses?" [Catalog preview]":"",
                     projects[proj], p ? ui_root_name(p, root) : "?", root + 1, p ? ui_root_count(p) : 0,
                     drawn, skipped);
            printf("%s\n", title);
            last_root = root;
            last_proj = proj;
        }
    }
done:
    photo_travel_close(presentation_photo);presentation_photo=NULL;
    race_game_close(game);game=NULL; /* audio writes its jukebox back before the final profile save */
    if(mn){menu_save_capture(mn);if(gt4_save_dirty() && gt4_save_auto_enabled() && gt4_save_active() && !gt4_save_store())fprintf(stderr,"Auto Save: %s\n",gt4_save_error());}
    gt4_save_shutdown();
    gt4_game_set_stats_reader(NULL,NULL);
    gt4_game_set_service_reader(NULL,NULL);
    gt4_game_set_setting_reader(NULL,NULL);
    for(int i=0;i<2;i++){showroom_close(split_selection_cars[i]);split_selection_cars[i]=NULL;}
    showroom_close(cars);race_view_close(race);gt4_car_ratings_close(app_car_ratings);gt4_game_set_car_factory(NULL,NULL);gt4_game_set_auto_gears(NULL,NULL);gt4_career_vehicle_close(app_car_factory);
    for(int i=0;i<GT4_NOTICE_COUNT;i++)plat_texture_destroy(notice_tex[i]);
    gt4_boot_notices_close(&notices);
    if(movie.thread){SDL_LockMutex(movie.lock);movie.stop=1;SDL_CondSignal(movie.wake);SDL_UnlockMutex(movie.lock);SDL_WaitThread(movie.thread,NULL);}
    sdl_movie_close(movie.previous);sdl_movie_close(movie.decoder);sdl_movie_close(movie.pending);sdl_movie_close(movie.retired);
    if(movie.wake)SDL_DestroyCond(movie.wake);if(movie.lock)SDL_DestroyMutex(movie.lock);
    gt4_vol_close(movie.volume);
    preparing_jobs=NULL;preparing_count=NULL;preparing_pcm=NULL;
    pregrid_audio_close(pregrid_music);
    plat_audio_shutdown();
    for(int i=0;i<music_count;i++) {
        if(music_load[i].thread)SDL_WaitThread(music_load[i].thread,NULL);
        if(music_load[i].playlist){
            for(unsigned j=0;j<music_load[i].count;j++)gt4_music_close(music_load[i].playlist[j]);
            free(music_load[i].playlist);free(music_load[i].track_ready);free(music_load[i].track_frames);
        } else gt4_music_close(music_load[i].music);
    }
    for (int i = 0; i < NPROJECTS; i++) menu_close(cache[i]);
    ui_project_free(dialog_ui);dialog_ui=NULL;
    loading_screen_close();
    plat_video_shutdown();
    gt4_vol_close(vol);
    return exit_code;
}
