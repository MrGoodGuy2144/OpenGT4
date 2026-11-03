#include "gtmode_game.h"
#include "menu.h"

#include "adhoc/adhoc_vm.h"
#include "platform/platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include "sound.h"
#include "save.h"
#include "career_blob.h"
#include "career_parts.h"
#include "specdb.h"
#include "arcade.h"
#include "career_admission.h"
#include "replay_file.h"
#include "disc_replay.h"
#include "ps2_media.h"
static int to_int(ah_val v);
static char film_loaded[96];
#include "pad_config.h"
#include "album_slide.h"
#include "tex1.h"
#include "gpb.h"
#include "pcm_music.h"
#include "ranking_code.h"
static char memcard_origin[64];   /* where the memory card screen goes back to */
static int replay_action;          /* MMemoryCardManager::setAction: 1 plays the replay loaded */
static char replay_loaded[64];     /* the replay a memory card screen loaded (below) */
static int replay_theater;         /* labomode opened at ReplayTheaterRoot: a replay played there comes back to it */
/* the playlist played (commandPlayList: menu_result "list"), one replay
   after the other, in order or shuffled (option replay_play_mode) */
static char replay_queue[128][64];
static int replay_queue_n,replay_queue_at;
#ifdef _WIN32
#include <windows.h>
#endif

typedef struct menu_actor menu_actor;

typedef struct {
    ah_obj *obj;
    int     count;
} watcher;

struct menu {
    int (*profile_picker)(menu *);
    int (*photo_saver)(void *);void *photo_saver_user;
    gt4_vol    *vol;
    char project_label[32];
    gt4_disc_replay *disc_replays;
    ui_project *ui;
    ah_vm      *vm;
    ah_obj     *project;       /* e.g. ArcadeProject */
    ah_obj     *context;
    char       *entered;       /* per root: onInitialize ran */
    char       *loaded;        /* per root: onLoad ran (once per project load) */
    watcher    *watchers;
    int         nwatchers;
    int         pages[32], depth;
    ui_widget **focus;
    int         pending_page, pending_kind;
    int         page_transition_depth;
    int         pending_more[8], pending_mores;   /* startPage's further pages, pushed in order */
    ah_obj     *events[128];
    int         nevents;
    char        next_project[64];
    char        labo_arg[32];   /* the labomode page or card mode this project asked for */
    /* lists whose items the script fills (labomode.ad's visible_callback /
       update_callback): the ones to show again, and items asking for more
       updates (setItemUpdate, e.g. a thumbnail still loading) */
    ui_widget  *item_lists[8];
    int         nitem_lists;
    struct { ui_widget *list; int index; } item_updates[64];
    int         nitem_updates;
    unsigned    pad_held;       /* the pad buttons held (getPadButtonState) */
    unsigned    idle_frames;
    double tick_remainder, animation_ticks;
    uint32_t loading_seed;
    menu_actor *actors;
    gt4_sound *sound;
    char pcm_group[64];int pcm_requested,pcm_volume,pcm_sequence,pcm_repeat,pcm_playing;unsigned pcm_serial;
    int pcm_paused;                       /* sound.pauseStream */
    uint8_t *course_db,*course_labels;size_t course_db_size,course_labels_size;
    int preview_courses, course_load_attempted;
    uint8_t *car_labels,*car_db,*car_names,*car_strings;
    size_t car_labels_size,car_db_size,car_names_size,car_strings_size;int car_load_attempted;
    uint8_t *car_links,*car_variants;size_t car_links_size,car_variants_size;
    uint8_t *race_labels,*race_db;size_t race_labels_size,race_db_size;int race_load_attempted;
    int car_color;                        /* CarFace.setColorIndex, -1 before any */
    int gtmode_save_entry,new_career_entry,quit_requested;
    char music_name[64];int music_set;void (*music_fader)(float);
    void (*audio_preparer)(menu *,void *);void *audio_preparer_user;
    int options_dirty;
    int race_requested;                   /* finish() with a race sequence */
    gt4_arcade *arcade;                   /* the Arcade's SpecDB answers, opened on use */
    int (*rate_cars)(void *,const uint64_t *,int,float *);void *rate_ctx;
    char *entry_car[6];int entry_color[6];/* game.setEntryCarCode */
    uint64_t quick_result_generation;
    uint64_t quick_tune_generation;       /* the race its QuickTune was prepared for */
    ah_obj *quick_work;                   /* MQuickWork: this VM's view of the quick race */
    char quick_command[32];               /* its selectedCommand at finish() */
    void (*present_wait)(menu *, float, void *);
    void *present_user;
    int (*present_dialog)(menu *,const char *,const char *,int,const char *,const char *);
    void (*present_progress)(menu *,int,const char *,int,float);
    int (*present_event)(menu *,void *);
    void *event_user;
    int (*load_car)(menu *,const ui_widget *,void *);void *car_loader_user;
    unsigned char event_loops[32];
    int event_depth;
    int render_context_count;
    int initializing;
    float pending_fade_in;
    gt4_photo_mode photo;
    float photo_apertures[17];int photo_apertures_ready;
    gt4_album_slide album_layer[2];
    int album_playing,album_shuffle,album_front,album_at,album_count,album_order[GT4_ALBUM_CAPACITY];
    uint32_t album_random;
    int album_preview_w,album_preview_h;
    int album_music_ready;gt4_music_playlist album_music_snapshot;

};

static int demo_movie_count; /* manager +6C, remaining until movie */
static int attract_index,attract_left,attract_cancelled;
static char attract_pending[64],attract_origin[64];
static void attract_next(menu *m);
int menu_attract_active(void){return attract_left>0;}
int menu_take_attract_replay(char *name,size_t size)
{if(!attract_pending[0])return 0;snprintf(name,size,"%s",attract_pending);attract_pending[0]=0;return 1;}
void menu_attract_finish(menu *m,int cancelled)
{
    if(!m)return;
    if(cancelled || attract_cancelled || attract_left<=1){
        attract_left=0;attract_cancelled=0;attract_pending[0]=0;m->idle_frames=0;
        menu_request_project(m,attract_origin);
    }else {attract_left--;attract_next(m);}
}
static void attract_next(menu *m)
{
    /* 00434770 +18..90/+144..168: movie countdown, independent tape index.
       0010BE20 +230..238 loops until the fourth presentation completes. */
    int interval=(int)menu_option_number(m,"demo_movie_interval",3);
    /* arcade.ad _start_page L52..55 already completed panOut/syncOut.
       Preserve that black surface until the movie/replay takes over. */
    m->idle_frames=0;
    if(demo_movie_count<=0 && interval>0)menu_request_project(m,"boot:MovieRoot");
    else {
        char files[32][64];int n=gt4_disc_replay_list(m->vol,files,32,0);
        if(n<=0){menu_attract_finish(m,1);return;}
        if(attract_index>=n)attract_index=0;
        snprintf(attract_pending,sizeof attract_pending,"%s",files[attract_index]);
        attract_index=(attract_index+1)%n;
    }
    if(--demo_movie_count<0)demo_movie_count=interval-1;
}

static void log_line(void *user, const char *msg)
{
    (void)user;
    fprintf(stderr, "script: %s\n", msg);
}

/* ---------------------------------------------------------------- widgets */

static const ah_host widget_host;
static int nat_album_widget(ah_vm *,ah_obj *,void *,int,const ah_val *,ah_val *);
static void album_tick(menu *m);
static void jukebox_store(menu *m);
static void album_music_bind(menu *m);
static void album_music_store(menu *m);
static int album_draw(void *,const ui_widget *,float,float,float,float,float);

static ah_obj *widget_module(menu *m, ui_widget *w);
static void set_focus(menu *m, ui_widget *w);
static int available(ui_widget *w);
static void set_tooltip(menu *m, int root, const ui_widget *w);
static void apply_pages(menu *m);
static int logger_can(void);
static void logger_tick(menu *m);
static int logger_key(menu *m,ui_widget *w,int keysym,int release);
static int logger_draw(void *user,const ui_widget *w,float x,float y,float wd,float h,float opacity);
static void finalize_page(menu *m, int root);
static const char *car_name(menu *m,const char *code);
static int nat_car_name(ah_vm *,ah_obj *,void *,int,const ah_val *,ah_val *);
static int nat_car_paints(ah_vm *vm,ah_obj *self,void *d,int argc,const ah_val *argv,ah_val *ret);
static int nat_chip_set_data(ah_vm *vm,ah_obj *self,void *d,int argc,const ah_val *argv,ah_val *ret);
static int nat_set_color_index(ah_vm *vm,ah_obj *self,void *d,int argc,const ah_val *argv,ah_val *ret);
static void update_progress(menu *m,ui_widget *w);
static void bind_garage_values(ah_vm *vm,ah_obj *o);
static const char *car_label_of(ah_vm *vm,ah_val v);

/* These commands still require native B-spec or record implementations
   (Replay itself plays back natively: race_game_replay).
   Visibility itself belongs to the original quick menu script. */
static int quick_pending_icon(menu *m,const ui_widget *w)
{
    const char *name=mp_string(ui_project_tree(m->ui),"name");
    return name && (!strcmp(name,"QuickArcadeProject") || !strcmp(name,"QuickTimeTrialProject")) && w && w->name &&
        w->parent && w->parent->name && !strcmp(w->parent->name,"Buttons") &&
        !strcmp(w->name,"Spectator");
}

static ah_val maybe_module(menu *m, ui_widget *w) { return w ? ah_objv(widget_module(m, w)) : ah_nil(); }
static int nat_prelight(ah_vm *vm, ah_obj *self, void *d, int argc, const ah_val *argv, ah_val *ret);

/* P: the LAN and network modes are not ported (no LAN play on the PC
   yet): the Arcade's Multi-LAN Race (GameRoot LANBattle) and the
   Options' LAN Settings and Network pages (option.ad OptionPage titles
   "LAN", "Network") answer with the menu's refusal sound instead. */
static int lan_blocked(menu *m, ui_widget *w)
{
    const char *project = mp_string(ui_project_tree(m->ui), "name");
    if (!w || !project) return 0;
    if (!strcmp(project, "ArcadeProject")) return w->name && !strcmp(w->name, "LANBattle");
    if (!strcmp(project, "OptionProject")) {
        ah_val page = ah_nil(), title = ah_nil();
        if (!ah_get_member(m->vm, widget_module(m, w), "page", &page) || page.t != AV_OBJ) return 0;
        if (!ah_get_member(m->vm, page.u.o, "title", &title) || !ah_cstr(title)) return 0;
        return !strcmp(ah_cstr(title), "LAN") || !strcmp(ah_cstr(title), "Network");
    }
    return 0;
}

static int sibling_index(const ui_widget *w)
{
    for (int i = 0; w->parent && i < w->parent->nkids; i++)
        if (w->parent->kids[i] == w) return i;
    return -1;
}

/* item = widget.doCopy(): a deep copy with the original's script members. */
static void copy_script(menu *m, ui_widget *src, ui_widget *dst)
{
    ah_obj *so = src->script, *dobj = widget_module(m, dst);
    if (so) {
        /* child widget modules of the original must not be shared */
        ah_obj *tmp = ah_module_new(m->vm, "", NULL, NULL, NULL);
        ah_copy_members(m->vm, tmp, so);
        for (int i = 0; i < src->nkids; i++) {
            ah_obj *child = src->kids[i]->script;
            if (child && src->kids[i]->name) {
                ah_val v;
                if (ah_get_member(m->vm, tmp, src->kids[i]->name, &v) && v.t == AV_OBJ && v.u.o == child)
                    ah_set_member(m->vm, tmp, src->kids[i]->name, ah_objv(widget_module(m, dst->kids[i])));
            }
        }
        /* a static still unset stays the module's, shared by its copies: set
           later (labomode.ad's Template::setListActivate, after the page
           copied Template::Normal::Scroll), the copy's onActivate sees it */
        for (int i = ah_object_count(tmp) - 1; i >= 0; i--)
            if (ah_object_value(tmp, i).t == AV_NIL) ah_remove_member(m->vm, tmp, ah_object_member_name(tmp, i));
        ah_copy_members(m->vm, dobj, tmp);
    }
    for (int i = 0; i < src->nkids && i < dst->nkids; i++) copy_script(m, src->kids[i], dst->kids[i]);
}

static int nat_do_copy(ah_vm *vm, ah_obj *self, void *d, int argc, const ah_val *argv, ah_val *ret)
{
    (void)argc; (void)argv;
    menu *m = d;
    ui_widget *w = ah_host_data(self);
    ui_widget *c = ui_widget_copy(m->ui, w);
    if (!c) return ah_error(vm, "doCopy failed");
    copy_script(m, w, c);
    *ret = ah_objv(widget_module(m, c));
    return 0;
}

/* parent.appendChild(context, item), or parent.appendChild(item). */
static int nat_append_child(ah_vm *vm, ah_obj *self, void *d, int argc, const ah_val *argv, ah_val *ret)
{
    (void)vm; (void)d;
    ah_obj *item = argc ? ah_as_obj(argv[argc-1], AO_MODULE) : NULL;
    if (item && ah_host_of(item) == &widget_host) {
        ui_widget *box = ah_host_data(self), *child = ah_host_data(item);
        /* a VBox/HBox stacks what is appended after what it holds
           (labomode.ad's folders, copied from templates at other places) */
        int vbox = !strcmp(box->type, "VBox"), hbox = !strcmp(box->type, "HBox");
        if ((vbox || hbox) && !mp_number(box->src, "packing", 0)) {
            float at = 0;
            for (int i = 0; i < box->nkids; i++) {
                /* a face without its own rectangle is the box's background
                   (the dropdown vbox's "bg"), not a row */
                const ui_widget *k = box->kids[i];
                size_t n = strlen(k->type);
                if (n >= 4 && !strcmp(k->type + n - 4, "Face") && !mp_prop(k->src, "geometry")) continue;
                at = fmaxf(at, vbox ? k->y + k->h : k->x + k->w);
            }
            if (vbox) { child->x = 0; child->y = at; } else { child->x = at; child->y = 0; }
        }
        /* 002A2608: an ImageFace with no rectangle of its own draws in its
           parent's client area, the new one once appended. TuningApplyRoot
           places hidden::pict copies at pict.w * index * 2 and scrolls the
           270x127 frame by PartsPict.w * i * 2 (gtmode.ad L7344, L7452):
           the 320x150 part images fill the frame. */
        if (!strcmp(child->type, "ImageFace") && !child->has_geometry && !mp_prop(child->src, "geometry")) {
            child->w = box->w; child->h = box->h; child->has_geometry = 1;
        }
        ui_widget_append(box, child);
    }
    *ret = ah_nil();
    return 0;
}

/* HistoryRoot inserts era cards before its Surface decoration. */
static int nat_insert_child(ah_vm *vm,ah_obj *self,void *d,int argc,const ah_val *argv,ah_val *ret)
{
    (void)d;ui_widget *parent=ah_host_data(self);
    if(argc!=2)return ah_error(vm,"insertChild requires a child and an anchor");
    ah_obj *item=ah_as_obj(argv[0],AO_MODULE),*anchor=ah_as_obj(argv[1],AO_MODULE);
    if(!item || !anchor || ah_host_of(item)!=&widget_host || ah_host_of(anchor)!=&widget_host)
        return ah_error(vm,"insertChild requires widgets");
    ui_widget *child=ah_host_data(item),*before=ah_host_data(anchor);
    if(before->parent!=parent)return ah_error(vm,"insertChild anchor belongs to another parent");
    if(child!=before){
        ui_widget_append(parent,child);
        if(child->parent!=parent)return ah_error(vm,"insertChild allocation failed");
        for(int i=0;i<parent->nkids-1;i++)if(parent->kids[i]==before){
            memmove(parent->kids+i+1,parent->kids+i,(size_t)(parent->nkids-i-1)*sizeof *parent->kids);
            parent->kids[i]=child;break;
        }
    }
    *ret=ah_nil();return 0;
}

static int nat_remove_child(ah_vm *vm,ah_obj *self,void *d,int argc,const ah_val *argv,ah_val *ret)
{
    (void)vm;(void)d;
    ah_obj *item=argc?ah_as_obj(argv[argc-1],AO_MODULE):NULL;
    ui_widget *w=item && ah_host_of(item)==&widget_host?ah_host_data(item):NULL;
    if(w && w->parent==ah_host_data(self))ui_widget_detach(w);
    *ret=ah_nil();return 0;
}

/* MComposite countChild (002058E0 -> 00206880), and indexed getChild.
   Return the live authored children, including dynamically appended cards. */
static int nat_widget_children(ah_vm *vm,ah_obj *self,void *data,int argc,const ah_val *argv,ah_val *ret)
{
    ui_widget *w=ah_host_data(self);menu *m=ah_user(vm);
    if(!data){*ret=ah_int(w->nkids);return 0;}
    if(argc!=1 || !ah_is_num(argv[0]))return ah_error(vm,"getChild requires one index");
    double value=ah_num(argv[0]);
    if(!isfinite(value) || value<0 || value>=w->nkids){*ret=ah_nil();return 0;}
    int64_t index=(int64_t)value;
    *ret=maybe_module(m,index>=0 && index<w->nkids?w->kids[index]:NULL);return 0;
}
static int nat_widget_image(ah_vm *vm,ah_obj *self,void *data,int argc,const ah_val *argv,ah_val *ret)
{
    (void)data;const char *path=argc?ah_cstr(argv[0]):NULL;
    if(argc!=1 || !path)return ah_error(vm,"setImagePath requires one path");
    ui_widget_set_image(ah_host_data(self),path);*ret=ah_nil();return 0;
}

/* Distinguish an absent optional label from a named TextFace. Scripts
   query defined("label") before indexing dynamically selected tickets. */
static int nat_widget_defined(ah_vm *vm,ah_obj *self,void *data,int argc,const ah_val *argv,ah_val *ret)
{
    (void)data;ui_widget *w=ah_host_data(self);const char *name=argc?ah_cstr(argv[0]):NULL;
    ah_val value;
    if(name && !strcmp(name,"label") && !ui_widget_child(w,name) && !mp_prop(w->src,name)){
        *ret=ah_bool(0);return 0;
    }
    *ret=ah_bool(name && ah_get_member(vm,self,name,&value));return 0;
}
/* func_001B1050 -> func_0042E6B8. US disc divisor is 1 (0042E440).
   Nonpositive amounts are displayed as "--", not as zero or negative money. */
static int nat_money_string(ah_vm *vm,ah_obj *self,void *data,int argc,const ah_val *argv,ah_val *ret)
{
    (void)self;(void)data;
    if(argc!=1 || !ah_is_num(argv[0]) || !isfinite(ah_num(argv[0])))return ah_error(vm,"GetMoneyString requires one amount");
    char out[48];if(!gt4_game_money_text(ah_num(argv[0]),out,sizeof out))return ah_error(vm,"money display overflow");
    *ret=ah_str(vm,out);return 0;
}

/* UpdateContext enter/leaveEventLoop (0024EB40 -> 00251CE8,
   00251E80): each nested call owns a live frame; leave stops the innermost
   frame, and only then does its suspended script caller resume. The PC
   presenter replaces the original platform update/render callbacks. */
void menu_set_event_presenter(menu *m,int (*fn)(menu *,void *),void *user)
{if(m){m->present_event=fn;m->event_user=user;}}
static int nat_update_context(ah_vm *vm,ah_obj *self,void *data,int argc,const ah_val *argv,ah_val *ret)
{(void)vm;(void)self;(void)argc;(void)argv;menu *m=data;*ret=ah_objv(m->context);return 0;}
/* context.getUpdateContext().getPadButtonState(port): the buttons held,
   as the platform gives them (menu_set_pad_held). labomode.ad's help
   names the bits: onListActivate's 256 and 2048 are L1 and R1 ("R1 + X
   multiple select", "L1 + R1 + X complete select") */
static int nat_pad_buttons(ah_vm *vm,ah_obj *self,void *data,int argc,const ah_val *argv,ah_val *ret)
{(void)vm;(void)self;(void)argc;(void)argv;menu *m=data;*ret=ah_int(argc && ah_num(argv[0])!=0?0:m->pad_held);return 0;}
void menu_set_pad_held(menu *m,unsigned buttons){if(m)m->pad_held=buttons;}
static int nat_pad_analog(ah_vm *vm,ah_obj *self,void *data,int argc,const ah_val *argv,ah_val *ret)
{
    (void)vm;(void)self;(void)data;(void)argc;(void)argv;
    /* P: photo keyboard movement is read by photo_travel at the platform
       boundary; untouched analog channels are centered. */
    *ret=ah_float(0);return 0;
}
static int nat_event_loop(ah_vm *vm,ah_obj *self,void *data,int argc,const ah_val *argv,ah_val *ret)
{
    (void)self;(void)argc;(void)argv;menu *m=ah_user(vm);*ret=ah_nil();
    if(data){if(m->event_depth)m->event_loops[m->event_depth-1]=0;return 0;}
    if(!m->present_event)return ah_error(vm,"modal event loop requires a platform presenter");
    if(m->event_depth>=32)return ah_error(vm,"modal event loop nesting limit");
    int level=m->event_depth++;m->event_loops[level]=1;
    apply_pages(m);
    int interrupted=0;
    while(m->event_loops[level]){
        unsigned budget=ah_exchange_instruction_budget(vm,1000000);
        int alive=m->present_event(m,m->event_user);
        ah_exchange_instruction_budget(vm,budget);
        if(!alive){interrupted=1;break;}
    }
    m->event_loops[level]=0;m->event_depth--;
    return interrupted?ah_error(vm,"modal event loop interrupted by application shutdown"):0;
}

/* labomode's EntryRoot keyboard on an InputTextFace: putString(s) at the
   caret up to max_input_length characters, backspace, left, right */
static int nat_input_edit(ah_vm *vm, ah_obj *self, void *d, int argc, const ah_val *argv, ah_val *ret)
{
    ui_widget *w = ah_host_data(self);
    const char *op = d;
    char buf[128], next[256];
    const char *t = w->text ? w->text : "";
    int len = (int)strlen(t), max = (int)mp_number(w->src, "max_input_length", 16);
    *ret = ah_nil();
    if (w->caret < 0 || w->caret > len) w->caret = len;
    if (!strcmp(op, "left")) { if (w->caret > 0) w->caret--; return 0; }
    if (!strcmp(op, "right")) { if (w->caret < len) w->caret++; return 0; }
    if (!strcmp(op, "backspace")) {
        if (w->caret <= 0) return 0;
        snprintf(next, sizeof next, "%.*s%s", w->caret - 1, t, t + w->caret);
        w->caret--;
    } else {
        const char *add = argc ? ah_to_text(argv[0], buf, sizeof buf) : "";
        int room = max - len, n = (int)strlen(add);
        if (n > room) n = room;
        if (n <= 0) return 0;
        snprintf(next, sizeof next, "%.*s%.*s%s", w->caret, t, n, add, t + w->caret);
        w->caret += n;
    }
    (void)vm;
    free(w->text);
    w->text = strdup(next);
    return 0;
}

static int nat_noop(ah_vm *vm, ah_obj *self, void *d, int argc, const ah_val *argv, ah_val *ret)
{
    (void)vm; (void)self; (void)d; (void)argc; (void)argv;
    *ret = ah_nil();
    return 0;
}

static int nat_widget_is_closed(ah_vm *vm,ah_obj *self,void *d,int argc,const ah_val *argv,ah_val *ret)
{
    (void)d;(void)argc;(void)argv;
    menu *m=ah_user(vm);ui_widget *w=ah_host_data(self);
    int closed=!m || !w || menu_active_root(m)!=w->root || !m->entered[w->root];
    *ret=ah_bool(closed);return 0;
}

/* quick-event.ad:893 supplies the trophy model and three loading flags:
   the ModelFace draws that model (A: the flags 0, 1, 0 not read) */
static int nat_car_option_motion(ah_vm *vm,ah_obj *self,void *d,int argc,const ah_val *argv,ah_val *ret){
    menu *m=ah_user(vm);ui_widget *w=ah_host_data(self);(void)d;
    if(argc<1 || !ah_cstr(argv[0]) || !w)return ah_error(vm,"loadModelOptionMotion requires widget/path");
    w->car_camera=ui_load_camera(m->ui,w->resource_root,ah_cstr(argv[0]));w->car_camera_time=0;w->car_camera_loop=1;w->car_camera_playing=1;
    if(!w->car_camera)return ah_error(vm,"original option camera unavailable: %s",ah_cstr(argv[0]));
    *ret=ah_nil();return 0;
}
static int nat_car_crew_load(ah_vm *vm,ah_obj *self,void *d,int argc,const ah_val *argv,ah_val *ret){
    menu *m=ah_user(vm);ui_widget *w=ah_host_data(self);(void)d;
    if(argc!=1 || !w || !ah_cstr(argv[0]))return ah_error(vm,"loadModelSet requires model path");
    size_t size=0;const uint8_t *data=ui_load_model_bytes(m->ui,w->resource_root,ah_cstr(argv[0]),&size);
    if(!data)return ah_error(vm,"additional model unavailable: %s",ah_cstr(argv[0]));
    /* 00138F80 -> 0013A4A8 -> 00153FC8 appends, preserving model indices. */
    static uint64_t serial;
    gt4_service_model *models=realloc(w->car_models,(size_t)(w->car_model_count+1)*sizeof *models);
    if(!models)return ah_error(vm,"additional model allocation failed");w->car_models=models;
    models[w->car_model_count++]=(gt4_service_model){data,size,++serial};
    if(w->car_model_count==1){w->car_crew_serial=serial;w->car_crew_data=data;w->car_crew_size=size;w->car_additional_scale=1;}
    w->car_crew_loop=1;*ret=ah_nil();return 0;
}
static ah_obj *game_object(menu *m);
static int nat_car_crew_motion(ah_vm *vm,ah_obj *self,void *d,int argc,const ah_val *argv,ah_val *ret){
    menu *m=ah_user(vm);ui_widget *w=ah_host_data(self);(void)d;
    if(argc<1 || argc>5 || !w || !ah_cstr(argv[0]) || !w->car_model_count)return ah_error(vm,"loadModelMotion requires a loaded model and path");
    /* 00139098..F0 defaults alignment/hidden/yaw to zero. 00154124..30
       maps a model index beyond the loaded collection to zero. */
    int index=argc>1?(int)ah_num(argv[1]):0;if(index<0)return ah_error(vm,"negative additional model index");
    if(index>=w->car_model_count)index=0;
    const struct gt4_mot1 *motion=ui_load_motion(m->ui,w->resource_root,ah_cstr(argv[0]));
    if(!motion)return ah_error(vm,"additional motion unavailable: %s",ah_cstr(argv[0]));
    gt4_service_motion *tracks=realloc(w->car_motions,(size_t)(w->car_motion_count+1)*sizeof *tracks);
    if(!tracks)return ah_error(vm,"additional motion allocation failed");w->car_motions=tracks;
    tracks[w->car_motion_count++]=(gt4_service_motion){motion,0,argc>4?(float)ah_num(argv[4]):0,index,argc>2?(int)ah_num(argv[2]):0,argc>3?ah_num(argv[3])!=0:0,0,1};
    ah_obj *garage;ah_val v;if(ah_get_member(vm,game_object(m),"garage",&v) && (garage=ah_as_obj(v,AO_MODULE)) && ah_get_member(vm,garage,"riding_car",&v))
        w->car_service_geometry_valid=gt4_game_car_service_geometry(vm,ah_as_obj(v,AO_MODULE),w->car_service_geometry);
    if(w->car_motion_count==1){w->car_crew_motion=motion;w->car_crew_time=0;w->car_crew_playing=0;}
    *ret=ah_nil();return 0;
}
static int nat_car_crew_play(ah_vm *vm,ah_obj *self,void *d,int argc,const ah_val *argv,ah_val *ret){
    ui_widget *w=ah_host_data(self);(void)vm;
    /* 001525F0 -> 00153E20 marks additional motion controllers playing;
       00152650 -> 00153F28 sets loop, defaulting to true without an argument. */
    if(d==(void *)2){*ret=ah_bool(w->car_camera && w->car_camera_playing);return 0;} /* 00153FA8 tests only option motion+10. */
    if(d){w->car_crew_loop=argc?ah_num(argv[0])!=0:1;w->car_camera_loop=w->car_crew_loop;}
    else{w->car_crew_playing=1;w->car_camera_playing=1;}
    for(int i=0;i<w->car_motion_count;i++){
        if(d)w->car_motions[i].loop=w->car_crew_loop;
        else w->car_motions[i].playing=1;
    }
    *ret=ah_nil();return 0;
}
static int nat_model_set_path(ah_vm *vm,ah_obj *self,void *d,int argc,const ah_val *argv,ah_val *ret)
{
    (void)d;if(argc!=4 || !ah_cstr(argv[0]))return ah_error(vm,"setModelSetPath requires path and three flags");
    ah_set_member(vm,self,"requested_model_path",argv[0]);
    ui_widget *w=ah_host_data(self);
    if(w && !strcmp(w->type,"ModelFace")){free(w->model_path);w->model_path=strdup(ah_cstr(argv[0]));}
    ah_obj *flags=ah_array_new(vm);for(int i=1;i<4;i++)ah_array_push(vm,flags,argv[i]);
    ah_set_member(vm,self,"requested_model_flags",ah_objv(flags));*ret=ah_nil();return 0;
}
static int nat_set_focus(ah_vm *vm, ah_obj *self, void *d, int argc, const ah_val *argv, ah_val *ret)
{
    (void)vm;
    menu *m = d;
    ui_widget *root = ah_host_data(self);
    ah_obj *o = argc ? ah_as_obj(argv[0], AO_MODULE) : NULL;
    ui_widget *w = o && ah_host_of(o) == &widget_host ? ah_host_data(o) : NULL;
    if (w && w->root != root->root) return ah_error(vm, "setFocus: widget belongs to another page");
    if (root->root == menu_active_root(m)) set_focus(m, w);
    else m->focus[root->root] = w;
    *ret = ah_nil(); return 0;
}

/* The scripts transfer focus between the course and direction SelectBoxes. */
static int nat_widget_focus(ah_vm *vm,ah_obj *self,void *d,int argc,const ah_val *argv,ah_val *ret)
{
    (void)argc;(void)argv;menu *m=ah_user(vm);ui_widget *w=ah_host_data(self);
    if(!d) {
        ui_widget *pick=w;
        if(!strcmp(w->type,"SelectBox") && w->nkids) {
            int index=w->focus_index;
            if(index<0 || index>=w->nkids)index=0;
            pick=w->kids[index];
        }
        set_focus(m,pick);
    } else {
        for(ui_widget *p=m->focus[w->root];p;p=p->parent)
            if(p==w){set_focus(m,NULL);break;}
    }
    *ret=ah_nil();return 0;
}
static int nat_get_focus(ah_vm *vm,ah_obj *self,void *d,int argc,const ah_val *argv,ah_val *ret)
{
    (void)vm;(void)argc;(void)argv;menu *m=d;ui_widget *w=ah_host_data(self);
    /* a SelectBar's focused item is its child at its index (memcard.ad's
       FileList), whatever holds the page's focus */
    if(!strcmp(w->type,"SelectBox") && w->name && !strcmp(w->name,"FileList")){
        int i=w->focus_index;
        *ret=w->nkids?maybe_module(m,w->kids[i<0?0:i>=w->nkids?w->nkids-1:i]):ah_nil();return 0;
    }
    *ret=maybe_module(m,m->focus[w->root]);return 0;
}

/* clearChildren takes every child; clearWindow (widget vtable +0x310,
   00205330) empties the window's items and keeps its own faces:
   option.ad append_optionmenu clears each dropdown's vbox, whose "bg"
   ColorFace is the grey behind the popup's rows (the PCSX2 print). A: the
   class's own lists not read; told apart by the children's type. */
static int clear_children(menu *m, ui_widget *w, int keep_faces)
{
    ui_widget *f = m->focus[w->root];
    for (ui_widget *p = f; p; p = p->parent)
        if (p == w) { m->focus[w->root] = NULL; break; }
    for (int i = w->nkids - 1; i >= 0; i--) {
        size_t n = strlen(w->kids[i]->type);
        if (keep_faces && n >= 4 && !strcmp(w->kids[i]->type + n - 4, "Face")) continue;
        ui_widget_detach(w->kids[i]);
    }
    return 0;
}
static int nat_clear_children(ah_vm *vm, ah_obj *self, void *d, int argc, const ah_val *argv, ah_val *ret)
{
    (void)vm; (void)argc; (void)argv;
    *ret = ah_nil();
    return clear_children(d, ah_host_data(self), 0);
}
static int nat_clear_window(ah_vm *vm, ah_obj *self, void *d, int argc, const ah_val *argv, ah_val *ret)
{
    (void)vm; (void)argc; (void)argv;
    *ret = ah_nil();
    return clear_children(d, ah_host_data(self), 1);
}

static int nat_adjust_scale(ah_vm *vm, ah_obj *self, void *d, int argc, const ah_val *argv, ah_val *ret)
{
    (void)vm; (void)argc; (void)argv;
    menu *m = d;
    ui_text_adjust_scale(m->ui, ah_host_data(self));
    *ret = ah_nil();
    return 0;
}

static int nat_abbreviate(ah_vm *vm,ah_obj *self,void *d,int argc,const ah_val *argv,ah_val *ret)
{
    (void)vm;(void)argc;(void)argv;menu *m=d;
    *ret=ah_bool(ui_text_abbreviate(m->ui,ah_host_data(self)));return 0;
}

/* context.translate(root, key), or context.translate(root, page, key). */
static int nat_translate(ah_vm *vm, ah_obj *self, void *d, int argc, const ah_val *argv, ah_val *ret)
{
    (void)self;
    menu *m = d;
    ah_obj *obj = argc >= 2 ? ah_as_obj(argv[0], AO_MODULE) : NULL;
    const char *text = "";
    if (obj && ah_host_of(obj) == &widget_host && (argc == 2 || argc == 3)) {
        ui_widget *w = ah_host_data(obj);
        char page[128], key[128];
        const char *label = ah_to_text(argv[argc - 1], key, sizeof key);
        text = ui_translate(m->ui, w->root,
                            argc == 3 ? ah_to_text(argv[1], page, sizeof page) : NULL,
                            label);
        /* Scripts also pass already-translated strings (OptionRoot.onCancel). */
        if (!text) text = label;
        *ret = ah_str(vm, text); return 0;
    }
    if (obj && obj == m->project && argc == 3) {   /* the project and a page: labomode's CourseNameCaption */
        char page[128], key[128];
        const char *label = ah_to_text(argv[2], key, sizeof key);
        text = ui_translate(m->ui, menu_active_root(m) < 0 ? 0 : menu_active_root(m), ah_to_text(argv[1], page, sizeof page), label);
        *ret = ah_str(vm, text ? text : label); return 0;
    }
    *ret = ah_str(vm, text);
    return 0;
}

static void assign_index(ui_widget *w,int index)
{
    if(!strcmp(w->type,"OptionMenu")) {
        int count=ui_option_count(w);
        if(index>=count)index=count-1;
        if(index<0)index=0;
    }
    if(!strcmp(w->type,"SliderBar") || !strcmp(w->type,"ScaleBar") && w->slider_range_set){double range[2]={0,100};mp_numbers(w->src,"range",range,2);
        if(w->slider_range_set){range[0]=w->slider_min;range[1]=w->slider_max;}
        if(index<range[0])index=(int)range[0];if(index>range[1])index=(int)range[1];}
    w->focus_index=index;
}

/* 002C7A80 converts each mask integer to !=0; 002C9B68 applies at
   most min(mask length, logical item count), leaving the tail intact.
   Disabling the current choice calls 002C9898 -> 002C9740(start=0):
   first non-disabled logical child; no eligible child preserves index. */
static int nat_option_insensitive(ah_vm *vm,ah_obj *self,void *d,int argc,const ah_val *argv,ah_val *ret)
{
    (void)vm;(void)d;ui_widget *w=ah_host_data(self);*ret=ah_nil();
    if(argc<2 || !w)return 0;
    ah_obj *mask=ah_as_obj(argv[1],AO_ARRAY);if(!mask)return ah_error(vm,"setInsensitive requires an array");
    int count=ui_option_count(w),n=ah_array_size(mask),reset=0;if(n>count)n=count;
    for(int i=0;i<n;i++){
        ui_widget *item=ui_option_item(w,i);ah_val value=ah_array_get(mask,i);
        int disabled=(value.t==AV_INT || value.t==AV_LONG || value.t==AV_BOOL)?(uint32_t)value.u.i!=0:(int32_t)ah_num(value)!=0;
        item->insensitive=disabled;if(i==w->focus_index && disabled)reset=1;
    }
    if(reset)for(int i=0;i<count;i++)if(!ui_option_item(w,i)->insensitive){w->focus_index=i;break;}
    return 0;
}
static int nat_set_index(ah_vm *vm, ah_obj *self, void *d, int argc, const ah_val *argv, ah_val *ret)
{
    (void)vm; (void)d;
    ui_widget *w = ah_host_data(self);
    if (argc >= 2) {
        int index = (int)ah_num(argv[1]);
        assign_index(w,index);
    }
    *ret = ah_nil(); return 0;
}

static int nat_items(ah_vm *vm,ah_obj *self,void *d,int argc,const ah_val *argv,ah_val *ret)
{
    menu *m=ah_user(vm);ui_widget *w=ah_host_data(self);int op=(int)(intptr_t)d;*ret=ah_nil();
    if(op==0) {
        ah_obj *o=argc>1?ah_as_obj(argv[1],AO_MODULE):NULL;
        if(!o || ah_host_of(o)!=&widget_host)return ah_error(vm,"invalid item template");
        w->item_template=ah_host_data(o);
    } else if(op==1) {
        int count=argc?(int)ah_num(argv[0]):0;
        if(!w->item_template || count<0 || count>2048)return ah_error(vm,"invalid item count");
        /* the focus let go only if it was on an item going away (not the list) */
        for(ui_widget *f=m->focus[w->root];f;f=f->parent)if(f->parent==w){m->focus[w->root]=NULL;break;}
        while(w->nkids)ui_widget_detach(w->kids[w->nkids-1]);
        for(int i=0;i<count;i++) {
            ui_widget *c=ui_widget_copy(m->ui,w->item_template);
            if(!c)return ah_error(vm,"cannot copy item");
            copy_script(m,w->item_template,c);ui_widget_append(w,c);
        }
        /* its visible_callback runs on the next tick, for every item */
        int known=0;
        for(int i=0;i<m->nitem_lists;i++)known|=m->item_lists[i]==w;
        if(!known && m->nitem_lists<8)m->item_lists[m->nitem_lists++]=w;
    } else if(op==3) {
        int i=argc?(int)ah_num(argv[0]):-1;*ret=ah_bool(i>=0 && i<w->nkids);   /* A: every item counts as shown */
    } else if(op==4) {
        int i=argc?(int)ah_num(argv[0]):-1,flags=argc>1?(int)ah_num(argv[1]):0,k=0;
        while(k<m->nitem_updates && !(m->item_updates[k].list==w && m->item_updates[k].index==i))k++;
        if(flags && k==m->nitem_updates && k<64){m->item_updates[k].list=w;m->item_updates[k].index=i;m->nitem_updates++;}
        else if(!flags && k<m->nitem_updates)m->item_updates[k]=m->item_updates[--m->nitem_updates];
    } else if(op==6) *ret=ah_int(w->nkids);
    else if(op==5 || op==7) {}   /* setItemWidget (a widget let go), postRedraw */
    else {
        int i=argc?(int)ah_num(argv[0]):-1;
        if(i>=0 && i<w->nkids)*ret=maybe_module(m,w->kids[i]);
    }
    return 0;
}
/* list.setItemActive(index, on): an item's active state (its faces' visible_compare) */
static int nat_item_active(ah_vm *vm,ah_obj *self,void *d,int argc,const ah_val *argv,ah_val *ret)
{
    ui_widget *w=ah_host_data(self);int i=argc?(int)ah_num(argv[0]):-1;(void)vm;(void)d;
    if(i>=0 && i<w->nkids)w->kids[i]->active=argc>1?ah_truthy(argv[1]):1;
    *ret=ah_nil();return 0;
}
/* widget.interpolateY(target, ratio) (002579A0 -> 0025C0A0/0025BFE0):
   within one unit of the target it lands there and answers 1, otherwise
   it moves by the distance times the ratio and answers 0 */
static int nat_interpolate_y(ah_vm *vm,ah_obj *self,void *d,int argc,const ah_val *argv,ah_val *ret)
{
    ui_widget *w=ah_host_data(self);(void)d;
    if(argc<2)return ah_error(vm,"interpolateY requires target and ratio");
    float target=(float)ah_num(argv[0]),ratio=(float)ah_num(argv[1]),dist=target-w->y;
    if(-1.f<dist && dist<1.f){w->y=target;*ret=ah_int(1);return 0;}
    w->y=w->y+dist*ratio;*ret=ah_int(0);return 0;
}
/* parent.movetoLast(child): the child made the last one, drawn over its
   siblings (gtmode.ad TuningApplyRoot L7444: Spec.movetoLast(Spec.label),
   the label over the chip appended at L7442) */
static int nat_move_to_last(ah_vm *vm,ah_obj *self,void *d,int argc,const ah_val *argv,ah_val *ret)
{
    (void)vm;(void)d;
    ah_obj *child=argc?ah_as_obj(argv[0],AO_MODULE):NULL;
    if(argc && (!child || ah_host_of(child)!=&widget_host))return ah_error(vm,"movetoLast requires a child widget");
    ui_widget *w=child?ah_host_data(child):ah_host_data(self),*pa=child?ah_host_data(self):w?w->parent:NULL;
    if(pa)for(int i=0;i<pa->nkids;i++)if(pa->kids[i]==w){
        memmove(&pa->kids[i],&pa->kids[i+1],(size_t)(pa->nkids-i-1)*sizeof *pa->kids);
        pa->kids[pa->nkids-1]=w;break;
    }
    *ret=ah_nil();return 0;
}
static int nat_adjust_size(ah_vm *vm,ah_obj *self,void *d,int argc,const ah_val *argv,ah_val *ret)
{
    (void)vm;(void)argc;(void)argv;menu *m=d;ui_adjust_size(m->ui,ah_host_data(self));*ret=ah_nil();return 0;
}
static int nat_load_gpb(ah_vm *vm,ah_obj *self,void *d,int argc,const ah_val *argv,ah_val *ret)
{
    (void)self;menu *m=d;
    if(argc!=2 || !ui_load_package(m->ui,ah_cstr(argv[0]),ah_cstr(argv[1])))return ah_error(vm,"missing menu image package");
    *ret=ah_nil();return 0;
}
static int nat_exist_gpb(ah_vm *vm,ah_obj *self,void *data,int argc,const ah_val *argv,ah_val *ret)
{
    (void)self;menu *m=data;const char *path=argc?ah_cstr(argv[0]):NULL;
    if(argc!=1 || !path)return ah_error(vm,"existGpbBinary requires one path");
    *ret=ah_bool(ui_package_contains(m->ui,menu_active_root(m),path));return 0;
}

static void scroll_to(ui_widget *w, int snap);

/* scroll_window.warp(item): show item at once, without easing. */
static int nat_scroll_warp(ah_vm *vm, ah_obj *self, void *d, int argc, const ah_val *argv, ah_val *ret)
{
    (void)vm; (void)self; (void)d;
    ah_obj *item = argc ? ah_as_obj(argv[0], AO_MODULE) : NULL;
    if (item && ah_host_of(item) == &widget_host) scroll_to(ah_host_data(item), 1);
    *ret = ah_nil();
    return 0;
}

/* scroll_window.updateGeometry(): the scrolled "space" takes the size of its
   content (MakerRoot then adds a 32-unit border). Content is the union of
   the visible children's rectangles. */
static int nat_update_geometry(ah_vm *vm, ah_obj *self, void *d, int argc, const ah_val *argv, ah_val *ret)
{
    (void)vm; (void)argc; (void)argv;
    menu *m=d;
    ui_widget *sw = ah_host_data(self), *clip = ui_widget_child(sw, "clip"), *space = ui_widget_child(clip, "space");
    *ret = ah_nil();
    if (!space) return 0;
    if(!strcmp(ah_module_name(m->project),"LicenseProject")) {
        ui_widget *info=ui_widget_child(space,"Info"),*label=ui_widget_child(info,"label");
        double parent_rect[4],label_rect[4];
        if(info && label && mp_numbers(info->src,"geometry",parent_rect,4)==4 &&
           mp_numbers(label->src,"geometry",label_rect,4)==4 && parent_rect[3]>0) {
            /* 0025B578 +0xc0..0xd0: relative height follows the parent.
               PCSX2 B-1 menu: TextFace 01e34740 has geometry mode 1,
               local h=1 and parent Info/space h=240. quick-license.ad
               L255..264 enlarges Info before reading label.h; using the
               preferred text height here made range and scrollbar disagree.
               A: paragraph wrapping/measurement still lacks a PS2 oracle. */
            label->h=(float)(label_rect[3]/parent_rect[3])*info->h;
        }
    }
    float r = 0, b = 0;
    for (int i = 0; i < space->nkids; i++) {
        const ui_widget *c = space->kids[i];
        if (!c->visible) continue;
        r = fmaxf(r, c->x + c->w);
        b = fmaxf(b, c->y + c->h);
    }
    if (r > 0) space->w = r;
    if (b > 0) space->h = b;
    return 0;
}

/* ToolTipFace.remove(): empty help line. */
static int nat_tooltip_clear(ah_vm *vm, ah_obj *self, void *d, int argc, const ah_val *argv, ah_val *ret)
{
    (void)vm; (void)d; (void)argc; (void)argv;
    ui_set_tooltips(ah_host_data(self), NULL);
    *ret = ah_nil();
    return 0;
}

/* 00233468 -> 00235CF8 refreshes the current focus; 00249B70 starts
   the tooltip controller again, even when its text has not changed. */
static int nat_tooltip_reset(ah_vm *vm, ah_obj *self, void *d, int argc, const ah_val *argv, ah_val *ret)
{
    (void)vm; (void)argc; (void)argv;
    menu *m=d; ui_widget *root=ah_host_data(self);
    if(root && root->root>=0 && root->root<ui_root_count(m->ui)) {
        set_tooltip(m,root->root,m->focus[root->root]);
        ui_reset_tooltips(root);
    }
    *ret=ah_nil(); return 0;
}

static int nat_text_size(ah_vm *vm,ah_obj *self,void *d,int argc,const ah_val *argv,ah_val *ret)
{
    (void)argc;(void)argv;menu *m=d;float size[2];
    ui_widget *w=ah_host_data(self);
    /* quick-license.ad L253..256 measures the multiline information
       after assigning it. Its width and multiline flag are authored in
       quick-license.mproject. Reuse the existing word-wrap adapter (A:
       original text layout/equalization has no differential proof yet). */
    if(!strcmp(ah_module_name(m->project),"LicenseProject") && mp_number(w->src,"multiline",0))
        ui_text_wrap(m->ui,w,w->w);
    ui_text_size(m->ui,w,size);
    /* 00245950 +0x38..0x4c publishes the preferred size (+0x44), not
       allocated geometry (+0x34). updateGeometry resolves label.h from Info. */
    ah_obj *a=ah_array_new(vm);ah_array_push(vm,a,ah_float(size[0]));ah_array_push(vm,a,ah_float(size[1]));*ret=ah_objv(a);return 0;
}

static int nat_flash(ah_vm *vm,ah_obj *self,void *d,int argc,const ah_val *argv,ah_val *ret)
{
    ui_widget *w=ah_host_data(self);
    if(d) {
        const char *path=argc?ah_cstr(argv[0]):NULL;
        if(!path)return ah_error(vm,"setFlashPath requires a path");
        free(w->flash_path);w->flash_path=strdup(path);
    }
    w->flash_time=0;*ret=ah_nil();return 0;
}

static ah_val color_value(ah_vm *vm,const double rgba[4]);
static int widget_color(ah_vm *vm,ah_obj *self,void *data,int argc,const ah_val *argv,ah_val *ret);
/* CarFace's original syncLoading wrapper is func_00138F50 -> func_0013A470,
   which waits through the model loader vtable. The PC loader owns its thread. */
void menu_set_car_loader(menu *m,int (*fn)(menu *,const ui_widget *,void *),void *user)
{if(m){m->load_car=fn;m->car_loader_user=user;}}
static int nat_car_sync(ah_vm *vm,ah_obj *self,void *data,int argc,const ah_val *argv,ah_val *ret)
{
    (void)data;(void)argc;(void)argv;menu *m=ah_user(vm);ui_widget *w=ah_host_data(self);
    if(w && w->car_code && (!m->load_car || !m->load_car(m,w,m->car_loader_user)))
        return ah_error(vm,"CarFace model loading failed: %s",w->car_code);
    *ret=ah_nil();return 0;
}
static int nat_car_warp(ah_vm *vm,ah_obj *self,void *data,int argc,const ah_val *argv,ah_val *ret)
{(void)vm;(void)data;(void)argc;(void)argv;ui_widget *w=ah_host_data(self);
 /* 00153B08 +0x00..0x54 copies targets into the displayed pose. */
 memcpy(w->car_position,w->car_target_position,sizeof w->car_position);
 memcpy(w->model_angles,w->car_target_angles,sizeof w->model_angles);
 w->car_pose_set=1;w->car_spin=0;w->car_spin_ticks=0;*ret=ah_nil();return 0;}
static int car_model_set(ah_vm *vm,ah_obj *self,const char *name,ah_val value)
{
    (void)vm;ui_widget *w=ah_host_data(self);const char *angle[]={"rx","ry","rz"},*position[]={"tx","ty","tz"};
    if(!strcmp(name,"motion")){
        w->car_motion=(int)ah_num(value);w->car_spin_ticks=0;return 0;
    }
    if(!strcmp(name,"posInterpolateSpeed")){
        float speed=(float)ah_num(value);
        if(isfinite(speed)){w->car_position_ratio=speed;w->car_position_easing=1;}
        return 0;
    }
    /* 00151A38 default; 00153994 property -> model+8C, 00156E68 scale. */
    if(!strcmp(name,"additionalModelScale")){w->car_additional_scale=(float)ah_num(value);return 0;}
    for(int i=0;i<3;i++){
        if(!strcmp(name,angle[i])){w->car_target_angles[i]=(float)ah_num(value);if(w->car_motion!=2)w->model_angles[i]=w->car_target_angles[i];w->car_pose_set=1;return 0;}
        if(!strcmp(name,position[i])){w->car_target_position[i]=(float)ah_num(value);if(!w->car_motion)w->car_position[i]=w->car_target_position[i];w->car_pose_set=1;return 0;}
    }
    return 0;
}
static const ah_host car_model_host={"car_model",NULL,car_model_set,NULL,NULL};
static ah_val car_model_value(ah_vm *vm,ah_obj *self,ui_widget *w)
{
    /* HomeRoot L3950 configures CarModel even with an empty garage. */
    ah_val value;if(ah_get_member(vm,self,"_pc_car_model",&value))return value;
    ah_obj *model=ah_module_new(vm,"CarModel",NULL,&car_model_host,w);
    const char *fields[]={"rx","ry","rz","tx","ty","tz","motion"};
    for(unsigned i=0;i<sizeof fields/sizeof fields[0];i++)ah_set_member(vm,model,fields[i],ah_float(0));
    w->car_pose_set=0;
    ah_set_member(vm,model,"warp",ah_bind(vm,model,ah_native(vm,"warp",nat_car_warp,NULL)));
    ah_set_member(vm,model,"playModelMotion",ah_bind(vm,model,ah_native(vm,"playModelMotion",nat_car_crew_play,NULL)));
    ah_set_member(vm,model,"setModelMotionLoop",ah_bind(vm,model,ah_native(vm,"setModelMotionLoop",nat_car_crew_play,(void *)1)));
    ah_set_member(vm,model,"isPlayingModelMotion",ah_bind(vm,model,ah_native(vm,"isPlayingModelMotion",nat_car_crew_play,(void *)2)));
    value=ah_objv(model);ah_set_member(vm,self,"_pc_car_model",value);return value;
}
static int nat_photo(ah_vm *vm,ah_obj *self,void *data,int argc,const ah_val *argv,ah_val *ret)
{
    (void)self;menu *m=ah_user(vm);const char *op=data;*ret=ah_nil();
    if(!strcmp(op,"getAperture")) {
        if(!m->photo_apertures_ready)return ah_error(vm,"Photo Travel: original aperture table unavailable");
        int i=argc?to_int(argv[argc-1]):1;i=i<1?1:i>16?16:i;*ret=ah_float(m->photo_apertures[i]);
    } else if(!strcmp(op,"setBaseLen")) {
        /* 001960F8 -> 00194C90 / 00370240: native map length. */
        if(argc && ah_is_num(argv[argc-1]) && isfinite(ah_num(argv[argc-1])) && ah_num(argv[argc-1])>0)m->photo.location.map_length=(float)ah_num(argv[argc-1]);
    } else if(!strcmp(op,"loadShutterSound")) {
        const char *kind=argc?ah_cstr(argv[argc-1]):NULL;
        if(m->sound && !gt4_sound_shutter(m->sound,kind))return ah_error(vm,"Photo Travel: shutter bank unavailable");
    } else if(!strcmp(op,"isRendering")) *ret=ah_bool(!m->photo.image_width);
    else if(!strcmp(op,"doFocus")) {
        /* 001952D0: centre-depth focus, consumed by the scene renderer. */
        m->photo.focus_requested=1;*ret=ah_int(0);
    }
    return 0;
}

static int widget_get(ah_vm *vm, ah_obj *self, const char *s, ah_val *out)
{
    menu *m = ah_user(vm);
    ui_widget *w = ah_host_data(self);
    if (!w) return 0;

    if(!strcmp(w->type,"ScaleBar")) {
        float *field=!strcmp(s,"value")?&w->photo_value:!strcmp(s,"min")?&w->photo_min:
            !strcmp(s,"max")?&w->photo_max:!strcmp(s,"step")?&w->photo_step:
            !strcmp(s,"current_value")?&w->photo_current:!strcmp(s,"repeat_start")?&w->photo_repeat:NULL;
        if(field){*out=ah_float(*field);return 1;}
    }
    /* Photo filters use SliderBar.repeat_start. The script assigns it;
       its module value must remain numeric, rather than a TextFace string. */
    if(!strcmp(w->type,"SliderBar") && !strcmp(s,"value")) {
        *out=ah_int(w->focus_index);return 1;
    }
    if(!strcmp(w->type,"PhotoMapWindow")) {
        gt4_photo_mode *p=&m->photo;
        float *field=!strcmp(s,"wheel_angle")?&p->wheel_angle:!strcmp(s,"lightness")?&p->lightness:
            !strcmp(s,"brightness")?&p->brightness:!strcmp(s,"contrast")?&p->contrast:
            !strcmp(s,"view_angle")?&p->view_angle:!strcmp(s,"aperture")?&p->aperture:NULL;
        if(field){*out=ah_float(*field);return 1;}
        if(!strcmp(s,"operate_mode")){*out=ah_str(vm,p->operate_mode);return 1;}
        if(!strcmp(s,"color_filter")){*out=ah_str(vm,p->color_filter);return 1;}
        if(!strcmp(s,"whiteBalance")){*out=ah_int(p->white_balance);return 1;}
        if(!strcmp(s,"stand")){*out=ah_int(p->stand);return 1;}
        if(!strcmp(s,"can_shoot")){*out=ah_int(p->ready);return 1;}
        if(!strcmp(s,"getAperture") || !strcmp(s,"setBaseLen") || !strcmp(s,"doFocus")) {
            *out=ah_bind(vm,self,ah_native(vm,s,nat_photo,(void *)ah_sym(vm,s)));return 1;
        }
    }
    if(!strcmp(w->type,"SlideShowFace") || ((!strcmp(m->project_label,"labomode") || !strcmp(m->project_label,"photo_save")) && !strcmp(w->type,"PhotoRenderFace"))) {
        if(!strcmp(s,"doPlay") || !strcmp(s,"doStop") || !strcmp(s,"setPhotoFile") || !strcmp(s,"allocateImageBuffer") || !strcmp(s,"outUSBStorage")) {
            *out=ah_bind(vm,self,ah_native(vm,s,nat_album_widget,(void *)ah_sym(vm,s)));return 1;
        }
        if(!strcmp(s,"shuffle")){*out=ah_int(m->album_shuffle);return 1;}
    }
    if(!strcmp(w->type,"PhotoRenderFace")) {
        gt4_photo_mode *p=&m->photo;
        if(!strcmp(s,"imageWidth")){*out=ah_int(p->image_width);return 1;}
        if(!strcmp(s,"imageHeight")){*out=ah_int(p->image_height);return 1;}
        if(!strcmp(s,"imageScale")){*out=ah_float(p->image_scale);return 1;}
        if(!strcmp(s,"ofs_x")){*out=ah_float(p->image_x);return 1;}
        if(!strcmp(s,"ofs_y")){*out=ah_float(p->image_y);return 1;}
        if(!strcmp(s,"portrait")){*out=ah_int(p->stand!=0);return 1;}
        if(!strcmp(s,"isRendering")){*out=ah_bind(vm,self,ah_native(vm,s,nat_photo,(void *)ah_sym(vm,s)));return 1;}
    }
    if(!strcmp(s,"color") || !strcmp(s,"shadow_color")){
        int shadow=!strcmp(s,"shadow_color");double rgba[4]={255,255,255,255};
        mp_numbers(w->src,shadow?"shadow_color":!strcmp(w->type,"TextFace")?"text_color":"color",rgba,4);
        const uint8_t *runtime=shadow?w->runtime_shadow:w->runtime_color;
        for(int i=0;i<4;i++)rgba[i]=((shadow?w->has_runtime_shadow:w->has_runtime_color)?runtime[i]:rgba[i])/255.;
        *out=color_value(vm,rgba);return 1;
    }
    if(!strcmp(s,"defined")){*out=ah_bind(vm,self,ah_native(vm,s,nat_widget_defined,NULL));return 1;}
    if(!strcmp(s,"setColor")){*out=ah_bind(vm,self,ah_native(vm,s,widget_color,NULL));return 1;}
    /* GT Mode has a TextFace named "name". Authored children take priority
       over generic widget properties, as they do in qualified script paths. */
    ui_widget *named_child=ui_widget_child(w,s);
    if(named_child){*out=ah_objv(widget_module(m,named_child));return 1;}
    if((!strcmp(w->type,"SliderBar") || !strcmp(w->type,"ScaleBar")) && !strcmp(s,"range")){
        /* arcade.ad SettingPopup.setupSlider reads the authored pair;
           assignments use 002DFA10's same endpoints as min/max below. */
        double range[2]={0,100};mp_numbers(w->src,"range",range,2);
        if(w->slider_range_set){range[0]=w->slider_min;range[1]=w->slider_max;}
        ah_obj *pair=ah_array_new(vm);for(int i=0;i<2;i++)ah_array_push(vm,pair,ah_int((int)range[i]));
        *out=ah_objv(pair);return 1;
    }
    if((!strcmp(w->type,"SliderBar") || !strcmp(w->type,"ScaleBar")) && (!strcmp(s,"min") || !strcmp(s,"max") || !strcmp(s,"display_min") || !strcmp(s,"display_max"))){
        double range[2]={0,100};mp_numbers(w->src,"range",range,2);
        if(w->slider_range_set){range[0]=w->slider_min;range[1]=w->slider_max;}
        int maximum=strstr(s,"max")!=NULL;
        /* 002DE148/248 read E0/E4; 002DFA10 +1C..20 copies
           assigned range into those display endpoints. */
        if(strstr(s,"display_") && (w->slider_display_set&(1<<maximum)))
            *out=ah_float(maximum?w->slider_display_max:w->slider_display_min);
        else *out=ah_int((int)range[maximum]);return 1;}
    if (!strcmp(s, "name")) { *out = ah_str(vm, w->name ? w->name : ""); return 1; }
    if (!strcmp(s, "label")) { const char *label=mp_string(w->src,"label"); *out=label?ah_str(vm,label):ah_nil(); return 1; }
    if (!strcmp(w->type, "InputTextFace")) {
        if (!strcmp(s, "value")) { *out = ah_str(vm, w->text ? w->text : ""); return 1; }
        if (!strcmp(s, "putString") || !strcmp(s, "backspace") || !strcmp(s, "left") || !strcmp(s, "right")) {
            *out = ah_bind(vm, self, ah_native(vm, s, nat_input_edit, (void *)ah_sym(vm, s)));
            return 1;
        }
    }
    if (!strcmp(w->type, "ToolTipFace")) {
        if(!strcmp(s,"lock")) {
            /* PhotoShoot PreviewRoot L2875..2877: native scalar holder.
               A: tooltip locking elsewhere still needs a separate audit. */
            if(!ah_get_member(vm,self,"_pc_photo_tip_lock",out)) {
                ah_obj *lock=ah_module_new(vm,"tip_lock",NULL,NULL,NULL);
                ah_set_member(vm,lock,"value",ah_int(0));*out=ah_objv(lock);
                ah_set_member(vm,self,"_pc_photo_tip_lock",*out);
            }return 1;
        }
        if (!strcmp(s,"attach")) { *out=ah_bind(vm,self,ah_native(vm,s,nat_noop,m));return 1; }
        if (!strcmp(s, "value")) { *out = ah_str(vm, w->text ? w->text : ""); return 1; }
        if (!strcmp(s, "remove")) { *out = ah_bind(vm, self, ah_native(vm, s, nat_tooltip_clear, m)); return 1; }
    }
    if (!strcmp(s, "resetToolTip")) { *out = ah_bind(vm, self, ah_native(vm, s, nat_tooltip_reset, m)); return 1; }
    if (!strcmp(s, "warp") && !strcmp(w->type, "ScrollWindow")) { *out = ah_bind(vm, self, ah_native(vm, s, nat_scroll_warp, m)); return 1; }
    if (!strcmp(s, "updateGeometry") && !strcmp(w->type, "ScrollWindow")) { *out = ah_bind(vm, self, ah_native(vm, s, nat_update_geometry, m)); return 1; }
    if (!strcmp(s, "text") || !strcmp(s, "value")) {   /* value: memcard.ad StoreHouse messages, the translated text */ *out = ah_str(vm, ui_widget_source_text(m->ui, w)); return 1; }
    if (!strcmp(s,"align")) { *out=ah_int(w->has_runtime_align?w->runtime_align:(int)mp_number(w->src,"align",1)); return 1; }
    if (!strcmp(s,"isClosed")) { *out=ah_bind(vm,self,ah_native(vm,s,nat_widget_is_closed,m));return 1; }
    if (!strcmp(s,"getTextSize")) {*out=ah_bind(vm,self,ah_native(vm,s,nat_text_size,m));return 1;}
    if (!strcmp(s, "key")) { *out = ah_str(vm, w->text_key ? w->text_key : ""); return 1; }
    if (!strcmp(s, "visible")) { *out = ah_int(w->visible); return 1; }
    if (!strcmp(s, "x")) { *out = ah_float(w->x); return 1; }
    if (!strcmp(s, "y")) { *out = ah_float(w->y); return 1; }
    if (!strcmp(s, "w")) { *out = ah_float(w->w); return 1; }
    if (!strcmp(s, "h")) { *out = ah_float(w->h); return 1; }
    if (!strcmp(s, "opacity")) { *out = ah_float(w->opacity); return 1; }
    if(!strcmp(w->type,"ModelFace")) {
        if(!strcmp(s,"target_mode")){*out=ah_int(w->model_target_mode);return 1;}
        if(!strcmp(s,"model_rotate") || !strcmp(s,"rotate_v") || !strcmp(s,"model_rotate_v")) {
            const float *v=!strcmp(s,"model_rotate")?w->model_angles:w->model_velocity;
            ah_obj *a=ah_array_new(vm);for(int i=0;i<3;i++)ah_array_push(vm,a,ah_float(v[i]));*out=ah_objv(a);return 1;
        }
    }
    if (!strcmp(s, "scale_x")) { *out = ah_float(w->scale_x); return 1; }
    if (!strcmp(s, "scale_y")) { *out = ah_float(w->scale_y); return 1; }
    if (!strcmp(s, "letter_spacing")) { *out = ah_float(w->letter_spacing); return 1; }
    if (!strcmp(s,"movie_name")) {*out=ah_str(vm,w->movie_name?w->movie_name:"");return 1;}
    if (!strcmp(s,"flash_path")) {*out=ah_str(vm,w->flash_path?w->flash_path:"");return 1;}
    if(!strcmp(w->type,"FlashFace") && (!strcmp(s,"setFlashPath") || !strcmp(s,"reset") || !strcmp(s,"doInitialize"))) {
        *out=ah_bind(vm,self,ah_native(vm,s,nat_flash,!strcmp(s,"setFlashPath")?(void *)1:NULL));return 1;
    }
    if (!strcmp(s,"audio_track")) {*out=ah_int(w->movie_audio_track);return 1;}
    if (!strcmp(s, "image_path")) { *out = ah_str(vm, w->image_path ? w->image_path : ""); return 1; }
    if(!strcmp(s,"setImagePath") && (!strcmp(w->type,"ImageFace") || !strcmp(w->type,"FrameImageFace"))) {
        *out=ah_bind(vm,self,ah_native(vm,s,nat_widget_image,NULL));return 1;
    }
    if(!strcmp(s,"countChild") || !strcmp(s,"getChild")) {
        *out=ah_bind(vm,self,ah_native(vm,s,nat_widget_children,!strcmp(s,"getChild")?(void *)1:NULL));return 1;
    }
    if(!strcmp(s,"selected_item") && !strcmp(w->type,"OptionMenu")){*out=maybe_module(m,ui_option_item(w,w->focus_index));return 1;}
    if (!strcmp(s, "focus_index") || !strcmp(s, "index")) { *out = ah_int(w->focus_index); return 1; }
    if (!strcmp(s, "insensitive")) { *out = ah_bool(w->insensitive); return 1; }
    /* the page's focused widget (labomode.ad resetFocus), and whether one can take it */
    if (!strcmp(s, "focus_widget")) { *out = maybe_module(m, m->focus[w->root]); return 1; }
    /* insensitive or hidden, it cannot: labomode.ad setFocusTopCommand skips
       the commands the selection disabled, down to Option and Exit */
    if (!strcmp(s, "can_focus")) { *out = ah_bool(available(w) && (mp_number(w->src, "can_focus", 0) || !strcmp(w->type,"Button") || !strcmp(w->type,"ListBox"))); return 1; }
    if (!strcmp(s, "setItemActive")) { *out = ah_bind(vm, self, ah_native(vm, s, nat_item_active, m)); return 1; }
    if (!strcmp(s, "active")) { *out = ah_bool(w->active); return 1; }
    if(!strcmp(s,"setInsensitive") && !strcmp(w->type,"OptionMenu")){*out=ah_bind(vm,self,ah_native(vm,s,nat_option_insensitive,m));return 1;}
    if(!strcmp(s,"applyAppend") && !strcmp(w->type,"OptionMenu")){*out=ah_bind(vm,self,ah_native(vm,s,nat_noop,m));return 1;}
    if (!strcmp(s, "setIndex")) { *out = ah_bind(vm, self, ah_native(vm, s, nat_set_index, m)); return 1; }
    if (!strcmp(s,"callFocus") || !strcmp(s,"outFocus")) {
        *out=ah_bind(vm,self,ah_native(vm,s,nat_widget_focus,!strcmp(s,"outFocus")?(void *)1:NULL));return 1;
    }
    if (!strcmp(s,"getFocusedWidget")) {*out=ah_bind(vm,self,ah_native(vm,s,nat_get_focus,m));return 1;}
    if (!strcmp(s, "setFocus")) { *out = ah_bind(vm, self, ah_native(vm, s, nat_set_focus, m)); return 1; }
    if (!strcmp(s, "setData") && !strcmp(w->type, "ColorChipFace")) { *out = ah_bind(vm, self, ah_native(vm, s, nat_chip_set_data, m)); return 1; }
    if (!strcmp(s, "setColorIndex") && !strcmp(w->type, "CarFace")) { *out = ah_bind(vm, self, ah_native(vm, s, nat_set_color_index, m)); return 1; }
    if (!strcmp(s, "car_model") && !strcmp(w->type, "CarFace")) { *out=car_model_value(vm,self,w); return 1; }
    if(!strcmp(s,"syncLoading") && !strcmp(w->type,"CarFace")){*out=ah_bind(vm,self,ah_native(vm,s,nat_car_sync,NULL));return 1;}
    if (!strcmp(s, "clearChildren") || !strcmp(s, "clearWindow")) {
        *out = ah_bind(vm, self, ah_native(vm, s, s[5] == 'W' ? nat_clear_window : nat_clear_children, m)); return 1;
    }
    if (!strcmp(s, "parent")) { *out = maybe_module(m, w->parent); return 1; }
    if (!strcmp(s, "first")) { *out = maybe_module(m, w->nkids ? w->kids[0] : NULL); return 1; }
    if (!strcmp(s, "last")) { *out = maybe_module(m, w->nkids ? w->kids[w->nkids - 1] : NULL); return 1; }
    if (!strcmp(s, "next") || !strcmp(s, "prev")) {
        int i = sibling_index(w) + (s[0] == 'n' ? 1 : -1);
        *out = maybe_module(m, w->parent && i >= 0 && i < w->parent->nkids ? w->parent->kids[i] : NULL);
        return 1;
    }
    if (!strcmp(s, "doCopy") || !strcmp(s, "getDeepCopy")) { *out = ah_bind(vm, self, ah_native(vm, s, nat_do_copy, m)); return 1; }   /* memcard.ad FileSelector copies its templates */
    if(!strcmp(s,"insertChild")){*out=ah_bind(vm,self,ah_native(vm,s,nat_insert_child,m));return 1;}
    if (!strcmp(s, "appendChild")) { *out = ah_bind(vm, self, ah_native(vm, s, nat_append_child, m)); return 1; }
    if (!strcmp(s, "interpolateY")) { *out = ah_bind(vm, self, ah_native(vm, s, nat_interpolate_y, m)); return 1; }
    if (!strcmp(s, "movetoLast")) { *out = ah_bind(vm, self, ah_native(vm, s, nat_move_to_last, m)); return 1; }
    if (!strcmp(s, "removeChild")) { *out = ah_bind(vm, self, ah_native(vm, s, nat_remove_child, m)); return 1; }
    if (!strcmp(s, "adjustScale")) { *out = ah_bind(vm, self, ah_native(vm, s, nat_adjust_scale, m)); return 1; }
    if(!strcmp(s,"loadModelOptionMotion")){*out=ah_bind(vm,self,ah_native(vm,s,nat_car_option_motion,m));return 1;}
    if(!strcmp(s,"loadModelSet")){*out=ah_bind(vm,self,ah_native(vm,s,nat_car_crew_load,m));return 1;}
    if(!strcmp(s,"loadModelMotion")){*out=ah_bind(vm,self,ah_native(vm,s,nat_car_crew_motion,m));return 1;}
    if (!strcmp(s,"setModelSetPath")) {*out=ah_bind(vm,self,ah_native(vm,s,nat_model_set_path,m));return 1;}
    if (!strcmp(s, "abbreviate")) { *out = ah_bind(vm, self, ah_native(vm, s, nat_abbreviate, m)); return 1; }
    if(!strcmp(s,"setItemTemplate") || !strcmp(s,"setItemCount") || !strcmp(s,"getItemWidget") || !strcmp(s,"getItemVisible") ||
       !strcmp(s,"setItemUpdate") || !strcmp(s,"setItemWidget") || !strcmp(s,"getItemCount") || !strcmp(s,"postRedraw")) {
        intptr_t op=!strcmp(s,"setItemTemplate")?0:!strcmp(s,"setItemCount")?1:!strcmp(s,"getItemWidget")?2:!strcmp(s,"getItemVisible")?3:
                    !strcmp(s,"setItemUpdate")?4:!strcmp(s,"setItemWidget")?5:!strcmp(s,"getItemCount")?6:7;
        *out=ah_bind(vm,self,ah_native(vm,s,nat_items,(void *)op));return 1;
    }
    if(!strcmp(s,"adjustSize")){*out=ah_bind(vm,self,ah_native(vm,s,nat_adjust_size,m));return 1;}
    /* Texture decode is lazy; initialization needs no extra work. */
    if (!strcmp(s, "doInitialize")) {
        *out = ah_bind(vm, self, ah_native(vm, s, nat_noop, m));
        return 1;
    }
    ui_widget *child = ui_widget_child(w, s);       /* children by name (copies) */
    if (child) { *out = ah_objv(widget_module(m, child)); return 1; }
    const mp_node *prop = mp_prop(w->src, s);     /* other numbers from the screen description */
    if (prop && prop->kind == MP_PROPERTY && prop->count == 1 && prop->items[0]->kind == MP_NUMBER) {
        *out = ah_float(prop->items[0]->number);
        return 1;
    }
    return 0;
}

static int nat_car_value(ah_vm *vm,ah_obj *self,void *d,int argc,const ah_val *argv,ah_val *ret)
{
    (void)argc;(void)argv;
    if(!d){if(!ah_get_member(vm,self,"code",ret))*ret=ah_nil();}
    else *ret=ah_str(vm,"--"); /* specifications not decoded yet */
    return 0;
}

static int widget_set(ah_vm *vm, ah_obj *self, const char *s, ah_val v)
{
    ui_widget *w = ah_host_data(self);
    if (!w) return 0;
    if(!strcmp(w->type,"GraphFace")) {
        if(!strcmp(s,"points")){ah_obj *points=ah_as_obj(v,AO_ARRAY);int n=ah_array_size(points);
            if(n<0 || n>256)return 1;
            float (*pairs)[2]=n?malloc((size_t)n*sizeof *pairs):NULL;if(n && !pairs)return 1;
            for(int i=0;i<n;i++){ah_obj *xy=ah_as_obj(ah_array_get(points,i),AO_ARRAY);
                if(ah_array_size(xy)!=2){free(pairs);return 1;}
                for(int j=0;j<2;j++){double f=ah_num(ah_array_get(xy,j));if(!isfinite(f)){free(pairs);return 1;}pairs[i][j]=(float)f;}}
            free(w->graph_points);w->graph_points=pairs;
            /* 0029EE30 / 0029F798: count C0 and float pairs at C4. */
            w->graph_count=n;return 1;}
        if(!strcmp(s,"line_width")){w->graph_line_width=(float)ah_num(v);return 1;}
        if(!strcmp(s,"type")){const char *kind=ah_cstr(v);w->graph_segments=kind && !strcmp(kind,"lines");w->graph_type_set=1;return 1;}
    }
    if(!strcmp(w->type,"ScaleBar")) {
        float *field=!strcmp(s,"value")?&w->photo_value:!strcmp(s,"min")?&w->photo_min:
            !strcmp(s,"max")?&w->photo_max:!strcmp(s,"step")?&w->photo_step:
            !strcmp(s,"current_value")?&w->photo_current:!strcmp(s,"repeat_start")?&w->photo_repeat:NULL;
        if(field){if(ah_is_num(v) && isfinite(ah_num(v))) {
            float n=(float)ah_num(v);
            /* 00239020 +0..24, clamp C8/CC before storing C4. */
            *field=field==&w->photo_value?fminf(w->photo_max,fmaxf(w->photo_min,n)):n;
        }return 1;}
        /* The project binds the child Composite named popup here.
           It is a widget reference, not a request to open the control. */
        if(!strcmp(s,"popup") && !ah_as_obj(v,AO_MODULE)){w->option_open=ah_truthy(v);return 1;}
    }
    if(!strcmp(w->type,"SliderBar") && !strcmp(s,"value")) {
        assign_index(w,to_int(v));return 1;
    }
    if(!strcmp(w->type,"SliderBar") || !strcmp(w->type,"ScaleBar")) {
        /* setting.ad initSlider L142 / initSliderDisplay L166..167. */
        if(!strcmp(s,"range")){ah_obj *a=ah_as_obj(v,AO_ARRAY);if(ah_array_size(a)==2){
            w->slider_min=to_int(ah_array_get(a,0));w->slider_max=to_int(ah_array_get(a,1));w->slider_range_set=1;w->slider_display_set=0;}return 1;}
        if(!strcmp(s,"display_min") || !strcmp(s,"display_max")){
            int maximum=strstr(s,"max")!=NULL;w->slider_display_set|=1<<maximum;
            if(maximum)w->slider_display_max=ah_num(v);else w->slider_display_min=ah_num(v);return 1;}
    }
    if(!strcmp(w->type,"PhotoMapWindow")) {
        gt4_photo_mode *p=&((menu *)ah_user(vm))->photo;
        /* MPhotoMapWindow registrations 001965A0; original setters retain
           these values at +F4/+114..130 without host-side tuning/clamps. */
        float *field=!strcmp(s,"wheel_angle")?&p->wheel_angle:!strcmp(s,"lightness")?&p->lightness:
            !strcmp(s,"brightness")?&p->brightness:!strcmp(s,"contrast")?&p->contrast:
            !strcmp(s,"view_angle")?&p->view_angle:!strcmp(s,"aperture")?&p->aperture:NULL;
        if(field){if(isfinite(ah_num(v)))*field=(float)ah_num(v);return 1;}
        if(!strcmp(s,"operate_mode") || !strcmp(s,"color_filter")){
            const char *value=ah_cstr(v);if(value)snprintf(!strcmp(s,"operate_mode")?p->operate_mode:p->color_filter,24,"%s",value);return 1;
        }
        if(!strcmp(s,"whiteBalance")){p->white_balance=(int)ah_num(v);return 1;}
        if(!strcmp(s,"stand")){p->stand=ah_truthy(v);return 1;}
    }
    if(!strcmp(w->type,"SlideShowFace") && !strcmp(s,"shuffle")) {((menu *)ah_user(vm))->album_shuffle=ah_truthy(v);return 1;}
    if(!strcmp(w->type,"PhotoRenderFace")) {
        gt4_photo_mode *p=&((menu *)ah_user(vm))->photo;
        float *field=!strcmp(s,"imageScale")?&p->image_scale:!strcmp(s,"ofs_x")?&p->image_x:!strcmp(s,"ofs_y")?&p->image_y:NULL;
        if(field){if(isfinite(ah_num(v)))*field=(float)ah_num(v);return 1;}
    }
    if(!strcmp(s,"color") || !strcmp(s,"shadow_color")){
        const char *keys[]={"r","g","b","a"};ah_obj *color=ah_as_obj(v,AO_MODULE);uint8_t rgba[4];
        if(!color)return 1;
        for(int i=0;i<4;i++){ah_val component;if(!ah_get_member(vm,color,keys[i],&component) || !ah_is_num(component) || !isfinite(ah_num(component)))return 1;rgba[i]=(uint8_t)lround(fmin(1,fmax(0,ah_num(component)))*255);}
        int shadow=!strcmp(s,"shadow_color");memcpy(shadow?w->runtime_shadow:w->runtime_color,rgba,4);
        if(shadow)w->has_runtime_shadow=1;else w->has_runtime_color=1;return 1;
    }
    if(!strcmp(w->type,"ModelFace")) {
        if(!strcmp(s,"target_mode")){w->model_target_mode=ah_num(v)!=0;return 1;}
        if(!strcmp(s,"model_rotate") || !strcmp(s,"rotate_v") || !strcmp(s,"model_rotate_v")) {
            ah_obj *a=ah_as_obj(v,AO_ARRAY);float values[3];
            if(ah_array_size(a)!=3)return 1;
            for(int i=0;i<3;i++){double n=ah_num(ah_array_get(a,i));if(!isfinite(n))return 1;values[i]=(float)n;}
            memcpy(!strcmp(s,"model_rotate")?w->model_angles:w->model_velocity,values,sizeof values);return 1;
        }
    }
    if(!strcmp(s,"action_type") && !strcmp(w->type,"CarFace")) {
        const char *t=ah_cstr(v);w->car_normal=t && !strcmp(t,"normal");
        return 0; /* the module keeps the value for the scripts */
    }
    if(!strcmp(s,"wing") && !strcmp(w->type,"CarFace")){
        const char *label=ah_cstr(v);free(w->car_wing);/* WingShopRoot.tryon L12436 passes default to restore the stock model. */
        w->car_wing=label && *label && strcmp(label,"default")?strdup(label):NULL;return 0;
    }
    if(!strcmp(s,"wheel") && !strcmp(w->type,"CarFace")){
        const char *label=ah_cstr(v);free(w->car_wheel);
        w->car_wheel=label && *label?strdup(label):NULL;return 0;
    }
    if(!strcmp(s,"car_garage") && !strcmp(w->type,"CarFace")){
        const char *code=car_label_of(vm,v);
        widget_set(vm,self,"car_code",code?ah_str(vm,code):ah_nil());
        ah_obj *car=ah_as_obj(v,AO_MODULE);ah_val color;
        /* 00440938 reads live +0x470 (serialized +0x468), normal tyres.
           Re-entering a page must show the owned wheel, not only try-on. */
        uint8_t body[0x4b8];
        if(car && gt4_game_car_body(vm,car,body)){
            uint64_t wheel;memcpy(&wheel,body+0x468,8);
            if(wheel!=UINT64_MAX){menu *m=ah_user(vm);size_t bytes=0;
                uint8_t *labels=gt4_vol_read(m->vol,"/specdb/GT4_US2560/WHEEL.idi",&bytes);
                const char *label=gt4_specdb_label_for_id(labels,bytes,(uint32_t)wheel);
                if(label)w->car_wheel=strdup(label);free(labels);
            }
            uint32_t slot;memcpy(&slot,body+0x488,4);
            if(slot<3){uint64_t wing;memcpy(&wing,body+slot*0x178+0x100,8);
                if(wing!=UINT64_MAX){menu *m=ah_user(vm);size_t bytes=0;
                    uint8_t *labels=gt4_vol_read(m->vol,"/specdb/GT4_US2560/WING.idi",&bytes);
                    const char *label=gt4_specdb_label_for_id(labels,bytes,(uint32_t)wing);
                    if(label)w->car_wing=strdup(label);free(labels);
                }
            }
        }
        if(car && ah_get_member(vm,car,"color_index",&color) && ah_is_num(color))
            ((menu *)ah_user(vm))->car_color=w->car_color_index=(int)ah_num(color);
        return 0; /* scripts retain the garage object, not just its label */
    }
    if(!strcmp(s,"car_code") && !strcmp(w->type,"CarFace")) {
        menu *m=ah_user(vm);const char *code=ah_cstr(v),*name=car_name(m,code);
        free(w->car_code);w->car_code=code && *code?strdup(code):NULL;
        w->car_crew_data=NULL;w->car_crew_size=0;w->car_crew_motion=NULL;w->car_crew_time=0;w->car_crew_playing=0;
        free(w->car_models);w->car_models=NULL;w->car_model_count=0;
        free(w->car_motions);w->car_motions=NULL;w->car_motion_count=0;w->car_service_geometry_valid=0;
        free(w->car_wheel);w->car_wheel=NULL;free(w->car_wing);w->car_wing=NULL;
        m->car_color=w->car_color_index=-1;   /* a new car shows its default paint (PCSX2) */
        ah_obj *info=ah_module_new(vm,"car_data",NULL,NULL,NULL);
        ah_set_member(vm,info,"name",ah_str(vm,name?name:""));
        ah_set_member(vm,info,"code",v);
        ah_set_member(vm,info,"getCarLabel",ah_bind(vm,info,ah_native(vm,"getCarLabel",nat_car_value,NULL)));
        static const char *const specs[]={"getDTrainType","getCatPs","getCatTq"};
        for(int i=0;i<3;i++)ah_set_member(vm,info,specs[i],ah_native(vm,specs[i],nat_car_value,(void *)1));
        bind_garage_values(vm,info);
        ah_set_member(vm,info,"getColors",ah_bind(vm,info,ah_native(vm,"getColors",nat_car_paints,NULL)));
        ah_set_member(vm,info,"getColorName",ah_bind(vm,info,ah_native(vm,"getColorName",nat_car_paints,(void *)1)));
        ah_set_member(vm,self,"car_data",ah_objv(info));
        return 0; /* retain the assigned code in the module */
    }
    if(!strcmp(s,"name")){
        /* Binding a child module named "name" is not a widget rename. */
        if(ah_as_obj(v,AO_MODULE))return 0;
        const char *name=ah_cstr(v);w->name=ah_sym(vm,name?name:"");return 1;
    }
    if (!strcmp(w->type, "InputTextFace") && !strcmp(s, "value")) {   /* the entry's text, the caret after it */
        char buf[128];
        free(w->text);
        w->text = strdup(v.t == AV_NIL ? "" : ah_to_text(v, buf, sizeof buf));
        w->caret = w->text ? (int)strlen(w->text) : 0;
        return 1;
    }
    if (!strcmp(w->type, "ToolTipFace") && !strcmp(s, "value")) {
        char buf[64];
        ui_set_tooltips(w, v.t == AV_NIL ? NULL : ah_to_text(v, buf, sizeof buf));
        return 1;
    }
    if (!strcmp(s, "text") || !strcmp(s, "key")) {
        char buf[64];
        char **dst = !strcmp(s, "text") ? &w->text : &w->text_key;
        char *copy = strdup(v.t == AV_NIL ? "" : ah_to_text(v, buf, sizeof buf));
        /* The header's car name: "--" when none. Its balance comes already
           formatted from main::game.money (gtmode_game.c game_read). */
        if (!strcmp(s,"text") && w->name && !strcmp(w->name,"label") &&
            w->parent && w->parent->parent && w->parent->parent->name &&
            !strcmp(w->parent->parent->name,"Header") &&
            w->parent->name && !strcmp(w->parent->name,"Car") && !*copy) {
            free(copy);copy=strdup("--");
        }
        free(*dst); *dst = copy;
        if (!strcmp(s, "key")) { free(w->text); w->text = NULL; }
        return 1;
    }
    if (!strcmp(s, "visible")) { w->visible = ah_truthy(v); return 1; }
    if (!strcmp(s,"align")) { int align=(int)ah_num(v);if(align>=0 && align<=8){w->runtime_align=align;w->has_runtime_align=1;}return 1; }
    if (!strcmp(s, "x")) { w->x = (float)ah_num(v); return 1; }
    if (!strcmp(s, "y")) { w->y = (float)ah_num(v); return 1; }
    if (!strcmp(s, "w")) {
        float next=(float)ah_num(v);
        if(w->nkids==1 && !strcmp(w->kids[0]->type,"ImageFace") && w->w>0)w->kids[0]->w*=next/w->w;
        w->w=next;w->has_geometry=1;return 1;
    }
    if (!strcmp(s, "h")) {
        float next=(float)ah_num(v);
        if(w->nkids==1 && !strcmp(w->kids[0]->type,"ImageFace") && w->h>0)w->kids[0]->h*=next/w->h;
        w->h=next;w->has_geometry=1;return 1;
    }
    if (!strcmp(s, "opacity")) { w->opacity = (float)ah_num(v); return 1; }
    if (!strcmp(s, "scale_x")) { w->scale_x = (float)ah_num(v); return 1; }
    if (!strcmp(s, "scale_y")) { w->scale_y = (float)ah_num(v); return 1; }
    if (!strcmp(s, "letter_spacing")) { double n=ah_num(v); if(isfinite(n) && n>=0 && n<=32) w->letter_spacing=(float)n; return 1; }
    if (!strcmp(s, "focus_index") || !strcmp(s, "index")) { assign_index(w,(int)ah_num(v)); return 1; }
    if (!strcmp(s, "insensitive")) { w->insensitive = ah_truthy(v); return 1; }
    if (!strcmp(s, "focus_widget")) {
        menu *m = ah_user(vm);
        ah_obj *o = ah_as_obj(v, AO_MODULE);
        if (o && ah_host_of(o) == &widget_host) set_focus(m, ah_host_data(o));
        return 1;
    }
    if (!strcmp(s, "active")) { w->active = ah_truthy(v); return 1; }
    if (!strcmp(s,"movie_name")) {
        const char *name=ah_cstr(v);free(w->movie_name);w->movie_name=name?strdup(name):NULL;return 1;
    }
    if (!strcmp(s,"audio_track")) {w->movie_audio_track=(int)ah_num(v);return 1;}
    if (!strcmp(s,"flash_path")) {
        const char *path=ah_cstr(v);free(w->flash_path);w->flash_path=path?strdup(path):NULL;w->flash_time=0;return 1;
    }
    if (!strcmp(s, "image_chunk")) {   /* labomode.ad update_widget: a file's picture (getIconTexture) */
        char buf[128];
        ui_widget_set_image(w, ah_to_text(v, buf, sizeof buf));
        return 1;
    }
    if (!strcmp(s, "image_path")) {
        char buf[64];
        ui_widget_set_image(w, ah_to_text(v, buf, sizeof buf));
        (void)vm;
        return 1;
    }
    return 0;
}

static void widget_mark(ah_vm *vm, ah_obj *self)
{
    ui_widget *w = ah_host_data(self);
    if (!w) return;
    if(w->item_template && w->item_template->script)ah_mark(vm,ah_objv(w->item_template->script));
    for (int i = 0; i < w->nkids; i++)
        if (w->kids[i]->script) ah_mark(vm, ah_objv(w->kids[i]->script));
}

static void widget_finalize(ah_obj *self)
{
    ui_widget *w = ah_host_data(self);
    if (w && w->script == self) w->script = NULL;
}

static const ah_host widget_host = { "widget", widget_get, widget_set, widget_mark, widget_finalize };

static ah_obj *widget_module(menu *m, ui_widget *w)
{
    if (w->script) return w->script;
    ah_obj *parent = w->parent ? widget_module(m, w->parent) : m->project;
    ah_obj *o = ah_module_new(m->vm, w->name ? w->name : w->type, parent, &widget_host, w);
    w->script = o;
    return o;
}

/* The screens' widgets are modules nested by name, as the scripts expect
   (ArcadeProject::TopRoot::StartScroll::SelectBox). */
static void bind_tree(menu *m, ui_widget *w)
{
    ah_obj *o = widget_module(m, w);
    ah_obj *parent = w->parent ? widget_module(m, w->parent) : m->project;
    if (w->name) ah_set_member(m->vm, parent, w->name, ah_objv(o));
    for (int i = 0; i < w->nkids; i++) bind_tree(m, w->kids[i]);
}

static int widget_color(ah_vm *vm,ah_obj *self,void *data,int argc,const ah_val *argv,ah_val *ret)
{
    (void)data;
    ui_widget *w=ah_host_data(self);
    ah_obj *list=argc==1?ah_as_obj(argv[0],AO_ARRAY):NULL;
    if(argc!=1)return ah_error(vm,"setColor requires one color");
    if(list && w && w->type && !strcmp(w->type,"ColorChipFace")) {
        /* the quick menus' chips: MQuickWork.getColorChipInfo's colours,
           the used ones (the paint's flag bit 0) in bands, the first's
           metallic bit (1) for the sheen */
        w->chip_band_count=0;w->chip_kind=0;
        for(int i=0;i<ah_array_size(list) && i<4;i++) {
            ah_val c=ah_array_get(list,i),r,g,b,a;ah_obj *o=ah_as_obj(c,AO_MODULE);
            if(!o || !ah_get_member(vm,o,"r",&r) || !ah_get_member(vm,o,"g",&g) || !ah_get_member(vm,o,"b",&b) || !ah_get_member(vm,o,"a",&a))continue;
            int flag=(int)(ah_num(a)*255+.5);
            if(!(flag&1))continue;
            if(!w->chip_band_count)w->chip_kind=flag&2?1:2;
            w->chip_bands[w->chip_band_count][0]=(uint8_t)(ah_num(r)*255+.5);
            w->chip_bands[w->chip_band_count][1]=(uint8_t)(ah_num(g)*255+.5);
            w->chip_bands[w->chip_band_count][2]=(uint8_t)(ah_num(b)*255+.5);
            if(w->chip_band_count==0)memcpy(w->chip_rgb,w->chip_bands[0],3);
            w->chip_band_count++;
        }
        *ret=ah_nil();return 0;
    }
    widget_set(vm,self,"color",argv[0]);*ret=ah_nil();return 0;
}

/* ---------------------------------------------------------------- engine objects */

/* Native actor state is owned by the VM object; the menu list is weak. */
struct menu_actor {
    menu_actor *next;
    menu *owner;
    ah_obj *target;
    int kind, out, repeat, running;
    float period, ratio, phase, over, dwell, lo[3], hi[3], value[3]; /* [2]: MMoveActor magnification */
    ah_obj *anchor;              /* MAnchorActor: widget whose movement is followed */
    float base[2], anchor_base[2];
    int follow[2];
    double chase_remainder, actor_ticks;
    float chase_step, chase_range[2];
    int chase_angle;
};

static void actor_apply(menu_actor *a)
{
    ui_widget *w = ah_host_data(a->target);
    if (a->kind == 0 || a->kind == 2) w->opacity = a->value[0];
    else if (a->kind == 1) { w->x = a->value[0]; w->y = a->value[1]; w->magnify = a->value[2]; }
    else if (a->kind == 4) w->rotation = a->value[0];
    else if (a->kind == 3) { w->scale_x = a->value[0]; w->scale_y = a->value[1]; }
}

static int actor_method(ah_vm *vm, ah_obj *self, void *data, int argc, const ah_val *argv, ah_val *ret)
{
    (void)vm; (void)argc; (void)argv;
    menu_actor *a = ah_host_data(self);
    const char *method = data;
    if (a->kind == 6 && (!strcmp(method,"doStart") || !strcmp(method,"doStop"))) {
        a->out=!strcmp(method,"doStop");
        a->chase_angle=0; a->chase_remainder=0;
        /* CORE 0x002841B0 resets phase; stopped updates restore the baseline. */
        if(a->out) { ui_widget *w=ah_host_data(a->target); w->x=a->base[0]; w->y=a->base[1]; }
        a->running=1; *ret=ah_nil(); return 0;
    }
    if (!strcmp(method, "warp")) {
        for (int i = 0; i < 3; i++) a->value[i] = a->out ? a->lo[i] : a->hi[i];
        a->dwell=a->over;actor_apply(a);
    } else if (!strcmp(method, "stop")) a->running = 0;
    else {
        a->running = 1; a->phase = 0;
        if (strcmp(method, "start")) {
            for (int i = 0; i < 3; i++) a->value[i] = a->out ? a->hi[i] : a->lo[i];
            actor_apply(a);
        }
    }
    *ret = ah_nil(); return 0;
}
static int actor_get(ah_vm *vm, ah_obj *self, const char *key, ah_val *out)
{
    menu_actor *a = ah_host_data(self);
    if(a->kind==6) {
        if(!strcmp(key,"doStart") || !strcmp(key,"doStop")) {
            *out=ah_bind(vm,self,ah_native(vm,key,actor_method,(void *)ah_sym(vm,key)));return 1;
        }
        if(!strcmp(key,"floating_range_x")){*out=ah_float(a->chase_range[0]);return 1;}
        if(!strcmp(key,"floating_range_y")){*out=ah_float(a->chase_range[1]);return 1;}
        if(!strcmp(key,"period")){*out=ah_float(360.f/(60*a->chase_step));return 1;}
    }
    if(a->kind==1 && (!strcmp(key,"destinationX") || !strcmp(key,"destinationY"))) {
        *out=ah_float(a->lo[!strcmp(key,"destinationY")]);return 1;
    }
    if (!strcmp(key,"out")) *out = ah_bool(a->out);
    else if (!strcmp(key,"repeat")) *out = ah_bool(a->repeat);
    else if (!strcmp(key,"period")) *out = ah_float(a->period);
    else if (!strcmp(key,"ratio")) *out = ah_float(a->ratio);
    else if (!strcmp(key,"warp") || !strcmp(key,"reset") || !strcmp(key,"start") ||
             !strcmp(key,"stop") || !strcmp(key,"restart"))
        *out = ah_bind(vm,self,ah_native(vm,key,actor_method,(void *)ah_sym(vm,key)));
    else return 0;
    return 1;
}
static int actor_set(ah_vm *vm, ah_obj *self, const char *key, ah_val v)
{
    (void)vm;
    menu_actor *a = ah_host_data(self);
    if(a->kind==6) {
        double n=ah_num(v);
        if(!strcmp(key,"floating_range_x") || !strcmp(key,"floating_range_y")) {
            if(isfinite(n))a->chase_range[!strcmp(key,"floating_range_y")]=(float)n;
            return 1;
        }
        if(!strcmp(key,"period")) {
            float step=(float)(360/(60*n));
            if(isfinite(n) && n>0 && isfinite(step) && step>0)a->chase_step=step;
            return 1;
        }
    }
    if(a->kind==1 && (!strcmp(key,"destinationX") || !strcmp(key,"destinationY"))) {
        double value=ah_num(v);
        if(isfinite(value))a->lo[!strcmp(key,"destinationY")]=(float)value;
        return 1;
    }
    if (!strcmp(key,"out")) {
        a->out = ah_truthy(v); a->running = 1;
        if(a->kind==1 && !a->out)ui_cursor_hold(a->owner->ui,0);
        /* Closing a modal moves its panel, not the context cursor. Keep the
           selected screen position until closePage restores the underlying page. */
        menu *m=a->owner;
        if(a->kind==1 && !m->initializing && a->out && m->depth>1 && (a->lo[1]>=480 || a->lo[1]<0)) {
            ui_widget *panel=ah_host_data(a->target);
            for(ui_widget *focus=m->focus[menu_active_root(m)];focus;focus=focus->parent)
                if(focus==panel){ui_cursor_hold(m->ui,1);break;}
        }
    }
    else if (!strcmp(key,"destination") && a->kind==4) {
        float goal=(float)ah_num(v);if(!isfinite(goal))return 1;
        float delta=fmodf(goal-a->value[0],360);
        if(a->repeat && delta<0)delta+=360;
        if(!a->repeat && delta>0)delta-=360;
        a->hi[0]=a->value[0]+delta;a->out=0;a->running=1;
    }
    else if (!strcmp(key,"over")) {double n=ah_num(v);if(isfinite(n) && n>=0 && n<=60)a->over=(float)n;}
    else if (!strcmp(key,"repeat")) a->repeat = ah_truthy(v);
    else if (!strcmp(key,"period")) { double n = ah_num(v); if (isfinite(n) && n > 0) a->period = (float)n; }
    else if (!strcmp(key,"ratio")) { double n = ah_num(v); if (isfinite(n) && n > 0 && n <= 1) a->ratio = (float)n; }
    else return 0;
    return 1;
}
static void actor_mark(ah_vm *vm, ah_obj *self)
{ menu_actor *a = ah_host_data(self); ah_mark(vm, ah_objv(a->target)); if (a->anchor) ah_mark(vm, ah_objv(a->anchor)); }
static void actor_free(ah_obj *self)
{
    menu_actor *a = ah_host_data(self);
    menu_actor **link = &a->owner->actors;
    while (*link && *link != a) link = &(*link)->next;
    if (*link) *link = a->next;
    free(a);
}
static const ah_host actor_host = {"actor", actor_get, actor_set, actor_mark, actor_free};
void menu_native_method(menu *m,ui_widget *w,const char *method,ah_native_fn fn,void *user)
{ if(m && w)ah_set_member(m->vm,widget_module(m,w),method,ah_native(m->vm,method,fn,user)); }
static int nat_page(ah_vm *vm,ah_obj *self,void *d,int argc,const ah_val *argv,ah_val *ret);
void menu_native_page(menu *m,int root)
{
    if(m && root>=0 && root<ui_root_count(m->ui)) {
        if(!strcmp(ah_module_name(m->project),"OptionProject")) {
            ah_val page=maybe_module(m,ui_root_widget(m->ui,root)),result;
            nat_page(m->vm,NULL,(void *)2,1,&page,&result);
        } else {m->pending_kind=2;m->pending_page=root;m->pending_mores=0;}
    }
}
void menu_native_focus(menu *m,ui_widget *w)
{if(m)set_focus(m,w);}
static int nat_actor(ah_vm *vm, ah_obj *self, void *d, int argc, const ah_val *argv, ah_val *ret)
{
    (void)self;
    ah_obj *target = argc ? ah_as_obj(argv[0],AO_MODULE) : NULL;
    if (!target || ah_host_of(target) != &widget_host) return ah_error(vm,"actor requires a widget");
    const char *name = d;
    int kind = !strcmp(name,"MFadeActor") ? 0 : !strcmp(name,"MMoveActor") ? 1 :
               !strcmp(name,"MBlinkActor") ? 2 : !strcmp(name,"MScaleActor") ? 3 : !strcmp(name,"MRotateActor") ? 4 : !strcmp(name,"MAnchorActor") ? 5 : !strcmp(name,"MChaseActor") ? 6 : -1;
    if (kind < 0) { /* Unsupported actor kinds remain explicit in the log. */
        ah_logf(vm,"unsupported actor: %s",name);
        ah_obj *stub=ah_module_new(vm,name,NULL,NULL,NULL);
        const char *methods[]={"warp","reset","start","stop","restart"};
        for(int i=0;i<5;i++) ah_set_member(vm,stub,methods[i],ah_native(vm,methods[i],nat_noop,NULL));
        *ret=ah_objv(stub); return 0;
    }
    menu_actor *a = calloc(1,sizeof *a);
    if (!a) return ah_error(vm,"out of memory");
    menu *m = ah_user(vm); ui_widget *w = ah_host_data(target);
    if(kind==1 && !strcmp(ah_module_name(m->project),"LicenseProject") &&
       ui_widget_child(w,"Info")) {
        /* quick-share/icon.ad L46 replaces space.actor on each entry.
           The old native object loses its reference; our GC-backed actor
           list can retain it until collection. Stop that superseded move
           so it cannot overwrite the new scroll position every frame. */
        for(menu_actor *old=m->actors;old;old=old->next)
            if(old->kind==1 && old->target==target)old->running=0;
    }
    a->owner=m; a->target=target; a->kind=kind; a->running=1; a->period=0.25f; a->ratio=0.3f;
    a->hi[0]=kind==1?w->x:kind==3?w->scale_x:argc>1?(float)ah_num(argv[1]):1;
    a->hi[1]=kind==1?w->y:kind==3?w->scale_y:0;
    a->lo[0]=kind==1||kind==3?(argc>1?(float)ah_num(argv[1]):0):(argc>2?(float)ah_num(argv[2]):0);
    a->lo[1]=argc>2?(float)ah_num(argv[2]):a->lo[0];
    if(kind==4){a->lo[0]=a->hi[0];a->repeat=argc>2?ah_truthy(argv[2]):1;}
    /* MMoveActor's optional 4th argument is the magnification at the "out"
       position (MakerRoot.setMagnifyActor: 0.8/0.5 for the focused logo). */
    a->hi[2]=1; a->lo[2]=kind==1 && argc>3 && ah_num(argv[3])>0 ? (float)ah_num(argv[3]) : 1;
    a->value[0]=a->hi[0]; a->value[1]=a->hi[1]; a->value[2]=a->hi[2];
    if(kind==6) {
        /* CORE 0x00283618: stopped, x range 0, y range 16, 8.5 deg/tick. */
        a->base[0]=w->x; a->base[1]=w->y; a->out=1;
        a->chase_range[1]=16; a->chase_step=8.5f;
    }
    if (kind == 5) {
        ah_obj *anchor = argc > 1 ? ah_as_obj(argv[1], AO_MODULE) : NULL;
        if (!anchor || ah_host_of(anchor) != &widget_host) { free(a); return ah_error(vm, "MAnchorActor requires two widgets"); }
        ui_widget *t = ah_host_data(anchor);
        a->anchor = anchor;
        a->base[0] = w->x; a->base[1] = w->y;
        a->anchor_base[0] = t->x; a->anchor_base[1] = t->y;
        a->follow[0] = argc > 2 && ah_truthy(argv[2]);
        a->follow[1] = argc > 3 && ah_truthy(argv[3]);
    }
    a->next=m->actors; m->actors=a;
    *ret=ah_objv(ah_module_new(vm,name,NULL,&actor_host,a)); return 0;
}

/* A face of a ScrollBox's inc_arrow/dec_arrow composite. */
static int arrow_face(const ui_widget *w, const char *arrow)
{
    return w && w->parent && w->parent->name && !strcmp(w->parent->name, arrow) &&
           w->parent->parent && !strcmp(w->parent->parent->type, "ScrollBox");
}
/* For an inc_arrow face's fade actor: the fade actor on the same face of
   the same ScrollBox's dec_arrow, which leads it. */
static menu_actor *arrow_partner(menu *m, const menu_actor *a)
{
    const ui_widget *w = ah_host_data(a->target);
    if (a->kind != 0 || !arrow_face(w, "inc_arrow") || !w->name) return NULL;
    for (menu_actor *b = m->actors; b; b = b->next) {
        const ui_widget *v = ah_host_data(b->target);
        if (b != a && b->kind == 0 && arrow_face(v, "dec_arrow") && v->parent->parent == w->parent->parent &&
            v->name && !strcmp(v->name, w->name)) return b;
    }
    return NULL;
}
static int arrow_partner_of(menu *m, const menu_actor *lead)
{
    for (menu_actor *a = m->actors; a; a = a->next) if (arrow_partner(m, a) == lead) return 1;
    return 0;
}

float menu_cursor_speed(menu *m)
{
    ah_val value;
    return m && ah_get_member(m->vm,m->context,"cursor_speed_ratio",&value) && ah_is_num(value)
        ? (float)ah_num(value):1.0f;
}

static void menu_animate_frame(menu *m, float seconds)
{
    if (!m || !isfinite(seconds) || seconds <= 0) return;
    if (seconds > 0.1f) seconds=0.1f;
    int cursor_root=menu_active_root(m);
    if(cursor_root>=0)ui_cursor_select(m->ui,m->focus[cursor_root]);
    update_progress(m,ui_root_widget(m->ui,cursor_root));
    ah_val cursor_ratio;
    if(ah_get_member(m->vm,m->context,"cursor_speed_ratio",&cursor_ratio) && ah_is_num(cursor_ratio))
        ui_cursor_speed(m->ui,(float)ah_num(cursor_ratio));
    for (menu_actor *a=m->actors; a; a=a->next) {
        ui_widget *w=ah_host_data(a->target);
        if (!a->running || w->root!=menu_active_root(m)) continue;
        if(a->kind==6) {
            /* CORE 0x00284270 truncates phase to integer degrees each tick.
               Preserve that 60 Hz clock independently of presentation FPS. */
            if(a->out) {w->x=a->base[0];w->y=a->base[1];continue;}
            a->chase_remainder+=seconds*60.0;
            while(a->chase_remainder>=1.0-1e-6) {
                a->chase_remainder-=1;
                a->chase_angle=(int)fmodf(a->chase_angle+a->chase_step,360.f);
            }
            float wave=sinf(a->chase_angle*(3.141592502593994f/180.f));
            w->x=a->base[0]+wave*a->chase_range[0];
            w->y=a->base[1]-fabsf(wave)*a->chase_range[1];
            continue;
        }
        if (a->kind==5) {
            /* MAnchorActor(widget, anchor, follow_x, follow_y): the widget moves
               by as much as the anchor has moved (MakerRoot's nation flags
               scroll with the manufacturer logos). */
            ui_widget *t=a->anchor?ah_host_data(a->anchor):NULL;
            if (t) {
                if (a->follow[0]) w->x=a->base[0]+t->x-a->anchor_base[0];
                if (a->follow[1]) w->y=a->base[1]+t->y-a->anchor_base[1];
            }
            continue;
        }
        /* A widget's fade/blink actor advances with the widget's own update,
           so it holds while the widget isn't shown (e.g. a ScrollBox arrow
           face hidden by its state). */
        unsigned tick=ui_draw_tick(m->ui);
        /* ScrollBox arrows (TopRoot's StartScroll: setFadeActor2 on both
           bga faces): in PCSX2 one is fully shown while the other is gone,
           so inc_arrow's fade is kept the complement of dec_arrow's. */
        menu_actor *lead=arrow_partner(m,a);
        if(lead) {
            a->out=!lead->out;
            for(int i=0;i<3;i++)a->value[i]=a->hi[i]+a->lo[i]-lead->value[i];
            actor_apply(a);
            continue;
        }
        int paired=a->kind==0 && arrow_face(w,"dec_arrow") && arrow_partner_of(m,a);
        if(!paired && (a->kind==0 || a->kind==2) && tick && w->drawn_tick!=tick)continue;
        if(a->kind==0 && a->repeat && a->dwell>0){a->dwell=fmaxf(0,a->dwell-seconds);continue;}
        if (a->kind==2) {
            a->phase=fmodf(a->phase+seconds,2*a->period);
            a->value[0]=a->phase<a->period?a->hi[0]:a->lo[0];
        } else if(a->kind==3 || a->kind==4) {
            /* Scale/rotation actors have their own native implementations;
               retain their existing adapter until those are traced. */
            float blend=1-powf(1-a->ratio,seconds*60);
            for(int i=0;i<3;i++) {
                float goal=a->out?a->lo[i]:a->hi[i];
                a->value[i]+=(goal-a->value[i])*blend;
                if(fabsf(goal-a->value[i])<.001f)a->value[i]=goal;
            }
            if(a->kind!=4 && a->repeat && a->value[0]==(a->out?a->lo[0]:a->hi[0])){a->out=!a->out;a->dwell=a->over;}
        } else {
            /* MMoveActor 002C1960 / MFadeActor 0020C478. Advance
               original frames, without exponential dt resampling or snapping. */
            a->actor_ticks+=(double)seconds*60.0;
            while(a->actor_ticks>=1.0-1e-6) {
                a->actor_ticks-=1;
                for (int i=0;i<3;i++) {
                    float goal=a->out?a->lo[i]:a->hi[i];
                    if (a->kind==1 || a->kind==3 || a->kind==4)
                        a->value[i]=gt4_move_value(a->value[i],goal,a->ratio);
                    else {
                        float step=fabsf(a->hi[i]-a->lo[i])/(a->period*60.0f);
                        if(goal<a->value[i])step=-step;
                        a->value[i]=gt4_fade_value(a->value[i],goal,step);
                    }
                }
                if (a->kind!=4 && a->repeat && a->value[0]==(a->out?a->lo[0]:a->hi[0])) {
                    a->out=!a->out;a->dwell=a->over;
                    if(a->dwell>0)break;
                }
            }
        }
        actor_apply(a);
    }
    ui_animate(m->ui,menu_active_root(m),seconds);
}

static void context_cursor_visibility(menu *m)
{
    ah_val visible;
    /* A modal has its own visible cursor; the underlying context may hide
       its arrow while a dialog owns focus. TopRoot explicitly hides it. */
    int shown=m->depth>1 || !ah_get_member(m->vm,m->context,"cursor_visible",&visible) || ah_truthy(visible);
    ui_cursor_show(m->ui,shown);
}

void menu_animate(menu *m,float seconds)
{
    if(!m)return;
    context_cursor_visibility(m);
    if(!isfinite(seconds) || seconds<=0)return;
    int cursor_root=menu_active_root(m);
    if(cursor_root>=0)ui_cursor_select(m->ui,m->focus[cursor_root]);
    update_progress(m,ui_root_widget(m->ui,cursor_root));
    m->animation_ticks+=(double)fminf(seconds,.1f)*60.0;
    /* A panel and its cursor must advance together for each original frame,
       rather than advancing the panel N times before chasing its final target. */
    while(m->animation_ticks>=1.0-1e-6) {
        m->animation_ticks-=1;
        menu_animate_frame(m,1.0f/60.0f);
    }
}

void menu_set_presenter(menu *m, void (*fn)(menu *,float,void *), void *user)
{ if(m) { m->present_wait=fn; m->present_user=user; } }
static int nat_sync(ah_vm *vm, ah_obj *self, void *data, int argc, const ah_val *argv, ah_val *ret)
{
    (void)vm; (void)self; menu *m=data;
    double seconds=argc?ah_num(argv[0]):1.0/60;
    if (isfinite(seconds) && seconds>=0 && m->present_wait) {
        /* A dialog queues pushPage, starts its incoming actors, then Syncs.
           Commit that push before presenting: waiting until enterEventLoop
           painted the old Purchase page throughout the popup entrance. */
        if(m->pending_kind==1 && !m->initializing)apply_pages(m);
        m->present_wait(m,(float)seconds,m->present_user);
    }
    *ret=ah_nil(); return 0;
}

/* GT Mode requests a single render context. Its pages share the current
   surface and event queue; returning that context also preserves transition
   assignments for later callbacks. Reject additional surfaces explicitly. */
static int nat_render_context(ah_vm *vm,ah_obj *self,void *data,int argc,const ah_val *argv,ah_val *ret)
{
    (void)self;menu *m=ah_user(vm);
    if(argc!=1 || !ah_is_num(argv[0]))return ah_error(vm,"render context requires a numeric count/index");
    double n=ah_num(argv[0]);
    if(data) {
        if(n!=1)return ah_error(vm,"only one UI render context is supported");
        m->render_context_count=1;*ret=ah_nil();
    } else {
        if(n!=0 || !m->render_context_count)return ah_error(vm,"UI render context index out of range");
        *ret=ah_objv(m->context);
    }
    return 0;
}

static int nat_transition_method(ah_vm *vm,ah_obj *self,void *data,int argc,const ah_val *argv,ah_val *ret)
{
    (void)argc;(void)argv;menu *m=ah_user(vm);ah_val value;
    ah_get_member(vm,self,"period",&value);
    float period=(float)ah_num(value);
    if(!isfinite(period) || period<0 || period>2)period=.25f;
    const char *method=data;
    const char *name=ah_module_name(self);
    if(getenv("OPENGT4_TRANSITION_TRACE"))fprintf(stderr,"transition %s/%s %s.%s period %.6f pending %.6f\n",m->project_label,ui_root_name(m->ui,menu_active_root(m)),name,method,period,m->pending_fade_in);
    int kind=name && !strcmp(name,"MCrossTransition")?1:name && !strcmp(name,"MBlockTransition")?2:0;
    if(kind) {
        gt4_transition_motion *motion=ui_transition_state(m->ui);
        if(motion->kind!=kind)gt4_transition_init(motion,kind);
        if(kind==2 && ah_get_member(vm,self,"_block_seed",&value))
            motion->random=(uint32_t)ah_num(value);
        static const char *const fields[]={"range","slope","magnify_ratio","magnify_diff","opacity","type","with_black","columns","rows"};
        for(int k=0;k<9;k++)if(ah_get_member(vm,self,fields[k],&value)) {
            float n=(float)ah_num(value);
            switch(k){case 0:motion->range=n;break;case 1:motion->slope=n;break;
            case 2:motion->magnify_ratio=n;break;case 3:motion->magnify_diff=n;break;
            case 4:motion->black_opacity=n;break;case 5:motion->type=(int)n;break;
            case 6:motion->with_black=ah_truthy(value);break;
            case 7:motion->columns=(int)fminf(32,fmaxf(1,n));break;
            case 8:motion->rows=(int)fminf(24,fmaxf(1,n));break;}
        }
        if(!strcmp(method,"panIn")){
            ui_transition_start(m->ui,kind,1,period);
            if(m->initializing)m->pending_fade_in=-1;
            else ui_transition_reveal(m->ui);
        }
        else if(!strcmp(method,"panOut"))ui_transition_start(m->ui,kind,0,period);
        else if(!strcmp(method,"panOutIn")) {
            ui_transition_start(m->ui,kind,2,period);
            /* 0027B2A8/0028A6B0: outgoing stage signals ready in one frame;
               it does not fade to black before changing pages. */
            if(m->present_wait)m->present_wait(m,1.0f/60,m->present_user);
            else ui_animate(m->ui,menu_active_root(m),1.0f/60);
            m->pending_fade_in=-1;
        } else {
            float remaining=ui_transition_remaining(m->ui);
            if(remaining>0 && m->present_wait)m->present_wait(m,remaining,m->present_user);
        }
        if(kind==2)ah_set_member(vm,self,"_block_seed",ah_int(motion->random));
        *ret=ah_nil();return 0;
    }
    if(!strcmp(method,"panIn")){
        if(m->initializing && period>0){ui_fade(m->ui,1,1,0);m->pending_fade_in=period;}
        else ui_fade(m->ui,1,0,period);
    }
    else if(!strcmp(method,"panOut"))ui_fade(m->ui,0,1,period);
    else if(!strcmp(method,"panOutIn")) {
        if(period==0){ui_fade(m->ui,0,0,0);m->pending_fade_in=0;*ret=ah_nil();return 0;}
        /* MColorTransition 002874F0: period applies to each leg. */
        ui_fade(m->ui,0,1,period);
        while(m->present_wait && ui_fade_remaining(m->ui)>0)
            m->present_wait(m,ui_fade_remaining(m->ui),m->present_user);
        m->pending_fade_in=period;
    } else {
        float remaining=ui_fade_remaining(m->ui);
        while(remaining>0 && m->present_wait) {
            m->present_wait(m,remaining,m->present_user);
            remaining=ui_fade_remaining(m->ui);
        }
    }
    *ret=ah_nil();return 0;
}
static int nat_transition(ah_vm *vm,ah_obj *self,void *data,int argc,const ah_val *argv,ah_val *ret)
{
    (void)self;(void)argc;(void)argv;
    ah_obj *o=ah_module_new(vm,data,NULL,NULL,NULL);
    /* Constructors 00286EA0 (colour), 00289888 (cross). */
    int block=!strcmp(data,"MBlockTransition");
    ah_set_member(vm,o,"period",ah_float(block?.125:!strcmp(data,"MColorTransition")?.25:.5));
    if(block){
        ah_set_member(vm,o,"_block_seed",ah_int(0x12345678));
        ah_set_member(vm,o,"range",ah_float(.125));ah_set_member(vm,o,"slope",ah_float(2));
        ah_set_member(vm,o,"columns",ah_int(32));ah_set_member(vm,o,"rows",ah_int(24));
    } else if(!strcmp(data,"MCrossTransition")) {
        ah_set_member(vm,o,"type",ah_int(1));ah_set_member(vm,o,"with_black",ah_int(1));
        ah_set_member(vm,o,"opacity",ah_float(0.09999999403953552f));
        ah_set_member(vm,o,"magnify_ratio",ah_float(.5));ah_set_member(vm,o,"magnify_diff",ah_float(0));
    }
    static const char *const methods[]={"panIn","panOut","panOutIn","syncIn","syncOut"};
    for(int i=0;i<5;i++)ah_set_member(vm,o,methods[i],ah_bind(vm,o,ah_native(vm,methods[i],nat_transition_method,(void *)methods[i])));
    *ret=ah_objv(o);return 0;
}
static int nat_progress(ah_vm *vm,ah_obj *self,void *data,int argc,const ah_val *argv,ah_val *ret)
{
    (void)self;(void)data;(void)argc;(void)argv;
    ah_obj *o=ah_module_new(vm,"MProgress",NULL,NULL,NULL);
    ah_set_member(vm,o,"value",ah_float(0));*ret=ah_objv(o);return 0;
}

static void update_progress(menu *m,ui_widget *w)
{
    if(!w)return;
    if(!strcmp(w->type,"ProgressFace") && w->script) {
        ah_val progress,value;
        if(ah_get_member(m->vm,w->script,"progress",&progress) &&
           ah_get_member(m->vm,ah_as_obj(progress,AO_MODULE),"value",&value)) {
            double n=ah_num(value);if(isfinite(n))w->progress=(float)fmin(1,fmax(0,n));
        }
    }
    for(int i=0;i<w->nkids;i++)update_progress(m,w->kids[i]);
}

/* Authored RootWindow.bgm and main.sound.keepMusic own the requested group. */
static void request_music(menu *m,const char *name)
{
    if(!name || !strcmp(name,"keep"))return;
    size_t n=strlen(name);if(n>=sizeof m->music_name)return;
    for(size_t i=0;i<n;i++) {
        unsigned char c=(unsigned char)name[i];
        if(!((c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || c=='_'))return;
    }
    for(size_t i=0;i<=n;i++)m->music_name[i]=name[i]>='A' && name[i]<='Z'?name[i]+32:name[i];
    m->music_set=1;
    if(n && m->audio_preparer)m->audio_preparer(m,m->audio_preparer_user);
}
static int nat_music(ah_vm *vm,ah_obj *self,void *data,int argc,const ah_val *argv,ah_val *ret)
{
    (void)self;menu *m=ah_user(vm);
    if(data && m->music_fader)m->music_fader(argc?(float)ah_num(argv[0]):0);
    request_music(m,data?"":argc?ah_cstr(argv[0]):NULL);
    *ret=ah_nil();return 0;
}
const char *menu_music_name(menu *m){return m && m->music_set?m->music_name:NULL;}

/* startStreamEx registration 0023ED48 +0x6F4, implementation 0023E740.
   Keep PCM requests separate from /music MIDI names. quick-arcade.ad L159
   requests pcm/default_quick, track -1, repeat 1 and normalized menu volume. */
static int nat_quick_stream(ah_vm *vm,ah_obj *self,void *data,int argc,const ah_val *argv,ah_val *ret)
{
    (void)self;menu *m=ah_user(vm);
    if(!data){
        const char *device=argc?ah_cstr(argv[0]):NULL,*group=argc>1?ah_cstr(argv[1]):NULL;
        /* gtmode.ad:14542 uses pcm/get_car with this same registration.
           Validate group syntax here; the existing ADS loader resolves it. */
        if(!device || strcmp(device,"pcm") || !group || !*group || strlen(group)>=sizeof m->pcm_group)return ah_error(vm,"unsupported PCM stream");
        for(const unsigned char *p=(const unsigned char *)group;*p;p++)if(!((*p>='a' && *p<='z') || (*p>='0' && *p<='9') || *p=='_'))return ah_error(vm,"invalid PCM group");
        double volume=argc>4?ah_num(argv[4]):120./128.;
        if(!isfinite(volume))return ah_error(vm,"invalid PCM volume");
        snprintf(m->pcm_group,sizeof m->pcm_group,"%s",group);
        m->pcm_sequence=argc>2?to_int(argv[2]):-1;m->pcm_repeat=argc>3?to_int(argv[3]):1;m->pcm_serial++;
        m->pcm_volume=(int)lround(fmin(1,fmax(0,volume))*128);
    } else m->pcm_group[0]=0;
    m->pcm_paused=0;m->pcm_requested=1;
    if(!data && m->audio_preparer)m->audio_preparer(m,m->audio_preparer_user);
    *ret=ah_nil();return 0;
}
/* sound.pauseStream(flag) (slide.ad musiccontrol.h music_player_do_pause /
   do_resume): the stream held where it is, or going on */
static int nat_pcm_pause(ah_vm *vm,ah_obj *self,void *d,int argc,const ah_val *argv,ah_val *ret)
{(void)self;(void)d;menu *m=ah_user(vm);m->pcm_paused=argc && ah_truthy(argv[0]);*ret=ah_nil();return 0;}
int menu_pcm_paused(menu *m){return m && m->pcm_paused;}
void menu_pcm_state(menu *m,int playing){if(m)m->pcm_playing=playing;}
void menu_pcm_parameters(menu *m,int *sequence,int *repeat,unsigned *serial)
{if(sequence)*sequence=m?m->pcm_sequence:-1;if(repeat)*repeat=m?m->pcm_repeat:1;if(serial)*serial=m?m->pcm_serial:0;}
static int nat_pcm_playing(ah_vm *vm,ah_obj *self,void *d,int argc,const ah_val *argv,ah_val *ret)
{(void)self;(void)d;(void)argc;(void)argv;*ret=ah_int(((menu *)ah_user(vm))->pcm_playing);return 0;}
const char *menu_pcm_request(menu *m,int *volume)
{
    if(volume)*volume=m?m->pcm_volume:0;
    return m && m->pcm_requested?m->pcm_group:NULL;
}

static int nat_sound_load(ah_vm *vm,ah_obj *self,void *data,int argc,const ah_val *argv,ah_val *ret){
    (void)vm;(void)self;(void)argc;(void)argv;menu *m=data;if(!m->sound)m->sound=gt4_sound_open(m->vol);*ret=ah_nil();return 0;
}
static int nat_sound_unload(ah_vm *vm,ah_obj *self,void *data,int argc,const ah_val *argv,ah_val *ret){
    (void)vm;(void)self;(void)argc;(void)argv;menu *m=data;gt4_sound_close(m->sound);m->sound=NULL;*ret=ah_nil();return 0;
}
static int nat_sound_play(ah_vm *vm, ah_obj *self, void *data, int argc, const ah_val *argv, ah_val *ret)
{
    (void)self; menu *m=data;
    if (argc && ah_cstr(argv[0])) {
        if (!m->sound) m->sound=gt4_sound_open(m->vol);
        gt4_sound_gain(m->sound,(float)(menu_option_number(m,"menu_se_volume_raw",120)/120.0));
        if (m->sound && !gt4_sound_play(m->sound,ah_cstr(argv[0])))
            ah_logf(vm,"unsupported sound: %s",ah_cstr(argv[0]));
    }
    *ret=ah_nil(); return 0;
}

void menu_play_effect(menu *m,const char *name)
{if(m){ah_val sound=ah_str(m->vm,name),result;nat_sound_play(m->vm,NULL,m,1,&sound,&result);}}

/* MScriptWatcher(context, func): calls func(context) every `interval` frames
   once appended. */
static int nat_watcher_append(ah_vm *vm, ah_obj *self, void *d, int argc, const ah_val *argv, ah_val *ret)
{
    (void)argc; (void)argv;
    menu *m = d;
    *ret = ah_nil();
    for (int i = 0; i < m->nwatchers; i++)
        if (m->watchers[i].obj == self) return 0;
    watcher *grown = realloc(m->watchers, (size_t)(m->nwatchers + 1) * sizeof *grown);
    if (!grown) return ah_error(vm, "out of memory");
    m->watchers = grown;
    m->watchers[m->nwatchers].obj = self;
    m->watchers[m->nwatchers++].count = 0;
    ah_pin(vm, ah_objv(self));
    return 0;
}

static int nat_watcher_remove(ah_vm *vm, ah_obj *self, void *d, int argc, const ah_val *argv, ah_val *ret)
{
    (void)argc; (void)argv;
    menu *m = d;
    *ret = ah_nil();
    for (int i = 0; i < m->nwatchers; i++)
        if (m->watchers[i].obj == self) {
            ah_unpin(vm, ah_objv(self));
            m->watchers[i] = m->watchers[--m->nwatchers];
            break;
        }
    return 0;
}

static int nat_watcher(ah_vm *vm, ah_obj *self, void *d, int argc, const ah_val *argv, ah_val *ret)
{
    (void)self;
    ah_obj *w = ah_module_new(vm, "MScriptWatcher", NULL, NULL, NULL);
    ah_set_member(vm, w, "context", argc > 0 ? argv[0] : ah_nil());
    ah_set_member(vm, w, "func", argc > 1 ? argv[1] : ah_nil());
    ah_set_member(vm, w, "interval", ah_int(1));
    ah_set_member(vm, w, "append", ah_bind(vm, w, ah_native(vm, "append", nat_watcher_append, d)));
    ah_set_member(vm, w, "remove", ah_bind(vm, w, ah_native(vm, "remove", nat_watcher_remove, d)));
    *ret = ah_objv(w);
    return 0;
}

/* Region of the disc: the supported disc is NTSC-U (config/supported_discs.json). */
static int nat_region(ah_vm *vm, ah_obj *self, void *d, int argc, const ah_val *argv, ah_val *ret)
{
    (void)self; (void)d; (void)argc; (void)argv;
    *ret = ah_str(vm, "us");
    return 0;
}
/* 001AC598 returns D_00692E20, "NTSC", on the supported US disc. */
static int nat_video_system(ah_vm *vm,ah_obj *self,void *d,int argc,const ah_val *argv,ah_val *ret)
{(void)self;(void)d;(void)argc;(void)argv;*ret=ah_str(vm,"NTSC");return 0;}

static ah_obj *submodule(ah_vm *vm, ah_obj *parent, const char *name)
{
    ah_obj *o = ah_module_new(vm, name, parent, NULL, NULL);
    ah_set_member(vm, parent, name, ah_objv(o));
    return o;
}

/* Presentation-only virtual loading, varying across 30..60 rendered ticks.
   No blocking 500-ms sleep: the presenter keeps drawing and pumping events. */
float menu_loading_duration(menu *m)
{
    if(!m->loading_seed)m->loading_seed=((uint32_t)time(NULL)^(uint32_t)(uintptr_t)m)|1u;
    uint32_t n=m->loading_seed;
    n^=n<<13;n^=n>>17;n^=n<<5;m->loading_seed=n;
    return (30u+n%31u)/60.0f;
}

/* Deferred page operations avoid entering another script inside a callback. */
static int nat_page(ah_vm *vm, ah_obj *self, void *d, int argc, const ah_val *argv, ah_val *ret)
{
    (void)self;
    menu *m = ah_user(vm);
    ah_obj *o = argc ? ah_as_obj(argv[0], AO_MODULE) : NULL;
    ui_widget *w = o && ah_host_of(o) == &widget_host ? ah_host_data(o) : NULL;
    if (!w || strcmp(w->type, "RootWindow")) return ah_error(vm, "page operation needs a RootWindow");
    int option_switch=(int)(intptr_t)d==2 && menu_active_root(m)>=0 &&
        w->root!=menu_active_root(m) && !strcmp(ah_module_name(m->project),"OptionProject");
    /* Native settings callbacks also use the authored colour transition.
       Scripted start_page already queued its outgoing/incoming legs. */
    if(option_switch && m->pending_fade_in==0) {
        ah_val transition,context=ah_objv(m->context),result;
        if(ah_get_member(vm,m->context,"transition",&transition))
            ah_call_member(vm,ah_as_obj(transition,AO_MODULE),"panOutIn",1,&context,&result);
    }
    if(option_switch)ui_cursor_icon(m->ui,"load");
    /* startPage -> 0022F6A0 pauses context updates around two Sync(2)
       barriers (0022F6E0, 0022F79C). Keep the old cursor/loading sprite
       presented before the deferred teardown; otherwise a cached page
       resets "load" before any frame ever displays it. */
    if((int)(intptr_t)d==2 && menu_active_root(m)>=0 && w->root!=menu_active_root(m) && m->present_wait) {
        ui_widget *leaving=ui_root_widget(m->ui,menu_active_root(m));
        int visible=leaving->visible;
        int paused=ui_cursor_pause(m->ui,1);
        float loading=menu_loading_duration(m);
        m->present_wait(m,2.0f/60.0f,m->present_user);
        /* 00233D18 marks the departed root between the two barriers.
           The retained transition surface and context cursor remain. */
        leaving->visible=0;
        /* The simulation includes the first two native barrier frames. */
        m->present_wait(m,loading-2.0f/60.0f,m->present_user);
        leaving->visible=visible;
        ui_cursor_pause(m->ui,paused);
    }
    if(option_switch)ui_cursor_icon(m->ui,NULL);
    m->pending_kind = (int)(intptr_t)d;
    m->pending_page = w->root;
    /* startPage (0022BC20) pushes each of its pages in order (0022F890):
       arcade CategoryRoot.onCancel's startPage(TopRoot, GameRoot) leaves
       GameRoot on top of TopRoot */
    m->pending_mores = 0;
    for (int i = 1; m->pending_kind == 2 && i < argc && m->pending_mores < 8; i++) {
        ah_obj *more = ah_as_obj(argv[i], AO_MODULE);
        ui_widget *page = more && ah_host_of(more) == &widget_host ? ah_host_data(more) : NULL;
        if (page && !strcmp(page->type, "RootWindow")) m->pending_more[m->pending_mores++] = page->root;
    }
    *ret = ah_nil(); return 0;
}

static int nat_event_new(ah_vm *vm, ah_obj *self, void *d, int argc, const ah_val *argv, ah_val *ret)
{
    (void)self;
    int script = d != NULL;
    if (argc < (script ? 3 : 1)) return ah_error(vm, "event constructor: missing arguments");
    ah_obj *e = ah_module_new(vm, script ? "MScriptEvent" : "MFunctionEvent", NULL, NULL, NULL);
    ah_obj *args = ah_array_new(vm);
    if (script) {
        ah_set_member(vm, e, "target", argv[1]);
        ah_set_member(vm, e, "method", argv[2]);
        ah_array_push(vm, args, argv[0]);
        ah_obj *extra = argc > 3 ? ah_as_obj(argv[3], AO_ARRAY) : NULL;
        for (int i = 0; i < ah_array_size(extra); i++) ah_array_push(vm, args, ah_array_get(extra, i));
    } else {
        ah_set_member(vm, e, "function", argv[0]);
        for (int i = 1; i < argc; i++) ah_array_push(vm, args, argv[i]);
    }
    ah_set_member(vm, e, "arguments", ah_objv(args));
    *ret = ah_objv(e); return 0;
}

/* photo_shoot.ad L219/1279: preserve event payload and defer callbacks
   through the existing script-event queue. A: general event parity pending. */
static int nat_photo_event(ah_vm *vm,ah_obj *self,void *d,int argc,const ah_val *argv,ah_val *ret)
{
    (void)self;int kind=(int)(intptr_t)d;
    if(argc<(kind==2?2:3))return ah_error(vm,"photo event: missing arguments");
    ah_obj *event=ah_module_new(vm,"PhotoEvent",NULL,NULL,NULL),*args=ah_array_new(vm);
    ah_set_member(vm,event,"widget",argv[1]);
    ah_set_member(vm,event,"keysym",kind==2?ah_int(0):argv[2]);
    ah_array_push(vm,args,ah_objv(event));
    if(kind==2)ah_array_push(vm,args,argv[1]);
    ah_val call[]={argv[0],argv[1],ah_str(vm,kind==2?"onActivate":kind==1?"onKeyRelease":"onKeyPress"),ah_objv(args)};
    return nat_event_new(vm,NULL,(void *)1,4,call,ret);
}

static int nat_push_event(ah_vm *vm, ah_obj *self, void *d, int argc, const ah_val *argv, ah_val *ret)
{
    (void)self;
    menu *m = d;
    ah_obj *e = argc ? ah_as_obj(argv[0], AO_MODULE) : NULL;
    if (!e || (strcmp(ah_module_name(e), "MScriptEvent") && strcmp(ah_module_name(e), "MFunctionEvent")))
        return ah_error(vm, "unsupported event type");
    if (m->nevents == 128) return ah_error(vm, "menu event queue is full");
    ah_pin(vm, ah_objv(e));
    m->events[m->nevents++] = e;
    *ret = ah_nil(); return 0;
}

static ah_obj *game_object(menu *m)
{
    ah_val main_, game;
    if (!ah_get_member(m->vm, ah_root(m->vm), "main", &main_) ||
        !ah_get_member(m->vm, ah_as_obj(main_, AO_MODULE), "game", &game)) return NULL;
    return ah_as_obj(game, AO_MODULE);
}

static int nat_finish(ah_vm *vm, ah_obj *self, void *d, int argc, const ah_val *argv, ah_val *ret)
{
    (void)self; (void)argc; (void)argv;
    menu *m = d;
    ah_val sequence = ah_nil(), project = ah_nil(), command = ah_nil();
    /* A quick menu's command (start, replay, ...) is the race sequence's:
       the caller acts on it; leaving (COMMAND_EXIT) goes on to the next
       menu project as below */
    if (m->quick_work && ah_get_member(vm, m->quick_work, "selectedCommand", &command) && ah_cstr(command)
        && *ah_cstr(command) && strcmp(ah_cstr(command), "COMMAND_EXIT")) {
        snprintf(m->quick_command, sizeof m->quick_command, "%s", ah_cstr(command));
        ah_set_member(vm, m->quick_work, "selectedCommand", ah_str(vm, ""));
        ui_fade(m->ui, 0, 0, 0);
        *ret = ah_nil(); return 0;
    }
    album_music_store(m);
    if(!strcmp(ah_module_name(m->project),"SlideProject"))jukebox_store(m);   /* the Jukebox's race playlist kept */
    ah_get_member(vm, game_object(m), "next_sequence", &sequence);
    ah_get_member(vm, game_object(m), "next_menu_project", &project);
    const char *s = ah_cstr(sequence), *p = ah_cstr(project);
    if(!strcmp(ah_module_name(m->project),"SettingProject")){
        /* HomeRoot L4127..4133 and SettingRoot L843 retain major project/page. */
        ah_val major=ah_nil(),page=ah_nil();ah_get_member(vm,game_object(m),"major_menu_project",&major);
        ah_get_member(vm,game_object(m),"major_menu_page",&page);
        const char *origin=ah_cstr(major),*root=ah_cstr(page);
        if(!origin || !*origin)return ah_error(vm,"missing setting return project");
        snprintf(m->next_project,sizeof m->next_project,"%s%s%s",origin,root && *root?":":"",root?root:"");
        *ret=ah_nil();return 0;
    }
    if(!strcmp(ah_module_name(m->project),"PhotoShootProject")) {
        /* photo_shoot.ad L55, gtmode.ad L13591..13594: return to the
           selecting page, not a new race or the GT Mode root. */
        snprintf(m->next_project,sizeof m->next_project,"gtmode:PhotoTravelRoot");
        *ret=ah_nil();return 0;
    }
    if(!strcmp(ah_module_name(m->project),"PhotoSaveProject")) {
        /* photo_save.ad PreviewRoot::onCancel L63 finishes its caller's
           photo sequence. Native cached projects return through origin. */
        ah_val origin=ah_nil();ah_get_member(vm,game_object(m),"last_menu_project",&origin);
        const char *caller=ah_cstr(origin);
        if(caller && *caller){snprintf(m->next_project,sizeof m->next_project,"%s",caller);*ret=ah_nil();return 0;}
    }
    if(s && !strcmp(s,"setting")) {
        /* gtmode.ad: GarageRoot calls start_sequence(context,"setting"). */
        snprintf(m->next_project,sizeof m->next_project,"setting");
        *ret=ah_nil();return 0;
    }
    if(s && !strcmp(s,"photo_shoot")) {
        /* gtmode.ad L13585..13596: selected course, garage car, capture
           loading mode, then start_sequence(context,"photo_shoot"). */
        snprintf(m->next_project,sizeof m->next_project,"photo_shoot");
        *ret=ah_nil();return 0;
    }
    if (s && (!strcmp(s, "single_race") || !strcmp(s, "time_trial") || !strcmp(s,"freerun") || !strcmp(s,"practice") || !strcmp(s,"photo_drive") || !strcmp(s,"machinetest") ||
              !strcmp(s,"solo_event") || !strcmp(s,"championship") || !strcmp(s,"license") ||
              !strcmp(s,"mission") || !strcmp(s,"split_battle"))) {   /* gtmode.ad MissionRoot: exit_menu("mission") -> 0011BB60 */
        /* The race itself is the platform's (race view); the menu comes
           back from black when it returns. */
        m->race_requested = 1; ui_fade(m->ui, 0, 0, 0);
        *ret = ah_nil(); return 0;
    }
    if(mp_string(ui_project_tree(m->ui),"name") && !strcmp(mp_string(ui_project_tree(m->ui),"name"),"LaboModeProject") &&
       (!p || !*p || !strcmp(p,"labomode") || replay_action)) {
        /* labomode's page done (start_page(context, nil)): a replay it
           loaded plays (MMemoryCardManager::setAction(1), the caller's
           menu_take_loaded_replay) and comes back here; the save screen
           goes back where it was opened from */
        ah_set_member(vm,game_object(m),"next_sequence",ah_str(vm,"menu"));
        /* a best lap loaded (MODE_LOAD_BESTLAP_REPLAY) is the Time Trial's
           ghost: back where the screen was opened from, the caller taking
           it (menu_take_loaded_ghost) */
        if(replay_action && strncmp(replay_loaded,"bestlap_",8))snprintf(m->next_project,sizeof m->next_project,"labomode");
        else snprintf(m->next_project,sizeof m->next_project,"%s",memcard_origin[0]?memcard_origin:"arcade");
        *ret=ah_nil();return 0;
    }
    if(s && !strcmp(s,"photo_labo")) {
        /* gtmode.ad HomeRoot Labo L4202..4206: start_sequence("photo_labo")
           loads labomode with next_menu_arg "" (labomode.ad L37: PhotoRoot);
           its exit goes back to major_menu_project. */
        ah_val major;const char *origin=ah_get_member(vm,game_object(m),"major_menu_project",&major) && ah_cstr(major) && *ah_cstr(major)?ah_cstr(major):"gtmode";
        snprintf(memcard_origin,sizeof memcard_origin,"%s",origin);
        ah_set_member(vm,game_object(m),"next_sequence",ah_str(vm,"menu"));
        snprintf(m->labo_arg,sizeof m->labo_arg,"PhotoRoot");
        snprintf(m->next_project,sizeof m->next_project,"labomode");
        *ret=ah_nil();return 0;
    }
    if(s && !strcmp(s,"load_replay")) {
        /* the Replay Theater: labomode's ReplayTheaterRoot (menu_open_labo);
           its exit goes back to major_menu_project */
        ah_val major;const char *origin=ah_get_member(vm,game_object(m),"major_menu_project",&major) && ah_cstr(major) && *ah_cstr(major)?ah_cstr(major):"arcade";
        snprintf(memcard_origin,sizeof memcard_origin,"%s",origin);
        ah_set_member(vm,game_object(m),"next_sequence",ah_str(vm,"menu"));
        ah_set_member(vm,game_object(m),"last_sequence",ah_str(vm,"load_replay"));
        snprintf(m->labo_arg,sizeof m->labo_arg,"ReplayTheaterRoot");
        snprintf(m->next_project,sizeof m->next_project,"labomode");
        *ret=ah_nil();return 0;
    }
    if(attract_left && !strcmp(ah_module_name(m->project),"BootProject") &&
       !strcmp(ui_root_name(m->ui,menu_active_root(m)),"MovieRoot")) {
        menu_attract_finish(m,attract_cancelled);*ret=ah_nil();return 0;
    }
    if(s && !strcmp(s,"autodemo")) {
        /* 004345D8 +0x2C/+0x48, 0015DB40 +0x34: four presentations;
           the first cycle starts interval-1 before its opening movie. */
        if(!attract_origin[0])demo_movie_count=(int)menu_option_number(m,"demo_movie_interval",3)-1;
        snprintf(attract_origin,sizeof attract_origin,"%s:%s",m->project_label,ui_root_name(m->ui,menu_active_root(m)));
        attract_left=4;attract_cancelled=0;attract_next(m);
        *ret=ah_nil();return 0;
    }
    if (s && *s && strcmp(s, "menu")) return ah_error(vm, "sequence '%s' is not implemented", s);
    if (!p || !*p || strlen(p) >= sizeof m->next_project) return ah_error(vm, "missing next menu project");
    snprintf(m->next_project, sizeof m->next_project, "%s", p);
    if(!strcmp(p,"slide")){   /* slide.ad onLoad L25..28: next_menu_arg is its first page (MusicRoot from the GT Mode's MusicTheater) */
        ah_val arg=ah_nil();ah_get_member(vm,game_object(m),"next_menu_arg",&arg);
        if(ah_cstr(arg) && *ah_cstr(arg) && strlen(p)+1+strlen(ah_cstr(arg))<sizeof m->next_project)
            snprintf(m->next_project,sizeof m->next_project,"%s:%s",p,ah_cstr(arg));
    }
    *ret = ah_nil(); return 0;
}

static int nat_unavailable(ah_vm *vm, ah_obj *self, void *d, int argc, const ah_val *argv, ah_val *ret)
{
    (void)self; (void)argc; (void)argv; (void)ret;
    return ah_error(vm, "%s is not implemented", (const char *)d);
}

void menu_set_dialog_presenter(menu *m,int (*fn)(menu *,const char *,const char *,int,const char *,const char *))
{if(m)m->present_dialog=fn;}
void menu_set_progress_presenter(menu *m,void (*fn)(menu *,int,const char *,int,float))
{if(m)m->present_progress=fn;}

static int nat_dialog_open(ah_vm *vm, ah_obj *self, void *d, int argc, const ah_val *argv, ah_val *ret)
{
    (void)self; (void)d;
    /* ConfirmDialog.open(context, type, message, title, ..., yes, no). */
    if (argc < 3) return ah_error(vm, "ConfirmDialog.open: missing message");
    const char *message = ah_cstr(argv[2]), *title = argc > 3 ? ah_cstr(argv[3]) : NULL;
    menu *m=d;
    title=ui_storage_text(title);message=ui_storage_text(message);
    const char *yes=argc>5?ah_cstr(argv[5]):NULL,*no=argc>6?ah_cstr(argv[6]):NULL;
    int result=m && m->present_dialog?m->present_dialog(m,title && *title?title:"Info",message?message:"",(int)ah_num(argv[1]),yes,no):plat_dialog(title && *title?title:"Info",message?message:"",ah_num(argv[1])!=0);
    if (result < 0) return ah_error(vm, "could not display confirmation dialog");
    *ret = ah_int(result); return 0;
}

static int nat_cursor_icon(ah_vm *vm,ah_obj *self,void *data,int argc,const ah_val *argv,ah_val *ret)
{
    (void)self;(void)data;
    menu *m=ah_user(vm);
    ui_cursor_icon(m->ui,argc>1?ah_cstr(argv[1]):NULL);
    *ret=ah_nil();return 0;
}

/* cursor.ad's ProgressDialog (open(context, title, callback): the message,
   the OK button when there is a callback, pushPage; setProgress(p);
   close(context): closePage), shown by the platform over the page */
static int nat_progress_dialog(ah_vm *vm,ah_obj *self,void *d,int argc,const ah_val *argv,ah_val *ret)
{
    (void)self;menu *m=ah_user(vm);const char *op=d;*ret=ah_nil();
    if(!m || !m->present_progress)return 0;
    if(!strcmp(op,"setRawMode"))return 0;   /* logger.ad: A, the dialog's raw mode not read */
    if(!strcmp(op,"open"))m->present_progress(m,1,argc>1 && ah_cstr(argv[1])?ah_cstr(argv[1]):"",argc>2 && argv[2].t!=AV_NIL,-1);
    else if(!strcmp(op,"close"))m->present_progress(m,0,NULL,0,-1);
    else {   /* setProgress: a number, or the manager's progress (its value) */
        ah_val v=argc?argv[argc-1]:ah_nil(),inner;
        if(v.t==AV_OBJ && ah_get_member(vm,ah_as_obj(v,AO_MODULE),"value",&inner))v=inner;
        m->present_progress(m,2,NULL,0,ah_is_num(v)?(float)ah_num(v):-1);
    }
    return 0;
}

static int nat_common_page(ah_vm *vm, ah_obj *self, void *d, int argc, const ah_val *argv, ah_val *ret)
{
    (void)self; (void)d;
    const char *name = argc ? ah_cstr(argv[0]) : NULL;
    if (name && !strcmp(name, "CursorRoot")) {
        ah_obj *cursor = ah_module_new(vm, "CursorRoot", NULL, NULL, NULL);
        ah_set_member(vm, cursor, "setIcon", ah_native(vm, "setIcon", nat_cursor_icon, NULL));
        *ret = ah_objv(cursor); return 0;
    }
    if (name && !strcmp(name, "ConfirmDialog")) {
        ah_obj *dialog = ah_module_new(vm, "ConfirmDialog", NULL, NULL, NULL);
        ah_set_member(vm, dialog, "open", ah_native(vm, "open", nat_dialog_open, d));
        *ret = ah_objv(dialog); return 0;
    }
    if (name && !strcmp(name, "ProgressDialog")) {
        ah_obj *dialog = ah_module_new(vm, "ProgressDialog", NULL, NULL, NULL);
        static const char *const ops[] = {"open", "close", "setProgress", "setRawMode"};
        for (unsigned i = 0; i < sizeof ops / sizeof ops[0]; i++)
            ah_set_member(vm, dialog, ops[i], ah_native(vm, ops[i], nat_progress_dialog, (void *)ops[i]));
        *ret = ah_objv(dialog); return 0;
    }
    return ah_error(vm, "common page '%s' is not implemented", name ? name : "?");
}

typedef struct {const char *name;int valid,type,count;double number[16];char text[64];} saved_option;
static saved_option saved_options[]={
    {"pad_buttons_1p"},{"pad_buttons_2p"},{"pad_analog_1p"},{"pad_analog_2p"},
    {"limit_favorite_car_only"},
    {"limit_favorite_course_only"},
    {"game_zone"},
    {"special_arcade_tuner"},
    {"wide_mode"},
    {"automatic_gear"},
    {"race_display_config"},
    {"race_display_map"},
    {"race_display_steering"},
    {"can_watch_ending"},
    {"sound_type"},
    {"enableAC3"},
    {"timeout_count_to_autodemo"},
    {"replay_display_enable"},
    {"replay_memory_card_slot"},
    {"opening_after_autoload"},
    {"demo_movie_interval"},
    {"auto_save"},
    {"race_laps"},
    {"tire_damage"},
    {"assist_asm"},
    {"assist_tcs"},
    {"arcade_time_limit"},
    {"split_race_laps"},
    {"split_tire_damage"},
    {"split_handicap"},
    {"split_boost_level"},
    {"album_slide_effect"},
    {"album_slide_play_time"},
    {"album_slide_transition_time"},
    {"album_memory_card_slot"},
    {"photo_shutter"},
    {"photo_aspect"},{"photo_professional"},{"photo_manual_focus"},
    {"vibration_1p"},
    {"vibration_2p"},
    {"active_steering_1p"},
    {"active_steering_2p"},
    {"menu_bgm_volume_raw"},
    {"menu_se_volume_raw"},
    {"race_bgm_volume_raw"},
    {"race_bgm"},
    {"race_se_volume_raw"},
    {"replay_bgm_volume_raw"},
    {"replay_bgm"},
    {"replay_se_volume_raw"},
    {"slide_bgm_volume_raw"},
    {"narration"},
    {"replay_mode"},
    {"replay_play_mode"},   /* labomode.ad commandPlayMode: STRAIGHT or SHUFFLE */
    {"penalty_type"},
    {"photo_quality"},
    {"ustorage_photo_quality"},
    {"steering_assist_1p"},
    {"steering_assist_2p"},
    {"unit_velocity"},
    {"unit_power"},
    {"unit_torque"},
    {"race_color_correction"},
    {"replay_color_correction"},
};
static char option_path[1100];
static void options_restore(menu *m);

void menu_options_storage(const char *path)
{
    demo_movie_count=0;attract_index=attract_left=attract_cancelled=0;
    attract_pending[0]=attract_origin[0]=0;
    snprintf(option_path,sizeof option_path,"%s",path?path:"");
    for(unsigned i=0;i<sizeof saved_options/sizeof saved_options[0];i++)saved_options[i].valid=0;
    FILE *f=option_path[0]?fopen(option_path,"r"):NULL;if(!f)return;
    char line[512],key[64],payload[256];int type;
    while(fgets(line,sizeof line,f))if(sscanf(line,"%63s %d %255[^\n]",key,&type,payload)==3) {
        for(unsigned i=0;i<sizeof saved_options/sizeof saved_options[0];i++)if(!strcmp(key,saved_options[i].name)) {
            saved_option *v=&saved_options[i];v->type=type;v->count=0;v->valid=0;
            if(type==1){if(strlen(payload)<sizeof v->text){strcpy(v->text,payload);v->valid=1;}}
            else if(type==0 || type==2){char *at=payload,*end;do{double n=strtod(at,&end);
                if(at==end || !isfinite(n) || fabs(n)>100000){v->count=0;break;}
                v->number[v->count++]=n;at=end;while(*at==' ' || *at==',')at++;
            }while(*at && v->count<16);v->valid=v->count>0 && !*at;}
        }
    }
    fclose(f);
}
static void options_capture(menu *m)
{
    ah_obj *game=game_object(m);ah_val option;
    if(!ah_get_member(m->vm,game,"option",&option))return;
    for(unsigned i=0;i<sizeof saved_options/sizeof saved_options[0];i++) {
        saved_option *v=&saved_options[i];ah_val value;
        if(!ah_get_member(m->vm,ah_as_obj(option,AO_MODULE),v->name,&value))continue;
        const char *str=ah_cstr(value);ah_obj *array=ah_as_obj(value,AO_ARRAY);
        if(str){if(strlen(str)>=sizeof v->text)continue;v->type=1;strcpy(v->text,str);}
        else if(array){int count=ah_array_size(array);if(count<1 || count>16)continue;
            v->type=2;v->count=count;for(int k=0;k<count;k++)v->number[k]=ah_num(ah_array_get(array,k));}
        else if(value.t==AV_INT || value.t==AV_FLOAT || value.t==AV_BOOL){v->type=0;v->count=1;v->number[0]=ah_num(value);}
        else continue;
        v->valid=1;
    }
    if(!option_path[0])return;
    char temp[1120];snprintf(temp,sizeof temp,"%s.tmp",option_path);FILE *f=fopen(temp,"w");if(!f)return;
    int ok=1;
    for(unsigned i=0;i<sizeof saved_options/sizeof saved_options[0];i++) {
        saved_option *v=&saved_options[i];if(!v->valid)continue;
        if(fprintf(f,"%s %d ",v->name,v->type)<0)ok=0;
        if(v->type==1){if(fprintf(f,"%s",v->text)<0)ok=0;}
        else for(int k=0;k<v->count;k++)if(fprintf(f,"%s%.9g",k?",":"",v->number[k])<0)ok=0;
        if(fputc('\n',f)==EOF)ok=0;
    }
    if(fclose(f)!=0)ok=0;
#ifdef _WIN32
    if(ok)ok=MoveFileExA(temp,option_path,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0;
#else
    if(ok)ok=rename(temp,option_path)==0;
#endif
    if(!ok)ah_logf(m->vm,"failed to save game option preferences");
}
static void options_restore(menu *m)
{
    ah_val option;if(!ah_get_member(m->vm,game_object(m),"option",&option))return;
    for(unsigned i=0;i<sizeof saved_options/sizeof saved_options[0];i++) {
        saved_option *v=&saved_options[i];if(!v->valid)continue;
        ah_val value;
        if(v->type==1)value=ah_str(m->vm,v->text);
        else if(v->type==0)value=ah_int((int)v->number[0]);
        else {ah_obj *a=ah_array_new(m->vm);for(int k=0;k<v->count;k++)ah_array_push(m->vm,a,ah_float(v->number[k]));value=ah_objv(a);}
        ah_set_member(m->vm,ah_as_obj(option,AO_MODULE),v->name,value);
    }
}
static int nat_option_apply(ah_vm *vm,ah_obj *self,void *data,int argc,const ah_val *argv,ah_val *ret)
{
    (void)self;(void)argc;(void)argv;menu *m=data;
    /* MOption.apply -> 004365B8 +4C..68 publishes the three unit enums to
       MUnit (006244D8), after option.ad UnitRoot.apply L4134..4136 writes
       them. Native projects share preferences through the save bridge. */
    options_capture(m);gt4_save_capture(vm,game_object(m),1);
    *ret=ah_nil();return 0;
}
double menu_option_number(menu *m,const char *name,double fallback)
{
 if(!m){for(unsigned i=0;i<sizeof saved_options/sizeof saved_options[0];i++)
    if(saved_options[i].valid && saved_options[i].type==0 && !strcmp(saved_options[i].name,name))return saved_options[i].number[0];
    return gt4_save_option_number(name,fallback);}
 ah_val option,value;if(!ah_get_member(m->vm,game_object(m),"option",&option) ||
 !ah_get_member(m->vm,ah_as_obj(option,AO_MODULE),name,&value) || value.t==AV_NIL)return fallback;return ah_num(value);}
void menu_pad_configuration(menu *m,int port,gt4_pad_config *out)
{
 ah_val option;
 if(m && ah_get_member(m->vm,game_object(m),"option",&option)){
  gt4_pad_option(m->vm,ah_as_obj(option,AO_MODULE),port,out);return;
 }
 gt4_pad_defaults(out,port);
 /* Applied options survive switching cached projects and a race restart. */
 for(int analog=0;analog<2;analog++){
  const char *name=analog?(port?"pad_analog_2p":"pad_analog_1p"):(port?"pad_buttons_2p":"pad_buttons_1p");
  int *values=analog?out->analog:out->button,count=analog?6:16;
  /* A loaded profile's option takes precedence over standalone preferences. */
  if(!gt4_save_option_array_get(name,values,count))
   for(unsigned k=0;k<sizeof saved_options/sizeof *saved_options;k++){
    saved_option *v=&saved_options[k];if(!v->valid || v->type!=2 || strcmp(name,v->name) || v->count!=count)continue;
    for(int j=0;j<count;j++)values[j]=(int)v->number[j];
   }
  for(int j=0;j<count;j++)if(values[j]<-2 || values[j]>=GT4_PAD_ACTIONS)values[j]=-1;
 }
}
const char *menu_option_text(menu *m,const char *name,const char *fallback)
{
 ah_val option,value;const char *s;
 if(!m)return fallback;
 if(!ah_get_member(m->vm,game_object(m),"option",&option) ||
 !ah_get_member(m->vm,ah_as_obj(option,AO_MODULE),name,&value) || !(s=ah_cstr(value)))return fallback;return s;}

static int nat_need_save(ah_vm *vm,ah_obj *self,void *data,int argc,const ah_val *argv,ah_val *ret)
{
    (void)self;(void)argc;(void)argv;
    ah_set_member(vm,game_object(data),"need_save",ah_int(1));
    ((menu *)data)->options_dirty=1;
    *ret=ah_nil();return 0;
}

static int course_capacity(menu *m,const char *label)
{
    if(!label)return 0;
    if(!m->course_load_attempted) {
        m->course_load_attempted=1;
        m->course_db=gt4_vol_read(m->vol,"/specdb/GT4_US2560/COURSE.dbt",&m->course_db_size);
        m->course_labels=gt4_vol_read(m->vol,"/specdb/GT4_US2560/COURSE.idi",&m->course_labels_size);
    }
    uint32_t id;uint8_t row[256];
    if(!gt4_specdb_label(m->course_labels,m->course_labels_size,label,&id))return 0;
    int bytes=gt4_specdb_row(m->course_db,m->course_db_size,id,row,sizeof row);
    return bytes==22?row[17]:0; /* US2560 COURSE.EntryMax */
}
static int nat_course_capacity(ah_vm *vm,ah_obj *self,void *data,int argc,const ah_val *argv,ah_val *ret)
{
    (void)vm;(void)self;*ret=ah_int(argc?course_capacity(data,ah_cstr(argv[0])):0);return 0;
}
static int course_key(menu *m,const char *label,uint64_t *key){
    if(!label)return 0;course_capacity(m,label);uint32_t id;
    if(!gt4_specdb_label(m->course_labels,m->course_labels_size,label,&id))return 0;
    /* 00447C78 +0x14: COURSE namespace 0x22. */
    *key=UINT64_C(0x2200000000)|id;return 1;
}
static ah_obj *profile_module(ah_vm *vm,ah_obj *parent,const char *name){ah_val v;if(ah_get_member(vm,parent,name,&v) && ah_as_obj(v,AO_MODULE))return v.u.o;return submodule(vm,parent,name);}
static void course_store(ah_vm *vm,ah_obj *available,const uint64_t keys[128]){
    ah_obj *courses=ah_array_new(vm);
    for(unsigned i=0;i<128;i++){ah_obj *entry=ah_module_new(vm,"course",NULL,NULL,NULL);ah_set_member(vm,entry,"code",ah_int((int32_t)keys[i]));ah_set_member(vm,entry,"table",ah_int((int32_t)(keys[i]>>32)));ah_array_push(vm,courses,ah_objv(entry));}
    ah_set_member(vm,available,"courses",ah_objv(courses));
}
static int course_saved(menu *m,ah_obj *available,uint64_t keys[128]){
    ah_vm *vm=m->vm;ah_val value;for(unsigned i=0;i<128;i++)keys[i]=UINT64_MAX;
    if(ah_get_member(vm,available,"courses",&value)){
        ah_obj *courses=ah_as_obj(value,AO_ARRAY);if(!courses)return 0;
        for(int i=0;i<ah_array_size(courses);i++){ah_obj *entry=ah_as_obj(ah_array_get(courses,i),AO_MODULE);ah_val code,table;if(!entry || !ah_get_member(vm,entry,"code",&code) || !ah_get_member(vm,entry,"table",&table))return 0;
            gt4_career_course_unlock(keys,(uint32_t)code.u.i|((uint64_t)(uint32_t)table.u.i<<32));}
        return 1;
    }
    /* boot.ad initAvailable: authored US_carlist.course_carlist, plus
       the reverse only when CourseData[course].has_reverse is set. */
    size_t size=0;uint8_t *bytes=gt4_vol_read(m->vol,"/projects/boot/US_carlist.adc",&size);char error[256];
    ah_code *script=bytes?ah_load(bytes,size,error,sizeof error):NULL;free(bytes);if(!script)return 0;
    ah_vm *seed=ah_vm_new();int ok=ah_run_script(seed,script)>=0;ah_val init,list,course_data;
    ok=ok && ah_get_member(seed,ah_root(seed),"InitData",&init) && ah_get_member(seed,init.u.o,"course_carlist",&list) && ah_get_member(vm,m->project,"CourseData",&course_data);
    if(ok)for(int i=0;i<ah_array_size(list.u.o);i++){
        const char *label=ah_cstr(ah_array_get(list.u.o,i));ah_val info,reverse;uint64_t key;
        if(label && ah_get_member(vm,course_data.u.o,label,&info) && course_key(m,label,&key)){
            gt4_career_course_unlock(keys,key);
            if(ah_get_member(vm,info.u.o,"has_reverse",&reverse) && ah_truthy(reverse)){char name[128];snprintf(name,sizeof name,"r_%s",label);if(course_key(m,name,&key))gt4_career_course_unlock(keys,key);}
        }
    }
    ah_vm_free(seed);if(ok){course_store(vm,available,keys);ah_set_member(vm,game_object(m),"need_save",ah_int(1));m->options_dirty=1;}return ok;
}
static int nat_course_available(ah_vm *vm,ah_obj *self,void *data,int argc,const ah_val *argv,ah_val *ret){
    (void)self;menu *m=data;uint64_t key,keys[128];ah_obj *available=profile_module(vm,game_object(m),"available");ah_val all;
    *ret=ah_bool(0);if(!argc || !course_key(m,ah_cstr(argv[0]),&key))return 0;
    if(!course_saved(m,available,keys))return ah_error(vm,"original course availability unavailable");
    *ret=ah_bool(gt4_career_course_available(keys,key,ah_get_member(vm,available,"all",&all) && ah_truthy(all)));return 0;
}
static int nat_course_unlock(ah_vm *vm,ah_obj *self,void *data,int argc,const ah_val *argv,ah_val *ret){
    (void)self;menu *m=data;uint64_t key,keys[128];*ret=ah_nil();if(!argc || !course_key(m,ah_cstr(argv[0]),&key))return 0;
    ah_obj *available=profile_module(vm,game_object(m),"available");if(!course_saved(m,available,keys))return ah_error(vm,"original course availability unavailable");
    gt4_career_course_unlock(keys,key);course_store(vm,available,keys);ah_set_member(vm,game_object(m),"need_save",ah_int(1));m->options_dirty=1;ret->t=AV_VOID;return 0;
}
void menu_preview_courses(menu *m,int enabled){if(m)m->preview_courses=enabled;}

/* ---------------------------------------------------------------- the Arcade's race setup (arcade.h) */

static gt4_arcade *arcade_of(menu *m)
{
    if(!m->arcade){m->arcade=gt4_arcade_open(m->vol);if(m->arcade)gt4_arcade_seed(m->arcade,(uint32_t)time(NULL)*2654435761u);}
    return m->arcade;
}
/* the original's toInt: numbers truncated, strings read as a number (0 when not one) */
static int to_int(ah_val v){const char *s=ah_cstr(v);if(s)return (int)strtol(s,NULL,10);return ah_is_num(v)?(int)ah_num(v):0;}
static const char *car_label_of(ah_vm *vm,ah_val v)
{
    ah_obj *o=ah_as_obj(v,AO_MODULE);ah_val code;
    if(!o)return ah_cstr(v);
    if(ah_get_member(vm,o,"car_code",&code) && ah_cstr(code))return ah_cstr(code);
    return ah_get_member(vm,o,"code",&code)?ah_cstr(code):NULL;
}
/* MCourseData.GetType (func_001B3030), GetAttrString (func_001B3320) */
static int nat_course_text(ah_vm *vm,ah_obj *self,void *d,int argc,const ah_val *argv,ah_val *ret)
{
    (void)self;menu *m=ah_user(vm);gt4_arcade *a=arcade_of(m);const char *crs=argc?ah_cstr(argv[0]):NULL;
    if(!argc){*ret=ah_nil();return 0;}
    *ret=ah_str(vm,d?gt4_arcade_course_attr(a,crs):gt4_arcade_course_type(a,crs));return 0;
}
/* MCarData.GetRaceCode (func_0012E150), IsTireNormal (func_0012E388),
   IsDirtRunnable (func_0012E498, 0012E4F0),
   RaceForbidden (func_00130420), ShiftCarCode (func_0012F3E0) */
static int nat_arcade_car(ah_vm *vm,ah_obj *self,void *d,int argc,const ah_val *argv,ah_val *ret)
{
    (void)self;menu *m=ah_user(vm);gt4_arcade *a=arcade_of(m);const char *op=d,*car=argc?car_label_of(vm,argv[0]):NULL;
    *ret=ah_nil();if(!argc)return 0;
    if(!strcmp(op,"GetRaceCode")){
        if(argc<2)return 0;
        const char *race=gt4_arcade_race_code(a,car,to_int(argv[1]),argc>2?to_int(argv[2]):0);
        if(race)*ret=ah_str(vm,race);
        return 0;
    }
    if(!strcmp(op,"IsTireNormal")){*ret=ah_bool(gt4_arcade_tire_normal(a,car));return 0;}
    if(!strcmp(op,"IsDirtRunnable")){*ret=ah_bool(gt4_arcade_dirt_runnable(a,car));return 0;}
    if(!strcmp(op,"IsTestCar")){*ret=ah_bool(gt4_arcade_test_car(a,car));return 0;}
    if(!strcmp(op,"RaceForbidden")){*ret=ah_bool(gt4_arcade_race_forbidden(a,car));return 0;}
    if(!strcmp(op,"ShiftCarCode")){
        const char *shifted=gt4_arcade_shift_car(a,car,argc>1?ah_cstr(argv[1]):NULL);
        *ret=ah_str(vm,shifted?shifted:car?car:"");return 0;
    }
    return 0;
}
/* MCarGarage (and CarFace.car_data): getDTrainType (func_0013CC28),
   isAutomaticOnly (func_0013ED58), getColorNum (func_00445828) */
static int nat_garage_value(ah_vm *vm,ah_obj *self,void *d,int argc,const ah_val *argv,ah_val *ret)
{
    (void)argc;(void)argv;menu *m=ah_user(vm);gt4_arcade *a=arcade_of(m);const char *op=d,*car=self?car_label_of(vm,ah_objv(self)):NULL;
    if(!strcmp(op,"getDTrainType"))*ret=ah_str(vm,gt4_arcade_drivetrain(a,car));
    else if(!strcmp(op,"isAutomaticOnly"))*ret=ah_bool(gt4_arcade_automatic_only(a,car));
    else *ret=ah_int(gt4_arcade_color_num(a,car));
    return 0;
}
static int nat_quick_catalog(ah_vm*,ah_obj*,void*,int,const ah_val*,ah_val*);
static int nat_catalog_value(ah_vm*,ah_obj*,void*,int,const ah_val*,ah_val*);
static int nat_owned_summary(ah_vm*,ah_obj*,void*,int,const ah_val*,ah_val*);
static int nat_car_metadata(ah_vm*,ah_obj*,void*,int,const ah_val*,ah_val*);
static int nat_garage_aspiration(ah_vm*,ah_obj*,void*,int,const ah_val*,ah_val*);
static void bind_garage_values(ah_vm *vm,ah_obj *o)
{
    ah_set_member(vm,o,"getCatPs",ah_bind(vm,o,ah_native(vm,"getCatPs",nat_quick_catalog,NULL)));
    ah_set_member(vm,o,"getCatTq",ah_bind(vm,o,ah_native(vm,"getCatTq",nat_quick_catalog,(void *)1)));
    ah_set_member(vm,o,"getCurrentPs",ah_bind(vm,o,ah_native(vm,"getCurrentPs",nat_quick_catalog,(void *)2)));
    ah_set_member(vm,o,"getCurrentTq",ah_bind(vm,o,ah_native(vm,"getCurrentTq",nat_quick_catalog,(void *)3)));
    static const char *const specs[]={"getCatDisplacement","getCatEngineType","getCatEngineCam","getCatLength","getCatWidth","getCatHeight","getCatMs","getCurrentMs"};
    for(int i=0;i<8;i++)ah_set_member(vm,o,specs[i],ah_bind(vm,o,ah_native(vm,specs[i],nat_catalog_value,(void *)(intptr_t)i)));
    static const char *const summary[]={"getCurrentPower","getDate","getCurrentPWR"};
    for(int i=0;i<3;i++)ah_set_member(vm,o,summary[i],ah_bind(vm,o,ah_native(vm,summary[i],nat_owned_summary,(void *)(intptr_t)i)));
    ah_set_member(vm,o,"getAspiration",ah_bind(vm,o,ah_native(vm,"getAspiration",nat_garage_aspiration,NULL)));
    static const char *const names[]={"getDTrainType","isAutomaticOnly","getColorNum"};
    for(int i=0;i<3;i++)ah_set_member(vm,o,names[i],ah_bind(vm,o,ah_native(vm,names[i],nat_garage_value,(void *)names[i])));
}
static int nat_bind_game_car(ah_vm*,ah_obj*,void*,int,const ah_val*,ah_val*);
/* MCarGarage().copyFrom(car): the copy made a full garage car (its part
   and setting methods), as gtmode.ad TuningDialog L7747..7755 previews a
   part on such a copy */
static int nat_garage_copy_from(ah_vm *vm,ah_obj *self,void *d,int argc,const ah_val *argv,ah_val *ret)
{
    ah_val copy,arg,bound;
    if(!ah_get_member(vm,self,"_copyFrom",&copy) || copy.t==AV_NIL)return ah_error(vm,"copyFrom unavailable");
    if(ah_call(vm,copy,argc,argv,ret)<0)return -1;
    /* gtmode.ad L6736..6737 snapshots the previous riding car even in
       an empty garage. 0013BE08 +0x1c..0x7c leaves the converted handle
       empty for nil; there is no car label to resolve at this boundary. */
    if(argc==1){
        const char *label=car_label_of(vm,argv[0]);
        if(argv[0].t==AV_NIL || (label && !*label))return 0;
    }
    arg=ah_objv(self);
    if(nat_bind_game_car(vm,NULL,d,1,&arg,&bound)<0)return -1;
    ah_set_member(vm,self,"copyFrom",ah_bind(vm,self,ah_native(vm,"copyFrom",nat_garage_copy_from,d)));
    return 0;
}
static int nat_car_garage(ah_vm *vm,ah_obj *self,void *d,int argc,const ah_val *argv,ah_val *ret)
{
    ah_obj *o=self?self:ah_module_new(vm,"MCarGarage",NULL,NULL,NULL);
    const char *car=argc?car_label_of(vm,argv[0]):NULL;
    ah_set_member(vm,o,"car_code",ah_str(vm,car?car:""));
    ah_set_member(vm,o,"car_color",ah_int(argc>1?to_int(argv[1]):0));
    ah_set_member(vm,o,"color_index",ah_int(argc>1?to_int(argv[1]):0));
    ah_set_member(vm,o,"odometer",ah_int(0));gt4_game_bind_car(vm,o);
    if(car && gt4_game_has_car_factory()){
        if(!gt4_game_construct_car(vm,o,car,argc>1?to_int(argv[1]):0,0))return ah_error(vm,"MCarGarage: original car construction failed");
        ah_val arg=ah_objv(o),bound;if(nat_bind_game_car(vm,NULL,d,1,&arg,&bound)<0)return -1;
    }
    bind_garage_values(vm,o);
    {ah_val copy;if(ah_get_member(vm,o,"copyFrom",&copy) && copy.t!=AV_NIL){
        ah_set_member(vm,o,"_copyFrom",copy);
        ah_set_member(vm,o,"copyFrom",ah_bind(vm,o,ah_native(vm,"copyFrom",nat_garage_copy_from,d)));}}
    *ret=ah_objv(o);return 0;
}
/* MCarData.PickUpEnemyCars(garage, list, offset, range, count, dirt)
   (func_001308C8): [[car, paint], ...] */
static int nat_pick_enemies(ah_vm *vm,ah_obj *self,void *d,int argc,const ah_val *argv,ah_val *ret)
{
    (void)self;menu *m=d;gt4_arcade *a=arcade_of(m);gt4_arcade_enemy out[5];
    if(argc<2){*ret=ah_nil();return 0;}
    if(!m->rate_cars)return ah_error(vm,"PickUpEnemyCars: no car ratings (the game's data)");
    const char *player=car_label_of(vm,argv[0]);ah_obj *list=ah_as_obj(argv[1],AO_ARRAY);
    int n=list?ah_array_size(list):0;const char **labels=calloc((size_t)n+1,sizeof *labels);
    if(!labels)return ah_error(vm,"out of memory");
    for(int i=0;i<n;i++){const char *c=ah_cstr(ah_array_get(list,i));labels[i]=c?c:"";}
    float offset=argc>2?(float)ah_num(argv[2]):0.0f,range=argc>3?(float)ah_num(argv[3]):0.002f;
    int count=argc>4?to_int(argv[4]):5,dirt=argc>5?ah_truthy(argv[5]):0;
    int got=gt4_arcade_pick_enemies(a,m->rate_cars,m->rate_ctx,player,labels,n,offset,range,count,dirt,out);
    ah_obj *result=ah_array_new(vm);
    for(int i=0;i<got;i++){
        ah_obj *e=ah_array_new(vm);ah_array_push(vm,e,ah_str(vm,out[i].car));ah_array_push(vm,e,ah_int(out[i].color));
        ah_array_push(vm,result,ah_objv(e));
    }
    free(labels);
    if(got<0)return ah_error(vm,"PickUpEnemyCars: the cars' ratings failed");
    *ret=ah_objv(result);return 0;
}
/* MRandom.GetValue(low, high) (func_002CB888, the game's generator) */
static int nat_random_value(ah_vm *vm,ah_obj *self,void *d,int argc,const ah_val *argv,ah_val *ret)
{
    (void)self;menu *m=d;gt4_arcade *a=arcade_of(m);
    if(argc<2){*ret=ah_nil();return 0;}
    int lo=to_int(argv[0]),hi=to_int(argv[1]);
    *ret=ah_int((int)(gt4_arcade_random01(a)*(float)(hi-lo)+(float)lo));return 0;
}
static int nat_random_new(ah_vm *vm,ah_obj *self,void *d,int argc,const ah_val *argv,ah_val *ret)
{
    (void)argc;(void)argv;
    ah_set_member(vm,self,"getValue",ah_bind(vm,self,ah_native(vm,"getValue",nat_random_value,d)));
    *ret=ah_nil();return 0;
}
/* MMemoryCardPlayList: the Replay Theater's playlist (labomode.ad
   addPlayList, commandTrashAlias): its replays' names in order; the
   card's "replaylist" is the save folder's playlist file here
   (gt4_replay_playlist_*). The scripts read its size as a member
   (list.size), kept with each change; files are told apart by name. */
static char photo_playlist_names[GT4_ALBUM_CAPACITY][64];
static int photo_playlist_count,photo_playlist_read;
static void photo_playlist_load(void);
static char playlist_names[128][64];
static int playlist_count,playlist_read;
static void playlist_load(void)
{if(!playlist_read){playlist_count=gt4_replay_playlist_read(playlist_names,128);playlist_read=1;}}
static const char *file_name_of(ah_vm *vm,ah_val v)
{ah_val n;ah_obj *o=ah_as_obj(v,AO_MODULE);return o && ah_get_member(vm,o,"_name",&n)?ah_cstr(n):NULL;}
static int playlist_search(const char *name)
{for(int i=0;name && i<playlist_count;i++)if(!strcmp(playlist_names[i],name))return i;return -1;}
static int nat_playlist_op(ah_vm *vm,ah_obj *self,void *d,int argc,const ah_val *argv,ah_val *ret)
{
    const char *op=d;ah_val marker;int photo=ah_get_member(vm,self,"_photo_playlist",&marker) && ah_truthy(marker);
    if(photo)photo_playlist_load();else playlist_load();
    char (*names)[64]=photo?photo_playlist_names:playlist_names;
    int *count=photo?&photo_playlist_count:&playlist_count;*ret=ah_nil();
    if(!strcmp(op,"clear"))(*count)=0;
    else if(!strcmp(op,"search")) {int found=-1;const char *name=argc?file_name_of(vm,argv[0]):NULL;
        for(int k=0;name && k<*count;k++)if(!strcmp(names[k],name)){found=k;break;}*ret=ah_int(found);}
    else if(!strcmp(op,"push") || !strcmp(op,"append") || !strcmp(op,"insert")) {
        int at=!strcmp(op,"insert") && argc>1?(int)ah_num(argv[0]):(*count);
        const char *name=argc?file_name_of(vm,argv[argc-1]):NULL;
        if(name && strlen(name)<64 && (*count)<(photo?GT4_ALBUM_CAPACITY:128)){
            if(at<0)at=0;if(at>(*count))at=(*count);
            memmove(names[at+1],names[at],(size_t)((*count)-at)*64);
            snprintf(names[at],64,"%s",name);(*count)++;
        }
    } else if(!strcmp(op,"remove") || !strcmp(op,"erase")) {
        int at=argc?(int)ah_num(argv[0]):-1;
        if(at>=0 && at<(*count)){memmove(names[at],names[at+1],(size_t)((*count)-at-1)*64);(*count)--;}
    } else if(!strcmp(op,"rotate") && (*count)) {   /* the first n to the end (addPlayList: the new ones first) */
        int n=argc?(int)ah_num(argv[0]):0;char copy[GT4_ALBUM_CAPACITY][64];
        n=((n%(*count))+(*count))%(*count);
        memcpy(copy,names,(size_t)(*count)*64);
        for(int i=0;i<(*count);i++)memcpy(names[i],copy[(i+n)%(*count)],64);
    }
    ah_set_member(vm,self,"size",ah_int((*count)));
    return 0;
}
static int nat_playlist_new(ah_vm *vm,ah_obj *self,void *d,int argc,const ah_val *argv,ah_val *ret)
{
    static const char *const ops[]={"clear","search","push","append","remove","erase","insert","rotate"};
    (void)d;(void)argc;(void)argv;playlist_load();
    for(unsigned i=0;i<sizeof ops/sizeof ops[0];i++)ah_set_member(vm,self,ops[i],ah_bind(vm,self,ah_native(vm,ops[i],nat_playlist_op,(void *)ops[i])));
    ah_set_member(vm,self,"size",ah_int(playlist_count));
    *ret=ah_nil();return 0;
}
/* game.setEntryCarCode(i, car, paint) (func_00161A50), getEntryCarCode(i) */
static int nat_entry_car(ah_vm *vm,ah_obj *self,void *d,int argc,const ah_val *argv,ah_val *ret)
{
    (void)self;menu *m=ah_user(vm);int i=argc?to_int(argv[0]):-1;*ret=ah_nil();
    if(i<0 || i>=6)return 0;
    if(d){
        const char *car=argc>1?ah_cstr(argv[1]):NULL;
        free(m->entry_car[i]);m->entry_car[i]=car?strdup(car):NULL;m->entry_color[i]=argc>2?to_int(argv[2]):0;
        return 0;
    }
    ah_obj *e=ah_array_new(vm);
    if(!m->entry_car[i] && i==0){
        ah_val car,color;ah_obj *game=game_object(m);
        ah_array_push(vm,e,ah_get_member(vm,game,"car_code",&car)?car:ah_nil());
        ah_array_push(vm,e,ah_get_member(vm,game,"car_color",&color)?color:ah_int(0));
    } else {
        ah_array_push(vm,e,m->entry_car[i]?ah_str(vm,m->entry_car[i]):ah_nil());
        ah_array_push(vm,e,ah_int(m->entry_color[i]));
    }
    *ret=ah_objv(e);return 0;
}
void menu_set_car_rater(menu *m,int (*rate)(void *,const uint64_t *,int,float *),void *ctx){if(m){m->rate_cars=rate;m->rate_ctx=ctx;}}
int menu_race_entries(menu *m,const char **cars,int *colors,int max)
{
    ah_val v;int n=0,others=0;
    if(!m)return 0;
    if(ah_get_member(m->vm,game_object(m),"overwrite_enemy_num",&v))others=to_int(v);
    for(int i=1;i<=others && i<6 && n<max;i++)if(m->entry_car[i]){cars[n]=m->entry_car[i];colors[n]=m->entry_color[i];n++;}
    return n;
}
/* MCourseRecord (CourseRecordT<128>, 0x238-byte units keyed by the course
   code, 00433AA0/00433C10): ten entries per course sorted by time, each
   with its date, car and passcode (00433028..00433708); best_score is the
   Arcade's score byte (+0x230). Kept as game.course_record.ranking. */
#define COURSE_RECORD_EMPTY 0x157529ffu
static unsigned car_u32(const uint8_t *b);
static void load_car_tables(menu *m);
static ah_val record_field(ah_vm *vm,ah_obj *o,const char *name){ah_val v=ah_nil();if(o)ah_get_member(vm,o,name,&v);return v;}
static ah_obj *course_record_entries(ah_vm *vm,ah_obj *unit){return ah_as_obj(record_field(vm,unit,"entries"),AO_ARRAY);}
static ah_obj *course_record_entry_new(ah_vm *vm,uint32_t time,uint32_t date,const char *car,const char *passcode){
    ah_obj *e=ah_module_new(vm,"entry",NULL,NULL,NULL);
    ah_set_member(vm,e,"time",ah_int((int64_t)time));ah_set_member(vm,e,"date",ah_int((int64_t)date));
    ah_set_member(vm,e,"car",ah_str(vm,car?car:""));ah_set_member(vm,e,"passcode",ah_str(vm,passcode?passcode:""));
    return e;
}
static uint32_t course_record_time(ah_vm *vm,ah_obj *entries,int i){   /* 004334F8 */
    ah_obj *e=entries && i>=0 && i<10 && i<ah_array_size(entries)?ah_as_obj(ah_array_get(entries,i),AO_MODULE):NULL;
    ah_val t=record_field(vm,e,"time");return e && ah_is_num(t)?(uint32_t)ah_num(t):COURSE_RECORD_EMPTY;
}
static uint64_t record_u64(const uint8_t *p){uint64_t v=0;for(int i=7;i>=0;i--)v=v<<8|p[i];return v;}
/* A PS2 save's course records (records_ps2, 132 units) for one course. */
static void course_record_import(menu *m,ah_obj *course_record,ah_obj *unit,uint64_t key){
    ah_vm *vm=m->vm;enum{UNITS=132,SIZE=0x238};
    if(record_field(vm,course_record,"records_ps2").t==AV_NIL)return;
    uint8_t *all=malloc(UNITS*SIZE);if(!all)return;
    if(gt4_career_blob_get(vm,course_record,"records_ps2",all,UNITS*SIZE)){
        load_car_tables(m);
        for(int u=0;u<UNITS;u++){
            const uint8_t *p=all+u*SIZE;if(record_u64(p)!=key)continue;
            ah_obj *entries=course_record_entries(vm,unit);
            for(int i=0;i<10;i++){
                const uint8_t *e=p+8+i*0x30;uint32_t time=car_u32(e),date=car_u32(e+4);uint64_t car=record_u64(e+8);
                const char *label=(car>>32)==0?gt4_specdb_label_for_id(m->car_labels,m->car_labels_size,(uint32_t)car):NULL;
                char passcode[33];memcpy(passcode,e+0x10,32);passcode[32]=0;
                ah_array_set(entries,i,ah_objv(course_record_entry_new(vm,time,date,label,time==COURSE_RECORD_EMPTY?"":passcode)));
            }
            ah_set_member(vm,unit,"best_score",ah_int(p[0x230]));
            break;
        }
    }
    free(all);
}
static ah_obj *course_record_unit(menu *m,const char *course){
    ah_vm *vm=m->vm;uint64_t key;if(!course_key(m,course,&key))return NULL;
    ah_obj *course_record=profile_module(vm,game_object(m),"course_record");
    ah_obj *ranking=ah_as_obj(record_field(vm,course_record,"ranking"),AO_ARRAY);
    if(!ranking){ranking=ah_array_new(vm);ah_set_member(vm,course_record,"ranking",ah_objv(ranking));}
    for(int i=0;i<ah_array_size(ranking);i++){
        ah_obj *unit=ah_as_obj(ah_array_get(ranking,i),AO_MODULE);const char *name=ah_cstr(record_field(vm,unit,"course"));
        if(unit && name && !strcmp(name,course) && course_record_entries(vm,unit))return unit;
    }
    ah_obj *unit=ah_module_new(vm,"MCourseRecordUnit",NULL,NULL,NULL),*entries=ah_array_new(vm);
    ah_set_member(vm,unit,"course",ah_str(vm,course));ah_set_member(vm,unit,"best_score",ah_int(0));ah_set_member(vm,unit,"entries",ah_objv(entries));
    for(int i=0;i<10;i++)ah_array_push(vm,entries,ah_objv(course_record_entry_new(vm,COURSE_RECORD_EMPTY,0,"","")));   /* 00432E40 */
    course_record_import(m,course_record,unit,key);
    ah_array_push(vm,ranking,ah_objv(unit));return unit;
}
/* 00433178 + 00433028: the slot before the first slower or empty entry
   (an equal time goes after); the later entries move down, the tenth drops. */
static int ranking_passcode(menu *m,const char *car,uint32_t lap,char code[33])
{
 uint8_t key[16];uint32_t id;load_car_tables(m);
 if(!gt4_specdb_label(m->car_labels,m->car_labels_size,car,&id) || !gt4_ranking_code_key(m->vol,key))return 0;
 /* 0015B7A0 +114..160 / 0015B948 +128..16C: car ordinal,
    fixed test fields, score. A: the caller does not initialize the
    unused one-bit stack field; native storage explicitly uses zero.
    004482C0 +4..24 extracts and sign-extends the low 32-bit identifier
    (lui FFFF is sign-extended before dsrl32) for 00449190. */
 int index=gt4_specdb_row_index(m->car_db,m->car_db_size,(uint64_t)(int64_t)(int32_t)id);if(index<0)return 0;
 uint32_t fields[10]={0,(uint32_t)index,1,0,1,1,1,lap,1,1};
 gt4_ranking_code(fields,key,(uint32_t)time(NULL),code);return 1;
}
int menu_course_record_insert(menu *m,const char *course,const char *car,uint32_t lap){
    if(!m || !course || !car || lap==COURSE_RECORD_EMPTY)return 0;
    ah_vm *vm=m->vm;ah_obj *unit=course_record_unit(m,course),*entries=course_record_entries(vm,unit);if(!entries)return 0;
    int at=-1;for(int i=0;i<10;i++){uint32_t t=course_record_time(vm,entries,i);if(t==COURSE_RECORD_EMPTY || lap<t){at=i;break;}}
    if(at<0)return 0;
    char code[33];if(!ranking_passcode(m,car,lap,code))return 0;
    for(int i=9;i>at;i--)ah_array_set(entries,i,ah_array_get(entries,i-1));
    time_t now=time(NULL);struct tm *t=localtime(&now);   /* 00548918/00579BD8: the console clock */
    uint32_t date=t?(uint32_t)(uint16_t)(t->tm_year+1900)|(uint32_t)(t->tm_mon+1)<<16|(uint32_t)t->tm_mday<<24:0;
    ah_array_set(entries,at,ah_objv(course_record_entry_new(vm,lap,date,car,code)));
    gt4_save_capture(vm,game_object(m),1);
    return 1;
}
/* the top speed of a Power & Speed entry (00433528: 0 none) */
static float machine_record_speed(ah_vm *vm,ah_obj *entries,int i){
    ah_obj *e=entries && i>=0 && i<10 && i<ah_array_size(entries)?ah_as_obj(ah_array_get(entries,i),AO_MODULE):NULL;
    ah_val v=record_field(vm,e,"speed");return e && ah_is_num(v)?(float)ah_num(v):0.f;
}
/* MMachineRecord (GameData +0x26640, four CourseRecordT units, 00433C78):
   0 the 400 m, 1 the 1000 m, 2 the top speed, 3 the 400 m of Las Vegas
   (00116268 / 00116440 / 001165B8; quick-mt.ad UnitData). Kept as
   game.machine_record.ranking. */
static ah_obj *machine_record_unit(menu *m,int index){
    ah_vm *vm=m->vm;if(index<0 || index>3)return NULL;
    ah_obj *record=profile_module(vm,game_object(m),"machine_record");
    ah_obj *ranking=ah_as_obj(record_field(vm,record,"ranking"),AO_ARRAY);
    if(!ranking){ranking=ah_array_new(vm);ah_set_member(vm,record,"ranking",ah_objv(ranking));}
    while(ah_array_size(ranking)<4){
        ah_obj *unit=ah_module_new(vm,"MCourseRecordUnit",NULL,NULL,NULL),*entries=ah_array_new(vm);
        ah_set_member(vm,unit,"entries",ah_objv(entries));
        for(int i=0;i<10;i++){ah_obj *e=course_record_entry_new(vm,COURSE_RECORD_EMPTY,0,"","");ah_set_member(vm,e,"speed",ah_float(0));ah_array_push(vm,entries,ah_objv(e));}
        ah_array_push(vm,ranking,ah_objv(unit));
    }
    return ah_as_obj(ah_array_get(ranking,index),AO_MODULE);
}
/* 00433260 (a time) / 00433348 (a top speed: before the first slower or
   empty one, 004331F8) into unit `index` */
int menu_machine_record_insert(menu *m,int index,const char *car,uint32_t lap,float speed){
    if(!m || !car)return 0;
    ah_vm *vm=m->vm;ah_obj *unit=machine_record_unit(m,index),*entries=course_record_entries(vm,unit);if(!entries)return 0;
    int at=-1;
    for(int i=0;i<10;i++){
        if(index==2){float v=machine_record_speed(vm,entries,i);if(v==0.f || v<speed){at=i;break;}}
        else {uint32_t t=course_record_time(vm,entries,i);if(t==COURSE_RECORD_EMPTY || lap<t){at=i;break;}}
    }
    if(at<0 || (index==2?!(speed>0):lap==COURSE_RECORD_EMPTY))return 0;
    char code[33];if(!ranking_passcode(m,car,index==2?0:lap,code))return 0;   /* 0015B948 +144: speed test packs zero */
    for(int i=9;i>at;i--)ah_array_set(entries,i,ah_array_get(entries,i-1));
    time_t now=time(NULL);struct tm *t=localtime(&now);
    uint32_t date=t?(uint32_t)(uint16_t)(t->tm_year+1900)|(uint32_t)(t->tm_mon+1)<<16|(uint32_t)t->tm_mday<<24:0;
    ah_obj *e=course_record_entry_new(vm,index==2?COURSE_RECORD_EMPTY:lap,date,car,code);
    ah_set_member(vm,e,"speed",ah_float(index==2?speed:0));
    ah_array_set(entries,at,ah_objv(e));
    gt4_save_capture(vm,game_object(m),1);
    return 1;
}
static int course_unit_method(ah_vm *vm,ah_obj *self,void *data,int argc,const ah_val *argv,ah_val *ret){
    menu *m=ah_user(vm);const char *op=data;ah_obj *entries=course_record_entries(vm,self);
    int count=0;for(int i=0;i<10;i++)if(course_record_time(vm,entries,i)!=COURSE_RECORD_EMPTY)count++;   /* 00433470 */
    if(!strcmp(op,"getRankNum")){*ret=ah_int(count);return 0;}
    if(!strcmp(op,"getRankNumMaxSpeed")){   /* 004334B0: the entries whose speed is not 0 */
        int n=0;for(int i=0;i<10;i++)if(machine_record_speed(vm,entries,i)!=0.f)n++;
        *ret=ah_int(n);return 0;
    }
    int i=argc?(int)ah_num(argv[0]):0;
    ah_obj *entry=entries && i>=0 && i<10 && i<ah_array_size(entries)?ah_as_obj(ah_array_get(entries,i),AO_MODULE):NULL;
    char text[96];
    if(!strcmp(op,"deleteEntry") || !strcmp(op,"deleteEntryMaxSpeed")){   /* 00433430 / 00433450: the later entries move up, the last is cleared */
        if(!entry)return *ret=ah_nil(),0;
        for(int k=i;k<9;k++)ah_array_set(entries,k,ah_array_get(entries,k+1));
        ah_array_set(entries,9,ah_objv(course_record_entry_new(vm,COURSE_RECORD_EMPTY,0,"","")));
        gt4_save_capture(vm,game_object(m),1);*ret=ah_nil();return 0;
    }
    if(!strcmp(op,"getBestSpeed")){   /* 00433528 -> 0042E568: "--%s" or "%d.%02d%s" in the option's unit */
        float v=machine_record_speed(vm,entries,i);int mph=strcmp(menu_option_text(m,"unit_velocity","MPH"),"KMPH")!=0;
        const char *unit=mph?"mph":"km/h";
        if(v==0.f)snprintf(text,sizeof text,"--%s",unit);
        else {float k=v*3.6f;if(mph)k=k*0.6213712f;int n=(int)(k*100.f);snprintf(text,sizeof text,"%d.%02d%s",n/100,n%100,unit);}
    }else if(!strcmp(op,"getBestTime")){   /* 00433558 -> 0042E478 */
        uint32_t time=course_record_time(vm,entries,i);
        if(time==COURSE_RECORD_EMPTY)snprintf(text,sizeof text,"\x7f\x7f'\x7f\x7f.\x7f\x7f\x7f");
        else if(time/3600000)snprintf(text,sizeof text,"%u:%02u'%02u.%03u",time/3600000,(time/60000)%60,(time/1000)%60,time%1000);
        else snprintf(text,sizeof text,"%u'%02u.%03u",time/60000,(time/1000)%60,time%1000);
    }else if(!strcmp(op,"getDate")){   /* 00433598 -> 0042E660 */
        if(!entry)snprintf(text,sizeof text,"\x7f/\x7f/\x7f");
        else {uint32_t date=(uint32_t)ah_num(record_field(vm,entry,"date"));snprintf(text,sizeof text,"%d/%d/%d",(int16_t)date,(date>>16)&255,date>>24);}
    }else if(!strcmp(op,"getCarName")){   /* 00433708 -> 004481B8: CAR_NAME's short name */
        const char *car=entry?ah_cstr(record_field(vm,entry,"car")):NULL;uint32_t id;uint8_t row[16];const char *name=NULL;
        load_car_tables(m);
        if(car && *car && gt4_specdb_label(m->car_labels,m->car_labels_size,car,&id) && gt4_specdb_row(m->car_names,m->car_names_size,id,row,sizeof row)==16)
            name=gt4_specdb_string(m->car_strings,m->car_strings_size,car_u32(row+8));
        snprintf(text,sizeof text,"%s",name?name:"");
    }else {   /* getPassCode(order, part): 00433618, eight characters per part */
        const char *code=entry?ah_cstr(record_field(vm,entry,"passcode")):NULL;int part=argc>1?(int)ah_num(argv[1]):0;
        size_t n=code?strlen(code):0;text[0]=0;
        if(part>=0 && part<4 && n>(size_t)part*8)snprintf(text,sizeof text,"%.8s",code+part*8);
    }
    *ret=ah_str(vm,text);return 0;
}
/* game.machine_record.getMTUnit(index): quick-mt.ad RankingRoot L589 */
static int nat_machine_unit(ah_vm *vm,ah_obj *self,void *d,int argc,const ah_val *argv,ah_val *ret)
{
    (void)self;menu *m=d;ah_obj *stored=argc && ah_is_num(argv[0])?machine_record_unit(m,(int)ah_num(argv[0])):NULL;
    if(!stored){*ret=ah_nil();return 0;}
    ah_obj *u=ah_module_new(vm,"MCourseRecordUnit",NULL,NULL,NULL);
    ah_set_member(vm,u,"entries",record_field(vm,stored,"entries"));
    static const char *const methods[]={"getRankNum","getRankNumMaxSpeed","getBestTime","getBestSpeed","getDate","getCarName","getPassCode","deleteEntry","deleteEntryMaxSpeed"};
    for(unsigned i=0;i<sizeof methods/sizeof *methods;i++)ah_set_member(vm,u,methods[i],ah_bind(vm,u,ah_native(vm,methods[i],course_unit_method,(void *)methods[i])));
    *ret=ah_objv(u);return 0;
}
/* game.course_record.getUnit(course): RankingRoot's list and the Arcade's best_score */
static int nat_score_unit(ah_vm *vm,ah_obj *self,void *d,int argc,const ah_val *argv,ah_val *ret)
{
    (void)self;menu *m=d;ah_obj *stored=course_record_unit(m,argc?ah_cstr(argv[0]):NULL);
    if(!stored){*ret=ah_nil();return 0;}
    ah_obj *u=ah_module_new(vm,"MCourseRecordUnit",NULL,NULL,NULL);   /* the methods stay off the saved unit */
    ah_set_member(vm,u,"entries",record_field(vm,stored,"entries"));ah_set_member(vm,u,"best_score",record_field(vm,stored,"best_score"));
    static const char *const methods[]={"getRankNum","getBestTime","getDate","getCarName","getPassCode","deleteEntry"};
    for(unsigned i=0;i<sizeof methods/sizeof *methods;i++)ah_set_member(vm,u,methods[i],ah_bind(vm,u,ah_native(vm,methods[i],course_unit_method,(void *)methods[i])));
    *ret=ah_objv(u);return 0;
}
static int nat_version_branch(ah_vm *vm,ah_obj *self,void *d,int argc,const ah_val *argv,ah_val *ret)
{(void)self;(void)d;(void)argc;(void)argv;*ret=ah_str(vm,"main");return 0;}   /* func_001AC120: D_00693258 */

/* Script course definitions supply metres. Metric presentation is provisional
   until saved unit preferences are available. */
static int nat_course_units(ah_vm *vm,ah_obj *self,void *d,int argc,const ah_val *argv,ah_val *ret)
{
    (void)self;double metres=argc?ah_num(argv[0]):0;char value[64];
    if(!isfinite(metres) || metres<0 || metres>1e7)return ah_error(vm,"invalid course dimension");
    if(d)snprintf(value,sizeof value,"%.1f m",metres);
    else snprintf(value,sizeof value,"%.2f km",metres/1000);
    *ret=ah_str(vm,value);return 0;
}

static int nat_project_load(ah_vm *vm,ah_obj *self,void *data,int argc,const ah_val *argv,ah_val *ret)
{
    menu *m=data;const char *name=argc?ah_cstr(argv[0]):NULL;
    char path[96];
    if(name && (!strcmp(name,"us_carlist") || !strcmp(name,"/projects/arcade/us_carlist")))snprintf(path,sizeof path,"/projects/arcade/US_carlist.adc");
    else if(name && strlen(name)==19 && !strncmp(name,"usedcar/us_used",15) && strspn(name+15,"0123456789")==4 && name[19]==0)
        snprintf(path,sizeof path,"/usedcar/US_used%.4s.adc",name+15);
    else return ah_error(vm,"unsupported project script: %s",name?name:"");
    /* _tmp_gtmode.ad L5593 loads the authored weekly stock into UsedCarModule. */
    size_t size=0;uint8_t *bytes=gt4_vol_read(m->vol,path,&size);
    if(!bytes)return ah_error(vm,"missing US car catalog script");
    char error[256];ah_code *code=ah_load(bytes,size,error,sizeof error);free(bytes);
    if(!code)return ah_error(vm,"car catalog: %s",error);
    *ret=ah_nil();return ah_run_module(vm,code,self?self:m->project);
}
/* Module.load is an engine method, not an authored static. UsedCarRoot
   L5701 clears the weekly table before L5593 loads the next stock. */
static int project_loader_get(ah_vm *vm,ah_obj *self,const char *name,ah_val *out)
{
    if(strcmp(name,"load"))return 0;
    *out=ah_bind(vm,self,ah_native(vm,name,nat_project_load,ah_user(vm)));return 1;
}
static const ah_host project_loader_host={"project loader",project_loader_get,NULL,NULL,NULL};
static unsigned car_u32(const uint8_t *b){return b[0]|(unsigned)b[1]<<8|(unsigned)b[2]<<16|(unsigned)b[3]<<24;}
static void load_car_tables(menu *m)
{
    if(m->car_load_attempted)return;m->car_load_attempted=1;
    m->car_labels=gt4_vol_read(m->vol,"/specdb/GT4_US2560/GENERIC_CAR.idi",&m->car_labels_size);
    m->car_db=gt4_vol_read(m->vol,"/specdb/GT4_US2560/GENERIC_CAR.dbt",&m->car_db_size);
    m->car_names=gt4_vol_read(m->vol,"/specdb/GT4_US2560/CAR_NAME_american.dbt",&m->car_names_size);
    m->car_strings=gt4_vol_read(m->vol,"/specdb/GT4_US2560/american_StrDB.sdb",&m->car_strings_size);
    m->car_links=gt4_vol_read(m->vol,"/specdb/GT4_US2560/CAR_VARIATION_american.dbt",&m->car_links_size);
    m->car_variants=gt4_vol_read(m->vol,"/specdb/GT4_US2560/VARIATIONamerican.dbt",&m->car_variants_size);
}
/* A car's paints in their menu order (VARIATION rows of its model, by the
   order word +4): name (+12), chip colour and kind (+36: r, g, b, then
   3 metallic / 1 solid). The same table and order as the showroom's. */
typedef struct {unsigned order;const char *name;uint8_t rgb[3];int kind;uint8_t chip[4][4];} car_paint;
static int car_paints(menu *m,const char *code,car_paint *out,int max)
{
    load_car_tables(m);uint32_t id;uint8_t r[256];
    if(!code || !gt4_specdb_label(m->car_labels,m->car_labels_size,code,&id) ||
       gt4_specdb_row(m->car_links,m->car_links_size,id,r,sizeof r)!=8 ||
       gt4_specdb_row(m->car_variants,m->car_variants_size,car_u32(r),r,sizeof r)!=52)return 0;
    const char *model=gt4_specdb_string(m->car_strings,m->car_strings_size,car_u32(r));
    if(!model || m->car_variants_size<16)return 0;
    unsigned rows=car_u32(m->car_variants+8);int n=0;
    for(unsigned i=0;i<rows && n<max;i++) {
        if(gt4_specdb_row_at(m->car_variants,m->car_variants_size,i,r,sizeof r)!=52)continue;
        const char *name=gt4_specdb_string(m->car_strings,m->car_strings_size,car_u32(r));
        if(!name || strcmp(name,model))continue;
        car_paint *c=&out[n++];c->order=car_u32(r+4);
        c->name=gt4_specdb_string(m->car_strings,m->car_strings_size,car_u32(r+12));
        memcpy(c->rgb,r+36,3);c->kind=r[39]&2?1:2;memcpy(c->chip,r+36,16);
    }
    for(int i=0;i<n;i++)for(int j=i+1;j<n;j++)if(out[j].order<out[i].order){car_paint t=out[j];out[j]=out[i];out[i]=t;}
    return n;
}
static int nat_car_paints(ah_vm *vm,ah_obj *self,void *d,int argc,const ah_val *argv,ah_val *ret)
{
    menu *m=ah_user(vm);ah_val code;car_paint paints[128];
    int n=car_paints(m,car_label_of(vm,ah_objv(self)),paints,128);
    if(!d){ah_obj *a=ah_array_new(vm);for(int i=0;i<n;i++)ah_array_push(vm,a,ah_int(i));*ret=ah_objv(a);return 0;}
    ah_val current=ah_nil();ah_get_member(vm,self,"color_index",&current);int i=argc?(int)ah_num(argv[0]):(int)ah_num(current);
    *ret=ah_str(vm,i>=0 && i<n && paints[i].name && *paints[i].name?paints[i].name:"-");
    return 0;
}
/* ColorChipFace.setData(car_data, index): the chip shows that paint. */
static int nat_chip_set_data(ah_vm *vm,ah_obj *self,void *d,int argc,const ah_val *argv,ah_val *ret)
{
    (void)d;menu *m=ah_user(vm);ui_widget *w=ah_host_data(self);car_paint paints[128];
    /* refreshCar passes the owned car alone; showroom chips pass an explicit
       paint index. Read the owned color_index in the one-argument form. */
    ah_obj *data=argc?ah_as_obj(argv[0],AO_MODULE):NULL;ah_val current=ah_nil();
    if(data)ah_get_member(vm,data,"color_index",&current);
    int i=argc>1?(int)ah_num(argv[1]):(int)ah_num(current);
    int n=argc?car_paints(m,car_label_of(vm,argv[0]),paints,128):0;
    if(w){w->chip_kind=0;w->chip_band_count=0;}
    if(w && i>=0 && i<n){
        w->chip_kind=paints[i].kind;memcpy(w->chip_rgb,paints[i].rgb,3);
        for(int b=0;b<4;b++)if(paints[i].chip[b][3]&1)
            memcpy(w->chip_bands[w->chip_band_count++],paints[i].chip[b],3);
    }
    *ret=ah_nil();return 0;
}
static int nat_set_color_index(ah_vm *vm,ah_obj *self,void *d,int argc,const ah_val *argv,ah_val *ret)
{
    (void)d;menu *m=ah_user(vm);ui_widget *w=ah_host_data(self);
    /* arcade.ad SplitRoot.set_car invokes setColorIndex separately on
       P1.CarFace and P2.CarFace. Retaining only the last menu paint aliases them. */
    if(argc && isfinite(ah_num(argv[0]))){m->car_color=(int)ah_num(argv[0]);if(w)w->car_color_index=m->car_color;}
    *ret=ah_nil();return 0;
}
static const char *car_name(menu *m,const char *code)
{
    load_car_tables(m);uint32_t id;uint8_t row[256];
    if(gt4_specdb_label(m->car_labels,m->car_labels_size,code,&id) &&
       gt4_specdb_row(m->car_names,m->car_names_size,id,row,sizeof row)==16)
        return gt4_specdb_string(m->car_strings,m->car_strings_size,car_u32(row));
    return NULL;
}
/* func_00447BB8: GENERIC_CAR exact label, then TUNED_CAR exact label. */
static int nat_calendar_car_key(ah_vm *vm,ah_obj *self,void *data,int argc,const ah_val *argv,ah_val *ret){
    (void)self;menu *m=data;const char *label=argc?ah_cstr(argv[0]):NULL;
    if(!label)return ah_error(vm,"car key requires a label");load_car_tables(m);uint32_t id;
    if(gt4_specdb_label(m->car_labels,m->car_labels_size,label,&id)){*ret=ah_int(id);return 0;}
    size_t size=0;uint8_t *labels=gt4_vol_read(m->vol,"/specdb/GT4_US2560/TUNED_CAR.idi",&size);
    int found=gt4_specdb_label(labels,size,label,&id);free(labels);
    *ret=ah_int(found?(int64_t)(UINT64_C(0x2600000000)|id):-1);return 0;
}
/* func_004409D8 / 00440B18: original WHEEL/WING namespace lookup. */
static int nat_calendar_custom_key(ah_vm *vm,ah_obj *self,void *data,int argc,const ah_val *argv,ah_val *ret){
    (void)self;menu *m=ah_user(vm);int wing=(int)(intptr_t)data;
    const char *label=argc?ah_cstr(argv[0]):NULL;
    if(!label){*ret=ah_int(-1);return 0;}
    char path[96];snprintf(path,sizeof path,"/specdb/GT4_US2560/%s.idi",wing?"WING":"WHEEL");
    size_t size=0;uint8_t *labels=gt4_vol_read(m->vol,path,&size);uint32_t id;
    int found=gt4_specdb_label(labels,size,label,&id);free(labels);
    *ret=ah_int(found?(int64_t)(((uint64_t)(wing?30:29)<<32)|id):-1);return 0;
}
/* GarageRoot L12612 always queries riding_car.getGarageIndex, including
   after selling the selected car. Keep native profile nil semantics while
   exposing the original empty garage view (index -1) to authored UI. */
static int garage_ui_get(ah_vm *vm,ah_obj *self,const char *name,ah_val *out){
    if(strcmp(name,"riding_car"))return 0;
    ah_val index,cars;ah_get_member(vm,self,"riding_index",&index);ah_get_member(vm,self,"cars",&cars);
    *out=ah_array_get(cars.u.o,(int)ah_num(index));if(out->t!=AV_NIL)return 1;
    ah_val empty;if(!ah_get_member(vm,self,"_empty_riding_car",&empty)){
        ah_obj *car=ah_module_new(vm,"empty riding car",NULL,NULL,NULL);gt4_game_bind_car(vm,car);
        ah_set_member(vm,car,"garage_index",ah_int(-1));ah_set_member(vm,car,"car_code",ah_str(vm,""));
        empty=ah_objv(car);ah_set_member(vm,self,"_empty_riding_car",empty);
    }
    gt4_game_bind_car(vm,empty.u.o);*out=empty;return 1;
}
static int garage_ui_set(ah_vm *vm,ah_obj *self,const char *name,ah_val value){
    (void)value;if(strcmp(name,"riding_car"))return 0;ah_remove_member(vm,self,name);return 1;
}
static const ah_host garage_ui_host={"garage view",garage_ui_get,garage_ui_set,NULL,NULL,1};
/* 00170088/00170278/001705E0: occupied slots, optional filters,
   country/tuner enumeration ordered by their GENERIC_CAR numeric IDs. */
static int nat_garage_pickup(ah_vm *vm,ah_obj *self,void *data,int argc,const ah_val *argv,ah_val *ret){
    menu *m=ah_user(vm);load_car_tables(m);int mode=(int)(intptr_t)data;
    ah_module_attach_host(self,&garage_ui_host);ah_remove_member(vm,self,"riding_car");
    ah_val cars;ah_get_member(vm,self,"cars",&cars);ah_obj *out=ah_array_new(vm);
    ah_val enums[256];unsigned char used[256]={0};
    for(int i=0;i<ah_array_size(cars.u.o);i++){
        ah_val car=ah_array_get(cars.u.o,i);if(car.t==AV_NIL)continue;
        const char *label=car_label_of(vm,car);uint32_t id;uint8_t row[24];
        if(!label || !gt4_specdb_label(m->car_labels,m->car_labels_size,label,&id) || gt4_specdb_row(m->car_db,m->car_db_size,id,row,sizeof row)!=24)return ah_error(vm,"garage catalogue unavailable");
        ah_val arg=ah_str(vm,label),country,tuner;
        if(nat_car_metadata(vm,NULL,"GetCountry",1,&arg,&country)<0 || nat_car_metadata(vm,NULL,"GetTuner",1,&arg,&tuner)<0)return -1;
        const char *values[]={ah_cstr(country),ah_cstr(tuner),gt4_arcade_drivetrain(arcade_of(m),label)};int matches=1;
        int filters=mode==2?argc:mode==1?argc:0;
        for(int j=0;j<filters && j<3;j++){const char *filter=ah_cstr(argv[j]);if(filter && strcmp(filter,"all") && (!values[j] || strcmp(filter,values[j])))matches=0;}
        if(!matches)continue;
        if(mode==2)ah_array_push(vm,out,car);
        else {unsigned key=row[mode==0?14:16];used[key]=1;enums[key]=mode==0?country:tuner;}
    }
    if(mode!=2)for(int i=0;i<256;i++)if(used[i])ah_array_push(vm,out,enums[i]);
    *ret=ah_objv(out);return 0;
}
/* 001467A0: turbo first (00445650, TURBINEKIT byte +9),
   then original generic supercharger flag or equipped category 28 stage 1. */
static int nat_garage_aspiration(ah_vm *vm,ah_obj *self,void *data,int argc,const ah_val *argv,ah_val *ret){
    (void)data;(void)argc;(void)argv;menu *m=ah_user(vm);uint8_t body[0x4b8],row[256];
    if(!gt4_game_car_body(vm,self,body))return ah_error(vm,"garage aspiration body unavailable");
    unsigned slot=car_u32(body+0x488);if(slot>=3)return ah_error(vm,"garage setup unavailable");
    uint64_t key;memcpy(&key,body+slot*0x178+0xb0,8);
    if(key!=UINT64_MAX){ah_val arg=ah_int((int64_t)key),bytes;
        if(ah_call_member(vm,self,"_getSettingPartRow",1,&arg,&bytes)!=1)return -1;
        if(ah_array_size(bytes.u.o)>9 && ah_num(ah_array_get(bytes.u.o,9))){*ret=ah_int(1);return 0;}}
    load_car_tables(m);uint32_t id;const char *label=car_label_of(vm,ah_objv(self));
    if(!gt4_specdb_label(m->car_labels,m->car_labels_size,label,&id) || gt4_specdb_row(m->car_db,m->car_db_size,id,row,sizeof row)!=24)return -1;
    ah_val type=ah_int(28),stage;
    if(row[23]){*ret=ah_int(2);return 0;}
    if(ah_call_member(vm,self,"getEquipedParts",1,&type,&stage)!=1)return -1;
    *ret=ah_int(stage.u.i==1?2:0);return 0;
}
/* Registration00141B90 +0x2004/+0x20AC ->0013F598/0013F668.
   0013F528 +0x30..0x58 tests GENERIC_CAR+0x17, then the equipped
   supercharger stage (0043F4C8, type28) against1. NA additionally
   rejects TURBINEKIT+9 via00445650 +0xC..0x30. Queries only. */
static int nat_car_aspiration_predicate(ah_vm *vm,ah_obj *self,void *data,int argc,const ah_val *argv,ah_val *ret){
    (void)argv;menu *m=ah_user(vm);uint8_t body[0x4b8],row[24];
    if(argc || !gt4_game_car_body(vm,self,body))return ah_error(vm,"aspiration predicate requires owned car");
    unsigned slot=car_u32(body+0x488);if(slot>=3)return ah_error(vm,"invalid aspiration setup");
    uint64_t key;memcpy(&key,body+slot*0x178,8);load_car_tables(m);
    if((key>>32)==0x26){size_t n;uint8_t *db=gt4_vol_read(m->vol,"/specdb/GT4_US2560/ENEMY_CARS.dbt",&n),base[24];
        int ok=gt4_specdb_row(db,n,(uint32_t)key,base,sizeof base)>=8;free(db);
        if(!ok)return ah_error(vm,"original prepared-car key unavailable");memcpy(&key,base,8);}
    if((key>>32) || gt4_specdb_row(m->car_db,m->car_db_size,(uint32_t)key,row,sizeof row)!=24)
        return ah_error(vm,"original aspiration car row unavailable");
    int super=row[23]!=0;ah_val type=ah_int(28),stage;
    if(!super){if(ah_call_member(vm,self,"getEquipedParts",1,&type,&stage)<0)return -1;super=ah_num(stage)==1;}
    if(data==(void *)1){*ret=ah_bool(super);return 0;}
    memcpy(&key,body+slot*0x178+0xb0,8);int turbo=0;
    if(key!=UINT64_MAX){ah_val arg=ah_int((int64_t)key),bytes;
        if(ah_call_member(vm,self,"_getSettingPartRow",1,&arg,&bytes)<0)return -1;
        if(ah_array_size(bytes.u.o)>9)turbo=ah_num(ah_array_get(bytes.u.o,9))!=0;}
    *ret=ah_bool(data==(void *)2?turbo:!super && !turbo);return 0;
}
static int garage_name_compare(const char *a,const char *b){
    while(*a && *b){unsigned x=(unsigned char)*a++,y=(unsigned char)*b++;if(x>='A' && x<='Z')x+=32;if(y>='A' && y<='Z')y+=32;if(x!=y)return x<y?-1:1;}
    return (unsigned char)*a<(unsigned char)*b?-1:(unsigned char)*a>(unsigned char)*b;
}
/* 0043DD10 / 0043DDB0: HSV rank, saturation threshold 0.25,
   hue rotation 120 degrees, grey range 152 and final scale 1/512. */
static float garage_color_rank(menu *m,ah_vm *vm,ah_val car){
    car_paint paints[128];int count=car_paints(m,car_label_of(vm,car),paints,128);ah_val color;
    ah_get_member(vm,car.u.o,"color_index",&color);int index=(int)ah_num(color);if(index<0 || index>=count)return 0;
    car_paint *paint=&paints[index];float r=paint->rgb[0]/255.0f,g=paint->rgb[1]/255.0f,b=paint->rgb[2]/255.0f;
    float maximum=fmaxf(r,fmaxf(g,b)),minimum=fminf(r,fminf(g,b)),delta=maximum-minimum;
    float saturation=maximum?delta/maximum:0,hue=0;
    if(delta){if(maximum==r)hue=(g-b)/delta;else if(maximum==g)hue=(b-r)/delta+2;else hue=(r-g)/delta+4;hue*=60;if(hue<0)hue+=360;}
    float rank;if(saturation>=0.25f){hue+=120;if(hue>360)hue-=360;rank=hue+152;}else rank=(1-maximum)*152;
    rank*=1.0f/512.0f;
    if(!(paint->chip[0][3]&4) && (paint->chip[1][3]&1))rank+=1;
    return rank;
}
/* 00146D80..001479B8: compact power/mass/date and catalogue keys;
   00146F38 lowercases the original short name before comparing it.
   A: stable insertion order for equal keys awaits the PS2 sort oracle. */
static int garage_sort_key(ah_vm *vm,ah_val car,int mode,double *number,const char **text){
    uint8_t unit[32],row[24];menu *m=ah_user(vm);*text=NULL;*number=0;
    if(!gt4_game_car_unit(vm,car.u.o,unit))return -1;
    unsigned ps=(car_u32(unit+8)>>9)&0x3ffff,mass=car_u32(unit+12)&8191;
    switch(mode){
    case 0:*number=car_u32(unit+24)&1023;return 0;
    case 1:*number=ps;return 0;case 2:*number=mass;return 0;
    case 3:*number=(float)mass/((float)ps/100.0f);return 0;
    case 4:*number=car_u32(unit+16)&0xffffff;return 0;
    case 9:{ah_val name;ah_get_member(vm,car.u.o,"short_name",&name);*text=ah_cstr(name);return *text?0:-1;}
    case 10:*text=gt4_arcade_drivetrain(arcade_of(m),car_label_of(vm,car));return *text?0:-1;
    case 11:*number=garage_color_rank(m,vm,car);return 0;
    case 12:{ah_val aspiration;if(nat_garage_aspiration(vm,car.u.o,NULL,0,NULL,&aspiration)<0)return -1;*number=ah_num(aspiration);return 0;}
    }
    load_car_tables(m);uint32_t id;
    if(!gt4_specdb_label(m->car_labels,m->car_labels_size,car_label_of(vm,car),&id) || gt4_specdb_row(m->car_db,m->car_db_size,id,row,sizeof row)!=24)return -1;
    if(mode==5)*number=(int32_t)car_u32(row+8);else if(mode==6)*number=(int16_t)(row[12]|row[13]<<8);
    else if(mode==7)*number=row[14];else if(mode==8)*number=row[16];else return -1;
    return 0;
}
/* 00146E88 -> 00146A98: unsigned ten-bit ride-history ranks. */
static int nat_garage_sort_history(ah_vm *vm,ah_obj *self,void *data,int argc,const ah_val *argv,ah_val *ret){
    (void)self;int mode=(int)(intptr_t)data;if(!argc)return ah_error(vm,"garage sort requires an array");
    ah_obj *list=ah_as_obj(argv[0],AO_ARRAY);if(!list)return ah_error(vm,"garage sort requires an array");
    int up=argc<2 || ah_num(argv[1]);
    for(int i=1;i<ah_array_size(list);i++){ah_val value=ah_array_get(list,i);double rank;const char *text;
        if(garage_sort_key(vm,value,mode,&rank,&text)<0)return ah_error(vm,"garage sort key unavailable");int j=i;
        while(j){ah_val prior=ah_array_get(list,j-1);double previous;const char *prior_text;
            if(garage_sort_key(vm,prior,mode,&previous,&prior_text)<0)return ah_error(vm,"garage sort key unavailable");
            int order=text && prior_text?garage_name_compare(prior_text,text):previous<rank?-1:previous>rank?1:0;
            if(up?order<=0:order>=0)break;ah_array_set(list,j,prior);j--;}
        ah_array_set(list,j,value);
    }
    *ret=argv[0];return 0;
}
/* 001412A8 -> 001465D8: compact PS hundredths, rounded with +0.5.
   00435110: acquisition age plus the disc calendar epoch (2005-04-02). */
static int nat_owned_summary(ah_vm *vm,ah_obj *self,void *data,int argc,const ah_val *argv,ah_val *ret){
    (void)argc;(void)argv;uint8_t unit[32];
    if(!gt4_game_car_unit(vm,self,unit))return ah_error(vm,"owned car summary unavailable");
    if(!(intptr_t)data){uint32_t bits=car_u32(unit+8);*ret=ah_int((int)((float)((bits>>9)&0x3ffff)/100.0f+0.5f));}
    else if((intptr_t)data==2){/* 00146448: mass / PS, rounded in thousandths. */
        uint32_t ps=(car_u32(unit+8)>>9)&0x3ffff,mass=car_u32(unit+12)&8191;
        if(!ps || !mass)*ret=ah_str(vm,"----");else {int value=(int)((float)mass/((float)ps/100.0f)*1000.0f+0.5f);char text[40];snprintf(text,sizeof text,"%d.%03d",value/1000,value%1000);*ret=ah_str(vm,text);}
    }
    else {ah_val main_,menu_,calendar,epoch,args[]={ah_int(2005),ah_int(4),ah_int(2)};
        if(!ah_get_member(vm,ah_root(vm),"main",&main_) || !ah_get_member(vm,main_.u.o,"menu",&menu_) || !ah_get_member(vm,menu_.u.o,"MCalendar",&calendar) || ah_call_member(vm,calendar.u.o,"dateToDay",3,args,&epoch)!=1)return ah_error(vm,"garage acquisition epoch unavailable");
        *ret=ah_int((car_u32(unit+16)&0xffffff)+epoch.u.i);}
    return 0;
}
/* func_001402A8 -> 0044A2E0. CARS is the original counted parts list;
   namespace names and switch targets come from the user's CORE.GT4. */
static int nat_owned_parts_price(ah_vm *vm,ah_obj *self,void *user,int argc,const ah_val *argv,ah_val *ret){
    (void)user;menu *m=ah_user(vm);ah_val id;
    const gt4_career_part_data *rules=gt4_game_part_data();
    if(!m || !rules || argc<2 || (argv[0].t!=AV_INT && argv[0].t!=AV_LONG) || (argv[1].t!=AV_INT && argv[1].t!=AV_LONG) || !ah_get_member(vm,self,"spec_id",&id))
        return ah_error(vm,"getPartsPrice requires original part data, a resolved car and integer type/stage");
    char path[160];snprintf(path,sizeof path,"/specdb/GT4_US2560/CARS/%016llX",(unsigned long long)(uint32_t)id.u.i);
    size_t size=0;uint8_t *list=gt4_vol_read(m->vol,path,&size);
    uint64_t key=gt4_career_part_key(list,size,argv[0].u.i,argv[1].u.i);free(list);*ret=ah_int(-1);
    if(key==UINT64_MAX || argv[0].u.i<1 || argv[0].u.i>31)return 0;
    uint32_t branch=rules->price_branches[(unsigned)argv[0].u.i-1];
    if(branch==0x44a458 || branch==0x44a45c){*ret=ah_int(gt4_career_part_row_price(branch,NULL,0));return 0;}
    unsigned table=(unsigned)(key>>32);if(table>=32)return 0;
    snprintf(path,sizeof path,"/specdb/GT4_US2560/%s.dbt",rules->table_names[table]);
    uint8_t *db=gt4_vol_read(m->vol,path,&size),row[4096];
    int width=gt4_specdb_row(db,size,(uint32_t)key&65535u,row,sizeof row);free(db);
    if(width>0)*ret=ah_int(gt4_career_part_row_price(branch,row,(size_t)width));return 0;
}
/* 001401A8 -> 0044A048: an existing part is any result other than
   UINT64_MAX. Category 31 is the original special TIRESIZE key. */
static int nat_owned_parts_exist(ah_vm *vm,ah_obj *self,void *data,int argc,const ah_val *argv,ah_val *ret){
    menu *m=data;ah_val id;
    if(argc<2 || (argv[0].t!=AV_INT && argv[0].t!=AV_LONG) ||
       (argv[1].t!=AV_INT && argv[1].t!=AV_LONG) || !ah_get_member(vm,self,"spec_id",&id))
        return ah_error(vm,"isExistParts requires resolved car and type/stage");
    char path[160];snprintf(path,sizeof path,"/specdb/GT4_US2560/CARS/%016llX",(unsigned long long)(uint32_t)id.u.i);
    size_t size=0;uint8_t *list=gt4_vol_read(m->vol,path,&size);
    uint64_t key=gt4_career_part_key(list,size,(int32_t)(uint32_t)argv[0].u.i,(int32_t)(uint32_t)argv[1].u.i);free(list);
    *ret=ah_int(key!=UINT64_MAX);return 0;
}
/* 001403A0 resolves type/stage through the same original CARS lookup. */
static int nat_setting_part_key(ah_vm *vm,ah_obj *self,void *data,int argc,const ah_val *argv,ah_val *ret){
    menu *m=data;ah_val id;if(argc!=2 || !ah_get_member(vm,self,"spec_id",&id) ||
       (argv[0].t!=AV_INT && argv[0].t!=AV_LONG) || (argv[1].t!=AV_INT && argv[1].t!=AV_LONG))return ah_error(vm,"part key requires car/type/stage");
    char path[160];snprintf(path,sizeof path,"/specdb/GT4_US2560/CARS/%016llX",(unsigned long long)(uint32_t)id.u.i);
    size_t size=0;uint8_t *list=gt4_vol_read(m->vol,path,&size);
    uint64_t key=gt4_career_part_key(list,size,(int32_t)(uint32_t)argv[0].u.i,(int32_t)(uint32_t)argv[1].u.i);free(list);
    int64_t signed_key;memcpy(&signed_key,&key,8);*ret=ah_int(signed_key);return 0;
}
/* 00147D08 -> 00449E10 +0x40..0xD4: cache this car's original CARS
   entries on beginSetting. This is disc data, never synthetic defaults. */
static int nat_cache_setting_parts(ah_vm *vm,ah_obj *self,void *data,int argc,const ah_val *argv,ah_val *ret){
 (void)argv;menu *m=data;ah_val id;
 if(argc || !ah_get_member(vm,self,"spec_id",&id))return ah_error(vm,"setting car key unavailable");
 char path[160];snprintf(path,sizeof path,"/specdb/GT4_US2560/CARS/%016llX",(unsigned long long)(uint32_t)id.u.i);
 size_t size=0;uint8_t *list=gt4_vol_read(m->vol,path,&size);
 int ok=list && size>=8 && car_u32(list)<=((size-8)/16) && gt4_career_blob_set(vm,self,"_setting_car_parts_ps2",list,size);
 free(list);if(!ok)return ah_error(vm,"original CARS list unavailable");*ret=ah_nil();return 0;
}
/* 00443ED0 -> 00443F00: signed low32 row id, high32 namespace.
   Missing table/row leaves the caller's existing buffer untouched. */
static int nat_setting_part_row(ah_vm *vm,ah_obj *self,void *data,int argc,const ah_val *argv,ah_val *ret){
    (void)self;menu *m=data;const gt4_career_part_data *rules=gt4_game_part_data();
    if(argc!=1 || !rules || (argv[0].t!=AV_INT && argv[0].t!=AV_LONG))return ah_error(vm,"setting part key unavailable");
    uint64_t key=(uint64_t)argv[0].u.i;unsigned table=(unsigned)(key>>32);*ret=ah_nil();
    if(table>=39 || !rules->table_names[table][0])return 0;
    char path[160];snprintf(path,sizeof path,"/specdb/GT4_US2560/%s.dbt",rules->table_names[table]);
    size_t size=0;uint8_t *db=gt4_vol_read(m->vol,path,&size),row[256];
    int32_t id;uint32_t bits=(uint32_t)key;memcpy(&id,&bits,4);
    int index=gt4_specdb_row_index(db,size,(uint64_t)(int64_t)id);
    int width=index<0?0:gt4_specdb_row_at(db,size,(uint32_t)index,row,sizeof row);free(db);
    if(width<=0)return 0;ah_obj *bytes=ah_array_new(vm);
    for(int i=0;i<width;i++)ah_array_push(vm,bytes,ah_int(row[i]));*ret=ah_objv(bytes);return 0;
}
/* 004462E0 -> 00443ED0: TRANSMISSION row byte +0x3a. */
static int nat_transmission_type(ah_vm *vm,ah_obj *self,void *data,int argc,const ah_val *argv,ah_val *ret){
    ah_val row;if(nat_setting_part_row(vm,self,data,argc,argv,&row)<0)return -1;
    ah_obj *bytes=ah_as_obj(row,AO_ARRAY);
    if(!bytes || ah_array_size(bytes)<=0x3a)return ah_error(vm,"transmission row unavailable");
    *ret=ah_array_get(bytes,0x3a);return 0;
}
/* 00140FA8 +0x70..+0x98 prepends GENERIC_CAR +0x10 tuner unless
   its enum is "nothing"; +0x170..+0x180 appends TUNER_LIST bytes,
   004478F8 +0x4c..+0xa4 stops at zero, with at most ten entries. */
static int nat_car_tuners(ah_vm *vm,ah_obj *self,void *data,int argc,const ah_val *argv,ah_val *ret){
    (void)argv;menu *m=data;ah_val id;
    const gt4_career_part_data *rules=gt4_game_part_data();
    if(argc || !rules || !ah_get_member(vm,self,"spec_id",&id))return ah_error(vm,"tuner list requires resolved original car");
    load_car_tables(m);uint8_t car[24],tuners[10];ah_obj *list=ah_array_new(vm);
    if(gt4_specdb_row(m->car_db,m->car_db_size,(uint32_t)id.u.i,car,sizeof car)!=24)return ah_error(vm,"original tuner car row unavailable");
    const char *name=rules->tuner_names[car[16]];
    if(!name[0])return ah_error(vm,"unknown original car tuner enum");
    if(strcmp(name,"nothing"))ah_array_push(vm,list,ah_str(vm,name));
    size_t size=0;uint8_t *db=gt4_vol_read(m->vol,"/specdb/GT4_US2560/TUNER_LIST.dbt",&size);
    int count=gt4_specdb_row(db,size,(uint32_t)id.u.i,tuners,sizeof tuners);free(db);
    if(count>0 && count!=10)return ah_error(vm,"invalid original tuner list row");
    for(int i=0;i<count && tuners[i];i++){
        name=rules->tuner_names[tuners[i]];
        if(!name[0])return ah_error(vm,"unknown original tuner enum");
        ah_array_push(vm,list,ah_str(vm,name));
    }
    *ret=ah_objv(list);return 0;
}
/* car.canTune(tuner, ...) (00140D68): whether any named tuner (00448DC8,
   the enum by name; unknown names are -1 and never match) is the car's
   own, GENERIC_CAR +0x10 (00448788, "nothing" included), or one of its
   TUNER_LIST entries (004478F8). */
static int nat_car_can_tune(ah_vm *vm,ah_obj *self,void *data,int argc,const ah_val *argv,ah_val *ret){
    menu *m=data;ah_val id;const gt4_career_part_data *rules=gt4_game_part_data();
    if(!rules || !ah_get_member(vm,self,"spec_id",&id))return ah_error(vm,"canTune requires resolved original car");
    load_car_tables(m);uint8_t car[24],tuners[10];
    if(gt4_specdb_row(m->car_db,m->car_db_size,(uint32_t)id.u.i,car,sizeof car)!=24)return ah_error(vm,"original tuner car row unavailable");
    size_t size=0;uint8_t *db=gt4_vol_read(m->vol,"/specdb/GT4_US2560/TUNER_LIST.dbt",&size);
    int count=gt4_specdb_row(db,size,(uint32_t)id.u.i,tuners,sizeof tuners);free(db);
    if(count<0)count=0;if(count>10)return ah_error(vm,"invalid original tuner list row");
    int found=0;
    for(int a=0;a<argc && !found;a++){
        char text[64];const char *name=ah_to_text(argv[a],text,sizeof text);int tuner=-1;
        for(int k=0;k<256 && name;k++)if(rules->tuner_names[k][0] && !strcmp(rules->tuner_names[k],name)){tuner=k;break;}
        if(tuner<0)continue;
        if(car[16]==tuner)found=1;
        for(int i=0;i<count && !found;i++)if(tuners[i]==tuner)found=1;
    }
    *ret=ah_bool(found);return 0;
}
/* GT Auto owned-car API: 0013F168/0013F288 -> 00448980/00448970;
   0014D9B0/0014DBA0 -> 00440938/00440988 (live +0x470);
   0014DF50/0014E0B8 -> 00440A10/00440A68 (setup +0x100).
   Body serialization omits the original eight-byte class header. */
static int nat_car_customization(ah_vm *vm,ah_obj *self,void *data,int argc,const ah_val *argv,ah_val *ret){
    menu *m=ah_user(vm);const char *op=data;ah_val id;uint8_t body[0x4b8];
    const gt4_career_part_data *rules=gt4_game_part_data();
    if(!m || !rules || !ah_get_member(vm,self,"spec_id",&id))return ah_error(vm,"customization requires original car data");
    *ret=ah_nil();
    if(!strcmp(op,"isChangeWheelDisabled") || !strcmp(op,"isChangeWingDisabled")){
        uint8_t row[24];load_car_tables(m);
        if(argc || gt4_specdb_row(m->car_db,m->car_db_size,(uint32_t)id.u.i,row,sizeof row)!=24)return ah_error(vm,"customization flags unavailable");
        *ret=ah_int(row[!strcmp(op,"isChangeWheelDisabled")?21:22]!=0);return 0;
    }
    int wheel=strstr(op,"Wheel")!=NULL;
    if(argc!=(wheel?2:1) || !gt4_game_car_body(vm,self,body))return ah_error(vm,"customization requires owned car and selection");
    unsigned category=0;
    if(wheel){const char *cat=ah_cstr(argv[0]);for(;category<4;category++)if(cat && !strcmp(cat,rules->wheel_categories[category]))break;
        if(category==4)return ah_error(vm,"invalid wheel category");}
    const char *label=ah_cstr(argv[wheel?1:0]);
    /* Original wrappers map w000/p000 to a null label (factory equipment). */
    if(label && !strcmp(label,wheel?"w000":"p000"))label=NULL;
    uint64_t key=UINT64_MAX;
    if(label){char path[128];snprintf(path,sizeof path,"/specdb/GT4_US2560/%s.idi",wheel?"WHEEL":"WING");size_t size=0;
        uint8_t *idi=gt4_vol_read(m->vol,path,&size);uint32_t piece;int found=gt4_specdb_label(idi,size,label,&piece);free(idi);
        if(found)key=((uint64_t)(wheel?29:30)<<32)|piece;}
    uint32_t slot=car_u32(body+0x488);if(slot>=3)return ah_error(vm,"invalid customization setup");
    unsigned offset=wheel?0x468+category*8:slot*0x178+0x100;uint64_t current;memcpy(&current,body+offset,8);
    if(!strncmp(op,"have",4)){*ret=ah_int(current==key);return 0;}
    if(wheel)memcpy(body+offset,&key,8);
    else for(unsigned i=0;i<3;i++)memcpy(body+i*0x178+0x100,&key,8);
    if(!gt4_career_blob_set(vm,self,"full_car_ps2",body,sizeof body))return ah_error(vm,"customization write failed");
    ah_val callback;if(ah_get_member(vm,self,"_markDirty",&callback))ah_call(vm,callback,0,NULL,ret);
    *ret=ah_nil();return 0;
}
/* Original loadPartsInfo (0013FE78 -> 00449E10) loads CARS for the
   selected car. Native catalog methods read that same file directly. */
static int nat_car_parts_catalog(ah_vm *vm,ah_obj *self,void *data,int argc,const ah_val *argv,ah_val *ret){
    menu *m=ah_user(vm);const char *op=data;ah_val id;const gt4_career_part_data *rules=gt4_game_part_data();
    if(!m || !rules || !ah_get_member(vm,self,"spec_id",&id))return ah_error(vm,"parts catalog unavailable");
    /* 0013E370 -> 00445D90: active CHASSIS +0 length in millimetres,
       formatted as decimal text by the wrapper. */
    if(!strcmp(op,"getLength")){
        uint8_t body[0x4b8];if(argc || !gt4_game_car_body(vm,self,body))return ah_error(vm,"car length requires owned car");
        uint32_t slot=car_u32(body+0x488);if(slot>=3)return ah_error(vm,"invalid length setup");
        int64_t key;memcpy(&key,body+slot*0x178+0x30,8);ah_val arg=ah_int(key),row;
        if(nat_setting_part_row(vm,self,m,1,&arg,&row)<0)return -1;ah_obj *bytes=ah_as_obj(row,AO_ARRAY);
        if(ah_array_size(bytes)<2)return ah_error(vm,"original chassis row unavailable");
        char text[24];snprintf(text,sizeof text,"%u",(unsigned)ah_array_get(bytes,0).u.i|((unsigned)ah_array_get(bytes,1).u.i<<8));
        *ret=ah_str(vm,text);return 0;
    }
    if(!strcmp(op,"getPartsTypeMax")){
        if(argc!=1)return ah_error(vm,"getPartsTypeMax requires category");int64_t type=(int64_t)ah_num(argv[0]);
        *ret=ah_int(type>=1 && type<=32?rules->type_max[type-1]:0);return 0;
    }
    char path[128];snprintf(path,sizeof path,"/specdb/GT4_US2560/CARS/%016llX",(unsigned long long)(uint32_t)id.u.i);
    size_t size=0;uint8_t *list=gt4_vol_read(m->vol,path,&size);if(!list)return ah_error(vm,"original parts list missing");
    if(!strcmp(op,"loadPartsInfo")){free(list);*ret=ah_nil();return 0;}
    /* 001408A0 -> 0043F3E0: only lightweight type 8 has sequential
       ownership prerequisites. Missing preceding stage also permits it. */
    if(!strcmp(op,"isAllowedParts")){
        if(argc!=2){free(list);return ah_error(vm,"isAllowedParts requires type/stage");}
        int32_t type=(int32_t)(uint32_t)argv[0].u.i,stage=(int32_t)(uint32_t)argv[1].u.i;int allowed=1;
        if(type==8 && stage>(rules->normal_flags[7]?0:1)){
            ah_val args[]={ah_int(type),ah_int((int32_t)((uint32_t)stage-1))},owned;
            if(ah_call_member(vm,self,"isHavingParts",2,args,&owned)!=1){free(list);return ah_error(vm,"lightweight ownership unavailable");}
            allowed=ah_num(owned)!=0;
            if(!allowed)allowed=stage>0 && gt4_career_part_key(list,size,type,(int32_t)((uint32_t)stage-1))==UINT64_MAX;
        }
        free(list);*ret=ah_int(allowed);return 0;
    }
    if(argc<1){free(list);return ah_error(vm,"parts type required");}int64_t type=(int64_t)ah_num(argv[0]);unsigned count=0;
    if(size>=8 && car_u32(list)<=(size-8)/16)for(unsigned i=0;i<car_u32(list);i++)if((int32_t)car_u32(list+8+i*16+4)==type)count++;
    /* 0044A128: one-entry categories are available only without stage 0. */
    *ret=ah_int(count==1?gt4_career_part_key(list,size,type,0)==UINT64_MAX:count);free(list);return 0;
}
/* 00141B90 +0x1D14 registers isOpenCar -> 0013F040 +0x30..0x64:
   00441248 selects the active setup, 004454C0 reads its GENERIC_CAR key,
   00448820 resolves the row and 004489A0 +0..C returns byte +0x12 bit 4.
   This is an inscription predicate, with no vehicle-physics mutation. */
static int nat_car_is_open(ah_vm *vm,ah_obj *self,void *data,int argc,const ah_val *argv,ah_val *ret){
    (void)argv;menu *m=data;uint8_t body[0x4b8],row[24];
    if(argc || !gt4_game_car_body(vm,self,body))return ah_error(vm,"isOpenCar requires an owned car");
    uint32_t slot=car_u32(body+0x488);if(slot>=3)return ah_error(vm,"invalid open-car setup");
    uint64_t key;memcpy(&key,body+slot*0x178,8);load_car_tables(m);
    /* 00448820 ->00448A58 +0x8..0x48 resolves an ENEMY_CARS key
       through its first qword before querying the generic row. */
    if((key>>32)==0x26){
        size_t size=0;uint8_t *table=gt4_vol_read(m->vol,"/specdb/GT4_US2560/ENEMY_CARS.dbt",&size),tuned[32];
        int found=gt4_specdb_row(table,size,(uint32_t)key,tuned,sizeof tuned)>=8;free(table);
        if(!found){*ret=ah_bool(0);return 0;}memcpy(&key,tuned,8);
    }
    *ret=ah_bool((key>>32)==0 && gt4_specdb_row(m->car_db,m->car_db_size,(uint32_t)key,row,sizeof row)==24 && ((row[18]>>4)&1));
    return 0;
}
static int nat_bind_game_car(ah_vm *vm,ah_obj *self,void *data,int argc,const ah_val *argv,ah_val *ret){
    (void)self;menu *m=data;ah_obj *car=argc==1?ah_as_obj(argv[0],AO_MODULE):NULL;
    if(!car)return ah_error(vm,"car binding requires a car");
    load_car_tables(m);ah_val code,number,table;const char *label=NULL;uint32_t id;
    if(ah_get_member(vm,car,"car_code",&code))label=ah_cstr(code);
    if(!label && ah_get_member(vm,car,"code",&number)){
        label=ah_cstr(number);
        if(!label && (number.t==AV_INT || number.t==AV_LONG) && ah_get_member(vm,car,"table",&table) && table.u.i==0)label=gt4_specdb_label_for_id(m->car_labels,m->car_labels_size,(uint32_t)number.u.i);
    }
    if(!label || !gt4_specdb_label(m->car_labels,m->car_labels_size,label,&id))return ah_error(vm,"unsupported car key in imported garage");
    ah_set_member(vm,car,"spec_id",ah_int(id));ah_set_member(vm,car,"car_code",ah_str(vm,label));
    /* OilChangeRoot L12042 queries the owned car after save restore. */
    bind_garage_values(vm,car);
    const char *name=car_name(m,label);if(name){ah_set_member(vm,car,"name",ah_str(vm,name));
        uint8_t name_row[16];const char *short_label=NULL;
        if(gt4_specdb_row(m->car_names,m->car_names_size,id,name_row,sizeof name_row)==16)short_label=gt4_specdb_string(m->car_strings,m->car_strings_size,car_u32(name_row+8));
        if(!short_label)short_label=name;ah_set_member(vm,car,"short_name",ah_str(vm,short_label));
        /* 0013FBF8: truncate at a backtick followed by two decimal digits. */
        char short_name[256];snprintf(short_name,sizeof short_name,"%s",short_label);char *year=strchr(short_name,0x60);
        if(year && year[1]>='0' && year[1]<='9' && year[2]>='0' && year[2]<='9')*year=0;
        ah_set_member(vm,car,"short_name_no_year",ah_str(vm,short_name));}
    uint8_t row[24];if(gt4_specdb_row(m->car_db,m->car_db_size,id,row,sizeof row)!=24)return ah_error(vm,"original car price row unavailable");
    uint32_t price_bits=car_u32(row+8);int32_t base;memcpy(&base,&price_bits,4);
    ah_set_member(vm,car,"original_base_price",ah_int(base));
    ah_set_member(vm,car,"getPartsPrice",ah_bind(vm,car,ah_native(vm,"getPartsPrice",nat_owned_parts_price,m)));
    ah_set_member(vm,car,"getTunerList",ah_bind(vm,car,ah_native(vm,"getTunerList",nat_car_tuners,m)));
    ah_set_member(vm,car,"canTune",ah_bind(vm,car,ah_native(vm,"canTune",nat_car_can_tune,m)));
    ah_set_member(vm,car,"isOpenCar",ah_bind(vm,car,ah_native(vm,"isOpenCar",nat_car_is_open,m)));
    ah_set_member(vm,car,"isNAtune",ah_bind(vm,car,ah_native(vm,"isNAtune",nat_car_aspiration_predicate,NULL)));
    ah_set_member(vm,car,"isSuperCharger",ah_bind(vm,car,ah_native(vm,"isSuperCharger",nat_car_aspiration_predicate,(void *)1)));
    /* 00143AF0 registration ->0013F488 ->00146758 ->00445650. */
    ah_set_member(vm,car,"isTurbo",ah_bind(vm,car,ah_native(vm,"isTurbo",nat_car_aspiration_predicate,(void *)2)));
    const char *custom[]={"isChangeWheelDisabled","isChangeWingDisabled","haveTireCategoryWheel","setTireCategoryWheel","haveWing","setWing"};
    for(unsigned i=0;i<6;i++)ah_set_member(vm,car,custom[i],ah_bind(vm,car,ah_native(vm,custom[i],nat_car_customization,(void *)custom[i])));
    const char *catalog[]={"loadPartsInfo","getPartsTypeMax","isExistPartsType","getLength","isAllowedParts"};
    for(unsigned i=0;i<5;i++)ah_set_member(vm,car,catalog[i],ah_bind(vm,car,ah_native(vm,catalog[i],nat_car_parts_catalog,(void *)catalog[i])));
    gt4_game_bind_car(vm,car);ah_set_member(vm,car,"_cacheSettingParts",ah_bind(vm,car,ah_native(vm,"_cacheSettingParts",nat_cache_setting_parts,m)));ah_set_member(vm,car,"isExistParts",ah_bind(vm,car,ah_native(vm,"isExistParts",nat_owned_parts_exist,m)));ah_set_member(vm,car,"_getSettingPartKey",ah_bind(vm,car,ah_native(vm,"_getSettingPartKey",nat_setting_part_key,m)));ah_set_member(vm,car,"_getSettingPartRow",ah_bind(vm,car,ah_native(vm,"_getSettingPartRow",nat_setting_part_row,m)));ah_set_member(vm,car,"_getTransmissionType",ah_bind(vm,car,ah_native(vm,"_getTransmissionType",nat_transmission_type,m)));ah_set_member(vm,car,"getColorName",ah_bind(vm,car,ah_native(vm,"getColorName",nat_car_paints,(void *)1)));ah_set_member(vm,car,"_markDirty",ah_native(vm,"_markDirty",nat_need_save,m));*ret=argv[0];return 0;
}
static int nat_car_metadata(ah_vm *vm,ah_obj *self,void *data,int argc,const ah_val *argv,ah_val *ret)
{
    (void)self;menu *m=ah_user(vm);const char *code=argc?ah_cstr(argv[0]):NULL;
    const char *method=data;*ret=ah_nil();if(!code)return ah_error(vm,"car code required");
    load_car_tables(m);uint32_t id;uint8_t row[256];
    if(!gt4_specdb_label(m->car_labels,m->car_labels_size,code,&id) ||
       gt4_specdb_row(m->car_db,m->car_db_size,id,row,sizeof row)!=24)return ah_error(vm,"unknown car %s",code);
    /* 0012FEA8 +0xa8 -> 004480D8: CAR_NAME description ID +0x0c. */
    if(!strcmp(method,"GetDescriptionID")){
        uint8_t name[16];if(gt4_specdb_row(m->car_names,m->car_names_size,id,name,sizeof name)!=16)return ah_error(vm,"car description unavailable");
        *ret=ah_int((int32_t)car_u32(name+12));return 0;
    }
    if(!strcmp(method,"GetCategory")){
        const gt4_career_part_data *rules=gt4_game_part_data();
        if(!rules)return ah_error(vm,"original car category data not loaded");
        const char *category=gt4_career_car_category_name(rules,row);
        *ret=ah_str(vm,category?category:rules->category_fallback);return 0;
    }
    /* 0012EBA0 -> 00448788 -> 00448DF0: tuner byte +0x10,
       which can differ from the manufacturer byte +0x0f. */
    if(!strcmp(method,"GetTuner")){
        const gt4_career_part_data *rules=gt4_game_part_data();
        if(!rules || !rules->tuner_names[row[16]][0])return ah_error(vm,"original tuner enum unavailable");
        *ret=ah_str(vm,rules->tuner_names[row[16]]);return 0;
    }
    /* func_001300D0 -> func_00448868: signed lw GENERIC_CAR +8.
       func_00130300 -> func_004489A0(row,0): lbu +18, bit zero. */
    if(!strcmp(method,"GetPrice")){uint32_t bits=car_u32(row+8);int32_t price;memcpy(&price,&bits,4);*ret=ah_int(price);return 0;}
    if(!strcmp(method,"NotForSell")){*ret=ah_int(row[18]&1u);return 0;}
    if(!strcmp(method,"GetCarLabel")){*ret=ah_str(vm,code);return 0;}
    if(!strcmp(method,"GetOpenModelType")){*ret=ah_int(gt4_career_car_open_type(row));return 0;}
    if(!strcmp(method,"GetModelYear")){*ret=ah_int(gt4_career_car_year(row));return 0;}
    /* 0012EE08 -> 004488A8/00448E50: native country enum. */
    if(!strcmp(method,"GetCountry")){
        const gt4_career_part_data *rules=gt4_game_part_data();
        if(!rules || !rules->country_names[row[14]][0])return ah_error(vm,"original country enum unavailable");
        *ret=ah_str(vm,rules->country_names[row[14]]);return 0;
    }
    /* Manufacturer and country labels are authored in the menu's catalog. */
    ah_val root,nations,makerdata,catalog;
    if(ah_get_member(vm,m->project,"MakerRoot",&root) &&
       ah_get_member(vm,ah_as_obj(root,AO_MODULE),"nation_list",&nations) &&
       ah_get_member(vm,m->project,"MakerData",&makerdata) && ah_get_member(vm,m->project,"CarData",&catalog)) {
        ah_obj *ns=ah_as_obj(nations,AO_ARRAY);
        for(int i=0;i<ah_array_size(ns);i++) {
            const char *nation=ah_cstr(ah_array_get(ns,i));char key[128];ah_val makers;
            snprintf(key,sizeof key,"%s_makers",nation);
            if(!ah_get_member(vm,ah_as_obj(makerdata,AO_MODULE),key,&makers))continue;
            ah_obj *ms=ah_as_obj(makers,AO_ARRAY);
            for(int j=0;j<ah_array_size(ms);j++) {
                const char *maker=ah_cstr(ah_array_get(ms,j));ah_val cars;
                snprintf(key,sizeof key,"%s_carlist",maker);
                if(!ah_get_member(vm,ah_as_obj(catalog,AO_MODULE),key,&cars))continue;
                ah_obj *cs=ah_as_obj(cars,AO_ARRAY);
                for(int k=0;k<ah_array_size(cs);k++) {
                    const char *entry=ah_cstr(ah_array_get(cs,k));
                    if(entry && !strcmp(entry,code)) {
                        if(!strcmp(method,"GetCountry")) {
                            ah_val codes;
                            if(ah_get_member(vm,ah_as_obj(root,AO_MODULE),"nation_list2",&codes)) {
                                *ret=ah_array_get(ah_as_obj(codes,AO_ARRAY),i);return 0;
                            }
                            return ah_error(vm,"missing country code for %s",code);
                        }
                        *ret=ah_str(vm,maker);return 0;
                    }
                }
            }
        }
    }
    return ah_error(vm,"missing manufacturer for %s",code);
}
static int nat_language(ah_vm *vm,ah_obj *self,void *d,int argc,const ah_val *argv,ah_val *ret)
{(void)self;(void)argc;(void)argv;if(d){*ret=ah_nil();return 0;}*ret=ah_str(vm,"US");return 0;}

static int nat_empty_favorites(ah_vm *vm,ah_obj *self,void *d,int argc,const ah_val *argv,ah_val *ret)
{
    (void)vm;(void)self;(void)d;(void)argc;(void)argv;*ret=ah_int(0);return 0;
}

static int nat_calendar_day_first(ah_vm *vm,ah_obj *self,void *d,int argc,const ah_val *argv,ah_val *ret)
{
    (void)vm;(void)self;(void)d;(void)argc;(void)argv;*ret=ah_int(1);return 0;
}

static int nat_car_available(ah_vm *vm,ah_obj *self,void *data,int argc,const ah_val *argv,ah_val *ret)
{
    (void)self;menu *m=data;ah_val key;
    *ret=ah_bool(0);if(!argc || nat_calendar_car_key(vm,NULL,m,1,argv,&key)<0 || key.u.i==-1)return 0;
    ah_obj *available=profile_module(vm,game_object(m),"available");ah_val all,list,legacy;
    if(m->preview_courses || (ah_get_member(vm,available,"all",&all) && ah_truthy(all))){*ret=ah_bool(1);return 0;}
    /* boot.ad initAvailable iterates US_carlist.InitData.car_carlist.
       Imported PS2 lists use 0042EAC8 -> 0042EA80, 1024 keys at +8,
       with the same override at +2408 as courses. Do not require the
       catalog-preview debug flag to select a car in a normal Arcade session. */
    if(!ah_get_member(vm,available,"cars",&list)){
        size_t size=0;uint8_t *bytes=gt4_vol_read(m->vol,"/projects/boot/US_carlist.adc",&size);char error[256];
        ah_code *script=bytes?ah_load(bytes,size,error,sizeof error):NULL;free(bytes);
        if(!script)return ah_error(vm,"initial Arcade car list unavailable");
        ah_vm *seed=ah_vm_new();ah_val init,labels;int ok=ah_run_script(seed,script)>=0 &&
            ah_get_member(seed,ah_root(seed),"InitData",&init) && ah_get_member(seed,init.u.o,"car_carlist",&labels);
        ah_obj *cars=ah_array_new(vm);
        if(ok)for(int i=0;i<ah_array_size(labels.u.o);i++){
            ah_val arg=ah_str(vm,ah_cstr(ah_array_get(labels.u.o,i))),car_key;
            if(nat_calendar_car_key(vm,NULL,m,1,&arg,&car_key)<0 || car_key.u.i==-1){ok=0;break;}
            ah_obj *entry=ah_module_new(vm,"car",NULL,NULL,NULL);
            ah_set_member(vm,entry,"code",ah_int((int32_t)car_key.u.i));ah_set_member(vm,entry,"table",ah_int((int32_t)((uint64_t)car_key.u.i>>32)));
            ah_array_push(vm,cars,ah_objv(entry));
        }
        ah_vm_free(seed);if(!ok)return ah_error(vm,"invalid original initial Arcade cars");
        list=ah_objv(cars);ah_set_member(vm,available,"cars",list);
    }
    const char *label=ah_cstr(argv[0]);
    /* Preserve dated/purchased unlocks already recorded by the GT Mode adapter. */
    if(ah_get_member(vm,available,label,&legacy) && ah_truthy(legacy)){*ret=ah_bool(1);return 0;}
    for(int i=0;i<ah_array_size(ah_as_obj(list,AO_ARRAY));i++){
        ah_obj *entry=ah_as_obj(ah_array_get(list.u.o,i),AO_MODULE);ah_val code,table;
        if(entry && ah_get_member(vm,entry,"code",&code) && ah_get_member(vm,entry,"table",&table) &&
           ((uint64_t)(uint32_t)code.u.i|((uint64_t)(uint32_t)table.u.i<<32))==(uint64_t)key.u.i){*ret=ah_bool(1);break;}
    }
    return 0;
}

/* The original Misc apply resets the demonstration schedule before updating
   Auto Save. Keep the schedule state in game.demonstration so this call cannot
   abort the remaining assignments. Playback consumes this state separately. */
static int nat_demo_reset(ah_vm *vm,ah_obj *self,void *data,int argc,const ah_val *argv,ah_val *ret)
{
    (void)self;(void)argc;(void)argv;menu *m=data;demo_movie_count=0;ah_val demo;
    if(ah_get_member(vm,game_object(m),"demonstration",&demo))
        ah_set_member(vm,ah_as_obj(demo,AO_MODULE),"movie_count",ah_int(0));
    *ret=ah_nil();return 0;
}

static int nat_save_status(ah_vm *vm,ah_obj *self,void *data,int argc,const ah_val *argv,ah_val *ret)
{(void)vm;(void)self;(void)argc;(void)argv;*ret=ah_bool(data?gt4_save_dirty():gt4_save_active()!=0);return 0;}
static int nat_save_auto(ah_vm *vm,ah_obj *self,void *data,int argc,const ah_val *argv,ah_val *ret)
{(void)self;(void)argc;(void)argv;menu *m=data;gt4_save_capture(vm,game_object(m),gt4_save_dirty());int ok=!gt4_save_dirty() || !gt4_save_auto_enabled() || !gt4_save_active() || gt4_save_store();*ret=ah_str(vm,ok?"NO_ERROR":"IO_ERROR");return 0;}
/* PC storage adapters for the authored acquisition sequence. These replace
   PS2 device I/O, never the verified garage/money/calendar game rules. */
/* P: local profile files replace PS2 card I/O for both Arcade garages
   and Home Trade. The original callers consume [status, game]. */
void menu_set_photo_saver(menu *m,int (*fn)(void *),void *user){if(m){m->photo_saver=fn;m->photo_saver_user=user;}}
void menu_set_profile_picker(menu *m,int (*fn)(menu *)){if(m)m->profile_picker=fn;}
static int nat_pc_file(ah_vm *vm,ah_obj *self,void *data,int argc,const ah_val *argv,ah_val *ret){
 const char *op=data;ah_val slot=ah_nil();ah_get_member(vm,self,"_slot",&slot);
 int index=(int)ah_num(slot);gt4_save_info info;
 if(op && !strcmp(op,"load")){int ok=gt4_save_load_slot(index);if(ok)menu_save_refresh(ah_user(vm));*ret=ah_str(vm,ok?"OK":"FILE_NOT_FOUND");return 0;}
 if(op && !strcmp(op,"saveNew")){menu *m=ah_user(vm);int ok=gt4_save_capture(vm,game_object(m),1) && gt4_save_store();*ret=ah_str(vm,ok?"OK":"DEVICE_ERROR");return 0;}

 if(!op || !strcmp(op,"isExist")){*ret=ah_int(gt4_save_info_get(index,&info)>0?0:-1);return 0;}
 ah_obj *game=ah_module_new(vm,"profile_game",NULL,NULL,NULL);
 int ok=gt4_save_read_profile(vm,game,index);
 if(ok){gt4_game_bind(vm,game);menu *m=ah_user(vm);ah_val garage=ah_nil(),cars=ah_nil();
  ah_get_member(vm,game,"garage",&garage);ah_module_attach_host(garage.u.o,&garage_ui_host);
  const char *queries[]={"pickupCountry","pickupTuner","pickup"};
  for(int i=0;i<3;i++)ah_set_member(vm,garage.u.o,queries[i],ah_bind(vm,garage.u.o,ah_native(vm,queries[i],nat_garage_pickup,(void *)(intptr_t)i)));
  ah_set_member(vm,garage.u.o,"_getCarName",ah_native(vm,"_getCarName",nat_car_name,m));
  ah_set_member(vm,garage.u.o,"_bindCar",ah_native(vm,"_bindCar",nat_bind_game_car,m));
  if(ah_get_member(vm,garage.u.o,"cars",&cars))for(int i=0;i<ah_array_size(cars.u.o);i++){
   ah_val car=ah_array_get(cars.u.o,i),bound;nat_bind_game_car(vm,NULL,m,1,&car,&bound);
  }
 }
 ah_obj *result=ah_array_new(vm);ah_array_push(vm,result,ah_str(vm,ok?"OK":"FILE_NOT_FOUND"));
 ah_array_push(vm,result,ok?ah_objv(game):ah_nil());*ret=ah_objv(result);(void)argc;(void)argv;return 0;
}
static int nat_pc_manager(ah_vm*,ah_obj*,void*,int,const ah_val*,ah_val*);
static int nat_pc_check_profile(ah_vm *vm,ah_obj *self,void *data,int argc,const ah_val *argv,ah_val *ret){
 menu *m=data;ah_obj *manager=argc>2?ah_as_obj(argv[2],AO_MODULE):NULL;
 int slot=m->profile_picker?m->profile_picker(m):gt4_save_active();
 gt4_save_info info;if(slot<=0 || gt4_save_info_get(slot,&info)<=0){*ret=ah_nil();return 0;}
 if(!manager){manager=ah_module_new(vm,"native_profile_manager",NULL,NULL,NULL);ah_val mode=ah_str(vm,"MODE_LOAD_GAME");nat_pc_manager(vm,manager,m,1,&mode,ret);}
 ah_val file=ah_nil();ah_get_member(vm,manager,"_file",&file);ah_set_member(vm,file.u.o,"_slot",ah_int(slot));
 *ret=ah_objv(manager);(void)self;return 0;
}

/* MMemoryCardManager's statics labomode.ad calls: setAction (1 after a
   replay is loaded: the caller plays it), setExistValidGameData, and the
   photo print list (none here) */
/* Native PNG exports have no printer queue. labomode.ad L1968 still
   queries the original list.search(file) contract, rather than an array. */
static int nat_print_search(ah_vm *vm,ah_obj *self,void *d,int argc,const ah_val *argv,ah_val *ret)
{(void)vm;(void)self;(void)d;(void)argc;(void)argv;*ret=ah_int(-1);return 0;}
static int nat_mc_static(ah_vm *vm,ah_obj *self,void *data,int argc,const ah_val *argv,ah_val *ret)
{
    const char *op=data;(void)self;
    if(!strcmp(op,"setAction")){
        replay_action=argc?(int)ah_num(argv[0]):0;
        /* the Replay Theater's commandPlay: start_page(context, nil) then
           setAction(1); the replay plays over the theater and the theater
           comes back after it, so the page stays */
        if(replay_action==1 && replay_theater){menu *m=ah_user(vm);m->next_project[0]=0;}
        if(replay_action==1){   /* commandPlayList: the playlist, in order or shuffled */
            menu *m=ah_user(vm);ah_val result;
            if(ah_get_member(vm,game_object(m),"menu_result",&result) && ah_cstr(result) && !strcmp(ah_cstr(result),"list")){
                playlist_load();replay_queue_n=replay_queue_at=0;
                for(int k=0;k<playlist_count;k++)memcpy(replay_queue[replay_queue_n++],playlist_names[k],64);
                const char *mode=menu_option_text(m,"replay_play_mode","STRAIGHT");
                if(mode && !strcmp(mode,"SHUFFLE"))for(int k=replay_queue_n-1;k>0;k--){
                    int j=rand()%(k+1);char t[64];memcpy(t,replay_queue[k],64);memcpy(replay_queue[k],replay_queue[j],64);memcpy(replay_queue[j],t,64);}
            }
        }
        *ret=ah_nil();return 0;
    }
    if(!strcmp(op,"getPrintList")){
        ah_obj *list=ah_module_new(vm,"native_print_list",NULL,NULL,NULL);
        ah_set_member(vm,list,"size",ah_int(0));
        ah_set_member(vm,list,"search",ah_native(vm,"search",nat_print_search,NULL));
        *ret=ah_objv(list);return 0;
    }
    *ret=ah_nil();return 0;
}
static ah_obj *native_profile_file(menu *m,int slot){
 ah_vm *vm=m->vm;ah_obj *file=ah_module_new(vm,"native_profile_file",NULL,NULL,NULL);gt4_save_info info;
 ah_set_member(vm,file,"_slot",ah_int(slot));ah_set_member(vm,file,"objectName",ah_str(vm,"FileGame"));
 ah_set_member(vm,file,"title",ah_str(vm,gt4_save_info_get(slot,&info)>0?info.name:"New Player Profile"));
 uint8_t *bytes=NULL;size_t size=0;if(gt4_state_pack(vm,game_object(m),&bytes,&size))free(bytes);ah_set_member(vm,file,"size",ah_int((int64_t)size));
 const char *ops[]={"isExist","loadInstance","load","saveNew"};
 for(unsigned i=0;i<sizeof ops/sizeof *ops;i++)ah_set_member(vm,file,ops[i],ah_bind(vm,file,ah_native(vm,ops[i],nat_pc_file,(void *)ops[i])));
 return file;
}
static void native_profile_list(menu *m,ah_obj *manager){
 ah_obj *slots=ah_array_new(m->vm);gt4_save_info info;
 for(int i=1;i<=GT4_SAVE_SLOTS;i++)if(gt4_save_info_get(i,&info)>0)ah_array_push(m->vm,slots,ah_int(i));
 ah_set_member(m->vm,manager,"_profile_slots",ah_objv(slots));ah_set_member(m->vm,manager,"count",ah_int(ah_array_size(slots)));
}
#include "menu_legacy_import.inc"
static int nat_pc_manager_method(ah_vm *vm,ah_obj *self,void *data,int argc,const ah_val *argv,ah_val *ret){
 const char *name=data;menu *m=ah_user(vm);
 if(!strcmp(name,"getNewFile")){ah_get_member(vm,self,"_file",ret);return 0;}
 if(!strcmp(name,"getFile")){ah_val slots;ah_get_member(vm,self,"_profile_slots",&slots);ah_val slot=ah_array_get(slots.u.o,argc?to_int(argv[0]):-1);*ret=slot.t==AV_NIL?ah_nil():ah_objv(native_profile_file(m,to_int(slot)));return 0;}
 if(!strcmp(name,"start") || !strcmp(name,"restart") || !strcmp(name,"clear")){native_profile_list(m,self);*ret=ah_nil();return 0;}
 if(!strcmp(name,"isNoFile")){ah_val count;ah_get_member(vm,self,"count",&count);*ret=ah_bool(!ah_num(count));return 0;}
 if(!strcmp(name,"isUseNewFile")){ah_val mode;ah_get_member(vm,self,"_mode",&mode);*ret=ah_bool(ah_cstr(mode) && strstr(ah_cstr(mode),"SAVE"));return 0;}
 if(!strcmp(name,"format")){const char *dir=gt4_save_directory();*ret=ah_bool(dir && *dir && gt4_save_mkdir(dir));return 0;}
 *ret=ah_bool(strcmp(name,"isChanged") && strcmp(name,"isAutoSaveDisabled"));return 0;
}

/* The memory card's replay files (memcard.ad FileSelector in the modes
   MODE_LOAD_REPLAY, MODE_SAVE_FULL_REPLAY, ...), here the save folder's
   (replay_file.h, the PC layer): a card always there and formatted, its
   files the saved replays newest first, each with what the list shows
   (setItem: objectName, size in KB, date, time, title, carName). Loading
   one leaves its name for the caller; saving writes the race the caller
   lent (menu_set_replay_to_save). */
static char replay_names[256][64];
static int replay_count;
static char replay_loaded[64];
static const gt4_replay_file *replay_to_save;
void menu_set_replay_to_save(const gt4_replay_file *r){replay_to_save=r;}
int menu_in_replay_theater(const menu *m){return m && replay_theater;}
int menu_take_loaded_replay(char *name,size_t size)
{
    if(replay_action!=1)return 0;
    if(replay_queue_at<replay_queue_n){   /* the playlist's next */
        snprintf(name,size,"%s",replay_queue[replay_queue_at++]);
        if(replay_queue_at==replay_queue_n){replay_action=0;replay_queue_n=replay_queue_at=0;}
        return 1;
    }
    if(!replay_loaded[0])return 0;
    replay_action=0;snprintf(name,size,"%s",replay_loaded);replay_loaded[0]=0;return 1;
}
int menu_take_loaded_ghost(char *name,size_t size)
{
    if(replay_action!=1 || strncmp(replay_loaded,"bestlap_",8))return 0;
    replay_action=0;snprintf(name,size,"%s",replay_loaded);replay_loaded[0]=0;return 1;
}
static int replay_kind_matches(const char *name,const char *mode)
{
    int best=mode && strstr(mode,"BESTLAP")!=NULL;
    return best?!strncmp(name,"bestlap_",8):!strncmp(name,"replay_",7);
}
static int menu_read_replay(menu *m,const char *name,gt4_replay_file *r)
{
    if(strncmp(name,"/replay/",8) && gt4_replay_load(name,r))return 1;
    if(!m->disc_replays)m->disc_replays=gt4_disc_replay_open(m->vol);
    char error[128];return !strncmp(name,"/replay/",8)?gt4_disc_replay_read(m->disc_replays,name,r,error,sizeof error):gt4_disc_replay_saved(m->disc_replays,name,r,error,sizeof error);
}
static void replay_refresh(menu *m,const char *mode)
{
    if(mode && !strcmp(mode,"MODE_DEMONSTRATION")){
        replay_count=gt4_disc_replay_list(m->vol,replay_names,256,0);return;
    }
    char all[256][64];int n=gt4_replay_list(all,256);replay_count=0;
    if(mode && strstr(mode,"PLAYLIST")){   /* the Playlist folder: the list's replays in its order, those still there */
        playlist_load();
        int kept=0;
        for(int k=0;k<playlist_count;k++){
            int found=0;for(int i=0;i<n && !found;i++)found=!strcmp(all[i],playlist_names[k]);
            if(found){memcpy(playlist_names[kept],playlist_names[k],64);memcpy(replay_names[replay_count++],playlist_names[k],64);kept++;}
        }
        playlist_count=kept;
        return;
    }
    for(int i=0;i<n;i++)if(replay_kind_matches(all[i],mode))memcpy(replay_names[replay_count++],all[i],64);
}
static void replay_manager_files(ah_vm *vm,ah_obj *self,const char *mode)
{
    replay_refresh(ah_user(vm),mode);ah_obj *files=ah_array_new(vm);
    for(int i=0;i<replay_count;i++)ah_array_push(vm,files,ah_str(vm,replay_names[i]));
    ah_set_member(vm,self,"_files",ah_objv(files));ah_set_member(vm,self,"count",ah_int(replay_count));
}
void menu_stop_replay_queue(void){replay_queue_n=replay_queue_at=0;}
/* labomode.ad's update_widget: title, time, courseName (a course label
   it translates by CourseNameCaption), carName, size in KB, date (the
   PS2's dd/mm/yy), spectatorMode (B-spec), and the thumbnail
   (isIconValid / getIconTexture; disc icon and native saved picture). */
static int nat_rp_info(ah_vm *vm,ah_obj *self,void *data,int argc,const ah_val *argv,ah_val *ret)
{
    (void)argc;(void)argv;ah_val icon;
    int has_icon=ah_get_member(vm,self,"_icon",&icon) && ah_cstr(icon) && *ah_cstr(icon);
    if(!strcmp((const char *)data,"getIconTexture"))*ret=has_icon?icon:ah_str(vm,"");
    else *ret=ah_bool(!strcmp((const char *)data,"isInfoValid") || has_icon);
    return 0;
}
static void replay_file_fields(ah_vm *vm,ah_obj *o,const gt4_replay_file *f,size_t bytes,uint64_t when)
{
    static const char *const info[]={"isInfoValid","isIconValid","getIconTexture"};
    char date[32]="",clock[16]="",best[24]="";time_t t=(time_t)when;struct tm *local=when?localtime(&t):NULL;
    if(local){strftime(date,sizeof date,"%d/%m/%y",local);strftime(clock,sizeof clock,"%H:%M",local);}
    else snprintf(date,sizeof date,"--/--/--");
    if(f && f->best_lap && f->best_lap<0x157529ffu)
        snprintf(best,sizeof best,"%u'%02u.%03u",f->best_lap/60000,f->best_lap/1000%60,f->best_lap%1000);
    ah_set_member(vm,o,"objectName",ah_str(vm,f && f->kind==GT4_REPLAY_BEST_LAP?"FileReplayLicense":"FileReplayFull"));
    ah_set_member(vm,o,"size",ah_int((int64_t)((bytes+1023)/1024)));
    ah_set_member(vm,o,"date",ah_str(vm,date));
    ah_set_member(vm,o,"time",ah_str(vm,best));
    /* the race's mode (labomode.ad's SaveReplayRoot: racemode, racesubmode,
       its title the rtext RaceModeName of the mode, the submode's number
       after it when 0 or more) */
    {
        const char *mode=!f?"":f->time_trial?"ARCADE_TIME_TRIAL":f->disc_info?"ARCADE_SINGLE_RACE":f->prepared?(f->championship?"GTMODE_CHAMPIONSHIP":"GTMODE_EVENT_RACE"):"ARCADE_SINGLE_RACE";
        menu *m=ah_user(vm);
        const char *name=m && *mode?ui_translate(m->ui,0,"RaceModeName",mode):NULL;
        ah_set_member(vm,o,"racemode",ah_str(vm,mode));
        ah_set_member(vm,o,"racesubmode",ah_int(-1));
        ah_set_member(vm,o,"title",ah_str(vm,f && f->title[0]?f->title:name?name:""));   /* the player's name for it, else the mode's */
    }
    ah_set_member(vm,o,"courseName",ah_str(vm,f?f->course:""));
    {   /* the car's name; a file without it (older) by its label's CAR_NAME */
        const char *name=!f?"":f->car_name[0]?f->car_name:car_name(ah_user(vm),f->car);
        ah_set_member(vm,o,"carName",ah_str(vm,name?name:f->car));
    }
    ah_set_member(vm,o,"spectatorMode",ah_int(0));
    ah_set_member(vm,o,"portrait",ah_int(0));
    ah_set_member(vm,o,"dispstep",ah_int(0));
    (void)clock;
    for(unsigned k=0;k<3;k++)ah_set_member(vm,o,info[k],ah_bind(vm,o,ah_native(vm,info[k],nat_rp_info,(void *)info[k])));
}
static int nat_rp_file(ah_vm *vm,ah_obj *self,void *data,int argc,const ah_val *argv,ah_val *ret)
{
    const char *op=data;ah_val filename;
    const char *path=ah_get_member(vm,self,"_name",&filename)?ah_cstr(filename):NULL;
    (void)argc;(void)argv;
    if(!strcmp(op,"load")){
        gt4_replay_file f;
        if(!path || !*path){*ret=ah_str(vm,"DEVICE_ERROR");return 0;}
        if(!menu_read_replay(ah_user(vm),path,&f)){*ret=ah_str(vm,"INVALID_FILE");return 0;}
        gt4_replay_free(&f);snprintf(replay_loaded,sizeof replay_loaded,"%s",path);
        *ret=ah_str(vm,"OK");return 0;   /* labomode.ad / memcard.ad handleLoadSaveResult: "OK" */
    }
    if(!strcmp(op,"remove")){*ret=ah_bool(path && strncmp(path,"/replay/",8) && gt4_replay_remove(path));return 0;}
    if(!strcmp(op,"saveNew")){   /* a new file, or over the one chosen (Overwrite), with the name the player gave (its title) */
        char name[64],buf[64];ah_val title;int ok=0;
        if(replay_to_save){
            gt4_replay_file f=*replay_to_save;   /* the tape and picture lent */
            if(ah_get_member(vm,self,"title",&title) && title.t!=AV_NIL)snprintf(f.title,sizeof f.title,"%s",ah_to_text(title,buf,sizeof buf));
            ok=path && *path?(strncmp(path,"/replay/",8) && gt4_replay_store_over(&f,path)):gt4_replay_store(&f,name,sizeof name);
        }
        *ret=ah_str(vm,ok?"OK":"DEVICE_ERROR");return 0;
    }
    *ret=ah_bool(1);return 0;   /* isExist, setNewFile */
}
static ah_obj *replay_file_object(ah_vm *vm,int index,const char *filename,const gt4_replay_file *f,size_t bytes,uint64_t when)
{
    static const char *const ops[]={"load","remove","saveNew","setNewFile","isExist"};
    ah_obj *o=ah_module_new(vm,"native_replay_file",NULL,NULL,NULL);
    replay_file_fields(vm,o,f,bytes,when);
    if(f && f->disc_icon){
        menu *m=ah_user(vm);char path[96];snprintf(path,sizeof path,"disc/icon/%s",filename?filename:"new");
        ui_image_put(m->ui,path,64,64,f->disc_icon);ah_set_member(vm,o,"_icon",ah_str(vm,path));
    }
    else if(f && f->thumb){   /* its picture, an image the list's Icon names (image_chunk) */
        menu *m=ah_user(vm);char path[64];uint8_t *rgba=malloc(GT4_REPLAY_THUMB_W*GT4_REPLAY_THUMB_H*4);
        snprintf(path,sizeof path,"pc/replay/%s",filename?filename:"new");
        if(rgba){
            for(int k=0;k<GT4_REPLAY_THUMB_W*GT4_REPLAY_THUMB_H;k++){memcpy(rgba+k*4,f->thumb+k*3,3);rgba[k*4+3]=255;}
            ui_image_put(m->ui,path,GT4_REPLAY_THUMB_W,GT4_REPLAY_THUMB_H,rgba);free(rgba);
            ah_set_member(vm,o,"_icon",ah_str(vm,path));
        }
    }
    ah_set_member(vm,o,"_index",ah_int(index));
    if(filename)ah_set_member(vm,o,"_name",ah_str(vm,filename));
    for(unsigned k=0;k<sizeof ops/sizeof ops[0];k++)ah_set_member(vm,o,ops[k],ah_bind(vm,o,ah_native(vm,ops[k],nat_rp_file,(void *)ops[k])));
    return o;
}
static int nat_rp_manager_method(ah_vm *vm,ah_obj *self,void *data,int argc,const ah_val *argv,ah_val *ret)
{
    const char *name=data;ah_val mode;ah_get_member(vm,self,"_mode",&mode);
    if(!strcmp(name,"getFile")){
        int i=argc?(int)ah_num(argv[0]):-1;gt4_replay_file f;ah_val files;
        ah_get_member(vm,self,"_files",&files);const char *path=ah_cstr(ah_array_get(ah_as_obj(files,AO_ARRAY),i));
        if(!path){*ret=ah_nil();return 0;}
        if(menu_read_replay(ah_user(vm),path,&f)){
            uint8_t *packed;size_t bytes=0;if(gt4_replay_pack(&f,&packed,&bytes))free(packed);
            if(f.disc_info)bytes=*(uint32_t *)f.disc_info;
            ah_obj *file=replay_file_object(vm,i,path,&f,bytes+32,f.when);
            *ret=ah_objv(file);gt4_replay_free(&f);
        }else *ret=ah_objv(replay_file_object(vm,i,path,NULL,0,0));
        return 0;
    }
    if(!strcmp(name,"getNewFile")){
        uint8_t *packed;size_t bytes=0;
        if(replay_to_save && gt4_replay_pack(replay_to_save,&packed,&bytes))free(packed);
        *ret=ah_objv(replay_file_object(vm,-1,NULL,replay_to_save,bytes+32,(uint64_t)time(NULL)));return 0;
    }
    if(!strcmp(name,"start") || !strcmp(name,"restart") || !strcmp(name,"clear")){
        replay_manager_files(vm,self,ah_cstr(mode));
        *ret=ah_nil();return 0;
    }
    if(!strcmp(name,"isNoFile")){ah_val count;ah_get_member(vm,self,"count",&count);*ret=ah_bool(ah_num(count)==0);return 0;}
    if(!strcmp(name,"isUseNewFile")){*ret=ah_bool(ah_cstr(mode) && strstr(ah_cstr(mode),"SAVE")!=NULL);return 0;}
    if(!strcmp(name,"isChanged") || !strcmp(name,"isAutoSaveDisabled")){*ret=ah_bool(0);return 0;}
    *ret=ah_bool(1);return 0;   /* isConnect, isFormat, waitPause, requestPause, clearAutoSaveDisabled */
}
static int nat_playlist_save(ah_vm *vm,ah_obj *self,void *data,int argc,const ah_val *argv,ah_val *ret)
{
    (void)self;(void)data;(void)argc;(void)argv;playlist_load();
    *ret=ah_str(vm,gt4_replay_playlist_write((const char (*)[64])playlist_names,playlist_count)?"OK":"DEVICE_ERROR");return 0;
}
static void replay_manager(ah_vm *vm,ah_obj *self,const char *mode)
{
    static const char *const methods[]={"getFile","getNewFile","start","restart","clear","isNoFile","isUseNewFile",
        "isChanged","isAutoSaveDisabled","isConnect","isFormat","waitPause","requestPause","clearAutoSaveDisabled"};
    ah_obj *progress=ah_module_new(vm,"progress",NULL,NULL,NULL);
    ah_set_member(vm,progress,"value",ah_float(0));
    ah_set_member(vm,self,"progress",ah_objv(progress));
    ah_set_member(vm,self,"_mode",ah_str(vm,mode));
    replay_manager_files(vm,self,mode);
    {   /* the free space in KB: the PC layer's (docs/EDITIONS.md), the disk's
           that holds the saves, shown as "Hard Disk" (ui_translate); 8000 is
           labomode's "unformatted", never given */
        uint64_t bytes=0;int64_t kb;
        const char *dir=gt4_save_directory();
        kb=plat_disk_free(dir && *dir?dir:".",&bytes)?(int64_t)(bytes/1024):0;
        if(kb==8000)kb=7999;
        ah_set_member(vm,self,"free",ah_int(kb));ah_set_member(vm,self,"realFree",ah_int(kb));
    }
    ah_set_member(vm,self,"list",ah_nil());   /* the playlist labomode assigns */
    {   /* the playlist file, read with the card (autoloadPlayListResult) and written by save() */
        ah_obj *autoload=ah_module_new(vm,"autoloadPlayList",NULL,NULL,NULL);ah_set_member(vm,autoload,"list",ah_nil());
        ah_set_member(vm,autoload,"save",ah_bind(vm,autoload,ah_native(vm,"save",nat_playlist_save,NULL)));
        ah_set_member(vm,self,"autoloadPlayList",ah_objv(autoload));
        ah_set_member(vm,self,"autoloadPlayListResult",ah_str(vm,"OK"));
    }
    for(unsigned i=0;i<sizeof methods/sizeof methods[0];i++)
        ah_set_member(vm,self,methods[i],ah_bind(vm,self,ah_native(vm,methods[i],nat_rp_manager_method,(void *)methods[i])));
}
#include "menu_album.inc"
static int nat_pc_manager(ah_vm *vm,ah_obj *self,void *data,int argc,const ah_val *argv,ah_val *ret){
    menu *m=data;
    {const char *mode=argc?ah_cstr(argv[0]):NULL;
     ah_set_member(vm,self,"_mode",ah_str(vm,mode?mode:"MODE_OPERATION"));
     ah_set_member(vm,self,"_directory",ah_str(vm,gt4_save_directory()));
     ah_set_member(vm,self,"_storage_kind",ah_str(vm,mode && strstr(mode,"PHOTO")?"photos":mode && (strstr(mode,"REPLAY") || strstr(mode,"DEMONSTRATION"))?"replays":"profiles"));
     if(mode && strstr(mode,"PHOTO")){album_manager(vm,self,mode);*ret=ah_nil();return 0;}
     if(mode && (strstr(mode,"REPLAY") || strstr(mode,"DEMONSTRATION"))){replay_manager(vm,self,mode);*ret=ah_nil();return 0;}}
    ah_set_member(vm,self,"_native_save_manager",ah_int(1));
    ah_val mode;ah_get_member(vm,self,"_mode",&mode);replay_manager(vm,self,ah_cstr(mode)); /* Native directory/free-space/status contract. */
    ah_set_member(vm,self,"_file",ah_objv(native_profile_file(m,gt4_save_active())));native_profile_list(m,self);
    const char *methods[]={"getNewFile","getFile","start","restart","clear","isNoFile","isUseNewFile","isConnect","isFormat","format","isChanged","isAutoSaveDisabled","waitPause","requestPause","clearAutoSaveDisabled"};
    for(unsigned i=0;i<sizeof methods/sizeof *methods;i++)ah_set_member(vm,self,methods[i],ah_bind(vm,self,ah_native(vm,methods[i],nat_pc_manager_method,(void *)methods[i])));
    const char *imports[]={"isExistGT3Data","inheritGT3Data","isExistGT4pData","inheritGT4pData"};
    for(unsigned i=0;i<4;i++)ah_set_member(vm,self,imports[i],ah_bind(vm,self,ah_native(vm,imports[i],nat_legacy_import,(void *)imports[i])));
    *ret=ah_nil();return 0;
}
static int nat_pc_change_car(ah_vm *vm,ah_obj *self,void *data,int argc,const ah_val *argv,ah_val *ret){
    (void)data;ah_val method;if(!ah_get_member(vm,self,"_changeRidingCarNative",&method))return ah_error(vm,"native selection unavailable");
    ah_val marker=ah_nil();if(argc==2)ah_get_member(vm,ah_as_obj(argv[1],AO_MODULE),"_native_save_manager",&marker);
    if(!ah_truthy(marker))return ah_call(vm,method,argc,argv,ret);
    /* The PC entry owns its body; no PS2 garage file needs to be loaded. */
    if(ah_call(vm,method,1,argv,ret)<0)return -1;
    *ret=ah_str(vm,"OK");return 0;
}
static int nat_pc_autosave(ah_vm *vm,ah_obj *self,void *data,int argc,const ah_val *argv,ah_val *ret){
    (void)self;menu *m=data;int ok=gt4_save_capture(vm,game_object(m),1);
    if(ok && gt4_save_active() && gt4_save_auto_enabled())ok=gt4_save_store();
    if(argc>1){ah_val progress;if(ah_get_member(vm,ah_as_obj(argv[1],AO_MODULE),"progress",&progress))ah_set_member(vm,ah_as_obj(progress,AO_MODULE),"value",ah_float(ok?1:0));}
    if(!ok)return ah_error(vm,"native Auto Save: %s",gt4_save_error());
    if(argc>3)ah_reference_set(vm,argv[3],ah_int(1));
    *ret=ah_bool(1);return 0;
}
static int nat_open_save(ah_vm *vm,ah_obj *self,void *data,int argc,const ah_val *argv,ah_val *ret)
{(void)self;(void)argc;(void)argv;menu *m=data;
 /* P: Save writes the current PC profile; ManageSaves opens the browser. */
 if(!gt4_save_active()){menu_request_project(m,"save");*ret=ah_int(2);return 0;}
 int ok=gt4_save_capture(vm,game_object(m),1) && gt4_save_store();
 menu_play_effect(m,ok?"select":"disable");
 const char *message=ok?"Game saved.":gt4_save_error();
 if(m->present_dialog)m->present_dialog(m,"Save Game",message,0,"OK",NULL);
 else plat_dialog("Save Game",message,0);
 *ret=ah_int(2);return 0;}
static int nat_enter_gtmode(ah_vm *vm,ah_obj *self,void *data,int argc,const ah_val *argv,ah_val *ret)
{
    (void)vm;(void)self;(void)argc;(void)argv;menu *m=data;
    m->gtmode_save_entry=!gt4_save_active();
    /* arcade.adc: GTmode.onActivate L488 uses ok before profile creation;
       startGTmode L585 uses return with existing game data. */
    menu_play_effect(m,m->gtmode_save_entry?"ok":"return");
    menu_request_project(m,m->gtmode_save_entry?"save":"gtmode");*ret=ah_int(2);return 0;
}
int menu_take_gtmode_save_entry(menu *m){int result=m?m->gtmode_save_entry:0;if(m)m->gtmode_save_entry=0;return result;}
static int nat_pc_menu_action(ah_vm *vm,ah_obj *self,void *data,int argc,const ah_val *argv,ah_val *ret)
{
    (void)self;(void)argc;(void)argv;menu *m=ah_user(vm);const char *action=data;
    if(!strcmp(action,"QuitGame")) {
        if(menu_confirm(m,"Exit OpenGT4","Exit the game?")) {
            menu_save_capture(m);
            if(gt4_save_active() && gt4_save_dirty() && gt4_save_auto_enabled() && !gt4_save_store())
                menu_confirm(m,"Save Game",gt4_save_error());
            else m->quit_requested=1;
        }
    } else {
        menu_save_capture(m);
        if(gt4_save_active() && gt4_save_dirty() && gt4_save_auto_enabled() && !gt4_save_store()) {
            menu_confirm(m,"Save Game",gt4_save_error());*ret=ah_int(2);return 0;
        }
        m->new_career_entry=!strcmp(action,"NewCareer");
        menu_play_effect(m,"ok");menu_request_project(m,"save");
    }
    *ret=ah_int(2);return 0;
}
int menu_take_new_career(menu *m){int result=m?m->new_career_entry:0;if(m)m->new_career_entry=0;return result;}
int menu_take_labo_arg(menu *m,char *arg,size_t size)
{
    if(!m || !m->labo_arg[0])return 0;
    snprintf(arg,size,"%s",m->labo_arg);m->labo_arg[0]=0;return 1;
}
void menu_memcard_return(const char *project){snprintf(memcard_origin,sizeof memcard_origin,"%s",project?project:"");}
/* labomode.ad onLoad: startPage by main.game.next_menu_arg, a card mode
   (MODE_SAVE_FULL_REPLAY: SaveReplayRoot) or a page's name */
int menu_open_labo(menu *m,const char *mode)
{
    ah_val ctx,result;
    if(!m || !mode)return 0;
    /* PhotoRoot stands for the empty argument (the photo_labo sequence) */
    ah_set_member(m->vm,game_object(m),"next_menu_arg",ah_str(m->vm,strcmp(mode,"PhotoRoot")?mode:""));
    replay_theater=!strcmp(mode,"ReplayTheaterRoot");
    while(m->depth)finalize_page(m,m->pages[--m->depth]);
    ctx=ah_objv(m->context);
    return ah_call_member(m->vm,m->project,"onLoad",1,&ctx,&result)>=0;
}
const char *menu_car_name(menu *m,const char *label){return m && label?car_name(m,label):NULL;}
int menu_take_quit(menu *m){int result=m?m->quit_requested:0;if(m)m->quit_requested=0;return result;}
static void pc_label(ui_widget *w,const char *label)
{if(w){free(w->text_key);w->text_key=NULL;free(w->text);w->text=strdup(label);}}
static void pc_menu_entries(menu *m,ui_widget *root)
{
    if(!strcmp(root->name,"MakerRoot")){
        /* P: one local-profile chooser replaces two fixed card-slot buttons. */
        ui_widget *tab=ui_widget_child(root,"Tab2p"),*a=ui_widget_child(tab,"Slot1"),*b=ui_widget_child(tab,"Slot2");
        if(a && b){b->visible=0;a->w=224;
            ui_widget *bg=ui_widget_child(a,"bg"),*label=ui_widget_child(a,"label");
            if(bg)bg->w=224;if(label){label->w=224;pc_label(label,"Player Profiles");}
        }
    }

    if(!strcmp(ah_module_name(m->project),"ArcadeProject") && !strcmp(root->name,"TopRoot")) {
        ui_widget *list=ui_widget_child(ui_widget_child(root,"StartScroll"),"SelectBox");
        ui_widget *sample=ui_widget_child(ui_widget_child(root,"hidden"),"GTmode");
        if(!list || !sample || ui_widget_child(list,"NewCareer"))return;
        const char *names[]={"NewCareer","QuitGame"},*labels[]={"New Career","Exit Game"};
        for(int i=0;i<2;i++) {
            ui_widget *item=ui_widget_copy(m->ui,sample);item->name=names[i];
            copy_script(m,sample,item);pc_label(ui_widget_child(item,"label"),labels[i]);
            ah_val arg=maybe_module(m,item),result;
            ah_call_member(m->vm,widget_module(m,root),"setButtonActor",1,&arg,&result);
            ui_widget_append(list,item);
            menu_native_method(m,item,"onActivate",nat_pc_menu_action,(void *)names[i]);
        }
        /* SelectBox uses child order for both layout and navigation. */
        ui_widget *after=ui_widget_child(list,"GTmode");
        const char *ordered[]={"Save","NewCareer"};
        for(int i=0;after && i<2;i++) {
            ui_widget *item=ui_widget_child(list,ordered[i]);
            if(!item)continue;
            ui_widget_detach(item);ui_widget_append(list,item);
            int index=0;
            while(index<list->nkids && list->kids[index]!=after)index++;
            if(index<list->nkids-1) {
                memmove(&list->kids[index+2],&list->kids[index+1],
                        (list->nkids-index-2)*sizeof *list->kids);
                list->kids[index+1]=item;
            }
            after=item;
        }
    } else if(!strcmp(ah_module_name(m->project),"OptionProject") && !strcmp(root->name,"OptionRoot")) {
        ui_widget *scroll=ui_widget_child(root,"ScrollWindow");
        ui_widget *space=ui_widget_child(ui_widget_child(scroll,"clip"),"space");
        if(!space || ui_widget_child(space,"ManageSaves"))return;
        ui_widget *sample=NULL,*photo=NULL;float bottom=0;
        for(int i=0;i<space->nkids;i++) {
            ui_widget *item=space->kids[i];bottom=fmaxf(bottom,item->y+item->h);
            ui_widget *label=ui_widget_child(item,"label");
            if(!sample && label && item->w<120)sample=item;
            if(label && label->text && !strcmp(label->text,"Photo Lab"))photo=item;
        }
        if(!sample)return;
        ui_widget *group_sample=ui_widget_child(ui_widget_child(root,"hidden"),"Group");
        float section_y=photo?photo->y+photo->h+4:bottom+4;
        float group_h=group_sample?group_sample->h:24;
        float added=group_h+sample->h+8;
        for(int i=0;i<space->nkids;i++)if(space->kids[i]->y>=section_y)space->kids[i]->y+=added;
        space->h+=added;
        if(group_sample) {
            ui_widget *group=ui_widget_copy(m->ui,group_sample);group->name="PlayerProfiles";
            group->x=0;group->y=section_y;pc_label(ui_widget_child(group,"label"),"Player Profiles");
            ui_widget_append(space,group);
        }
        ui_widget *card=ui_widget_copy(m->ui,sample);card->name="ManageSaves";
        copy_script(m,sample,card);card->x=0;card->y=section_y+group_h;
        pc_label(ui_widget_child(card,"label"),"Save Games");
        ui_load_package(m->ui,"/projects/gtmode","HomeRoot");
        ui_widget_set_image(ui_widget_child(card,"bg"),"image/gtmode/home/garage.png");
        ui_widget_append(space,card);
        menu_native_method(m,card,"onActivate",nat_pc_menu_action,"ManageSaves");
    }
}

static void bind_save_buttons(menu *m,ui_widget *w){if(!w)return;if(w->name && !strcmp(w->name,"GTmode") && !strcmp(w->type,"Composite"))menu_native_method(m,w,"onActivate",nat_enter_gtmode,m);if(w->name && !strcmp(w->name,"Save") && (!strcmp(w->type,"Button") || mp_number(w->src,"can_focus",0)))menu_native_method(m,w,"onActivate",nat_open_save,m);for(int i=0;i<w->nkids;i++)bind_save_buttons(m,w->kids[i]);}
void menu_request_project(menu *m,const char *name){if(m && name && strlen(name)<sizeof m->next_project)snprintf(m->next_project,sizeof m->next_project,"%s",name);}
int menu_confirm(menu *m,const char *title,const char *message){int r=m && m->present_dialog?m->present_dialog(m,title,message,1,"OK","Cancel"):plat_dialog(title,message,1);return r==1;}
/* func_00447C50 resolves namespace 0x23; func_004483B8 uses the
   low 16 identifier bits and func_00449190 maps to the record ordinal. */
static void load_race_tables(menu *m){
    if(m->race_load_attempted)return;
    m->race_load_attempted=1;
    m->race_labels=gt4_vol_read(m->vol,"/specdb/GT4_US2560/RACE.idi",&m->race_labels_size);
    m->race_db=gt4_vol_read(m->vol,"/specdb/GT4_US2560/RACE.dbt",&m->race_db_size);
}
static int nat_race_record_index(ah_vm *vm,ah_obj *self,void *user,int argc,const ah_val *argv,ah_val *ret){
    (void)self;menu *m=user;const char *label=argc==1?ah_cstr(argv[0]):NULL;
    if(!label)return ah_error(vm,"race record requires a label");
    load_race_tables(m);
    *ret=ah_int(gt4_career_race_record_index(gt4_game_race_lookup_data(),m->race_labels,m->race_labels_size,m->race_db,m->race_db_size,label));return 0;
}
/* func_00447C50: calendar stores the exact RACE key; no record fallback. */
static int nat_calendar_race_key(ah_vm *vm,ah_obj *self,void *user,int argc,const ah_val *argv,ah_val *ret){
    (void)self;menu *m=user;const char *label=argc==1?ah_cstr(argv[0]):NULL;
    if(!label)return ah_error(vm,"race calendar requires a label");load_race_tables(m);uint32_t id;
    *ret=ah_int(gt4_specdb_label(m->race_labels,m->race_labels_size,label,&id)?(int64_t)(UINT64_C(0x2300000000)|id):-1);return 0;
}
/* MRaceData's boxed object has a 0x10-byte prefix. _raceRow contains only
   the 151 decoded SpecDB bytes, beginning at original object+0x10.
   These adapters expose data; they do not enforce entry requirements. */
static int nat_race_field(ah_vm *vm,ah_obj *self,void *user,int argc,const ah_val *argv,ah_val *ret){
    const char *method=user;ah_val stored;uint8_t row[151];
    if(!self || !ah_get_member(vm,self,"_raceRow",&stored) || ah_array_size(ah_as_obj(stored,AO_ARRAY))!=151)
        return ah_error(vm,"MRaceData has no decoded RACE row");
    for(unsigned i=0;i<151;i++)row[i]=(uint8_t)ah_array_get(stored.u.o,i).u.i;
    if(!strcmp(method,"getAdditionalDriverPoints")){
        if(argc<1 || (argv[0].t!=AV_INT && argv[0].t!=AV_LONG) || (uint64_t)argv[0].u.i>=6 || !gt4_game_race_lookup_data())
            return ah_error(vm,"getAdditionalDriverPoints requires original data and position 0..5");
        *ret=ah_int(gt4_career_driver_points(gt4_game_race_lookup_data(),(unsigned)argv[0].u.i));return 0;
    }
    if(!strcmp(method,"getPrize")){
        if(argc<1)return ah_error(vm,"getPrize requires a position");
        *ret=ah_int(gt4_career_race_prize(row,(uint64_t)argv[0].u.i));return 0;
    }
    if(argc)return ah_error(vm,"%s takes no arguments",method);
    if(!strcmp(method,"getNeedCountry")){
        const char *names[8];unsigned n=gt4_career_countries(row[0x96],names);
        ah_obj *countries=ah_array_new(vm);
        for(unsigned i=0;i<n;i++)ah_array_push(vm,countries,ah_str(vm,names[i]));
        *ret=ah_objv(countries);return 0;
    }
    if(!strcmp(method,"getAllowedCarList")){
        char labels[60][128];unsigned count;menu *m=ah_user(vm);
        *ret=ah_nil();if(!gt4_career_allowed_cars(m->vol,row,labels,&count))return 0;
        ah_obj *cars=ah_array_new(vm);
        for(unsigned i=0;i<count;i++)ah_array_push(vm,cars,ah_str(vm,labels[i]));
        *ret=ah_objv(cars);return 0;
    }
    if(!strcmp(method,"getNeedYearRange")){
        ah_obj *years=ah_array_new(vm);
        ah_array_push(vm,years,ah_int(gt4_career_race_field(row,GT4_RACE_YEAR_MIN)));
        ah_array_push(vm,years,ah_int(gt4_career_race_field(row,GT4_RACE_YEAR_MAX)));
        *ret=ah_objv(years);return 0;
    }
    static const struct {const char *name;gt4_career_race_field_id field;} fields[]={
        {"getNeedLicense",GT4_RACE_LICENSE},{"getNeedDrivetrain",GT4_RACE_DRIVETRAIN},
        {"getNeedAspiration",GT4_RACE_ASPIRATION},{"getNeedCarType",GT4_RACE_CAR_TYPE},
        {"getNeedTire",GT4_RACE_TIRE},{"getNeedPs",GT4_RACE_PS},{"getNeedPrice",GT4_RACE_PRICE},
        {"getLimitedWeight",GT4_RACE_WEIGHT},{"getLimitedLength",GT4_RACE_LENGTH},
        {"getNeedCountryID",GT4_RACE_COUNTRY},{"getNumberOfLaps",GT4_RACE_LAPS},
        {"getLimitTime",GT4_RACE_LIMIT_TIME},{"getPrizeGC",GT4_RACE_PRIZE_GC}
    };
    for(unsigned i=0;i<sizeof fields/sizeof fields[0];i++)if(!strcmp(method,fields[i].name)){
        *ret=ah_int(gt4_career_race_field(row,fields[i].field));return 0;
    }
    return ah_error(vm,"unsupported MRaceData field %s",method);
}
static int nat_race_available(ah_vm *vm,ah_obj *self,void *user,int argc,const ah_val *argv,ah_val *ret){
    (void)vm;(void)self;menu *m=user;uint32_t id;load_race_tables(m);
    const char *label=argc?ah_cstr(argv[0]):NULL;
    /* func_001A2A90 -> func_00447C50: label existence, without decoding. */
    *ret=ah_bool(label && gt4_specdb_label(m->race_labels,m->race_labels_size,label,&id));return 0;
}
static int nat_race_init(ah_vm *vm,ah_obj *self,void *user,int argc,const ah_val *argv,ah_val *ret){
    menu *m=user;uint8_t row[151];load_race_tables(m);
    const char *label=argc?ah_cstr(argv[0]):NULL;
    /* func_001A0E48 -> func_00447188 -> func_004470E8 -> func_00443ED0.
       The no-argument constructor remains unported. */
    if(!gt4_career_race_row(gt4_game_race_lookup_data(),m->race_labels,m->race_labels_size,m->race_db,m->race_db_size,label,row))
        return ah_error(vm,"MRaceData: original RACE row unavailable for %s",label?label:"<no label>");
    ah_obj *bytes=ah_array_new(vm);for(unsigned i=0;i<sizeof row;i++)ah_array_push(vm,bytes,ah_int(row[i]));
    ah_set_member(vm,self,"_raceRow",ah_objv(bytes));
    /* func_001A1408 -> 004472A0. 004470E8 copies decoded RACE+0
       into the cached course key at +0xA0 (00447238). Resolve its low
       identifier in the disc COURSE label table, as 00443E60 does. */
    if(!m->course_load_attempted){
        m->course_load_attempted=1;
        m->course_db=gt4_vol_read(m->vol,"/specdb/GT4_US2560/COURSE.dbt",&m->course_db_size);
        m->course_labels=gt4_vol_read(m->vol,"/specdb/GT4_US2560/COURSE.idi",&m->course_labels_size);
    }
    uint32_t course_id=(uint32_t)row[0]|(uint32_t)row[1]<<8;
    const char *course=gt4_specdb_label_for_id(m->course_labels,m->course_labels_size,course_id);
    ah_set_member(vm,self,"course_code",course?ah_str(vm,course):ah_nil());
    /* func_001A1370: original registered read-only scalar attribute. */
    ah_set_member(vm,self,"entry_num",ah_int(gt4_career_race_field(row,GT4_RACE_ENTRY_NUM)));
    static const char *const names[]={"getPrize","getAdditionalDriverPoints","getPrizeGC","getNeedYearRange","getNeedLicense",
        "getNeedDrivetrain","getNeedAspiration","getNeedCarType","getNeedTire","getNeedPs",
        "getNeedPrice","getLimitedWeight","getLimitedLength","getNeedCountryID","getNumberOfLaps","getLimitTime",
        "getNeedCountry","getAllowedCarList"};
    for(unsigned i=0;i<sizeof names/sizeof names[0];i++)
        ah_set_member(vm,self,names[i],ah_bind(vm,self,ah_native(vm,names[i],nat_race_field,(void *)names[i])));
    *ret=ah_nil();return 0;
}
/* 001652F0: active at championship live +28, serialized header omitted. */
static int nat_game_suspended(ah_vm *vm,ah_obj *self,void *data,int argc,const ah_val *argv,ah_val *ret){
 (void)self;(void)argc;(void)argv;menu *m=data;ah_val champ=ah_nil(),stored=ah_nil();
 ah_get_member(vm,game_object(m),"championship",&champ);
 ah_get_member(vm,ah_as_obj(champ,AO_MODULE),"context_ps2",&stored);
 uint8_t context[0x88]={0};
 if(stored.t!=AV_NIL && !gt4_career_blob_get(vm,champ.u.o,"context_ps2",context,sizeof context))return ah_error(vm,"invalid championship context");
 *ret=ah_bool(context[0x20]!=0);return 0;
}
void menu_save_refresh(menu *m)
{
    if(!m)return;
    ah_obj *game=game_object(m);
    gt4_save_restore(m->vm,game);
    /* gtmode.ad getRaceEventRecordStatistics L701..706 caches both values
       for the original project lifetime. Native cached VMs survive a race
       project switch: restoring newer records must invalidate this snapshot
       before HomeRoot L3982 reads it again. Licences are not in L712..767's
       event/mission denominator; do not invent licence completion points. */
    if(!strcmp(ah_module_name(m->project),"GTmodeProject")){
        ah_set_member(m->vm,m->project,"cached_achievement",ah_nil());
        ah_set_member(m->vm,m->project,"cached_aspec_score",ah_nil());
    }
    if(m->album_music_ready)album_music_bind(m);
    {ah_val option;if(ah_get_member(m->vm,game,"option",&option))gt4_pad_bind_option(m->vm,ah_as_obj(option,AO_MODULE));}
    const char *project=ah_module_name(m->project);
    if(!strcmp(project,"RaceEventProject") || !strcmp(project,"ChampionshipProject") || !strcmp(project,"MissionProject")){
        /* share/prize_action.ad L52 uses the same main.game.incMoney
           as GT Mode. Native projects have separate VMs, so restore
           its gameplay bindings as well as the saved profile fields. */
        gt4_game_bind(m->vm,game);
        ah_set_member(m->vm,game,"_markDirty",ah_native(m->vm,"_markDirty",nat_need_save,m));
    }
    if(!strcmp(ah_module_name(m->project),"GTmodeProject") || !strcmp(ah_module_name(m->project),"LicenseProject") || !strcmp(ah_module_name(m->project),"SettingProject")){
        gt4_game_bind(m->vm,game);
        ah_set_member(m->vm,game,"isSuspended",ah_native(m->vm,"isSuspended",nat_game_suspended,m));
        ah_val race_record;if(ah_get_member(m->vm,game,"race_record",&race_record))
            ah_set_member(m->vm,ah_as_obj(race_record,AO_MODULE),"_getRecordIndex",ah_native(m->vm,"_getRecordIndex",nat_race_record_index,m));
        ah_val calendar;ah_get_member(m->vm,game,"calendar",&calendar);
        ah_set_member(m->vm,ah_as_obj(calendar,AO_MODULE),"_getCarKey",ah_native(m->vm,"_getCarKey",nat_calendar_car_key,m));
        ah_set_member(m->vm,ah_as_obj(calendar,AO_MODULE),"_getRaceKey",ah_native(m->vm,"_getRaceKey",nat_calendar_race_key,m));
        ah_set_member(m->vm,calendar.u.o,"_getWheelKey",ah_native(m->vm,"_getWheelKey",nat_calendar_custom_key,(void *)(intptr_t)0));
        ah_set_member(m->vm,calendar.u.o,"_getWingKey",ah_native(m->vm,"_getWingKey",nat_calendar_custom_key,(void *)(intptr_t)1));
        ah_val garage;ah_get_member(m->vm,game,"garage",&garage);
        ah_module_attach_host(garage.u.o,&garage_ui_host);ah_remove_member(m->vm,garage.u.o,"riding_car");
        const char *queries[]={"pickupCountry","pickupTuner","pickup"};
        for(int i=0;i<3;i++)ah_set_member(m->vm,garage.u.o,queries[i],ah_bind(m->vm,garage.u.o,ah_native(m->vm,queries[i],nat_garage_pickup,(void *)(intptr_t)i)));
        ah_val change;ah_get_member(m->vm,garage.u.o,"changeRidingCar",&change);
        ah_set_member(m->vm,garage.u.o,"_changeRidingCarNative",change);
        ah_set_member(m->vm,garage.u.o,"changeRidingCar",ah_bind(m->vm,garage.u.o,ah_native(m->vm,"changeRidingCar",nat_pc_change_car,m)));
        ah_set_member(m->vm,ah_as_obj(garage,AO_MODULE),"_getCarName",ah_native(m->vm,"_getCarName",nat_car_name,m));
        ah_set_member(m->vm,ah_as_obj(garage,AO_MODULE),"_bindCar",ah_native(m->vm,"_bindCar",nat_bind_game_car,m));
        ah_val car_array;if(ah_get_member(m->vm,ah_as_obj(garage,AO_MODULE),"cars",&car_array)){
            for(int i=0;i<ah_array_size(ah_as_obj(car_array,AO_ARRAY));i++){
                ah_val car=ah_array_get(car_array.u.o,i),result;if(car.t!=AV_NIL)nat_bind_game_car(m->vm,NULL,m,1,&car,&result);
            }
        }
        ah_val dirty=ah_native(m->vm,"_markDirty",nat_need_save,m);
        ah_set_member(m->vm,game,"_markDirty",dirty);
        const char *owners[]={"garage","calendar","presents","race_record"};
        for(int i=0;i<4;i++){
            ah_val owner;ah_get_member(m->vm,game,owners[i],&owner);
            ah_set_member(m->vm,ah_as_obj(owner,AO_MODULE),"_markDirty",dirty);
        }
    }
}
void menu_save_capture(menu *m)
{
    if(!m)return;
    album_music_store(m);
    /* Cached menus must not overwrite either playlist with an older copy. */
    uint8_t playlist[0x810];ah_val option;
    if(ah_get_member(m->vm,game_object(m),"option",&option)){
        const char *names[]={"bgm_playlist_ps2","slide_playlist_ps2"};
        for(int i=0;i<2;i++)if(gt4_save_option_blob_get(names[i],playlist,sizeof playlist))
            gt4_career_blob_set(m->vm,ah_as_obj(option,AO_MODULE),names[i],playlist,sizeof playlist);
    }
    /* options the scripts marked (setNeedSave) but no page change applied
       yet, as when the game is closed on the options page: kept too */
    int dirty=m->options_dirty;
    if(dirty){options_capture(m);m->options_dirty=0;}
    else {
        /* Only setNeedSave/apply commits preferences. An untouched cached
           menu still owns an old option copy; do not publish it on exit. */
        if(!gt4_save_restore_options(m->vm,game_object(m)))options_restore(m);
    }
    gt4_save_capture(m->vm,game_object(m),dirty);
}

/* Original selection stack: 00169214..00169410 registers the methods
   00165EE0/00165FC0/00166100/00166278; the byte rules have an EE oracle. */
static int nat_menu_selection(ah_vm *vm,ah_obj *self,void *data,int argc,const ah_val *argv,ah_val *ret){
    (void)self;static const char *names[]={"menuPush","menuRef","menuPop","menuClearStack"};
    menu *m=ah_user(vm);return gt4_game_menu_stack_action(vm,game_object(m),names[(intptr_t)data],argc,argv,ret);
}

static ah_val color_value(ah_vm *vm,const double rgba[4])
{
    ah_obj *color=ah_module_new(vm,"MColorObject",NULL,NULL,NULL);const char *keys[]={"r","g","b","a"};
    for(int i=0;i<4;i++)ah_set_member(vm,color,keys[i],ah_float(rgba[i]));return ah_objv(color);
}
static int nat_color(ah_vm *vm,ah_obj *self,void *data,int argc,const ah_val *argv,ah_val *ret)
{
    const char *keys[]={"r","g","b","a"};double rgba[4]={0,0,0,1};
    if(!data){if(argc!=3 && argc!=4)return ah_error(vm,"color requires RGB and optional alpha");for(int i=0;i<argc;i++){if(!ah_is_num(argv[i]) || !isfinite(ah_num(argv[i])))return ah_error(vm,"invalid color component");rgba[i]=fmin(1,fmax(0,ah_num(argv[i])));}for(int i=0;i<4;i++)ah_set_member(vm,self,keys[i],ah_float(rgba[i]));*ret=ah_objv(self);}
    else {if(argc!=3 || !ah_as_obj(argv[0],AO_MODULE) || !ah_as_obj(argv[1],AO_MODULE) || !ah_is_num(argv[2]) || !isfinite(ah_num(argv[2])))return ah_error(vm,"invalid color interpolation");double t=fmin(1,fmax(0,ah_num(argv[2])));for(int i=0;i<4;i++){ah_val a,b;if(!ah_get_member(vm,argv[0].u.o,keys[i],&a) || !ah_get_member(vm,argv[1].u.o,keys[i],&b) || !ah_is_num(a) || !ah_is_num(b))return ah_error(vm,"invalid color");rgba[i]=ah_num(a)+(ah_num(b)-ah_num(a))*t;}*ret=color_value(vm,rgba);}
    return 0;
}
/* ---- the quick menus' race (MQuickWork, func_001255F8's class) ---- */
static menu_quick_race quick_race;
static uint64_t quick_result_generation=1;
void menu_update_quick_player(int32_t power,int32_t weight){
 if(quick_race.player<0 || quick_race.player>=quick_race.entries)return;
 quick_race.grid[quick_race.player].power=power;quick_race.grid[quick_race.player].weight=weight;quick_result_generation++;}
static uint64_t quick_tune_generation=1;
void menu_set_quick_race(const menu_quick_race *q){if(q)quick_race=*q;else memset(&quick_race,0,sizeof quick_race);quick_result_generation++;quick_tune_generation++;}
/* quick-arcade.ad L73..80: replay icons belong to the completed session. */
static uint32_t quick_best=0x157529ffu;
void menu_set_quick_best(uint32_t best){quick_best=best;}
static int32_t quick_best_speed;
void menu_set_quick_best_speed(int32_t speed){quick_best_speed=speed;}
static int quick_ghost;
void menu_set_quick_ghost(int on){quick_ghost=!!on;}
/* The PS2 loads the Arcade again when the race's quick menu leaves.
   SettingPopup's module code (arcade.ad L4651..4654) then hides the Veil
   its OK raised (black with the course and car names, L4981..4987) and
   re-arms the three fade actors; CarRoot's CarFace (hidden at L4991)
   comes back as authored. The host's cached Arcade runs the same. */
void menu_arcade_return(menu *m)
{
    ah_val popup,veil,root,face,part;
    if(!m || !m->project || strcmp(ah_module_name(m->project),"ArcadeProject"))return;
    if(ah_get_member(m->vm,m->project,"SettingPopup",&popup) && ah_as_obj(popup,AO_MODULE)) {
        ah_obj *sp=popup.u.o;
        if(ah_get_member(m->vm,sp,"Veil",&veil) && ah_as_obj(veil,AO_MODULE)) {
            ah_set_member(m->vm,veil.u.o,"visible",ah_int(0));
            static const char *const parts[]={"bg","Course","Car"};
            for(int k=0;k<3;k++)if(ah_get_member(m->vm,veil.u.o,parts[k],&part) && ah_as_obj(part,AO_MODULE)) {
                ah_val ret;
                if(ah_call_member(m->vm,sp,"setFadeActor",1,&part,&ret)<0)
                    fprintf(stderr,"arcade return: setFadeActor failed\n");
            }
        }
    }
    if(ah_get_member(m->vm,m->project,"CarRoot",&root) && ah_as_obj(root,AO_MODULE) &&
       ah_get_member(m->vm,root.u.o,"CarFace",&face) && ah_as_obj(face,AO_MODULE))
        ah_set_member(m->vm,face.u.o,"visible",ah_int(1));
}
void menu_set_quick_completed(menu *m,int completed)
{
    quick_race.can_replay=!!completed;
    /* The host caches projects; the native race caller must re-run L65..173
       when the session changes, including visibility and setIconPosition. */
    if(m && (!strcmp(ah_module_name(m->project),"QuickArcadeProject") ||
             !strcmp(ah_module_name(m->project),"QuickTimeTrialProject")))
        m->entered[0]=0;
}
int menu_race_is_split(menu *m)
{
    ah_val v;return m && ah_get_member(m->vm,game_object(m),"next_sequence",&v) &&
        ah_cstr(v) && !strcmp(ah_cstr(v),"split_battle");
}
const char *menu_split_car(menu *m,int port)
{
    /* arcade.ad GarageRoot.onActivate retains the selected garage car.
       GameRoot.doGame also writes car_code in single-player modes. */
    if(!m || port<0 || port>1)return NULL;
    ah_val v=ah_nil();ah_obj *game=game_object(m);
    ah_get_member(m->vm,game,port?"car_garage_2p":"car_garage",&v);
    const char *label=car_label_of(m->vm,v);if(label && *label)return label;
    ah_get_member(m->vm,game,port?"car_code_2p":"car_code",&v);
    label=ah_cstr(v);return label?label:"";
}
int menu_split_owned(menu *m,int port,uint8_t body[0x4b8],int *color)
{
    if(!m || port<0 || port>1 || !body || !color)return -1;
    ah_obj *game=game_object(m);ah_val code=ah_nil(),car=ah_nil();
    *color=(int)menu_game_number(m,port?"car_color_2p":"car_color",0);
    ah_get_member(m->vm,game,port?"car_garage_2p":"car_garage",&car);
    if(car.t==AV_NIL){
        ah_get_member(m->vm,game,port?"car_code_2p":"car_code",&code);
        return ah_cstr(code) && *ah_cstr(code)?0:-1;
    }
    /* 00441248 selects its sheet; +0x18 is signed paint. */
    uint8_t setup[0x178];
    if(!gt4_game_car_body(m->vm,ah_as_obj(car,AO_MODULE),body) ||
       !gt4_career_active_setup(body,setup))return -1;
    *color=(int8_t)setup[0x18];return 1;
}
int menu_race_is_time_trial(menu *m)
{
    ah_val v;return m && ah_get_member(m->vm,game_object(m),"next_sequence",&v) &&
        ah_cstr(v) && !strcmp(ah_cstr(v),"time_trial");
}

int menu_get_career_request(menu *m,menu_career_request *out,char *error,size_t capacity)
{
    ah_val sequence=ah_nil(),race=ah_nil(),garage=ah_nil(),car=ah_nil();
    const char *failure="missing menu or output";
    if(out)memset(out,0,sizeof *out);
    if(!m || !out)goto fail;
    ah_obj *game=game_object(m);
    ah_get_member(m->vm,game,"next_sequence",&sequence);
    const char *kind=ah_cstr(sequence);
    failure="not a career event sequence";
    if(!kind || (strcmp(kind,"solo_event") && strcmp(kind,"championship")))goto fail;
    /* _tmp_gtmode.ad L10920 and L10925..10928: preserve the authored
       label and route; a championship code is not an Arcade course. */
    ah_get_member(m->vm,game,"race_code",&race);
    const char *label=ah_cstr(race);
    failure="missing or oversized race label";
    if(!label || !*label || strlen(label)>=sizeof out->race)goto fail;
    ah_get_member(m->vm,game,"garage",&garage);
    ah_get_member(m->vm,ah_as_obj(garage,AO_MODULE),"riding_car",&car);
    failure="equipped garage car has no complete original body";
    /* 003EE3C8 / 00444840: session setup comes from the owned car;
       gt4_game_car_body also carries its current +0x498 wear state. */
    if(!gt4_game_car_body(m->vm,ah_as_obj(car,AO_MODULE),out->player_body))goto fail;
    snprintf(out->race,sizeof out->race,"%s",label);
    out->championship=!strcmp(kind,"championship");
    if(error && capacity)*error=0;
    return 1;
fail:
    if(out)memset(out,0,sizeof *out);
    if(error && capacity)snprintf(error,capacity,"cannot read career event request: %s",failure);
    return 0;
}
int menu_race_is_license(menu *m){
    ah_val kind;return m && ah_get_member(m->vm,game_object(m),"next_sequence",&kind) && ah_cstr(kind) && !strcmp(ah_cstr(kind),"license");
}
int menu_get_license_request(menu *m,char code[32]){
    ah_val value;if(!m || !code || !menu_race_is_license(m) || !ah_get_member(m->vm,game_object(m),"race_code",&value))return 0;
    const char *text=ah_cstr(value);if(!text || !*text || strlen(text)>=32 || gt4_career_license_index(text)<0)return 0;
    snprintf(code,32,"%s",text);return 1;
}
uint32_t menu_license_best(menu *m,const char *code){return m?gt4_game_license_best(m->vm,game_object(m),code):0x157529ffu;}
/* gtmode.ad MissionRoot.onActivate: race_code "l0m%04d", exit_menu("mission") */
/* gtmode.ad CourseEntryRoot FreeRun / Practice: exit_menu("freerun") or
   ("practice") (00117668 / 00118670), Photo Drive (0011ABE0):
   1 Free Run, 2 Practice, 3 Photo Drive, 0 neither */
int menu_race_is_free(menu *m){
    ah_val kind;if(!m || !ah_get_member(m->vm,game_object(m),"next_sequence",&kind) || !ah_cstr(kind))return 0;
    return !strcmp(ah_cstr(kind),"freerun")?1:!strcmp(ah_cstr(kind),"practice")?2:!strcmp(ah_cstr(kind),"photo_drive")?3:
           !strcmp(ah_cstr(kind),"machinetest")?4:0;   /* gtmode.ad L3607/L3667/L3698: Power & Speed (00115A90) */
}
/* the course they chose (game.course_code) and the garage's riding car:
   its label, paint and the body the race takes as it is; *laps those of
   the race_code Practice sets (practice_info.race_code), 0 without one */
/* a string of the game object (main.game.<name>), "" without one */
const char *menu_game_text(menu *m,const char *name){
    ah_val v;if(!m || !ah_get_member(m->vm,game_object(m),name,&v) || !ah_cstr(v))return "";return ah_cstr(v);
}
int menu_free_request(menu *m,char course[32],char car[96],int *color,uint8_t body[0x4b8],int *laps){
    if(!m)return 0;ah_obj *game=game_object(m);ah_val scene=ah_nil(),garage=ah_nil(),vehicle=ah_nil(),paint=ah_nil(),code=ah_nil();
    ah_get_member(m->vm,game,"course_code",&scene);
    uint8_t row[151];*laps=0;load_race_tables(m);
    if(ah_get_member(m->vm,game,"race_code",&code) && ah_cstr(code) && *ah_cstr(code) &&
       gt4_career_race_row(gt4_game_race_lookup_data(),m->race_labels,m->race_labels_size,m->race_db,m->race_db_size,ah_cstr(code),row))
        *laps=gt4_career_race_field(row,GT4_RACE_LAPS);
    ah_get_member(m->vm,game,"garage",&garage);
    ah_get_member(m->vm,ah_as_obj(garage,AO_MODULE),"riding_car",&vehicle);
    const char *label=car_label_of(m->vm,vehicle);
    ah_get_member(m->vm,ah_as_obj(vehicle,AO_MODULE),"color_index",&paint);
    if(!ah_cstr(scene) || !*ah_cstr(scene) || !label || !*label)return 0;
    snprintf(course,32,"%s",ah_cstr(scene));snprintf(car,96,"%s",label);
    *color=ah_is_num(paint)?(int)ah_num(paint):0;
    return gt4_game_car_body(m->vm,ah_as_obj(vehicle,AO_MODULE),body);
}
/* 0011B160 +0..20 -> 003BB440: Photo Drive uses the disc's "photo_drive"
   RACE row, not Arcade or FreeRun rules. No RACES opponent list exists. */
int menu_photo_drive_rules(menu *m,uint8_t row[151]){
    if(!m || !row || menu_race_is_free(m)!=3)return 0;load_race_tables(m);
    return gt4_career_race_row(gt4_game_race_lookup_data(),m->race_labels,m->race_labels_size,
        m->race_db,m->race_db_size,"photo_drive",row);
}
int menu_race_is_mission(menu *m){
    ah_val kind;return m && ah_get_member(m->vm,game_object(m),"next_sequence",&kind) && ah_cstr(kind) && !strcmp(ah_cstr(kind),"mission");
}
int menu_get_mission_request(menu *m,char code[32]){
    ah_val value;if(!m || !code || !menu_race_is_mission(m) || !ah_get_member(m->vm,game_object(m),"race_code",&value))return 0;
    const char *text=ah_cstr(value);
    if(!text || strlen(text)!=7 || strncmp(text,"l0m",3) || strspn(text+3,"0123456789")!=4)return 0;
    snprintf(code,32,"%s",text);return 1;
}
int menu_apply_mission_result(menu *m,const char *race,int finished,int32_t position,
    uint32_t race_time,int32_t aspec,uint32_t prize,uint32_t *award)
{
    if(!m || !gt4_game_apply_mission_result(m->vm,game_object(m),race,finished,position,race_time,aspec,prize,award))return 0;
    menu_save_capture(m);return 1;
}
int menu_license_begin(menu *m,const char *code,gt4_career_license_attempts *visit){return m && gt4_game_license_begin(m->vm,game_object(m),code,visit);}
int menu_license_attempt(menu *m,const char *code,gt4_career_license_attempts *visit,int32_t state,uint32_t time,uint32_t date){
    if(!m || !gt4_game_license_attempt(m->vm,game_object(m),code,visit,state,time,date))return 0;
    menu_save_capture(m);return 1;
}
int menu_license_leave(menu *m,const char *code,const gt4_career_license_attempts *visit){
    if(!m || !gt4_game_license_leave(m->vm,game_object(m),code,visit))return 0;
    menu_save_capture(m);return 1;
}
void menu_license_return_prepare(menu *m){
    if(!m)return;
    /* gtmode.ad L8394 hides the outgoing context before exit_menu("license").
       LicenseDetailRoot.onInitialize L8226..8314 has no matching reveal:
       it runs in a newly loaded project on the PS2. Our cached context must
       discard that outgoing visibility override, preserving cursor position
       and interpolation while the exam page is initialized again. */
    ah_set_member(m->vm,m->context,"cursor_visible",ah_int(1));
    /* gtmode.ad LicenseDetailRoot.onInitialize L8226..8227 clears the
       outgoing Veil; L8254..8296 refreshes all exam medals. onActivate
       L8394..8398 shows that veil before exit_menu("license"). A cached
       project must re-run initialization when the examination returns. */
    for(int i=0;i<ui_root_count(m->ui);i++)
        if(!strcmp(ui_root_name(m->ui,i),"LicenseDetailRoot"))m->entered[i]=0;
}

int menu_apply_car_wear(menu *m,const uint8_t wear[32]){
    if(!m || !gt4_game_apply_car_wear(m->vm,game_object(m),wear))return 0;
    menu_save_capture(m);return 1;
}
int menu_apply_event_distances(menu *m,uint32_t a,uint32_t b){
    if(!m || !gt4_game_apply_event_distances(m->vm,game_object(m),a,b))return 0;
    menu_save_capture(m);return 1;
}
int menu_commit_career_distances(menu *career,menu *quick,uint32_t a,uint32_t b,int *pending){
    if(!career || !quick || !pending)return 0;
    if(!*pending)return 1;
    /* 00113B48 +0x4fc..0x530 (00114044..78) and 00118EF0
       +0x86c..0x890 (0011975c..80): unsigned metres enter persistent
       u64 totals before returning to GT Mode. Preserve PrizeRoot's paid
       balance across the separate native VMs, then refresh its snapshot
       so the outgoing capture/Auto Save cannot overwrite the totals. */
    menu_save_capture(quick);menu_save_refresh(career);
    if(!menu_apply_event_distances(career,a,b))return 0;
    menu_save_refresh(quick);*pending=0;return 1;
}
int menu_apply_career_result(menu *m,const char *race,int finished,int32_t position,
    uint32_t best_lap,int32_t mode,int32_t aspec,uint32_t prize,uint8_t *best,uint32_t *award)
{
    if(!m || !gt4_game_apply_event_result(m->vm,game_object(m),race,finished,position,best_lap,mode,aspec,prize,best,award))return 0;
    menu_save_capture(m);return 1;
}

int menu_apply_championship_result(menu *m,const char *race,const char *series,
    int finished,int final_session,int32_t position,uint32_t lap,int32_t mode,int32_t aspec,
    uint32_t prize,const int32_t points[6],int16_t units,uint32_t *award,uint32_t *grand){
    if(!m || !gt4_game_apply_championship_result(m->vm,game_object(m),race,series,
        finished,final_session,position,lap,mode,aspec,prize,points,units,award,grand))return 0;
    menu_save_capture(m);return 1;
}
void menu_quick_event_result(menu *m,int finished,uint32_t prize)
{
    quick_race.finished=!!finished;quick_race.prize=prize;quick_result_generation++;
    if(m && (!strcmp(ah_module_name(m->project),"RaceEventProject") || !strcmp(ah_module_name(m->project),"ChampionshipProject") || !strcmp(ah_module_name(m->project),"MissionProject")))
        for(int i=0;i<ui_root_count(m->ui);i++)m->entered[i]=0;
}
void menu_quick_event_context(menu *m,const char *race)
{
    if(!m)return;
    /* 0010FBD8..0010FCA0 rebuilds quick-license for the next exam.
       Cached pages must finalize before onInitialize runs again:
       quick-license.ad L312 restores Info.h to info_h; otherwise L256
       compares against the previous enlarged height and hides the bar. */
    if(!strcmp(ah_module_name(m->project),"LicenseProject"))
        while(m->depth)finalize_page(m,m->pages[--m->depth]);
    /* _tmp_gtmode.ad L10920..10922 / quick-event.ad L201: return route
       remains GT Mode even though this project has its own native VM. */
    ah_obj *game=game_object(m);
    ah_set_member(m->vm,game,"race_code",ah_str(m->vm,race));
    ah_set_member(m->vm,game,"major_menu_project",ah_str(m->vm,"gtmode"));
    ah_set_member(m->vm,game,"next_sequence",ah_str(m->vm,"menu"));
    ah_set_member(m->vm,game,"next_menu_project",ah_str(m->vm,"gtmode"));
    /* 0010FBD8..0010FCA0 rebuilds quick-license for every new exam.
       Our cached project must run its initialization again for that work. */
    if(!strcmp(ah_module_name(m->project),"LicenseProject"))
        for(int i=0;i<ui_root_count(m->ui);i++)m->entered[i]=0;
}
int menu_enter_quick_start(menu *m)
{
    ah_val page;if(!m)return 0;
    /* 0010FE40 reload / quick-license.ad L312: finalize the cached
       Information area on same-exam returns too (Options/Retry/result). */
    if(!strcmp(ah_module_name(m->project),"LicenseProject"))
        while(m->depth)finalize_page(m,m->pages[--m->depth]);
    /* 00115A90 +0x380/+0x3A4 -> 0010FE40 reloads each menu cycle.
       Its onInitialize L99/L175 refreshes replay buttons and session best.
       The native cache must not retain the first entry's widget values. */
    for(int i=0;i<ui_root_count(m->ui);i++)m->entered[i]=0;
    /* quick-license.ad, LicenseProject.onLoad +29..33: startPage is
       LicenseRoot. This project does not define getStartPage. */
    if(!strcmp(ah_module_name(m->project),"LicenseProject")){
        if(!ah_get_member(m->vm,m->project,"LicenseRoot",&page))return 0;
    }else {ah_val selector;
        if(!ah_get_member(m->vm,m->project,"getStartPage",&selector)){
            menu_enter(m,menu_active_root(m)<0?0:menu_active_root(m));return 1;
        }
        if(ah_call_member(m->vm,m->project,"getStartPage",0,NULL,&page)<0)return 0;
    }
    for(int i=0;i<ui_root_count(m->ui);i++)if(ah_as_obj(page,AO_MODULE)==widget_module(m,ui_root_widget(m->ui,i))){menu_enter(m,i);return 1;}
    return 0;
}
int menu_enter_career_return(menu *m){
    if(!m)return 0;menu_save_refresh(m);ah_val root,list;
    /* GTmodeProject.onLoad L141..174 starts a freshly loaded project.
       Native cached roots must initialize again against the returned save;
       TopRoot.onInitialize L1152 checks dated course presents and HomeRoot
       refreshes the owned car/status. Keeping entered skips both. */
    for(int i=0;i<ui_root_count(m->ui);i++)m->entered[i]=0;
    if(!ah_get_member(m->vm,m->project,"PresentRoot",&root) || !ah_as_obj(root,AO_MODULE) ||
       ah_call_member(m->vm,root.u.o,"makeEntitledPresentListByProject",0,NULL,&list)<0 || !ah_as_obj(list,AO_ARRAY))return 0;
    /* GT Mode onLoad L141..154: the original script decides eligibility,
       consumes pending results and fills the original presentation list. */
    /* win() also consumes per-race results when the whole series has not
       qualified yet. Capture even an empty list before menu_enter refresh. */
    menu_save_capture(m);
    if(ah_array_size(list.u.o)>0){
        ah_set_member(m->vm,root.u.o,"current_list",list);
        for(int i=0;i<ui_root_count(m->ui);i++)if(widget_module(m,ui_root_widget(m->ui,i))==root.u.o){m->entered[i]=0;menu_enter(m,i);return 1;}
        return 0;
    }
    menu_enter(m,menu_active_root(m)<0?0:menu_active_root(m));return 1;
}
/* option.ad RaceRoot.reflect/apply L2092/L2106: index 0 AT, 1 MT,
   automatic_gear = 1-index. GT Mode has no Arcade game.manual field;
   003D6350 +18..20 reads the global gearbox option for the player. */
int menu_driving_manual(menu *m)
{
    return menu_option_number(m,"automatic_gear",1)==0;
}
double menu_game_number(menu *m,const char *name,double fallback)
{
    ah_val v;
    if(!m || !ah_get_member(m->vm,game_object(m),name,&v) || !ah_is_num(v))return fallback;
    return ah_num(v);
}
int menu_pending_command(const menu *m){return m && m->quick_command[0];}
static ui_widget *find_named(ui_widget *w,const char *name)
{
    if(!w)return NULL;if(w->name && !strcmp(w->name,name))return w;
    for(int i=0;i<w->nkids;i++){ui_widget *f=find_named(w->kids[i],name);if(f)return f;}
    return NULL;
}
/* tests: the active page's widget of that name focused and chosen */
int menu_test_activate(menu *m,const char *name)
{
    int root=m?menu_active_root(m):-1;ui_widget *w=root>=0?find_named(ui_root_widget(m->ui,root),name):NULL;
    if(!w)return 0;set_focus(m,w);menu_send_input(m,MENU_ACCEPT);return 1;
}
void menu_test_quick_command(menu *m,const char *command){if(m && command)snprintf(m->quick_command,sizeof m->quick_command,"%s",command);}
int menu_take_quick_command(menu *m,char *out,size_t size)
{
    if(!m || !m->quick_command[0])return 0;
    snprintf(out,size,"%s",m->quick_command);m->quick_command[0]=0;return 1;
}
static int quick_index(int argc,const ah_val *argv)
{
    int i=argc && ah_is_num(argv[0])?(int)ah_num(argv[0]):-1;
    return i>=0 && i<quick_race.entries && i<6?i:-1;
}
/* 001109D4 calls 00445880 after garage construction (paint starts at
   zero): an index >= the linked VARIATION row count is rejected, keeping
   zero. 004458D0 +0xA4 selects order == paint+1; 00445B28 +0x1C..+0x3C
   copies the four chip words at +0x24. Do not index a sorted model list. */
static int quick_paint_chip(menu *m,const char *car,int color,uint8_t chip[4][4])
{
    load_car_tables(m);uint32_t id,key;uint8_t row[256];int count=0;
    memset(chip,0,16);
    if(!car || !gt4_specdb_label(m->car_labels,m->car_labels_size,car,&id) ||
       gt4_specdb_row(m->car_links,m->car_links_size,id,row,sizeof row)!=8)return 0;
    key=car_u32(row);
    while(gt4_specdb_row_nth(m->car_variants,m->car_variants_size,key,count,row,sizeof row)==52)count++;
    if(color>=count)color=0; /* 00445880 rejects; the new garage keeps zero */
    color=(int8_t)color; /* garage +0x18: sb, then lb in 00445AE8 */
    if(color==-1)return 0;
    for(int i=0;i<count;i++){
        gt4_specdb_row_nth(m->car_variants,m->car_variants_size,key,i,row,sizeof row);
        if(car_u32(row+4)==(uint32_t)color+1){memcpy(chip,row+36,16);return 1;}
    }
    return 0;
}
/* getPower (func_00124F68: quick work +0x364), getWeight (+0x37C),
   getTireType (+0x394), getGridCarName (+4, the car's name),
   getColorChipInfo (+0x304: four colours, VARIATION +0x24 of its paint) */
static int nat_quick_car(ah_vm *vm,ah_obj *self,void *d,int argc,const ah_val *argv,ah_val *ret)
{
    (void)self;menu *m=ah_user(vm);int which=(int)(intptr_t)d,i=quick_index(argc,argv);
    if(which==0){*ret=ah_int(i<0?0:(quick_race.championship?quick_race.total_points[i]:quick_race.grid[i].power));return 0;}
    if(which==1){*ret=ah_int(i<0?0:(quick_race.championship?quick_race.round_points[i]:quick_race.grid[i].weight));return 0;}
    if(which==2){*ret=ah_int(i<0?0:quick_race.tire);return 0;}
    /* 00127DF8 loads the raw result word at quick +0x454 +4*index. */
    if(which==5){*ret=ah_int(i<0?0:(int32_t)quick_race.grid[i].time);return 0;}
    if(which==3){const char *name=i<0?NULL:car_name(m,quick_race.grid[i].car);*ret=ah_str(vm,name?name:"");return 0;}
    {
        ah_obj *a=ah_array_new(vm);uint8_t chip[4][4];
        int valid=i>=0 && quick_paint_chip(m,quick_race.grid[i].car,quick_race.grid[i].color,chip);
        for(int k=0;k<4;k++) {
            double rgba[4]={0,0,0,0};
            if(valid)for(int j=0;j<4;j++)rgba[j]=chip[k][j]/255.0;
            ah_array_push(vm,a,color_value(vm,rgba));
        }
        *ret=ah_objv(a);return 0;
    }
}
/* MUtility.GetTimeString: 001B0D78 -> 0042E478; strings 006A4F70/80/98.
   0x7f is the original dash glyph. Arithmetic is unsigned milliseconds. */
static int nat_time_string(ah_vm *vm,ah_obj *self,void *d,int argc,const ah_val *argv,ah_val *ret)
{
    (void)self;(void)d;uint32_t t=argc?(uint32_t)ah_num(argv[0]):0x157529ffu;char text[64];
    if(t==0x157529ffu)strcpy(text,"\x7f\x7f'\x7f\x7f.\x7f\x7f\x7f");
    else if(t/3600000u)snprintf(text,sizeof text,"%u:%02u'%02u.%03u",t/3600000u,t/60000u%60,t/1000u%60,t%1000);
    else snprintf(text,sizeof text,"%u'%02u.%03u",t/60000u%60,t/1000u%60,t%1000);
    *ret=ah_str(vm,text);return 0;
}
static ah_val calendar_label(menu *m,uint64_t key){
    /* 00133370 -> 00447CA0: shared SpecDB key-to-label resolver. */
    unsigned table=(unsigned)(key>>32);const char *name=table==0?"GENERIC_CAR":table==0x26?"TUNED_CAR":table==0x23?"RACE":table==0x22?"COURSE":table==29?"WHEEL":table==30?"WING":NULL;
    if(!name)return ah_nil();char path[96];snprintf(path,sizeof path,"/specdb/GT4_US2560/%s.idi",name);size_t size=0;uint8_t *labels=gt4_vol_read(m->vol,path,&size);
    const char *label=gt4_specdb_label_for_id(labels,size,(uint32_t)key);ah_val value=label?ah_str(m->vm,label):ah_nil();free(labels);return value;
}
static int nat_calendar_get(ah_vm *vm,ah_obj *self,void *data,int argc,const ah_val *argv,ah_val *ret){
    (void)self;menu *m=data;uint8_t e[16];int found=gt4_game_calendar_event(vm,profile_module(vm,game_object(m),"calendar"),argc,argv,e);*ret=ah_nil();
    if(found<0)return ah_error(vm,"invalid original diary history");if(!found)return 0;
    const gt4_career_home_data *home=gt4_game_home_data();const gt4_career_race_lookup_data *race=gt4_game_race_lookup_data();const gt4_career_part_data *parts=gt4_game_part_data();
    if(!home || !race || !parts || e[0]>8)return ah_error(vm,"original diary enums unavailable");
    ah_obj *event=ah_array_new(vm);ah_array_push(vm,event,ah_str(vm,home->event_names[e[0]]));
    uint64_t key=0;for(unsigned i=0;i<8;i++)key|=(uint64_t)e[8+i]<<(8*i);
    if(e[0])ah_array_push(vm,event,calendar_label(m,key));
    /* Callback 00133370 +0x17c..0x1850: preserve each event's distinct
       array schema, consumed directly by DiaryRoot's authored scripts. */
    if(e[0]==1 || e[0]==2){ah_array_push(vm,event,ah_int(e[1]));ah_array_push(vm,event,e[0]==2?ah_int(e[2]|(unsigned)e[3]<<8):e[2]<5?ah_str(vm,home->car_event_names[e[2]]):ah_nil());}
    else if(e[0]==6){ah_array_push(vm,event,e[1]<4?ah_str(vm,parts->wheel_categories[e[1]]):ah_nil());ah_array_push(vm,event,ah_int(e[2]));}
    else if(e[0]==3 || e[0]==4 || e[0]==5 || e[0]==8){
        if(e[0]==8)ah_array_push(vm,event,e[3]<8?ah_str(vm,home->course_event_names[e[3]]):ah_nil());
        const char *grade=gt4_career_result_name(race,e[1]),*method=NULL;for(unsigned i=0;i<15;i++)if(race->method_values[i]==e[2]){method=race->method_names[i];break;}
        ah_array_push(vm,event,grade?ah_str(vm,grade):ah_nil());ah_array_push(vm,event,method?ah_str(vm,method):ah_nil());ah_val time=ah_int(car_u32(e+4)),text;nat_time_string(vm,NULL,NULL,1,&time,&text);ah_array_push(vm,event,text);
    }
    *ret=ah_objv(event);return 0;
}
static int nat_calendar_init(ah_vm *vm,ah_obj *self,void *data,int argc,const ah_val *argv,ah_val *ret){
    (void)argc;(void)argv;ah_set_member(vm,self,"getEvent",ah_bind(vm,self,ah_native(vm,"getEvent",nat_calendar_get,data)));*ret=ah_nil();return 0;
}
static int power_unit(menu *m);
/* 004729A0: retain the original float words, including HP's last bit. */
static float power_display(float value,int unit)
{
    if(unit==1 || unit==2 || unit==4){
        uint32_t bits=unit==4?0x3f3c49bau:0x3f7c76f2u;float factor;
        memcpy(&factor,&bits,4);value*=factor;
    }
    return value;
}
static int nat_photo_format(ah_vm *vm,ah_obj *self,void *d,int argc,const ah_val *argv,ah_val *ret)
{
    (void)self;(void)d;const char *fmt=argc?ah_cstr(argv[0]):NULL;char text[96];
    /* gtmode.ad L3995/L14895: MUtility's integer format, 00490A40. */
    if(argc==2 && fmt && !strcmp(fmt,"%d") && ah_is_num(argv[1])){snprintf(text,sizeof text,"%d",(int32_t)to_int(argv[1]));*ret=ah_str(vm,text);return 0;}
    /* 001B0ED0 +70..84 -> 00490A40. PhotoShoot L106/L2350 uses
       %.1t; caller passes rounded tenths. A: other conversions pending. */
    int precision=fmt && !strcmp(fmt,"%.1t")?1:fmt && !strcmp(fmt,"%.2t")?2:0;
    if(argc!=2 || !precision || !ah_is_num(argv[1]) || !isfinite(ah_num(argv[1])) || fabs(ah_num(argv[1]))>=9223372036854775808.)return ah_error(vm,"unsupported Photo Travel format");
    int64_t v=(int64_t)ah_num(argv[1]);if(v==INT64_MIN)return ah_error(vm,"invalid fixed decimal");
    uint64_t a=v<0?(uint64_t)-v:(uint64_t)v;
    unsigned scale=precision==1?10:100;
    snprintf(text,sizeof text,"%s%llu.%0*llu",v<0?"-":"",(unsigned long long)(a/scale),precision,(unsigned long long)(a%scale));
    *ret=ah_str(vm,text);return 0;
}
/* 0013DAA0/E0E0/E228 -> 00445FF8/004460F8/00446138: ENGINE
   string keys +0/+8/+4. 0013DBE8/DD90/DF38 format nonzero dimensions.
   00146358/001463E0 use kg and return ---- for undisclosed mass. */
static int nat_catalog_value(ah_vm *vm,ah_obj *self,void *d,int argc,const ah_val *argv,ah_val *ret)
{
    menu *m=ah_user(vm);int field=(int)(intptr_t)d;const char *label=car_label_of(vm,ah_objv(self));char text[128];
    if(field<3){
        uint8_t e[128];int drive;size_t size;
        if(!gt4_arcade_catalog_parts(arcade_of(m),label,e,&drive))return ah_error(vm,"catalog engine unavailable");
        uint8_t *strings=gt4_vol_read(m->vol,"/specdb/GT4_US2560/UnistrDB.sdb",&size);
        const char *value=strings?gt4_specdb_string(strings,size,car_u32(e+(field==2?8:field==1?4:0))):NULL;
        *ret=ah_str(vm,value?value:"no value");free(strings);return 0;
    }
    unsigned value;
    if(field==7){int32_t stats[3];if(!gt4_game_car_stats(vm,self,stats))return ah_error(vm,"current car statistics unavailable");value=(unsigned)stats[2];}
    else {uint16_t geometry[4];if(!gt4_arcade_catalog_geometry(arcade_of(m),label,geometry))return ah_error(vm,"catalog geometry unavailable");value=geometry[field-3];}
    if(field>=6){if(argc!=1 || !ah_cstr(argv[0]) || strcmp(ah_cstr(argv[0]),"%d%s"))return ah_error(vm,"unsupported mass format");snprintf(text,sizeof text,value?"%ukg":"----",value);}
    else snprintf(text,sizeof text,value?"%u":"-",value);
    *ret=ah_str(vm,text);return 0;
}
/* 00127C48/00127D20 -> stock ENGINE via 00445EE8..00445FB8.
   Only the two catalog formats authored by share/catalog_util.ad are accepted.
   Decimal/rounding parity with 00491690 remains A pending formatter oracle. */
static int nat_quick_catalog(ah_vm *vm,ah_obj *self,void *d,int argc,const ah_val *argv,ah_val *ret)
{
    (void)self;menu *m=ah_user(vm);uint8_t e[128];int drive;
    int player=quick_race.player;const char *label=self?car_label_of(vm,ah_objv(self)):NULL;
    if(!label){if(player<0 || player>=quick_race.entries)return ah_error(vm,"quick player unavailable");label=quick_race.grid[player].car;}
    if(!gt4_arcade_catalog_parts(arcade_of(m),label,e,&drive))return ah_error(vm,"stock catalog parts unavailable");
    int mode=(int)(intptr_t)d,torque=mode&1;const char *format=argc?ah_cstr(argv[0]):NULL;
    if(!format || strcmp(format,torque?"%.2t%s/%s%s":"%d%s/%s%s"))return ah_error(vm,"unsupported catalog format");
    size_t size;uint8_t *strings=gt4_vol_read(m->vol,"/specdb/GT4_US2560/UnistrDB.sdb",&size);
    const char *rpm=strings?gt4_specdb_string(strings,size,car_u32(e+(torque?20:16))):NULL;
    if(!rpm){free(strings);return ah_error(vm,"catalog RPM string unavailable");}
    char text[128];
    int32_t stats[3]={0};if(mode>=2 && !gt4_game_car_stats(vm,self,stats)){free(strings);return ah_error(vm,"current car statistics unavailable");}
    /* 00145FE8/00146198: undisclosed RPM ('-') suppresses the whole value. */
    if(rpm[0]=='-'){free(strings);*ret=ah_str(vm,"----");return 0;}
    if(!torque){
        static const char *units[]={"PS","HP","BHP","CH","kW","CV","PF"};
        int unit=power_unit(m);float value=mode>=2?(float)stats[0]/100.0f:(float)(e[26]|(unsigned)e[27]<<8);
        value=power_display(value,unit);
        snprintf(text,sizeof text,"%d%s/%srpm",(int)floorf(value+0.5f),units[unit],rpm);
    }else{
        const char *unit=menu_option_text(m,"unit_torque","FTLB"),*label="kgf.m";
        float value=mode>=2?(float)stats[1]:(float)(e[28]|(unsigned)e[29]<<8);
        /* 00472A08 +0x38/+0x50; torque is stored in hundredths. */
        if(!strcmp(unit,"FTLB")){uint32_t bits=0x40e773ea;float factor;memcpy(&factor,&bits,4);value*=factor;label="ft.lb";}
        else if(!strcmp(unit,"NM")){uint32_t bits=0x411ccccc;float factor;memcpy(&factor,&bits,4);value*=factor;label="NM";}
        snprintf(text,sizeof text,"%.2f%s/%srpm",floorf(value+0.5f)/100.0,label,rpm);
    }
    free(strings);*ret=ah_str(vm,text);return 0;
}
static int quick_aspec_available(menu *m,const char *race)
{
    ah_vm *vm=m->vm;uint8_t row[151];unsigned passed=0;load_race_tables(m);
    if(!race || !*race || !gt4_career_race_row(gt4_game_race_lookup_data(),m->race_labels,m->race_labels_size,m->race_db,m->race_db_size,race,row))return 0;
    unsigned need=row[0x45];
    if(need>=1 && need<=5){
        static const char *series[]={"l0b","l0a","lib","lia","l0s"};ah_val records,result;
        if(ah_get_member(vm,game_object(m),"race_record",&records) && records.t==AV_OBJ){
            ah_val arg=ah_str(vm,series[need-1]);
            if(ah_call_member(vm,records.u.o,"isLicensePassed",1,&arg,&result)>=0 && ah_truthy(result))passed=1u<<(need-1);
        }
    }
    return gt4_career_aspec_allowed((uint8_t)need,passed);
}
int menu_career_aspec_available(menu *m,const char *race){
    return m?quick_aspec_available(m,race):0;
}
/* MQuickWork(): this VM's view of the quick race, its settings kept
   between calls (selectedCommand, cursorPosition, the quick tune) */
static int nat_quick_work(ah_vm *vm,ah_obj *self,void *d,int argc,const ah_val *argv,ah_val *ret)
{
    (void)self;(void)d;(void)argc;(void)argv;menu *m=ah_user(vm);ah_obj *q=m->quick_work;ah_val v;
    if(!q) {
        static const char *const methods[]={"getPower","getWeight","getTireType","getGridCarName","getColorChipInfo","getGridTime"};
        q=m->quick_work=ah_module_new(vm,"MQuickWork",NULL,NULL,NULL);ah_pin(vm,ah_objv(q));
        for(int i=0;i<6;i++)ah_set_member(vm,q,methods[i],ah_native(vm,methods[i],nat_quick_car,(void *)(intptr_t)i));
        static const char *const settings[]={"QuickTunePowerLevel","QuickTuneWeightLevel","QuickTuneTransmission",
            "QuickTuneDrivingAssist","QuickTuneGearMaxSpeed","IsQuickTuned","cursorPosition"};
        for(int i=0;i<7;i++)ah_set_member(vm,q,settings[i],ah_int(0));
        ah_set_member(vm,q,"selectedCommand",ah_str(vm,""));
        ah_set_member(vm,q,"getCatPs",ah_native(vm,"getCatPs",nat_quick_catalog,NULL));
        ah_set_member(vm,q,"getCatTq",ah_native(vm,"getCatTq",nat_quick_catalog,(void *)1));
    }
    ah_set_member(vm,q,"numberOfEntries",ah_int(quick_race.entries));
    ah_set_member(vm,q,"playerGridNumber",ah_int(quick_race.player));
    ah_set_member(vm,q,"courseLabel",ah_str(vm,quick_race.course));
    /* 00119AA0..00119AB4 populate MQuickWork +0x4f4/+0x4f0. */
    ah_set_member(vm,q,"Laps",ah_int(quick_race.laps));
    ah_set_member(vm,q,"CourseLength",ah_int(quick_race.course_length));
    /* 00114250 initializes event work +0x46c (completion), +0x490
       (driver mode), +0x4a4 (prize); 00114498 supplies the actual prize.
       quick-event.ad L22..39 / quick-championship.getStartPage consume
       these fields. Refresh on every call: cached VMs must not retain the
       preceding race's result when the next pre-race work is installed. */
    ah_set_member(vm,q,"raceLabel",ah_str(vm,quick_race.race));
    /* 0011D340: exam limits from MRaceData, not Arcade lap records. */
    if(quick_race.license){
        ah_set_member(vm,q,"canLoadGhost",ah_int(quick_race.license_load_ghost));
        ah_set_member(vm,q,"DisableLogger",ah_int(quick_race.license_disable_logger));
        /* 00127AF0 +0x20..0x34 calls 0042E478: these properties are
           formatted strings, directly assigned by quick-license.ad L276..278. */
        const char *names[]={"goldTime","silverTime","bronzeTime"};
        uint32_t times[]={quick_race.gold_time,quick_race.silver_time,quick_race.bronze_time};
        for(int i=0;i<3;i++){
            ah_val value=ah_int(times[i]),formatted;
            nat_time_string(vm,NULL,NULL,1,&value,&formatted);
            ah_set_member(vm,q,names[i],formatted);
        }
    }
    ah_set_member(vm,q,"IsSessionFinished",ah_int(!!quick_race.finished));
    /* quick-championship.ad L1049..1050 consumes these writable fields.
       Re-entering/replaying must not restore an already paid award. */
    if(m->quick_result_generation!=quick_result_generation){
        ah_set_member(vm,q,"prize",ah_int((int32_t)quick_race.prize));
        ah_set_member(vm,q,"GrandPrize",ah_int((int32_t)quick_race.grand_prize));
        m->quick_result_generation=quick_result_generation;
    }
    /* 00119918/0011A168/00119F28: championship work getters. */
    ah_set_member(vm,q,"SessionNumber",ah_int(quick_race.session_number));
    ah_set_member(vm,q,"IsFinalSession",ah_int(quick_race.final_session));
    ah_set_member(vm,q,"SeriesRank",ah_int(quick_race.series_rank));
    /* quick-arcade.ad L601 consults main.game.course_code in QuickTune.
       Native projects have separate VMs; carry the race caller's course. */
    ah_set_member(vm,game_object(m),"course_code",ah_str(vm,quick_race.course));
    ah_set_member(vm,q,"canReplay",ah_int(quick_race.can_replay));
    ah_set_member(vm,q,"CanLogger",ah_int(logger_can()));
    /* quick-tt.ad L72: SaveGhost shown while the session has a best lap
       to save (menu_set_quick_ghost). 00112E78 sets BestTime (+0x498)
       to the no-time 0x157529FF when the session opens; 00112620 lowers
       it to each race's best lap. */
    ah_set_member(vm,q,"canSaveGhost",ah_int(quick_ghost));
    ah_set_member(vm,q,"BestTime",ah_int((int32_t)quick_best));
    ah_set_member(vm,q,"BestMaxSpeed",ah_int(quick_best_speed));   /* 00115CF4..00115D70: +0x49C */
    if(quick_race.player>=0 && quick_race.player<quick_race.entries){
        const char *car=quick_race.grid[quick_race.player].car;uint32_t id;uint8_t row[24],engine[128];int drive;
        load_car_tables(m);
        if(gt4_specdb_label(m->car_labels,m->car_labels_size,car,&id) &&
           gt4_specdb_row(m->car_db,m->car_db_size,id,row,sizeof row)==24 &&
           gt4_arcade_catalog_parts(arcade_of(m),car,engine,&drive)){
            /* Quick work +0x4E4/4E8/4EC, filled at 00112F44..00112F78.
               00448908: category %100; 004488A0: model year. */
            ah_set_member(vm,q,"driveTrainType",ah_int(drive));
            ah_set_member(vm,q,"carCategory",ah_int(row[17]%100));
            ah_set_member(vm,q,"carYear",ah_int(gt4_career_car_year(row)));
        }
    }
    ah_set_member(vm,q,"IsBSpec",ah_int(!!quick_race.bspec));
    /* 00114334..00114340 / 005C4610: IsASpec is licence eligibility,
       independent of IsBSpec. quick-event.ad:311 consults it on Start. */
    ah_val race_label=ah_nil();ah_get_member(vm,q,"raceLabel",&race_label);
    ah_set_member(vm,q,"IsASpec",ah_int(menu_career_aspec_available(m,ah_cstr(race_label))));
    ah_set_member(vm,q,"IsDryCourse",ah_int(quick_race.dry));
    if(m->quick_tune_generation!=quick_tune_generation){
        /* 00111B50 +0x19C..0x238, for each race prepared: no power or
           weight change, the tyres the race runs, the options' gearbox
           (00430878) and aids (00110B70: 2 both, 1 TCS alone), the
           gearbox's top speed and range, IsQuickTuned off */
        /* quick_tune_popup.ad L370..404 reads the same game.option */
        ah_val options,o;ah_obj *opt=ah_get_member(vm,game_object(m),"option",&options)?ah_as_obj(options,AO_MODULE):NULL;
        int manual=opt && ah_get_member(vm,opt,"automatic_gear",&o) && ah_is_num(o)?(int)ah_num(o)==0:0;
        int asm_on=opt && ah_get_member(vm,opt,"assist_asm",&o) && ah_is_num(o)?(int)ah_num(o)!=0:0;
        int tcs_on=opt && ah_get_member(vm,opt,"assist_tcs",&o) && ah_is_num(o)?(int)ah_num(o)!=0:1;
        ah_set_member(vm,q,"QuickTunePowerLevel",ah_int(0));
        ah_set_member(vm,q,"QuickTuneWeightLevel",ah_int(0));
        ah_set_member(vm,q,"QuickTuneFrontTireType",ah_int(quick_race.tire));
        ah_set_member(vm,q,"QuickTuneRearTireType",ah_int(quick_race.tire));
        ah_set_member(vm,q,"QuickTuneTransmission",ah_int(manual));
        ah_set_member(vm,q,"QuickTuneDrivingAssist",ah_int(asm_on?(tcs_on?2:0):tcs_on));
        ah_set_member(vm,q,"QuickTuneGearMaxSpeed",ah_int(quick_race.gear));
        ah_set_member(vm,q,"IsQuickTuned",ah_int(0));
        m->quick_tune_generation=quick_tune_generation;
    }
    ah_set_member(vm,q,"QuickTuneGearMaxSpeedMin",ah_int(quick_race.gear_min));
    ah_set_member(vm,q,"QuickTuneGearMaxSpeedMax",ah_int(quick_race.gear_max));
    *ret=ah_objv(q);return 0;
}
/* The QuickTune as 00111DC0 takes it at the Arcade's start; IsQuickTuned
   is consumed (+0x488 = 0). 0 without a quick work. */
int menu_quick_tune(menu *m,menu_quick_tune_values *out)
{
    if(!m || !m->quick_work || !out)return 0;
    ah_obj *q=m->quick_work;ah_val v;
    static const char *const names[]={"IsQuickTuned","QuickTunePowerLevel","QuickTuneWeightLevel",
        "QuickTuneFrontTireType","QuickTuneRearTireType","QuickTuneGearMaxSpeed","QuickTuneTransmission","QuickTuneDrivingAssist"};
    int *fields[]={&out->tuned,&out->power,&out->weight,&out->front_tire,&out->rear_tire,&out->gear,&out->transmission,&out->assist};
    for(unsigned i=0;i<8;i++)*fields[i]=ah_get_member(m->vm,q,names[i],&v) && ah_is_num(v)?(int)ah_num(v):0;
    ah_set_member(m->vm,q,"IsQuickTuned",ah_int(0));
    return 1;
}
/* MUnit.GetUnitOfPower (func_001AF648) and GetPowerForDisplay (func_001AF598:
   PS converted by the option's unit, func_004729A0, rounded): PS, HP
   (x 0.98619), BHP (the same), CH, kW (x 0.7355), CV, PF */
static int power_unit(menu *m)
{
    /* option.ad PowerString uses KW; 004726B8 displays it as kW. */
    static const char *const names[]={"PS","HP","BHP","CH","KW","CV","PF"};
    const char *u="HP";
    for(unsigned i=0;i<sizeof saved_options/sizeof saved_options[0];i++)
        if(saved_options[i].valid && saved_options[i].type==1 && !strcmp(saved_options[i].name,"unit_power"))u=saved_options[i].text;
    if(m)u=menu_option_text(m,"unit_power",u);
    if(u && !strcmp(u,"kW"))return 4; /* old native preference files */
    for(int i=0;i<7;i++)if(u && !strcmp(u,names[i]))return i;
    return 1;
}
/* MUnit 001AF3B0: km/h hundredths -> the option's hundredths, rounded;
   001AF480 -> 00472680 supplies the suffix. This does not change physics. */
static int nat_velocity_unit(ah_vm *vm,ah_obj *self,void *d,int argc,const ah_val *argv,ah_val *ret)
{
    (void)self;menu *m=ah_user(vm);int mph=strcmp(menu_option_text(m,"unit_velocity","MPH"),"KMPH")!=0;
    if(!d){*ret=ah_str(vm,mph?"mph":"km/h");return 0;}
    if(argc<1 || !ah_is_num(argv[0]))return ah_error(vm,"GetVelocityForDisplay requires a value");
    float v=(float)ah_num(argv[0]);
    if(mph){uint32_t word=0x3e8e38e3u;float factor;memcpy(&factor,&word,4);v=v*factor;v=v*3.6f;v=v*0.6213712f;}
    *ret=ah_int((int32_t)(v+0.5f));return 0;
}
static int nat_power_unit(ah_vm *vm,ah_obj *self,void *d,int argc,const ah_val *argv,ah_val *ret)
{
    static const char *const names[]={"PS","HP","BHP","CH","kW","CV","PF"};
    (void)self;int unit=power_unit(ah_user(vm));
    if(!d){*ret=ah_str(vm,names[unit]);return 0;}
    float ps=argc && ah_is_num(argv[0])?(float)ah_num(argv[0]):0,v=power_display(ps,unit);
    *ret=ah_int((int32_t)(v+0.5f));return 0;
}

/* MUnit.GetTorqueForDisplay: 001AF760 +34..58 converts through
   00472A08, adds 0.5, and cvt.w.s under the game's toward-zero mode.
   GetUnitOfTorque 001AF810 -> 00472758 returns the original unit labels.
   These are display values only; no dynamics or car parameters change. */
static int torque_unit(menu *m)
{
    const char *unit="FTLB";
    for(unsigned i=0;i<sizeof saved_options/sizeof saved_options[0];i++)
        if(saved_options[i].valid && saved_options[i].type==1 && !strcmp(saved_options[i].name,"unit_torque"))unit=saved_options[i].text;
    if(m)unit=menu_option_text(m,"unit_torque",unit);
    return !strcmp(unit,"FTLB")?1:!strcmp(unit,"NM")?2:0;
}
static int nat_torque_unit(ah_vm *vm,ah_obj *self,void *d,int argc,const ah_val *argv,ah_val *ret)
{
    static const char *const names[]={"kgf.m","ft.lb","NM"};
    (void)self;int unit=torque_unit(ah_user(vm));
    if(!d){*ret=ah_str(vm,names[unit]);return 0;}
    float value=argc && ah_is_num(argv[0])?(float)ah_num(argv[0]):0;
    if(unit){uint32_t bits=unit==1?0x40e773ea:0x411ccccc;float factor;memcpy(&factor,&bits,4);value*=factor;}
    *ret=ah_int((int32_t)(value+0.5f));return 0;
}

static int nat_car_name(ah_vm *vm,ah_obj *self,void *data,int argc,const ah_val *argv,ah_val *ret)
{(void)self;(void)data;const char *code=argc?ah_cstr(argv[0]):NULL;const char *name=code?car_name(ah_user(vm),code):NULL;*ret=ah_str(vm,name?name:"");return 0;}

#include "menu_logger.inc"

static void make_engine(menu *m)
{
    ah_vm *vm = m->vm;
    ah_obj *main_ = submodule(vm, ah_root(vm), "main");

    ah_obj *game = submodule(vm, main_, "game");
    ah_obj *option = submodule(vm, game, "option");
    ah_set_member(vm,option,"apply",ah_native(vm,"apply",nat_option_apply,m));
    static const char *const selection_methods[]={"menuPush","menuRef","menuPop","menuClearStack"};
    for(int i=0;i<4;i++)ah_set_member(vm,game,selection_methods[i],ah_native(vm,selection_methods[i],nat_menu_selection,(void *)(intptr_t)i));
    ah_obj *demo=submodule(vm,game,"demonstration");
    ah_set_member(vm,demo,"movie_count",ah_int(0));
    ah_set_member(vm,demo,"resetMovieCount",ah_native(vm,"resetMovieCount",nat_demo_reset,m));
    ah_set_member(vm,game,"isCourseAvailable",ah_native(vm,"isCourseAvailable",nat_course_available,m));
    ah_set_member(vm,game,"setCourseAvailable",ah_native(vm,"setCourseAvailable",nat_course_unlock,m));
    ah_set_member(vm,game,"isCarAvailable",ah_native(vm,"isCarAvailable",nat_car_available,m));
    ah_set_member(vm,option,"limit_favorite_car_only",ah_int(0));
    ah_set_member(vm,option,"limit_favorite_course_only",ah_int(0));
    ah_set_member(vm,game,"setNeedSave",ah_native(vm,"setNeedSave",nat_need_save,m));
    ah_set_member(vm,game,"setReturnToLastMenuProject",ah_native(vm,"setReturnToLastMenuProject",nat_return_last,m));
    gt4_pad_bind_option(vm,option);
    /* PhotoShoot Mode.Reset L1143 -> 00165CA8 / 00462EC8. */
    ah_set_member(vm,game,"loadShutterSound",ah_native(vm,"loadShutterSound",nat_photo,"loadShutterSound"));
    ah_set_member(vm,game,"need_save",ah_int(0));
    /* Options for a fresh game. Values are the port's defaults until the real
       ones are read from the game (save data / engine). */
    ah_set_member(vm, option, "game_zone", ah_str(vm, "us"));
    ah_set_member(vm, option, "special_arcade_tuner", ah_str(vm, ""));
    ah_set_member(vm, option, "wide_mode", ah_int(0));
    /* Defaults of a fresh option, as 004363A0 builds GranTurismo4Option
       (getters 0017FD20..00189A60 name the offsets): race_laps +0x10C0 2,
       tire_damage +0x10C4 0, automatic_gear +0x10C8 1, assist_asm +0x10D0
       and assist_tcs +0x10D4 both 1, split_boost_level +0x1108 1,
       album_slide_effect +0x1170 1, play time
       +0x1174 8, transition +0x1180 4. demo_movie_interval (+0x116C) is 3 there and in
       the native sequence too. A native save
       overrides them. */
    ah_set_member(vm, option, "automatic_gear", ah_int(1));
    ah_set_member(vm, option, "race_display_config", ah_int(0));
    ah_set_member(vm, option, "race_display_map", ah_int(1));
    ah_set_member(vm, option, "race_display_steering", ah_int(0));
    ah_set_member(vm, option, "can_watch_ending", ah_int(0));
    ah_set_member(vm, option, "sound_type", ah_str(vm, "STEREO"));
    ah_set_member(vm, option, "enableAC3", ah_int(0));
    static const struct {const char *name;int value;} defaults[]={
        {"replay_display_enable",0},{"replay_memory_card_slot",0},
        {"opening_after_autoload",1},{"demo_movie_interval",3},{"auto_save",1},
        {"race_laps",2},{"tire_damage",0},{"assist_asm",1},{"assist_tcs",1},{"arcade_time_limit",0},
        {"split_race_laps",2},{"split_tire_damage",0},{"split_handicap",0},{"split_boost_level",1},
        {"album_slide_effect",1},{"album_slide_play_time",8},{"album_slide_transition_time",4},
        {"album_memory_card_slot",0},
        {"vibration_1p",1},{"vibration_2p",1},{"active_steering_1p",0},{"active_steering_2p",0},
        {"menu_bgm_volume_raw",120},{"menu_se_volume_raw",120},{"race_bgm_volume_raw",120},
        {"race_se_volume_raw",120},{"replay_bgm_volume_raw",120},{"replay_se_volume_raw",120},
        {"slide_bgm_volume_raw",120},{"narration",1}};
    for(unsigned i=0;i<sizeof defaults/sizeof defaults[0];i++)ah_set_member(vm,option,defaults[i].name,ah_int(defaults[i].value));
    static const char *const string_defaults[][2]={{"photo_shutter","typeA"},{"replay_mode","NORMAL"},{"replay_play_mode","STRAIGHT"},{"penalty_type","NONE"},
        {"photo_quality","STANDARD"},{"ustorage_photo_quality","STANDARD"},
        {"steering_assist_1p","AMATEUR"},{"steering_assist_2p","AMATEUR"},
        {"unit_velocity","MPH"},{"unit_power","HP"},{"unit_torque","FTLB"}};
    for(unsigned i=0;i<sizeof string_defaults/sizeof string_defaults[0];i++)ah_set_member(vm,option,string_defaults[i][0],ah_str(vm,string_defaults[i][1]));
    /* 004363A0 +128..134: manual/pro modes start at zero.
       A: 4x3 names the script default branch; enum table pending. */
    ah_set_member(vm,option,"photo_manual_focus",ah_int(0));
    ah_set_member(vm,option,"photo_professional",ah_int(0));
    ah_set_member(vm,option,"photo_aspect",ah_str(vm,"4x3"));
    const char *correction[]={"race_color_correction","replay_color_correction"};
    for(int i=0;i<2;i++){ah_obj *a=ah_array_new(vm);for(int j=0;j<4;j++)ah_array_push(vm,a,ah_int(0));ah_set_member(vm,option,correction[i],ah_objv(a));}
    ah_obj *languages=ah_array_new(vm);ah_array_push(vm,languages,ah_str(vm,"US"));
    ah_set_member(vm,main_,"UsingLanguages",ah_objv(languages));ah_set_member(vm,main_,"UsingLanguageNum",ah_int(1));

    ah_set_member(vm, game, "last_sequence", ah_str(vm, ""));
    ah_set_member(vm, game, "labo_folder", ah_int(-1));      /* labomode.ad: none remembered */
    ah_set_member(vm, game, "labo_list_index", ah_int(-1));
    ah_set_member(vm, game, "major_menu_page", ah_str(vm, ""));
    ah_set_member(vm, game, "last_menu_project", ah_str(vm, ""));
    ah_set_member(vm, game, "next_sequence", ah_str(vm, "menu"));
    ah_set_member(vm, game, "major_menu_project", ah_str(vm, "arcade"));
    ah_set_member(vm, game, "course_code", ah_str(vm, ""));
    ah_set_member(vm, option, "timeout_count_to_autodemo", ah_int(30)); /* native movie-stage attract timeout */

    ah_obj *sound = submodule(vm, main_, "sound");
    ah_set_member(vm, sound, "play", ah_native(vm, "play", nat_sound_play, m));
    ah_set_member(vm,sound,"load",ah_native(vm,"load",nat_sound_load,m));
    ah_set_member(vm,sound,"unload",ah_native(vm,"unload",nat_sound_unload,m));
    ah_set_member(vm, sound, "keepMusic", ah_native(vm, "keepMusic", nat_music, NULL));
    ah_set_member(vm,sound,"fadeOutMusic",ah_native(vm,"fadeOutMusic",nat_music,(void *)1));
    /* 0023F38C registers startStream -> 0023E5C8 -> 0023FA78.
       PaperPane (gtmode.ad:14712) uses the two-argument PCM request.
       Reuse the existing host stream/gain adapter, rather than a stub. */
    ah_set_member(vm,sound,"startStream",ah_native(vm,"startStream",nat_quick_stream,NULL));
    ah_set_member(vm,sound,"isPlayingStream",ah_native(vm,"isPlayingStream",nat_pcm_playing,NULL));
    ah_set_member(vm,sound,"startStreamEx",ah_native(vm,"startStreamEx",nat_quick_stream,NULL));
    ah_set_member(vm,sound,"stopStream",ah_native(vm,"stopStream",nat_quick_stream,(void *)1));
    ah_set_member(vm,sound,"pauseStream",ah_native(vm,"pauseStream",nat_pcm_pause,NULL));
    /* 00180AA8 +0x78..+0xA4: saved raw menu BGM volume / 128. */
    { ah_val raw;double volume=ah_get_member(vm,option,"menu_bgm_volume_raw",&raw)?ah_num(raw):120;
      ah_set_member(vm,option,"menu_bgm_volume",ah_float((float)(volume/128.))); }

    ah_obj *menu_ = submodule(vm, main_, "menu");
    ah_obj *calendar_class=ah_native_class(vm,"MCalendar",nat_calendar_init,m);gt4_calendar_bind(vm,calendar_class);ah_set_member(vm,menu_,"MCalendar",ah_objv(calendar_class));
    ah_obj *utility=submodule(vm,menu_,"MUtility");
    ah_set_member(vm,utility,"GetTimeString",ah_native(vm,"GetTimeString",nat_time_string,NULL));
    ah_set_member(vm,utility,"GetFormatString",ah_native(vm,"GetFormatString",nat_photo_format,NULL));
    ah_set_member(vm,utility,"GetMoneyString",ah_native(vm,"GetMoneyString",nat_money_string,m));
    ah_obj *race=ah_native_class(vm,"MRaceData",nat_race_init,m);
    ah_set_member(vm,race,"available",ah_native(vm,"available",nat_race_available,m));
    ah_set_member(vm,menu_,"MRaceData",ah_objv(race));
    ah_obj *color=ah_native_class(vm,"MColorObject",nat_color,NULL);
    ah_set_member(vm,color,"interpolate",ah_native(vm,"interpolate",nat_color,(void *)1));
    ah_set_member(vm,menu_,"MColorObject",ah_objv(color));
    ah_set_member(vm,menu_,"MProgress",ah_native(vm,"MProgress",nat_progress,NULL));
    ah_set_member(vm,menu_,"MLoggerControl",ah_native(vm,"MLoggerControl",nat_logger_control,m));
    static const char *const transitions[]={"MColorTransition","MCrossTransition","MBlockTransition"};
    for(int i=0;i<3;i++)ah_set_member(vm,menu_,transitions[i],ah_native(vm,transitions[i],nat_transition,(void *)transitions[i]));
    ah_obj *unit=submodule(vm,menu_,"MUnit");
    ah_set_member(vm,unit,"GetUnitOfVelocity",ah_native(vm,"GetUnitOfVelocity",nat_velocity_unit,NULL));
    ah_set_member(vm,unit,"GetVelocityForDisplay",ah_native(vm,"GetVelocityForDisplay",nat_velocity_unit,(void *)1));
    ah_set_member(vm,unit,"GetCourseLengthString",ah_native(vm,"GetCourseLengthString",nat_course_units,NULL));
    ah_set_member(vm,unit,"GetCourseHeightString",ah_native(vm,"GetCourseHeightString",nat_course_units,(void *)1));
    ah_set_member(vm,unit,"GetUnitOfPower",ah_native(vm,"GetUnitOfPower",nat_power_unit,NULL));
    ah_set_member(vm,unit,"GetPowerForDisplay",ah_native(vm,"GetPowerForDisplay",nat_power_unit,(void *)1));
    ah_set_member(vm,unit,"GetUnitOfTorque",ah_native(vm,"GetUnitOfTorque",nat_torque_unit,NULL));
    ah_set_member(vm,unit,"GetTorqueForDisplay",ah_native(vm,"GetTorqueForDisplay",nat_torque_unit,(void *)1));
    ah_set_member(vm,menu_,"MQuickWork",ah_native(vm,"MQuickWork",nat_quick_work,m));
    ah_obj *car=submodule(vm,menu_,"MCarData");
    ah_set_member(vm,car,"GetCarName",ah_native(vm,"GetCarName",nat_car_name,m));
    static const char *const methods[]={"GetCarLabel","GetTuner","GetCountry","GetOpenModelType","GetModelYear","GetCategory","GetPrice","NotForSell","GetDescriptionID"};
    for(int i=0;i<9;i++)ah_set_member(vm,car,methods[i],ah_native(vm,methods[i],nat_car_metadata,(void *)methods[i]));
    static const char *const arcade_car[]={"GetRaceCode","IsTireNormal","RaceForbidden","ShiftCarCode","IsDirtRunnable","IsTestCar"};
    for(unsigned i=0;i<sizeof arcade_car/sizeof *arcade_car;i++)ah_set_member(vm,car,arcade_car[i],ah_native(vm,arcade_car[i],nat_arcade_car,(void *)arcade_car[i]));
    ah_set_member(vm,car,"PickUpEnemyCars",ah_native(vm,"PickUpEnemyCars",nat_pick_enemies,m));
    ah_obj *garage_class=ah_native_class(vm,"MCarGarage",nat_car_garage,m);
    const char *sorts[]={"sortByRideHistory","sortByPower","sortByWeight","sortByPWR","sortByDate","sortByPrice","sortByYear","sortByCountry","sortByTuner","sortByName","sortByDriveTrain","sortByColor"};
    for(unsigned i=0;i<12;i++)ah_set_member(vm,garage_class,sorts[i],ah_native(vm,sorts[i],nat_garage_sort_history,(void *)(intptr_t)i));
    ah_set_member(vm,garage_class,"sortByAspiration",ah_native(vm,"sortByAspiration",nat_garage_sort_history,(void *)12));
    ah_set_member(vm,menu_,"MCarGarage",ah_objv(garage_class));
    {   /* MRandom::GetValue, and MRandom() whose getValue(min, max) labomode.ad's initBG draws */
        ah_obj *rnd=ah_native_class(vm,"MRandom",nat_random_new,m);
        ah_set_member(vm,menu_,"MRandom",ah_objv(rnd));
        ah_set_member(vm,rnd,"GetValue",ah_native(vm,"GetValue",nat_random_value,m));
    }
    ah_set_member(vm,menu_,"MMemoryCardPlayList",ah_objv(ah_native_class(vm,"MMemoryCardPlayList",nat_playlist_new,m)));
    /* the Arcade's race (the game object's fields, func_00128738: zero) */
    ah_set_member(vm,game,"difficulty",ah_str(vm,"EASY"));
    ah_set_member(vm,game,"enemy_level",ah_int(0));
    ah_set_member(vm,game,"race_code",ah_str(vm,""));
    ah_set_member(vm,game,"max_entry",ah_int(1));
    ah_set_member(vm,game,"overwrite_enemy_num",ah_int(0));
    ah_set_member(vm,game,"setEntryCarCode",ah_native(vm,"setEntryCarCode",nat_entry_car,(void *)1));
    ah_set_member(vm,game,"getEntryCarCode",ah_native(vm,"getEntryCarCode",nat_entry_car,NULL));
    ah_set_member(vm,option,"limit_enemies_to_favorite_car_only",ah_int(0));
    ah_set_member(vm,profile_module(vm,game,"course_record"),"getUnit",ah_native(vm,"getUnit",nat_score_unit,m));
    ah_set_member(vm,profile_module(vm,game,"machine_record"),"getMTUnit",ah_native(vm,"getMTUnit",nat_machine_unit,m));
    ah_obj *favorites=submodule(vm,game,"favorite_car");
    ah_set_member(vm,favorites,"getSize",ah_native(vm,"getSize",nat_empty_favorites,m));
    ah_set_member(vm,favorites,"has",ah_native(vm,"has",nat_empty_favorites,m));
    ah_obj *course=submodule(vm,menu_,"MCourseData");
    ah_set_member(vm,course,"GetMaxCarNum",ah_native(vm,"GetMaxCarNum",nat_course_capacity,m));
    ah_set_member(vm,course,"GetType",ah_native(vm,"GetType",nat_course_text,NULL));
    ah_set_member(vm,course,"GetAttrString",ah_native(vm,"GetAttrString",nat_course_text,(void *)1));
    static const char *const actors[] = { "MFadeActor", "MMoveActor", "MBlinkActor", "MScaleActor",
                                          "MRotateActor", "MAnchorActor", "MChaseActor" };
    for (size_t i = 0; i < sizeof actors / sizeof actors[0]; i++)
        ah_set_member(vm, menu_, actors[i], ah_native(vm, actors[i], nat_actor, (void *)actors[i]));
    ah_set_member(vm, menu_, "MScriptWatcher", ah_native(vm, "MScriptWatcher", nat_watcher, m));
    ah_set_member(vm, menu_, "MScriptEvent", ah_native(vm, "MScriptEvent", nat_event_new, (void *)1));
    ah_set_member(vm, menu_, "MFunctionEvent", ah_native(vm, "MFunctionEvent", nat_event_new, NULL));
    ah_set_member(vm,menu_,"MKeyPressEvent",ah_native(vm,"MKeyPressEvent",nat_photo_event,(void *)0));
    ah_set_member(vm,menu_,"MKeyReleaseEvent",ah_native(vm,"MKeyReleaseEvent",nat_photo_event,(void *)1));
    ah_set_member(vm,menu_,"MActivateEvent",ah_native(vm,"MActivateEvent",nat_photo_event,(void *)2));
    ah_obj *mc = ah_native_class(vm,"MMemoryCardManager",nat_pc_manager,m);
    ah_set_member(vm,menu_,"MMemoryCardManager",ah_objv(mc));
    ah_set_member(vm, mc, "getExistValidGameData", ah_native(vm, "getExistValidGameData", nat_save_status, NULL));
    {static const char *const statics[]={"setAction","setExistValidGameData","getPrintList"};
     for(unsigned i=0;i<3;i++)ah_set_member(vm,mc,statics[i],ah_native(vm,statics[i],nat_mc_static,(void *)statics[i]));}
    ah_obj *system = submodule(vm, menu_, "MSystem");
    ah_set_member(vm,system,"GetLanguage",ah_native(vm,"GetLanguage",nat_language,NULL));
    ah_set_member(vm,system,"SetLanguage",ah_native(vm,"SetLanguage",nat_language,(void *)1));
    ah_set_member(vm, system, "GetRegionCode", ah_native(vm, "GetRegionCode", nat_region, NULL));
    ah_set_member(vm, system, "GetVideoSystem", ah_native(vm, "GetVideoSystem", nat_video_system, NULL));
    ah_set_member(vm, system, "GetVersionBranch", ah_native(vm, "GetVersionBranch", nat_version_branch, NULL));
    /* 001AD97C registers 001AC248 -> 00104E30: selects the loading
       presentation by string. The PC application's loading_screen_begin
       owns that boundary; it does not create the PS2 loading thread. */
    ah_set_member(vm,system,"SetLoadingMode",ah_native(vm,"SetLoadingMode",nat_noop,m));

    m->context = ah_module_new(vm, "context", NULL, NULL, NULL);
    ah_set_member(vm,m->context,"getUpdateContext",ah_native(vm,"getUpdateContext",nat_update_context,m));
    ah_set_member(vm,m->context,"getPadButtonState",ah_native(vm,"getPadButtonState",nat_pad_buttons,m));
    ah_set_member(vm,m->context,"getPadAnalogChannel",ah_native(vm,"getPadAnalogChannel",nat_pad_analog,m));
    ah_set_member(vm,m->context,"enterEventLoop",ah_native(vm,"enterEventLoop",nat_event_loop,NULL));
    ah_set_member(vm,m->context,"leaveEventLoop",ah_native(vm,"leaveEventLoop",nat_event_loop,(void *)1));
    ah_set_member(vm,m->context,"cursor_speed_ratio",ah_float(1));
    ah_set_member(vm,m->context,"createRenderContext",ah_native(vm,"createRenderContext",nat_render_context,(void *)1));
    ah_set_member(vm,m->context,"getRenderContext",ah_native(vm,"getRenderContext",nat_render_context,NULL));
    ah_set_member(vm,m->context,"unloadGpb",ah_native(vm,"unloadGpb",nat_noop,m));
    ah_set_member(vm,m->context,"loadGpb",ah_native(vm,"loadGpb",nat_load_gpb,m));
    ah_set_member(vm,m->context,"existGpbBinary",ah_native(vm,"existGpbBinary",nat_exist_gpb,m));
    ah_set_member(vm, m->context, "translate", ah_native(vm, "translate", nat_translate, m));
    ah_set_member(vm, m->context, "pushPage", ah_native(vm, "pushPage", nat_page, (void *)1));
    ah_set_member(vm, m->context, "startPage", ah_native(vm, "startPage", nat_page, (void *)2));
    ah_set_member(vm, m->context, "closePage", ah_native(vm, "closePage", nat_page, (void *)3));
    ah_set_member(vm, m->context, "pushEvent", ah_native(vm, "pushEvent", nat_push_event, m));
    ah_set_member(vm, m->context, "finish", ah_native(vm, "finish", nat_finish, m));
    /* The context starts with a colour (black) transition: the arcade
       scripts never assign one, yet _start_page's panOutIn fades through
       black between pages in PCSX2 (menu entry, maker <-> showroom). */
    {
        ah_val transition = ah_nil();
        if (nat_transition(vm, NULL, (void *)"MColorTransition", 0, NULL, &transition) == 0)
            ah_set_member(vm, m->context, "transition", transition);
        else
            ah_set_member(vm, m->context, "transition", ah_nil());
    }
    ah_set_member(vm, m->context, "event_mask", ah_int(-1));
    ah_set_member(vm, m->context, "time_after_last_input", ah_float(0));
    ah_set_member(vm, m->context, "flushKeyEvent", ah_native(vm, "flushKeyEvent", nat_noop, m));
    /* Numeric waits keep presentation moving without reentering the VM. */
    ah_set_member(vm, m->context, "sync", ah_native(vm, "sync", nat_sync, m));
    ah_set_member(vm, m->context, "getCommonPage", ah_native(vm, "getCommonPage", nat_common_page, m));
    ah_set_member(vm, m->context, "getPrelightWidget", ah_native(vm, "getPrelightWidget", nat_prelight, m));
    ah_pin(vm, ah_objv(m->context));
}

/* ---------------------------------------------------------------- menu */

menu *menu_open(gt4_vol *vol,const char *project,char *err,size_t err_len)
{return menu_open_presented(vol,project,err,err_len,NULL,NULL);}

menu *menu_open_presented(gt4_vol *vol,const char *project,char *err,size_t err_len,void (*progress)(void *),void *user)
{
    menu *m = calloc(1, sizeof *m);
    if (!m) return NULL;
    m->vol = vol;snprintf(m->project_label,sizeof m->project_label,"%s",project);
    /* MPhotoMapWindow ctor 00192BE0 +94/+B4/+BC. */
    m->photo.view_angle=70;m->photo.aperture=7;m->photo.lightness=10;m->photo.image_scale=1;
    strcpy(m->photo.operate_mode,"none");strcpy(m->photo.color_filter,"none");
    if(!strcmp(project,"photo_shoot"))m->photo_apertures_ready=gt4_photo_apertures_load(gt4_vol_source(vol),m->photo_apertures);
    m->car_color = -1;
    m->ui = ui_project_load(vol, project, err, err_len);
    if (!m->ui) { free(m); return NULL; }
    ui_set_native_renderer(m->ui,logger_draw,m);
    int n = ui_root_count(m->ui);
    m->entered = calloc((size_t)(n ? n : 1), 1);
    m->loaded = calloc((size_t)(n ? n : 1), 1);
    m->focus = calloc((size_t)(n ? n : 1), sizeof *m->focus);
    m->vm = ah_vm_new();
    ui_set_prepare_presenter(m->ui,progress,user);
    ah_set_progress(m->vm,progress,user);
    if(progress)progress(user);
    ah_set_user(m->vm, m);
    ah_set_log(m->vm, log_line, m);
    make_engine(m);
    /* main.game.option exists before project bytecode runs on the PS2.
       quick_tune.ad module construction calls MQuickWork, whose initial
       transmission (00111B50 +1B0..1BC) must see the current preference,
       not make_engine's default followed by a late menu_enter restore. */
    if(gt4_save_active())gt4_save_restore(m->vm,game_object(m));
    else options_restore(m);

    const char *pname = mp_string(ui_project_tree(m->ui), "name");
    m->project = ah_module_new(m->vm, pname ? pname : project, ah_root(m->vm), NULL, NULL);
    ah_set_member(m->vm, ah_root(m->vm), pname ? pname : project, ah_objv(m->project));
    ah_set_member(m->vm, m->project, "load", ah_bind(m->vm,m->project,ah_native(m->vm,"load",nat_project_load,m)));
    for (int i = 0; i < n; i++) {bind_tree(m, ui_root_widget(m->ui, i));if(progress)progress(user);}

    char path[256];
    size_t size = 0;
    snprintf(path, sizeof path, "/projects/%s/%s.adc", project, project);
    uint8_t *data = gt4_vol_read(vol, path, &size);
    if (!data) {
        fprintf(stderr, "menu: no script %s, screens only\n", path);
        return m;
    }
    char e[256];
    ah_code *code = ah_load(data, size, e, sizeof e);
    free(data);
    if (!code) { fprintf(stderr, "menu: %s: %s\n", path, e); return m; }
    if(!strcmp(project,"gtmode")) {
        size_t catalog_size=0;uint8_t *catalog=gt4_vol_read(vol,"/projects/gtmode/US_carlist.adc",&catalog_size);
        if(catalog){ah_code *catalog_code=ah_load(catalog,catalog_size,e,sizeof e);free(catalog);if(catalog_code)ah_run_module(m->vm,catalog_code,m->project);else fprintf(stderr,"menu: GT Mode catalog: %s\n",e);}
    }
    if(progress)progress(user);
    if (ah_run_script(m->vm, code) < 0) fprintf(stderr, "menu: %s stopped with an error\n", path);
    {ah_val check;if(ah_get_member(m->vm,m->project,"checkGameData",&check))
        ah_set_member(m->vm,m->project,"checkGameData",ah_native(m->vm,"checkGameData",nat_pc_check_profile,m));}
    if(!strcmp(project,"photo_shoot")) {
        ah_val ctx=ah_objv(m->context),result;
        if(ah_call_member(m->vm,m->project,"onLoad",1,&ctx,&result)<0)
            fprintf(stderr,"menu: photo_shoot project onLoad failed\n");
    }
    if(!strcmp(project,"gtmode")) {
        ah_val used_page_value,used_module_value;
        ah_get_member(m->vm,m->project,"UsedCarRoot",&used_page_value);
        ah_get_member(m->vm,ah_as_obj(used_page_value,AO_MODULE),"UsedCarModule",&used_module_value);
        ah_obj *used_module=ah_as_obj(used_module_value,AO_MODULE);
        ah_module_attach_host(used_module,&project_loader_host);
        /* Native profile data is restored before authored panel construction. */
        ah_obj *game=game_object(m);
        ah_set_member(m->vm,game,"major_menu_widget",ah_str(m->vm,""));
        ah_set_member(m->vm,game,"major_menu_page",ah_str(m->vm,""));
        ah_set_member(m->vm,game,"username",ah_str(m->vm,"Player"));
        ah_set_member(m->vm,game,"money",ah_int(10000));
        ah_set_member(m->vm,game,"isNeedSave",ah_native(m->vm,"isNeedSave",nat_save_status,(void *)1));
        ah_obj *garage=submodule(m->vm,game,"garage");
        ah_set_member(m->vm,garage,"hasRidingCar",ah_native(m->vm,"hasRidingCar",nat_empty_favorites,m));
        ah_obj *calendar=submodule(m->vm,game,"calendar"),*date=ah_array_new(m->vm);
        ah_array_push(m->vm,date,ah_int(2005));ah_array_push(m->vm,date,ah_int(4));ah_array_push(m->vm,date,ah_int(2));
        ah_set_member(m->vm,calendar,"today",ah_objv(date));
        ah_set_member(m->vm,calendar,"getElapsedDate",ah_native(m->vm,"getElapsedDate",nat_empty_favorites,m));
        ah_obj *record=submodule(m->vm,game,"race_record");
        ah_set_member(m->vm,record,"isLicensePassed",ah_native(m->vm,"isLicensePassed",nat_empty_favorites,m));
        ah_val main_,main_menu,unit;
        ah_get_member(m->vm,ah_root(m->vm),"main",&main_);
        ah_get_member(m->vm,ah_as_obj(main_,AO_MODULE),"menu",&main_menu);
        ah_get_member(m->vm,ah_as_obj(main_menu,AO_MODULE),"MUnit",&unit);
        ah_set_member(m->vm,ah_as_obj(unit,AO_MODULE),"IsCalendarTypeDayFirst",ah_native(m->vm,"IsCalendarTypeDayFirst",nat_calendar_day_first,m));
        menu_save_refresh(m);
        /* Project onLoad constructs the shared Common panel from Template.
           It must run before any page's onLoad/onInitialize. */
        ah_val ctx=ah_objv(m->context),result;
        if(ah_call_member(m->vm,m->project,"onLoad",1,&ctx,&result)<0)
            fprintf(stderr,"menu: gtmode project onLoad failed\n");
    }
    if(!strcmp(project,"labomode"))album_music_bind(m);
    if(!strcmp(project,"slide")){album_music_bind(m);jukebox_bind(m);}
    if(!strcmp(project,"labomode"))ui_set_album_renderer(m->ui,album_draw,m);
    for(int i=0;i<ui_root_count(m->ui);i++)bind_save_buttons(m,ui_root_widget(m->ui,i));
    ah_set_member(m->vm,m->project,"autoSave",ah_native(m->vm,"autoSave",nat_save_auto,m));
    if(!strcmp(project,"gtmode"))ah_set_member(m->vm,m->project,"autoSaveImpl",ah_native_refs(m->vm,"autoSaveImpl",nat_pc_autosave,m,1u<<3));
    return m;
}

void menu_close(menu *m)
{
    if (!m) return;
    gt4_sound_close(m->sound);
    gt4_disc_replay_close(m->disc_replays);
    free(m->race_labels);free(m->race_db);
    free(m->car_labels);free(m->car_db);free(m->car_names);free(m->car_strings);free(m->car_links);free(m->car_variants);
    gt4_arcade_close(m->arcade);for(int i=0;i<6;i++)free(m->entry_car[i]);
    free(m->course_db);free(m->course_labels);
    ah_vm_free(m->vm);            /* before the widgets its finalizers look at */
    ui_project_free(m->ui);
    free(m->watchers);
    free(m->entered);
    free(m->loaded);
    free(m->focus);
    free(m);
}

ui_project *menu_ui(menu *m) { return m->ui; }

static int movie_finish_tree(menu *m,ui_widget *w)
{
    if(!w)return 0;
    if(!strcmp(w->type,"MovieFace") && w->script) {
        ah_val fn,ctx=ah_objv(m->context),result;
        if(ah_get_member(m->vm,w->script,"onMovieFinish",&fn)) {
            ah_call_member(m->vm,w->script,"onMovieFinish",1,&ctx,&result);
            apply_pages(m);return 1;
        }
    }
    for(int i=0;i<w->nkids;i++)if(movie_finish_tree(m,w->kids[i]))return 1;
    return 0;
}

int menu_movie_finished(menu *m)
{
    if(!m || menu_active_root(m)<0)return 0;
    return movie_finish_tree(m,ui_root_widget(m->ui,menu_active_root(m)));
}

/* The widgets' own onLoad, below the page's: gtmode.ad's TuningRoot::Pane
   defines its categories' parts_types there (L7132..7158) and only its
   onInitialize is called by the script (L6959), so the engine runs the
   loads. Pre-order, a widget before its children. */
static void load_widgets(menu *m,ui_widget *w,ah_val context)
{
    for(int i=0;i<w->nkids;i++){
        ui_widget *k=w->kids[i];ah_val fn,result;
        if(k->script && ah_get_own_member(m->vm,k->script,"onLoad",&fn) && fn.t!=AV_NIL &&
           ah_call_member(m->vm,k->script,"onLoad",1,&context,&result)<0)
            fprintf(stderr,"menu: %s.onLoad stopped with an error\n",k->name?k->name:"?");
        load_widgets(m,k,context);
    }
}
static void load_page_data(menu *m,int root,ah_val context)
{
    if(!m->loaded || m->loaded[root])return;
    m->loaded[root]=1;ui_widget *page=ui_root_widget(m->ui,root);ah_val result;
    ah_obj *module=widget_module(m,page);
    if(ah_call_member(m->vm,module,"onLoadInitialize",1,&context,&result)<0)
        fprintf(stderr,"menu: %s.onLoadInitialize stopped with an error\n",page->name);
    if(ah_call_member(m->vm,module,"onLoad",1,&context,&result)<0)
        fprintf(stderr,"menu: %s.onLoad stopped with an error\n",page->name);
    load_widgets(m,page,context);
}

void menu_enter(menu *m, int root)
{
    if (root < 0 || root >= ui_root_count(m->ui)) return;
    if(!gt4_save_active())options_restore(m);
    if(!m->page_transition_depth && strcmp(ah_module_name(m->project),"SettingProject"))menu_save_refresh(m);
    /* 00180AA8 +0x78..+0xA4: refresh the normalized getter after restoring
       saved raw preferences; quick-arcade.ad reads it during onInitialize. */
    ah_val option;
    if(ah_get_member(m->vm,game_object(m),"option",&option))
        ah_set_member(m->vm,option.u.o,"menu_bgm_volume",ah_float((float)menu_option_number(m,"menu_bgm_volume_raw",120)/128.f));
    /* An explicit first screen (including the viewer's debug page argument)
       supersedes the project's queued initial startPage. */
    if(!m->depth)m->pending_kind=0;
    if (!m->depth || menu_active_root(m) != root) {
        while (m->depth) finalize_page(m, m->pages[--m->depth]);
        m->pages[0] = root; m->depth = 1;
    }
    m->idle_frames=0;
    /* Options has no page onStart panIn. Initial and cached project entries
       still need the same reveal as its authored start_page callbacks. */
    if(!strcmp(ah_module_name(m->project),"OptionProject") &&
       m->pending_fade_in==0 && ui_fade_remaining(m->ui)==0) {
        ah_val transition,period;
        if(ah_get_member(m->vm,m->context,"transition",&transition) &&
           ah_get_member(m->vm,ah_as_obj(transition,AO_MODULE),"period",&period) &&
           ah_num(period)>0) {
            ui_fade(m->ui,1,1,0);
            m->pending_fade_in=(float)ah_num(period);
        }
    }
    /* A fresh opening resets MovieRoot's authored end flag even on a cached VM. */
    if(!strcmp(ui_root_name(m->ui,root),"MovieRoot"))m->entered[root]=0;
    request_music(m,mp_string(ui_root_widget(m->ui,root)->src,"bgm"));
    if (m->entered[root]) {
        ah_val ctx=ah_objv(m->context), result;
        ah_call_member(m->vm,widget_module(m,ui_root_widget(m->ui,root)),"enter",1,&ctx,&result);
        return;
    }
    m->entered[root] = 1;
    m->initializing++;
    ui_widget *w = ui_root_widget(m->ui, root);
    ah_val ctx = ah_objv(m->context), r;
    /* onLoad(context) prepares a page's data once (MakerRoot: nation flags
       and car counts), before its first onInitialize. */
    /* Both authored tab totals are read by either selection page. Prepare
       their data together without entering or focusing the other page. */
    if(w->name && (!strcmp(w->name,"MakerRoot") || !strcmp(w->name,"HistoryRoot")))
        for(int i=0;i<ui_root_count(m->ui);i++){
            const char *name=ui_root_name(m->ui,i);
            if(!strcmp(name,"MakerRoot") || !strcmp(name,"HistoryRoot"))load_page_data(m,i,ctx);
        }
    request_music(m,mp_string(w->src,"bgm"));
    /* labomode's dialogs are opened by their open() (OptionReplayDialog,
       OptionAlbumDialog), which moves the Pane's actor before pushPage:
       their onLoad (setMoveActor) has run by then. A: when the game runs
       them is not read; with the first page here. */
    if(!strcmp(ah_module_name(m->project),"LaboModeProject"))
        for(int i=0;i<ui_root_count(m->ui);i++){
            const char *name=ui_root_name(m->ui,i);size_t n=strlen(name);
            if(i!=root && n>6 && !strcmp(name+n-6,"Dialog"))load_page_data(m,i,ctx);
        }
    /* gtmode.ad GetCourseDialog.onLoad L15577..15580 creates the actors
       used by open_motion L15585..15592, before pushPage at L15593.
       Prepare this resource before TopRoot can deliver a dated course.
       A: original project-wide loading phase is not yet reproduced. */
    if(!strcmp(ah_module_name(m->project),"GTmodeProject"))
        for(int i=0;i<ui_root_count(m->ui);i++)
            if(!strcmp(ui_root_name(m->ui,i),"GetCourseDialog"))load_page_data(m,i,ctx);
    load_page_data(m,root,ctx);
    if (ah_call_member(m->vm, widget_module(m, w), "onInitialize", 1, &ctx, &r) < 0)
        fprintf(stderr, "menu: %s.onInitialize stopped with an error\n", w->name ? w->name : "?");
    pc_menu_entries(m,w);
    ah_call_member(m->vm,widget_module(m,w),"enter",1,&ctx,&r);
    m->initializing--;
    /* Legacy onEnter runs after page initialization so notices created after
       setFocus are available. Modern onFocusEnter retains its existing timing. */
    if(m->focus[root]) {
        ah_obj *focus=widget_module(m,m->focus[root]);ah_val method;
        if(!ah_get_member(m->vm,focus,"onFocusEnter",&method))
            ah_call_member(m->vm,focus,"onEnter",1,&ctx,&r);
    }
}

void menu_advance(menu *m, double seconds)
{
    if (!m || !isfinite(seconds) || seconds<=0) return;
    if (seconds>0.25) seconds=0.25;
    menu_animate(m,(float)seconds);
    m->tick_remainder+=seconds;
    while(m->tick_remainder+1e-10>=1.0/60) {
        m->tick_remainder-=1.0/60;
        menu_tick(m);
    }
}

/* a list's script callbacks (labomode.ad): visible_callback(context,
   indices) for the lists filled since the last tick, then
   update_callback(context, indices) for items still asking */
static void item_callbacks(menu *m)
{
    for(int i=0;i<m->nitem_lists;i++){
        ui_widget *w=m->item_lists[i];ah_val fn,r,args[2];
        if(!ah_get_member(m->vm,widget_module(m,w),"visible_callback",&fn) || fn.t==AV_NIL)continue;
        ah_obj *index=ah_array_new(m->vm);for(int k=0;k<w->nkids;k++)ah_array_push(m->vm,index,ah_int(k));
        args[0]=ah_objv(m->context);args[1]=ah_objv(index);ah_call(m->vm,fn,2,args,&r);
    }
    m->nitem_lists=0;
    for(int pass=0;pass<1 && m->nitem_updates;pass++){
        ui_widget *list=m->item_updates[0].list;ah_val fn,r,args[2];
        ah_obj *index=ah_array_new(m->vm);
        for(int k=0;k<m->nitem_updates;k++)if(m->item_updates[k].list==list)ah_array_push(m->vm,index,ah_int(m->item_updates[k].index));
        if(!ah_get_member(m->vm,widget_module(m,list),"update_callback",&fn) || fn.t==AV_NIL){m->nitem_updates=0;break;}
        args[0]=ah_objv(m->context);args[1]=ah_objv(index);ah_call(m->vm,fn,2,args,&r);
    }
}

void menu_tick(menu *m)
{
    if(m)album_tick(m);
    logger_tick(m);
    ah_set_member(m->vm, m->context, "time_after_last_input", ah_float(++m->idle_frames / 60.0));
    item_callbacks(m);
    for (int i = 0; i < m->nwatchers; i++) {
        watcher *wt = &m->watchers[i];
        ah_val interval, fn, ctx, r;
        ah_get_member(m->vm, wt->obj, "interval", &interval);
        int every = (int)ah_num(interval);
        if (++wt->count < (every > 0 ? every : 1)) continue;
        wt->count = 0;
        ah_get_member(m->vm, wt->obj, "func", &fn);
        ah_get_member(m->vm, wt->obj, "context", &ctx);
        ah_call(m->vm, fn, 1, &ctx, &r);
    }
    /* Bound work per frame; callbacks may append more events. Keep the
       current event pinned until the call returns, including failures. */
    for (int budget = 0; m->nevents && budget < 64; budget++) {
        ah_obj *e = m->events[0];
        memmove(m->events, m->events + 1, (size_t)--m->nevents * sizeof *m->events);
        ah_val av, fn, target, method, result;
        ah_get_member(m->vm, e, "arguments", &av);
        ah_obj *a = ah_as_obj(av, AO_ARRAY);
        int n = ah_array_size(a);
        ah_val *args = calloc((size_t)(n ? n : 1), sizeof *args);
        if (args) {
            for (int i = 0; i < n; i++) args[i] = ah_array_get(a, i);
            if (ah_get_member(m->vm, e, "function", &fn)) ah_call(m->vm, fn, n, args, &result);
            else if (ah_get_member(m->vm, e, "target", &target) && ah_get_member(m->vm, e, "method", &method) && ah_cstr(method))
                ah_call_member(m->vm, ah_as_obj(target, AO_MODULE), ah_cstr(method), n, args, &result);
            free(args);
        }
        ah_unpin(m->vm, ah_objv(e));
        apply_pages(m);
    }
    apply_pages(m);
    ah_gc(m->vm);
}

int menu_active_root(const menu *m) { return m && m->depth ? m->pages[m->depth - 1] : -1; }
int menu_page_depth(const menu *m) { return m ? m->depth : 0; }
int menu_page_at(const menu *m, int i) { return m && i >= 0 && i < m->depth ? m->pages[i] : -1; }
const char *menu_focus_name(const menu *m)
{
    int root = menu_active_root(m);
    ui_widget *w = root >= 0 ? m->focus[root] : NULL;
    return w && w->name ? w->name : "";
}

static int event_call(menu *m, ui_widget *handler, ui_widget *target, const char *method, int key, int argc, ah_val *result)
{
    ah_obj *e = ah_module_new(m->vm, "event", NULL, NULL, NULL);
    ah_set_member(m->vm, e, "widget", maybe_module(m, target));
    ah_set_member(m->vm, e, "keysym", ah_int(key));
    /* the first controller's (option.ad's secret command takes port 1 only) */
    ah_set_member(m->vm, e, "port", ah_int(0));
    ah_val args[3] = {ah_objv(m->context), ah_objv(e), maybe_module(m, target)};
    return ah_call_member(m->vm, widget_module(m, handler), method, argc, args, result);
}

static int available(ui_widget *w)
{
    for (ui_widget *p = w; p; p = p->parent) if (!p->visible || p->insensitive) return 0;
    return w != NULL;
}

/* The page's help line follows focus through its TOOLTIP|<name> text;
   onFocusEnter may replace it (tip.value) right after. */
static void set_tooltip(menu *m, int root, const ui_widget *w)
{
    char key[160];const char *authored=w?mp_string(w->src,"tooltip_key"):NULL;
    /* /projects/quick-arcade/quick-arcade.mproject Buttons.*.tooltip_key names
       /projects/quick-arcade/US/rtext.rt2's TOOLTIP|icon_text_* entries. */
    snprintf(key, sizeof key, "TOOLTIP|%s", authored?authored:w && w->name?w->name:"");
    const char *text = ui_translate(m->ui, root, NULL, key);
    if (lan_blocked(m, (ui_widget *)w)) {
        /* P: the blocked LAN entries say so: their title, then "Coming soon" */
        char soon[160];
        const char *colon = text ? strchr(text, ':') : NULL;
        if (colon) snprintf(soon, sizeof soon, "%.*s: Coming soon", (int)(colon - text), text);
        else snprintf(soon, sizeof soon, "Coming soon");
        ui_set_tooltips(ui_root_widget(m->ui, root), soon);
        return;
    }
    ui_set_tooltips(ui_root_widget(m->ui, root), text);
}

/* Scrolled lists ("clip" > "space", e.g. MakerRoot's logo map). Native
   approximation of the PS2 behaviour reported by the user: the view moves
   only when the focused item comes within one item of the edge, then eases
   there (ui_animate) at the cursor's rate. The original MScrollWindow code
   has not been traced yet. snap: jump at once (ScrollWindow.warp). */
static void scroll_to(ui_widget *w, int snap)
{
    for (ui_widget *space = w->parent; space && space->parent; space = space->parent) {
        ui_widget *clip = space->parent;
        if (!space->name || !clip->name || strcmp(space->name, "space") || strcmp(clip->name, "clip")) continue;
        float x = w->x, y = w->y;
        for (ui_widget *p = w->parent; p && p != space; p = p->parent) { x += p->x; y += p->y; }
        float tx = space->scroll_set ? space->scroll_tx : space->x, ty = space->scroll_set ? space->scroll_ty : space->y;
        /* one row/column of margin: the spacing to the nearest neighbour */
        float px = w->w, py = w->h;
        for (int i = 0; w->parent && i < w->parent->nkids; i++) {
            const ui_widget *o = w->parent->kids[i];
            if (o == w || !o->visible) continue;
            float dx = fabsf((o->x + o->w * .5f) - (w->x + w->w * .5f)), dy = fabsf((o->y + o->h * .5f) - (w->y + w->h * .5f));
            if (dy < 4 && dx > 4 && (px == w->w || dx < px)) px = dx;
            if (dx < 4 && dy > 4 && (py == w->h || dy < py)) py = dy;
        }
        float mx = fminf(px, fmaxf(0, (clip->w - w->w) * .5f)), my = fminf(py, fmaxf(0, (clip->h - w->h) * .5f));
        if (x + tx < mx) tx = mx - x;
        if (x + tx + w->w > clip->w - mx) tx = clip->w - mx - x - w->w;
        if (y + ty < my) ty = my - y;
        if (y + ty + w->h > clip->h - my) ty = clip->h - my - y - w->h;
        /* The GT map keeps the selected 3D landmark at the view centre.
           List/card scroll windows retain their visibility-margin behavior. */
        ui_widget *model=ui_widget_child(w,"bg");
        if(model && !strcmp(model->type,"ModelFace")) {
            tx=(clip->w-w->w)*.5f-x;ty=(clip->h-w->h)*.5f-y;
        }
        /* never scroll past the content */
        tx = space->w > clip->w ? fminf(0, fmaxf(clip->w - space->w, tx)) : 0;
        ty = space->h > clip->h ? fminf(0, fmaxf(clip->h - space->h, ty)) : 0;
        space->scroll_tx = tx; space->scroll_ty = ty; space->scroll_set = 1;
        if (snap) { space->x = tx; space->y = ty; }
    }
}

static void set_focus(menu *m, ui_widget *w)
{
    int root = menu_active_root(m);
    /* arcade.ad SplitRoot.onInitialize calls setFocus(RaceStart) before
       assigning RaceStart.visible = decided1p && decided2p. Retain the
       authored focus during initialization; ensure_focus validates it after. */
    if (root < 0 || (w && (w->root != root || (!m->initializing && !available(w))))) return;
    ui_cursor_select(m->ui,w);
    ui_widget *old = m->focus[root];
    /* The native modal-close adapter holds the outgoing cursor only while
       it owns focus. A new authored setFocus releases that old hold; it
       must not strand the arrow when SplitRoot switches P1/P2 Select.
       A: adapter state, not a new original cursor timing constant. */
    if(old!=w)ui_cursor_hold(m->ui,0);
    if (old == w) {
        /* A page may store its focus before activation, or retain it while
           covered by a modal page. Restore the visual state without events. */
        if (w) w->focused = 1;
        /* Refocusing (e.g. after tip.remove() in onInitialize) restores the
           widget's help line without repeating focus events. */
        /* Only a translated help line is restored; a script-made one
           ("Hyundai  2 cars") stays. */
        char key[160];
        const char *authored=w?mp_string(w->src,"tooltip_key"):NULL;
        snprintf(key, sizeof key, "TOOLTIP|%s", authored?authored:w && w->name?w->name:"");
        const char *tip = w ? ui_translate(m->ui, root, NULL, key) : NULL;
        if (tip) ui_set_tooltips(ui_root_widget(m->ui, root), tip);
        return;
    }
    ah_val result;
    m->focus[root] = w;
    if (old) {
        old->option_open=0;
        old->focused = 0;
        if(!event_call(m,old,old,"onFocusLeave",0,2,&result))event_call(m,old,old,"onLeave",0,1,&result);
    }
    if (w) {
        w->focused = 1;
        scroll_to(w, 0);
        if (w->parent && !strcmp(w->parent->type, "SelectBox")) w->parent->focus_index = sibling_index(w);
        set_tooltip(m, root, w);
        if(!event_call(m,w,w,"onFocusEnter",0,2,&result) && !m->initializing)event_call(m,w,w,"onEnter",0,1,&result);
    }
}

/* Candidates come from widget semantics, never from hard-coded menu labels. */
static void candidates(ui_widget *w, ui_widget **items, int *n)
{
    if (!available(w) || *n >= 512) return;
    if (!strcmp(w->type, "SelectBox")) {
        for (int i = 0; i < w->nkids && *n < 512; i++)
            if (available(w->kids[i])) items[(*n)++] = w->kids[i];
        return;
    }
    if (!strcmp(w->type, "Button") || (!strcmp(w->type,"OptionMenu") && ui_option_count(w)) || !strcmp(w->type,"SliderBar") || !strcmp(w->type,"ScaleBar") || mp_number(w->src, "can_focus", 0)) {
        items[(*n)++] = w; return;
    }
    for (int i = 0; i < w->nkids; i++) candidates(w->kids[i], items, n);
}

static void ensure_focus(menu *m)
{
    int root = menu_active_root(m);
    if (root < 0) return;
    ui_widget *items[512]; int n = 0;
    /* the page itself holds the focus only while nothing on it can: a list
       filled later (memcard.ad's FileList) takes it */
    if (available(m->focus[root]) && m->focus[root] == ui_root_widget(m->ui, root))
        candidates(ui_root_widget(m->ui, root), items, &n);
    if (available(m->focus[root]) && !n) {
        if (!m->focus[root]->focused) set_focus(m, m->focus[root]);
        return;
    }
    if (!n) candidates(ui_root_widget(m->ui, root), items, &n);
    ui_widget *pick = n ? items[0] : ui_root_widget(m->ui, root);
    if (pick->parent && !strcmp(pick->parent->type, "SelectBox")) {
        ui_widget *pa = pick->parent;
        if (pa->focus_index >= 0 && pa->focus_index < pa->nkids && available(pa->kids[pa->focus_index])) pick = pa->kids[pa->focus_index];
    }
    set_focus(m, pick);
}

static void center(const ui_widget *w, float *x, float *y)
{
    *x = w->w * .5f; *y = w->h * .5f;
    for (; w; w = w->parent) { *x += w->x; *y += w->y; }
}

/* Authored navigation regions are local left/top/right/bottom bounds, not
   geometry x/y/width/height. GT Mode uses these to form a logical map grid. */
static void navigation_region(const ui_widget *w,const char *key,float r[4])
{
    double bounds[4]={0,0,w->w,w->h};
    mp_numbers(w->src,key,bounds,4);
    float x=0,y=0;
    for(const ui_widget *p=w;p;p=p->parent){x+=p->x;y+=p->y;}
    r[0]=x+(float)bounds[0];r[1]=y+(float)bounds[1];
    r[2]=x+(float)bounds[2];r[3]=y+(float)bounds[3];
}

static float navigation_score(const ui_widget *from,const ui_widget *to,menu_input input)
{
    int vertical=input==MENU_UP || input==MENU_DOWN;
    int step=input==MENU_UP || input==MENU_LEFT?-1:1;
    double authored[4];
    if(mp_numbers(from->src,"navigate_source",authored,4)==4) {
        float s[4],t[4];navigation_region(from,"navigate_source",s);navigation_region(to,"navigate_target",t);
        int a=vertical?1:0,b=1-a;
        float along=((t[a]+t[a+2])-(s[a]+s[a+2]))*.5f*step;
        if(along<=1)return 1e30f;
        float gap=fmaxf(0,fmaxf(t[b]-s[b+2],s[b]-t[b+2]));
        float forward=fmaxf(0,step>0?t[a]-s[a+2]:s[a]-t[a+2]);
        /* Authored regions define navigation lanes. A target outside the
           perpendicular lane cannot receive focus, even at a map edge. */
        if(gap>0)return 1e30f;
        return forward+.001f*along;
    }
    float x,y,nx,ny;center(from,&x,&y);center(to,&nx,&ny);
    float along=(vertical?ny-y:nx-x)*step;
    return along>1?along+3*fabsf(vertical?nx-x:ny-y):1e30f;
}

static void finalize_page(menu *m, int root)
{
    ah_val ctx = ah_objv(m->context), result;
    ah_call_member(m->vm, widget_module(m, ui_root_widget(m->ui, root)), "onFinalize", 1, &ctx, &result);
    m->entered[root] = 0;
    if (m->focus[root]) m->focus[root]->focused = 0;
    m->focus[root] = NULL;
}

static void apply_pages(menu *m)
{
    /* Authored push/pop/startPage (0022F6A0) keep the live main.game.
       Restoring the separate native save VM here invalidates references
       held by callbacks such as BuyCarRoot L6246..6278 across its modal. */
    m->page_transition_depth++;
    /* Original apply scripts mark dirty before assigning the new values.
       Capture after the callback, before another page restores preferences. */
    album_music_store(m);
    if(m->options_dirty){options_capture(m);gt4_save_capture(m->vm,game_object(m),1);m->options_dirty=0;
        if(gt4_save_active() && gt4_save_auto_enabled() && !gt4_save_store())ah_logf(m->vm,"Auto Save: %s",gt4_save_error());}
    for (int budget = 0; m->pending_kind && budget < 16; budget++) {
        int kind = m->pending_kind, next = m->pending_page;
        m->pending_kind = 0;
        int old = menu_active_root(m);
        if (kind == 3) {
            if (old != next || m->depth <= 1) continue;
            finalize_page(m, old);
            m->depth--;
            ui_cursor_hold(m->ui,0);
            menu_enter(m,menu_active_root(m));
        } else if (kind == 2 && m->pending_mores) {
            /* 0022F6A0 finalizes every page of the stack (vt+0x2D8 on its
               copy), then each page is pushed */
            int more[8], mores = m->pending_mores;
            memcpy(more, m->pending_more, sizeof more);
            m->pending_mores = 0;
            while (m->depth) finalize_page(m, m->pages[--m->depth]);
            m->pages[m->depth++] = next;
            menu_enter(m, next);
            for (int i = 0; i < mores && m->depth < 32; i++) {
                m->pages[m->depth++] = more[i];
                menu_enter(m, more[i]);
            }
        } else {
            /* 0022F6A0 +0x104..0x184 finalizes the old pages before
               pushing the requested page, even if it is the same one.
               quick-championship.ad L1044..1045 reopens PrizeRoot to
               initialize the separate championship bonus payment. */
            if ((next == old && kind != 2) || m->depth >= 32) continue;
            if (kind == 2 && m->depth) { finalize_page(m, old); m->depth--; }
            m->pages[m->depth++] = next;
            menu_enter(m, next);
        }
    }
    ensure_focus(m);
    context_cursor_visibility(m);
    /* Reveal again even when a script requests the current page or a later
       page callback fails: panOutIn must not strand the UI behind black. */
    if(m->pending_fade_in<0){ui_transition_reveal(m->ui);m->pending_fade_in=0;}
    else if(m->pending_fade_in>0){ui_fade(m->ui,1,0,m->pending_fade_in);m->pending_fade_in=0;}
    m->page_transition_depth--;
}

/* The page's focus-move sound: RootWindow `navigate_sound`, default "cursor"
   (CORE 0x00232E00; played at 0x002359B0 and by ListBox at 0x002DAB50). */
static ah_val navigate_sound(menu *m, int root)
{
    const ui_widget *page=ui_root_widget(m->ui,root);
    const char *name=page?mp_string(page->src,"navigate_sound"):NULL;
    return ah_str(m->vm,name && *name?name:"cursor");
}

/* A PS2 button the engine hands the scripts as a key of its own
   (00252404: the D_006192E4 keysyms), pressed or let go: onKeyPress /
   onKeyRelease from the focused widget up its parents to the page, then
   the project's (memcard.ad's MemcardProject), until one answers true
   (the scripts return 2 when they take it). event.widget stays the
   focused one. */
/* MRootWindow::getPrelightWidget: the widget the cursor lights, the
   focused one here. gtmode.ad TopRoot.onKeyPress L1347..1354 activates it
   on Return and keeps the key (2) whether or not there is one. */
static int nat_prelight(ah_vm *vm, ah_obj *self, void *d, int argc, const ah_val *argv, ah_val *ret)
{
    menu *m = d;(void)vm;(void)self;(void)argc;(void)argv;
    int root = menu_active_root(m);
    ui_widget *w = root >= 0 ? m->focus[root] : NULL;
    *ret = w ? maybe_module(m, w) : ah_nil();
    return 0;
}

static int key_dispatch(menu *m, int keysym, int release)
{
    if (!m || menu_active_root(m) < 0 || !keysym) return 0;
    m->idle_frames = 0;
    ensure_focus(m);
    int root = menu_active_root(m);
    ui_widget *w = m->focus[root] ? m->focus[root] : ui_root_widget(m->ui, root);
    if (logger_key(m, w, keysym, release)) return 1;
    const char *method = release ? "onKeyRelease" : "onKeyPress";
    for (ui_widget *h = w; h; h = h->parent) {
        ah_val result;
        int called = event_call(m, h, w, method, keysym, 2, &result);
        if (called < 0 || (called > 0 && ah_truthy(result))) return 1;
    }
    {
        ah_obj *e = ah_module_new(m->vm, "event", NULL, NULL, NULL);
        ah_set_member(m->vm, e, "widget", maybe_module(m, w));
        ah_set_member(m->vm, e, "keysym", ah_int(keysym));
        ah_set_member(m->vm, e, "port", ah_int(0));
        ah_val args[2] = {ah_objv(m->context), ah_objv(e)}, result;
        int called = ah_call_member(m->vm, m->project, method, 2, args, &result);
        return called < 0 || (called > 0 && ah_truthy(result));
    }
}
void menu_send_key(menu *m, int keysym, int release)
{
    if (!m || menu_active_root(m) < 0 || !keysym) return;
    /* P: US StatusRoot help is "%Ba Transfer game data" (Start), but
       gtmode.ad L14926..14934 listens to 0xFF8D (Triangle), whereas
       D_006192D8 +0x50..54 maps Start to 0xFF13. Honour that visible hint
       here as an alias; retain the authored Triangle route and all other
       pages' distinct Start event. Releases use the same alias. */
    if(keysym==0xff13 && !strcmp(ah_module_name(m->project),"GTmodeProject") &&
       !strcmp(ui_root_name(m->ui,menu_active_root(m)),"StatusRoot"))keysym=0xff8d;
    key_dispatch(m, keysym, release);
    apply_pages(m);
}

void menu_send_input(menu *m, menu_input input)
{
    if(attract_left && m && !strcmp(ui_root_name(m->ui,menu_active_root(m)),"MovieRoot"))attract_cancelled=1;
    if (!m || menu_active_root(m) < 0) return;
    /* Circle/Square keep their own PS2 script keys before the default action. */
    menu_input physical=input;
    if(input==MENU_CIRCLE)input=MENU_ACCEPT;
    if(input==MENU_SQUARE || input==MENU_TRIANGLE)input=MENU_BACK;
    if(!strcmp(ah_module_name(m->project),"PhotoShootProject") || !strcmp(m->project_label,"photo_save")) {
        const char *page=ui_root_name(m->ui,menu_active_root(m));
        /* P: PC storage boundary. PreviewRoot Enter exports a PNG rather
           than entering the original Memory Card / USB SaveDialog. */
        if(input==MENU_ACCEPT && page && !strcmp(page,"PreviewRoot")) {
            if(m->photo.image_width)m->photo.save_requested=1;
            return;
        }
    }
    m->idle_frames = 0;
    ensure_focus(m);
    int root = menu_active_root(m);
    ui_widget *w = m->focus[root];
    if (!w && (input==MENU_ACCEPT || input==MENU_BACK))
        w=ui_root_widget(m->ui,root);
    if (!w) return;
    /* P: these icons can receive focus/tooltips, but cannot finish the menu,
       stop its music, save data or launch an unimplemented mode. */
    if(input==MENU_ACCEPT && quick_pending_icon(m,w))return;
    if(input==MENU_ACCEPT && lan_blocked(m,w)){menu_play_effect(m,"disable");return;}
    /* 00252404: every pad key reaches the scripts first (onKeyPress from
       the focused widget up to the page and the project; menu_send_key),
       the engine's own move or activation only when none takes it:
       labomode.ad onListKeyPress keeps the arrows, photo_shoot.ad's
       ShootRoot grabs a tool on Return (and lets go on its release),
       SlideRoot takes Return with no onActivate, HomeRoot / setting.ad
       their Triangle. Its release comes with the button's (main.c). */
    {
        static const int keysym[]={[MENU_UP]=65362,[MENU_DOWN]=65364,[MENU_LEFT]=65361,[MENU_RIGHT]=65363,
                                   [MENU_ACCEPT]=65293,[MENU_BACK]=65307,[MENU_TRIANGLE]=65421,
                                   [MENU_CIRCLE]=65307,[MENU_SQUARE]=65379};
        if(physical>=0 && physical<=MENU_SQUARE && keysym[physical] && key_dispatch(m,keysym[physical],0)){apply_pages(m);return;}
    }
    if(!strcmp(w->type,"ScaleBar") && !w->slider_range_set && (w->option_open || input==MENU_ACCEPT)) {
        /* P: keyboard adapter invokes the original onValueChanged; floating
           step/min/max/value are supplied by photo_shoot.ad Reset methods. */
        if(input==MENU_ACCEPT){w->option_open=!w->option_open;return;}
        if(input==MENU_BACK){w->option_open=0;return;}
        float step=w->photo_step; if(!(step>0))step=1; /* A: pad repetition pending. */
        w->photo_value=fminf(w->photo_max,fmaxf(w->photo_min,w->photo_value+(input==MENU_LEFT || input==MENU_DOWN?-step:step)));
        ah_val ctx=ah_objv(m->context),result;
        ah_call_member(m->vm,widget_module(m,w),"onValueChanged",1,&ctx,&result);
    } else if((!strcmp(w->type,"SliderBar") || !strcmp(w->type,"ScaleBar") && w->slider_range_set) && (w->option_open || input==MENU_ACCEPT || input==MENU_LEFT || input==MENU_RIGHT)) {
        if(input==MENU_ACCEPT){if(!w->option_open)w->option_pending=w->focus_index;w->option_open=!w->option_open;menu_play_effect(m,"select");return;}
        int old=w->focus_index;
        if(input==MENU_BACK){assign_index(w,w->option_pending);w->option_open=0;menu_play_effect(m,"cancel");}
        else assign_index(w,old+(input==MENU_LEFT || input==MENU_DOWN?-1:1));
        if(old!=w->focus_index){ah_val args[2]={ah_objv(m->context),ah_int(w->focus_index)},result;
            ah_call_member(m->vm,widget_module(m,w),"onValueChanged",2,args,&result);menu_play_effect(m,"param");}
    } else if(!strcmp(w->type,"ListBox") && (int)mp_number(w->src,"direction",0)==1 &&
              (input==MENU_UP || input==MENU_DOWN) && w->nkids>1) {
        /* a vertical ListBox (labomode.ad's lists): up and down move along
           it, stopping at its ends; left and right leave it by geometry */
        int next=w->focus_index+(input==MENU_UP?-1:1);
        if(next>=0 && next<w->nkids) {
            w->focus_index=next;
            ah_val args[2]={ah_objv(m->context),ah_int(next)},result;
            ah_call_member(m->vm,widget_module(m,w),"onValueChanged",2,args,&result);
            ah_val sound=navigate_sound(m,root);nat_sound_play(m->vm,NULL,m,1,&sound,&result);
        }
    } else if(!strcmp(w->type,"ListBox") && (int)mp_number(w->src,"direction",0)!=1 && input<=MENU_RIGHT && w->nkids) {
        int step=input==MENU_UP || input==MENU_LEFT?-1:1;
        w->focus_index=(w->focus_index+step+w->nkids)%w->nkids;
        ah_val args[2]={ah_objv(m->context),ah_int(w->focus_index)},result;
        ah_call_member(m->vm,widget_module(m,w),"onValueChanged",2,args,&result);
        ah_val sound=navigate_sound(m,root);nat_sound_play(m->vm,NULL,m,1,&sound,&result);
    } else if(!strcmp(w->type,"OptionMenu") && (w->option_open || input==MENU_ACCEPT)) {
        int count=ui_option_count(w),step=input==MENU_LEFT || input==MENU_UP?-1:1;
        if(input==MENU_BACK){w->option_open=0;menu_play_effect(m,"cancel");return;}
        if(input==MENU_ACCEPT) {
            if(!w->option_open){w->option_pending=w->focus_index;w->option_open=1;menu_play_effect(m,"select");return;}
            w->option_open=0;menu_play_effect(m,"select");
            if(w->option_pending!=w->focus_index) {
                w->focus_index=w->option_pending;
                ah_val args[2]={ah_objv(m->context),ah_int(w->focus_index)},result;
                ah_call_member(m->vm,widget_module(m,w),"onValueChanged",2,args,&result);
                apply_pages(m);
            }
            return;
        }
        int old=w->option_open?w->option_pending:w->focus_index;
        for(int offset=1;offset<=count;offset++) {
            int next=((old+step*offset)%count+count)%count;
            ui_widget *item=ui_option_item(w,next);
            if(!available(item))continue;
            if(next!=old) {
                if(w->option_open){w->option_pending=next;menu_play_effect(m,"cursor");return;}
                w->focus_index=next;
                ah_val args[2]={ah_objv(m->context),ah_int(next)},result;
                ah_call_member(m->vm,widget_module(m,w),"onValueChanged",2,args,&result);
                ah_val sound=ah_str(m->vm,"param");
                nat_sound_play(m->vm,NULL,m,1,&sound,&result);
            }
            break;
        }
    } else if (input <= MENU_RIGHT) {
        ui_widget *items[512]; int n = 0, index = -1;
        candidates(w->parent && !strcmp(w->parent->type,"SelectBox") ? w->parent : ui_root_widget(m->ui, root), items, &n);
        for (int i = 0; i < n; i++) if (items[i] == w) index = i;
        if (n) {
            int step = input == MENU_UP || input == MENU_LEFT ? -1 : 1;
            int next = index < 0 ? 0 : (index + step + n) % n;
            /* SelectBox stops at its ends (CORE 0x002D7468/0x002D7528). */
            if (index >= 0 && w->parent && !strcmp(w->parent->type, "SelectBox") &&
                (index + step < 0 || index + step >= n)) next = index;
            if (index >= 0 && (!w->parent || strcmp(w->parent->type, "SelectBox"))) {
                /* Directional focus for grids; SelectBox keeps its ordered list. */
                float best = 1e30f;
                next = index;
                for (int i = 0; i < n; i++) if (i != index) {
                    float score = navigation_score(w,items[i],input);
                    if (score < best) { best = score; next = i; }
                }
            }
            set_focus(m, items[next]);
            if (next != index) {
                ah_val sound = navigate_sound(m,root), result;
                nat_sound_play(m->vm,NULL,m,1,&sound,&result);
            }
            ui_widget *pa = items[next]->parent;
            if (pa && !strcmp(pa->type, "SelectBox") && next != index) {
                ah_val args[2] = {ah_objv(m->context), ah_int(pa->focus_index)}, result;
                ah_call_member(m->vm, widget_module(m, pa), "onValueChanged", 2, args, &result);
            }
        }
    } else {
        /* Keep event.widget at the original target while bubbling to parents.
           The scripts use return 2 to consume an activation/cancel. */
        const char *method = input == MENU_ACCEPT ? "onActivate" : "onCancel";
        for (ui_widget *h = w; h; h = h->parent) {
            ah_val result;
            int called = event_call(m, h, w, method, 0, 3, &result);
            if (called < 0 || (called && ah_num(result) != 0)) break;
        }
    }
    apply_pages(m);
}

int menu_take_project(menu *m, char *name, size_t capacity)
{
    if (!m->next_project[0] || !capacity) return 0;
    snprintf(name, capacity, "%s", m->next_project);
    m->next_project[0] = 0;
    return 1;
}

void menu_set_origin(menu *m, const char *project)
{
    /* 00109D00 +44..58 clears game+0x3A368 before loading each menu.
       Cached native VMs must also consume the previous sequence here;
       arcade.ad TopRoot.Option L647..649 only changes next_menu_project. */
    ah_set_member(m->vm, game_object(m), "next_sequence", ah_str(m->vm, ""));
    m->race_requested=0;
    ah_set_member(m->vm, game_object(m), "last_menu_project", ah_str(m->vm, project));
}

int menu_take_photo_film(menu *m,char *path,size_t capacity){
 if(!m || !capacity || !film_loaded[0])return 0;ah_val result;
 if(!ah_get_member(m->vm,game_object(m),"menu_result",&result) || !ah_cstr(result) || strcmp(ah_cstr(result),"develop") || !m->next_project[0])return 0;
 /* labomode.ad commandDevelop sets menu_result after file.load OK. */
 snprintf(path,capacity,"%s/films/%s",gt4_save_directory(),film_loaded);
 film_loaded[0]=0;m->next_project[0]=0;ah_set_member(m->vm,game_object(m),"menu_result",ah_nil());return 1;
}

int menu_setting_begin(menu *m){
 if(!m || strcmp(ah_module_name(m->project),"SettingProject"))return 0;
 menu_save_refresh(m);ah_val ctx=ah_objv(m->context),result;
 /* Native sequence dispatch consumed setting; authored exit requests menu. */
 ah_set_member(m->vm,game_object(m),"next_sequence",ah_str(m->vm,"menu"));
 /* setting.ad L28..58: bind current car, resolve rows, queue SettingRoot. */
 return ah_call_member(m->vm,m->project,"onLoad",1,&ctx,&result)>=0;
}
void menu_setting_context(menu *m,menu *source){
 if(!m || !source)return;const char *keys[]={"race_code","course_code","major_menu_project","major_menu_page"};
 /* setting.ad L36..45 reads the shared game context. Cached PC VMs
    carry separate objects, so copy the context labels at this boundary. */
 for(int i=0;i<4;i++){ah_val value=ah_nil();ah_get_member(source->vm,game_object(source),keys[i],&value);
  ah_set_member(m->vm,game_object(m),keys[i],ah_str(m->vm,ah_cstr(value)?ah_cstr(value):""));}
}
/* gtmode.ad MusicTheater -> exit_kick_project("slide"): its page
   (next_menu_arg) and the way back (major_menu_*), carried into the
   slide project's own VM */
void menu_jukebox_context(menu *m,menu *source){
 if(!m || !source)return;menu_setting_context(m,source);
 const char *keys[]={"next_menu_arg","major_menu_widget"};
 for(int i=0;i<2;i++){ah_val value=ah_nil();ah_get_member(source->vm,game_object(source),keys[i],&value);
  if(ah_cstr(value))ah_set_member(m->vm,game_object(m),keys[i],ah_str(m->vm,ah_cstr(value)));}
}
int menu_setting_end(menu *m){
 if(!m || strcmp(ah_module_name(m->project),"SettingProject"))return 0;
 ah_val ctx=ah_objv(m->context),result;
 /* setting.ad L65..69: synchronize sheets before saving the car. */
 if(ah_call_member(m->vm,m->project,"onUnload",1,&ctx,&result)<0)return 0;
 menu_save_capture(m);return 1;
}
int menu_riding_car_body(menu *m,uint8_t body[0x4b8]){
 if(!m)return 0;ah_val garage=ah_nil(),car=ah_nil();
 ah_get_member(m->vm,game_object(m),"garage",&garage);
 ah_get_member(m->vm,ah_as_obj(garage,AO_MODULE),"riding_car",&car);
 return gt4_game_car_body(m->vm,ah_as_obj(car,AO_MODULE),body);
}

/* Photo Travel's car parts: the riding car's fitted wheel and wing when
   it is the garage's car (no game.car_code), else none */
int menu_photo_parts(menu *m,char wheel[32],char wing[32])
{
    wheel[0]=wing[0]=0;if(!m)return 0;
    ah_val vehicle=ah_nil();ah_get_member(m->vm,game_object(m),"car_code",&vehicle);
    if(ah_cstr(vehicle) && *ah_cstr(vehicle))return 1;
    uint8_t body[0x4b8];return menu_riding_car_body(m,body) && gt4_body_part_labels(m->vol,body,wheel,wing);
}
int menu_photo_request(menu *m,char course[32],char car[96],int *color)
{
    if(!m || !course || !car || !color)return 0;
    ah_val scene=ah_nil(),vehicle=ah_nil(),paint=ah_nil(),garage=ah_nil();
    ah_obj *game=game_object(m);ah_get_member(m->vm,game,"course_code",&scene);
    ah_get_member(m->vm,game,"car_code",&vehicle);const char *label=ah_cstr(vehicle);
    if(!label || !*label) {
        ah_get_member(m->vm,game,"garage",&garage);
        ah_get_member(m->vm,ah_as_obj(garage,AO_MODULE),"riding_car",&vehicle);
        label=car_label_of(m->vm,vehicle);
        ah_get_member(m->vm,ah_as_obj(vehicle,AO_MODULE),"color_index",&paint);
    } else ah_get_member(m->vm,game,"color_index",&paint);
    const char *location=ah_cstr(scene);
    if(!location || strncmp(location,"ss_",3) || strlen(location)>=32 || !label || !*label || strlen(label)>=96)return 0;
    snprintf(course,32,"%s",location);snprintf(car,96,"%s",label);*color=ah_is_num(paint)?to_int(paint):0;return 1;
}

void menu_photo_set_selection(menu *m,const char *course,const char *car,int color)
{
    if(!m)return;ah_obj *game=game_object(m);
    ah_set_member(m->vm,game,"course_code",ah_str(m->vm,course));
    ah_set_member(m->vm,game,"car_code",ah_str(m->vm,car));
    ah_set_member(m->vm,game,"color_index",ah_int(color));
}
gt4_photo_mode *menu_photo_state(menu *m){return m?&m->photo:NULL;}
void menu_photo_set_location(menu *m,const gt4_course *course)
{
    if(!m)return;
    m->photo.ready=0;
    if(!gt4_photo_mode_init(&m->photo,course)){fprintf(stderr,"Photo Travel: invalid scene placement\n");return;}
    /* A: native map zoom setup is not yet compared; bounds 20..1000
       from 00370240 +60..7C, initial length from the destination. */
    for(int i=0;i<ui_root_count(m->ui);i++)if(!strcmp(ui_root_name(m->ui,i),"ShootRoot")) {
        ui_widget *map=ui_widget_child(ui_root_widget(m->ui,i),"Map"),*pm=ui_widget_child(map,"PM"),*zoom=ui_widget_child(pm,"zoom");
        if(zoom){zoom->photo_min=20;zoom->photo_max=1000;zoom->photo_value=fminf(1000,fmaxf(20,m->photo.location.map_length));}
        /* Loading a different destination must rerun the authored resets. */
        while(m->depth)finalize_page(m,m->pages[--m->depth]);
        m->pending_kind=0;m->entered[i]=0;
    }
}

const char *menu_selected_course(menu *m)
{
    ah_val root,code;
    if(m && ah_get_member(m->vm,m->project,"CourseRoot",&root) &&
       ah_get_member(m->vm,ah_as_obj(root,AO_MODULE),"current_crs",&code))
        return ah_cstr(code);
    return NULL;
}

int menu_car_color(const menu *m) { return m ? m->car_color : -1; }
int menu_take_race_request(menu *m)
{
    int r=m && m->race_requested;
    if(r && !strcmp(ah_module_name(m->project),"ArcadeProject") && !menu_race_is_split(m)){
        /* arcade.ad apply_setting L4689..4694 mutates main.game.option
           without setNeedSave. The PS2 keeps that same object across
           00111B50 (+1B0..1BC -> 00430878) and quick_tune_popup.ad L403.
           Native projects use separate VMs: publish the live preference
           before the pre-race VM restores its option snapshot. */
        options_capture(m);
        gt4_save_capture(m->vm,game_object(m),1);
    }
    if(m)m->race_requested=0;
    return r;
}
/* tests: the menu stack as a dealer leaves it (game.menuPush each name) */
void menu_test_push(menu *m,const char *names)
{
    if(!m || !names)return;
    ah_obj *game=game_object(m);ah_val push,result;
    if(!ah_get_member(m->vm,game,"menuPush",&push))return;
    char copy[256];snprintf(copy,sizeof copy,"%s",names);
    for(char *name=strtok(copy,",");name;name=strtok(NULL,",")){ah_val arg=ah_str(m->vm,name);ah_call(m->vm,push,1,&arg,&result);}
}
/* tests: credits through the game's own incMoney */
void menu_test_money(menu *m,int64_t amount)
{
    ah_val fn,arg,result;
    if(!m || !ah_get_member(m->vm,game_object(m),"incMoney",&fn) || fn.t==AV_NIL)return;
    arg=ah_int(amount);ah_call(m->vm,fn,1,&arg,&result);
}
/* tests: "label" a new car through the garage's own addNewCar, or
   "label:meters" a used one (MCarGarage.makeUsed, addUsedCar), then ridden */
void menu_test_car(menu *m,const char *spec)
{
    ah_val garage,ret,args[3],cars;char label[64];
    if(!m || !spec || !ah_get_member(m->vm,game_object(m),"garage",&garage) || garage.t==AV_NIL)return;
    snprintf(label,sizeof label,"%s",spec);char *meters=strchr(label,':');if(meters)*meters++=0;
    ah_obj *g=ah_as_obj(garage,AO_MODULE);args[0]=ah_str(m->vm,label);args[1]=ah_int(0);
    if(meters){
        ah_val main_,menu_,cls,source;
        if(!ah_get_member(m->vm,ah_root(m->vm),"main",&main_) || !ah_get_member(m->vm,ah_as_obj(main_,AO_MODULE),"menu",&menu_) ||
           !ah_get_member(m->vm,ah_as_obj(menu_,AO_MODULE),"MCarGarage",&cls) || ah_call(m->vm,cls,1,args,&source)<0)return;
        ah_val used[]={ah_int(1),ah_int(0),ah_int((int64_t)strtoull(meters,NULL,10))};
        if(ah_call_member(m->vm,ah_as_obj(source,AO_MODULE),"makeUsed",3,used,&ret)!=1 ||
           ah_call_member(m->vm,g,"addUsedCar",1,&source,&ret)!=1)return;
    }else if(ah_call_member(m->vm,g,"addNewCar",2,args,&ret)!=1)return;
    if(!ah_get_member(m->vm,g,"cars",&cars) || cars.t==AV_NIL)return;
    ah_val index=ah_int((int32_t)ah_array_size(ah_as_obj(cars,AO_ARRAY))-1);
    ah_call_member(m->vm,g,"setRidingCar",1,&index,&ret);
    menu_save_capture(m);   /* menu_enter reloads the garage from the save */
}
/* tests: what MissionRoot.onActivate leaves once its checks pass */
void menu_request_mission(menu *m,const char *code)
{
    if(!m || !code)return;
    ah_obj *game=game_object(m);
    ah_set_member(m->vm,game,"race_code",ah_str(m->vm,code));
    ah_set_member(m->vm,game,"next_sequence",ah_str(m->vm,"mission"));
    m->race_requested=1;
}
const char *menu_selected_car(menu *m)
{
    ah_val root,code;
    if(m && ah_get_member(m->vm,m->project,"CarRoot",&root) &&
       ah_get_member(m->vm,ah_as_obj(root,AO_MODULE),"current_car",&code))return ah_cstr(code);
    return NULL;
}

void menu_set_music_fader(menu *m,void (*fn)(float)){if(m)m->music_fader=fn;}
void menu_set_audio_preparer(menu *m,void (*fn)(menu *,void *),void *user)
{if(m){m->audio_preparer=fn;m->audio_preparer_user=user;}}
int menu_import_ps2_media(menu *m,const char *path,gt4_media_import *report){return m && gt4_media_import_ps2(m->vol,path,report);}

int menu_championship_context_get(menu *m,uint8_t live[0x90]){
    return m && gt4_game_championship_context_get(m->vm,game_object(m),live);
}
int menu_championship_context_set(menu *m,const uint8_t live[0x90]){
    if(!m || !gt4_game_championship_context_set(m->vm,game_object(m),live))return 0;
    menu_save_capture(m);return 1;
}
