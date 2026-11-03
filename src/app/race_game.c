/*
 * A race in the game (race_game.h). The physics is the native race of
 * src/core/physics (race_run.c); the player's car answers to the keyboard
 * or a game controller through the physics' host hooks (gt4_phys_pad), as
 * in tests/physics/drive.c: the keyboard as a digital pad, a controller's
 * stick and triggers as an analog one; the other cars keep the AI. The
 * view draws the course and every car where the physics has it.
 */
#include "race_game.h"
#include "career_original.h"
static int watched_car(const race_game *g);
#include "race_run.h"
#include "race_view.h"
#include "race_hud.h"
#include "license_session.h"
#include "license_presentation.h"
#include "license_concourse.h"
#include "license_motion.h"
#include "race_pit_crew.h"
#include "license_scene.h"
#include "license_image.h"
#include "car_model.h"
#include "career_license.h"
#include "career_parts.h"
#include "logger.h"
#include "race_result.h"
#include "race_mission.h"
#include "render3d.h"
#include "video_gl.h"
#include "race_pause.h"
#include "race_bspec.h"
#include "race_pitmenu.h"
#include "race_audio.h"
#include "race_smoke.h"
#include "race_leaves.h"
#include "pcm_music.h"
#include "course_motion.h"
#include "course_pregrid.h"
#include "course_tvcam.h"
#include "car.h"
#include "specdb.h"
#include "arcade.h"
#include "vol.h"
#include "platform/platform.h"
#include "replay_file.h"
#include "disc_replay.h"
#include "disc_checkpoint.h"
#include "race_split_layout.h"
#include "save.h"
#include "plat_sdl.h"

#include <SDL.h>
#include <fenv.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STEP (1.0 / 60)

struct race_game {
    char *disc;
    race_view *view;
    gt4_race_run *run;
    int loading, ready, others, laps, time_trial;
    int world_mode;        /* GT Mode Free Run 11 / Practice 12 (race_game_set_world_mode) */
    int tire, manual, assist_asm, assist_tcs;
    int split;gt4_race_desc split_options;
    uint8_t split_owned[2][0x4b8],arcade_owned[0x4b8];int arcade_owned_on;
    race_hud *hud_2p;
    int camera_2p,view_held_2p,look_back_2p,retry_requested;
    int pause_keys_2p[6];
    float split_eye[2][3],split_rows[2][9],split_projection[2][3];
    int split_camera_valid[2];
    /* B-Spec: the next race's (COMMAND_SPECTATOR) and the pad's buttons
       last frame, its orders sent on a press */
    int bspec, run_bspec;
    uint32_t bspec_buttons, bspec_pressed;
    race_bspec *bspec_ui;   /* the race's B-Spec display (world+0xE424) */
    race_pitmenu *pitmenu;  /* the pit menu (world+0xE40C) */
    int pit_car, pit_touched, pit_mode_allowed;   /* the car it is up for, a key sent to it (+0xE4) */
    const race_hud_state *bspec_hud;   /* the display's state while it draws (its map) */
    /* the Arcade's QuickTune as the next race takes it (00111DC0) */
    int quick_tuned, quick_power, quick_weight, quick_front_tire, quick_rear_tire, quick_gear;
    double finished;
    unsigned ids[6];
    double acc;
    SDL_GameController *pad;
    gt4_vol *vol;          /* the display's assets */
    race_hud *hud;
    race_audio *audio;
    gt4_license_motion *pit_motion[6];gt4_pit_crew pit_crew[6];license_scene *pit_scene;uint32_t pit_random,pit_camera_random;int pit_package;
    int bgm_volume, bgm_enabled, se_volume;
    int unit;
    int camera;            /* the view (the car model's camera point): 1 behind the car (the start's), 0 on it, 6 the roof */
    int view_held;
    int look_back;
    int ghost_button,ghost_hidden;
    char hud_course[32];   /* the course whose map the display takes next */
    race_result *result;   /* the results over the race, from the player's finish */
    int mission_presentation;
    FILE *mission_test_pad;
    r3d_mesh *trophy[3];int trophy_tried;
    plat_texture *license_screen;
    float license_saturation;
    int license_image_options[4],license_image_valid,license_image_active;
    uint8_t license_image_lut[256][3];
    gt4_ray_hit license_surfaces[3];
    gt4_license_motion *license_motion;
    license_scene *license_scene;
    gt4_license_concourse concourse;
    gt4_license_fade license_fade;
    gt4_driver_car license_car;
    float license_origin[4],license_people[3][4];
    unsigned license_camera_cycle;
    race_pause *pause;     /* the pause menu while the race is held */
    int quit;              /* the pause menu chose to leave */
    int replay_back_held;SDL_JoystickID pad_id;
    int pause_keys[6];     /* the pad's Start, D-pad and A held, for their edges */
    int confirm_held;
    char names[6][96];     /* each car's name (CAR_NAME, the StrDB's) */
    gt4_motion_bank *start_cam;  /* the course's start camera motions (header +0xD0) */
    int start_cam_tried, start_cam_on;
    gt4_license_motion *license_start_motion;float license_start_origin[4];
    int license_start_left,license_start_tried;
    gt4_pregrid *pregrid;
    gt4_tvcam *tvcam;      /* the course's TV cameras (REP4 +0xB0) */
    gt4_dive *dive;        /* the replay's Dive mode on the course's camera points */
    int dive_tried, dive_had;
    float dive_prev[3];
    float cue; int cue_passed;   /* the music's marks (race_audio_music_cue) */
    int tv_on, tv_tried;
    double pregrid_acc;
    int pregrid_tried;
    gt4_license_motion *license_menu_motion;
    unsigned license_menu_ticks;
    uint8_t *license_menu_tape;
    size_t license_menu_steps,license_menu_at;
    int license_menu_playing;
    gt4_race_desc license_menu_desc;
    gt4_license_menu_cycle license_menu_cycle;
    unsigned license_menu_cycles;
    float license_menu_origin[4],license_menu_pos[3],license_menu_rot[9],license_menu_wheels[4][3];
    float license_menu_distance;
    int inspect_on,inspect_seeded;
    float inspect_eye[3],inspect_yaw,inspect_pitch,inspect_projection[3];
    double film_clock; /* P: opt-in PC filming rig; never a physics clock */
    struct {
        int on,seeded,mode,target,aim,clean,paused,was_held,aperture,focus_auto;
        float eye[3],rows[9],projection[3],anchor[3],mount[3];
        float original_eye[3],original_rows[9],original_projection[3];
        int original_visible;
        float yaw,pitch,roll,velocity[3],focus,mouse[2];
        plat_texture *sample,*image;
    } cinema;
    /* The replay (func_003F59A0, the session's method 0xC8): the command
       read for the player's car each step goes to the world's recorder
       (+0x240, func_00426C50); the replay runs the same race again with
       those commands. The physics being the same step for step, the race
       comes out the same. */
    uint8_t *tape;         /* 0x20 bytes a step */
    uint8_t thumb[GT4_REPLAY_THUMB_W * GT4_REPLAY_THUMB_H * 3];   /* the race's picture for a saved replay */
    int thumb_on;
    size_t tape_n, tape_cap, tape_at;
    uint8_t *tape_2p;size_t tape_n_2p,tape_cap_2p,tape_at_2p;
    unsigned license_test_frame; /* PC test-controller clock, not session state */
    gt4_logger_point *license_log_points;
    gt4_logger_sampler license_log;
    int license_log_collecting;
    /* the Logger's track B (entry 1: the ghost or the guide) and each
       entry's time when the collection ended */
    gt4_logger_point *log_b_points;
    char logger_loaded_car[2][96];int logger_loaded_power[2];
    gt4_logger_sampler log_b;
    uint32_t log_time[2];
    int log_analyzing;     /* race_game_logger_analyze: its replay samples and has the ghost */
    size_t log_steps;      /* the analysis' steps so far */
    int photo_held;       /* Photo Drive retains the entire world between shots */
    double license_result_test_clock;unsigned license_result_test_sent;
    int replaying, replay_next;
    gt4_disc_replay *disc_replays;
    unsigned disc_segments,disc_segment,disc_segments_2p,disc_segment_2p;
    size_t disc_segment_at_2p[3];uint8_t disc_states_2p[3][0x14f4];
    size_t disc_segment_at[3];
    uint8_t disc_states[3][0x14f4];
    int demonstrating;
    struct { uint8_t *tape;size_t n,cap;int thumb_on;gt4_logger_sampler log; } before_demo;
    int held;              /* the replay held (func_003C3B08's message): Select */
    /* the replay's controls (PCSX2, the user's map): Square the mode
       (Normal -> Dive -> Display, labels ReplayModeNormal/Dive/
       NormalDisplay), Circle the music (ReplayModeBGMOn/Off), Cross the
       car's camera, L3 the fast play (ReplayModeAccelerate/NormalSpeed),
       up/down the car watched */
    int rmode;             /* 0 Normal, 1 Dive, 2 Display (the race's display) */
    int bgm_off, fast, car_cam, focus;
    float zoom;            /* Triangle: the TV camera's lens times 4 (ShotManager +0x14C: 4.0 in rv_01, 1.0 elsewhere) */
    int zoom_on;           /* Triangle's flag (the view's +0x1E) */
    int start_events;      /* the start events (4) of the watched car seen */
    float zoom_spring[3];  /* 003852A0's spring: 1 to 4 over some frames */
    const char *label;     /* the plate's rtext key, shown for a while */
    double label_t;        /* its time left on (RaceReplayModeDisplay +0x54's envelope) */
    float label_v;         /* and its light */
    int label_out;
    int pad_held[8];
    long replay_open;      /* the opening's clock at the tape's start (frames), -1 unset */
    double replay_end;     /* seconds since the tape ran out */
    /* the last race's lineup, for its replay */
    char last_course[32], last_car[96], last_others[5][96];
    const char *last_other_ptr[5];
    int last_color, last_colors[5], last_nothers, last_has_colors;
    int last_prepared;
    gt4_race_desc last_desc;
    uint8_t last_owned[6][0x4b8],last_event_row[151];
    uint32_t last_rng;
    int ghost_car;         /* the Time Trial's: car 1 drawn while it drives */
    race_smoke *smoke;     /* the tyres' smoke (the world's +0x31D0) */
    race_leaves *leaves;   /* the leaves the wheels throw up (the world's +0x3238) */
    /* The Time Trial's ghost kept for the session (the PS2 keeps the best
       lap's recording while the Time Trial goes on, and its best-lap
       replay can be saved and loaded): the course and car it was driven
       with, the car at the lap's start, the start's remaining thirds and
       step fraction, its time and commands */
    struct { int on; char course[32], car[96]; uint8_t state[GT4_GHOST_CAR]; uint32_t rest, lap; float frac; uint8_t *tape; size_t steps; } ghost;
    int power_unit;        /* the unit_power option (PS HP BHP CH kW CV PF) */
    /* the replay's opening (race_hud_draw_opening): the player's car as
       the race took it */
    int open_on, open_front, open_rear;
    float open_power;
};

/* a licence examination or training (desc.license 1, 2); a driving
   mission (3) races with the common display and results */
#define license_exam(g) ((g)->last_desc.license==1 || (g)->last_desc.license==2)
int race_game_mission_active(const race_game *g){return g && g->run && g->last_prepared && g->last_desc.license==3;}

/* the controller's command for the player's car, as the game reads it */
typedef struct { uint8_t flags, device, special; int8_t shift; int16_t steer, throttle, brake, reverse, handbrake; } race_command;
static race_command command,command_2p;
static uint32_t player,player_2p;

static race_game *playing;   /* the race whose player reads the controller */
static int host_read(void *u, uint32_t car, uint8_t *cmd, const uint8_t *ev)
{
    race_game *g = playing;
    (void)u;
    if (car != player && (!g || !g->split || car!=player_2p)) return 0;
    const race_command *input=car==player?&command:&command_2p;
    if(g && g->license_menu_playing){
        if(g->license_menu_at<g->license_menu_steps)memcpy(cmd,g->license_menu_tape+32*g->license_menu_at++,32);
        else memset(cmd,0,32);
        return 1; /* 003EAAC0: teacher recorder in Scene mode 2, no player recording. */
    }
    if (g && g->replaying) {
        size_t *at=car==player?&g->tape_at:&g->tape_at_2p;
        size_t n=car==player?g->tape_n:g->tape_n_2p;
        const uint8_t *tape=car==player?g->tape:g->tape_2p;
        if (*at < n) memcpy(cmd, tape + (*at)++ * 0x20, 0x20);
        else { memset(cmd, 0, 0x20); cmd[1] = 1; cmd[3] = ev[3]; }
        return 1;
    }
    cmd[0] = input->flags;
    cmd[1] = input->device;         /* 1 digital pad, 2 analog pad */
    cmd[2] = (uint8_t)input->shift;
    cmd[3] = ev[3];
    memcpy(cmd + 4, &input->steer, 2);
    memcpy(cmd + 8, &input->throttle, 2);
    memcpy(cmd + 0xa, &input->brake, 2);
    /* 00349F48/00348EE8: +C reverse throttle, +7 nitrous request. */
    memcpy(cmd + 0xc, &input->reverse, 2);
    cmd[7]=input->special;
    memcpy(cmd + 0xe, &input->handbrake, 2);
    /* 00426D50: the event block's orders (ev+0x10, ev[3] bytes: the pit's
       and, for a car of kind 0/1, the B-Spec driver's at +0x14/+0x15) */
    memcpy(cmd + 0x10, ev + 0x10, ev[3] <= 0x10 ? ev[3] : 0x10);
    /* Test-only pad tape; normal session rules determine the outcome.
       The recorded bytes never write car state or solver parameters. */
    if(g && g->mission_test_pad){
        if(fread(cmd,1,32,g->mission_test_pad)!=32){memset(cmd,0,32);cmd[1]=1;cmd[3]=ev[3];}
    }
    /* the recording ends with the player's finish (func_003BA260's state 4
       -> func_003BA3E8: the recorders closed, func_00346498 sets +0x38 and
       func_00346138 records no more) */
    if (g && (g->finished < 0 || (g->split && !GT4_U8(player_2p+0x5b6)))) {
        /* 00426C50: one recorder per human entry. Both clocks keep
           advancing until the two-player result, including neutral inputs. */
        uint8_t **tape=car==player?&g->tape:&g->tape_2p;
        size_t *n=car==player?&g->tape_n:&g->tape_n_2p,*capacity=car==player?&g->tape_cap:&g->tape_cap_2p;
        if (*n == *capacity) {
            size_t cap = *capacity ? *capacity * 2 : 60 * 60 * 4;
            uint8_t *t = realloc(*tape, cap * 0x20);
            if (!t) return 1;
            *tape = t;
            *capacity = cap;
        }
        memcpy(*tape + (*n)++ * 0x20, cmd, 0x20);
    }
    return 1;
}

static int host_wheel(void *u, uint32_t car, int16_t state[2])
{
    (void)u;
    (void)state;
    return car == player || (playing && playing->split && car==player_2p);
}

static SDL_GameController *rumble_pad;
static int host_write(void *u, uint32_t car, const uint8_t *out)
{
    (void)u;
    int port=car==player?0:playing && playing->split && car==player_2p?1:-1;
    if(port<0)return 0;
    /* 00353EE8 writes out[2], 0x80 centred. Publish only that byte;
       the original force calculation and steering response are untouched. */
    int force_on=playing && !playing->pause && !playing->photo_held && !playing->replaying;
    plat_input_set_ffb(port,force_on?(128.f-out[2])/127.f:0.f);
    rumble_pad=menu_option_number(NULL,port?"vibration_2p":"vibration_1p",1)?plat_sdl_pad(port):NULL;
    if (rumble_pad) SDL_GameControllerRumble(rumble_pad, (uint16_t)(out[1] > 127 ? 65535 : out[1] * 516), (uint16_t)(out[0] ? 32768 : 0), 100);
    return 1;
}

race_game *race_game_open(const char *disc)
{
    race_game *g = calloc(1, sizeof *g);
    if (!g) return NULL;
    g->disc = malloc(strlen(disc) + 1);
    if (g->disc) strcpy(g->disc, disc);
    g->view = race_view_open(disc);
    if (!g->disc || !g->view) {
        race_game_close(g);
        return NULL;
    }
    SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER);
    {
        const char *err = NULL;
        g->vol = gt4_vol_open(disc, &err);
    }
    g->unit = 1;   /* the option unit_velocity's default (MPH) */
    g->power_unit = 1;   /* unit_power: HP */
    g->camera = 1;
    g->bgm_enabled=1;
    g->pit_mode_allowed=1; /* 003BFB90 +50: race options +970 default. */
    g->bgm_volume = g->se_volume = 120; /* menu option defaults */
    return g;
}

/* the run's best lap kept as the session's ghost, when it is better */
static void keep_ghost(race_game *g)
{
    const uint8_t *state, *tape;
    uint32_t rest, lap;
    float frac;
    size_t steps;
    uint8_t *copy;
    if (!g->run || !g->time_trial || g->replaying || !gt4_race_run_ghost_get(g->run, &state, &rest, &frac, &lap, &tape, &steps)) return;
    if (g->ghost.on && !strcmp(g->ghost.course, g->last_course) && !strcmp(g->ghost.car, g->last_car) && g->ghost.lap <= lap) return;
    if (!(copy = malloc(steps * 0x20))) return;
    memcpy(copy, tape, steps * 0x20);
    free(g->ghost.tape);
    g->ghost.tape = copy;
    g->ghost.steps = steps;
    memcpy(g->ghost.state, state, GT4_GHOST_CAR);
    g->ghost.rest = rest;
    g->ghost.frac = frac;
    g->ghost.lap = lap;
    snprintf(g->ghost.course, sizeof g->ghost.course, "%s", g->last_course);
    snprintf(g->ghost.car, sizeof g->ghost.car, "%s", g->last_car);
    g->ghost.on = 1;
}

int race_game_has_ghost(const race_game *g) { return g && g->ghost.on; }

/* the session's ghost as a best-lap replay (labomode's MODE_SAVE_BESTLAP_REPLAY),
   its commands and car lent */
int race_game_ghost_export(race_game *g, gt4_replay_file *r)
{
    if (!g || !r) return 0;
    keep_ghost(g);
    if (!g->ghost.on) return 0;
    memset(r, 0, sizeof *r);
    r->kind = GT4_REPLAY_BEST_LAP;
    snprintf(r->course, sizeof r->course, "%s", g->ghost.course);
    snprintf(r->car, sizeof r->car, "%s", g->ghost.car);
    r->color = g->last_color;
    r->laps = 1;
    r->tire = g->tire;
    r->manual = g->manual;
    r->assist_asm = g->assist_asm;
    r->assist_tcs = g->assist_tcs;
    r->time_trial = 1;
    r->best_lap = r->race_time = g->ghost.lap;
    r->tape = g->ghost.tape;
    r->steps = g->ghost.steps;
    r->ghost_car = g->ghost.state;
    r->ghost_rest = g->ghost.rest;
    r->ghost_frac = g->ghost.frac;
    return 1;
}

/* a saved best lap as the session's ghost: only for the course and car it
   was driven with (the car as it was is copied into the race whole) */
int race_game_ghost_import(race_game *g, const gt4_replay_file *r)
{
    uint8_t *copy;
    if (!g || !r || r->kind != GT4_REPLAY_BEST_LAP || !r->ghost_car || !r->steps
        || strcmp(r->course, g->last_course) || strcmp(r->car, g->last_car) || !(copy = malloc(r->steps * 0x20))) return 0;
    memcpy(copy, r->tape, r->steps * 0x20);
    free(g->ghost.tape);
    g->ghost.tape = copy;
    g->ghost.steps = r->steps;
    memcpy(g->ghost.state, r->ghost_car, GT4_GHOST_CAR);
    g->ghost.rest = r->ghost_rest;
    g->ghost.frac = r->ghost_frac;
    g->ghost.lap = r->best_lap;
    snprintf(g->ghost.course, sizeof g->ghost.course, "%s", r->course);
    snprintf(g->ghost.car, sizeof g->ghost.car, "%s", r->car);
    g->ghost.on = 1;
    return 1;
}

void race_game_stop(race_game *g)
{
    if (!g) return;
    if(g->cinema.on)race_game_cinematic_toggle(g);
    plat_texture_destroy(g->cinema.sample);plat_texture_destroy(g->cinema.image);
    memset(&g->cinema,0,sizeof g->cinema);
    plat_input_set_ffb(0,0);plat_input_set_ffb(1,0);
    if(g->mission_test_pad){fclose(g->mission_test_pad);g->mission_test_pad=NULL;}
    g->photo_held=0;
    g->mission_presentation=GT4_MISSION_RUNNING;
    if(g->split || g->hud_2p){
        race_hud_close(g->hud);g->hud=NULL;
        race_hud_close(g->hud_2p);g->hud_2p=NULL;
    }
    if(g->license_motion)g->license_camera_cycle=(unsigned)g->concourse.camera_cycle;
    license_scene_close(g->license_scene);g->license_scene=NULL;
    for(unsigned i=0;i<6;i++)race_view_set_pit_crew(g->view,i,NULL);
    license_scene_close(g->pit_scene);g->pit_scene=NULL;
    for(int i=5;i>=0;i--){gt4_license_motion_close(g->pit_motion[i]);g->pit_motion[i]=NULL;}
    memset(g->pit_crew,0,sizeof g->pit_crew);
    gt4_license_motion_close(g->license_motion);g->license_motion=NULL;
    memset(&g->concourse,0,sizeof g->concourse);
    keep_ghost(g);
    race_audio_close(g->audio);g->audio=NULL;
    race_bspec_close(g->bspec_ui);g->bspec_ui=NULL;
    race_pitmenu_close(g->pitmenu);g->pitmenu=NULL;
    if (g->loading)                               /* the loader finishes, what it made dropped */
        while (race_view_poll(g->view, 1) == 0) SDL_Delay(5);
    gt4_pregrid_close(g->pregrid);g->pregrid=NULL;g->pregrid_acc=0;g->pregrid_tried=0;
    free(g->license_menu_tape);g->license_menu_tape=NULL;
    g->license_menu_playing=0;g->license_menu_at=g->license_menu_steps=0;
    gt4_license_motion_close(g->license_menu_motion);g->license_menu_motion=NULL;g->license_menu_ticks=0;
    gt4_tvcam_close(g->tvcam);g->tvcam=NULL;g->tv_tried=0;
    gt4_dive_close(g->dive);g->dive=NULL;g->dive_tried=g->dive_had=0;
    race_view_clear(g->view);
    race_result_close(g->result);
    race_pause_close(g->pause);
    g->pause = NULL;g->quit = 0;
    g->result = NULL;
    for(int i=0;i<3;i++){r3d_mesh_destroy(g->trophy[i]);g->trophy[i]=NULL;}g->trophy_tried=0;
    gt4_license_motion_close(g->license_start_motion);g->license_start_motion=NULL;
    g->license_start_left=0;g->license_start_tried=0;
    gt4_motion_bank_close(g->start_cam);
    g->start_cam = NULL;
    g->start_cam_tried = g->start_cam_on = 0;
    g->inspect_on=g->inspect_seeded=0;g->film_clock=0;
    gt4_race_run_close(g->run);
    g->run = NULL;
    g->loading = g->ready = 0;
    g->ghost_car = 0;
    if (gt4_wheel_smoke_user == g) { gt4_wheel_smoke = NULL; gt4_wheel_leaves = NULL; gt4_wheel_smoke_user = NULL; }
    race_view_set_smoke(g->view, NULL);
    race_smoke_close(g->smoke);
    g->smoke = NULL;
    race_view_set_leaves(g->view, NULL);
    race_leaves_close(g->leaves);
    g->leaves = NULL;
    gt4_phys_pad.read = NULL;
    gt4_phys_pad.wheel_state = NULL;
    gt4_phys_pad.write = NULL;
}

void race_game_close(race_game *g)
{
    if (g) {free(g->tape);free(g->tape_2p);}
    if (playing == g) playing = NULL;
    if (!g) return;
    free(g->before_demo.tape);
    gt4_disc_replay_close(g->disc_replays);
    race_game_stop(g);
    race_view_close(g->view);
    race_hud_close(g->hud);race_hud_close(g->hud_2p);
    gt4_vol_close(g->vol);
    g->pad=NULL; /* platform owns controller handles, including hotplug */
    free(g->ghost.tape);
    free(g->disc);
    free(g->license_log_points);
    free(g->log_b_points);
    plat_texture_destroy(g->license_screen);
    free(g);
}

static uint32_t license_place_table(race_game *g,unsigned *count)
{
    /* 00395338/00395380: active RNW4 variant, 32-byte record,
       count +0xe, 16-byte authored positions at +0x10. */
    uint32_t car=gt4_race_run_car(g->run,0),course=GT4_U32(car+8),rw=GT4_U32(GT4_U32(course+4)+0xdc);
    int variant=(int32_t)GT4_U32(course+0xb0);*count=0;
    if(variant<0 || variant>=GT4_U16(rw+0x54))return 0;
    uint32_t row=GT4_U32(rw+0x9c)+(unsigned)variant*32;*count=GT4_U16(row+14);return GT4_U32(row+16);
}
static int license_ground(uint32_t course,const float pos[3],float *y,gt4_ray_hit *surface)
{
    /* 00395930 +0x44..0x78: authored y + 1; 00409A48 +0x88..0xc4
       solves the plane, then 00395760 queries ground context from y + 5.
       Query failures keep normal (0,1,0), as 00407EF0 +0x8c..0xc8. */
    gt4_ray_hit hit;float query[3]={pos[0],pos[1]+1,pos[2]};
    uint32_t mesh=GT4_U32(GT4_U32(course+4)+0xdc);
    if(surface){memset(surface,0,sizeof *surface);surface->normal[1]=1;surface->u=1;surface->leaf=-1;}
    if(!gt4_course_ray_down(mesh,&hit,query))return 0;
    *y=hit.normal[1]? -((hit.normal[0]*pos[0]+hit.normal[2]*pos[2])+hit.plane)/hit.normal[1]:0;
    if(surface){query[1]=*y+5;if(gt4_course_ray_down(mesh,&hit,query))*surface=hit;}
    return 1;
}
static void license_place(race_game *g)
{
    unsigned count;uint32_t table=license_place_table(g,&count);
    if(!table || (unsigned)g->concourse.placement>=count)return;
    uint32_t car=gt4_race_run_car(g->run,0),params=GT4_U32(car+16),layout=gt4_car_layout(car),course=GT4_U32(car+8);
    fenv_t env;fegetenv(&env);gt4_phys_fpu_enter();
    memcpy(g->license_origin,gt4_ptr(table+g->concourse.placement*16),16);
    /* 00408DB8 +0xa4: the original uses 3.1400001, not M_PI. */
    g->license_origin[3]+=gt4_f32(0x4048f5c2);
    float y;if(license_ground(course,g->license_origin,&y,NULL))g->license_origin[1]=y;
    float wheels[2][2];for(unsigned i=0;i<2;i++){
        float blend=GT4_F32(car+0x554);
        wheels[i][0]=GT4_F32(car+0x900+i*4)*blend+GT4_F32(car+0x16c+i*0xec)*(1-blend); /* 00344AA8 */
        int front=(int8_t)GT4_U8(layout+0x3c+i);float half=GT4_F32(params+0x18)*.5f;
        wheels[i][1]=front>0?-half:front<0?half:0; /* 00344C08 */
    }
    float local[3][4];gt4_license_concourse_people(g->license_car.seat[0]+GT4_F32(0x8464c0),g->license_car.seat[2]+GT4_F32(0x8464bc),g->license_car.shift[0],wheels,local);
    float sn=sinf(g->license_origin[3]),cs=cosf(g->license_origin[3]);
    for(unsigned i=0;i<3;i++){
        g->license_people[i][0]=(cs*local[i][0]+sn*local[i][2])+g->license_origin[0];
        g->license_people[i][1]=g->license_origin[1];
        g->license_people[i][2]=(-sn*local[i][0]+cs*local[i][2])+g->license_origin[2];
        g->license_people[i][3]=local[i][3]+g->license_origin[3];
        if(license_ground(course,g->license_people[i],&y,g->license_surfaces+i))g->license_people[i][1]=y;
    }fesetenv(&env);
}
static void license_begin(race_game *g,const char *car,int cars)
{
    unsigned count;license_place_table(g,&count);gt4_license_concourse_begin(&g->concourse,(int)count,g->license_camera_cycle);
    gt4_license_fade_begin(&g->license_fade);
    g->license_saturation=1;g->license_image_valid=g->license_image_active=0;
    if(!g->concourse.enabled)return;
    car_model_tables *tables=car_model_tables_open(g->vol);
    int metadata=car_model_race_driver(g->vol,tables,car,cars,&g->license_car);car_model_tables_close(tables);
    size_t size;uint8_t *data=gt4_vol_read(g->vol,"/pit/license_001",&size);
    if(data && metadata)g->license_motion=gt4_license_motion_open(data,size);free(data);
    if(!g->license_motion){fprintf(stderr,"licence scene: cannot load package/car geometry\n");g->concourse.enabled=0;return;}
    g->license_scene=license_scene_open(g->license_motion);
    for(unsigned i=0;i<3;i++)gt4_license_motion_select(g->license_motion,i,gt4_license_concourse_actor(&g->concourse,i));
    if(g->concourse.placement_ready)license_place(g);
}
static void license_step(race_game *g)
{
    if(!g->license_motion || g->demonstrating)return;
    /* 00409168: actor/camera clocks first, director transitions after. */
    for(unsigned i=0;i<3;i++)gt4_license_motion_step(g->license_motion,i,(float)STEP);
    if(gt4_license_concourse_has_camera(&g->concourse))gt4_license_motion_step(g->license_motion,3,(float)STEP);
    int32_t state;uint32_t time;if(race_game_license_result(g,&state,&time) && state!=-2)gt4_license_concourse_finish(&g->concourse,state>0);
    gt4_license_concourse_step(&g->concourse,(float)STEP);
    gt4_license_fade_step(&g->license_fade,g->concourse.enabled && g->concourse.outcome,
        g->concourse.previous,g->concourse.phase,(float)STEP);
    if(g->concourse.changed&GT4_CONCOURSE_ACTORS)for(unsigned i=0;i<3;i++)gt4_license_motion_select(g->license_motion,i,gt4_license_concourse_actor(&g->concourse,i));
    if(g->concourse.changed&GT4_CONCOURSE_CAMERA)gt4_license_motion_select(g->license_motion,3,gt4_license_concourse_camera(&g->concourse));
    if(g->concourse.changed&GT4_CONCOURSE_PLACE)license_place(g);
    if(gt4_license_concourse_cut(&g->concourse)){
        /* 003FAB10 presentation handover, using existing native car APIs;
           ownership of core/physics is unchanged. */
        uint32_t car=gt4_race_run_car(g->run,0),cond=GT4_U32(car+4),b=car+0x104,v=cond+0xcab8+GT4_U8(car+0x544)*48;
        fenv_t env;fegetenv(&env);gt4_phys_fpu_enter();
        gt4_car_place(car,0,g->license_origin[0],-g->license_origin[2],g->license_origin[1],g->license_origin[3]);
        GT4_U32(b+0x5fc)=GT4_U32(b+0x600)=0;GT4_F32(v)=GT4_F32(b+0x570);GT4_U32(v+4)=GT4_U32(v+8)=0;GT4_F32(v+12)=GT4_F32(b+0x568);
        for(unsigned o=0x10;o<=0x2c;o+=4)GT4_U32(v+o)=0; /* 003455F0 */
        gt4_car_rest(car);gt4_walls_in(cond);gt4_walls_setup(cond);
        if(GT4_U8(car+0x56a)!=4)GT4_U8(car+0x56b)=8; /* 003455C8 */
        fesetenv(&env);
        fprintf(stderr,"licence scene: phase %d, %s, authored place %.6f %.6f %.6f\n",g->concourse.phase,gt4_license_concourse_camera(&g->concourse),g->license_origin[0],g->license_origin[1],g->license_origin[2]);
    }
}
static int license_look(race_game *g,const float look[9],float fov,const float origin[4])
{
    /* 004094C8 +0xb8..0x1a0 rotates eye/target/up by root heading;
       only eye and target receive the authored world translation. */
    float sn=sinf(origin[3]),cs=cosf(origin[3]),world[9];
    for(unsigned i=0;i<3;i++){world[i*3]=cs*look[i*3]+sn*look[i*3+2];world[i*3+1]=look[i*3+1];world[i*3+2]=-sn*look[i*3]+cs*look[i*3+2];if(i<2)for(unsigned k=0;k<3;k++)world[i*3+k]+=origin[k];}
    float back[3],right[3],up[3],rows[9];float norm=0;
    for(unsigned k=0;k<3;k++){back[k]=world[k]-world[3+k];norm+=back[k]*back[k];}if(norm<=0)return 0;norm=sqrtf(norm);for(unsigned k=0;k<3;k++)back[k]/=norm;
    right[0]=world[7]*back[2]-world[8]*back[1];right[1]=world[8]*back[0]-world[6]*back[2];right[2]=world[6]*back[1]-world[7]*back[0];
    norm=sqrtf(right[0]*right[0]+right[1]*right[1]+right[2]*right[2]);if(norm<=0)return 0;for(unsigned k=0;k<3;k++)right[k]/=norm;
    up[0]=back[1]*right[2]-back[2]*right[1];up[1]=back[2]*right[0]-back[0]*right[2];up[2]=back[0]*right[1]-back[1]*right[0];
    memcpy(rows,right,12);memcpy(rows+3,up,12);memcpy(rows+6,back,12);
    float sx,sy;gt4_license_camera_projection(fov,&sx,&sy);
    /* C: lens coefficients from 0037DC78/003794A8. A: native viewport
       and inherited .1 near plane; complete PS2 raster proof pending. */
    race_view_set_camera(g->view,world,rows,sx,sy,.1f);race_view_show_car(g->view,1);return 1;
}
static int license_camera(race_game *g)
{
    if(!g->license_motion || !gt4_license_concourse_has_camera(&g->concourse))return 0;
    float look[9],fov,camera_coord,target_coord;if(!gt4_license_motion_camera(g->license_motion,look,&fov,&camera_coord,&target_coord))return 0;
    return license_look(g,look,fov,g->license_origin);
}

static int race_game_start_impl(race_game *g, const char *course, const char *car, int color,
                    const char *const *others, const int *colors, int nothers,
                    const gt4_race_desc *prepared,char *msg, size_t len)
{
    gt4_race_desc d;
    const char *err = NULL;
    gt4_vol *vol;
    uint8_t *labels;
    size_t n = 0;
    char prepared_labels[6][96];const char *prepared_others[5];int prepared_colors[5];
    memset(&d, 0, sizeof d);
    msg[0] = 0;
    if (!g) return 0;
    race_game_stop(g);
    g->finished=-1;
    if (nothers > 5) nothers = 5;
    vol = gt4_vol_open(g->disc, &err);
    labels = vol ? gt4_vol_read(vol, "/specdb/GT4_US2560/GENERIC_CAR.idi", &n) : NULL;
    if(prepared){
        d=*prepared;course=d.course;nothers=d.cars-1;
        for(int i=0;i<d.cars;i++){
            const char *label=labels?gt4_specdb_label_for_id(labels,n,d.car[i]):NULL;
            if(!label || strlen(label)>=sizeof prepared_labels[i]){
                snprintf(msg,len,"unknown prepared car %u",d.car[i]);
                free(labels);gt4_vol_close(vol);return 0;
            }
            snprintf(prepared_labels[i],sizeof prepared_labels[i],"%s",label);
            if(i)prepared_others[i-1]=prepared_labels[i];
        }
        car=prepared_labels[0];others=prepared_others;
    }
    if(!prepared)snprintf(d.course, sizeof d.course, "%s", course);
    snprintf(g->hud_course, sizeof g->hud_course, "%s", course);
    if(!prepared){
    d.laps = getenv("OPENGT4_RACE_LAPS") ? atoi(getenv("OPENGT4_RACE_LAPS")) : g->laps;   /* tests: a shorter race */
    d.tire = g->tire;
    d.time_trial = g->time_trial;
    d.world_mode = g->world_mode;
    if(g->arcade_owned_on)d.owned_car[0]=g->arcade_owned;
    if(g->split){
        d.split_battle=1;d.tire_2p=g->split_options.tire_2p;
        d.manual_2p=g->split_options.manual_2p;d.assist_asm_2p=g->split_options.assist_asm_2p;
        d.assist_tcs_2p=g->split_options.assist_tcs_2p;
        d.split_boost=g->split_options.split_boost;d.split_handicap=g->split_options.split_handicap;
        d.split_tire_damage=g->split_options.split_tire_damage;
        for(int i=0;i<2;i++)d.owned_car[i]=g->split_options.owned_car[i];
        d.time_trial=d.bspec=0;
    }
    if (getenv("OPENGT4_RACE_BSPEC")) g->bspec = 1;   /* tests: an Arcade race watched in B-Spec */
    d.bspec = g->bspec;
    d.manual = g->manual;
    d.assist_asm = g->assist_asm;
    d.assist_tcs = g->assist_tcs;
    d.quick_tuned = g->quick_tuned;
    d.quick_power = g->quick_power;
    d.quick_weight = g->quick_weight;
    d.quick_front_tire = g->quick_front_tire;
    d.quick_rear_tire = g->quick_rear_tire;
    d.quick_gear = g->quick_gear;
    g->finished = -1;
    for (int i = 0; i <= nothers; i++) {
        const char *label = i ? others[i - 1] : car;
        if (!labels || !gt4_specdb_label(labels, n, label, &d.car[d.cars])) {
            snprintf(msg, len, "unknown car %s", label);
            free(labels);
            gt4_vol_close(vol);
            return 0;
        }
        d.cars++;
    }
    }
    free(labels);
    {   /* the names the results show: CAR_NAME's first word, a StrDB string */
        size_t nn = 0, ns = 0;
        uint8_t *names = vol ? gt4_vol_read(vol, "/specdb/GT4_US2560/CAR_NAME_american.dbt", &nn) : NULL;
        uint8_t *strs = vol ? gt4_vol_read(vol, "/specdb/GT4_US2560/american_StrDB.sdb", &ns) : NULL;
        for (int i = 0; i < 6; i++) {
            uint8_t row[256];
            const char *s = NULL;
            if (i < d.cars && names && strs && gt4_specdb_row(names, nn, d.car[i], row, sizeof row) >= 4)
                s = gt4_specdb_string(strs, ns, (uint32_t)row[0] | (uint32_t)row[1] << 8 | (uint32_t)row[2] << 16 | (uint32_t)row[3] << 24);
            snprintf(g->names[i], sizeof g->names[i], "%s", s ? s : "");
        }
        free(names);
        free(strs);
    }
    gt4_vol_close(vol);
    for (int i = 0; i < 6; i++) g->ids[i] = i < d.cars ? d.car[i] : 0;
    d.pit_menu = !d.license;   /* every race world but the licences' makes the pit menu (003DCA28) */
    if(d.split_battle)d.bspec=0;
    g->split=d.split_battle;
    g->run = gt4_race_run_open(g->disc, &d, msg, len);
    if (!g->run) return 0;
    player = gt4_race_run_car(g->run, 0);
    player_2p=g->split?gt4_race_run_car(g->run,1):0;
    memset(&command,0,sizeof command);memset(&command_2p,0,sizeof command_2p);
    const char *mission_pad=getenv("OPENGT4_MISSION_TEST_PAD");
    if(mission_pad && prepared && prepared->license==3 && !g->replaying)g->mission_test_pad=fopen(mission_pad,"rb");
    memset(g->split_camera_valid,0,sizeof g->split_camera_valid);
    g->camera_2p=1;g->view_held_2p=g->look_back_2p=g->retry_requested=0;
    memset(g->pause_keys_2p,0,sizeof g->pause_keys_2p);
    if(d.license && !g->demonstrating)license_begin(g,car,d.cars);
    if(!g->replay_next && !g->log_analyzing)memset(g->logger_loaded_car,0,sizeof g->logger_loaded_car);
    if(!g->replay_next || g->log_analyzing){g->license_log_collecting=0;g->license_log.count=0;g->log_b.count=0;g->log_time[0]=g->log_time[1]=0;}
    if((d.license || d.time_trial) && !g->demonstrating && (!g->replay_next || g->log_analyzing)){
        /* 00122FA8 +0x9c..0x100 derives sample spacing from the runway and
           samples entries 0 and 1 (entry 0 again when there is no 1).
           A boundary: collect live physics instead of reconstructing replay.
           An absent demonstration stays absent. */
        if(!g->license_log_points)g->license_log_points=calloc(GT4_LOGGER_POINTS,sizeof *g->license_log_points);
        if(!g->log_b_points)g->log_b_points=calloc(GT4_LOGGER_POINTS,sizeof *g->log_b_points);
        if(!g->license_log_points || !g->log_b_points){snprintf(msg,len,"cannot allocate licence telemetry");race_game_stop(g);return 0;}
        uint32_t r=GT4_U32(gt4_race_run_conductor(g->run)),c=GT4_U32(r+0x80),cd=GT4_U32(c+4),rw=GT4_U32(cd+0xdc);
        fenv_t env;fegetenv(&env);gt4_phys_fpu_enter();float length=GT4_F32(rw+0x14);
        gt4_logger_begin(&g->license_log,g->license_log_points,gt4_logger_spacing(length),length);
        gt4_logger_begin(&g->log_b,g->log_b_points,gt4_logger_spacing(length),length);fesetenv(&env);
        g->license_log_collecting=1;
    }
    /* RaceTraining's second entry is the visible guide (003ECB68),
       while RaceLicense clones a ghost (003BDC88). */
    g->ghost_car = (d.time_trial || d.license==1) && gt4_race_run_cars(g->run) == 2;
    if (g->ghost_car && (!g->replay_next || g->log_analyzing) && g->ghost.on && !strcmp(g->ghost.course, course) && !strcmp(g->ghost.car, car))
        gt4_race_run_ghost_set(g->run, g->ghost.state, g->ghost.rest, g->ghost.frac, g->ghost.lap, g->ghost.tape, g->ghost.steps);
    if(prepared || (d.owned_car[0] || d.owned_car[1])){
        /* 003BB5A8/0040AA48 finalize paints before presentation; models
           must use entry garage +0x18, not the raw RACES paint qword. */
        uint32_t race=GT4_U32(gt4_race_run_conductor(g->run)),entries=GT4_U32(race+0x60);
        for(int i=0;i<d.cars;i++){
            if(!prepared && !d.owned_car[i]){if(i)prepared_colors[i-1]=colors?colors[i-1]:0;continue;}
            uint32_t entry=GT4_U32(GT4_U32(entries+8)+i*4);
            int paint=(int8_t)GT4_U8(entry+0x38);
            if(i)prepared_colors[i-1]=paint;else color=paint;
            d.event_paint[i]=(uint64_t)(int64_t)(paint+1);
        }
        colors=prepared_colors;
    }
    /* tests: a PCSX2 capture's moment, frozen (render comparisons) */
    if (getenv("OPENGT4_RACE_SNAPSHOT")) {
        static char snap[6][48];
        static const char *snap_cars[6];
        uint8_t *idi;
        size_t ni = 0;
        if (!gt4_race_run_restore(g->run, getenv("OPENGT4_RACE_SNAPSHOT")))
            fprintf(stderr, "race: cannot read the capture %s\n", getenv("OPENGT4_RACE_SNAPSHOT"));
        /* the captured lineup (race+0x60: the entries, each garage's car key at +0x20) */
        vol = gt4_vol_open(g->disc, &err);
        idi = vol ? gt4_vol_read(vol, "/specdb/GT4_US2560/GENERIC_CAR.idi", &ni) : NULL;
        for (int i = 0; i < 6 && i <= nothers; i++) {
            uint32_t race = GT4_U32(gt4_race_run_conductor(g->run)), ents = GT4_U32(race + 0x60);
            const char *s = i < (int)GT4_U32(ents) ? gt4_specdb_label_for_id(idi, ni, GT4_U32(GT4_U32(ents + 0x24 + i * 4) + 0x20)) : NULL;
            snprintf(snap[i], sizeof snap[i], "%s", s ? s : (i ? others[i - 1] : car));
            snap_cars[i] = snap[i];
        }
        free(idi);
        gt4_vol_close(vol);
        car = snap_cars[0];
        others = snap_cars + 1;
        colors = NULL;
    }
    if (getenv("OPENGT4_RACE_AUTOPILOT")) gt4_car_take_over(player, 5);   /* tests: the model drives the player's car too */
    if(g->split && getenv("OPENGT4_RACE_AUTOPILOT_2P"))
        gt4_car_take_over(gt4_race_run_car(g->run,1),5);   /* P: second original model driver for unattended filming */
    if (getenv("OPENGT4_RACE_VIEW")) g->camera = atoi(getenv("OPENGT4_RACE_VIEW"));   /* tests: the view to start in */
    g->run_bspec = d.bspec;   /* the race built B-Spec (the world's +0xCC0..+0xCCC) */
    if (!g->replay_next) {   /* the lineup kept for the replay */
        g->disc_segments=g->disc_segment=g->disc_segments_2p=g->disc_segment_2p=0;
        g->last_prepared=prepared!=NULL || (d.owned_car[0] || d.owned_car[1]);
        if(g->last_prepared){
            g->last_desc=d;
            if(d.event_row){memmove(g->last_event_row,d.event_row,sizeof g->last_event_row);
                g->last_desc.event_row=g->last_event_row;}
            for(int i=0;i<d.cars;i++)if(d.owned_car[i]){
                memmove(g->last_owned[i],d.owned_car[i],sizeof g->last_owned[i]);
                g->last_desc.owned_car[i]=g->last_owned[i];
            }
            /* RaceLicense (003BBF90) supplies the examination entries,
               without the event opponent/paint RNG (0040AA48). */
            if(d.event_rng){g->last_rng=*d.event_rng;g->last_desc.event_rng=&g->last_rng;}
        }
        snprintf(g->last_course, sizeof g->last_course, "%s", course);
        snprintf(g->last_car, sizeof g->last_car, "%s", car);
        g->last_color = color;
        g->last_nothers = nothers;
        g->last_has_colors = colors != NULL;
        for (int i = 0; i < nothers && i < 5; i++) {
            snprintf(g->last_others[i], sizeof g->last_others[i], "%s", others[i]);
            g->last_other_ptr[i] = g->last_others[i];
            g->last_colors[i] = colors ? colors[i] : 0;
        }
    }
    g->replaying = g->replay_next;
    g->replay_next = 0;
    g->tape_at = 0;
    g->tape_at_2p=0;
    g->license_test_frame=0;
    g->license_result_test_clock=0;g->license_result_test_sent=0;
    g->replay_end = -1;
    g->replay_open = -1;
    g->held = 0;
    g->bgm_off = g->fast = g->car_cam = g->focus = 0;
    g->zoom = 1;g->zoom_on = 0;memset(g->zoom_spring, 0, sizeof g->zoom_spring);
    g->start_events = 0;
    g->label = NULL; g->label_v = 0;
    memset(g->pad_held, 0, sizeof g->pad_held);
    if (!g->replaying) { g->tape_n = g->tape_n_2p = 0; g->thumb_on = 0; }
    g->open_on = 0;
    /* 0039BEC8 +FC..108: license modes 8/9 omit opening captions. */
    if (g->replaying && !g->demonstrating) {   /* before the first step: the ratings borrow the physics */
        uint8_t setup[0x178];
        gt4_car_ratings *cr = gt4_car_ratings_open(g->disc);
        g->open_on = cr && gt4_race_run_setup(g->run, 0, setup) &&
                     gt4_car_ratings_setup(cr, setup, &g->open_power, &g->open_front, &g->open_rear);
        gt4_car_ratings_close(cr);
        if (getenv("OPENGT4_RACE_REPLAY_LOG"))
            fprintf(stderr, "replay opening: %s power %.3f PS tyres %d/%d\n", g->names[0], g->open_power, g->open_front, g->open_rear);
    }
    playing = g;
    gt4_phys_pad.read = host_read;
    gt4_phys_pad.wheel_state = host_wheel;
    gt4_phys_pad.write = host_write;
    g->others = nothers;
    if (g->time_trial && !nothers) {   /* the ghost: the player's car again */
        const char *ghost = car;
        race_view_set_opponents(g->view, &ghost, &color, 1);
    } else race_view_set_opponents(g->view, others, colors, nothers);
    {   /* 00390200/00390340 load fitted parts for each owned entry, not
           only P1. TT's duplicated ghost also uses the player's body. */
        char wheel[32] = "", wing[32] = "";
        gt4_vol *pv = gt4_vol_open(g->disc, &err);
        for(int entry=0;entry<d.cars;entry++){
            const uint8_t *body=d.owned_car[entry];
            if(entry==1 && g->time_trial && !nothers)body=d.owned_car[0];
            wheel[0]=wing[0]=0;if(pv && body)gt4_body_part_labels(pv,body,wheel,wing);
            race_view_set_film_parts(g->view,(unsigned)entry,wheel,wing);
        }
        /* A Time Trial run may describe one entry while the view adds ghost. */
        if(g->time_trial && !nothers && d.cars==1){
            wheel[0]=wing[0]=0;if(pv && d.owned_car[0])gt4_body_part_labels(pv,d.owned_car[0],wheel,wing);
            race_view_set_film_parts(g->view,1,wheel,wing);
        }
        if(pv)gt4_vol_close(pv);
    }
    if (!race_view_request(g->view, course, car, color)) {
        snprintf(msg, len, "cannot load course %s", course);
        race_game_stop(g);
        return 0;
    }
    g->loading = 1;
    g->acc = 0;
    g->pad=plat_sdl_pad(0);
    rumble_pad = g->pad;
    return 1;
}

int race_game_start(race_game *g,const char *course,const char *car,int color,
    const char *const *others,const int *colors,int nothers,char *msg,size_t len)
{
    if(g)g->pit_mode_allowed=1;
    return race_game_start_impl(g,course,car,color,others,colors,nothers,NULL,msg,len);
}
int race_game_start_prepared(race_game *g,const gt4_race_desc *d,char *msg,size_t len)
{
    if(!d || !d->event_row || (!d->license && (!d->event_rng || !d->owned_car[0])) || d->cars<1 || d->cars>6){
        snprintf(msg,len,"incomplete prepared career race");return 0;
    }
    race_game_set_split(g,NULL);
    if(g)g->pit_mode_allowed=1;
    return race_game_start_impl(g,d->course,NULL,0,NULL,NULL,d->cars-1,d,msg,len);
}
int race_game_restart(race_game *g,char *msg,size_t len)
{
    if(!g || !g->last_course[0]){snprintf(msg,len,"no race to restart");return 0;}
    if(g->last_prepared)return race_game_start_impl(g,g->last_desc.course,NULL,0,NULL,NULL,g->last_desc.cars-1,&g->last_desc,msg,len);
    return race_game_start(g,g->last_course,g->last_car,g->last_color,g->last_other_ptr,
        g->last_has_colors?g->last_colors:NULL,g->last_nothers,msg,len);
}
int race_game_setting(race_game *g,const uint8_t body[0x4b8]){
 if(!g || !g->last_prepared || g->last_desc.license || !body)return 0;
 uint32_t slot;memcpy(&slot,body+0x488,4);if(slot>=3)return 0;
 /* 003EE3C8 / 00444840: the next attempt rebuilds the player from
    the selected sheet of the garage car; retain the opponent grid. */
 memcpy(g->last_owned[0],body,0x4b8);g->last_desc.owned_car[0]=g->last_owned[0];
 g->last_desc.tuned_car[0]=0;return 1;
}

/* func_003667A8 -> 00395C70 -> 003E75E8: each car's wheels, but the
   Time Trial's ghost (A: whether its copy of the car smokes is not read) */
static void smoke_wheel(void *user, uint32_t car, uint32_t e)
{
    race_game *g = user;
    race_smoke_wheel w;
    if (!g->smoke || !g->run || (g->ghost_car && car == gt4_race_run_car(g->run, 1))) return;
    w.wheel = (int)GT4_U32(e);
    w.surface = (int)GT4_U32(e + 4);
    for (int k = 0; k < 3; k++) w.pos[k] = GT4_F32(e + 8 + 4 * (uint32_t)k);
    w.shade = GT4_F32(e + 0x18);
    for (int k = 0; k < 4; k++) w.plane[k] = GT4_F32(e + 0x1c + 4 * (uint32_t)k);
    w.amount = GT4_F32(e + 0x2c);
    race_smoke_emit(g->smoke, &w);
}

/* func_003667A8 -> 00395C98 -> 003C2160: a wheel on a leaf triangle */
static void leaf_wheel(void *user, uint32_t car, uint32_t e, uint32_t vel)
{
    race_game *g = user;
    race_leaves_wheel w;
    if (!g->leaves || !g->run || (g->ghost_car && car == gt4_race_run_car(g->run, 1))) return;
    for (int k = 0; k < 3; k++) {
        w.pos[k] = GT4_F32(e + 8 + 4 * (uint32_t)k);
        w.vel[k] = GT4_F32(vel + 4 * (uint32_t)k);
    }
    w.heading = GT4_F32(e + 0x14);
    w.shade = GT4_F32(e + 0x18);
    for (int k = 0; k < 4; k++) w.plane[k] = GT4_F32(e + 0x1c + 4 * (uint32_t)k);
    w.spin = GT4_F32(e + 0x34);
    w.ground = GT4_F32(e + 0x38);
    race_leaves_emit(g->leaves, &w);
}

/* the smoke's classes from the course's Env0 (header +0x80) */
static void start_smoke(race_game *g)
{
    const gt4_course *c = race_view_course(g->view);
    uint32_t env;
    if (!c || c->size < 0x84 || g->smoke) return;
    memcpy(&env, c->data + 0x80, 4);
    if (!env || env >= c->size) return;
    g->smoke = race_smoke_open(c->data + env, c->size - env);
    if (!g->smoke) return;
    race_view_set_smoke(g->view, g->smoke);
    gt4_wheel_smoke = smoke_wheel;
    gt4_wheel_smoke_user = g;
    g->leaves = race_leaves_open(c->data + env, c->size - env);
    race_view_set_leaves(g->view, g->leaves);
    if (g->leaves) gt4_wheel_leaves = leaf_wheel;
}

int race_game_poll(race_game *g)
{
    int r;
    if (!g || !g->loading) return g && g->ready ? 1 : -1;
    r = race_view_poll(g->view, 0);
    if (r == 0) return 0;
    g->loading = 0;
    if (r < 0) {
        race_game_stop(g);
        return -1;
    }
    g->ready = 1;
    /* 00394ED8 / 003CC7B0: COURSE selects the resident pit package.
       One immutable package, six small independent clock workspaces. */
    if(!g->split && !race_game_license_active(g)){
        gt4_arcade *catalog=gt4_arcade_open(g->vol);
        g->pit_package=gt4_arcade_course_pit(catalog,g->last_course);gt4_arcade_close(catalog);
        size_t bytes=0;uint8_t *package=g->pit_package?gt4_vol_read(g->vol,g->pit_package==2?"/pit/pit_002":"/pit/pit_001",&bytes):NULL;
        g->pit_motion[0]=gt4_license_motion_open_pit(package,bytes);free(package);
        g->pit_random=0xfffffff0u; /* PitWorld constructor 003CC608 +11c. */
        g->pit_camera_random=0xfffffff0u; /* 003CF198 +74 seeds camera RNG. */
        if(g->pit_motion[0])g->pit_scene=license_scene_open_pit(g->pit_motion[0]);
        if(g->pit_motion[0])for(int i=0;i<gt4_race_run_cars(g->run);i++){
            if(i)g->pit_motion[i]=gt4_license_motion_clone(g->pit_motion[0]);
            gt4_pit_crew_init(g->pit_crew+i,g->pit_motion[i]);
            gt4_pit_crew_place(g->pit_crew+i,gt4_race_run_car(g->run,i),(unsigned)i,race_view_car_geometry(g->view,i),g->pit_package==1,&g->pit_random);
            race_view_set_pit_crew(g->view,(unsigned)i,g->pit_crew+i);
        }
        if(g->pit_motion[0])for(unsigned i=0;i<(unsigned)gt4_race_run_cars(g->run);i++){
            gt4_pit_crew*c=g->pit_crew+i;float distance=0;
            if(i){float dx=c->box[0]-g->pit_crew[i-1].box[0],dz=c->box[2]-g->pit_crew[i-1].box[2];distance=dx*dx+dz*dz;}
            /* 003CEB90 +38C..44C: shared actors for boxes 15/25 m apart. */
            c->walk_spacing=i?(distance<=225?2:distance<=625?1:0):0;
            gt4_pit_walk_init(c->walking,c->car,c->variant,gt4_pit_stage(c->car),c->walk_spacing,&g->pit_random);
        }
    }
    /* 0033BDB0 +114..124 / 00395B60: section from the built RaceCourse,
       including global objects. Ray queries only read its surface data. */
    uint32_t course=GT4_U32(gt4_race_run_car(g->run,0)+8);
    gt4_course_gadgets *objects=race_view_gadgets(g->view,(int32_t)GT4_U32(course+0xb0));
    if(objects){
        int section=(int32_t)GT4_U32(course+0xb0);uint32_t rw=GT4_U32(GT4_U32(course+4)+0xdc);
        for(unsigned pass=0;pass<2;pass++){
            if(pass && (section<0 || (unsigned)section>=objects->sections))continue;
            unsigned slot=pass?(unsigned)section+1:0;
            for(unsigned j=0;j<objects->length[slot];j++){
                gt4_course_gadget *object=objects->instance+objects->first[slot]+j;gt4_ray_hit hit;
                float pos[3]={object->pos[0],object->pos[1]+1,object->pos[2]};
                if(gt4_course_ray_down(rw,&hit,pos))gt4_course_gadget_surface(object,hit.normal,hit.plane,hit.u,hit.v);
            }
        }
    }
    if(g->last_prepared && license_exam(g)){
        /* 00387FB0 reconstructs the menu Scene after an attempt. Keep
           its initial presentation pose independently of the result's
           concourse handover. A: reset order still needs a menu oracle. */
        gt4_race_run_pose(g->run,0,g->license_menu_pos,g->license_menu_rot,g->license_menu_wheels);
        gt4_race_run_place(g->run,0,g->license_menu_origin,g->license_menu_origin+3);
        g->license_menu_distance=GT4_F32(gt4_race_run_car(g->run,0)+0x5a8);
    }
    start_smoke(g);
    return 1;
}

int race_game_active(const race_game *g) { return g && (g->loading || g->ready); }
unsigned race_game_car_id(const race_game *g, int i) { return g && i >= 0 && i < 6 ? g->ids[i] : 0; }
int race_game_player_hud(const race_game *g,gt4_race_hud *hud)
{return g && g->run && gt4_race_run_hud(g->run,0,hud);}
int race_game_player_wear(const race_game *g,uint8_t wear[32]){
    if(!g || !g->run || !wear)return 0;
    uint32_t car=gt4_race_run_car(g->run,0);if(!car)return 0;
    /* 0010FAA8..0010FB24: four eight-byte copies from car+0x6dc. */
    memcpy(wear,gt4_ptr(car+CAR_ENGINE_WEAR),32);return 1;
}
int race_game_player_distances(const race_game *g,uint32_t *a,uint32_t *b){
    if(!g || !g->run || !a || !b)return 0;uint32_t car=gt4_race_run_car(g->run,0);if(!car)return 0;
    /* 0035DDB0: kind 0 subtracts B-spec mileage, kind 1 returns it. */
    gt4_phys_fpu_enter();float bdistance=GT4_F32(car+0x7b0);
    *a=gt4_career_distance_integer(GT4_F32(car+0x6d4)-bdistance);
    *b=gt4_career_distance_integer(bdistance);return 1;
}
int race_game_driver_mode(const race_game *g){
    if(!g || !g->run)return 0;
    return (int)GT4_U32(GT4_U32(GT4_U32(gt4_race_run_conductor(g->run))+0x84)+0xcc8);
}
int race_game_championship_grid(const race_game *g,gt4_race_desc *desc,uint8_t bodies[6][0x4b8]){
    if(!g || !g->run || !g->last_prepared || !desc || !bodies)return 0;
    *desc=g->last_desc;memset(bodies,0,6*0x4b8);
    uint32_t race=GT4_U32(gt4_race_run_conductor(g->run)),entries=GT4_U32(race+0x60);
    for(int i=0;i<desc->cars;i++){
        if(i==0)memcpy(bodies[i],g->last_owned[0],0x4b8);
        uint32_t entry=GT4_U32(GT4_U32(entries+8)+i*4);
        /* 003EEC70 retains the championship entrants between rounds. */
        memcpy(bodies[i],gt4_ptr(entry+0x20),0x178);
        desc->owned_car[i]=bodies[i];desc->tuned_car[i]=0;
    }
    return 1;
}
int race_game_finish_order(const race_game *g,uint8_t order[6]){
    if(!g || !g->run || !order)return 0;int ranks[6],n=gt4_race_run_cars(g->run);
    /* 003B6FD0 uses stable signed car+0x5b3 ordering. */
    for(int i=0;i<n;i++){gt4_race_hud hud;if(!gt4_race_run_hud(g->run,i,&hud))return 0;
        ranks[i]=hud.rank;int j=i;while(j && ranks[order[j-1]]>ranks[i]){order[j]=order[j-1];j--;}order[j]=(uint8_t)i;}
    return n;
}
int race_game_quick_grid(const race_game *g,menu_quick_race *grid)
{
    if(!g || !g->run || !grid)return 0;
    memset(grid,0,sizeof *grid);grid->entries=gt4_race_run_cars(g->run);
    snprintf(grid->course,sizeof grid->course,"%s",g->last_course);grid->dry=1;
    /* 00119918 +0x4f4 copies race+0; +0x4f0 uses cvt.w.s of
       00395400: course+4 -> data+0xdc -> runway+0x14 metres.
       EE cvt.w.s truncates/saturates; host rounding mode is irrelevant. */
    uint32_t race=GT4_U32(gt4_race_run_conductor(g->run));
    uint32_t course=GT4_U32(race+0x80),data=GT4_U32(course+4),runway=GT4_U32(data+0xdc);
    grid->laps=(int32_t)GT4_U32(race);
    if(g->last_prepared && license_exam(g)){
        grid->license=1;grid->entries=1;
        grid->gold_time=gt4_career_license_time(g->last_event_row,0);
        grid->silver_time=gt4_career_license_time(g->last_event_row,1);
        grid->bronze_time=gt4_career_license_time(g->last_event_row,2);
        /* 0011D400 +0x4c..0x70: logger disabled on IB-6/IB-7;
           005C47A0 reads work +0x4b4, independent of row +0x75. */
        int exam=gt4_career_license_index(g->last_desc.license_code);
        grid->license_disable_logger=exam==37 || exam==38;
        grid->license_load_ghost=!g->last_event_row[0x75];
    }
    grid->course_length=gt4_career_course_length(GT4_F32(runway+0x14));
    for(int i=0;i<grid->entries;i++){
        gt4_race_hud hud;if(!gt4_race_run_hud(g->run,i,&hud))return 0;
        /* quick-license.ad L225..278 presents the student's car only.
           RaceTraining (003ECB68) retains its multi-car grid positions;
           that position is not an index into this one-car summary. */
        int slot=grid->license?0:hud.grid_slot;
        if(slot<0 || slot>=grid->entries)return 0;
        if(!i)grid->player=slot;
        snprintf(grid->grid[slot].car,sizeof grid->grid[slot].car,"%s",i?g->last_others[i-1]:g->last_car);
        grid->grid[slot].color=i?g->last_colors[i-1]:g->last_color;
        grid->grid[slot].time=gt4_career_grid_time(hud.finished,hud.line_time);
        uint32_t car=gt4_race_run_car(g->run,i),params=GT4_U32(car+CAR_PARAMS);
        /* 00112F44 / 00364868: summary of the actual prepared entry,
           not a stock reconstruction using its GENERIC_CAR label. */
        grid->grid[slot].power=(int)gt4_engine_peak_power(params,car+CAR_ENGINE_WEAR);
        grid->grid[slot].weight=(int)GT4_F32(params+PARAMS_MASS)-70;
    }
    return 1;
}
int race_game_quick_results(const race_game *g,menu_quick_race *grid)
{
    menu_quick_race initial;uint8_t order[6];
    if(!race_game_quick_grid(g,&initial) || race_game_finish_order(g,order)!=initial.entries)return 0;
    *grid=initial;
    /* 00114390 walks 003B6FD0's result list, not the initial grid.
       Map driver -> original grid slot before moving the entry. */
    for(int i=0;i<initial.entries;i++){
        gt4_race_hud hud;if(!gt4_race_run_hud(g->run,order[i],&hud))return 0;
        if(hud.grid_slot<0 || hud.grid_slot>=initial.entries)return 0;
        grid->grid[i]=initial.grid[hud.grid_slot];if(!order[i])grid->player=i;
    }
    return 1;
}
int race_game_event_position(const race_game *g)
{
    gt4_race_hud player;if(!race_game_player_hud(g,&player))return -1;
    int position=0;
    /* 003B6FD0 stable insertion by signed car+0x5B3, then 00110570:
       player is entry zero, so equal-rank entries follow it. */
    for(int i=1;i<gt4_race_run_cars(g->run);i++){
        gt4_race_hud other;if(!gt4_race_run_hud(g->run,i,&other))return -1;
        if(other.rank<player.rank)position++;
    }
    return position;
}
double race_game_finished(const race_game *g) { return g && g->ready ? g->finished : -1; }
int race_game_license_active(const race_game *g){return g && g->run && g->last_prepared && license_exam(g);}
int race_game_license_result(const race_game *g,int32_t *state,uint32_t *time){return g && g->run && g->last_prepared && license_exam(g) && gt4_race_run_license_result(g->run,state,time);}
int race_game_license_retry(const race_game *g){return g && race_result_retry(g->result);}
const gt4_logger_sampler *race_game_license_log(const race_game *g){return g && g->license_log.count?&g->license_log:NULL;}
/* 00122FA8 +0x1f0..0x210: the Logger's entries 0 and 1 after the
   session step, until the player's finish (no result-screen frames) */
static void logger_sample(race_game *g)
{
    fenv_t env;fegetenv(&env);gt4_phys_fpu_enter();gt4_logger_input input;
    gt4_logger_car_input(&input,gt4_ptr(gt4_race_run_car(g->run,0)));
    gt4_logger_sample(&g->license_log,&input);
    float bx,bz;   /* entry 1 while it is in the race (the Time Trial's ghost waits off it) */
    if(gt4_race_run_cars(g->run)>=2 && gt4_race_run_xy(g->run,1,&bx,&bz)>=2){
        gt4_logger_car_input(&input,gt4_ptr(gt4_race_run_car(g->run,1)));
        gt4_logger_sample(&g->log_b,&input);
    }
    fesetenv(&env);
    if(gt4_race_run_finished(g->run)){
        if(getenv("OPENGT4_LICENSE_LOGGER_LOG"))fprintf(stderr,"logger: finished at %u points\n",g->license_log.count);
        g->license_log_collecting=0;
        /* 00122D38: each entry's laps added up; A: the race's clock
           when it last crossed the line */
        for(int i=0;i<2 && i<gt4_race_run_cars(g->run);i++){gt4_race_hud hud;
            if(gt4_race_run_hud(g->run,i,&hud) && hud.line_time!=0x157529FFu)g->log_time[i]=hud.line_time;}
        if(getenv("OPENGT4_LICENSE_LOGGER_LOG"))fprintf(stderr,"licence logger: points=%u distance=%.6f max-speed=%.6f\n",g->license_log.count,gt4_logger_length(&g->license_log),g->license_log.maximum_speed);
    }
}

/* 00122FA8 / 0010FEE8: the Logger's analysis of the session again, its
   recorded commands replayed with no picture and the ghost (a best lap
   loaded for a track) driving as entry 1; the loaded track A swaps the
   two. A: the original replays the world on a thread with a progress */
int race_game_logger_analyze_begin(race_game *g,char *msg,size_t len)
{
    if(!g || !(g->time_trial || (g->last_prepared && license_exam(g))) || !race_game_can_replay(g)){snprintf(msg,len,"no race to analyse");return 0;}
    g->log_analyzing=1;g->replay_next=1;g->disc_segment=g->disc_segment_2p=0;
    if(!race_game_restart(g,msg,len)){g->log_analyzing=0;return 0;}
    g->log_steps=0;
    return 1;
}
/* 00122FA8 +0x1B8..0x1E0: steps until the tape ends; its progress as
   00122CC0 reads it (+0x2AC / +0x2B0: where the replay is of its whole) */
int race_game_logger_analyze_step(race_game *g,double budget,float *progress)
{
    if(!g || !g->log_analyzing || !g->run)return -1;
    uint64_t start=plat_time_ns();
    while(g->tape_at<g->tape_n && g->log_steps<g->tape_n+600){
        if(!gt4_race_run_frame(g->run))return -1;
        g->log_steps++;
        if(g->license_log_collecting)logger_sample(g);
        if((double)(plat_time_ns()-start)/1e9>=budget)break;
    }
    if(progress)*progress=g->tape_n?(float)g->tape_at/(float)g->tape_n:1.f;
    return g->tape_at>=g->tape_n || g->log_steps>=g->tape_n+600;
}
void race_game_logger_analyze_end(race_game *g,int track,int ok)
{
    if(!g || !g->log_analyzing)return;
    if(ok && g->run)for(int i=0;i<2 && i<gt4_race_run_cars(g->run);i++){gt4_race_hud hud;
        if(!g->log_time[i] && gt4_race_run_hud(g->run,i,&hud) && hud.line_time!=0x157529FFu)g->log_time[i]=hud.line_time;}
    g->license_log_collecting=0;g->log_analyzing=0;
    if(ok && track==0 && g->log_b.count){   /* the loaded lap as track A */
        gt4_logger_sampler t=g->license_log;g->license_log=g->log_b;g->log_b=t;
        gt4_logger_point *p=g->license_log_points;g->license_log_points=g->log_b_points;g->log_b_points=p;
        uint32_t u=g->log_time[0];g->log_time[0]=g->log_time[1];g->log_time[1]=u;
    }
    race_game_return_pregrid(g);
    if(getenv("OPENGT4_LICENSE_LOGGER_LOG"))fprintf(stderr,"logger analysis%s: A %u points, B %u points\n",ok?"":" stopped",g->license_log.count,g->log_b.count);
}
int race_game_logger_analyze(race_game *g,int track,char *msg,size_t len)
{
    if(!race_game_logger_analyze_begin(g,msg,len))return 0;
    int r;while((r=race_game_logger_analyze_step(g,1e9,NULL))==0){}
    if(r<0)snprintf(msg,len,"the analysis step failed");
    race_game_logger_analyze_end(g,track,r>0);
    return g->license_log.count>0;
}
/* 0010FEE8 +174 / 00122FA8: load a recording into one Logger bank.
   P: synchronous native reconstruction, atomic bank replacement. A: native
   file metadata replaces FileReplayBestLap. No solver formula is changed. */
int race_game_logger_load(race_game *g,const gt4_replay_file *r,int track,char *msg,size_t len)
{
    if(!g || !r || track<0 || track>1 || !g->license_log_points || !g->log_b_points || !g->license_log.count ||
       !r->tape || !r->steps || r->split_battle ||
       strcmp(r->course,g->last_course) || (r->kind==GT4_REPLAY_BEST_LAP && !r->ghost_car)){
        snprintf(msg,len,"Choose a recording from this course.");return 0;
    }
    gt4_replay_file old,borrowed;uint8_t *packed=NULL;size_t bytes=0;
    if(!race_game_replay_export(g,&borrowed,GT4_REPLAY_FULL) || !gt4_replay_pack(&borrowed,&packed,&bytes) ||
       !gt4_replay_unpack(packed,bytes,&old)){free(packed);snprintf(msg,len,"Cannot preserve the current recording.");return 0;}
    free(packed);gt4_race_desc desc=g->last_desc;int owned=g->arcade_owned_on,loaded_power=0;
    gt4_logger_sampler banks[2]={g->license_log,g->log_b},loaded;
    gt4_logger_point *copies[2]={NULL,NULL},*points=calloc(GT4_LOGGER_POINTS,sizeof *points);
    int ok=points!=NULL;uint32_t times[2]={g->log_time[0],g->log_time[1]};
    for(int i=0;i<2;i++)if(banks[i].count){copies[i]=malloc((size_t)banks[i].count*sizeof **copies);if(!copies[i])ok=0;else memcpy(copies[i],banks[i].points,(size_t)banks[i].count*sizeof **copies);}
    if(ok && race_game_replay_import(g,r)){
        g->arcade_owned_on=0;
        g->log_analyzing=1;g->replay_next=1;
        ok=race_game_restart(g,msg,len);
        if(ok){
            /* 003F58B0 / 0035ABA8: best-lap analysis begins at its stored
               state, without waiting for a new live-player lap event.
               P: reconstruct into entry zero of a private native session. */
            if(r->kind==GT4_REPLAY_BEST_LAP){
                uint8_t state[0x14f4]={0};state[2]=(uint8_t)r->ghost_rest;memcpy(state+0x54,r->ghost_car,GT4_GHOST_CAR);
                uint32_t cond=gt4_race_run_conductor(g->run),race=GT4_U32(cond),car=gt4_race_run_car(g->run,0);
                fenv_t env;fegetenv(&env);gt4_phys_fpu_enter();
                GT4_U32(cond+0xf898)=100;GT4_U32(race+0xbc)=0;disc_checkpoint_restore(cond,0,state);
                GT4_U16(car+0x5b0)=1;GT4_F32(car+0x554)=0;fesetenv(&env);
            }
            menu_quick_race grid;if(race_game_quick_grid(g,&grid))loaded_power=grid.grid[grid.player].power;
            if(ok){
                fenv_t env;fegetenv(&env);gt4_phys_fpu_enter();
                gt4_logger_begin(&loaded,points,gt4_logger_spacing(banks[0].course_length),banks[0].course_length);fesetenv(&env);
                for(size_t step=0;step<r->steps && ok;step++){
                    ok=gt4_race_run_frame(g->run);
                    if(ok){gt4_logger_input input;fegetenv(&env);gt4_phys_fpu_enter();
                        gt4_logger_car_input(&input,gt4_ptr(gt4_race_run_car(g->run,0)));
                        gt4_logger_sample(&loaded,&input);fesetenv(&env);}
                }
                ok=ok && loaded.count>0;
            }
        }
    }else ok=0;
    g->log_analyzing=0;g->license_log_collecting=0;
    int restored=race_game_replay_import(g,&old);g->last_desc=desc;g->arcade_owned_on=owned;
    if(restored){g->replay_next=1;restored=race_game_restart(g,msg,len);race_game_return_pregrid(g);}
    gt4_replay_free(&old);
    /* The other bank and its pointer remain unchanged even after rebuild. */
    for(int i=0;i<2;i++){
        gt4_logger_sampler *dst=i?&g->log_b:&g->license_log;
        gt4_logger_point *buf=i?g->log_b_points:g->license_log_points;
        *dst=banks[i];dst->points=buf;
        if(copies[i])memcpy(buf,copies[i],(size_t)banks[i].count*sizeof *buf);
        free(copies[i]);g->log_time[i]=times[i];
    }
    if(ok && restored){
        gt4_logger_sampler *dst=track?&g->log_b:&g->license_log;
        memcpy(dst->points,points,(size_t)loaded.count*sizeof *points);
        gt4_logger_point *buf=dst->points;*dst=loaded;dst->points=buf;
        g->log_time[track]=r->best_lap!=0x157529ffu?r->best_lap:r->race_time;
        snprintf(g->logger_loaded_car[track],96,"%s",r->car);g->logger_loaded_power[track]=loaded_power;
    }else snprintf(msg,len,"Cannot reconstruct this recording.");
    free(points);return ok && restored;
}
int race_game_logger_source(const race_game *g,menu_logger_source *s)
{
    memset(s,0,sizeof *s);s->subnumber=-1;
    if(!g || !g->run || !g->license_log.count)return 0;
    s->track[0]=&g->license_log;
    if(g->log_b.count)s->track[1]=&g->log_b;
    if(getenv("OPENGT4_LICENSE_LOGGER_LOG"))fprintf(stderr,"logger source: A %u points %.1f m, B %u points, collecting %d\n",
        g->license_log.count,gt4_logger_length(&g->license_log),g->log_b.count,g->license_log_collecting);
    menu_quick_race q;int grid=race_game_quick_grid(g,&q);
    for(int i=0;i<2;i++){
        gt4_race_hud hud;
        s->time[i]=g->log_time[i];
        if(!s->time[i] && i<gt4_race_run_cars(g->run) && gt4_race_run_hud(g->run,i,&hud) && hud.line_time!=0x157529FFu)
            s->time[i]=hud.line_time;   /* the Time Trial left from its pause: where it stood */
        if(grid && i<gt4_race_run_cars(g->run) && gt4_race_run_hud(g->run,i,&hud) && hud.grid_slot>=0 && hud.grid_slot<q.entries){
            snprintf(s->car[i],sizeof s->car[i],"%s",q.grid[hud.grid_slot].car);s->power[i]=q.grid[hud.grid_slot].power;
        }else if(i==1 && g->log_b.count){   /* the ghost: the player's own car (RaceLicense 003BDC88 clones it) */
            snprintf(s->car[1],sizeof s->car[1],"%s",s->car[0]);s->power[1]=s->power[0];
        }
        if(!s->car[i][0] && (i==0 || g->log_b.count))snprintf(s->car[i],sizeof s->car[i],"%s",g->last_car);   /* the Time Trial's grid keeps no label */
    }
    for(int i=0;i<2;i++)if(g->logger_loaded_car[i][0]){snprintf(s->car[i],sizeof s->car[i],"%s",g->logger_loaded_car[i]);s->power[i]=g->logger_loaded_power[i];}
    snprintf(s->course,sizeof s->course,"%s",g->last_course);
    if(getenv("OPENGT4_LICENSE_LOGGER_LOG"))fprintf(stderr,"logger source: cars '%s' %d / '%s' %d, times %u / %u\n",s->car[0],s->power[0],s->car[1],s->power[1],s->time[0],s->time[1]);
    /* logger.ad setLoggerInfo: RaceModeName's key and the number after
       it (the licence class of its RACE label l0<class><number>, as the
       table names them: GTMODE_LICENSE_A "A" License). A: the keys taken
       from the table, the original getters not read */
    if(license_exam(g)){
        const char *code=g->last_desc.license_code,*c=code;char cls[8]="";int n=0;
        if(!strncmp(c,"l0",2))c+=2;
        while(*c && !(*c>='0' && *c<='9') && n<7){cls[n++]=(char)(*c>='a' && *c<='z'?*c-32:*c);c++;}
        cls[n]=0;
        snprintf(s->mode,sizeof s->mode,"GTMODE_LICENSE_%s",cls);
        s->subnumber=*c?atoi(c):-1;
    }else snprintf(s->mode,sizeof s->mode,"ARCADE_TIME_TRIAL");
    return 1;
}
int race_game_over(const race_game *g)
{
    if (g && g->ready && g->replaying) return g->replay_end >= 3 || g->quit;
    if(g && g->ready && g->mission_presentation==GT4_MISSION_STOPPED)return 1;
    return g && g->ready && race_result_done(g->result);
}
int race_game_can_replay(const race_game *g) { return g && g->tape_n > 0 && (!g->split || g->tape_n_2p>0); }
void race_game_set_replay_options(race_game *g, int display, int dive)
{
    if (g) g->rmode = display ? 2 : dive ? 1 : 0;
}
int race_game_replay(race_game *g, char *msg, size_t len)
{
    if (!g || !g->tape_n || !g->last_course[0]) { snprintf(msg, len, "no replay"); return 0; }
    g->replay_next = 1;
    g->disc_segment=g->disc_segment_2p=0;
    return race_game_restart(g,msg,len);
}
int race_game_replaying(const race_game *g) { return g && g->replaying; }
int race_game_read_replay(race_game *g,const char *name,gt4_replay_file *r,char *msg,size_t len)
{
    if(!g || !name || !r)return 0;
    memset(r,0,sizeof *r);
    if(strncmp(name,"/replay/",8) && gt4_replay_load(name,r))return 1;
    if(!g->disc_replays)g->disc_replays=gt4_disc_replay_open(g->vol);
    return !strncmp(name,"/replay/",8)?gt4_disc_replay_read(g->disc_replays,name,r,msg,len):gt4_disc_replay_saved(g->disc_replays,name,r,msg,len);
}
int race_game_demonstrating(const race_game *g){return g && g->demonstrating;}
int race_game_end_demo(race_game *g,char *msg,size_t len)
{
    if(!g || !g->demonstrating)return 0;
    free(g->tape);g->tape=g->before_demo.tape;g->before_demo.tape=NULL;
    g->demonstrating=0;g->replay_next=0;
    int ok=race_game_restart(g,msg,len);
    g->tape_n=g->before_demo.n;g->tape_cap=g->before_demo.cap;
    g->thumb_on=g->before_demo.thumb_on;g->license_log=g->before_demo.log;
    g->license_log_collecting=0;
    return ok;
}
int race_game_license_demo(race_game *g,const char *code,char *msg,size_t len)
{
    gt4_replay_file r;char path[64];
    if(!g || g->demonstrating || !g->last_prepared || !license_exam(g))return 0;
    if(!code)code=g->last_desc.license_code;
    if(!*code){snprintf(msg,len,"examination has no demonstration label");return 0;}
    snprintf(path,sizeof path,"/replay/license/%s%s",strncmp(code,"us",2)?"us":"",code);
    if(!race_game_read_replay(g,path,&r,msg,len))return 0;
    /* 00110230 +98..F4 copies the recording's driving options;
       +F8..110 invokes 003EAD58 (+1C8), importing only the recorder into
       the current examination. Historical setup IDs are not restored. */
    gt4_race_desc d=g->last_desc;d.manual=r.manual;d.assist_asm=r.assist_asm;d.assist_tcs=r.assist_tcs;
    g->before_demo.tape=g->tape;g->before_demo.n=g->tape_n;g->before_demo.cap=g->tape_cap;
    g->before_demo.thumb_on=g->thumb_on;g->before_demo.log=g->license_log;
    g->tape=r.tape;r.tape=NULL;g->tape_n=g->tape_cap=r.steps;
    g->disc_segments=g->disc_segment=g->disc_segments_2p=g->disc_segment_2p=0;
    g->demonstrating=1;g->replay_next=1;
    int ok=race_game_start_impl(g,d.course,NULL,0,NULL,NULL,d.cars-1,&d,msg,len);
    if(ok){
        /* 0011D1B8 +44..60: demonstration Scene, RaceInfo +DC=1. */
        uint32_t race=GT4_U32(gt4_race_run_conductor(g->run));GT4_U32(race+0xdc)=1;
        fprintf(stderr,"demonstration: %s, %zu original commands\n",code,g->tape_n);
    }else {char ignored[256];race_game_end_demo(g,ignored,sizeof ignored);}
    gt4_replay_free(&r);return ok;
}

/* the last race as a file (replay_file.h): its lineup and commands; the
   tape is lent, not copied */
int race_game_replay_export(const race_game *g, gt4_replay_file *r, int kind)
{
    if (!g || !r || !g->tape_n || !g->last_course[0]) return 0;
    memset(r, 0, sizeof *r);
    r->kind = kind;
    snprintf(r->course, sizeof r->course, "%s", g->last_course);
    snprintf(r->car, sizeof r->car, "%s", g->last_car);
    r->color = g->last_color;
    r->nothers = g->last_nothers;
    r->has_colors = g->last_has_colors;
    for (int i = 0; i < 5; i++) { snprintf(r->others[i], sizeof r->others[i], "%s", g->last_others[i]); r->colors[i] = g->last_colors[i]; }
    r->laps = g->laps;
    r->tire = g->tire;
    r->manual = g->manual;
    r->assist_asm = g->assist_asm;
    r->assist_tcs = g->assist_tcs;
    r->time_trial = g->time_trial;
    r->world_mode = g->world_mode;
    if(g->split){
        const gt4_race_desc *d=&g->split_options;
        r->split_battle=1;r->tape_2p=g->tape_2p;r->steps_2p=g->tape_n_2p;
        r->tire_2p=d->tire_2p;r->manual_2p=d->manual_2p;r->assist_asm_2p=d->assist_asm_2p;r->assist_tcs_2p=d->assist_tcs_2p;
        r->split_boost=d->split_boost;r->split_handicap=d->split_handicap;r->split_tire_damage=d->split_tire_damage;
        for(int i=0;i<2;i++)if(d->owned_car[i]){r->owned_on[i]=1;memcpy(r->owned[i],d->owned_car[i],GT4_REPLAY_OWNED);}
    }
    r->prepared = g->last_prepared;
    if (r->prepared) {
        const gt4_race_desc *d = &g->last_desc;
        r->championship = d->championship;
        r->cars = d->cars;
        r->rng = g->last_rng;
        r->laps = d->laps;
        r->tire = d->tire;
        r->manual = d->manual;
        r->assist_asm = d->assist_asm;
        r->assist_tcs = d->assist_tcs;
        r->time_trial = d->time_trial;
        for (int i = 0; i < 6; i++) {
            r->car_id[i] = d->car[i];
            r->tuned_car[i] = d->tuned_car[i];
            r->event_paint[i] = d->event_paint[i];
            r->owned_on[i] = d->owned_car[i] != NULL;
            if (d->owned_car[i]) memcpy(r->owned[i], g->last_owned[i], sizeof r->owned[i]);
        }
        if(d->event_row)memcpy(r->event_row, d->event_row, sizeof r->event_row);
        else memset(r->event_row,0,sizeof r->event_row);
    }
    snprintf(r->player, sizeof r->player, "%s", gt4_save_player());
    /* the list's car: its name as the game writes it (CAR_NAME), not the label */
    if (g->names[0][0]) snprintf(r->car_name, sizeof r->car_name, "%s", g->names[0]);
    r->race_time = (uint32_t)(g->tape_n * 1000 / 60);
    r->best_lap = race_game_best_lap(g);
    r->tape = g->tape;
    r->steps = g->tape_n;
    r->thumb = g->thumb_on ? (uint8_t *)g->thumb : NULL;
    return 1;
}

/* A saved replay's picture: the frame the player crosses the line, or
   ten seconds in for a race left before (A: the PS2's own moment and
   picture not read), drawn without the HUD, scaled to 128x96 by averaging. */
int race_game_wants_thumbnail(const race_game *g)
{ return g && g->ready && !g->replaying && ((!g->thumb_on && g->tape_n >= 600) || (g->thumb_on < 2 && g->finished >= 0)); }
void race_game_set_thumbnail(race_game *g, const uint8_t *rgba, int w, int h)
{
    if (!g || !rgba || w < GT4_REPLAY_THUMB_W || h < GT4_REPLAY_THUMB_H) return;
    for (int y = 0; y < GT4_REPLAY_THUMB_H; y++)
        for (int x = 0; x < GT4_REPLAY_THUMB_W; x++) {
            int x0 = x * w / GT4_REPLAY_THUMB_W, x1 = (x + 1) * w / GT4_REPLAY_THUMB_W;
            int y0 = y * h / GT4_REPLAY_THUMB_H, y1 = (y + 1) * h / GT4_REPLAY_THUMB_H;
            unsigned sum[3] = { 0 }, n = 0;
            for (int yy = y0; yy < y1; yy++)
                for (int xx = x0; xx < x1; xx++, n++)
                    for (int c = 0; c < 3; c++) sum[c] += rgba[((size_t)yy * w + xx) * 4 + c];
            for (int c = 0; c < 3; c++) g->thumb[(y * GT4_REPLAY_THUMB_W + x) * 3 + c] = (uint8_t)(n ? sum[c] / n : 0);
        }
    g->thumb_on = g->finished >= 0 ? 2 : 1;
}

/* a file's race made the last one, ready for race_game_replay */
int race_game_replay_import(race_game *g, const gt4_replay_file *r)
{
    uint8_t *tape;
    if (!g || !r || !r->steps || !r->tape) return 0;
    uint8_t *second=NULL;
    if(r->split_battle){
        if(!r->tape_2p || !r->steps_2p || r->nothers!=1 || !(second=malloc(r->steps_2p*32)))return 0;
        memcpy(second,r->tape_2p,r->steps_2p*32);
    }
    if (!(tape = malloc(r->steps * 0x20))) {free(second);return 0;}
    race_game_set_split(g,NULL);
    if(r->split_battle){
        gt4_race_desc d={0};d.tire_2p=r->tire_2p;d.manual_2p=r->manual_2p;d.assist_asm_2p=r->assist_asm_2p;d.assist_tcs_2p=r->assist_tcs_2p;
        d.split_boost=r->split_boost;d.split_handicap=r->split_handicap;d.split_tire_damage=r->split_tire_damage;
        for(int i=0;i<2;i++)if(r->owned_on[i])d.owned_car[i]=r->owned[i];
        race_game_set_split(g,&d);
    }
    free(g->tape_2p);g->tape_2p=second;g->tape_n_2p=g->tape_cap_2p=r->split_battle?r->steps_2p:0;
    memcpy(tape, r->tape, r->steps * 0x20);
    free(g->tape);
    g->tape = tape;
    g->tape_n = g->tape_cap = r->steps;
    snprintf(g->last_course, sizeof g->last_course, "%s", r->course);
    snprintf(g->last_car, sizeof g->last_car, "%s", r->car);
    g->last_color = r->color;
    g->last_nothers = r->nothers;
    g->last_has_colors = r->has_colors;
    for (int i = 0; i < 5; i++) {
        snprintf(g->last_others[i], sizeof g->last_others[i], "%s", r->others[i]);
        g->last_other_ptr[i] = g->last_others[i];
        g->last_colors[i] = r->colors[i];
    }
    g->laps = r->laps;
    g->tire = r->tire;
    g->manual = r->manual;
    g->assist_asm = r->assist_asm;
    g->assist_tcs = r->assist_tcs;
    g->time_trial = r->time_trial;
    g->world_mode = r->world_mode;
    g->disc_segments=r->disc_segments;g->disc_segment=g->disc_segment_2p=0;g->disc_segments_2p=r->disc_segments_2p;g->disc_segment_2p=0;
    for(unsigned i=0;i<r->disc_segments_2p;i++){g->disc_segment_at_2p[i]=r->disc_segment_at_2p[i];memcpy(g->disc_states_2p[i],r->disc_info+r->disc_segment_offset_2p[i],0x14f4);}
    for(unsigned i=0;i<r->disc_segments;i++){
        g->disc_segment_at[i]=r->disc_segment_at[i];
        memcpy(g->disc_states[i],r->disc_info+r->disc_segment_offset[i],0x14f4);
    }
    g->last_prepared = r->prepared;
    if (r->prepared) {
        gt4_race_desc *d = &g->last_desc;
        memset(d, 0, sizeof *d);
        snprintf(d->course, sizeof d->course, "%s", r->course);
        d->cars = r->cars;
        d->championship = r->championship;
        d->laps = r->laps;
        d->tire = r->tire;
        d->manual = r->manual;
        d->assist_asm = r->assist_asm;
        d->assist_tcs = r->assist_tcs;
        d->time_trial = r->time_trial;d->world_mode=r->world_mode;d->license=r->disc_license;snprintf(d->license_code,sizeof d->license_code,"%s",r->license_code);
        d->split_battle=r->split_battle;d->tire_2p=r->tire_2p;d->manual_2p=r->manual_2p;
        d->assist_asm_2p=r->assist_asm_2p;d->assist_tcs_2p=r->assist_tcs_2p;
        d->split_boost=r->split_boost;d->split_handicap=r->split_handicap;d->split_tire_damage=r->split_tire_damage;
        memcpy(g->last_event_row, r->event_row, sizeof g->last_event_row);
        /* P: native files represent an absent RACE row with zero bytes.
           Owned Arcade/TT bodies are prepared, but are not career events. */
        const uint8_t no_event[GT4_REPLAY_ROW]={0};
        d->event_row = (r->disc_info && !r->disc_event) || r->split_battle ||
            (!r->disc_info && !memcmp(r->event_row,no_event,sizeof no_event))?NULL:g->last_event_row;
        g->last_rng = r->rng;
        d->event_rng = r->disc_info || r->split_battle || !d->event_row?NULL:&g->last_rng;
        for (int i = 0; i < 6; i++) {
            d->car[i] = r->car_id[i];
            d->tuned_car[i] = r->tuned_car[i];
            d->event_paint[i] = r->event_paint[i];
            if (r->owned_on[i]) { memcpy(g->last_owned[i], r->owned[i], sizeof g->last_owned[i]); d->owned_car[i] = g->last_owned[i]; }
        }
    }
    return 1;
}
void race_game_set_laps(race_game *g, int laps) { if (g) g->laps = laps; }
/* 00115CB4..00115D74: Power & Speed's end, car 0 through: its time (best
   lap, 0035E3F0) and top speed (car +0x6CC); the test (the laps word) */
int race_game_machine_result(const race_game *g,int *test,uint32_t *time,float *speed)
{
    if(!g || !g->run || g->world_mode!=15 || g->replaying)return 0;
    uint32_t car=gt4_race_run_car(g->run,0);
    if(!GT4_U8(car+0x5b6))return 0;
    if(test)*test=g->laps<4?g->laps:0;
    if(time)*time=GT4_U32(car+0x71c);
    if(speed)*speed=GT4_F32(car+0x6cc);
    return 1;
}
void race_game_set_time_trial(race_game *g, int on) { if (g) g->time_trial = on != 0; }
void race_game_set_world_mode(race_game *g, int mode) { if (g) g->world_mode = mode; }
/* B-Spec for the next build; the race already built keeps its own. Returns
   1 when the built race differs and must be rebuilt (race_game_restart). */
int race_game_set_bspec(race_game *g, int on)
{
    if (!g) return 0;
    g->bspec = on != 0;
    if (!g->last_prepared && !g->last_course[0]) return 0;
    int changed = g->last_desc.bspec != g->bspec;
    g->last_desc.bspec = g->bspec;
    return changed;
}
int race_game_bspec(const race_game *g) { return g && g->run && g->run_bspec; }
uint32_t race_game_best_lap(const race_game *g)
{
    gt4_race_hud h;
    /* func_0035E3F0 on the player's car (car+0x71C), as 00112620 reads it
       after the race; the no-time 0x157529FF without one */
    if (!g || !g->run || !gt4_race_run_hud(g->run, 0, &h) || !h.best_lap) return 0x157529ffu;
    return h.best_lap;
}
/* 00111DC0 at the Arcade's start: the QuickTune's gearbox (+0x3C0 into
   the entry's +0x3448) and aids (+0x3C4, 00110B90: 2 both, 1 TCS alone)
   always; power, weight, tyres and top speed when IsQuickTuned. Returns
   1 when the race already built must be rebuilt to take them. */
int race_game_quick_tune(race_game *g, int tuned, int power, int weight, int front, int rear,
                         int gear, int transmission, int assist)
{
    if (!g) return 0;
    int asm_on = assist == 2, tcs_on = assist >= 1, changed = 0;
    changed |= g->manual != (transmission != 0) || g->assist_asm != asm_on || g->assist_tcs != tcs_on;
    g->manual = transmission != 0;
    g->assist_asm = asm_on;
    g->assist_tcs = tcs_on;
    if (tuned) {
        g->quick_tuned = 1;
        g->quick_power = power;
        g->quick_weight = weight;
        g->quick_front_tire = front;
        g->quick_rear_tire = rear;
        g->quick_gear = gear;
        changed = 1;
    }
    return changed;
}
void race_game_quick_clear(race_game *g){ if (g) g->quick_tuned = 0; }
int race_game_gear_range(const race_game *g, int *gear, int *lo, int *hi)
{
    return g && g->run && gt4_race_run_gear_range(g->run, 0, gear, lo, hi);
}

void race_game_set_split(race_game *g,const gt4_race_desc *options)
{
    if(!g)return;g->split=options!=NULL;
    if(options)g->split_options=*options;else memset(&g->split_options,0,sizeof g->split_options);
    /* Own both buffers across loading, Retry and the menu's lifetime.
       The verified race builder consumes active sheets via 00441248. */
    if(options)for(int i=0;i<2;i++)if(options->owned_car[i]){
        memmove(g->split_owned[i],options->owned_car[i],sizeof g->split_owned[i]);
        g->split_options.owned_car[i]=g->split_owned[i];
    }
    if(g->split){g->time_trial=g->bspec=0;g->inspect_on=0;}
}
void race_game_set_arcade_owned(race_game *g,const uint8_t *body)
{
    if(!g)return;g->arcade_owned_on=body!=NULL;
    /* arcade.ad GameRoot.doGame passes car_garage to RaceArcade/RaceTT.
       The existing constructor reads its active sheet (00441248). */
    if(body)memmove(g->arcade_owned,body,sizeof g->arcade_owned);
}
int race_game_split(const race_game *g){return g && g->split;}
int race_game_take_retry(race_game *g){int pending=g && g->retry_requested;if(g)g->retry_requested=0;return pending;}

/* 003D6350 +18..20 copies the option into the player controller.
   The native pregrid is already prepared; report whether it needs a
   rebuild, and retain this choice through prepared Retry. */
int race_game_set_transmission(race_game *g,int manual)
{
    if(!g)return 0;
    manual=manual!=0;
    int previous=g->last_prepared?g->last_desc.manual:g->manual;
    g->manual=manual;
    if(g->last_prepared)g->last_desc.manual=manual;
    return previous!=manual;
}

void race_game_set_driving(race_game *g, int tire, int manual, int assist_asm, int assist_tcs)
{
    if (!g) return;
    g->tire = tire >= 0 ? tire + 1 : 0;
    g->manual = manual;
    g->assist_asm = assist_asm;
    g->assist_tcs = assist_tcs;
}

/* 0032F028 +0x8C..0x120 / 00426D50: port 1 belongs to entry 1.
   P: WASD/Q/E/C/Left Ctrl are PC bindings; port config is the original's. */
static void read_second_controls(race_game *g,const uint8_t *keys)
{
    plat_pad_state state;gt4_pad_config config;gt4_pad_actions pad={0};
    int connected=plat_input_poll(1,&state);
    if(connected){menu_pad_configuration(NULL,1,&config);gt4_pad_evaluate(&config,&state,&pad);}
    if(plat_input_is_wheel(1)){pad.steer=-state.axes[0];pad.action[GT4_PAD_ACCEL]=state.axes[4];pad.action[GT4_PAD_BRAKE]=state.axes[5];pad.analog|=(1u<<GT4_PAD_LSTEER)|(1u<<GT4_PAD_ACCEL)|(1u<<GT4_PAD_BRAKE);}
    race_command *c=&command_2p;memset(c,0,sizeof *c);c->device=plat_input_is_wheel(1)?4:connected?2:1;
    float steer=(float)(keys[SDL_SCANCODE_A]-keys[SDL_SCANCODE_D]);int analog=0;
    if(steer==0 && pad.steer!=0){steer=pad.steer;analog=!!(pad.analog&(1u<<GT4_PAD_LSTEER));}
    c->flags=(uint8_t)((analog?1:0)|(pad.analog&(1u<<GT4_PAD_ACCEL)?2:0)|
        (pad.analog&(1u<<GT4_PAD_BRAKE)?4:0)|(pad.analog&(1u<<GT4_PAD_REVERSE)?8:0)|
        (pad.analog&(1u<<GT4_PAD_SIDE)?16:0));
    c->steer=(int16_t)(steer*4096);
    c->throttle=(int16_t)(fmaxf(keys[SDL_SCANCODE_W]?1.f:0.f,pad.action[GT4_PAD_ACCEL])*(c->flags&2?4096:1));
    c->brake=(int16_t)(fmaxf(keys[SDL_SCANCODE_S]?1.f:0.f,pad.action[GT4_PAD_BRAKE])*(c->flags&4?4096:1));
    c->handbrake=(int16_t)(fmaxf(keys[SDL_SCANCODE_LCTRL]?1.f:0.f,pad.action[GT4_PAD_SIDE])*(c->flags&16?4096:1));
    c->reverse=(int16_t)(pad.action[GT4_PAD_REVERSE]*(c->flags&8?4096:1));
    c->shift=(int8_t)(keys[SDL_SCANCODE_Q]-keys[SDL_SCANCODE_E]+(pad.action[GT4_PAD_SHIFTUP]>0)-(pad.action[GT4_PAD_SHIFTDOWN]>0));
    c->special=pad.action[GT4_PAD_SPECIAL]>0;
    int view=keys[SDL_SCANCODE_C] || pad.action[GT4_PAD_PLAYVIEW]>0;
    if(view && !g->view_held_2p){static const int order[3]={0,1,6};int k=0;
        while(k<3 && order[k]!=g->camera_2p)k++;g->camera_2p=order[(k+1)%3];}
    g->view_held_2p=view;g->look_back_2p=pad.action[GT4_PAD_BACKVIEW]>0;
}

/* The keyboard is a digital pad (the game eases button steering itself);
   a controller's stick or triggers make it an analog one */
static void read_controls(race_game *g)
{
    const uint8_t *keys = SDL_GetKeyboardState(NULL);
    static const uint8_t no_camera_keys[SDL_NUM_SCANCODES];
    if(g->cinema.on)keys=no_camera_keys; /* camera keyboard, driving pad remains live */
    float steer = (float)(keys[SDL_SCANCODE_LEFT] - keys[SDL_SCANCODE_RIGHT]);
    float throttle = keys[SDL_SCANCODE_UP] ? 1.f : 0.f, brake = keys[SDL_SCANCODE_DOWN] ? 1.f : 0.f;
    if (getenv("OPENGT4_RACE_TEST_DRIVE")) {   /* tests: throttle on, the wheel swinging with the wall clock */
        double t = SDL_GetTicks() / 1000.0;
        throttle = 1;
        steer = (float)(sin(t * 1.7) * .5);
    }
    /* Test controller follows the same B-1 input tape as license_drive_run.
       It changes only pad input; no examination state/threshold is injected. */
    const char *license_brake=getenv("OPENGT4_LICENSE_TEST_BRAKE_AT");
    if(license_brake && g->last_prepared && license_exam(g)){
        unsigned brake_at=(unsigned)strtoul(license_brake,NULL,10);
        const char *license_wait=getenv("OPENGT4_LICENSE_TEST_WAIT");
        unsigned wait=license_wait?(unsigned)strtoul(license_wait,NULL,10):0;
        steer=0;throttle=g->license_test_frame>=wait && g->license_test_frame<brake_at;
        brake=g->license_test_frame>=brake_at;
    }
    float hand = keys[SDL_SCANCODE_SPACE] ? 1.f : 0.f;
    int analog = 0, view;
    command.shift = (int8_t)(g->split?(keys[SDL_SCANCODE_PAGEUP]-keys[SDL_SCANCODE_PAGEDOWN]):(keys[SDL_SCANCODE_A]-keys[SDL_SCANCODE_Z]));
    if(g->split)read_second_controls(g,keys);
    plat_pad_state state;gt4_pad_config config;gt4_pad_actions pad={0};
    int connected=plat_input_poll(0,&state);
    g->pad=connected?plat_sdl_pad(0):NULL;
    if(connected){menu_pad_configuration(NULL,0,&config);gt4_pad_evaluate(&config,&state,&pad);}
    if(plat_input_is_wheel(0)){pad.steer=-state.axes[0];pad.action[GT4_PAD_ACCEL]=state.axes[4];pad.action[GT4_PAD_BRAKE]=state.axes[5];pad.analog|=(1u<<GT4_PAD_LSTEER)|(1u<<GT4_PAD_ACCEL)|(1u<<GT4_PAD_BRAKE);}
    view=keys[SDL_SCANCODE_V] || pad.action[GT4_PAD_PLAYVIEW]>0;
    g->look_back=pad.action[GT4_PAD_BACKVIEW]>0;
    if (view && !g->view_held) {
        /* the views in the game's order (D_006211E8: on the car, behind it,
           the roof), func_0037AD10 stepping through them */
        static const int order[3] = { 0, 1, 6 };
        int k = 0;
        while (k < 3 && order[k] != g->camera) k++;
        g->camera = order[(k + 1) % 3];
    }
    g->view_held = view;
    if(connected){
        if(pad.steer!=0 && steer==0){steer=pad.steer;analog=!!(pad.analog&(1u<<GT4_PAD_LSTEER));}
        throttle=fmaxf(throttle,pad.action[GT4_PAD_ACCEL]);brake=fmaxf(brake,pad.action[GT4_PAD_BRAKE]);
        hand=fmaxf(hand,pad.action[GT4_PAD_SIDE]);
        command.shift=(int8_t)(command.shift+(pad.action[GT4_PAD_SHIFTUP]>0)-(pad.action[GT4_PAD_SHIFTDOWN]>0));
    }
    /* Each channel carries its own analog flag (driver.c/00349F48), so
       moving a stick cannot turn digital pedals off or make 1 mean 1/4096. */
    command.flags=(uint8_t)((analog?1:0)|
        (pad.analog&(1u<<GT4_PAD_ACCEL)?2:0)|(pad.analog&(1u<<GT4_PAD_BRAKE)?4:0)|
        (pad.analog&(1u<<GT4_PAD_REVERSE)?8:0)|(pad.analog&(1u<<GT4_PAD_SIDE)?16:0));
    command.device=plat_input_is_wheel(0)?4:g->pad?2:1;
    command.steer=(int16_t)(steer*4096);
    command.throttle=(int16_t)(throttle*(command.flags&2?4096:1));
    command.brake=(int16_t)(brake*(command.flags&4?4096:1));
    command.reverse=(int16_t)(pad.action[GT4_PAD_REVERSE]*(command.flags&8?4096:1));
    command.handbrake=(int16_t)(hand*(command.flags&16?4096:1));
    command.special=pad.action[GT4_PAD_SPECIAL]>0;
    if(pad.action[GT4_PAD_GHOST]>0 && !g->ghost_button)g->ghost_hidden=!g->ghost_hidden;
    g->ghost_button=pad.action[GT4_PAD_GHOST]>0;
    if(g->inspect_on){memset(&command,0,sizeof command);command.device=1;}
    if(race_game_bspec(g) && player && !g->replaying){
        /* B-Spec: the pad as the monitor's controller reads it (00426AF8
           pressed, 00426AF0 held): R1 0x1, R2 0x8000, Left/Right 0x4/0x8
           (with L1 held 0x100/0x200), Up/Down 0x20/0x40, L1 0x80,
           Triangle 0x10000, Circle 0x20000, Cross 0x2000 (the live view's
           camera) and 0x400 (the monitor's map), Square 0x1000. The help
           lines name them (Tool Tip1, RaceMonitorToolTip1). A: the
           controller's own mapping not read; the keyboard stands in with
           the arrows, Tab (R1), O, P, V, M and Shift (L1). */
        uint32_t b=connected?state.buttons:0,held=0;
        if(keys[SDL_SCANCODE_LEFT])b|=1u<<GT4_PAD_LEFT;
        if(keys[SDL_SCANCODE_RIGHT])b|=1u<<GT4_PAD_RIGHT;
        if(keys[SDL_SCANCODE_UP])b|=1u<<GT4_PAD_UP;
        if(keys[SDL_SCANCODE_DOWN])b|=1u<<GT4_PAD_DOWN;
        if(keys[SDL_SCANCODE_TAB])b|=1u<<GT4_PAD_R1;
        if(keys[SDL_SCANCODE_O])b|=1u<<GT4_PAD_TRIANGLE;
        if(keys[SDL_SCANCODE_P])b|=1u<<GT4_PAD_CIRCLE;
        if(keys[SDL_SCANCODE_V] || keys[SDL_SCANCODE_M])b|=1u<<GT4_PAD_CROSS;
        if(keys[SDL_SCANCODE_LSHIFT])b|=1u<<GT4_PAD_L1;
        int l1=(b>>GT4_PAD_L1)&1;
        if(b&(1u<<GT4_PAD_R1))held|=RACE_BSPEC_R1;
        if(b&(1u<<GT4_PAD_R2))held|=RACE_BSPEC_R2;
        if(l1)held|=RACE_BSPEC_L1;
        if(b&(1u<<GT4_PAD_LEFT))held|=l1?RACE_BSPEC_L1_LEFT:RACE_BSPEC_LEFT;
        if(b&(1u<<GT4_PAD_RIGHT))held|=l1?RACE_BSPEC_L1_RIGHT:RACE_BSPEC_RIGHT;
        if(b&(1u<<GT4_PAD_UP))held|=RACE_BSPEC_UP;
        if(b&(1u<<GT4_PAD_DOWN))held|=RACE_BSPEC_DOWN;
        if(b&(1u<<GT4_PAD_TRIANGLE))held|=RACE_BSPEC_OVERTAKE;
        if(b&(1u<<GT4_PAD_CIRCLE))held|=RACE_BSPEC_PIT;
        if(b&(1u<<GT4_PAD_CROSS))held|=RACE_BSPEC_VIEW|RACE_BSPEC_MAP;
        if(b&(1u<<GT4_PAD_SQUARE))held|=RACE_BSPEC_VIEW_BACK;
        g->bspec_pressed|=held&~g->bspec_buttons;g->bspec_buttons=held;
        memset(&command,0,sizeof command);command.device=1;
    }
}

/* B-Spec's orders through 003F1BD8 for the player's car */
static void bspec_order(void *user,int kind,int value)
{
    race_game *g=user;
    if(!g->run)return;
    uint32_t car=gt4_race_run_car(g->run,0);
    if(car)gt4_bspec_order(gt4_race_run_conductor(g->run),GT4_U8(car+0x104+0x440),kind,value);
}
/* the monitor's sounds (+0x9C): 2 a move, 6 an order, 3 refused. A: the
   names taken from the menu's set */
static void bspec_sound(void *user,int id)
{
    race_game *g=user;
    race_audio_menu_sound(g->audio,id==2?"cursor":id==6?"ok":"disable",g->se_volume);
}
/* the movie's extrenders: the watched car's picture (current_car) */
/* func_003DC7B0: a car's colour in each section of the lap (the session's
   0x150 the count): grey, darker in the odd ones, without a time there
   (003FA3D0: cond+0xD7A4, 16 a car), else by how many cars were quicker
   through it, red the quickest down to dark blue */
static int bspec_sectors(race_game *g,int car,uint32_t colors[16])
{
    uint32_t cond=gt4_race_run_conductor(g->run);
    int n=(int)gt4_session(cond,0x150,0,0,0,0,0,NULL),cars=gt4_race_run_cars(g->run);
    if(n>16)n=16;
    for(int s=0;s<n;s++){
        uint32_t t=GT4_U32(cond+0xd7a4u+(uint32_t)(car*16+s)*4);int quicker=0;
        if(t==0x157529ffu){colors[s]=s&1?0x80181818u:0x80282828u;continue;}
        for(int k=0;k<cars;k++)quicker+=GT4_U32(cond+0xd7a4u+(uint32_t)(k*16+s)*4)<t;
        float f=cars>1?(float)quicker/(float)(cars-1):0,e=1-f,
              r=e*255,gg=e*0.2f*255,b=(f*0.4f+e*0.2f)*255;   /* 0x3ECCCCCC, 0x3E4CCCCC */
        colors[s]=0x80000000u|(uint32_t)(int)b<<16|(uint32_t)(int)gg<<8|(uint32_t)(int)r;
    }
    return n<0?0:n;
}

static void bspec_ext(void *user,const char *path,float x,float y,float w,float h,const uint8_t color[4])
{
    race_game *g=user;
    const char *leaf=strrchr(path,'.');leaf=leaf?leaf+1:path;
    if(!strcmp(leaf,"current_car"))race_view_draw_icon_front(g->view,race_bspec_selected(g->bspec_ui),x,y,w,h);   /* 00393790 mode 0 */
    else if(!strcmp(leaf,"cargap") && !strncmp(path,"CARPOS_",7)){
        /* the strip's car: the one whose grid letter names its CARPOS */
        uint32_t cond=gt4_race_run_conductor(g->run),entries=GT4_U32(GT4_U32(GT4_U32(cond)+0x60)+8);
        for(int i=0;i<gt4_race_run_cars(g->run);i++)
            if((int)GT4_U32(GT4_U32(entries+(uint32_t)i*4)+0x3444)==path[7]-'A'){race_view_draw_icon(g->view,i,x,y,w,h);break;}
    }
    else if(!strcmp(leaf,"chip") && !strncmp(path,"BAR",3)){
        /* 003D7930: the row's car, its paint chip (003D87F0 -> 00445AE8) */
        uint32_t chip[4];int car=race_bspec_row(g->bspec_ui,atoi(path+3)-1);
        if(car>=0 && gt4_race_run_paint_chip(g->run,car,chip))race_hud_draw_chip(chip,x,y,w,h,color[3]/128.f);
    }
    else if(!strcmp(leaf,"sector") && !strncmp(path,"BAR",3)){
        int car=race_bspec_row(g->bspec_ui,atoi(path+3)-1);uint32_t colors[16];
        if(car>=0)race_hud_draw_sectors(colors,bspec_sectors(g,car,colors),GT4_U8(gt4_race_run_car(g->run,car)+0x5b2),x,y,w,h,color[3]/128.f);
    }
    else if(!strcmp(leaf,"tire") && g->bspec_hud)race_hud_draw_tire_panel(g->hud,g->bspec_hud,x,y,w,h,color[3]/128.f);
    else if(!strcmp(leaf,"map") && g->bspec_hud){
        int letter[6]={0};uint32_t cond=gt4_race_run_conductor(g->run),entries=GT4_U32(GT4_U32(GT4_U32(cond)+0x60)+8);
        for(int i=0;i<6 && i<gt4_race_run_cars(g->run);i++)letter[i]=(int)GT4_U32(GT4_U32(entries+(uint32_t)i*4)+0x3444);
        race_hud_draw_map_cars(g->hud,g->bspec_hud,x+w*.5f,y+h*.5f,fminf(w,h),letter,race_bspec_selected(g->bspec_ui),color[3]/128.f);
    }
}

/* func_003FA508: car a closes on car b ahead: at the last two sections a
   passed, the time it lost to b (the race's section times: cond+0xD40C
   the count, +0xD48C the cars, +0xCDEC their times) is less at the
   later one */
static int bspec_pace_up(uint32_t cond,int a,int b)
{
    uint32_t sa=cond+0x6698u+(uint32_t)a*0x10a0+0x104,base=cond+0xc;
    if(!(int16_t)GT4_U16(sa+0x4ac) || (GT4_U32(sa+0x4ac)&0xffffffu)==1)return 0;
    int sections=(int)gt4_session(cond,0x150,0,0,0,0,0,NULL),gap[2]={0,0};
    int sec=(int)GT4_U8(sa+0x4ae)-1;
    for(int pass=0;pass<2;pass++,sec--){
        if(sec<0)sec=sections-1;
        if(sec<0)return 0;
        uint32_t ta=0x157529ffu,tb=0x157529ffu;int n=(int8_t)GT4_U8(base+0xd400u+(uint32_t)sec);
        for(int k=0;k<n;k++){
            int id=(int8_t)GT4_U8(base+0xd480u+(uint32_t)sec*6+(uint32_t)k);uint32_t t=GT4_U32(base+0xcde0u+(uint32_t)sec*24+(uint32_t)k*4);
            if(id==a)ta=t;else if(id==b)tb=t;
        }
        if(ta==0x157529ffu || tb==0x157529ffu)return 0;
        gap[pass]=(int)(ta-tb);
        if(!pass && gap[0]<=0)return 0;
    }
    return gap[1]>0 && gap[0]<gap[1];
}

/* B-Spec's display each frame: the race's state as 003D8DB8 reads it */
static void bspec_frame(race_game *g,double dt)
{
    /* A pit-menu takeover posts event 10 through 0035BF08. Observe its
       native world mode after the event, rather than the start descriptor. */
    if(g->run){int mode=race_game_driver_mode(g);g->run_bspec=mode==1 || mode==3;}
    if(!race_game_bspec(g) || g->replaying || !g->run){race_bspec_close(g->bspec_ui);g->bspec_ui=NULL;return;}
    if(!g->bspec_ui){
        race_bspec_host h={bspec_order,bspec_sound,bspec_ext,g};
        g->bspec_ui=race_bspec_open(g->vol,&h,0);
        if(!g->bspec_ui)return;
        if(getenv("OPENGT4_RACE_BSPEC_MONITOR"))g->bspec_pressed|=RACE_BSPEC_R1;   /* tests: straight into the Race Monitor */
    }
    uint32_t cond=gt4_race_run_conductor(g->run),race=GT4_U32(cond),entries=GT4_U32(GT4_U32(race+0x60)+8);
    race_bspec_state st;memset(&st,0,sizeof st);
    int n=gt4_race_run_cars(g->run);if(n>RACE_BSPEC_CARS)n=RACE_BSPEC_CARS;
    int rank[RACE_BSPEC_CARS];
    for(int i=0;i<n;i++){
        gt4_race_hud c;race_bspec_car *k=&st.car[i];
        uint32_t car=gt4_race_run_car(g->run,i),b=car+0x104,entry=GT4_U32(entries+(uint32_t)i*4);
        if(!gt4_race_run_hud(g->run,i,&c))continue;
        rank[i]=c.rank;
        k->index=i;k->grid=(int)GT4_U32(entry+0x3444);
        snprintf(k->name,sizeof k->name,"%s",g->names[i]);
        k->lap=c.lap;k->last_lap=c.last_lap;k->best_lap=c.best_lap;
        k->run_laps=(int16_t)GT4_U16(car+0x5b0);k->distance=GT4_F32(car+0x5a8);
        for(int e=0;e<3;e++){   /* func_003FA4B0: the race's pit plan (+0xD9F0 the laps, +0xDA20 the tyres) */
            uint32_t at=(uint32_t)(i*4+e)*2;
            k->pit_lap[e]=(int16_t)GT4_U16(cond+0xd9f0u+at);   /* lh: 0xFFFF none */
            k->pit_tire[e][0]=GT4_U8(cond+0xda20u+at);k->pit_tire[e][1]=GT4_U8(cond+0xda21u+at);
        }
        k->diff=GT4_U32(entry+0x11e8);k->diff_laps=(int32_t)GT4_U32(entry+0x11ec);
        k->pits=GT4_U16(cond+0xd9e4u+(uint32_t)i*2);              /* func_003FA498 */
        /* func_003DA6F8: 3 in the pit (0035BBD8; A: its sequence's cases
           read as the pit flag), 2 out of fuel (00354E38/00354E68), 4 a
           penalty (0035FC80), 5 all four wheels off the course (003F4EA0) */
        k->in_pit=GT4_U8(b+0x467) && (GT4_U8(b+0x4b1)&0x10);
        if(k->in_pit)k->state=3;
        else if(GT4_F32(car+0x5d0)>0 && GT4_F32(b+0x4cc)>0 && GT4_F32(b+0x4d0)<=0)k->state=2;
        else if(!c.finished && GT4_U16(car+0x712))k->state=4;
        else{int off=1;for(int w=0;w<4;w++)if(!gt4_ground_off_road(car+0x7b8+(uint32_t)w*0x34))off=0;k->state=off?5:0;}
        if(!i){st.laps=c.laps;}
    }
    /* func_003B6FD0's order: the race's places */
    for(int a=0;a<n;a++)for(int z=a+1;z<n;z++)if(rank[z]<rank[a]){
        int t=rank[a];rank[a]=rank[z];rank[z]=t;race_bspec_car x=st.car[a];st.car[a]=st.car[z];st.car[z]=x;}
    for(int r=1;r<n;r++)st.car[r].pace_up=bspec_pace_up(cond,st.car[r].index,st.car[r-1].index);
    st.cars=n;st.player=0;
    st.course_length=GT4_F32(GT4_U32(GT4_U32(GT4_U32(cond+0xcbd8u)+4)+0xdc)+0x14);   /* func_00395400 */
    st.feet=g->unit==1;
    st.clock=GT4_U32(cond+0xf898u)/3;                                  /* func_00358440 */
    uint32_t pcar=gt4_race_run_car(g->run,0);
    st.pace=GT4_U8(pcar+0x14+0x6d);st.overtake=GT4_U8(pcar+0x14+0x6e);st.pit=(GT4_U8(pcar+0x104+0x4b1)&8)!=0;
    /* 003D9998 reads the watched entry, not just the menu visibility. */
    st.pit_sequence=gt4_pit_stage(gt4_race_run_car(g->run,watched_car(g)));
    race_bspec_update(g->bspec_ui,&st,g->bspec_pressed,g->bspec_buttons,dt);
    g->bspec_pressed=0;
}

void race_game_inspect_toggle(race_game *g)
{
    if(!g || !g->ready || g->split)return;
    if(g->cinema.on)race_game_cinematic_toggle(g);
    g->inspect_on=!g->inspect_on;g->inspect_seeded=0;
    fprintf(stderr,"race: inspection camera %s (F3; WASD, Q/E, arrows, Shift)\n",g->inspect_on?"on":"off");
}

/* P: native diagnostic controls requested by the user, not PS2 camera data.
   Speed 3 m/s (Shift 15), turn 1.5 rad/s and pitch limit are PC choices.
   No course/car collision query; the view starts at the active PS2 camera. */
static void inspect_camera(race_game *g,double dt)
{
    if(!g->inspect_on)return;
    if(!g->inspect_seeded) {
        float rows[9];
        if(!race_view_get_camera(g->view,g->inspect_eye,rows,g->inspect_projection))return;
        g->inspect_yaw=atan2f(-rows[6],-rows[8]);
        g->inspect_pitch=asinf(fmaxf(-1,fminf(1,-rows[7])));g->inspect_seeded=1;
    }
    const Uint8 *keys=SDL_GetKeyboardState(NULL);float step=(float)fmax(0,fmin(dt,.1));
    g->inspect_yaw+=1.5f*step*(keys[SDL_SCANCODE_LEFT]-keys[SDL_SCANCODE_RIGHT]);
    g->inspect_pitch+=1.5f*step*(keys[SDL_SCANCODE_UP]-keys[SDL_SCANCODE_DOWN]);
    g->inspect_pitch=fmaxf(-1.55f,fminf(1.55f,g->inspect_pitch));
    float sy=sinf(g->inspect_yaw),cy=cosf(g->inspect_yaw),sp=sinf(g->inspect_pitch),cp=cosf(g->inspect_pitch);
    float forward[3]={sy*cp,sp,cy*cp},right[3]={-cy,0,sy};
    float rows[9]={right[0],0,right[2],-sy*sp,cp,-cy*sp,-forward[0],-forward[1],-forward[2]};
    float speed=step*((keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT])?15.f:3.f);
    float ahead=keys[SDL_SCANCODE_W]-keys[SDL_SCANCODE_S],side=keys[SDL_SCANCODE_D]-keys[SDL_SCANCODE_A];
    float lift=keys[SDL_SCANCODE_E]-keys[SDL_SCANCODE_Q],length=sqrtf(ahead*ahead+side*side+lift*lift);
    if(length>1)speed/=length;
    for(int k=0;k<3;k++)g->inspect_eye[k]+=speed*(forward[k]*ahead+right[k]*side+(k==1?lift:0));
    race_view_set_camera(g->view,g->inspect_eye,rows,g->inspect_projection[0],g->inspect_projection[1],g->inspect_projection[2]);
    race_view_show_car(g->view,1);g->start_cam_on=0;
}

/* P: authored PC camera moves for the user's native showcase, enabled only
   by OPENGT4_RACE_FILM=orbit/crane/low. Reads the live pose and changes the
   camera/listener, never the car, controls, simulation speed or replay tape.
   Distances, lens and trajectories are deliberate filming choices, not PS2
   camera data. Clock starts after the race opening (10 seconds). */
static void film_camera(race_game *g,double dt)
{
    if(g->cinema.on)return;
    const char *mode=getenv("OPENGT4_RACE_FILM");
    if(!mode || g->split || (strcmp(mode,"orbit") && strcmp(mode,"crane") && strcmp(mode,"low")))return;
    g->film_clock+=dt;
    float t=(float)fmax(0,g->film_clock-10),pos[3],rot[9],wheels[4][3];
    if(!gt4_race_run_pose(g->run,0,pos,rot,wheels))return;
    float local[3];
    if(!strcmp(mode,"crane")){
        float q=fminf(1,t/24),ease=q*q*(3-2*q);
        local[0]=4+5*ease;local[1]=1.5f+20*ease;local[2]=-3+10*ease;
    }else if(!strcmp(mode,"low")){
        local[0]=3.6f;local[1]=.75f;local[2]=-2.2f+4.4f*(.5f-.5f*cosf(t*.12f));
    }else{
        float angle=-1.3f+t*.11f;
        local[0]=5.2f*sinf(angle);local[1]=1.4f+.7f*(1-cosf(t*.1f));local[2]=5.2f*cosf(angle);
    }
    float eye[3],target[3],f[3],rows[9],length=0;
    for(int k=0;k<3;k++){
        eye[k]=pos[k];for(int j=0;j<3;j++)eye[k]+=rot[k*3+j]*local[j];
        target[k]=pos[k]+rot[k*3+1]*.65f;f[k]=target[k]-eye[k];length+=f[k]*f[k];
    }
    length=sqrtf(length);if(length<.001f)return;
    for(int k=0;k<3;k++)f[k]/=length;
    length=sqrtf(f[0]*f[0]+f[2]*f[2]);if(length<.001f)return;
    rows[0]=-f[2]/length;rows[1]=0;rows[2]=f[0]/length;
    rows[3]=-f[1]*rows[2];rows[4]=f[0]*rows[2]-f[2]*rows[0];rows[5]=f[1]*rows[0];
    for(int k=0;k<3;k++)rows[6+k]=-f[k];
    race_view_set_camera(g->view,eye,rows,1.5f,1.95183f,.1f);
    race_view_show_car(g->view,1);g->start_cam_on=0;
}

static int rolling_phase(race_game *g);
static int tv_camera(race_game *g);
/* the car the cameras follow: the replay's (up/down), B-Spec's (its
   monitor's +0x64), else the player's */
static int watched_car(const race_game *g)
{
    if (g->bspec_ui) return race_bspec_selected(g->bspec_ui);
    return g->replaying ? g->focus : 0;
}
static int dive_camera(race_game *g);
#include "race_cinematic.inc"

/* The standing start's camera (CameraPathanim, func_0037D620): its
   clock a second ahead of the start's (the frames to it less 60), the
   motion by the time left (D_00621270: under 1 s the third, under 2 the
   second, else the first, held at its start until 9 s are left), each
   played from its start when chosen; the first in the frame of the
   pole's grid place (func_003FCA08: its heading turned half round), the
   others in the car's (its place and heading). The motion's eye, target
   and up (func_003EBCE8), its field of view (radians) the projection's. */
static void license_start_step(race_game *g)
{
    if(!g->last_prepared || !license_exam(g) || g->replaying)return;
    const gt4_course *c=race_view_course(g->view);if(!c)return;
    if(!g->license_start_tried){
        g->license_start_tried=1;uint32_t at=0;if(c->size>=0xd8)memcpy(&at,c->data+0xd4,4);
        if(at && at<c->size)g->license_start_motion=gt4_license_motion_open_camera(c->data+at,c->size-at);
        if(!g->license_start_motion || !gt4_license_motion_select(g->license_start_motion,3,"replay_start_camera1"))return;
        gt4_race_run_place(g->run,0,g->license_start_origin,g->license_start_origin+3);
        /* 00374720 selects kind 4 for a live license before start;
           003749A0 passes opening=1 to 0037D338: +60, no grid frame. */
        unsigned kind=g->last_event_row[0x40]+1;
        int seconds=kind==2?8:kind==3?10:kind==4?0:g->last_event_row[0x41];
        g->license_start_left=seconds*60+60;
    }
    if(g->license_start_motion && --g->license_start_left>=0)
        gt4_license_motion_step_control(g->license_start_motion,3,gt4_f32(0x3c888888),
            gt4_license_opening_control(g->license_start_left));
}
static int start_camera(race_game *g)
{
    if(g->last_prepared && license_exam(g) && !g->replaying){
        float look[9],fov,cc,tc;
        /* 00374720 +00..10: leave kind 4 when race+BC reaches zero,
           even though its internal opening clock includes another 60. */
        gt4_race_hud h;if(!gt4_race_run_hud(g->run,0,&h) || h.start_frames<=0)return 0;
        return g->license_start_left>0 && g->license_start_motion &&
            gt4_license_motion_camera(g->license_start_motion,look,&fov,&cc,&tc) &&
            license_look(g,look,fov,g->license_start_origin);
    }
    /* func_0037D5B0: the motion by the seconds left, D_00621270 for the
       start, D_00621290 for the replay's opening (+0x1D0 set) */
    static const float start_until[3] = { 1, 2, 9 }, replay_until[5] = { 1, 2, 3, 4, 11 };
    const float *until = g->replaying ? replay_until : start_until;
    const int motions = g->replaying ? 5 : 3;
    gt4_race_hud h;
    const gt4_course *course = race_view_course(g->view);
    float t, local = 0, look[9], fov, ref[3], yaw, c, s, eye[3], target[3], up[3], rows[9];
    int id = 1;
    if (!course || !gt4_race_run_hud(g->run, 0, &h) || h.rolling) return 0;
    /* func_0037D338: the clock +0x18C is the frames to the start less 60,
       for the replay's opening (+0x1D0) plus 60, then one less a frame
       (func_0037D620): the opening runs on a second past the start */
    if (g->replaying) {
        if (g->replay_open < 0) {   /* armed with the countdown */
            if (h.start_frames <= 0) return 0;
            g->replay_open = (long)h.start_frames + 60 + (long)g->tape_at;
        }
        t = (float)(g->replay_open - (long)g->tape_at) / 60;
        if (t <= 0) return 0;
    } else {
        if (h.start_frames <= 60) return 0;
        t = (float)(h.start_frames - 60) / 60;
    }
    if (!g->start_cam_tried) {
        /* the course header's Mot1 banks: +0xD0 the start's, +0xD4 the
           replay's opening (func_0037D620 at 0x0037D734 by +0x1D0) */
        unsigned o = g->replaying ? 0xd4 : 0xd0;
        uint32_t at;
        g->start_cam_tried = 1;
        at = course->size >= 0x100 ? (uint32_t)(course->data[o] | course->data[o + 1] << 8 | course->data[o + 2] << 16 | (uint32_t)course->data[o + 3] << 24) : 0;
        if (at && at < course->size) g->start_cam = gt4_motion_bank_open(course->data + at, course->size - at);
    }
    if (!g->start_cam) return 0;
    for (int k = 0; k < motions; k++)
        if (t < until[k]) { id = motions - k; local = until[k] - t; break; }
    if (!gt4_motion_bank_camera(g->start_cam, id - 1, local, look, &fov)) return 0;
    /* func_0037D338: entry+0x3444 bit 0 sets +0x1D1; func_0037D620
       mirrors local x on the car-relative shots, never the grid shot.
       The replay's opening (+0x1D0) skips both: no mirror, every motion
       in the car's frame. */
    if (!g->replaying && id != 1 && (h.grid_slot & 1))
        for (int k = 0; k < 3; k++) look[3 * k] = -look[3 * k];
    if (id == 1 && !g->replaying) {
        gt4_grid p = gt4_course_grid(course, 0);
        ref[0] = p.pos[0];
        ref[1] = p.pos[1];
        ref[2] = p.pos[2];
        gt4_course_height(course, p.pos[0], p.pos[2], p.pos[1] + 2, &ref[1]);
        yaw = p.heading + 3.14159265f;
    } else if (!gt4_race_run_place(g->run, 0, ref, &yaw)) return 0;
    /* world = the reference + Ry(yaw) * local (the view's Ry(-yaw)) */
    c = cosf(yaw);
    s = sinf(yaw);
    for (int k = 0; k < 3; k++) {
        const float *v = look + 3 * k;
        float *o = k == 0 ? eye : k == 1 ? target : up;
        float x = c * v[0] + s * v[2], z = -s * v[0] + c * v[2];
        o[0] = x + (k < 2 ? ref[0] : 0);
        o[1] = v[1] + (k < 2 ? ref[1] : 0);
        o[2] = z + (k < 2 ? ref[2] : 0);
    }
    {   /* the view's rows: right, up, back */
        float b[3] = { eye[0] - target[0], eye[1] - target[1], eye[2] - target[2] }, r[3], u[3], n;
        n = sqrtf(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]);
        if (n < 1e-6f) return 0;
        for (int k = 0; k < 3; k++) b[k] /= n;
        r[0] = up[1] * b[2] - up[2] * b[1];
        r[1] = up[2] * b[0] - up[0] * b[2];
        r[2] = up[0] * b[1] - up[1] * b[0];
        n = sqrtf(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
        if (n < 1e-6f) return 0;
        for (int k = 0; k < 3; k++) r[k] /= n;
        u[0] = b[1] * r[2] - b[2] * r[1];
        u[1] = b[2] * r[0] - b[0] * r[2];
        u[2] = b[0] * r[1] - b[1] * r[0];
        for (int k = 0; k < 3; k++) { rows[k] = r[k]; rows[3 + k] = u[k]; rows[6 + k] = b[k]; }
    }
    if (getenv("OPENGT4_START_CAM_LOG") && h.start_frames % 30 == 0)   /* tests */
        fprintf(stderr, "start cam: %d frames, motion %d at %.3f, ref %.2f %.2f %.2f yaw %.3f, local eye %.2f %.2f %.2f target %.2f %.2f %.2f, fov %.3f\n",
                h.start_frames, id, local, ref[0], ref[1], ref[2], yaw, look[0], look[1], look[2], look[3], look[4], look[5], fov);
    {
        float sx = 1 / tanf(fov);
        race_view_set_camera(g->view, eye, rows, sx, sx * 1.30122f, 0.1f);
        race_view_show_car(g->view, 1);
    }
    return 1;
}

/* 00385478 / 0040CA58 / 0040D0A0: copy the driver's presentation inputs.
   The physics' human record is not written; race_view owns its controller. */
static void update_drivers(race_game *g)
{
    for(int i=0;i<=g->others;i++) {
        uint32_t car=gt4_race_run_car(g->run,i);if(!car)continue;
        uint32_t params=GT4_U32(car+CAR_PARAMS);gt4_driver_animation_input in={0};
        int transmission[4];for(int k=0;k<4;k++)transmission[k]=(int32_t)GT4_U32(car+0x1080+k*4);
        in.dt=GT4_F32(car+CAR_DT);in.gear=GT4_U8(car+CAR_GEAR);
        in.steering=-GT4_F32(car+0x768)*gt4_f32(0x3ecccccc);
        in.steering_fraction=GT4_F32(car+CAR_56C);
        in.throttle=GT4_F32(car+CAR_THROTTLE_REQ);in.brake=GT4_F32(car+CAR_BRAKE_PEDAL);
        in.lever_pedal=GT4_F32(car+0x598);   /* 003446A8, type-five lever */
        int count=GT4_U8(params+PARAMS_DRIVE+2),rpm=(int)GT4_F32(car+CAR_TACHO);
        int up=in.gear==0 || in.gear==count?99999:(int)gt4_shift_rpm(car);
        int down=in.gear<2?-99999:GT4_U16(params+0x260+in.gear*2);
        if((int8_t)GT4_U8(car+CAR_56A)==1 && in.gear>=2)down=(down*GT4_U8(car+0x83))>>7;
        in.upshift=(in.gear==1?rpm>up-1000 && in.throttle>gt4_f32(0x3e4ccccc):rpm>up-300 && in.throttle>gt4_f32(0x3f333333));
        int speed_gear=gt4_gear_for_speed(car,fminf(GT4_F32(car+0x38),GT4_F32(car+0x3c)));
        if(in.gear==speed_gear)in.downshift=rpm<down-1000;
        else if(in.throttle>0)in.downshift=rpm<down;
        else if(in.brake==0)in.downshift=rpm<down+500;
        else {in.downshift=rpm<down+1000;in.speed_shift=speed_gear>0 && speed_gear<in.gear;}
        race_view_step_driver(g->view,(unsigned)i,transmission,&in);
    }
}

/* 003D73A0 -> 00344E48 supplies current/previous wheel angles to the
   renderer; its 003D73F8 exposure is read from the course in race_view.
   Read presentation inputs only. No physics fields are changed. */
static void update_wheel_motion(race_game *g,int entry,const float rot[9])
{
    uint32_t car=gt4_race_run_car(g->run,entry);if(!car)return;
    float current[4]={0},previous[4]={0};
    for(int k=0;k<gt4_car_wheels(car) && k<4;k++) {
        uint32_t w=gt4_car_wheel(car,k);float rr[3]={GT4_F32(w+WHEEL_MOUNT),GT4_F32(w+WHEEL_MOUNT+8),-GT4_F32(w+WHEEL_MOUNT+4)};
        float x=rot[0]*rr[0]+rot[3]*rr[1]+rot[6]*rr[2],z=rot[2]*rr[0]+rot[5]*rr[1]+rot[8]*rr[2];
        int slot=(z>0?2:0)+(x>0?0:1); /* same render slots as race_run_pose */
        current[slot]=GT4_F32(w+WHEEL_ROTATION);previous[slot]=GT4_F32(car+CAR_WHEEL_TURNS+k*4);
    }
    race_view_set_wheel_motion(g->view,(unsigned)entry,current,previous,GT4_F32(car+0x554));
}

/* the pause menu (RacePause): Escape or the pad's Start holds the race;
   the arrows and Enter (the pad's D-pad and A) go to its movie. A: the
   game's own pause button handling (Start again, Triangle) is not read;
   Escape / Start in the menu continues. */
static void pause_sound(void *user, const char *name)
{
    race_game *g = user;
    race_audio_menu_sound(g->audio, name, g->se_volume);
}
static void pause_toggle(race_game *g)
{
    if (g->replaying) { g->held = !g->held; return; }   /* the replay: its own hold */
    if (g->pause) { race_pause_close(g->pause); g->pause = NULL; race_audio_pause(g->audio, 0); return; }
    if ((!g->split && g->finished >= 0) || g->result) return;
    /* the Arcade's RacePause as every capture holds it: +0x28 retry 0,
       +0x30 screen setting 1, +0x2C confirm 0. The replay's the same
       (A: no capture of a replay's; func_003C2D80 makes every pause with
       retry 0, and only the race worlds set it) */
    /* 003BBF90 +0x110 sets License RacePause +0x28 to 1. The movie
       remains common/pause.strb (003C31C8, 006A2A60), not pause_license. */
    /* 003EDE50 / 003EF498 (the GT Mode event and championship worlds) set
       +0x2C to 1 unless D_00620208 (the demonstration, 0010CA50) is set:
       Exit asks to confirm. */
    int confirm = (!g->split && g->last_prepared && g->last_desc.event_row && !g->last_desc.license);
    g->pause = race_pause_open(g->vol, g->split || (g->last_prepared && license_exam(g)), !g->split, confirm, pause_sound, g);
    if (g->pause) {
        race_audio_pause(g->audio, 1);   /* 0038B428: the world's vt+0x2D0 */
        plat_input_set_ffb(0,0);plat_input_set_ffb(1,0);
    }
}

/* the replay's buttons: 0 Square, 1 Circle, 2 Cross, 3 L3, 4 up, 5 down, 6 Triangle */
static void replay_button(race_game *g, int b)
{
    static const char *const modes[3] = { "ReplayModeNormal", "ReplayModeDive", "ReplayModeNormalDisplay" };
    int cars = gt4_race_run_cars(g->run);
    switch (b) {
    case 0: g->rmode = (g->rmode + 1) % 3; g->label = modes[g->rmode]; break;
    case 1: g->bgm_off = !g->bgm_off; g->label = g->bgm_off ? "ReplayModeBGMOff" : "ReplayModeBGMOn"; break;
    case 2: g->car_cam = !g->car_cam; if (g->tvcam) gt4_tvcam_reset(g->tvcam); return;
    case 3: g->fast = !g->fast; g->label = g->fast ? "ReplayModeAccelerate" : "ReplayModeNormalSpeed"; break;
    case 6: g->zoom_on = !g->zoom_on; return;
    case 4: case 5:
        if (cars > 0) g->focus = (g->focus + (b == 4 ? cars - 1 : 1)) % cars;
        if (g->tvcam) gt4_tvcam_reset(g->tvcam);
        return;
    default: return;
    }
    g->label_t = 2.75;   /* func_003A98E8: at once, 2.75 s, in and out in a quarter (rv_01..06) */
    g->label_out = 0;
}

void race_game_key(race_game *g, int key)
{
    if (!g || !g->ready) return;
    if(g->result && g->last_prepared && license_exam(g)){
        int code=key==SDLK_LEFT?37:key==SDLK_UP?38:key==SDLK_RIGHT?39:key==SDLK_DOWN?40:key==SDLK_ESCAPE?27:0;
        if(code)race_result_key(g->result,code);return;
    }
    if (g->replaying) {   /* the replay: Select (Backspace, P) holds it, Start (Escape) leaves */
        if (key == SDLK_BACKSPACE || key == SDLK_p) g->held = !g->held;
        else if (key == SDLK_ESCAPE) g->quit = 1;
        else replay_button(g, key == SDLK_d ? 0 : key == SDLK_m ? 1 : key == SDLK_RETURN || key == SDLK_KP_ENTER ? 2
                              : key == SDLK_f ? 3 : key == SDLK_UP ? 4 : key == SDLK_DOWN ? 5 : key == SDLK_z ? 6 : -1);
        return;
    }
    if (key == SDLK_ESCAPE) { pause_toggle(g); return; }
    if (!g->pause && race_pitmenu_up(g->pitmenu)) {
        int c = key == SDLK_LEFT ? 37 : key == SDLK_UP ? 38 : key == SDLK_RIGHT ? 39 : key == SDLK_DOWN ? 40
              : key == SDLK_RETURN || key == SDLK_KP_ENTER ? 13 : 0;
        if (c) { race_pitmenu_key(g->pitmenu, c); g->pit_touched |= 1; }
        return;
    }
    if (!g->pause) return;
    int code = key == SDLK_LEFT ? 37 : key == SDLK_UP ? 38 : key == SDLK_RIGHT ? 39 : key == SDLK_DOWN ? 40
             : key == SDLK_RETURN || key == SDLK_KP_ENTER ? 13 : 0;
    if (code) race_pause_key(g->pause, code);
}
static void pause_input(race_game *g)
{
    if(g->split){
        plat_pad_state p;int connected=plat_input_poll(1,&p);
        static const int buttons[6]={GT4_PAD_START,GT4_PAD_UP,GT4_PAD_DOWN,GT4_PAD_LEFT,GT4_PAD_RIGHT,GT4_PAD_CROSS};
        static const int codes[6]={0,38,40,37,39,13};
        for(int k=0;k<6;k++){
            int down=connected && (p.buttons&(1u<<buttons[k]));
            if(down && !g->pause_keys_2p[k]){
                if(k==0)pause_toggle(g);else if(g->pause)race_pause_key(g->pause,codes[k]);
            }
            g->pause_keys_2p[k]=down;
        }
    }
    static const SDL_GameControllerButton button[6] = { SDL_CONTROLLER_BUTTON_START, SDL_CONTROLLER_BUTTON_DPAD_UP,
        SDL_CONTROLLER_BUTTON_DPAD_DOWN, SDL_CONTROLLER_BUTTON_DPAD_LEFT, SDL_CONTROLLER_BUTTON_DPAD_RIGHT, SDL_CONTROLLER_BUTTON_A };
    static const int code[6] = { 0, 38, 40, 37, 39, 13 };
    g->pad=plat_sdl_pad(0);
    SDL_JoystickID id=g->pad?SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(g->pad)):-1;
    if(!g->pad || id!=g->pad_id){
        memset(g->pause_keys,0,sizeof g->pause_keys);memset(g->pad_held,0,sizeof g->pad_held);
        g->replay_back_held=0;g->pad_id=id;
    }
    if (!g->pad) return;
    if (g->replaying) {
        int down = SDL_GameControllerGetButton(g->pad, SDL_CONTROLLER_BUTTON_BACK);
        if (down && !g->replay_back_held) g->held = !g->held;
        g->replay_back_held = down;
        if (SDL_GameControllerGetButton(g->pad, SDL_CONTROLLER_BUTTON_START) && !g->pause_keys[0]) g->quit = 1;
        g->pause_keys[0] = SDL_GameControllerGetButton(g->pad, SDL_CONTROLLER_BUTTON_START);
        {   /* Square, Circle, Cross, L3, up, down, Triangle */
            static const SDL_GameControllerButton b[7] = { SDL_CONTROLLER_BUTTON_X, SDL_CONTROLLER_BUTTON_B,
                SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_LEFTSTICK, SDL_CONTROLLER_BUTTON_DPAD_UP, SDL_CONTROLLER_BUTTON_DPAD_DOWN,
                SDL_CONTROLLER_BUTTON_Y };
            for (int i = 0; i < 7; i++) {
                int down = SDL_GameControllerGetButton(g->pad, b[i]);
                if (down && !g->pad_held[i]) replay_button(g, i);
                g->pad_held[i] = down;
            }
        }
        return;
    }
    for (int i = 0; i < 6; i++) {
        int down = SDL_GameControllerGetButton(g->pad, button[i]);
        if (down && !g->pause_keys[i]) {
            /* 003DF978 +0xd4 -> 003EBF40: the licence result receives
               menu input too. main.c leaves pad edges to this poller;
               without this route only keyboard keys reached Retry/Exit.
               Start remains the existing draw_result confirm shortcut. */
            if (g->result && g->last_prepared && license_exam(g)) {
                if (i) race_result_key(g->result, code[i]);
            }
            else if (i == 0) pause_toggle(g);
            else if (g->pause) race_pause_key(g->pause, code[i]);
            else if (race_pitmenu_up(g->pitmenu)) { race_pitmenu_key(g->pitmenu, code[i]); g->pit_touched |= 1; }
        }
        g->pause_keys[i] = down;
    }
}
int race_game_quit(const race_game *g) { return g && g->quit; }

/* func_0039F0C0: the car at a rank (car+0x5B3), its icon only for a car
   the race drives (func_00366678 3: +0x441 clear); -1 none */
static int icon_at_rank(race_game *g, int rank)
{
    for (int c = 0; c < gt4_race_run_cars(g->run); c++) {
        uint32_t car = gt4_race_run_car(g->run, c);
        if ((int8_t)GT4_U8(car + 0x5b3) != rank) continue;
        return GT4_U8(car + 0x104 + 0x441) ? -1 : c;
    }
    return -1;
}

/* The race's events of this step the display shows for the player (car
   0, the display's +0x2E), as func_0039DBB8 reads the queue at race+0xF4:
   a section passed (0xE, func_0039F368) or a lap (0xD, func_0039F200)
   with that car's time (0x10), and its gap (0x11, func_003A04B0) against
   the leader, or the second when leading (func_003A1030 -> 0039F0C0: the
   car of that rank, car+0x5B3), with that car's icon unless it is a
   ghost (func_00366678). Only the events posted this step (count still
   2: func_003B75D8 ages them each frame) */
static void timing_events(race_game *g)
{
    uint32_t race, q;
    int n, cars, lead;
    if (!g->hud || !g->run) return;
    race = GT4_U32(gt4_race_run_conductor(g->run));
    q = race + 0xf4;
    n = (int)GT4_U32(q + 0x824);
    {   /* func_0039E9B0: each event 4 (the start) of the watched car, or of
           none, with its countdown over: START (003A1488, the race only)
           and the start's sound (D_00621515). Ours stay queued: a new one
           is one more of them */
        int starts = 0, watched = watched_car(g);
        for (int i = 0; i < n && i < 0x100; i++) {
            uint32_t e = q + 8 + (uint32_t)i * 8;
            int32_t who = (int32_t)GT4_U32(e + 4);
            if ((int16_t)GT4_U16(e) == 4 && (who < 0 || who == watched)) starts++;
        }
        if (starts > g->start_events && gt4_race_run_countdown(g->run, watched) <= 0) {
            if (!g->replaying) race_hud_start(g->hud);
            race_audio_start(g->audio);
        }
        g->start_events = starts;
    }
    cars = gt4_race_run_cars(g->run);
    lead = (int8_t)GT4_U8(gt4_race_run_car(g->run, 0) + 0x5b3) == 1;
    for (int i = 0; i < n && i < 0x100; i++) {
        uint32_t e = q + 8 + (uint32_t)i * 8, v = GT4_U32(e + 4);
        int kind = (int16_t)GT4_U16(e);
        if (GT4_U16(e + 2) != 2) continue;
        if (kind == 0x12) {   /* func_003A0308: the player's laps behind the leader, or leading, the second's */
            int second = -1;
            for (int c = 0; c < cars; c++)
                if ((int8_t)GT4_U8(gt4_race_run_car(g->run, c) + 0x5b3) == 2) second = c;
            if (!lead && (v & 15) == 0) race_hud_laps_behind(g->hud, (int32_t)v >> 6, icon_at_rank(g, 1));
            else if (lead && second >= 0 && (int)(v & 15) == second) race_hud_laps_behind(g->hud, -((int32_t)v >> 6), icon_at_rank(g, 2));
            continue;
        }
        if ((v & 15) != 0) continue;
        if (kind == 0x13) {   /* func_003A0600 -> 003A3778: the speed x10 above the car */
            race_hud_top_speed(g->hud, (float)((v >> 4) & 0x3fffu) * 0.1f, g->unit == 1);
            continue;
        }
        if (kind == 0xe || kind == 0xd) {
            uint32_t ms = 0x157529FFu;
            for (int k = 0; k < n; k++) {   /* the first time message (0x10) of the same car */
                uint32_t f = q + 8 + (uint32_t)k * 8, t = GT4_U32(f + 4);
                int32_t x;
                if ((int16_t)GT4_U16(f) != 0x10 || (t & 15) != 0) continue;
                x = (int32_t)t >> 6;                              /* func_003A3680 */
                ms = (uint32_t)x + 0x1fffffeu > 0x3fffffcu ? 0x157529FFu : (uint32_t)x;
                break;
            }
            if (kind == 0xe) race_hud_section_time(g->hud, (int)((v >> 4) & 15) + 1, ms);
            else race_hud_lap_time(g->hud, ms);
        } else if (kind == 0x11) {
            int32_t t = (int32_t)v >> 6;
            if ((uint32_t)t + 0x1fffffeu > 0x3fffffcu) continue;  /* func_003A3620: no time */
            race_hud_gap(g->hud, t, (int)((v >> 4) & 3), icon_at_rank(g, t < 0 ? 2 : 1));   /* func_003A1030 */
        }
    }
}

/* 003B5E80 +0x1FC..+0x37C consumes consecutive camera positions for
   Doppler. Publish the camera after each simulation frame, before its
   sound requests, even when the PC draws several simulation frames at once.
   Camera construction remains the existing original-derived path. */
static void split_camera(race_game *g,int port)
{
    int view=port?g->camera_2p:g->camera;float point[8];
    float *eye=g->split_eye[port],*rows=g->split_rows[port],*projection=g->split_projection[port];
    if(!race_view_camera_point_entry(g->view,port,view,point) ||
       !gt4_race_run_camera(g->run,port,view,point,eye,rows))return;
    if(port?g->look_back_2p:g->look_back){
        for(int k=0;k<3;k++){if(view!=6)eye[k]+=2*(rows[k]*point[0]+rows[6+k]*point[2]);
            rows[k]=-rows[k];rows[6+k]=-rows[6+k];}
    }
    /* 0033FD88 +1DC..260: viewport/fullscreen height-to-width ratio
       times f23 (0032FDA4..AC: 0x3F999999). 004A7890 scales X/Y.
       A: the inherited single-view base lens still comes from captures. */
    float sx=view==0 || view==1?1.73205f:1/tanf(atanf(1/point[6])*4/3);
    float scale=gt4_f32(0x3f999999u);
    projection[0]=sx*.5f*scale;projection[1]=sx*1.30122f*scale;projection[2]=.1f;
    g->split_camera_valid[port]=1;
}
static void split_camera_audio_update(race_game *g)
{
    split_camera(g,0);split_camera(g,1);g->start_cam_on=g->tv_on=0;
    race_view_set_camera(g->view,g->split_eye[0],g->split_rows[0],g->split_projection[0][0],
                         g->split_projection[0][1],g->split_projection[0][2]);
    /* A: shared audio mix retains player 1's listener; no second music. */
    if(!g->audio)g->audio=race_audio_open_replay(g->disc,g->run,race_view_course(g->view),0);
    if(g->audio)for(int i=0;i<gt4_race_run_cars(g->run);i++)race_audio_set_exhaust_count(g->audio,(unsigned)i,race_view_exhaust_count(g->view,i));
    if(g->audio)race_audio_update(g->audio,g->run,g->split_eye[0],g->split_rows[0],g->camera,g->bgm_enabled?g->bgm_volume:-1,g->se_volume);
}
static void race_camera_audio_update(race_game *g,double dt)
{
    if(g->split){split_camera_audio_update(g);return;}
    if(!g->inspect_on || !g->inspect_seeded) {
    g->start_cam_on = getenv("OPENGT4_RACE_SNAPSHOT") ? 0 : start_camera(g);
    if (g->replaying) g->cue = race_audio_music_cue(g->audio, &g->cue_passed);   /* 0033D170, each frame */
    if (g->replaying) g->zoom = gt4_tvcam_zoom_step(g->zoom_spring, g->zoom_on);   /* 003751F0, each frame */
    g->tv_on = !g->start_cam_on && g->replaying && !g->car_cam && g->rmode == 1 && dive_camera(g);   /* Dive */
    {
        float grade[6];
        gt4_dive_filter(g->dive, grade);
        race_view_set_grade(g->view, g->tv_on && g->dive ? grade : NULL);
    }
    if (!g->tv_on)
        g->tv_on = !g->start_cam_on && (rolling_phase(g) || (g->replaying && !g->car_cam) || getenv("OPENGT4_RACE_TV") ||
                   (g->bspec_ui && race_bspec_view(g->bspec_ui) == 2)) && tv_camera(g);
    if (!g->tv_on && g->tvcam) gt4_tvcam_reset(g->tvcam);
    if (!g->start_cam_on && !g->tv_on) {   /* the game's chase camera on the player's car (its model's camera point 1) */
        float point[8], eye[3], rows[9];
        /* the replay's car camera (Cross, the selector's type 2) follows
           the car watched; A: through the player's model's point */
        int watched = watched_car(g), view = g->replaying ? 1 : g->bspec_ui ? (race_bspec_view(g->bspec_ui) == 0 ? 0 : 1) : g->camera;
        if (race_view_camera_point(g->view, view, point) && gt4_race_run_camera(g->run, watched, view, point, eye, rows)) {
            if(!g->replaying && g->look_back && (view==0 || view==1 || view==6)){
                /* 0037B3FC..0037B418: reverse pitch/roll and add pi to
                   yaw. Ry(pi)*view flips rows 0/2 and the translated
                   camera offset around the car, without touching physics.
                   Roof 0037B718..0037B738 keeps the point's position and
                   only reverses the orientation. A: full matrix not compared. */
                for(int k=0;k<3;k++){
                    if(view!=6)eye[k]+=2*(rows[k]*point[0]+rows[6+k]*point[2]);
                    rows[k]=-rows[k];rows[6+k]=-rows[6+k];
                }
            }
            /* the projection: the captures' (0x01CE9860) for the views on
               and behind the car; the others their point's own (+0x18, the
               field's half width at unit depth, func_0037B580), the height
               in the same proportion */
            float sx = 1.73205f;
            if (view != 0 && view != 1)
                /* the point's field (+0x18 its half width at unit depth: 52
                   degrees on the Ka) spread by 4/3, as the roof capture's
                   projection has it (1.44331 for 2.05) */
                sx = 1 / tanf(atanf(1 / point[6]) * 4 / 3);
            race_view_set_camera(g->view, eye, rows, sx, sx * 1.30122f, 0.1f);
        }
        race_view_show_car(g->view, view != 0);
    }
    }
    if(!g->inspect_on || !g->inspect_seeded)license_camera(g);
    if(!g->split && !g->start_cam_on && (!g->inspect_on || !g->inspect_seeded)){
        int entry=watched_car(g);float look[9],fov,origin[4]={0};
        /* 003CD0A0 -> 003CCBD0: return previous seed, then store the
           XOR-swapped next value; it advances on each camera evaluation. */
        int random=(int)g->pit_camera_random;uint32_t next=g->pit_camera_random*17u+17u;g->pit_camera_random=next^((next<<16)|(next>>16));
        if(entry>=0 && entry<6)gt4_pit_camera_pick(g->pit_crew+entry,random);
        if(entry>=0 && entry<6 && gt4_pit_camera(g->pit_crew+entry,look,&fov))license_look(g,look,fov,origin);
    }
    inspect_camera(g,dt);
    film_camera(g,dt);
    if(g->cinema.on){
        race_view_get_camera(g->view,g->cinema.original_eye,g->cinema.original_rows,g->cinema.original_projection);
        g->cinema.original_visible=race_view_car_visible(g->view);
    }
    cinematic_camera(g,dt);
    if(!g->audio)g->audio=race_audio_open_replay(g->disc,g->run,race_view_course(g->view),g->replaying);
    if(g->audio)for(int i=0;i<gt4_race_run_cars(g->run);i++)race_audio_set_exhaust_count(g->audio,(unsigned)i,race_view_exhaust_count(g->view,i));
    if(g->audio){float eye[3],rows[9],projection[3];
        if(race_view_get_camera(g->view,eye,rows,projection))
            race_audio_update(g->audio,g->run,eye,rows,g->start_cam_on || g->tv_on || g->inspect_on || g->cinema.on || getenv("OPENGT4_RACE_FILM") || (watched_car(g)<6 && g->pit_crew[watched_car(g)].camera_control>=0)?-1:g->camera,(g->bgm_enabled!=g->bgm_off)?g->bgm_volume:-1,g->se_volume);
    }
}

/* the world's frame for the pit menu (0038AB98): the watched car's stop
   asks for it (0035BC70: its tyres, its fuel; B-Spec's mode from
   world+0xCC8 1 or 3), it goes when the stop no longer waits (0035BCE8),
   the first key stops the stop's own countdown (0035BCC8), the choice goes
   to the car (0035BF08). Unavailable service roles are presentation
   flags (003D3080/003D30D0/003D3120); they do not change service physics. */
static void pit_sound(void *user, const char *name)
{
    race_game *g = user;
    race_audio_menu_sound(g->audio, name, g->se_volume);
}
void race_game_set_pit_mode_change(race_game *g,int allowed){
    /* Native session options are kept on the host; captured sessions
       still read their original option block. No physics memory write. */
    if(g)g->pit_mode_allowed=allowed!=0;
}
static void pit_frame(race_game *g, double dt)
{
    race_pitmenu_choice c;
    if (g->replaying || !g->run) return;
    {   /* tests: OPENGT4_RACE_PIT=x:<keys> has the B-Spec driver pit 8 s in, then
           sends the menu Flash key codes ("38,39,13"), one each 0.4 s */
        static double clock, up; static int sent, keys;
        const char *t = getenv("OPENGT4_RACE_PIT");
        if (t && g->bspec_ui && sent < 2 && (clock += dt) > 8) { sent++; bspec_order(g, 6, 1); }
        if (t && race_pitmenu_up(g->pitmenu)) {
            int n = 0;
            if (!g->pit_touched) g->pit_touched = 1;
            up += dt;
            for (const char *q = strchr(t, ':'); q; q = strchr(q + 1, ','), n++)
                if (n == keys && up > 1 + 0.4 * keys) { race_pitmenu_key(g->pitmenu, atoi(q + 1)); keys++; break; }
        }
    }
    if (!g->pitmenu && !(g->pitmenu = race_pitmenu_open(g->vol, pit_sound, g))) return;
    {
        int i = watched_car(g), v;
        uint32_t car = gt4_race_run_car(g->run, i), cond = gt4_race_run_conductor(g->run), world = GT4_U32(GT4_U32(cond) + 0x84);
        v = gt4_pit_menu_ask(car);
        if (v & 1) {
            race_pitmenu_setup s;
            memset(&s, 0, sizeof s);
            s.tyres = v >> 1 & 1;
            s.feed = v >> 3 & 1;
            s.front = GT4_U8(cond + 0xfb20u + (uint32_t)i * 2);
            s.rear = GT4_U8(cond + 0xfb21u + (uint32_t)i * 2);
            s.bspec = GT4_U32(world + 0xcc8) == 1 || GT4_U32(world + 0xcc8) == 3;
            s.mode=g->pit_mode_allowed;
            uint32_t options=GT4_U32(world+0x6c);
            /* 0038AB98 +244 -> 003C12A8: original mode-change gate. */
            if(options && options<0x2000000-0x974)s.mode=GT4_U32(options+0x970)!=0;
            if(i==0 && g->last_prepared && g->last_desc.event_row && g->last_desc.owned_car[0]){
                s.owned_filter=1;gt4_pit_owned_tires(g->last_owned[0],s.owned);
            }
            for(int entry=0;entry<gt4_race_run_cars(g->run);entry++)gt4_pit_crew_service_enable(g->pit_crew+entry,s.tyres,s.feed);
            race_pitmenu_show(g->pitmenu, &s);
            g->pit_car = i;
            g->pit_touched = 0;
        }
    }
    if (!race_pitmenu_up(g->pitmenu)) return;
    {
        uint32_t car = gt4_race_run_car(g->run, g->pit_car);
        if (gt4_pit_menu_gone(car)) { race_pitmenu_hide(g->pitmenu); return; }
        if (g->pit_touched == 1) { gt4_pit_menu_touch(car); g->pit_touched = 2; }
        race_pitmenu_update(g->pitmenu, dt);
        if (race_pitmenu_choice_take(g->pitmenu, &c)) gt4_pit_menu_choose(car, c.front, c.rear, c.feed, c.mode);
    }
}

static void publish_photo_poses(race_game *g)
{
    const gt4_course_gadgets *state=gt4_race_run_objects(g->run);
    uint32_t course=GT4_U32(gt4_race_run_car(g->run,0)+8);
    gt4_course_gadgets *draw=race_view_gadgets(g->view,(int32_t)GT4_U32(course+0xb0));
    if(state && draw && state->count==draw->count)for(unsigned i=0;i<draw->count;i++){
        const uint8_t *descriptor=draw->instance[i].descriptor;
        draw->instance[i]=state->instance[i];draw->instance[i].descriptor=descriptor;
    }
    float pos[3],rot[9],wheels[4][3];
    if (gt4_race_run_pose(g->run, 0, pos, rot, wheels)) {
        race_view_set_pose(g->view, pos, rot);
        race_view_set_wheels(g->view, wheels);
        update_wheel_motion(g,0,rot);
    }
    for (int i = 0; i <= g->others; i++) {
        float sun, level;
        if (gt4_race_run_shadow(g->run, i, pos, rot, &sun, &level)) race_view_set_shadow(g->view, i, pos, rot, sun, level);
        else race_view_set_shadow(g->view, i, NULL, NULL, 0, 0);
    }
    for (int i = 1; i <= g->others; i++)
        if (gt4_race_run_pose(g->run, i, pos, rot, wheels)) {race_view_set_opponent(g->view, i - 1, pos, rot, wheels);update_wheel_motion(g,i,rot);}
    if (g->ghost_car) {   /* the ghost, while it drives a lap */
        /* see-through: measured on a PCSX2 frame (Escort, Laguna Seca): its rear
           (64, 66, 72) between the player's same body (13, 13, 36) and the
           road beside it (125, 124, 122) gives 0.53..0.58 by channel; the
           code that blends it is not found */
        race_view_set_ghost(g->view, 0, .55f);
        if (!g->ghost_hidden && (gt4_race_run_ghost_on(g->run) || getenv("OPENGT4_RACE_SNAPSHOT")) && gt4_race_run_pose(g->run, 1, pos, rot, wheels)) {race_view_set_opponent(g->view, 0, pos, rot, wheels);update_wheel_motion(g,1,rot);}
        else race_view_set_opponent(g->view, 0, NULL, NULL, NULL);
    }
}

void race_game_update(race_game *g, double dt)
{
    if (!g || !g->ready || !g->run || g->photo_held) return;
    pause_input(g);
    {   /* tests: OPENGT4_RACE_PAUSE=<seconds>[,key,key...] opens the pause
           menu then sends Flash key codes, one each half second */
        const char *t = getenv("OPENGT4_RACE_PAUSE");
        static double clock; static int sent;
        if (t) {
            clock += dt;
            double at = atof(t);
            if (!sent && clock >= at) { pause_toggle(g); sent = 1; }
            const char *q = strchr(t, ',');
            for (int k = 1; q && g->pause; k++, q = strchr(q + 1, ','))
                if (sent == k && clock >= at + k * .5) { race_pause_key(g->pause, atoi(q + 1)); sent++; }
        }
    }
    if (g->pause || g->held) {   /* held: no step, no controls; the car sounds quiet */
        cinematic_camera(g,dt);
        if(g->audio){float eye[3],rows[9],projection[3];
            if(race_view_get_camera(g->view,eye,rows,projection))
                race_audio_update(g->audio,g->run,eye,rows,g->start_cam_on || g->inspect_on?-1:g->camera,(g->bgm_enabled!=g->bgm_off)?g->bgm_volume:-1,0);
            race_audio_hold(g->audio,dt);}
        return;
    }
    read_controls(g);
    bspec_frame(g,dt);
    if(!g->split)pit_frame(g,dt);
    double camera_dt=dt;
    if (g->bspec_ui) dt *= race_bspec_speed(g->bspec_ui);   /* the monitor's view speed (+0x74, x1..x3). A: how the world applies it not read */
    if (getenv("OPENGT4_RACE_SPEED")) dt *= atof(getenv("OPENGT4_RACE_SPEED"));   /* tests: the race run faster */
    if (g->replaying && g->fast) dt *= 2;   /* A: the replay player's step (+0x74, func_003D9FB8: 1 to 3) taken as one up; L3's own handler not read */
    if (g->label) {   /* func_003A9780 */
        if (!g->label_out && (g->label_t -= camera_dt) < 0) g->label_out = 1;
        g->label_v = fmaxf(0, fminf(1, g->label_v + (float)camera_dt * (g->label_out ? -4 : 4)));
        if (g->label_out && g->label_v <= 0) g->label = NULL;
    }
    g->acc += dt;
    if (g->acc > 0.1 + dt) g->acc = 0.1 + dt;
    if (getenv("OPENGT4_RACE_SNAPSHOT")) g->acc = 0;   /* tests: the captured moment stays */
    int stepped=0;
    while (g->acc >= STEP) {
        /* 0033C490 +10..34: an event-10 stop is consumed on the next
           world tick before advancing the car. Preserve its final pose. */
        g->mission_presentation=gt4_mission_begin_frame(g->mission_presentation);
        if(g->mission_presentation==GT4_MISSION_STOPPED){
            fprintf(stderr,"mission stop: frame %u\n",g->license_test_frame);
            g->acc-=STEP;break;
        }
        if(g->replaying && g->disc_segment<g->disc_segments && g->tape_at==g->disc_segment_at[g->disc_segment]){
            /* FreeRun replay entry, 00359D40 +A4..134: reset clock to
               100 thirds and restore the recorded car before its tape.
               A: app frame entry replaces the original session callback. */
            uint32_t cond=gt4_race_run_conductor(g->run),race=GT4_U32(cond);
            if(!g->disc_segment){GT4_U32(cond+0xf898)=100;GT4_U32(race+0xbc)=0;}
            fenv_t env;fegetenv(&env);gt4_phys_fpu_enter();
            disc_checkpoint_restore(cond,0,g->disc_states[g->disc_segment]);
            GT4_F32(gt4_race_run_car(g->run,0)+0x554)=0;
            fesetenv(&env);g->disc_segment++;
        }
        if(g->replaying && g->disc_segment_2p<g->disc_segments_2p && g->tape_at_2p==g->disc_segment_at_2p[g->disc_segment_2p]){
            fenv_t env;fegetenv(&env);gt4_phys_fpu_enter();
            disc_checkpoint_restore(gt4_race_run_conductor(g->run),1,g->disc_states_2p[g->disc_segment_2p]);
            GT4_F32(gt4_race_run_car(g->run,1)+0x554)=0;fesetenv(&env);g->disc_segment_2p++;
        }
        /* Controller sampling follows the physics step, including accelerated
           test runs. Ordinary input is still sampled by read_controls. */
        if(getenv("OPENGT4_LICENSE_TEST_BRAKE_AT"))read_controls(g);
        if (!gt4_race_run_frame(g->run)) {
            fprintf(stderr, "race: the step failed\n");
            race_game_stop(g);
            return;
        }
        g->license_test_frame++;
        license_start_step(g);
        license_step(g);
        if(race_game_mission_active(g) && !g->replaying){
            uint32_t race=GT4_U32(gt4_race_run_conductor(g->run));
            int previous=g->mission_presentation;
            g->mission_presentation=gt4_mission_presentation(gt4_ptr(race+0xf4),previous);
            if(g->mission_presentation && g->finished<0)g->finished=0;
            if(previous!=g->mission_presentation)fprintf(stderr,"mission presentation: %s frame %u\n",g->mission_presentation==GT4_MISSION_RETURN?"return":"board",g->license_test_frame);
        }
        if(g->license_log_collecting)logger_sample(g);
        /* 00389B08: arm the end-of-replay fade after the recorded step. */
        if(g->replaying) {
            uint32_t session=GT4_U32(gt4_race_run_conductor(g->run));
            gt4_music_replay_fade(&GT4_F32(session+0xd4),&GT4_F32(session+0xd8),
                                 (int32_t)(g->tape_n-g->tape_at),2,GT4_U32(session+0xe0)!=0,0);
        }
        update_drivers(g);
        timing_events(g);
        race_smoke_update(g->smoke, (float)STEP);   /* 0033C4F0 -> 003E7A58 */
        race_leaves_update(g->leaves, (float)STEP); /* 0033C4F0 -> 003C2780 */
        /* Presentation only, after the service solver. All crews advance
           once per physical step; only the watched car posts one-shots. */
        {gt4_pit_crew_sound heard={0};int watched=watched_car(g);
            for(int i=0;i<gt4_race_run_cars(g->run);i++)if(g->pit_motion[i]){
                gt4_pit_crew_sound out;
                /* 003D03E0: 003D0D88 placement precedes 003CFA98 clocks.
                   Retain the previous phase for its seek/history test. */
                int previous=g->pit_crew[i].stage,next=gt4_pit_stage(gt4_race_run_car(g->run,i));
                if(next){g->pit_crew[i].stage=next;gt4_pit_crew_place(g->pit_crew+i,gt4_race_run_car(g->run,i),(unsigned)i,race_view_car_geometry(g->view,i),g->pit_package==1,&g->pit_random);g->pit_crew[i].stage=previous;}
                gt4_pit_crew_step(g->pit_crew+i,gt4_race_run_car(g->run,i),(float)STEP,&out);
                gt4_pit_camera_step(g->pit_crew+i,(float)STEP);
                heard.refueling |= out.refueling;
                if(i==watched){heard.count=out.count;memcpy(heard.effect,out.effect,out.count);}
            }
            race_audio_pit(g->audio,&heard);
            for(unsigned i=0;i<(unsigned)gt4_race_run_cars(g->run);i++)if(g->pit_motion[i]){
                gt4_pit_crew*c=g->pit_crew+i;unsigned lane=GT4_U8(c->car+0x56b);
                /* 003CFA98 keeps 0035BD10's command only in phase 1. */
                int commands=c->stage==1?(lane==4?1:lane==2 || lane==3?(int)lane:0):0;
                gt4_pit_walk_step(g->pit_crew,(unsigned)gt4_race_run_cars(g->run),i,commands,(float)STEP,&g->pit_random);
            }
        }
        race_camera_audio_update(g,STEP);
        /* 0033B358: retain adjacent render frames, without advancing during exposure. */
        publish_photo_poses(g);race_view_photo_record(g->view);
        race_audio_step(g->audio, g->run);
        stepped++;
        if (getenv("OPENGT4_RACE_REPLAY_LOG")) {   /* tests: the player's place by the tape's step, live and replayed */
            size_t at = g->replaying ? g->tape_at : g->tape_n;
            float p3[3], r9[9], w[4][3];
            if (at && at % 300 == 0 && gt4_race_run_pose(g->run, 0, p3, r9, w))
                fprintf(stderr, "replay-log %s %zu %.6f %.6f %.6f\n", g->replaying ? "play" : "live", at, p3[0], p3[1], p3[2]);
        }
        g->acc -= STEP;
        {   /* tests: OPENGT4_REPLAY_BUTTONS=<seconds>:<button>,... presses the replay's buttons */
            const char *t = getenv("OPENGT4_REPLAY_BUTTONS");
            for (; g->replaying && t && *t; t = strchr(t, ',') ? strchr(t, ',') + 1 : "")
                if ((size_t)(atof(t) * 60) == g->tape_at && strchr(t, ':')) replay_button(g, atoi(strchr(t, ':') + 1));
        }
        if (g->replaying && g->tape_at >= g->tape_n && (!g->split || g->tape_at_2p>=g->tape_n_2p)) g->replay_end = g->replay_end < 0 ? 0 : g->replay_end + STEP;
        if (g->finished >= 0) g->finished += STEP;
        else if (gt4_race_run_finished(g->run)) g->finished = 0;
    }
    if(!stepped)publish_photo_poses(g);
    /* Snapshot/initialization has no physics tick to publish its camera. */
    if(!stepped && (!g->audio || getenv("OPENGT4_RACE_SNAPSHOT") || (g->cinema.on && !g->cinema.seeded)))
        race_camera_audio_update(g,camera_dt);
}

static int race_game_draw_scene(race_game *g)
{
    if (!g || !g->ready) return 0;
    if(g->split){
        int drawn=1;
        /* 0032FCC8 +130..258: horizontal top/bottom, each height/2.
           Models, textures, animation and physics remain shared. */
        for(int i=0;i<2;i++){
            if(!g->split_camera_valid[i])split_camera(g,i);
            float *p=g->split_projection[i];
            race_view_set_camera(g->view,g->split_eye[i],g->split_rows[i],p[0],p[1],p[2]);
            drawn &= race_view_draw_player(g->view,i,(i?g->camera_2p:g->camera)!=0,i*224.f,224);
            int camera=i?g->camera_2p:g->camera;
            if(camera==0 || camera==6){
                /* 0032FCC8 +17c..1d0 / +280..2d4 -> 00341E50:
                   E2E0 = (200,16/8,240,40,2). Rear camera +18,
                   projection reflection at 0034203c..4c.
                   A: pose adapter uses the installed camera axes. */
                float rear[9];memcpy(rear,g->split_rows[i],sizeof rear);
                for(int k=0;k<3;k++)rear[6+k]=-rear[6+k];
                const float *m=gt4_split_mirror[i];
                race_view_set_camera(g->view,g->split_eye[i],rear,m[4]*m[3]/m[2],m[4],p[2]);
                race_view_draw_mirror(g->view,i,m[0],i*224.f+m[1],m[2],m[3]);
                race_view_set_camera(g->view,g->split_eye[i],g->split_rows[i],p[0],p[1],p[2]);
            }
        }
        return drawn;
    }
    for(int i=0;g->audio && g->run && i<gt4_race_run_cars(g->run) && i<6;i++){   /* the backfire's flames */
        float flames[4][3];int on=race_audio_afterfire(g->audio,(unsigned)i,flames);
        race_view_set_afterfire(g->view,(unsigned)i,flames,on);
    }
    int drawn=race_view_draw(g->view);
    if(drawn && g->pit_scene){
        r3d_view view={0};float rows[9],projection[3];
        if(race_view_get_camera(g->view,view.cam,rows,projection)){
            memcpy(view.r,rows,12);memcpy(view.u,rows+3,12);for(unsigned k=0;k<3;k++)view.f[k]=-rows[6+k];
            view.focal=(448.f*4.f/3.f)*.5f*projection[0];view.focal_y=224*projection[1];view.near_clip=projection[2];
            for(int i=0;i<gt4_race_run_cars(g->run);i++)if(g->pit_crew[i].placed)
                license_scene_draw_pit(g->pit_scene,&view,g->pit_crew+i,GT4_U32(gt4_race_run_car(g->run,i)+8),race_view_course(g->view),i==watched_car(g));
        }
    }
    if(drawn && g->license_scene && g->concourse.placement_ready){
        r3d_view view={0};float rows[9],projection[3];
        if(race_view_get_camera(g->view,view.cam,rows,projection)){
            memcpy(view.r,rows,12);memcpy(view.u,rows+3,12);for(unsigned k=0;k<3;k++)view.f[k]=-rows[6+k];
            /* Same native pixel transform as race_view_draw's world/car.
               This adapter is A; scalar lens coefficients are tested C. */
            view.focal=(448.f*4.f/3.f)*.5f*projection[0];view.focal_y=224*projection[1];view.near_clip=projection[2];
            /* 00408490 draws authored pitman01 shadows and workman00
               with each actor's ground light context (00409B68). */
            uint32_t car=gt4_race_run_car(g->run,0);
            license_scene_draw(g->license_scene,&view,g->license_people,GT4_U32(car+8),race_view_course(g->view),g->license_surfaces);
        }
    }
    if(drawn && g->license_motion && g->license_fade.value<1.f){
        /* 0038B5A0 +0x18 draws Scene fade before display/result overlays.
           Original color 00620200 = black. A: 8-bit GL alpha conversion;
           the clock, clamping and cubic factor are tested separately E. */
        float factor=gt4_license_fade_factor(g->license_fade.value);
        uint8_t black[4]={0,0,0,(uint8_t)((1.f-factor)*255.f)};
        plat_video_set_view(640,448);plat_draw_quad(NULL,0,0,640,448,0,0,1,1,black);
    }return drawn;
}

int race_game_draw(race_game *g)
{
    if(!g || !g->cinema.on || !g->cinema.aperture || g->split)return race_game_draw_scene(g);
    /* P: optional live depth of field, eight GPU samples of the existing
       Photo Travel lens. Scene/physics clocks never advance between samples. */
    float eye[3],rows[9],pr[3],samples[8][4];
    if(!race_view_get_camera(g->view,eye,rows,pr))return 0;
    gt4_photo_samples(samples,8);static const float aperture[4]={0,2.8f,5.6f,11};
    const uint8_t tint[4]={255,255,255,255};int drawn=0;
    for(int i=0;i<8;i++){
        float shift[2],jitter[2],e[3];
        cinematic_lens(pr,aperture[g->cinema.aperture],g->cinema.focus,samples[i],shift,jitter);
        for(int k=0;k<3;k++)e[k]=eye[k]+rows[k]*shift[0]+rows[3+k]*shift[1];
        race_view_set_camera(g->view,e,rows,pr[0],pr[1],pr[2]);race_view_photo_jitter(g->view,jitter);
        if(!race_game_draw_scene(g))goto done;
        g->cinema.sample=video_grab(g->cinema.sample,0,0,640,448);if(!g->cinema.sample)goto done;
        if(i){plat_draw_quad(g->cinema.image,0,0,640,448,0,1,1,0,tint);video_flush2d();}
        plat_draw_quad_blend(g->cinema.sample,0,0,640,448,0,1,1,0,tint,1.f/(i+1));
        g->cinema.image=video_grab(g->cinema.image,0,0,640,448);if(!g->cinema.image)goto done;
    }drawn=1;
done:
    race_view_photo_jitter(g->view,NULL);race_view_set_camera(g->view,eye,rows,pr[0],pr[1],pr[2]);
    return drawn;
}

/* 001119A0 / 00388880: Arcade's menu camera manager runs mode 5.
   Its course bytes come from +0xC0, never the +0xD0 standing-start bank.
   Only presentation advances (0012BC18 -> conductor slot +0x38). */
/* a camera from its eye, the point it looks at and its roll (the
   pregrid's construction) */
static void look(race_game *g,const float eye[3],const float target[3],float roll,float sx,float sy)
{
    float b[3],r[3],u[3],rows[9],n;
    for(int k=0;k<3;k++)b[k]=eye[k]-target[k];
    n=sqrtf(b[0]*b[0]+b[1]*b[1]+b[2]*b[2]);if(n<=0)return;
    for(int k=0;k<3;k++)b[k]/=n;
    n=sqrtf(b[0]*b[0]+b[2]*b[2]);if(n<=0)return;
    r[0]=b[2]/n;r[1]=0;r[2]=-b[0]/n;
    u[0]=b[1]*r[2];u[1]=b[2]*r[0]-b[0]*r[2];u[2]=-b[1]*r[0];
    float a=roll*.017453292519943295f,cs=cosf(a),sn=sinf(a);
    for(int k=0;k<3;k++){rows[k]=cs*r[k]-sn*u[k];rows[k+3]=sn*r[k]+cs*u[k];rows[k+6]=b[k];}
    race_view_set_camera(g->view,eye,rows,sx,sy,.1f);
}

/* The rolling start's lead-in (cond+0xCBE4, the race's own rolling
   flag) is watched through the TV cameras: the view selector
   (func_00372DF8 -> func_003744F0) picks camera type 1 there, the
   ShotManager (func_003747B8, table 0x0069F9C0 -> func_003FD980), before
   the race's start frame race+0xBC. A: the exact moment it hands back
   to the driving camera is not read (the selector's +0x128/+0x138
   history); the flag's end is used. Tests: OPENGT4_RACE_TV=1 keeps the
   TV cameras for the whole race. */
static int rolling_phase(race_game *g)
{
    gt4_race_hud h;
    return gt4_race_run_hud(g->run, 0, &h) && h.rolling;
}

/* the TV cameras on the player's car (CameraShot): one 60 Hz tick */
static int ground_at(void *ctx, float x, float y, float z, float *height)
{
    const gt4_course *c = race_view_course(((race_game *)ctx)->view);
    return c && gt4_course_height(c, x, z, y, height);
}

static int ground_id(void *ctx, const float p[3])
{
    return gt4_race_run_ground_id(((race_game *)ctx)->run, p);
}

static int camera_wall(void *ctx, float margin, const float from[3], const float to[3], float out[3], int *id)
{
    return gt4_race_run_camera_wall(((race_game *)ctx)->run, margin, from, to, out, id);
}

static float distance_of(void *ctx, const float p[3])
{
    return gt4_race_run_course_distance(((race_game *)ctx)->run, p);
}

static int has_point(void *ctx, int i)
{
    float p[8];
    return race_view_camera_point(((race_game *)ctx)->view, i, p);
}

static int course_point(void *ctx, float d, float out[3])
{
    return gt4_race_run_course_point(((race_game *)ctx)->run, d, out);
}

/* the replay's Dive (the selector's type 8): the director on the car
   watched, cutting on the music's marks */
static int dive_camera(race_game *g)
{
    if (!g->dive_tried) {
        const gt4_course *c = race_view_course(g->view);
        g->dive_tried = 1;
        if (c) g->dive = gt4_dive_open(c->data, c->size);
        if (!g->dive) fprintf(stderr, "race: no camera points (REP4 +0xB0 +0x30) on this course\n");
    }
    if (!g->dive) return 0;
    int watched = g->focus;
    uint32_t car = gt4_race_run_car(g->run, watched);
    float rot[9], wheels[4][3];
    gt4_dive_target t;
    gt4_tvcam_view v;
    if (!car || !gt4_race_run_pose(g->run, watched, t.pos, rot, wheels)) return 0;
    memcpy(t.rot, rot, sizeof t.rot);
    if (!g->dive_had) memcpy(g->dive_prev, t.pos, sizeof t.pos);
    memcpy(t.prev, g->dive_prev, sizeof t.prev);
    memcpy(g->dive_prev, t.pos, sizeof t.pos);
    g->dive_had = 1;
    t.distance = GT4_F32(car + 0x5a8);
    t.speed = GT4_F32(car + 0x6c8);
    t.cue = g->cue;
    t.cue_passed = g->cue_passed;
    if (g->cue_passed && getenv("OPENGT4_DIVE_LOG")) fprintf(stderr, "dive cue: a mark went by, the next in %.2f s\n", g->cue);
    t.course_point = course_point;
    t.has_point = has_point;
    t.ground = ground_at;
    t.ground_id = ground_id;
    t.wall = camera_wall;
    t.distance_of = distance_of;
    if (!gt4_race_run_place(g->run, watched, t.place, &t.yaw)) { memcpy(t.place, t.pos, sizeof t.place); t.yaw = 0; }
    {   /* func_003F3708..3748: the params' sizes */
        uint32_t params = GT4_U32(car + 0x10);
        t.size[0] = GT4_F32(params + 0) + GT4_F32(params + 0x50);
        t.size[1] = GT4_F32(params + 4) - GT4_F32(params + 0x50);
        t.size[2] = GT4_F32(params + 8);
        t.size[3] = GT4_F32(params + 0xc);
    }
    t.ctx = g;
    int kind = gt4_dive_step(g->dive, &t, &v);
    if (kind == 2) {   /* a camera on the car: its model's point (A: the player's model's points for any car) */
        float point[8], eye[3], rows[9], sx;
        if (!race_view_camera_point(g->view, v.shot, point) || !gt4_race_run_camera(g->run, watched, v.shot, point, eye, rows)) return 0;
        if (getenv("OPENGT4_DIVE_LOG")) { static int last = -1; if (last != v.shot) fprintf(stderr, "dive car point %d fov %.1f pt %.2f %.2f %.2f turns %.1f %.1f %.1f w %.2f\n", v.shot, v.fov, point[0], point[1], point[2], point[3], point[4], point[5], point[6]); last = v.shot; }
        sx = v.fov > 0 ? 1 / tanf(v.fov * .5f * 0.017453292f) : v.shot == 1 ? 1.73205f : 1 / tanf(atanf(1 / point[6]) * 4 / 3);
        if (v.heading != 0) {   /* the hand's turn (func_004A79B0) about the camera's up */
            float a = v.heading * 0.017453292f, cs = cosf(a), sn = sinf(a), r[3], b[3];
            memcpy(r, rows, sizeof r); memcpy(b, rows + 6, sizeof b);
            for (int k = 0; k < 3; k++) { rows[k] = cs * r[k] + sn * b[k]; rows[k + 6] = -sn * r[k] + cs * b[k]; }
        }
        if (v.pitch != 0) {   /* and its nod (func_004A7988) about the camera's right */
            float a = v.pitch * 0.017453292f, cs = cosf(a), sn = sinf(a), u[3], b[3];
            memcpy(u, rows + 3, sizeof u); memcpy(b, rows + 6, sizeof b);
            for (int k = 0; k < 3; k++) { rows[k + 3] = cs * u[k] - sn * b[k]; rows[k + 6] = sn * u[k] + cs * b[k]; }
        }
        if (v.roll != 0) {   /* the camera's roll (func_004A79D8), turned as look() turns it */
            float a = v.roll * 0.017453292f, cs = cosf(a), sn = sinf(a), r[3], u[3];
            memcpy(r, rows, sizeof r); memcpy(u, rows + 3, sizeof u);
            for (int k = 0; k < 3; k++) { rows[k] = cs * r[k] - sn * u[k]; rows[k + 3] = sn * r[k] + cs * u[k]; }
        }
        race_view_set_camera(g->view, eye, rows, sx, sx * 1.30122f, 0.1f);
        race_view_show_car(g->view, v.shot != 0);
        return 1;
    }
    if (!kind) return 0;
    look(g, v.eye, v.target, v.roll, v.sx, v.sy);
    race_view_show_car(g->view, 1);
    return 1;
}

/* CarTarget 003FBE70 +E4..118 fills +14 from 00344798/003447F8/
   00344828; 003FC618 +C returns that camera anchor. +894 is the model
   origin and is not the TV target. Read through the existing runner API. */
static int tv_target(race_game *g,int watched,gt4_tvcam_target *target)
{
    uint32_t car=gt4_race_run_car(g->run,watched);float yaw;
    if(!car || !gt4_race_run_place(g->run,watched,target->pos,&yaw))return 0;
    target->distance=GT4_F32(car+0x5a8);
    return 1;
}

static int tv_camera(race_game *g)
{
    if(!g->tv_tried){const gt4_course *c=race_view_course(g->view);g->tv_tried=1;
        if(c)g->tvcam=gt4_tvcam_open(c->data,c->size);
        if(!g->tvcam)fprintf(stderr,"race: no TV cameras (REP4 +0xB0) on this course\n");}
    if(!g->tvcam)return 0;
    int watched=watched_car(g);
    gt4_tvcam_target t;gt4_tvcam_view v;
    if(!tv_target(g,watched,&t))return 0;
    gt4_tvcam_set_zoom(g->tvcam,g->replaying?g->zoom:1);   /* Triangle's zoom */
    if(!gt4_tvcam_step(g->tvcam,&t,&v))return 0;
    look(g,v.eye,v.target,v.roll,v.sx,v.sy);
    race_view_show_car(g->view,1);
    return 1;
}

/* 0011D1B8 +44..60 / 00387EF0: reconstruct Scene mode 2 with DC=1.
   003EAAC0/003EB370 step the selected recorder behind the menu. Reuse the
   existing native runner; the player's attempt tape and telemetry stay owned
   by the player. Scene thread completion remains an explicit boundary. */
static int license_menu_reset(race_game *g,char *error,size_t len)
{
    /* 0012B888 +1C..4C closes/reconstructs the same Scene mode; no menu
       reload or player recording reset. Native runner construction is A. */
    gt4_race_run_close(g->run);g->run=gt4_race_run_open(g->disc,&g->license_menu_desc,error,len);
    if(!g->run){g->ready=0;return 0;}
    player=gt4_race_run_car(g->run,0);playing=g;
    gt4_phys_pad.read=host_read;gt4_phys_pad.wheel_state=host_wheel;gt4_phys_pad.write=host_write;
    uint32_t race=GT4_U32(gt4_race_run_conductor(g->run));GT4_U32(race+0xdc)=1;
    g->license_menu_at=0;g->license_menu_ticks=0;
    gt4_race_run_pose(g->run,0,g->license_menu_pos,g->license_menu_rot,g->license_menu_wheels);
    gt4_race_run_place(g->run,0,g->license_menu_origin,g->license_menu_origin+3);
    if(g->license_menu_motion && !gt4_license_motion_select(g->license_menu_motion,3,"replay_start_camera1"))return 0;
    gt4_tvcam_reset(g->tvcam);
    update_wheel_motion(g,0,g->license_menu_rot);
    return 1;
}
static int license_menu_prime(race_game *g)
{
    /* C: slot05 Scene E48C recorder+40=286 at Mot1 clock 4.74998
       (285 camera ticks); menu-late recorder+40=1026. Preserve the
       world tick preceding the camera clock, not an invented delay.
       00389990 +0x134..0x14c steps the world before +0x150..0x154
       commits the camera. Full asynchronous Scene scheduling is A. */
    g->license_menu_playing=1;
    return gt4_race_run_frame(g->run);
}
static int license_menu_begin(race_game *g)
{
    gt4_replay_file r;char path[64],error[256];
    const char *code=g->last_desc.license_code;
    /* 0011D1B8 does NOT call 003EAD58 (teacher import): 003EA8C8
       selects the retained E48C recorder. Slot 05 has 1721 player
       commands, not the 1471-command teacher. Always copy the attempt;
       the preview must never consume or overwrite the player's tape.
       P: with no attempt, keep the user-requested automatic teacher. */
    if(g->tape_n){
        uint8_t *copy=malloc(g->tape_n*32);if(!copy)return 0;
        memcpy(copy,g->tape,g->tape_n*32);g->license_menu_desc=g->last_desc;
        if(!license_menu_reset(g,error,sizeof error)){free(copy);fprintf(stderr,"license menu replay: %s\n",error);return 0;}
        free(g->license_menu_tape);g->license_menu_tape=copy;
        g->license_menu_steps=g->tape_n;g->license_menu_at=0;g->license_menu_playing=1;
        memset(&g->license_menu_cycle,0,sizeof g->license_menu_cycle);g->license_menu_cycles=0;
        fprintf(stderr,"license menu replay: %s, %zu player commands\n",code,g->license_menu_steps);return license_menu_prime(g);
    }
    snprintf(path,sizeof path,"/replay/license/%s%s",strncmp(code,"us",2)?"us":"",code);
    if(!race_game_read_replay(g,path,&r,error,sizeof error)){
        fprintf(stderr,"license menu demo: %s\n",error);return 0;
    }
    g->license_menu_desc=g->last_desc;g->license_menu_desc.manual=r.manual;
    g->license_menu_desc.assist_asm=r.assist_asm;g->license_menu_desc.assist_tcs=r.assist_tcs;
    if(!license_menu_reset(g,error,sizeof error)){gt4_replay_free(&r);fprintf(stderr,"license menu demo: %s\n",error);return 0;}
    free(g->license_menu_tape);g->license_menu_tape=r.tape;r.tape=NULL;
    g->license_menu_steps=r.steps;g->license_menu_at=0;g->license_menu_playing=1;
    memset(&g->license_menu_cycle,0,sizeof g->license_menu_cycle);g->license_menu_cycles=0;
    fprintf(stderr,"license menu demo: %s, %zu original commands\n",code,g->license_menu_steps);
    gt4_replay_free(&r);return license_menu_prime(g);
}

void race_game_pregrid_update(race_game *g,double dt)
{
    if(!g || !g->ready)return;
    if(race_game_license_active(g)){
        /* 0011D1B8 +44..60: Scene mode 2, race+DC=1; slot 05
           confirms camera kind 4, state 2 and the course +D4 bank.
           003749A0 / 0037D734 select replay_start_camera1; 003EBC10
           advances its controlled sequence alongside the teacher replay. */
        if(!g->license_menu_playing && !license_menu_begin(g))return;
        if(!g->license_menu_motion){
            const gt4_course *c=race_view_course(g->view);uint32_t at=0;
            if(c && c->size>=0xd8)memcpy(&at,c->data+0xd4,4);
            if(c && at && at<c->size)g->license_menu_motion=gt4_license_motion_open_camera(c->data+at,c->size-at);
            if(!g->license_menu_motion || !gt4_license_motion_select(g->license_menu_motion,3,"replay_start_camera1"))return;
        }
        race_view_set_pose(g->view,g->license_menu_pos,g->license_menu_rot);
        race_view_set_wheels(g->view,g->license_menu_wheels);
        for(int i=0;i<7;i++)race_view_set_opponent(g->view,i,NULL,NULL,NULL);
        race_view_set_smoke(g->view,NULL);race_view_set_leaves(g->view,NULL);
        g->pregrid_acc+=dt;
        while(g->pregrid_acc>=STEP){
            if(gt4_license_menu_cycle_update(&g->license_menu_cycle)){
                char error[256];if(!license_menu_reset(g,error,sizeof error)){
                    fprintf(stderr,"license menu cycle: %s\n",error);return;
                }
                if(!license_menu_prime(g))return;
                g->license_menu_cycle.job=0;g->license_menu_cycle.ready=1;g->license_menu_cycles++;
                fprintf(stderr,"license menu cycle: %s restarted %u\n",g->last_desc.license_code,g->license_menu_cycles);
            }
            if(g->license_menu_cycle.paused || g->license_menu_cycle.phase){
                gt4_license_menu_cycle_draw(&g->license_menu_cycle,0);
                g->pregrid_acc-=STEP;continue;
            }
            /* 003C0208: countdown seconds from the authored RACE kind.
               0037D338 +FC and 0037D620 +B4: opening adds 60 frames,
               then decrements before choosing D_00621290's control. */
            if(g->license_menu_at<g->license_menu_steps && !gt4_race_run_frame(g->run))return;
            if(gt4_race_run_pose(g->run,0,g->license_menu_pos,g->license_menu_rot,g->license_menu_wheels)){
                race_view_set_pose(g->view,g->license_menu_pos,g->license_menu_rot);
                race_view_set_wheels(g->view,g->license_menu_wheels);update_wheel_motion(g,0,g->license_menu_rot);
            }
            float shadow_pos[3],shadow_rot[9],sun,level;
            if(gt4_race_run_shadow(g->run,0,shadow_pos,shadow_rot,&sun,&level))
                race_view_set_shadow(g->view,0,shadow_pos,shadow_rot,sun,level);
            else race_view_set_shadow(g->view,0,NULL,NULL,0,0);
            g->license_menu_distance=GT4_F32(player+0x5a8);
            unsigned kind=g->last_event_row[0x40]+1;
            int seconds=kind==2?8:kind==3?10:kind==4?0:g->last_event_row[0x41];
            int left=seconds*60-(int)++g->license_menu_ticks;
            if(left < -60){
                /* 0037DAD8 changes the opening's E8 to 3 at expiry;
                   003745B0..5D8 then selects TV. Confirmed independently
                   in the late menu PINE capture (kind 1/state 2).
                   The TV target follows the native teacher car. */
                if(!g->tv_tried){const gt4_course *c=race_view_course(g->view);g->tv_tried=1;
                    if(c)g->tvcam=gt4_tvcam_open(c->data,c->size);}
                gt4_tvcam_target target;gt4_tvcam_view camera;
                if(g->tvcam && tv_target(g,0,&target) && gt4_tvcam_step(g->tvcam,&target,&camera)){
                    look(g,camera.eye,camera.target,camera.roll,camera.sx,camera.sy);race_view_show_car(g->view,1);
                }
                /* 003EB370 +298..2CC reports recorder completion. A:
                   decoded pad EOF replaces the original Scene thread signal. */
                gt4_license_menu_cycle_draw(&g->license_menu_cycle,g->license_menu_at==g->license_menu_steps);
                g->pregrid_acc-=STEP;continue;
            }
            int control=gt4_license_opening_control(left+59);
            if(!gt4_license_motion_step_control(g->license_menu_motion,3,gt4_f32(0x3c888888),control))return;
            float look[9],fov,cc,tc;
            if(!gt4_license_motion_camera(g->license_menu_motion,look,&fov,&cc,&tc) ||
               !license_look(g,look,fov,g->license_menu_origin))return;
            gt4_license_menu_cycle_draw(&g->license_menu_cycle,g->license_menu_at==g->license_menu_steps);
            g->pregrid_acc-=STEP;
        }
        return;
    }
    if(!g->pregrid_tried){const gt4_course *c=race_view_course(g->view);g->pregrid_tried=1;
        if(c)g->pregrid=gt4_pregrid_open(c->data,c->size);
        if(!g->pregrid)fprintf(stderr,"pregrid: unsupported or missing REP4 camera bank\n");}
    if(!g->pregrid)return;
    g->pregrid_acc+=dt;
    while(g->pregrid_acc>=STEP){
        gt4_pregrid_camera c;if(!gt4_pregrid_step(g->pregrid,&c))return;
        float b[3],r[3],u[3],rows[9],n;
        for(int k=0;k<3;k++)b[k]=c.eye[k]-c.target[k];
        n=sqrtf(b[0]*b[0]+b[1]*b[1]+b[2]*b[2]);if(n<=0)return;
        for(int k=0;k<3;k++)b[k]/=n;
        n=sqrtf(b[0]*b[0]+b[2]*b[2]);if(n<=0)return;
        r[0]=b[2]/n;r[1]=0;r[2]=-b[0]/n;
        u[0]=b[1]*r[2];u[1]=b[2]*r[0]-b[0]*r[2];u[2]=-b[1]*r[0];
        /* 0037C138 +0x660..+0x674 builds Rz(roll)*Rx(pitch)*Ry(yaw).
           004A7350 +0x14..+0x28 has degree multiplier 0x3C8EFA35.
           A: cross products/libm replace the matrix VU path, not yet compared. */
        float a=c.roll*.017453292519943295f,cs=cosf(a),sn=sinf(a);
        for(int k=0;k<3;k++){rows[k]=cs*r[k]-sn*u[k];rows[k+3]=sn*r[k]+cs*u[k];rows[k+6]=b[k];}
        race_view_set_camera(g->view,c.eye,rows,c.sx,c.sy,.1f); /* A: inherited race near plane; pregrid original not proved */
        g->pregrid_acc-=STEP;
    }
}
int race_game_draw_pregrid(race_game *g)
{
    if(!g || !g->ready)return 0;
    if(race_game_license_active(g))return g->license_menu_motion && race_view_draw_license_menu(g->view);
    return g->pregrid && race_view_draw_pregrid(g->view);
}
void race_game_draw_pregrid_overlay(race_game *g)
{
    if(!g || !g->ready || !g->license_menu_playing || !race_game_license_active(g))return;
    /* 0012BC18 +1D4..214: black alpha over Scene AND menu. E scalar
       clock; A 8-bit GL conversion/fullscreen rasterization. */
    float alpha=g->license_menu_cycle.alpha;if(alpha<=0)return;
    uint8_t black[4]={0,0,0,(uint8_t)(alpha*255.f)};
    plat_video_set_view(640,480);plat_draw_quad(NULL,0,0,640,480,0,0,1,1,black);
}
void race_game_return_pregrid(race_game *g)
{
    if(!g)return;
    if(g->cinema.on)race_game_cinematic_toggle(g);
    race_audio_close(g->audio);g->audio=NULL;
    race_result_close(g->result);g->result=NULL;g->inspect_on=g->inspect_seeded=0;
    race_pause_close(g->pause);g->pause=NULL;g->quit=0;
    gt4_pregrid_reset(g->pregrid);g->pregrid_acc=0;
    if(g->license_menu_motion)gt4_license_motion_select(g->license_menu_motion,3,"replay_start_camera1");
    g->license_menu_ticks=0;g->license_menu_playing=0;
    gt4_tvcam_reset(g->tvcam);
}

void race_game_set_unit(race_game *g, int unit) { if (g) g->unit = unit; }
void race_game_set_power_unit(race_game *g, const char *unit)
{
    static const char *const names[7] = { "PS", "HP", "BHP", "CH", "KW", "CV", "PF" };
    if (!g) return;
    g->power_unit = 1;
    for (int i = 0; i < 7; i++)
        if (unit && !strcmp(unit, names[i])) g->power_unit = i;
    if(unit && !strcmp(unit,"kW"))g->power_unit=4; /* previous native files */
}

/* the replay's opening, the tape's seconds in (func_0039BEC8: the mode's
   key from the session, vtable+0x308) */
static void replay_opening(race_game *g)
{
    race_hud_opening o;
    if (!g->open_on || !g->hud) return;
    o.car = g->names[0];
    o.power = g->open_power;
    o.unit = g->power_unit;
    o.front = g->open_front;
    o.rear = g->open_rear;
    o.mode = g->time_trial ? "ARCADE_TIME_TRIAL" : g->last_prepared ? "GTMODE_EVENT_RACE" : "ARCADE_SINGLE_RACE";
    o.mode_n = 0;
    race_hud_draw_opening(g->hud, &o, (float)g->tape_at / 60);
}
void race_game_set_audio(race_game *g,int bgm,int enabled,int se)
{if(g){g->bgm_volume=bgm;g->bgm_enabled=enabled!=0;g->se_volume=se;}}

/* the display's car icon (RaceDisplay +0x2D14): func_003A6678 draws the
   car's 96x64 picture (func_004997B0 2, 0x60, 0x40), CarIconMaker's first
   per car (00393B70: 00393D20 mode 0, in perspective from the front
   three-quarter), not the results' 64x32 side view (mode 1) */
static void hud_icon(void *user, int car, float x, float y, float w, float h)
{
    race_game *g = user;
    race_view_draw_icon_front(g->view, car, x, y, w, h);
}

static void draw_icon(void *user, int car, float x, float y, float w, float h, const uint8_t color[4])
{
    race_game *g = user;
    (void)color;
    race_view_draw_icon(g->view, car, x, y, w, h);
}

static void draw_license_screen(void *user,float x,float y,float w,float h)
{
    race_game *g=user;int32_t state;uint32_t time;
    if(!race_game_license_result(g,&state,&time) || state>0 || !g->license_screen)return;
    /* 0033F2E8 consumes the previous callback's Scene parameters;
       003E0E38 then multiplies Result+568 by .98 and publishes for next
       frame. LUT is cached by the GPU; no readback or image allocation. */
    int options[4];float parameters[6];uint8_t mix[3][3];
    race_pause_display_options(options);gt4_license_image_options(options,parameters);
    if(!g->license_image_valid || memcmp(options,g->license_image_options,sizeof options)){
        gt4_license_image_lut(parameters,g->license_image_lut);
        memcpy(g->license_image_options,options,sizeof options);g->license_image_valid=1;
    }
    gt4_license_image_mix(parameters[5]*g->license_saturation,mix);
    if(g->license_image_active)video_license_image(g->license_screen,x,y,w,h,g->license_image_lut,mix);
    else{const uint8_t white[4]={255,255,255,255};plat_draw_quad(g->license_screen,x,y,w,h,0,1,1,0,white);}
    g->license_image_active=1;
    g->license_saturation=gt4_license_image_decay(g->license_saturation);
}

static void draw_license_trophy(void *user,const race_result_trophy_pose *pose)
{
    race_game *g=user;if(!g || !pose || pose->model<0 || pose->model>2)return;
    /* 003E1B58 loads race/trophy.mdl once; 003E18D0 selects the CLUT
       variant using 006A38F0. All three use geometry model zero. */
    if(!g->trophy_tried){
        g->trophy_tried=1;size_t size=0;uint8_t *bytes=gt4_vol_read(g->vol,"/race/trophy.mdl",&size);
        for(int i=0;bytes && i<3;i++){
            if(!gt4_license_trophy_variant(bytes,size,(unsigned)i))continue;
            gt4_model *m=gt4_model_decode_part(bytes,size,0,0);
            if(m){g->trophy[i]=r3d_mesh_create(m);
                fprintf(stderr,"licence trophy: model=%d triangles=%d unsupported=%d\n",i,m->count,m->unsupported);gt4_model_free(m);}
        }free(bytes);
    }
    if(!g->trophy[pose->model] || pose->alpha<=0)return;
    /* 003E0FF8 +0x108..0x154: perspective 50 degrees, 640/448
       aspect, near 0x3DCCCCCC; translate, then RotateY, RotateX, RotateZ.
       A: one spatial sample is drawn; the PS2 three-sample accumulation
       and exact field/composite transform are still pending. */
    const float rad=.01745329252f;float a=pose->angles[0]*rad,b=pose->angles[1]*rad,c=pose->angles[2]*rad;
    float ca=cosf(a),sa=sinf(a),cb=cosf(b),sb=sinf(b),cc=cosf(c),sc=sinf(c);
    r3d_view v={0};float rot[9]={cb*cc+sb*sa*sc,-cb*sc+sb*sa*cc,sb*ca,ca*sc,ca*cc,-sa,-sb*cc+cb*sa*sc,sb*sc+cb*sa*cc,cb*ca};
    memcpy(v.rot,rot,sizeof rot);memcpy(v.pos,pose->position,sizeof v.pos);
    v.r[0]=v.u[1]=1;v.f[2]=-1;v.focal=224/tanf(50*rad*.5f);v.near_clip=.099999994f;
    /* 003E1780: light 0 at (0,1,0,0), ambient 0x3E999999,
       diffuse/specular 0.5; no guessed studio lights. */
    r3d_ui_light light={0};light.direction[1]=1;
    for(int i=0;i<3;i++){light.ambient[i]=.299999982f;light.diffuse[i]=light.specular[i]=.5f;}
    r3d_begin_ui(0,0,640,480);
    v.ghost=1;r3d_draw_ui(g->trophy[pose->model],&v,&light);
    v.ghost=2;v.ghost_alpha=pose->alpha;r3d_draw_ui(g->trophy[pose->model],&v,&light);r3d_end();
}

/* the results' play_sound: "ok" on the confirm */
static void result_sound(void *user, const char *name)
{
    race_game *g = user;
    race_audio_menu_sound(g->audio, name, g->se_volume);
}

/* the pause menu over the race or the replay */
static void draw_pause(race_game *g, double dt)
{
    if (!g->pause) return;
    race_pause_update(g->pause, dt);
    race_pause_draw(g->pause);
    int state = race_pause_state(g->pause);
    if (state == RACE_PAUSE_EXIT) g->quit = 1;
    if (state != RACE_PAUSE_OPEN) { race_pause_close(g->pause); g->pause = NULL; race_audio_pause(g->audio, 0); }
    if(state==RACE_PAUSE_RETRY){
        g->retry_requested=1; /* app owns the loading transition, as on Start */
    }
}

/* over the replay: the plate a while after a button, the hold */
static void replay_overlay(race_game *g)
{
    if (g->label && g->label_v > 0) {
        race_hud_draw_replay(g->hud, g->label, g->label_v, 0);
    }
    if (g->held) race_hud_draw_simple_pause(g->hud);
}

/* 0032F028 +0x1C0..1D4 binds the same ResultArcade movie in Battle2P.
   A: its full split-world presentation timing is not yet compared. */
static void draw_result(race_game *g,int cars,double dt)
{
    race_result_state rs;
    const uint8_t *keys = SDL_GetKeyboardState(NULL);
    int confirm = keys[SDL_SCANCODE_RETURN] || (g->pad && SDL_GameControllerGetButton(g->pad, SDL_CONTROLLER_BUTTON_START));
    if(g->split){plat_pad_state p;if(plat_input_poll(1,&p))confirm|=!!(p.buttons&(1u<<GT4_PAD_START));}
    {   /* tests: OPENGT4_RACE_RESULT_CONFIRM presses the confirm on the results, on and off */
        static unsigned tick;
        if (getenv("OPENGT4_RACE_RESULT_CONFIRM") && g->finished > 8) confirm = (++tick / 20) & 1;
    }
    if (!g->result) {
        int32_t state;uint32_t time;
        g->result = race_game_license_result(g,&state,&time)?race_result_license_open(g->vol,state,time,g->last_desc.license_code,gt4_career_license_time(g->last_event_row,2),g):race_game_mission_active(g)?race_result_mission_open(g->vol,draw_icon,g):race_result_open(g->vol, draw_icon, g);
        race_result_set_sound(g->result, result_sound);
        race_result_set_trophy(g->result,draw_license_trophy);
        race_result_set_screen(g->result,draw_license_screen);
    }
    memset(&rs, 0, sizeof rs);
    rs.cars = cars < 6 ? cars : 6;
    rs.player = 0;
    for (int i = 0; i < rs.cars; i++) {
        gt4_race_hud c;
        if (!gt4_race_run_hud(g->run, i, &c)) continue;
        rs.rank[i] = c.rank;
        rs.finished[i] = c.finished;
        rs.time[i] = c.line_time;
        rs.laps[i] = c.lap;
        rs.name[i] = g->names[i];
        /* 003BE060..: RaceMachineTest's board, mode 4 for the top speed
           (car +0x6CC), else 5: the entry's laps added up (+0x1B0) */
        if (g->world_mode == 15) {
            uint32_t car = gt4_race_run_car(g->run, i), race = GT4_U32(gt4_race_run_conductor(g->run));
            uint32_t entry = GT4_U32(GT4_U32(GT4_U32(race + 0x60) + 8) + (uint32_t)i * 4);
            int test = g->laps < 4 ? g->laps : 0;
            if (test == 0 || test == 3) { rs.mode = 4; rs.top_speed[i] = GT4_F32(car + 0x6cc); }
            else { rs.mode = 5; rs.time[i] = GT4_U32(entry + 0x1b0); }
            rs.mph = g->unit == 1;
        }
    }
    if (confirm && !g->confirm_held) race_result_press(g->result);
    g->confirm_held = confirm;
    race_result_update(g->result, &rs, dt);
    /* Test-only key stream for the authored result menu. It neither
       sets a result nor grants a medal. Seconds are presentation time. */
    const char *result_keys=getenv("OPENGT4_LICENSE_RESULT_KEYS");
    if(g->last_prepared && license_exam(g) && result_keys){
        g->license_result_test_clock+=dt;unsigned at=0;
        for(const char *item=result_keys;item && *item;item=strchr(item,',')?strchr(item,',')+1:NULL,at++){
            const char *colon=strchr(item,':');
            if(colon && at==g->license_result_test_sent && g->license_result_test_clock>=atof(item)){
                int key=atoi(colon+1);race_result_key(g->result,key);g->license_result_test_sent++;
            }
        }
    }
    race_result_draw(g->result);
}

static void split_hud(race_game *g,double dt)
{
    for(int i=0;i<2;i++){
        race_hud **h=i?&g->hud_2p:&g->hud;
        if(!*h){*h=race_hud_open(g->vol);race_hud_set_course(*h,g->last_course);}
        gt4_race_hud r;race_hud_state s={0};
        if(!*h || !gt4_race_run_hud(g->run,i,&r))continue;
        s.rank=r.rank;s.cars=2;s.lap=r.lap;s.laps=r.laps;
        s.lap_time=r.lap_time;s.best_lap=r.best_lap;s.last_lap=r.last_lap;
        s.speed=r.speed;s.rpm=r.rpm;s.gear=r.gear;s.throttle=r.throttle;s.brake=r.brake;
        s.replay=g->replaying;
        s.side_g=r.side_g;s.shift_lamp=r.shift_lamp;s.fuel=r.fuel;s.fuel_cap=r.fuel_cap;
        s.rev_limit=r.rev_limit;s.aid=r.aid;s.brake_lock=r.brake_lock;s.driving=r.driving;
        s.start_frames=r.start_frames;s.count_frames=r.count_frames;s.rolling=r.rolling;
        s.start_seconds=r.start_seconds;s.intro_elapsed=r.intro_elapsed;s.finished=r.finished;
        s.speed_scale=(float)(r.speed_steps*(g->unit==1?50:80));s.tacho_scale=r.tacho_scale;
        s.odometer=r.odometer;s.unit=g->unit;s.signal=r.signal;
        /* 003A2360 mode 8: dedicated horizontal Battle2P layout. */
        s.split=1;s.player=i;s.panel=0;
        for(int j=0;j<2;j++)s.map_show[j]=gt4_race_run_xy(g->run,j,&s.map_x[j],&s.map_y[j]);
        plat_video_set_view(640,448);video_ui_panel(i*224.f,224);
        race_hud_draw(*h,&s,g->pause?0.f:(float)dt);
        video_ui_panel(0,0);
    }
    plat_video_set_view(640,448);
    /* Keep each driver's view until both original finish flags are set.
       A: Battle2P result activation ordering still needs a world oracle. */
    if(!g->replaying && gt4_race_run_finished(g->run) && GT4_U8(player_2p+0x5b6))draw_result(g,2,dt);
    draw_pause(g,dt);
}

void race_game_draw_hud(race_game *g, double dt)
{
    gt4_race_hud r;
    race_hud_state s;
    if (!g || !g->ready || !g->run || !g->vol) return;
    if(g->cinema.on && g->cinema.clean){if(g->pause)draw_pause(g,dt);return;}
    if(g->split){split_hud(g,dt);return;}
    g->pad=plat_sdl_pad(0);
    if (getenv("OPENGT4_RACE_SNAPSHOT")) return;   /* tests: the captured scene alone */
    if (getenv("OPENGT4_RACE_FILM")) return;   /* P: clean filming frame */
    /* the replay: the display off as replay_display_enable's default
       (P: what it shows when on is not read) */
    if (!g->hud) { g->hud = race_hud_open(g->vol); race_hud_set_icon(g->hud, hud_icon, g); }
    {
        char title[96], artist[96];
        if (race_audio_music_title(g->audio, title, sizeof title, artist, sizeof artist)) {
            static char last[96];
            race_hud_music(g->hud, title, artist);
            if (getenv("OPENGT4_SOUND_TRACE") && strcmp(last, title)) {
                fprintf(stderr, "race music title: %s / %s\n", title, artist);
                snprintf(last, sizeof last, "%s", title);
            }
        } else race_hud_music(g->hud, "", "");   /* no racing track yet in this race */
    }
    if (g->hud_course[0]) {   /* the course's map and logo (the replay's opening shows the logo) */
        race_hud_set_course(g->hud, g->hud_course);
        g->hud_course[0] = 0;
    }
    if (g->replaying && g->rmode != 2) {   /* Normal and Dive: no display, the logo */
        race_hud_draw_replay(g->hud, NULL, 0, 1);
        race_hud_draw_music(g->hud, g->held ? 0.f : (float)dt);
        replay_opening(g);
        replay_overlay(g);
        plat_video_set_view(640, 448);   /* the frame's 448 lines, as race_hud_draw leaves it: no black band below */
        return;
    }
    if (!g->hud) { g->hud = race_hud_open(g->vol); race_hud_set_icon(g->hud, hud_icon, g); }
    if (g->hud_course[0]) {
        race_hud_set_course(g->hud, g->hud_course);
        g->hud_course[0] = 0;
    }
    if (!gt4_race_run_hud(g->run, watched_car(g), &r)) return;
    memset(&s, 0, sizeof s);
    s.rank = r.rank;
    s.cars = gt4_race_run_cars(g->run);
    s.lap = r.lap;
    s.laps = r.laps;
    s.lap_time = r.lap_time;
    s.best_lap = r.best_lap;
    s.last_lap = r.last_lap;
    s.speed = r.speed;
    s.rpm = r.rpm;
    s.gear = r.gear;
    s.throttle = r.throttle;
    s.brake = r.brake;
    s.replay = g->replaying;
    s.side_g = r.side_g;
    s.shift_lamp = r.shift_lamp;
    s.fuel = r.fuel;
    s.fuel_cap = r.fuel_cap;
    {   /* 0039E790..0039E7C8: meter follows service state, not camera. */
        uint32_t car=gt4_race_run_car(g->run,watched_car(g));
        s.refueling=gt4_pit_refueling(GT4_U16(car+0x5bc),GT4_U16(car+0x5be));
    }
    s.rev_limit = r.rev_limit;
    s.aid = r.aid;
    s.brake_lock = r.brake_lock;
    s.driving = r.driving;
    s.start_frames = r.start_frames;
    s.count_frames = r.count_frames;
    s.rolling = r.rolling;
    s.start_seconds = r.start_seconds;
    s.intro_elapsed = r.intro_elapsed;
    s.aspec_points = r.aspec_points;
    s.finished = r.finished;
    if(g->last_prepared && license_exam(g)){
        /* RaceDisplayLicense 003BD150/003BD4F0: reference times and
           contact count belong to the examination conductor. */
        uint32_t cond=gt4_race_run_conductor(g->run);
        s.license=1;
        for(int i=0;i<3;i++)s.license_times[i]=gt4_career_license_time(g->last_event_row,i);
        s.license_pylon_limit=GT4_U8(cond+0xf85e);
        s.license_pylons=(int16_t)GT4_U16(cond+0x1003c);
        s.license_wait=gt4_license_wait(cond,0);
        int shown=0;for(int i=0;i<s.cars && i<6;i++){float x,y;if(gt4_race_run_xy(g->run,i,&x,&y)==3)shown++;}
        s.license_rank=shown>=2;
    }
    s.speed_scale = (float)(r.speed_steps * (g->unit == 1 ? 50 : 80));
    s.tacho_scale = r.tacho_scale;
    s.odometer = r.odometer;
    for (int i = 0; i < s.cars && i < 6; i++) s.map_show[i] = gt4_race_run_xy(g->run, i, &s.map_x[i], &s.map_y[i]);
    s.unit = g->unit;
    s.signal = r.signal;
    {   /* func_0035FC80 reads the car's +0x712 while it has not finished */
        uint32_t car = gt4_race_run_car(g->run, 0);
        s.penalty = car && !r.finished ? (int)GT4_U16(car + 0x712) : 0;
    }
    s.panel = g->camera != 1 || g->start_cam_on || g->tv_on;   /* the onboard panel with the views from the car (and the start's) */
    if (g->replaying) s.panel = g->car_cam;   /* the replay: the simple panel under the TV cameras, the gauges on the car (rv_02, rv_05) */
    if (g->bspec_ui) {   /* B-Spec's display (003A2360 cases 2/3, the panel's +0x18): no gauges, the race's time, the tyres */
        uint32_t cond=gt4_race_run_conductor(g->run),car=gt4_race_run_car(g->run,watched_car(g));
        s.panel=0;s.bspec=1;
        s.total_time=GT4_U32(cond+0xf898u)/3;                     /* func_00358440 */
        /* 003D80A8: each tyre's colour by the car's wheel layout (00359538:
           D_00620478 + params+0x30 * 0x54, kind 0..3) from its wheel's
           heat (003446D8: car + wheel * 0xEC + 0x215) through 003677F8 */
        int kind=GT4_U8(0x00620478u+GT4_U8(GT4_U32(car+0x10)+0x30)*0x54u);
        /* 003D80A8 +0x10..0x58: black unless the race wears its tyres
           (00367D40: cond+4 +0x68 or +0x69) and the player's car is watched */
        int shown=(GT4_U8(cond+4+0x68) || GT4_U8(cond+4+0x69)) && watched_car(g)==0;
        for(int i=0;i<4;i++){
            if(!shown){memset(s.tire_rgb[i],0,3);continue;}
            int k=kind==1?(i<2?0:i-1):kind==2?(i<3?i:2):kind==3?i>>1:i;
            int t=(int8_t)GT4_U8(car+(uint32_t)k*0xec+0x215);
            uint8_t r=0,gg=0,b=0;
            if(t>=0x40){r=0xfc;gg=(uint8_t)((0x7f-t)*2);}
            else if(t>=0x20){r=0xfc;gg=(uint8_t)((0x5f-t)*4);}
            else if(t>=0){r=(uint8_t)(t*8);gg=0xfc;}
            else if(t>=-0x3f){b=(uint8_t)(-t*4);gg=(uint8_t)(0xfc+t*4);}
            s.tire_rgb[i][0]=r;s.tire_rgb[i][1]=gg;s.tire_rgb[i][2]=b;
        }
    }
    int license_failed=0;
    if(s.license && r.finished){int32_t state;uint32_t time;
        license_failed=race_game_license_result(g,&state,&time) && state<=0;
    }
    if(license_failed){
        /* 003E0E38 "screen" owns the race viewport during failure.
           Preserve this frame before the HUD; other result primitives
           then draw over a black canvas, as the slot 03 reference. */
        g->license_screen=video_grab(g->license_screen,0,0,640,448);
        const uint8_t black[4]={0,0,0,255};
        plat_video_set_view(640,480);plat_draw_quad(NULL,0,0,640,480,0,0,1,1,black);
        plat_video_set_view(640,448);
    }else if(g->bspec_ui && race_bspec_mode(g->bspec_ui)==1){
        /* B-Spec's Race Monitor over the whole screen (003DA150: the race
           not drawn under it, +0x38) */
        const uint8_t black[4]={0,0,0,255};
        plat_video_set_view(640,480);plat_draw_quad(NULL,0,0,640,480,0,0,1,1,black);
        g->bspec_hud=&s;race_bspec_draw(g->bspec_ui);g->bspec_hud=NULL;
        race_hud_draw_tooltip(g->hud,race_bspec_tooltip(g->bspec_ui),race_bspec_light(g->bspec_ui));
        plat_video_set_view(640,448);
    }else{
        race_hud_draw(g->hud, &s, g->pause ? 0.f : (float)dt);   /* held with the race */
        if(g->bspec_ui){
            g->bspec_hud=&s;race_bspec_draw(g->bspec_ui);g->bspec_hud=NULL;
            race_hud_draw_tooltip(g->hud,race_bspec_tooltip(g->bspec_ui),race_bspec_light(g->bspec_ui));
            plat_video_set_view(640,448);
        }
    }
    /* the results, from the player's finish (ResultArcade's movie) */
    if(r.finished && !g->replaying && g->mission_presentation<GT4_MISSION_RETURN)draw_result(g,s.cars,dt);
    if (g->replaying) {
        replay_opening(g);
        replay_overlay(g);
        plat_video_set_view(640, 448);
    }
    else {
        race_pitmenu_draw(g->pitmenu);   /* 003DD690, under the pause */
        draw_pause(g, dt);
    }
}

/* 0011ABE0: Photo Drive single-car RACE mode 0, or a resident replay.
   This borrows only the renderer; the command tape and solver stay paused. */
race_view *race_game_photo_view(race_game *g){return g && g->ready && !g->split && (g->replaying || (g->last_prepared && !g->last_desc.license && g->last_desc.cars==1 && g->last_desc.event_row[0x43]==0))?g->view:NULL;}
void race_game_photo_pause(race_game *g,int on){if(g){g->photo_held=on!=0;race_audio_pause(g->audio,on || g->held);if(on){plat_input_set_ffb(0,0);plat_input_set_ffb(1,0);}}}

int race_game_photo_camera_data(race_game *g,float size[4],float *distance){
 if(!g || !g->run || !size || !distance)return 0;uint32_t car=gt4_race_run_car(g->run,watched_car(g)),params=GT4_U32(car+0x10);
 /* 003F3708/20/38/48, frozen car parameters; 003FDC50 distance. */
 size[0]=GT4_F32(params)+GT4_F32(params+0x50);size[1]=GT4_F32(params+4)-GT4_F32(params+0x50);size[2]=GT4_F32(params+8);size[3]=GT4_F32(params+12);*distance=GT4_F32(car+0x5a8);return 1;
}
