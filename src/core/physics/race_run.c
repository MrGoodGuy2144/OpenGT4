/*
 * A race run natively (race_run.h). The race's objects are the ones the
 * Arcade's setup reads before building the rest (docs/analysis/PHYSICS.md,
 * "Out of the snapshot"), at the addresses of the Arcade captures; the
 * course's runway and lines come from its files as the game loads them;
 * each car's garage entry from the SpecDB; then the race init
 * (func_0034D490), the race's start (func_003C0190) and, per frame, the
 * race's parts around the step.
 */
#include "race_run.h"
#include "course_objects.h"
#include "license_session.h"
#include "pcm_music.h"
#include "car.h"
#include "career_original.h"
#include "course.h"
#include "inflate.h"
#include "spectables.h"
#include "specdb.h"
#include "vol.h"

#include <fenv.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RAM_SIZE    0x02000000u
#define CORE_MAGIC  0x176265b3u
#define CORE_RECORDS 0x12cu

/* the race's objects (as in the Arcade captures) */
#define COND        0x01cf8ed0u
#define RACE        0x01c3f620u
#define ENTRIES     0x00a47cb0u
#define WORLD       0x01ce6910u
#define PROFILE     0x01cf8cdcu   /* the player's driver profile */
#define AI_PROFILE  0x00622b60u   /* the AI's, in the executable's data */
#define GHOST_PROFILE 0x01e31000u /* the Time Trial's second car's: a player's
                                     kind as the TT captures give it, in
                                     memory the setup does not otherwise use */
#define SETTINGS    0x00bcd010u   /* race+0x70: the race's settings */
#define COURSE      0x00bcd180u
#define COURSE_DATA 0x00ccd600u   /* course+4: the runway, the lines after it */
#define COURSE_END  0x01b00000u

#define RACE_VT      0x00679ce8u
#define TT_VT        0x00685890u  /* the Time Trial's session: DynamicsConductorFreeRun (003F54E0) */
#define TT_WORLD     0x00680140u  /* the Time Trial's world: RaceFreeRun (003B9A18) */
#define ARCADE_WORLD 0x0067bad8u  /* RaceArcadeSingle's table (world+0x64) */
#define GT_WORLD     0x00684678u  /* func_003EDE50 +0x38 */
#define CHAMP_WORLD  0x00684bf0u  /* func_003EF498 +0x50 */
#define PROFILE_VT   0x00686618u

/* guest memory the rewritten code borrows: below where the game's stack
   would be */
#define GARAGE_SCRATCH 0x01e32000u
#define PHYS_SCRATCH   0x01ee0000u
#define INIT_SCRATCH   0x01eef900u
#define STEP_BUF       0x01eeffc0u

struct gt4_race_run {
    gt4_course_gadgets *objects;
    uint8_t *ram;
    gt4_vol *vol;
    gt4_spec_tables *tables;
    gt4_specdb_src db;
    uint32_t cond;
    int cars;
    unsigned frames;                /* display time since the start event, at 60 Hz */
    int aspec_points;                /* func_003C0FF8, computed after the entries start */
    float height[6], travel0[6][4];   /* at the start: ride height, suspension travel */
    /* The Time Trial's ghost (the world's objects at vt+0x240): the
       player's commands as the replay records them (func_003F59A0 ->
       00426C50), the lap being recorded and the best one, each the
       player's car at its lap's start and its commands from there */
    uint8_t *tape;
    size_t tape_n, tape_cap;
    struct ghost_lap { uint8_t car[GT4_GHOST_CAR]; uint32_t rest; float frac; size_t from, to; int ok, saved; } rec, ghost;
    uint8_t *saved_tape;            /* a saved best lap's commands (gt4_race_run_ghost_set) */
    uint32_t ghost_time;            /* the ghost's lap (func_003F5580: 0x157529FF none) */
    size_t ghost_at;                /* the next command the ghost reads */
    int ghost_on;
    gt4_phys_pads outer;            /* the caller's controller, around the ghost's */
};

static const uint32_t entry_at[6] = { 0x01bd7710u, 0x01bdacf0u, 0x01cd9190u, 0x01cdc770u, 0x01cdfd50u, 0x01ce3330u };

static void w32(uint32_t a, uint32_t v) { GT4_U32(a) = v; }
static uint32_t le32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

/* Nothing is left to the original: a call there is a bug, reported once */
static uint32_t missing;
static float no_original(uint32_t addr, uint32_t a0, uint32_t a1, float f12, float f13)
{
    (void)a0; (void)a1; (void)f12; (void)f13;
    /* 0035AF10 +0..4: jr ra / daddu v0,zero,zero. This virtual query
       is reached by endurance; it has no memory writes or solver work. */
    if (addr == 0x0035af10u) { gt4_orig_v0 = 0; return 0; }
    /* 0038B980 +0..c: return the word at this+0xE408. Pit-entry
       virtual query; preserve the existing session value. */
    if (addr == 0x0038b980u) { gt4_orig_v0 = GT4_U32(a0 + 0xe408u); return 0; }
    if (!missing) fprintf(stderr, "race: original function 0x%08x called\n", addr);
    missing = addr;
    return 0;
}

/* CORE.GT4 from the disc's root: raw deflate after 6 bytes (01 01, size),
   load records {address, size, data} from 0x12C (docs/analysis/CORE_FORMAT.md) */
static int load_image(uint8_t *ram, const char *disc, char *msg, size_t len)
{
    size_t n = 0;
    uint8_t *raw = gt4_disc_read_root(disc, "CORE.GT4", &n), *img;
    uint32_t size;
    if (!raw || n < 6 || raw[0] != 1 || raw[1] != 1) {
        snprintf(msg, len, "no CORE.GT4 on the disc");
        free(raw);
        return 0;
    }
    size = le32(raw + 2);
    img = malloc(size ? size : 1);
    if (!img || core_inflate_raw(raw + 6, n - 6, img, size) != size || size < CORE_RECORDS || le32(img + 0x100) != CORE_MAGIC) {
        snprintf(msg, len, "CORE.GT4: cannot unpack (another disc?)");
        free(raw);
        free(img);
        return 0;
    }
    free(raw);
    for (uint32_t at = CORE_RECORDS; at + 8 <= size;) {
        uint32_t va = le32(img + at), sz = le32(img + at + 4);
        at += 8;
        if (at + sz > size || va + sz > RAM_SIZE) {
            snprintf(msg, len, "CORE.GT4: record out of range");
            free(img);
            return 0;
        }
        memcpy(ram + va, img + at, sz);
        at += sz;
    }
    free(img);
    return 1;
}

/* The objects the setup reads (40 words found on a capture) */
static void make_race(int cars, int laps, int time_trial)
{
    w32(0x006244e8u, 1);                       /* the display units */
    w32(0x00874d50u, 0xba8350bdu);             /* the random generator's state */
    w32(0x00a46a94u, 0x00687ff0u);             /* the controller's object: its table */
    w32(RACE, (uint32_t)(laps > 0 ? laps : 2));   /* the laps (session +0x128, func_003577F0) */
    w32(RACE + 0x60, ENTRIES);
    w32(RACE + 0x70, SETTINGS);
    w32(RACE + 0x78, 0x0000000au);             /* the COURSE row's key */
    w32(RACE + 0x80, COURSE);
    w32(RACE + 0x84, WORLD);
    w32(WORLD + 0x64, ARCADE_WORLD);
    w32(WORLD + 0x70, COND);
    w32(WORLD + 0xd68, 1);                   /* Arcade Single Race (capture ar_02) */
    w32(COURSE + 0x04, COURSE_DATA);
    /* no layout variant (cond+0xF839 -1 in every capture: Arcade, Time
       Trial, Nurburgring): the race's start (func_00395220) then keeps the
       course on the runway's own grid and timing points (course+0xB0 -1),
       the sections the display's T1, T2... come from */
    GT4_U8(COND + 0xf839u) = 0xff;
    GT4_U8(SETTINGS + 0x40) = 2;               /* ten seconds (func_003C0208, kind + 1) */
    w32(ENTRIES + 0x00, (uint32_t)cars);
    w32(ENTRIES + 0x08, ENTRIES + 0x24);
    for (int i = 0; i < cars; i++) {
        w32(ENTRIES + 0x24 + (uint32_t)i * 4, entry_at[i]);
        w32(entry_at[i] + 0x343c, i ? (time_trial ? GHOST_PROFILE : AI_PROFILE) : PROFILE);
        /* the grid slot; the Time Trial's two cars both 0 (a_parado, nur_02) */
        w32(entry_at[i] + 0x3444, time_trial ? 0u : (uint32_t)(i ? i - 1 : cars - 1));
        w32(entry_at[i] + 0x347c, 1);
    }
    w32(PROFILE + 0xd0, PROFILE_VT);
    w32(PROFILE + 0x1cc, 1);                   /* the car is not held at the start */
    w32(AI_PROFILE + 0xd0, PROFILE_VT);
    if (time_trial) {
        w32(GHOST_PROFILE + 0xd0, PROFILE_VT);
        w32(GHOST_PROFILE + 0x1cc, 1);
    }
    w32(COND, RACE);
    w32(COND + 0x10140, RACE_VT);
    w32(COND + 0xf864, 0x50);
    w32(COND + 0xf8a0, (uint32_t)cars);
}

/* The Arcade's tyres: the car's parts of that compound level (its parts
   list, CARS/<car>: FRONTTIRE 0x19 and REARTIRE 0x1A) into its garage
   entry's front and rear tyre (+0x60, +0x68), as the two Arcade captures'
   entries have them (level = the driving options' tyre index) */
static void arcade_tyres(uint32_t gar, uint32_t car, int level, const gt4_specdb_src *db)
{
    size_t size = 0;
    uint8_t *parts = db->car_parts ? db->car_parts(db->ctx, car, &size) : NULL;
    int32_t n;
    if (!parts || size < 8) { free(parts); return; }
    memcpy(&n, parts, 4);
    for (uint32_t table = 0x19; table <= 0x1a; table++)
        for (int32_t i = 0; i < n && 8 + (size_t)i * 16 + 16 <= size; i++) {
            const uint8_t *e = parts + 8 + (size_t)i * 16;
            uint32_t t;
            memcpy(&t, e + 4, 4);
            if (t != table || e[8] != level) continue;
            memcpy(gt4_ptr(gar + (table == 0x19 ? 0x60 : 0x68)), e, 8);
            break;
        }
    free(parts);
}

/* The course's runway (at its own offset in the course file, as the game
   has it) and the .ad file's blocks after it */
static int load_course(gt4_race_run *run,gt4_vol *vol, const char *name, char *msg, size_t len)
{
    char path[64];
    size_t n = 0, an = 0;
    uint8_t *crs, *adf;
    gt4_course co;
    uint32_t size, rw, ad;
    snprintf(path, sizeof path, "/crs/%s", name);
    crs = gt4_vol_read(vol, path, &n);
    snprintf(path, sizeof path, "/crs/%s.ad", name);
    adf = gt4_vol_read(vol, path, &an);
    if (!crs || !adf || an < 24 || !gt4_course_open(&co, crs, n)) {
        snprintf(msg, len, "cannot read course %s", name);
        free(crs);
        free(adf);
        return 0;
    }
    size = le32(crs + co.runway + 8);
    rw = COURSE_DATA + (uint32_t)co.runway;
    ad = (rw + size + 15u) & ~15u;
    if (size > n - co.runway || an > 0x40000u || ad + an > COURSE_END) {
        snprintf(msg, len, "course %s too big", name);
        free(crs);
        free(adf);
        return 0;
    }
    memcpy(gt4_ptr(rw), crs + co.runway, size);
    gt4_runway_relocate(rw);
    w32(COURSE_DATA + 0xdc, rw);
    memcpy(gt4_ptr(ad), adf, an);
    for (uint32_t k = 0; k < 5; k++) {
        uint32_t off = le32(adf + 4 + k * 4);
        w32(COURSE + 0x9c + k * 4, off ? ad + off : 0);
    }
    run->objects=gt4_course_gadgets_open_state(&co);
    free(crs);
    free(adf);
    return 1;
}

gt4_race_run *gt4_race_run_open(const char *disc, const gt4_race_desc *d, char *msg, size_t len)
{
    gt4_race_run *r;
    const char *err = NULL;
    fenv_t env;
    msg[0] = 0;
    gt4_race_desc tt;
    if (d && d->time_trial && d->cars == 1) {
        /* the Time Trial's entries: the player's car and its ghost, the
           same car (the TT captures' two entries, car 0x3B both) */
        tt = *d;
        tt.cars = 2;
        tt.car[1] = d->car[0];
        tt.owned_car[1] = d->owned_car[0];
        tt.tuned_car[1] = d->tuned_car[0];
        tt.event_paint[1] = d->event_paint[0];
        d = &tt;
    }
    if (!d || d->cars < 1 || d->cars > 6 || (d->split_battle && (d->cars!=2 || d->time_trial || d->event_row))) {
        snprintf(msg, len, "a race has 1 to 6 cars");
        return NULL;
    }
    r = calloc(1, sizeof *r);
    if (!r || !(r->ram = calloc(1, RAM_SIZE))) {
        free(r);
        snprintf(msg, len, "out of memory");
        return NULL;
    }
    r->cond = COND;
    r->cars = d->cars;
    r->ghost_time = 0x157529ffu;
    gt4_phys_mem = r->ram;
    gt4_orig_call = no_original;
    gt4_phys_headless = 1;
    gt4_phys_scratch = PHYS_SCRATCH;
    missing = 0;
    if (!load_image(r->ram, disc, msg, len)) goto fail;
    r->vol = gt4_vol_open(disc, &err);
    if (!r->vol) {
        snprintf(msg, len, "disc: %s", err ? err : "cannot open");
        goto fail;
    }
    r->tables = gt4_spec_tables_open(r->vol, NULL);
    r->db.row = gt4_spec_tables_row;
    r->db.ctx = r->tables;
    r->db.str = gt4_spec_tables_str;
    r->db.car_parts = gt4_spec_tables_car_parts;
    gt4_phys_specdb = &r->db;
    make_race(d->cars, d->laps, d->time_trial);
    if(d->split_battle){
        /* 003470B0 +0x18: Battle2P table. 0032F028 +0x8C..0x120
           binds a separate human controller to each entry. Host profiles
           use existing scratch storage; no dynamics parameters change. */
        w32(COND+0x10140,0x00679988u);
        w32(WORLD+0x64,0x00676c48u);
        w32(entry_at[1]+0x343c,GHOST_PROFILE);
        w32(GHOST_PROFILE+0xd0,PROFILE_VT);w32(GHOST_PROFILE+0x1cc,1);
        /* 003471A8 / 00347108 / 00347188: independent battle rules. */
        w32(RACE+0x14,d->split_boost);w32(RACE+0x18,d->split_handicap);
        w32(RACE+0x1c,d->split_tire_damage);
    }
    if (d->time_trial) {
        /* the Time Trial captures (a_parado, b_reta, nur_02): its session
           and world tables, the world's mode 2 (ARCADE_TIME_TRIAL in the
           mode names at 0069FE58), no car-against-car penalty threshold
           (cond+0xF864 0) */
        w32(COND + 0x10140, TT_VT);
        w32(WORLD + 0x64, TT_WORLD);
        w32(WORLD + 0xd68, 2);
        w32(COND + 0xf864, 0);
    }
    if (d->event_row) {
        /* 0034D418 +0x14..0x30 binds the decoded RACE object through
           0035E108 to conductor+0xf818. Constructors 003EDE50 and
           003EF498 set the GT world type/mode, not Arcade Single. */
        memcpy(gt4_ptr(SETTINGS), d->event_row, 151);
        gt4_career_session_rules(d->event_row, gt4_ptr(COND + 0xf818));
        /* 003BF6E8 (RaceMission): world 006815E0, mode 10, conductor
           00685DA0; otherwise the training's (003ECD40 their base) */
        w32(WORLD + 0x64, d->license == 3 ? 0x006815e0u : d->license == 2 ? 0x00683c20u : d->license ? 0x006807a8u : d->championship ? CHAMP_WORLD : GT_WORLD);
        w32(WORLD + 0xd68, d->license == 3 ? 10 : d->license ? 3 : d->championship ? 14 : 13);
        GT4_U32(RACE) = d->event_row[0x43];
        if(d->license){
            /* 003BDB98 reserves two entries; 003BB478 clamps the count.
               003BDC88 copies the exam setup, with grid slot zero. */
            int capacity=d->license>=2?6:2;
            int expected=d->event_row[0x3e]<capacity?d->event_row[0x3e]:capacity;
            if(d->cars!=expected){snprintf(msg,len,"invalid licence entry count");goto fail;}
            /* 003ED298 +0x38/+0x74 installs the training world/session.
               003ECC70 keeps the normal grid; only the student receives
               garage +0x178=0x100 (003ECB68 +0x40..0x48). */
            w32(COND+0x10140,d->license==3?GT4_MISSION_SESSION_VT:d->license==2?GT4_TRAINING_SESSION_VT:GT4_LICENSE_SESSION_VT);
            for(int i=0;i<d->cars;i++){
                if(d->license>=2){
                    if(!i)w32(entry_at[i]+0x198,0x100);
                    /* Guide entries retain 003B4B38 +0x11c..0x124
                       defaults; player options below only replace entry 0.
                       Confirmed in the B-5 slot-2 initial scene. */
                    w32(entry_at[i]+0x3450,1);w32(entry_at[i]+0x3454,1);
                    if(i)w32(entry_at[i]+0x3458,1);
                }
                else {w32(entry_at[i]+0x3444,0);w32(entry_at[i]+0x198,0x100u+(uint32_t)i);}
            }
        }
    }
    {   /* the race's COURSE row (race+0x78) by the course's label: its
           settings (func_00447F48) say, among others, whether it starts
           rolling (cond+0xCBE6) */
        size_t n = 0;
        uint8_t *labels = gt4_vol_read(r->vol, "/specdb/GT4_US2560/COURSE.idi", &n);
        uint32_t key;
        if (labels && gt4_specdb_label(labels, n, d->course, &key)) w32(RACE + 0x78, key);
        w32(SETTINGS + 0xa0, GT4_U32(RACE + 0x78)); /* same COURSE key, as in ar_02 */
        free(labels);
    }
    {
        /* the player's driving options into its entry, as the Arcade's
           race start does (func_00111B50, func_00111DC0): the gearbox
           (+0x3448, the car's shift kind through the session's 0xE8) and
           the aids' level (func_00110B70: 2 with both, 1 with TCS alone)
           as the stability aids (+0x3450) and traction control (+0x3454) */
        int level = d->assist_asm ? (d->assist_tcs ? 2 : 0) : d->assist_tcs != 0;
        w32(entry_at[0] + 0x3448, d->manual != 0);
        w32(entry_at[0] + 0x3450, level == 2);
        w32(entry_at[0] + 0x3454, level != 0);
        /* the Time Trial's ghost drives the player's commands again: its
           car gets the same gearbox and aids, or its TCS / ASM answer them
           otherwise (TCS on, Escort WRC at Laguna Seca: 852 m off a lap,
           tt_game_probe PROBE_TCS). A: the PS2's own entry 1 holds 1, 1 in
           nur_02/03 with the player's 0, 0 (both off in the slot-2 capture);
           which of the car's blocks those reach is not read */
        if (d->time_trial && d->cars == 2)
            for (uint32_t o = 0x3448; o <= 0x3454; o += 4) w32(entry_at[1] + o, GT4_U32(entry_at[0] + o));
    }
    if(d->split_battle){
        int level=d->assist_asm_2p?(d->assist_tcs_2p?2:0):d->assist_tcs_2p!=0;
        w32(entry_at[1]+0x3448,d->manual_2p!=0);
        w32(entry_at[1]+0x3450,level==2);w32(entry_at[1]+0x3454,level!=0);
    }
    /* GT Mode's Free Run (00117668): RaceFreeRun (003B9A18), the Time
       Trial's own classes (RaceFreeRun / DynamicsConductorFreeRun, the
       tables above), its mode 11. Practice (00118670): RaceFreePractice
       (003B92D0, 0067FC70) with DynamicsConductorFreePractice (003F4EB8,
       006856E0), mode 12; the two share the Arcade world's layout (base
       003D5CF8, the conductor at +0x125C0). */
    if (d->world_mode == 12) {
        w32(WORLD + 0x64, 0x0067fc70u);
        w32(COND + 0x10140, 0x006856e0u);
    }
    /* Power & Speed (00115A90 -> 003BDD60): RaceMachineTest, world
       00680EE0 with DynamicsConductorMachineTest (003F6958, 00685BF0),
       mode 15; the test is the race's laps word (session.c) */
    if (d->world_mode == 15) {
        w32(WORLD + 0x64, 0x00680ee0u);
        w32(COND + 0x10140, 0x00685bf0u);
    }
    if (d->world_mode) w32(WORLD + 0xd68, (uint32_t)d->world_mode);
    /* B-Spec slots 05..08: world +0xCC0..+0xCCC 1 (A-Spec ar_02/03, licences: 0) */
    for (uint32_t o = 0xcc0; o <= 0xccc; o += 4) w32(GT4_U32(RACE + 0x84) + o, d->bspec ? 1 : 0);
    /* world+0xE408: the pit menu (003DCA28's object, a pointer 0038B980
       only tests); the host plays it */
    w32(GT4_U32(RACE + 0x84) + 0xe408u, d->pit_menu ? 1 : 0);
    if (!load_course(r,r->vol, d->course, msg, len)) goto fail;
    fegetenv(&env);
    gt4_phys_fpu_enter();
    for (int i = 0; i < d->cars; i++) {
        if(d->owned_car[i]){
            /* 001140C0 +0xb4..e8 / 0011B160 +0x94..c8 select the
               owned setup (00441248) before copying it (004452F8).
               The 0x178-byte block is consumed at entry +0x20 by
               0034D490 (gt4_race_init), including equipped parts/settings. */
            uint8_t setup[0x178];
            if(!gt4_career_active_setup(d->owned_car[i],setup) ||
               le32(setup)!=d->car[i] || le32(setup+4)!=0){
                fesetenv(&env);
                snprintf(msg,len,"owned setup does not match car 0x%x",(unsigned)d->car[i]);
                goto fail;
            }
            memcpy(gt4_ptr(entry_at[i]+0x20),setup,sizeof setup);
            /* 0043E098 serializes live wear +0x4a0 to body +0x498;
               0034D490 passes entry +0x3468 to 00361908/gt4_car_init. */
            memcpy(gt4_ptr(entry_at[i]+0x3468),d->owned_car[i]+0x498,0x20);
            if(d->event_row && !i)w32(entry_at[i]+0x198,0x100); /* 003EDCB0 +0x3c */
            continue;
        }
        if(d->tuned_car[i]){
            /* 003BB5A8 -> 004474B0 -> 00444690 constructs the exact
               prepared opponent selected from the event's RACES list. */
            if(!gt4_garage_tuned(entry_at[i]+0x20,d->tuned_car[i],&r->db,GARAGE_SCRATCH) ||
               GT4_U32(entry_at[i]+0x20)!=d->car[i] || GT4_U32(entry_at[i]+0x24)!=0){
                fesetenv(&env);snprintf(msg,len,"cannot construct tuned entry %llx",(unsigned long long)d->tuned_car[i]);goto fail;
            }
            continue;
        }
        memset(gt4_ptr(GARAGE_SCRATCH), 0, 0x680);
        if (!gt4_garage_new(entry_at[i] + 0x20, d->car[i], &r->db, GARAGE_SCRATCH)) {
            fesetenv(&env);
            snprintf(msg, len, "no car 0x%x", (unsigned)d->car[i]);
            goto fail;
        }
        int tire=d->split_battle && i==1?d->tire_2p:d->tire;
        if (tire > 0) arcade_tyres(entry_at[i] + 0x20, d->car[i], tire - 1, &r->db);
    }
    if (d->quick_tuned && d->cars > 0) {
        /* 00111DC0 +0x54..0xB8: the QuickTune into the player's setup:
           power and weight over 100, the tyres by axle, the gearbox's top
           speed with its automatic ratios (00443240), +0x165 marking it */
        uint32_t setup = entry_at[0] + 0x20;
        GT4_U8(setup + 0x163) = (uint8_t)(d->quick_power + 100);
        GT4_U8(setup + 0x162) = (uint8_t)(d->quick_weight + 100);
        GT4_U8(setup + 0x160) = (uint8_t)d->quick_front_tire;
        GT4_U8(setup + 0x161) = (uint8_t)d->quick_rear_tire;
        if ((uint8_t)d->quick_gear) {
            GT4_U8(setup + 0x12a) = (uint8_t)d->quick_gear;
            uint64_t key = (uint64_t)GT4_U32(setup + 0x48) | (uint64_t)GT4_U32(setup + 0x4c) << 32;
            uint8_t row[256];
            int n = r->db.row(r->db.ctx, (uint32_t)(key >> 32), (uint32_t)key, 0, row, sizeof row);
            if (n > 0x25 && row[0x25]) {
                /* 00443240, as gt4_career_vehicle_auto_gears */
                memset(gt4_ptr(GARAGE_SCRATCH + 0x10000), 0, 0x2000);
                gt4_car_record_rows(&r->db, setup, GARAGE_SCRATCH + 0x10000);
                gt4_car_auto_ratios(GARAGE_SCRATCH + 0x10000, (int32_t)GT4_U8(setup + 0x12a) * 10,
                                    GARAGE_SCRATCH + 0x14000, 1, GARAGE_SCRATCH + 0x10600);
                memcpy(gt4_ptr(setup + 0x110), gt4_ptr(GARAGE_SCRATCH + 0x14000), 24);
            }
        }
        GT4_U8(setup + 0x165) = 1;
    }
    if(d->license>=2){   /* a mission's too: 0011BEC0 -> 003D6300, 003ECB68 */
        for(int i=0;i<d->cars;i++){
            /* 001151F0 +0x64..0x80 / 003D6300 +0x24..0x30;
               003ECB68 +0xa4..0xdc chooses each guide's RACES paint.
               -1 paint falls back to 0 for training, never random. */
            uint32_t setup=entry_at[i]+0x20;uint32_t rgb[32];
            int total=gt4_garage_paint_palette(setup,&r->db,0,rgb);
            int color=gt4_career_paint_list_index(d->event_paint[i],total,(int8_t)GT4_U8(setup+0x18));
            GT4_U8(setup+0x18)=(uint8_t)(color<0?0:color);
            GT4_U8(setup+0x166)=1;
        }
    }
    if (d->event_row && !d->license) {
        gt4_career_paint_car paints[6] = {0};
        int prepared = 0;
        for (int i = 0; i < d->cars; i++) prepared |= d->tuned_car[i] != 0;
        if (prepared && !d->event_rng) {
            fesetenv(&env);
            snprintf(msg, len, "prepared event grid requires its RNG state");
            goto fail;
        }
        if (d->event_rng) {
            for (int i = 0; i < d->cars; i++) {
                uint32_t setup = entry_at[i] + 0x20;
                int color = (int8_t)GT4_U8(setup + 0x18);
                if (d->tuned_car[i]) {
                    /* 004474B0 +0x40..70 -> 00445880: RACES indices
                       are one-based; rejected indices retain the setup. */
                    int total = gt4_garage_paint_palette(setup, &r->db, 0, paints[i].rgb);
                    color = gt4_career_paint_list_index(d->event_paint[i], total, color);
                }
                paints[i].color = color;
                paints[i].count = gt4_garage_paint_palette(setup, &r->db,
                                                          color == -1, paints[i].rgb);
            }
            /* 003BB5A8 -> 0040AA48 assigns pending paints after the
               complete grid exists, including the player's fixed color. */
            uint32_t seed = *d->event_rng;
            if (!gt4_career_select_paints(paints, (unsigned)d->cars, &seed)) {
                fesetenv(&env);
                snprintf(msg, len, "invalid event paint palette");
                goto fail;
            }
            for (int i = 0; i < d->cars; i++)
                GT4_U8(entry_at[i] + 0x38) = (uint8_t)paints[i].color;
            *d->event_rng = seed;
        }
    }
    if(d->event_row && !d->license){
        /* 003EE3C8 -> 00447550 +0x10..0x38: type comes from
           COURSE +0x10, resolved by settings +0xa0, not the RACE row. */
        uint8_t course_row[22]; /* decoded COURSE width, table ID is 0x22 */
        int type=r->db.row(r->db.ctx,0x22,GT4_U32(SETTINGS+0xa0),0,
                           course_row,sizeof course_row)==sizeof course_row?course_row[0x10]:-1;
        for(int i=0;i<d->cars;i++)
            gt4_garage_event_tyres(entry_at[i]+0x20,type,&r->db);
    }else if(!d->event_row && !d->license){
        /* 00387FB0 -> the Arcade world's +0x2E0 (00387348): course kind
           by the same COURSE row (00447550), then each car's tyres */
        uint8_t course_row[22];
        int type=r->db.row(r->db.ctx,0x22,GT4_U32(SETTINGS+0xa0),0,
                           course_row,sizeof course_row)==sizeof course_row?course_row[0x10]:-1;
        for(int i=0;i<d->cars;i++)
            gt4_garage_world_tyres(entry_at[i]+0x20,type,&r->db);
    }
    /* the sequence resets the records (003C0108, e.g. 00118EF0 +0x570)
       before the world starts the session: Practice's after-cars method
       (003F4F60) reads each entry's best lap there (+0x1198) */
    gt4_race_records_start(RACE);
    gt4_race_init(COND, GT4_U8(COND + 0xcbdcu), &r->db, INIT_SCRATCH);
    if(d->license){ /* 003F5FA8 after 0034D490 */
        GT4_U32(RACE+0x88)=0xfffffffeu;GT4_U32(RACE+0x8c)=0x157529ffu;
        GT4_U16(COND+0x1003e)=0;
    }
    for (int i = 0; i < d->cars; i++) {
        uint32_t car = COND + 0x6698u + (uint32_t)i * CAR_SIZE;
        float ground = 0;
        for (int k = 0; k < 4; k++) {
            ground += GT4_F32(gt4_car_wheel(car, k) + WHEEL_8C);
            r->travel0[i][k] = GT4_F32(gt4_car_wheel(car, k) + WHEEL_TRAVEL);
        }
        r->height[i] = GT4_F32(car + CAR_POSITION + 8) - ground * .25f;
    }
    /* 00387FB0 +0x418..0x47c: common world initialization consults
       conductor virtual +0x50, including GT and Time Trial worlds. */
    gt4_race_begin(RACE);
    gt4_race_select_start(RACE,
        gt4_session(COND, 0x50, 0, 0, 0, 0, 0, NULL));
    gt4_race_entries_begin(RACE, COND);
    gt4_course_objects_init(r->objects,COURSE);
    r->aspec_points = (int)gt4_race_aspec_points(RACE, 0, &r->db);
    fesetenv(&env);
    if (missing) {
        snprintf(msg, len, "original function 0x%08x needed", missing);
        goto fail;
    }
    return r;
fail:
    gt4_race_run_close(r);
    return NULL;
}

void gt4_race_run_close(gt4_race_run *r)
{
    if (!r) return;
    if (gt4_phys_specdb == &r->db) gt4_phys_specdb = NULL;
    if (gt4_phys_mem == r->ram) gt4_phys_mem = NULL;
    gt4_spec_tables_close(r->tables);
    gt4_vol_close(r->vol);
    gt4_course_gadgets_close(r->objects);
    free(r->tape);
    free(r->saved_tape);
    free(r->ram);
    free(r);
}

/* func_003F8540: the player's car as the ghost's recording keeps it at
   its lap's start (func_003F5820 through func_0035AB08: header +0x54) */
static void ghost_take(uint32_t cond, uint8_t out[GT4_GHOST_CAR])
{
    memcpy(out, gt4_ptr(cond + 0x6698u), GT4_GHOST_CAR);
}

/* func_003F58B0 with func_0035ABA8: the ghost (car 1) from a recording,
   its lap counter at `lap` + 1 and the step's fraction against the
   recording's; behind it, one step of the ghost alone (func_0034DB30 with
   1). A: the AI block the recording also keeps (0045CDC8 / 0045CF18,
   cond+0xDA78) is not restored. */
static void ghost_put(uint32_t cond, const uint8_t snap[GT4_GHOST_CAR], uint32_t rest, float frac_then,
                      uint32_t lap, float frac_now)
{
    uint32_t car = cond + 0x6698u + CAR_SIZE, b = car + 0x104, keep[5], v;
    uint8_t index = GT4_U8(b + 0x440), flags = GT4_U8(b + 0x441);
    float f;
    /* func_003F85F0: the copy over the car's first five words
       (func_003F84E0 / func_003F8510), then its model's links
       (func_00463930) */
    memcpy(keep, gt4_ptr(car), sizeof keep);
    memcpy(gt4_ptr(car), snap, GT4_GHOST_CAR);
    memcpy(gt4_ptr(car), keep, sizeof keep);
    GT4_U32(car + 0x14 + 0xe8) = car;
    GT4_U32(car + 0x14 + 0xec) = GT4_U32(car + 4) + 0xda50u;
    GT4_U32(car + 0x14 + 0xe4) = GT4_U32(car + 4);
    GT4_U8(b + 0x441) = flags;
    GT4_U8(b + 0x440) = index;
    /* func_003455F0 */
    v = GT4_U32(car + 4) + index * 48u + 0xcab8u;
    GT4_U32(b + 0x5fc) = 0;
    GT4_U32(b + 0x600) = 0;
    GT4_F32(v) = GT4_F32(b + 0x570);
    GT4_U32(v + 8) = 0;
    GT4_U32(v + 4) = 0;
    GT4_F32(v + 0xc) = GT4_F32(b + 0x568);
    for (uint32_t o = 0x10; o <= 0x2c; o += 4) GT4_U32(v + o) = 0;
    /* func_003584A0: the lap's start on the race clock (cond+0xF898) */
    GT4_U32(b + 0x614) = (GT4_U32(cond + 0xf898u) + 50 - rest) / 3;
    GT4_U16(b + 0x4ac) = (uint16_t)(lap + 1);
    f = frac_now - frac_then;
    if (!(f < 0)) { GT4_F32(car + 0x554) = f; return; }
    if (!GT4_U32(GT4_U32(cond) + 0xe0) && index == 1) gt4_step(cond, 1, STEP_BUF);
    GT4_F32(car + 0x554) = f + 1.0f;
}

/* func_003F59F8 / func_003F5B80, the player's start and lap events: a lap
   ended better than the ghost's becomes the ghost (00426CA0); with the
   ghost's car (cond+0xF8A0 2) the ghost starts its lap with the player
   (003F58B0); unless time runs slowed (cond+0xCBDC), the recording starts
   the next lap (003F5820). A: the recorder's own checks (00426C38,
   00426C78) are not read. */
static void ghost_event(void *ctx, uint32_t cond, uint32_t lap, uint32_t lap_time, uint32_t rest, float frac)
{
    gt4_race_run *r = ctx;
    if (lap && r->rec.ok) {
        r->rec.to = r->tape_n;
        if (lap_time < r->ghost_time) {
            r->ghost = r->rec;
            r->ghost_time = lap_time;
        }
    }
    if (!GT4_U32(GT4_U32(cond) + 0xe0) && GT4_U32(cond + 0xf8a0u) == 2 && r->ghost.ok) {
        r->ghost_at = r->ghost.from;
        r->ghost_on = 1;
        ghost_put(cond, r->ghost.car, r->ghost.rest, r->ghost.frac, lap, frac);
    }
    if (!GT4_U8(cond + 0xcbdcu)) {
        ghost_take(cond, r->rec.car);
        r->rec.rest = rest;
        r->rec.frac = frac;
        r->rec.from = r->tape_n;
        r->rec.ok = 1;
    }
}

/* The controllers during a Time Trial's step: the player's through the
   caller's, its commands kept; the ghost's (car 1) its lap's commands as
   the player gave them, then nothing (A: what it does past them is not
   read). */
static gt4_race_run *ghost_run;
static int ghost_read(void *u, uint32_t car, uint8_t *cmd, const uint8_t *ev)
{
    gt4_race_run *r = ghost_run;
    (void)u;
    if (car == r->cond + 0x6698u + CAR_SIZE) {
        if (r->ghost_on && r->ghost_at < r->ghost.to) memcpy(cmd, (r->ghost.saved ? r->saved_tape : r->tape) + r->ghost_at++ * 0x20, 0x20);
        else { memset(cmd, 0, 0x20); cmd[1] = 1; cmd[3] = ev[3]; r->ghost_on = 0; }
        return 1;
    }
    if (!r->outer.read || !r->outer.read(r->outer.ctx, car, cmd, ev)) return 0;
    if (car == r->cond + 0x6698u) {
        if (r->tape_n == r->tape_cap) {
            size_t cap = r->tape_cap ? r->tape_cap * 2 : 60 * 60 * 4;
            uint8_t *t = realloc(r->tape, cap * 0x20);
            if (!t) return 1;
            r->tape = t;
            r->tape_cap = cap;
        }
        memcpy(r->tape + r->tape_n++ * 0x20, cmd, 0x20);
    }
    return 1;
}
static int ghost_wheel(void *u, uint32_t car, int16_t state[2])
{
    gt4_race_run *r = ghost_run;
    (void)u;
    if (car == r->cond + 0x6698u + CAR_SIZE) return 1;   /* no wheel */
    return r->outer.wheel_state && r->outer.wheel_state(r->outer.ctx, car, state);
}
static int ghost_write(void *u, uint32_t car, const uint8_t *out)
{
    gt4_race_run *r = ghost_run;
    (void)u;
    if (car == r->cond + 0x6698u + CAR_SIZE) return 1;   /* no motors */
    return r->outer.write && r->outer.write(r->outer.ctx, car, out);
}

int gt4_race_run_ghost_on(const gt4_race_run *r) { return r && r->ghost_on; }

int gt4_race_run_ghost_get(const gt4_race_run *r, const uint8_t **car, uint32_t *rest, float *frac,
                           uint32_t *lap_time, const uint8_t **tape, size_t *steps)
{
    if (!r || !r->ghost.ok || r->ghost.to <= r->ghost.from || r->ghost_time == 0x157529ffu) return 0;
    *car = r->ghost.car;
    *rest = r->ghost.rest;
    *frac = r->ghost.frac;
    *lap_time = r->ghost_time;
    *tape = (r->ghost.saved ? r->saved_tape : r->tape) + r->ghost.from * 0x20;
    *steps = r->ghost.to - r->ghost.from;
    return 1;
}

int gt4_race_run_ghost_set(gt4_race_run *r, const uint8_t car[GT4_GHOST_CAR], uint32_t rest, float frac,
                           uint32_t lap_time, const uint8_t *tape, size_t steps)
{
    uint8_t *t;
    if (!r || !car || !tape || !steps || !(t = malloc(steps * 0x20))) return 0;
    memcpy(t, tape, steps * 0x20);
    free(r->saved_tape);
    r->saved_tape = t;
    memcpy(r->ghost.car, car, GT4_GHOST_CAR);
    r->ghost.rest = rest;
    r->ghost.frac = frac;
    r->ghost.from = 0;
    r->ghost.to = steps;
    r->ghost.ok = 1;
    r->ghost.saved = 1;
    r->ghost_time = lap_time;
    return 1;
}

int gt4_race_run_frame(gt4_race_run *r)
{
    fenv_t env;
    if (!r) return 0;
    fegetenv(&env);
    gt4_phys_fpu_enter();
    /* 00389990 +80..90 -> 003C04E8: transition tick before race step. */
    uint32_t session=GT4_U32(r->cond),dt_bits=0x3c888888;float dt;memcpy(&dt,&dt_bits,4);
    if(gt4_music_fade_step(&GT4_F32(session+0xd4),dt))
        gt4_race_queue_push(session+0xf4,1,0,0);
    gt4_race_queue_age(RACE + 0xf4);   /* func_00389758 -> 003B75D8: last frame's events age */
    gt4_race_before_step(RACE);
    if (r->cars == 2 && GT4_U32(r->cond + 0x10140) == TT_VT) {
        /* the Time Trial: the ghost's controller around the caller's */
        r->outer = gt4_phys_pad;
        ghost_run = r;
        gt4_phys_pad.read = ghost_read;
        gt4_phys_pad.wheel_state = ghost_wheel;
        gt4_phys_pad.write = ghost_write;
        gt4_tt_ghost = ghost_event;
        gt4_tt_ghost_ctx = r;
        gt4_step(r->cond, 0, STEP_BUF);
        gt4_tt_ghost = NULL;
        gt4_tt_ghost_ctx = NULL;
        gt4_phys_pad = r->outer;
    } else gt4_step(r->cond, 0, STEP_BUF);
    gt4_race_after_step(RACE);
    /* 0033C538: prop manager runs after 00389990 world simulation. */
    gt4_course_objects_step(r->objects,r->cond,dt);
    r->frames++;
    fesetenv(&env);
    return !missing;
}

/* The physics frame (x, y, z up) to the render frame (x, y up, z) */
static void to_render(const float v[3], float out[3]) { out[0] = v[0]; out[1] = v[2]; out[2] = -v[1]; }

/* func_0034C518: the course's line 6 at distance d, in the render frame */
int gt4_race_run_course_point(const gt4_race_run *r, float d, float out[3])
{
    float p[3];
    if (!r || !gt4_line_course_point(r->cond + 0xda50u, 6, d, p)) return 0;
    to_render(p, out);
    return 1;
}

/* the replay cameras' checks on the course (the render frame is the
   course's: x, height, z) */
int gt4_race_run_ground_id(const gt4_race_run *r, const float p[3])
{
    return r ? gt4_camera_ground_id(GT4_U32(r->cond + 0xcbd8u), p) : -1;
}

int gt4_race_run_camera_wall(const gt4_race_run *r, float margin, const float from[3], const float to[3], float out[3], int *id)
{
    int32_t g = *id;
    int hit;
    if (!r) { memcpy(out, to, 3 * sizeof *out); return 0; }
    hit = gt4_camera_wall(GT4_U32(r->cond + 0xcbd8u), margin, from, to, out, &g);
    *id = g;
    return hit;
}

float gt4_race_run_course_distance(const gt4_race_run *r, const float p[3])
{
    uint32_t id = 0xffffffffu;
    return r ? gt4_course_distance_p(GT4_U32(r->cond + 0xcbd8u), p, &id) : 0;
}

int gt4_race_run_pose(const gt4_race_run *r, int i, float pos[3], float rot[9], float wheels[4][3])
{
    uint32_t car = gt4_race_run_car(r, i);
    float p[3], a[3], up[3], fwd[3], u[3], c0[3], c2[3];
    if (!car) return 0;
    for (int k = 0; k < 3; k++) {
        p[k] = GT4_F32(car + CAR_POSITION + 4 * (uint32_t)k);
        a[k] = GT4_F32(car + CAR_MATRIX + 4 * (uint32_t)k);          /* forward */
        up[k] = GT4_F32(car + CAR_MATRIX + 0x18 + 4 * (uint32_t)k);  /* up */
    }
    (void)p;
    /* the model's origin: the car's reference point in the ground frame
       (car+0x894, already in the render frame: the position less
       params+0x50 along the car, chassis.c), as drawn between the last
       step's (+0x8B0) and this one's by the frame's part (+0x554); the
       cameras hang from the same point's other frame (+0x888) */
    {
        float t = 0 + GT4_F32(car + 0x554);
        for (int k = 0; k < 3; k++)
            pos[k] = GT4_F32(car + 0x8b0 + 4 * (uint32_t)k) * t + GT4_F32(car + 0x894 + 4 * (uint32_t)k) * (1 - t);
    }
    to_render(a, fwd);
    to_render(up, u);
    for (int k = 0; k < 3; k++) c2[k] = -fwd[k];
    c0[0] = u[1] * c2[2] - u[2] * c2[1];
    c0[1] = u[2] * c2[0] - u[0] * c2[2];
    c0[2] = u[0] * c2[1] - u[1] * c2[0];
    for (int k = 0; k < 3; k++) {
        rot[k * 3] = c0[k];
        rot[k * 3 + 1] = u[k];
        rot[k * 3 + 2] = c2[k];
    }
    if (wheels) {
        memset(wheels, 0, 4 * sizeof *wheels);
        for (int k = 0; k < 4; k++) {
            uint32_t w = gt4_car_wheel(car, k);
            float m[3], rr[3], local[3];
            int slot;
            for (int j = 0; j < 3; j++) m[j] = GT4_F32(w + WHEEL_MOUNT + 4 * (uint32_t)j);
            to_render(m, rr);
            for (int j = 0; j < 3; j++) local[j] = rot[j] * rr[0] + rot[3 + j] * rr[1] + rot[6 + j] * rr[2];
            slot = (local[2] > 0 ? 2 : 0) + (local[0] > 0 ? 0 : 1);
            wheels[slot][0] = GT4_F32(w + WHEEL_STEER);
            wheels[slot][1] = -GT4_F32(w + WHEEL_ROTATION);
            wheels[slot][2] = r->travel0[i][k] - GT4_F32(w + WHEEL_TRAVEL);
        }
    }
    return 1;
}

uint32_t gt4_race_run_conductor(const gt4_race_run *r) { return r ? r->cond : 0; }
int gt4_race_run_finished(const gt4_race_run *r) { return r && GT4_U8(gt4_race_run_car(r, 0) + 0x5b6) != 0; }
int gt4_race_run_license_result(const gt4_race_run *r,int32_t *state,uint32_t *time){
    if(!r || (GT4_U32(r->cond+0x10140)!=GT4_LICENSE_SESSION_VT && GT4_U32(r->cond+0x10140)!=GT4_TRAINING_SESSION_VT && GT4_U32(r->cond+0x10140)!=GT4_MISSION_SESSION_VT))return 0;
    uint32_t race=GT4_U32(r->cond);
    if(state)*state=(int32_t)GT4_U32(race+0x88);
    if(time)*time=GT4_U32(race+0x8c);return 1;
}
uint32_t gt4_race_run_car(const gt4_race_run *r, int i) { return r && i >= 0 && i < r->cars ? r->cond + 0x6698u + (uint32_t)i * CAR_SIZE : 0; }
int gt4_race_run_cars(const gt4_race_run *r) { return r ? r->cars : 0; }
float gt4_race_run_mass(const gt4_race_run *r, int i)
{
    uint32_t car = gt4_race_run_car(r, i);float m;
    if (!car) return 0;
    memcpy(&m, r->ram + car + CAR_MASS_A, 4);return m;
}

/* func_00343B50: the lap the car is on (the session asked first, 0x128,
   its answer unused): at least 1, one less while +0x4B2 holds */
static int display_lap(uint32_t car)
{
    int lap = (int16_t)GT4_U16(car + 0x104 + 0x4ac);
    if (lap <= 0) return 1;
    return GT4_U8(car + 0x104 + 0x4b2) ? lap - 1 : lap;
}

int gt4_race_run_countdown(const gt4_race_run *r, int i)
{
    if (!r || !r->cond || i < 0 || i > 5) return 0;
    return (int32_t)GT4_U32(GT4_U32(r->cond) + 0xbc) + (int32_t)GT4_U32(r->cond + 0xcbfcu + (uint32_t)i * 4u);
}

int gt4_race_run_hud(const gt4_race_run *r, int i, gt4_race_hud *out)
{
    uint32_t car = gt4_race_run_car(r, i);
    if (!car) return 0;
    out->rank = (int8_t)GT4_U8(car + 0x5b3);
    out->lap = display_lap(car);
    out->laps = (int32_t)gt4_session(r->cond, 0x128, 0, 0, 0, 0, 0, NULL);   /* as the display takes it (func_0039B2B8) */
    out->lap_time = GT4_U32(r->cond + 0xf898u) / 3u - GT4_U32(car + 0x718);   /* func_00357800 */
    out->best_lap = GT4_U32(car + 0x71c);
    out->last_lap = GT4_U32(entry_at[i] + 0x11dc);
    out->speed = GT4_F32(car + 0x6c8);
    out->rpm = (float)(int32_t)GT4_F32(car + 0x738);
    out->gear = GT4_U8(car + 0x624);
    out->throttle = GT4_F32(car + 0x58c);
    out->brake = GT4_F32(car + 0x590);
    out->side_g = -GT4_F32(car + 0x664) * 0.102f;
    {   /* func_003F3758: past the params' shift rpm (+0x262), or +0x625 */
        uint32_t params = GT4_U32(car + 0x10);
        out->shift_lamp = (int16_t)GT4_U16(car + 0x5fe) > (int)GT4_U16(params + 0x262) || GT4_U8(car + 0x625);
    }
    out->finished = GT4_U8(car + 0x5b6) != 0;
    out->line_time = GT4_U32(car + 0x718);
    out->signal = GT4_U8(car + 0x6fe);
    out->rolling = GT4_U8(r->cond + 0xcbe4u) != 0;
    out->start_frames = (int32_t)GT4_U32(GT4_U32(r->cond) + 0xbc);
    out->count_frames = out->start_frames + (int32_t)GT4_U32(r->cond + 0xcbfcu + i * 4u);
    out->start_seconds = (int32_t)GT4_U32(GT4_U32(r->cond) + 0xb8);
    out->grid_slot = (int32_t)GT4_U32(entry_at[i] + 0x3444);
    out->intro_elapsed = r->frames / 60.f;
    out->aspec_points = r->aspec_points;
    {   /* func_0036CDF8: the meters' scales */
        uint32_t params = GT4_U32(car + 0x10);
        int n = (int)(GT4_F32(params + 0xfd8) * 1.1f / 22.2222233f) + 1;
        out->speed_steps = n < 2 ? 2 : n > 4 ? 4 : n;
        out->tacho_scale = (float)(GT4_U8(params + 0x1075) * 1000);
    }
    out->odometer = GT4_U32(car + 0x6dc);
    out->fuel = GT4_F32(car + 0x5d4);
    out->fuel_cap = GT4_F32(car + 0x5d0);
    out->rev_limit = (float)(GT4_U8(GT4_U32(car + 0x10) + 0x1074) * 100);
    out->aid = GT4_U8(car + 0x6fc) > GT4_U8(car + 0x6fd) ? GT4_U8(car + 0x6fc) : GT4_U8(car + 0x6fd);
    out->brake_lock = GT4_F32(car + 0x594) > 0.25f;
    {   /* func_003451D8: not held (+0x441), and a player's kind (0, 1) or 4 */
        int8_t kind = (int8_t)GT4_U8(car + 0x104 + 0x466);
        out->driving = !GT4_U8(car + 0x104 + 0x441) && (kind == 0 || kind == 4 || kind == 1);
    }
    return 1;
}

/* func_00344798 and the like: a car value drawn between the last step's
   (old) and this one's (new) by the frame's part (+0x554) */
static float drawn(uint32_t car, uint32_t now, uint32_t old)
{
    float t = 0 + GT4_F32(car + 0x554);
    return GT4_F32(car + now) * t + GT4_F32(car + old) * (1 - t);
}

/* The matrix stack's turns as the game makes them (func_004A79D8 about
   z, func_004A7988 about x, func_004A79B0 about y, in that order):
   Rz(z) * Rx(x) * Ry(y), radians */
static void turns(float z, float x, float y, float R[9])
{
    float sa = sinf(y), ca = cosf(y), sb = sinf(x), cb = cosf(x), sc = sinf(z), cc = cosf(z), A[9];
    A[0] = ca;       A[1] = 0;   A[2] = sa;
    A[3] = sb * sa;  A[4] = cb;  A[5] = -sb * ca;
    A[6] = -cb * sa; A[7] = sb;  A[8] = cb * ca;
    for (int k = 0; k < 3; k++) {
        R[k] = cc * A[k] - sc * A[3 + k];
        R[3 + k] = sc * A[k] + cc * A[3 + k];
        R[6 + k] = A[6 + k];
    }
}

int gt4_race_run_camera(const gt4_race_run *r, int i, int type, const float point[8], float eye[3], float rows[9])
{
    uint32_t car = gt4_race_run_car(r, i);
    float p[3], yaw, pitch, roll = 0, R[9];
    if (!car) return 0;
    p[0] = drawn(car, 0x8a4, 0x888);                                   /* func_00344798 */
    p[1] = drawn(car, 0x8a8, 0x88c);                                   /* func_003447F8 */
    p[2] = drawn(car, 0x8ac, 0x890);                                   /* func_00344828 */
    yaw = drawn(car, 0x8c4, 0x674);                                    /* func_003448B8 */
    pitch = drawn(car, 0x8bc, 0x66c);                                  /* func_00344858 */
    if (type == 0) {
        /* on the car (func_0037B020): its roll (func_00344888) and the
           view's bob, the player's view block +0x18 (func_0034A760) */
        roll = drawn(car, 0x8c0, 0x670);
        pitch += GT4_F32(GT4_U32(car + 4) + 0xcab8u + GT4_U8(car + 0x544) * 0x30u + 0x18);
    } else if (type == 1) {
        /* behind it (func_0037B020): the view's lag (func_00345138,
           func_00345168), no roll */
        yaw -= drawn(car, 0x994, 0x700);
        pitch -= drawn(car, 0x998, 0x704);
    } else
        roll = drawn(car, 0x8c0, 0x670);                               /* func_0037B580: the car's turns as they are */
    /* the matrix stack: the point, then Rz(roll), Rx(-pitch), Ry(-yaw),
       then the car's place: view = Rz * Rx * Ry * (world - p) + point */
    turns(roll, -pitch, -yaw, R);
    for (int k = 0; k < 3; k++)
        eye[k] = p[k] - (R[k] * point[0] + R[3 + k] * point[1] + R[6 + k] * point[2]);
    if (type != 0 && type != 1) {
        /* func_0037B580: first the point's own turns (+0x14 about z, +0xC
           about x, +0x10 about y, degrees) */
        const float d = 3.14159265f / 180;
        float E[9], M[9];
        turns(point[5] * d, point[3] * d, point[4] * d, E);
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++)
                M[r * 3 + c] = E[r * 3] * R[c] + E[r * 3 + 1] * R[3 + c] + E[r * 3 + 2] * R[6 + c];
        memcpy(R, M, sizeof R);
    }
    memcpy(rows, R, sizeof R);
    return 1;
}

int gt4_race_run_place(const gt4_race_run *r, int i, float p[3], float *yaw)
{
    uint32_t car = gt4_race_run_car(r, i);
    if (!car) return 0;
    p[0] = drawn(car, 0x8a4, 0x888);
    p[1] = drawn(car, 0x8a8, 0x88c);
    p[2] = drawn(car, 0x8ac, 0x890);
    *yaw = drawn(car, 0x8c4, 0x674);
    return 1;
}

int gt4_race_run_shadow(const gt4_race_run *r, int i, float pos[3], float rot[9], float *sun, float *level)
{
    uint32_t car = gt4_race_run_car(r, i);
    float R[9], a = 0, b = 0;
    if (!car) return 0;
    pos[0] = drawn(car, 0x8b0, 0x894);                                 /* func_003448E8 */
    pos[1] = drawn(car, 0x8b4, 0x898);                                 /* func_00344918 */
    pos[2] = drawn(car, 0x8b8, 0x89c);                                 /* func_00344948 */
    /* func_00451D18: func_004A79B0 (y) with the heading (func_003449D8),
       func_004A7988 (x) with the ground's pitch (func_00344978), then
       func_004A79D8 (z) with its roll negated (func_003449A8): the
       inverse of the cameras' Rz(roll) * Rx(-pitch) * Ry(-yaw) */
    turns(drawn(car, 0x8cc, 0x67c), -drawn(car, 0x8c8, 0x678), -drawn(car, 0x8c4, 0x674), R);
    for (int k = 0; k < 3; k++)
        for (int c = 0; c < 3; c++) rot[k * 3 + c] = R[c * 3 + k];
    /* func_003D7460: func_00344EC8 (+0x7CC, the last step's +0x930) and
       func_00344F40 (+0x7D0, +0x940) of the four wheels, a quarter each */
    for (uint32_t w = 0; w < 4; w++) {
        a += drawn(car, 0x930 + 4 * w, 0x7cc + 0x34 * w);
        b += drawn(car, 0x940 + 4 * w, 0x7d0 + 0x34 * w);
    }
    *sun = a * 0.25f;
    *level = b * 0.25f;
    return 1;
}

int gt4_race_run_xy(const gt4_race_run *r, int i, float *x, float *y)
{
    uint32_t car = gt4_race_run_car(r, i);
    if (!car) return 0;
    {   /* func_00344798, func_00344828: the drawn position (+0x8A4, the
           render frame's x and z) blended from the last step's (+0x888)
           by the frame's part (+0x554) */
        float t = GT4_F32(car + 0x554);
        *x = GT4_F32(car + 0x8a4) * t + GT4_F32(car + 0x888) * (1 - t);
        *y = GT4_F32(car + 0x8ac) * t + GT4_F32(car + 0x890) * (1 - t);
    }
    return gt4_car_presence(car);
}

/* the car's setup as the race took it: its entry +0x20 (0x178 bytes) */
int gt4_race_run_setup(const gt4_race_run *r, int i, uint8_t out[0x178])
{
    if (!r || !out || i < 0 || i >= r->cars || i >= 6) return 0;
    memcpy(out, r->ram + entry_at[i] + 0x20, 0x178);
    return 1;
}

/* func_00445AE8 -> 00445B28 / 004458D0: the car's paint chip, its
   VARIATION row (the setup's +0x10 key) of order +0x18 + 1, its four
   colours at +0x24; none for paint -1 or past the rows */
int gt4_race_run_paint_chip(const gt4_race_run *r, int i, uint32_t chip[4])
{
    uint8_t setup[0x178], row[52];
    uint64_t key;
    int color, count = 0;
    if (!chip) return 0;
    memset(chip, 0, 16);
    if (!gt4_race_run_setup(r, i, setup)) return 0;
    memcpy(&key, setup + 0x10, 8);
    color = (int8_t)setup[0x18];
    if (color == -1) return 0;
    while (count < 64 && r->db.row(r->db.ctx, (uint32_t)(key >> 32), (uint32_t)key, count, row, sizeof row) > 0) count++;
    if (color >= count) return 0;
    for (int n = 0; n < count; n++) {
        uint32_t order;
        if (r->db.row(r->db.ctx, (uint32_t)(key >> 32), (uint32_t)key, n, row, sizeof row) != 52) return 0;
        memcpy(&order, row + 4, 4);
        if (order == (uint32_t)color + 1) { memcpy(chip, row + 0x24, 16); return 1; }
    }
    return 0;
}

int gt4_race_run_gear_range(const gt4_race_run *r, int i, int *gear, int *lo, int *hi)
{
    uint8_t setup[0x178], row[256];
    if (!gt4_race_run_setup(r, i, setup) || !gear || !lo || !hi) return 0;
    uint64_t key;
    memcpy(&key, setup + 0x48, 8);
    int n = r->db.row(r->db.ctx, (uint32_t)(key >> 32), (uint32_t)key, 0, row, sizeof row);
    *gear = *lo = *hi = 0;
    if (n > 0x27 && row[0x25]) { *gear = setup[0x12a]; *lo = row[0x26]; *hi = row[0x27]; }
    return 1;
}

/* ------------------------------------------------ the cars' ratings */

#define RATE_REC  0x01d00000u   /* the record, the parameters at +0x600 (0x1800) */
#define RATE_GAR  0x01d02000u   /* the garage entry (0x680) */
#define RATE_WEAR 0x01d02800u   /* no wear (func_003433E8) */
#define RATE_BUF  0x01d03000u
#define RATE_RACE 0x006203c8u   /* D_006203C8: the settings func_00364868 passes */

/* per car: its rating, and what the quick menu's grid shows: the engine
   summary's peak power (PS, truncated) and the weight (kg, the parameters'
   mass less the driver's 70) */
typedef struct { uint64_t key; float rating; int power, weight; } car_rated;

struct gt4_car_ratings {
    char *disc;
    int n, cap;
    car_rated *car;
};

gt4_car_ratings *gt4_car_ratings_open(const char *disc)
{
    gt4_car_ratings *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->disc = malloc(strlen(disc) + 1);
    if (!r->disc) {
        free(r);
        return NULL;
    }
    strcpy(r->disc, disc);
    return r;
}

void gt4_car_ratings_close(gt4_car_ratings *r)
{
    if (!r) return;
    free(r->disc);
    free(r->car);
    free(r);
}

static const car_rated *rating_cached(const gt4_car_ratings *r, uint64_t key)
{
    for (int i = 0; i < r->n; i++)
        if (r->car[i].key == key) return &r->car[i];
    return NULL;
}

static void rating_keep(gt4_car_ratings *r, const car_rated *c)
{
    if (r->n == r->cap) {
        int cap = r->cap ? r->cap * 2 : 256;
        car_rated *k = realloc(r->car, (size_t)cap * sizeof *k);
        if (!k) return;
        r->car = k;
        r->cap = cap;
    }
    r->car[r->n++] = *c;
}

/* func_001305A0 / func_00130540 with func_00364868 */
static void rate_car(car_rated *c, const gt4_specdb_src *db)
{
    uint64_t key = c->key;
    c->rating = 0;
    c->power = c->weight = 0;
    memset(gt4_ptr(RATE_GAR), 0, 0x680);
    memset(gt4_ptr(GARAGE_SCRATCH), 0, 0x680);
    if (!gt4_garage_new(RATE_GAR, key, db, GARAGE_SCRATCH)) return;   /* func_004443A8 failed */
    memset(gt4_ptr(RATE_WEAR), 0, 0x20);
    GT4_U8(RATE_WEAR + 0x14) = 1;
    gt4_surface_bumps(0);
    memset(gt4_ptr(RATE_REC), 0, 0x1800);
    gt4_car_record(RATE_REC, RATE_GAR, 0, db);
    gt4_orig_t0 = RATE_RACE;
    gt4_orig_t1 = RATE_WEAR;
    gt4_car_params(RATE_REC + 0x600, RATE_REC, 0, 0, RATE_RACE, RATE_WEAR, RATE_BUF);
    c->rating = GT4_F32(RATE_REC + 0x600 + 0xfe0);                        /* func_00364F00 */
    c->power = (int)gt4_engine_peak_power(RATE_REC + 0x600, RATE_WEAR);
    c->weight = (int)GT4_F32(RATE_REC + 0x600 + PARAMS_MASS) - 70;
}

/* the ratings' own memory: the executable's data from the disc, the
   physics pointed at it until left */
typedef struct {
    uint8_t *ram, *old_mem;
    float (*old_call)(uint32_t, uint32_t, uint32_t, float, float);
    int old_headless;
    uint32_t old_scratch, old_t0, old_t1;
    const gt4_specdb_src *old_db;
    gt4_vol *vol;
    gt4_spec_tables *tables;
    gt4_specdb_src db;
    fenv_t env;
} rate_env;

static int rate_enter(const gt4_car_ratings *r, rate_env *e)
{
    char msg[160];
    const char *err = NULL;
    memset(e, 0, sizeof *e);
    e->old_mem = gt4_phys_mem;
    e->old_call = gt4_orig_call;
    e->old_headless = gt4_phys_headless;
    e->old_scratch = gt4_phys_scratch;
    e->old_db = gt4_phys_specdb;
    e->old_t0 = gt4_orig_t0;
    e->old_t1 = gt4_orig_t1;
    e->ram = calloc(1, RAM_SIZE);
    if (!e->ram) return 0;
    gt4_phys_mem = e->ram;
    gt4_orig_call = no_original;
    gt4_phys_headless = 1;
    gt4_phys_scratch = PHYS_SCRATCH;
    e->vol = load_image(e->ram, r->disc, msg, sizeof msg) ? gt4_vol_open(r->disc, &err) : NULL;
    if (!e->vol) {
        fprintf(stderr, "car ratings: %s\n", err ? err : msg);
        return 0;
    }
    e->tables = gt4_spec_tables_open(e->vol, NULL);
    e->db.row = gt4_spec_tables_row;
    e->db.ctx = e->tables;
    e->db.str = gt4_spec_tables_str;
    e->db.car_parts = gt4_spec_tables_car_parts;
    gt4_phys_specdb = &e->db;
    fegetenv(&e->env);
    gt4_phys_fpu_enter();
    return 1;
}

static void rate_leave(rate_env *e)
{
    if (!e->ram) return;
    if (e->vol) fesetenv(&e->env);
    gt4_spec_tables_close(e->tables);
    gt4_vol_close(e->vol);
    free(e->ram);
    e->ram = NULL;
    gt4_phys_mem = e->old_mem;
    gt4_orig_call = e->old_call;
    gt4_phys_headless = e->old_headless;
    gt4_phys_scratch = e->old_scratch;
    gt4_phys_specdb = e->old_db;
    gt4_orig_t0 = e->old_t0;
    gt4_orig_t1 = e->old_t1;
    if (missing) {
        fprintf(stderr, "car ratings: original function 0x%08x needed\n", missing);
        missing = 0;
    }
}

static int rate_cars(gt4_car_ratings *r, const uint64_t *keys, int n, float *out, int *power, int *weight)
{
    rate_env e;
    int entered = 0, ok = 1;
    if (!r || n < 0) return 0;
    for (int i = 0; i < n; i++) {
        const car_rated *had = rating_cached(r, keys[i]);
        car_rated c;
        if (had) {
            if (out) out[i] = had->rating;
            if (power) power[i] = had->power;
            if (weight) weight[i] = had->weight;
            continue;
        }
        if (!entered) {
            entered = 1;
            if (!rate_enter(r, &e)) {
                ok = 0;
                break;
            }
        }
        c.key = keys[i];
        rate_car(&c, &e.db);
        rating_keep(r, &c);
        if (out) out[i] = c.rating;
        if (power) power[i] = c.power;
        if (weight) weight[i] = c.weight;
    }
    if (entered) rate_leave(&e);
    return ok;
}

/* func_00364868 then func_00364E78 (the replay's opening, func_0039C838):
   the record from the setup with the career's settings (D_0069F780), the
   parameters with no wear (func_00343490), the engine's peak power plus
   func_00356EA8's (drive 7: func_00356E08; drive+0x13 4: 13.597);
   GENERIC +0x12 bit 3 hides it (func_004489A0). The tyres' types:
   func_004462E0 / func_00446320, the front's row +0x3A, the rear's +0x38. */
int gt4_car_ratings_setup(gt4_car_ratings *r, const uint8_t setup[0x178], float *power, int *front, int *rear)
{
    rate_env e;
    uint8_t generic[24], row[256], settings[0x180];
    uint64_t key;
    if (!r || !setup) return 0;
    if (!rate_enter(r, &e)) {
        rate_leave(&e);
        return 0;
    }
    memset(gt4_ptr(RATE_GAR), 0, 0x680);
    memcpy(gt4_ptr(RATE_GAR), setup, 0x178);
    memset(gt4_ptr(RATE_WEAR), 0, 0x20);
    GT4_U8(RATE_WEAR + 0x14) = 1;
    gt4_surface_bumps(0);
    memset(gt4_ptr(RATE_REC), 0, 0x1800);
    /* func_003F3B68 takes the career's settings (D_0069F780) where the
       port's record reads them (D_00620CD8) */
    memcpy(settings, gt4_ptr(0x620cd8), sizeof settings);
    memcpy(gt4_ptr(0x620cd8), gt4_ptr(0x69f780), sizeof settings);
    gt4_car_record(RATE_REC, RATE_GAR, 0, &e.db);
    memcpy(gt4_ptr(0x620cd8), settings, sizeof settings);
    gt4_orig_t0 = RATE_RACE;
    gt4_orig_t1 = RATE_WEAR;
    gt4_car_params(RATE_REC + 0x600, RATE_REC, 0, 0, RATE_RACE, RATE_WEAR, RATE_BUF);
    if (power) {
        uint32_t drive = RATE_REC + 0x834;
        float p = gt4_engine_peak_power(RATE_REC + 0x600, RATE_WEAR), extra = 0;
        if (GT4_U8(drive) == 7) {
            float a = (float)GT4_U8(0x62045a), b = (float)GT4_U8(0x62045b);
            extra = ((a + a) + b) / gt4_f32(0x3f3c49ba);
        } else if (GT4_U8(drive + 0x13) == 4)
            extra = gt4_f32(0x41598a02);
        *power = p + extra;
        memset(generic, 0, sizeof generic);
        e.db.row(e.db.ctx, 0, le32(setup), 0, generic, sizeof generic);
        if (generic[0x12] & 8) *power = -1;
    }
    for (int k = 0; k < 2; k++) {
        int *out = k ? rear : front;
        if (!out) continue;
        memcpy(&key, setup + (k ? 0x68 : 0x60), 8);
        *out = -1;
        memset(row, 0, sizeof row);
        if (key != UINT64_MAX && e.db.row(e.db.ctx, (uint32_t)(key >> 32), (uint32_t)key, 0, row, sizeof row) > (k ? 0x38 : 0x3a))
            *out = row[k ? 0x38 : 0x3a];
    }
    rate_leave(&e);
    return 1;
}

int gt4_car_ratings_get(gt4_car_ratings *r, const uint64_t *keys, int n, float *out)
{
    return rate_cars(r, keys, n, out, NULL, NULL);
}

int gt4_car_ratings_spec(gt4_car_ratings *r, uint64_t key, int *power, int *weight)
{
    return rate_cars(r, &key, 1, NULL, power, weight);
}

/* Tests: the whole guest memory from a PCSX2 capture (eeMemory.bin) in
   place of the race's, so a frozen race shows the captured moment. */
int gt4_race_run_restore(gt4_race_run *r, const char *path)
{
    FILE *f;
    size_t n;
    if (!r || !path || !(f = fopen(path, "rb"))) return 0;
    n = fread(r->ram, 1, RAM_SIZE, f);
    fclose(f);
    if (n != RAM_SIZE) return 0;
    /* the conductor where the capture has it: its session table at +0x10140 */
    for (uint32_t a = 0x00100000u; a + 0x10144u <= RAM_SIZE; a += 16)
        if (GT4_U32(a + 0x10140u) == RACE_VT || GT4_U32(a + 0x10140u) == 0x00685890u) {   /* Arcade, Time Trial */
            r->cond = a;
            break;
        }
    return 1;
}

const gt4_course_gadgets *gt4_race_run_objects(const gt4_race_run *r){return r?r->objects:NULL;}
