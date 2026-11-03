#include "career_original.h"
#include <string.h>
#include <math.h>
#include "career_float.h"
#include <fenv.h>

/* 0014EC60 -> 00343A80 -> 00366300. 003660D8 initializes the linear
   distance curve from D_00620AF0: x=lh+4/+6 *1000, y=0/2, multiplier .5.
   Do not confuse this getter with the visual dirt field written by washing. */
float gt4_career_dirtiness(const uint8_t wear[32],const uint8_t params[12]){
 if(wear[0x14])return 0;
 int16_t a,b;uint32_t x;memcpy(&a,params+4,2);memcpy(&b,params+6,2);memcpy(&x,wear+0x10,4);
 uint32_t lo=career_f32_mul(career_f32_from_int(a),0x447a0000),hi=career_f32_mul(career_f32_from_int(b),0x447a0000);
 float left,right,value;memcpy(&left,&lo,4);memcpy(&right,&hi,4);memcpy(&value,&x,4);
 if(value<=left)return 0;if(value>=right)return 1;
 fenv_t env;fegetenv(&env);fesetround(FE_TOWARDZERO);
 float numerator=2.f*(value-left);float y=numerator/(right-left);value=y*.5f;
 fesetenv(&env);return value;
}
/* 0014EB80 setter -> 003438A0 +0..24: upper clamp only, at wear+C. */
void gt4_career_set_dirtiness(uint8_t wear[32],float value){uint32_t bits;memcpy(&bits,&value,4);if(value>0.7999999523162841796875f)bits=0x3f4ccccc;memcpy(wear+0xc,&bits,4);}

void gt4_career_process_event_result(gt4_career_event_result *s,int finished,
    int32_t position,uint32_t best_lap,int32_t driver_mode,int32_t aspec,uint32_t prize)
{
    /* 00114498 +0xa8 / +0x1bc -> 0043AFF0: zero is an abandonment,
       positions 1..6 are the finished-place counters. */
    uint32_t counter=finished?(uint32_t)position+1:0;
    if(counter<7){
        uint32_t value;memcpy(&value,s->statistics+0x54+counter*4,4);value++;
        memcpy(s->statistics+0x54+counter*4,&value,4);
    }
    s->calendar_method=14;
    if(!finished){
        /* 00114498 +0xdc..0x170: abandonments retain records, best
           position and accumulated prize; no credits are paid. */
        /* The work's prize remains untouched here. 00114250 +0xf4
           initializes it to zero before the session starts. */
        s->calendar_grade=15;s->calendar_time=0x157529ff;
        return;
    }
    /* 0011465C..68: the prize is lw/sign-extended before 64-bit addition. */
    uint64_t total;int32_t signed_prize;memcpy(&total,s->statistics+0x48,8);
    memcpy(&signed_prize,&prize,4);total+=(uint64_t)(int64_t)signed_prize;
    memcpy(s->statistics+0x48,&total,8);
    if(driver_mode==0 || driver_mode==2)gt4_career_record_score(&s->record,aspec);
    uint8_t grade=(uint8_t)position;
    /* 00114690..b8: each session keeps its own best position; update
       the persistent record only on improvement over that session byte. */
    if(grade<s->previous_best){
        gt4_career_record_update(&s->record,2,grade);s->previous_best=grade;
    }
    s->awarded_prize=prize;s->calendar_grade=grade;s->calendar_time=best_lap;
}

/* 0011C328 (a mission ended with the player through, world +0xCE0 4):
   the accumulated prize (+0x48, the prize sign-extended), the record's
   field 3 to the position and its A-spec score (no driver-mode test), a
   calendar event of method 15 with the race's time. 0011C2E0 (not
   through): only the event, grade and method 15, no time. The prize is
   paid afterwards by quick-mission.ad's onPrizeCheck. */
void gt4_career_process_mission_result(gt4_career_event_result *s,int finished,
    int32_t position,uint32_t race_time,int32_t aspec,uint32_t prize)
{
    s->calendar_method=15;
    if(!finished){s->calendar_grade=15;s->calendar_time=0x157529ff;return;}
    uint64_t total;int32_t signed_prize;memcpy(&total,s->statistics+0x48,8);
    memcpy(&signed_prize,&prize,4);total+=(uint64_t)(int64_t)signed_prize;
    memcpy(s->statistics+0x48,&total,8);
    uint8_t grade=(uint8_t)position;
    gt4_career_record_update(&s->record,3,grade);
    gt4_career_record_score(&s->record,aspec);
    s->awarded_prize=prize;s->calendar_grade=grade;s->calendar_time=race_time;
}

/* func_00579330/00578598/005792F0: original RNG float conversion. */
void gt4_career_enemy_rng_init(uint32_t *state){
    /* 005792D8..005792E4, invoked at startup by ctor 003BB820.
       The zero word stored in CORE is overwritten before any grid. */
    if(state)*state=0x12345678u;
}
static float enemy_random(uint32_t *state)
{
    uint32_t x=*state*17u+17u;*state=x;
    uint32_t r=x^((x<<16)|(x>>16));if(!r)return 0;
    uint32_t sign=(r>>31)?UINT32_MAX:0,y=r^sign,lead=0;
    while(lead<32 && !(y&(0x80000000u>>lead)))lead++;
    uint32_t lz=((lead-1)&31)|sign,e=125u-lz,m=(r<<((0u-e)&31))>>9;
    uint32_t bits=(e<<23)|m;float result;memcpy(&result,&bits,4);return result;
}

int gt4_career_enemy_indices(const uint8_t *blocked,unsigned count,unsigned cars,uint32_t *seed,unsigned *indices)
{
    /* 003BB5A8 +0x44..0x12c: preserve rejected entries, then avoid
       repeats until the remaining allowed list has been exhausted. */
    if(!blocked || !seed || !indices || !count || count>256 || cars>5)return 0;
    uint8_t used[256];unsigned remaining=0;
    for(unsigned i=0;i<count;i++){used[i]=blocked[i]!=0;if(!used[i])remaining++;}
    for(unsigned i=0;i<cars;i++){
        float choice=enemy_random(seed)*(float)count;unsigned selected=(unsigned)choice;
        if(selected>=count)return 0;
        if(remaining){while(used[selected])selected=(selected+1)%count;used[selected]=1;remaining--;}
        indices[i]=selected;
    }
    return 1;
}
void gt4_career_session_rules(const uint8_t r[151],uint8_t o[0x80]){
    /* func_0035E108 +0x28..0x2a8 clears then fills conductor+0xf818.
       Getter loops: 00447778 (+0x80..84), 004477D0 (+0x2e..36),
       00447828 (+0x91 for player, +0x8c..90 for five opponents). */
    static const uint8_t groups[]={0x4b,0x4f,0x5f,0x5b,0x57,0x53};
    static const uint8_t pairs[][2]={
        {0x20,0x63},{0x22,0x64},{0x24,0x65},{0x25,0x66},{0x26,0x67},
        {0x28,0x68},{0x29,0x69},{0x2a,0x6a},{0x2c,0x6b},{0x2d,0x6c},
        {0x2e,0x6d},{0x2f,0x6e},{0x30,0x6f},{0x33,0x70},{0x34,0x71},
        {0x31,0x72},{0x32,0x73},{0x49,0x8a},{0x48,0x8b},{0x4a,0x88},
        {0,0x85},{0x44,0x87},{0x46,0x75},{0x4e,0x42},{0x41,0x86}
    };
    memset(o,0,0x80);o[1]=r[0x43];o[2]=r[0x44];
    memcpy(o+0x38,r+0x18,2);
    for(unsigned i=0;i<sizeof groups;i++)memcpy(o+4+i*4,r+groups[i],4);
    for(unsigned i=0;i<sizeof pairs/sizeof pairs[0];i++)o[pairs[i][0]]=r[pairs[i][1]];
    if(r[0x6b] && !r[0x72])o[0x31]=4;
    o[0x4b]=50;o[0x40]=(uint8_t)(r[0x3e]-1);
    memcpy(o+0x54,r+0x14,2);
    for(unsigned i=0;i<5;i++){o[0x4f+i]=r[0x80+i];memcpy(o+0x56+i*2,r+0x2e + i*2,2);}
    o[0x60]=r[0x91];for(unsigned i=0;i<5;i++)o[0x62+i*2]=r[0x8c+i];
    o[0x4c]=0x50;
    o[0x21]=(int8_t)r[0x89]<-1?255:r[0x89];
}
int gt4_career_active_setup(const uint8_t body[0x4b8],uint8_t setup[0x178]){
    /* 00441248 +0x08 -> 00441228 +0x00..1c: live car +8 +
       0x178 * live car[0x490]. Serialized body starts at live car +8.
       001140C0 +0xb4..e8 and 0011B160 +0x94..c8 reject an empty key
       before 003EDD80 / 004452F8 copy exactly 0x178 bytes. */
    if(!body || !setup)return 0;
    uint32_t sheet;uint64_t key;
    memcpy(&sheet,body+0x488,sizeof sheet);
    if(sheet>=3)return 0; /* Host boundary: do not address beyond the body. */
    const uint8_t *selected=body+sheet*0x178u;
    memcpy(&key,selected,sizeof key);
    if(key==UINT64_MAX)return 0;
    memcpy(setup,selected,0x178);return 1;
}
/* func_00435348: first slot whose occupied bit is clear. Exactly 0x3e8
   entries, each 0x20 bytes; flags at garage+0x10+index*0x20. */
int gt4_career_garage_first_free(const uint8_t *garage){
    for(unsigned i=0;i<GT4_CAREER_GARAGE_CAPACITY;i++)if(!(garage[0x10+i*0x20]&1))return (int)i;
    return -1;
}
/* func_00435920: sum bit zero of the same 1000 flag words. */
unsigned gt4_career_garage_count(const uint8_t *garage){
    unsigned count=0;for(unsigned i=0;i<GT4_CAREER_GARAGE_CAPACITY;i++)count+=garage[0x10+i*0x20]&1;
    return count;
}
/* func_00435860 -> func_00434E08. The sd clears occupied bit only;
   the selected index at +0x81c8 becomes -1 only when it equals index.
   Empty-slot calls have the same writes; car data is deliberately retained. */
int gt4_career_garage_delete(uint8_t *garage,uint32_t index){
    garage[0x10+index*32]&=254;
    uint32_t selected=0;for(unsigned i=0;i<4;i++)selected|=(uint32_t)garage[0x81c8+i]<<(8*i);
    if(selected!=index)return 0;
    for(unsigned i=0;i<4;i++)garage[0x81c8+i]=255;return 1;
}
static uint32_t garage_u32(const uint8_t *p){
    return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
}
static void garage_w32(uint8_t *p,uint32_t value){
    for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(value>>(8*i));
}
/* func_00435690: promote the selected slot to history position zero.
   Iterate all 1000 slots, including unoccupied entries; keep upper 54 bits. */
void gt4_career_garage_promote(uint8_t *garage,uint32_t index){
    uint8_t *selected=garage+8+index*32;
    uint32_t position=garage_u32(selected+0x18)&1023u;
    if(!position)return;
    for(unsigned i=0;i<GT4_CAREER_GARAGE_CAPACITY;i++){
        uint8_t *history=garage+8+i*32+0x18;
        uint32_t value=garage_u32(history);
        if((value&1023u)<position)garage_w32(history,(value&~1023u)|(((value&1023u)+1)&1023u));
    }
    garage_w32(selected+0x18,garage_u32(selected+0x18)&~1023u);
}
/* func_00435388: copy the full car, retain destination class/vtable word,
   mark this unit as selected, then promote its history position. The
   original leaves the previous unit's selection flag untouched here. */
void gt4_career_garage_set_riding(uint8_t *garage,uint32_t index,const uint8_t car[0x4c0]){
    uint32_t header=garage_u32(garage+0x7d08);
    for(unsigned i=0;i<0x4c0;i++)garage[0x7d08+i]=car[i];
    garage_w32(garage+0x7d08,header);
    garage_w32(garage+0x81c8,index);
    garage[0x10+index*32]|=2;
    gt4_career_garage_promote(garage,index);
}
/* func_00435418: clearing the previous flag depends on +0x81f4,
   rather than merely on whether a previous selected index exists. */
void gt4_career_garage_change_riding(uint8_t *garage,uint32_t index,const uint8_t car[0x4c0]){
    uint32_t previous=garage_u32(garage+0x81c8);
    if(garage_u32(garage+0x81f4) && !(previous&0x80000000u))
        garage[0x10+previous*32]&=253;
    gt4_career_garage_set_riding(garage,index,car);
    garage_w32(garage+0x81f4,0);
}
/* func_00430C28: lbu, dsrl 4, andi limit 0xff, sentinel 0xf,
   then unsigned grade <= limit. Low record bits are not examined. */
int gt4_career_record_meets_grade(uint8_t record,uint32_t limit){
    uint32_t grade=record>>4;
    return grade!=15 && grade<=(limit&255u);
}
/* func_00430958: preserve top byte, clear type, initialize result fields,
   clear the byte at offset 2. */
uint32_t gt4_career_record_init(uint32_t record){return (record&0xff000000u)|0xfff0u;}
/* func_00430A10 */
uint32_t gt4_career_record_present(uint32_t record){return record|0xf000u;}
/* func_00430A20: signed upper clamp, followed by low-byte comparison.
   Negative inputs deliberately retain their original low-byte behavior. */
int gt4_career_record_score(uint32_t *record,int64_t score){
    uint32_t value=(uint32_t)(score<255?score:255)&255u;
    if(((*record>>16)&255u)>=value)return 0;
    *record=(*record&0xff00ffffu)|(value<<16);return 1;
}
/* func_00430A58: latest result always changes for an existing matching
   type; best result changes only on improvement. The comparison uses the
   full low byte of grade, while stored results use only its low nibble. */
int gt4_career_record_update(uint32_t *record,int32_t type,uint32_t grade){
    uint32_t old=*record,kind=old&15u,value=grade&255u;
    if(kind && (uint32_t)type!=kind)return 0;
    if(!kind){
        uint32_t next=(old&~15u)|((uint32_t)type&15u);
        next=(next&~0xf0u)|((value&15u)<<4);
        *record=(next&~0xf00u)|((value&15u)<<8);return 1;
    }
    if(kind>3)return 0;
    uint32_t next=(old&~0xf00u)|((value&15u)<<8);
    *record=next;
    if(value>=((next>>4)&15u))return 0;
    *record=(next&~0xf0u)|((value&15u)<<4);return 1;
}
/* func_00430BC0: type 1 best grade < 11; types 2/3 < 2. */
int gt4_career_record_predicate_00430bc0(uint32_t record){
    uint32_t grade=(record>>4)&15u,type=record&15u;
    if(grade==15)return 0;
    return type==1?grade<11:(type==2 || type==3)?grade<2:0;
}
/* func_00430C50: type 1 best grade < 13; types 2/3 == 0. */
int gt4_career_record_predicate_00430c50(uint32_t record){
    uint32_t grade=(record>>4)&15u,type=record&15u;
    return type==1?grade<13:(type==2 || type==3)?grade==0:0;
}
/* func_00430B58: type 1 best grade == 9; types 2/3 == 0. */
int gt4_career_record_predicate_00430b58(uint32_t record){
    uint32_t grade=(record>>4)&15u,type=record&15u;
    return type==1?grade==9:(type==2 || type==3)?grade==0:0;
}
/* func_004359E0: ld at garage+0x81e8, XOR with the 64-bit key. */
uint64_t gt4_career_money_get(const uint8_t *profile){
    uint64_t value=0;for(unsigned i=0;i<8;i++)value|=(uint64_t)profile[GT4_CAREER_MONEY_OFFSET+i]<<(8*i);
    return value^GT4_CAREER_MONEY_KEY;
}
/* func_004359B0: XOR, then sd at garage+0x81e8. */
void gt4_career_money_set(uint8_t *profile,uint64_t balance){
    uint64_t value=balance^GT4_CAREER_MONEY_KEY;
    for(unsigned i=0;i<8;i++)profile[GT4_CAREER_MONEY_OFFSET+i]=(uint8_t)(value>>(8*i));
}
/* func_0043AFB0: daddu at 0x0043AFD0, modulo 2^64. No saturation,
   overdraft check or signed-overflow exception exists in this function. */
uint64_t gt4_career_credit_add(uint64_t balance,uint64_t delta){return balance+delta;}
/* func_0043AFB0: profile+0xb8e8 is the garage object, getter then setter. */
void gt4_career_money_inc(uint8_t *profile,uint64_t delta){
    uint64_t balance=gt4_career_money_get(profile);
    gt4_career_money_set(profile,gt4_career_credit_add(balance,delta));
}

/* func_004475A8: unsigned 64-bit rank guard, six lhu branches,
   then the original 32-bit shift/add sequence (multiply by 100).
   race points to the original race structure; no prize data is embedded. */
uint32_t gt4_career_race_prize(const uint8_t *race,uint64_t position){
    uint32_t value=0;
    if(position<6){unsigned offset=0x20+(unsigned)position*2;value=race[offset]|((uint32_t)race[offset+1]<<8);}
    uint32_t next=(value<<1)+value;next=(next<<3)+value;return next<<2;
}
/* func_003434A8: sltu compares full 64-bit unsigned argument;
   movz retains the argument at/below the cap, sw stores its low word. */
uint32_t gt4_career_grid_time(int finished,uint32_t line_time){
    /* 00114424..00114438: car+0x5b6 selects car+0x718 or 0x157529ff. */
    return finished?line_time:0x157529ffu;
}
int32_t gt4_career_course_length(float metres){
    uint32_t bits;memcpy(&bits,&metres,4);
    /* 00119AB0 cvt.w.s: exponent >=158 saturates (including NaN/Inf);
       finite values in range truncate towards zero, regardless of FCSR. */
    if((bits&0x7f800000u)>=(158u<<23))return bits>>31?INT32_MIN:INT32_MAX;
    return (int32_t)metres;
}
uint32_t gt4_career_distance_integer(float distance){
    /* cvt.w.s saturates signed overflow and truncates towards zero;
       0010FB58..0010FB68 handles the unsigned high half separately. */
    int high=distance>=2147483648.0f;
    if(high)distance=distance-2147483648.0f;
    uint32_t bits;memcpy(&bits,&distance,4);
    uint32_t value=(bits&0x7f800000u)>=(158u<<23)?((bits>>31)?0x80000000u:0x7fffffffu):(uint32_t)(int32_t)distance;
    return high?value|0x80000000u:value;
}
void gt4_career_odometer_set(uint8_t *wear,uint64_t mileage){
    uint32_t stored=mileage>UINT64_C(0x5f5e1000)?0x5f5e1000u:(uint32_t)mileage;
    for(unsigned i=0;i<4;i++)wear[i]=(uint8_t)(stored>>(8*i));
}

/* func_00447670 / 004476B0 / 004476F0. Original minutes/seconds
   and little-endian millisecond residual, including the zero sentinel. */
uint32_t gt4_career_license_time(const uint8_t *race,unsigned medal){
    unsigned pair=medal*2;uint32_t seconds=race[0x77+pair];
    uint32_t next=(seconds<<5)-seconds;next=(next<<2)+seconds;next<<=3;
    uint32_t residual=race[0x1a+pair]|((uint32_t)race[0x1b+pair]<<8);
    uint32_t time=(uint32_t)race[0x76+pair]*0xea60u+next+residual;
    return time?time:0x157529ffu;
}

/* Scalar value paths in original MRaceData callbacks. Their v1 points to
   the boxed object; this API receives boxed+0x10 (the decoded RACE row).
   VM boxing/allocation is outside this port's equivalence boundary. */
int32_t gt4_career_race_field(const uint8_t *r,gt4_career_race_field_id field){
    switch(field){
    case GT4_RACE_LICENSE:return r[0x45]; /* func_001A16C0 */
    case GT4_RACE_DRIVETRAIN:return r[0x46]; /* func_001A1758 */
    case GT4_RACE_ASPIRATION:return r[0x48]; /* func_001A17F0 */
    case GT4_RACE_CAR_TYPE:return r[0x49]; /* func_001A1888 */
    case GT4_RACE_TIRE:return r[0x4a]; /* func_001A1920 */
    case GT4_RACE_PS:return ((r[0x92]<<2)+r[0x92])<<1; /* func_001A19B8 */
    case GT4_RACE_PRICE:return r[0x93]*10000; /* func_001A1A60 */
    case GT4_RACE_WEIGHT:return ((r[0x94]<<2)+r[0x94])<<1; /* func_001A1B08 */
    case GT4_RACE_LENGTH:{ /* func_001A1BB0 */
        uint32_t v=r[0x95],n=(v<<1)+v;n=(n<<3)+v;return (int32_t)(n<<2);}
    case GT4_RACE_COUNTRY:return r[0x96]; /* func_001A2638 */
    case GT4_RACE_ENTRY_NUM:return r[0x3e]; /* func_001A1370 */
    case GT4_RACE_LAPS:return r[0x43]; /* func_001A2E30 */
    case GT4_RACE_LIMIT_TIME:return r[0x18]|(uint32_t)r[0x19]<<8; /* func_001A2D98 */
    case GT4_RACE_PRIZE_GC: /* func_001A2910: signed lh, then original shifts. */
    case GT4_RACE_YEAR_MIN: /* func_001A26D0: s5=boxed+0x10, not v1=boxed. */
    case GT4_RACE_YEAR_MAX:{
        unsigned o=field==GT4_RACE_PRIZE_GC?0x2c:field==GT4_RACE_YEAR_MIN?0x38:0x3a;
        uint32_t v=r[o]|(uint32_t)r[o+1]<<8;
        int32_t signed_v=v<32768?(int32_t)v:(int32_t)v-65536;
        return field==GT4_RACE_PRIZE_GC?signed_v*100:signed_v;}
    }
    return 0;
}

/* func_00163990, value path at 001639F8: signed 64-bit slt, xori 1. */
int gt4_career_money_enough(int64_t balance,int64_t cost){return !(balance<cost);}

/* func_004309D0 (type), 001A4D88 (best), 001A4FA0 (latest),
   001A51B8 (present): direct nibble writes, without improvement checks. */
uint32_t gt4_career_record_field_set(uint32_t record,unsigned field,uint32_t value){
    if(field>3)return record;
    unsigned shift=field*4;return (record&~(15u<<shift))|((value&15u)<<shift);
}

/* func_004488A0: lh in the return delay slot, sign extended to 64 bits.
   GetModelYear callback 0012FFB8 invokes this after loading GENERIC_CAR. */
int32_t gt4_career_car_year(const uint8_t *row){
    uint32_t bits=row[12]|(uint32_t)row[13]<<8;
    return bits<32768?(int32_t)bits:(int32_t)bits-65536;
}
/* func_00448968, called by GetOpenModelType callback 0012E5A8. */
uint32_t gt4_career_car_open_type(const uint8_t *row){return row[20];}
/* func_00448908: unsigned byte divided by 100, returning the remainder.
   This is the enum key, not the category-name lookup 004488E8. */
uint32_t gt4_career_car_category(const uint8_t *row){return row[17]%100u;}

/* func_00439D70 / 00439D98, valid serialized flag indices 0..927. */
int gt4_career_present_get(const uint8_t flags[116],uint32_t bit){
    uint32_t at=(bit>>5)*4,word=0;
    for(unsigned i=0;i<4;i++)word|=(uint32_t)flags[at+i]<<(8*i);
    return (word>>(bit&31))&1;
}
void gt4_career_present_set(uint8_t flags[116],uint32_t bit){
    uint32_t at=(bit>>5)*4,word=0;
    for(unsigned i=0;i<4;i++)word|=(uint32_t)flags[at+i]<<(8*i);
    word|=UINT32_C(1)<<(bit&31);
    for(unsigned i=0;i<4;i++)flags[at+i]=(uint8_t)(word>>(8*i));
}
/* func_00163E80: addu then sw; arithmetic wraps at 32 bits. */
uint32_t gt4_career_prize_cars_add(uint32_t count,uint64_t delta){return count+(uint32_t)delta;}

/* func_00448C10 +0x70..0x84 delegates region membership to
   func_0044A5A8, whose delay slot +0x04 sets v0=0 in this CORE. */
int gt4_career_enemy_blocked(uint64_t key,int32_t region)
{
    (void)key; (void)region; return 0;
}

static float paint_float(uint32_t bits){float f;memcpy(&f,&bits,4);return f;}
/* 0040A428 +0x4c..0x164: normalized RGB and its magnitude; black
   substitutes length 1 before dividing. The fourth chip channel is unused. */
static void paint_choice(gt4_career_paint_choice *p,uint32_t rgb,int index)
{
    float r=(float)(rgb&255u)/255.0f,g=(float)((rgb>>8)&255u)/255.0f,b=(float)((rgb>>16)&255u)/255.0f;
    float rr=r*r,gg=g*g,bb=b*b,length=sqrtf((rr+gg)+bb);
    if(length==0)length=1.0f;
    p->index=index;p->rgb[0]=r/length;p->rgb[1]=g/length;p->rgb[2]=b/length;p->length=length;
}
/* 0040A338: identity special case, brightness distance and direction dot;
   preserve the instruction's order of float additions/multiplications. */
static float paint_similarity(const gt4_career_paint_choice *a,const gt4_career_paint_choice *b)
{
    if(a->rgb[0]==b->rgb[0] && a->rgb[1]==b->rgb[1] && a->rgb[2]==b->rgb[2] && a->length==b->length)return 1;
    float ar=a->rgb[0]*a->length,ag=a->rgb[1]*a->length,ab=a->rgb[2]*a->length;
    float br=b->rgb[0]*b->length,bg=b->rgb[1]*b->length,bb=b->rgb[2]*b->length;
    float dr=ar-br,dg=ag-bg,db=ab-bb;
    float distance=sqrtf((dr*dr+dg*dg)+db*db);
    float scale=paint_float(0x3fddb3d7u);
    float similarity=(scale-distance)/scale;
    float dot=(a->rgb[0]*b->rgb[0]+a->rgb[1]*b->rgb[1])+a->rgb[2]*b->rgb[2];
    similarity=similarity*dot;
    if(similarity<0)return 0;if(1.0f<similarity)return 1;return similarity;
}
int gt4_career_paint_weights(const gt4_career_paint_car *cars,unsigned count,gt4_career_paint_group groups[6])
{
    if(!cars || !groups || !count || count>6)return 0;
    for(unsigned i=0;i<count;i++)if(!cars[i].count || cars[i].count>32 || cars[i].color<-1 || cars[i].color>=(int)cars[i].count)return 0;
    /* 0040A6F0 +0x68..0x14c: geometric initial weights and normalization. */
    for(unsigned i=0;i<count;i++){
        gt4_career_paint_group *g=&groups[i];
        g->count=cars[i].color==-1?(int32_t)cars[i].count:1;
        float weight=1,total=0;
        for(int j=0;j<g->count;j++){
            int index=cars[i].color==-1?j:cars[i].color;
            paint_choice(&g->choices[j],cars[i].rgb[index],index);
            g->choices[j].weight=weight;total=total+weight;weight=weight*paint_float(0x3f7c0000u);
        }
        for(int j=0;j<g->count;j++)g->choices[j].weight=g->choices[j].weight/total;
    }
    /* +0x190..0x314: penalize similarity against every other candidate.
       Fixed paints (one choice) square similarity six times, then *10000. */
    for(unsigned i=0;i<count;i++){
        gt4_career_paint_group *g=&groups[i];float total=0;
        for(int j=0;j<g->count;j++){
            float penalty=1;
            for(unsigned k=0;k<count;k++)for(int l=0;l<groups[k].count;l++){
                if(i==k && j==l)continue;
                float similarity=paint_similarity(&g->choices[j],&groups[k].choices[l]);
                if(groups[k].count==1){for(int n=0;n<6;n++)similarity=similarity*similarity;similarity=similarity*paint_float(0x461c4000u);}
                penalty=penalty+similarity;
            }
            float reciprocal=1.0f/penalty;g->choices[j].weight=g->choices[j].weight*reciprocal;
            total=total+g->choices[j].weight;
        }
        /* 0040A5A0: stable ascending insertion, complete 0x18-byte records. */
        for(int j=0;j<g->count;j++){
            gt4_career_paint_choice value=g->choices[j];
            for(int k=0;k<j;k++)if(value.weight<g->choices[k].weight){gt4_career_paint_choice tmp=g->choices[k];g->choices[k]=value;value=tmp;}
            g->choices[j]=value;
        }
        for(int j=0;j<g->count;j++)g->choices[j].weight=g->choices[j].weight/total;
    }
    return 1;
}
int gt4_career_select_paints(gt4_career_paint_car *cars,unsigned count,uint32_t *seed)
{
    if(!seed)return 0;
    gt4_career_paint_group groups[6];
    if(!gt4_career_paint_weights(cars,count,groups))return 0;
    /* 0040AA48 +0x7c..0x11c rebuilds the weights after each assignment.
       +0xd0..0xec takes a fresh RNG draw per choice, not a cumulative draw. */
    for(unsigned i=0;i<count;i++)if(cars[i].color==-1){
        if(!gt4_career_paint_weights(cars,count,groups))return 0;
        int j=0;while(j<groups[i].count-1){if(enemy_random(seed)<groups[i].choices[j].weight)break;j++;}
        cars[i].color=groups[i].choices[j].index;
    }
    return 1;
}
int gt4_career_paint_list_index(uint64_t paint,int32_t total,int current)
{
    uint32_t bits=(uint32_t)(paint-1u);int32_t index;memcpy(&index,&bits,4);
    if(paint==UINT64_MAX)index=-1;
    if(index<total){unsigned byte=(uint8_t)index;return (int)byte-(byte>=128?256:0);}
    return current;
}

void gt4_career_championship_points(int32_t points[6],const uint8_t order[6],
    unsigned count,int finished,const int8_t awards[6]){
    /* 003EEFE0 +0x20..0x64 / 003EF1E8 +0x40..0x88. */
    unsigned rank=0;
    for(unsigned i=0;i<count && i<6;i++){
        unsigned driver=order[i];if(driver>=6 || (!finished && !driver))continue;
        uint32_t value;memcpy(&value,&points[driver],4);
        value+=(uint32_t)(int32_t)awards[rank++];memcpy(&points[driver],&value,4);
    }
}
int gt4_career_championship_rank(const int32_t points[6]){
    int rank=0;for(unsigned i=1;i<6;i++)if(points[i]>=points[0])rank++;return rank;
}
void gt4_career_process_championship_result(gt4_career_championship_result *s,
    int finished,int final_session,int32_t position,uint32_t best_lap,
    int32_t mode,int32_t aspec,uint32_t prize,const int32_t points[6],int16_t units){
    /* 00119CB8/00119F28: unlike an event retry, every completed round
       updates its own record, without a sequence-local best filter. */
    gt4_career_event_result *r=&s->result;
    uint32_t counter=finished?(uint32_t)position+1:0;
    if(counter<7){uint32_t value;memcpy(&value,r->statistics+0x54+counter*4,4);value++;memcpy(r->statistics+0x54+counter*4,&value,4);}
    if(finished && (mode==0 || mode==2))gt4_career_record_score(&r->record,aspec);
    s->grand_prize=0;s->series_rank=(uint32_t)gt4_career_championship_rank(points);
    if(final_session && !s->series_rank){
        /* 0011A0A8..0011A0C4: lh RACE+0x2c, signed units *100. */
        s->grand_prize=(uint32_t)((int32_t)units*100);
        gt4_career_record_update(&s->series_record,2,0);
    }
    if(finished || final_session)gt4_career_record_update(&r->record,2,finished?(uint8_t)position:255);
    uint64_t total;memcpy(&total,r->statistics+0x48,8);
    if(final_session){uint32_t low=(finished?prize:0)+s->grand_prize;int32_t signed_low;memcpy(&signed_low,&low,4);total+=(uint64_t)(int64_t)signed_low;}
    else if(finished){int32_t signed_prize;memcpy(&signed_prize,&prize,4);total+=(uint64_t)(int64_t)signed_prize;}
    memcpy(r->statistics+0x48,&total,8);
    if(finished || final_session)r->awarded_prize=finished?prize:0;
    r->calendar_grade=finished?(uint8_t)position:15;
    r->calendar_method=final_session?(!s->series_rank?0:14):(finished?14:15);
    r->calendar_time=finished || final_session?best_lap:0x157529ff;
}

void gt4_career_championship_context(uint8_t context[0x90],int active,
    uint32_t round,uint32_t last_round,const char *series,
    const uint64_t keys[6],const int32_t paints[6],const int32_t points[6]){
    /* 00119C0C/28/38: byte stores; 0042F860 -> 005A6AB0 copies and
       zero pads exactly 32 label bytes at +8. Driver records +0x30. */
    context[0x28]=(uint8_t)active;
    if(!active)return;
    context[0x29]=(uint8_t)round;context[0x2a]=(uint8_t)last_round;
    unsigned length=0;while(length<32 && series[length])length++;
    memcpy(context+8,series,length);memset(context+8+length,0,32-length);
    for(unsigned i=0;i<6;i++){
        memcpy(context+0x30+i*16,keys+i,8);
        memcpy(context+0x38+i*16,paints+i,4);
        memcpy(context+0x3c+i*16,points+i,4);
    }
}

void gt4_career_menu_clear(gt4_career_menu_stack *s){
    /* 0042FB48: only two leading bytes and two counts are cleared. */
    s->length=0;s->saved_length=-1;s->entries[0]=s->saved[0]=0;
}
void gt4_career_menu_push(gt4_career_menu_stack *s,const char *text){
    /* 0042FD38 compares length+strlen < 255, including empty strings. */
    size_t n=strlen(text);if(s->length>=0 && n<255 && (size_t)s->length+n<255){
        memcpy(s->entries+s->length,text,n+1);s->length+=(int32_t)n+1;
    }
}
const char *gt4_career_menu_pop(gt4_career_menu_stack *s){
    if(s->length<=0)return NULL;int32_t at=s->length-2;
    while(at>=0 && s->entries[at])at--;s->length=at+1;
    return (const char *)s->entries+s->length;
}
const char *gt4_career_menu_ref(const gt4_career_menu_stack *s,int32_t depth){
    if(s->length<=0)return NULL;
    int32_t steps=(int32_t)((uint32_t)depth+1);int32_t at=s->length-1;
    if(steps>s->length+1)return NULL;
    while(steps>0){at--;while(at>=0 && s->entries[at])at--;steps--;}
    return at< -1?NULL:(const char *)s->entries+at+1;
}
void gt4_career_menu_save(gt4_career_menu_stack *s){
    s->saved_length=s->length;memcpy(s->saved,s->entries,256); /* 0042FE88 */
}
int gt4_career_menu_load(gt4_career_menu_stack *s){
    if(s->saved_length<0)return 0; /* 0042FF4C leaves current intact. */
    s->length=s->saved_length;memcpy(s->entries,s->saved,256);return 1;
}

/* 00441300 jump branches: types 0..5 -> 0, 6..10 -> 1, 11 -> 2,
   12 -> 3, all others -> -1. 00441350 loads live+0x470+8*index
   and writes live+0xf8+0x178*sheet, omitting class header +8 here. */
void gt4_career_end_setting(uint8_t body[0x4b8],const int32_t types[3]){
    for(unsigned sheet=0;sheet<3;sheet++){
        int32_t type=types[sheet];int index=type>=0 && type<6?0:type>=6 && type<11?1:type==11?2:type==12?3:-1;
        memcpy(body+0xf0+sheet*0x178,body+0x468+index*8,8);
    }
}

/* 004401C0 calls 0043FE58..00440170 in this order. Live car has
   eight-byte header: keys live+0x30..0x100 -> body+0x28..0xf8.
   Each getter selects live+0x178*slot via live+0x490. */
int gt4_career_prepare_setting(const uint8_t body[0x4b8],uint8_t *buffer,size_t size,
    gt4_career_setting_lookup lookup,void *ctx){
    static const unsigned keys[]={0x28,0x48,0x40,0x80,0xb0,0x50,0x58,0xe8,0xe0,0x100,0xf8};
    static const unsigned offsets[]={0,0xc,0x3a,0x50,0x70,0x84,0xf4,0x110,0x11a,0x134,0x13a};
    uint32_t slot;memcpy(&slot,body+0x488,4);
    if(slot>=3 || !lookup || !buffer || size<0x13a)return 0;
    for(unsigned i=0;i<11;i++){
        uint64_t key;memcpy(&key,body+slot*0x178+keys[i],8);
        if(lookup(ctx,key,buffer+offsets[i],size-offsets[i])<0)return 0;
    }return 1;
}

/* 00449D58: cache key -1, pointer/size zero, type pointer at cache+16.
   00146628: six words at wrapper+0x174..0x188, word +0x180 is -1.
   Retain all other bytes, including the setting parts buffer. */
void gt4_career_setting_wrapper_reset(uint8_t wrapper[0x190],uint32_t cache_type){
    memset(wrapper+0x18,0xff,8);memset(wrapper+0x20,0,8);
    memcpy(wrapper+0x28,&cache_type,4);
    memset(wrapper+0x174,0,0x18);memset(wrapper+0x180,0xff,4);
}

/* 0043F4C8 jump branches call 004462A0..004468E0 on active setup.
   Offsets below are setup-relative keys (live setup header removed),
   then row byte offsets from each getter. 004463E0 alone uses signed lb.
   004468E0 reads setup+0x168 directly; zero means no equipped size. */
int gt4_career_equipped_stage(const uint8_t body[0x4b8],int32_t type,
    const uint8_t normal_flags[32],gt4_career_setting_lookup lookup,void *ctx,int32_t *stage){
    static const int keys[]={0x20,0x28,0x50,0xe0,0xe8,-1,0x80,0x78,-1,0x40,0x48,-1,0xa8,0xb0,0x88,0x90,0x98,0xa0,0xd8,0xd0,0xc0,0xb8,0xc8,0x58,0x60,0x68,0xf8,0x108,-1,-1};
    static const unsigned offsets[]={2,2,0xe,2,2,0,0xa,4,0,0xe,0x22,0,8,8,6,6,6,6,6,6,2,2,2,2,0x3a,0x38,4,6,0,0};
    if(!stage || !normal_flags)return 0;int32_t value=-1;
    if(type>=1 && type<=31){
        uint32_t slot;memcpy(&slot,body+0x488,4);if(slot>=3)return 0;
        const uint8_t *setup=body+slot*0x178;
        if(type==31){if(setup[0x168])value=setup[0x168];}
        else if(keys[type-1]>=0){
            uint64_t key;memcpy(&key,setup+keys[type-1],8);
            if(key!=UINT64_MAX){
                uint8_t row[256];int length=lookup?lookup(ctx,key,row,sizeof row):0;if(length<=(int)offsets[type-1])return 0;
                value=type==3?(int8_t)row[offsets[type-1]]:row[offsets[type-1]];
            }
        }
    }
    /* 0043F9E8 -> 0044A1B8: negative stages fall back to normal 0. */
    if(value<0 && type>=1 && type<=32 && normal_flags[type-1])value=0;
    *stage=value;return 1;
}

/* 00446AE0, jtbl_006A6E60, addresses relative to active setup. */
int gt4_career_remove_part(uint8_t body[0x4b8],uint32_t type){
    static const unsigned offsets[]={0x20,0x28,0x50,0xe0,0xe8,0x30,0x80,0x78,0x70,0x40,0x48,0x38,0xa8,0xb0,0x88,0x90,0x98,0xa0,0xd8,0xd0,0xc0,0xb8,0xc8,0x58,0x60,0x68,0xf8,0x108,0xf0,0x100};
    type&=65535u; /* 00446AE0 andi 0xffff precedes switch. */
    if(type<1 || type>30)return 1;
    uint32_t slot;memcpy(&slot,body+0x488,4);if(slot>=3)return 0;
    memset(body+slot*0x178+offsets[type-1],0xff,8);return 1;
}
/* 004455B0 obtains GENERIC_CAR by setup key +0, follows its +0 key
   to DEFAULT_PARTS, and only type 20 tests DEFAULT_PARTS+0x98 (sp+0xb8, row begins sp+0x20) == -1. */
int gt4_career_default_no_part(const uint8_t body[0x4b8],int32_t type,
    gt4_career_setting_lookup lookup,void *ctx,int *result){
    *result=0;if(type!=20)return 1;
    uint32_t slot;memcpy(&slot,body+0x488,4);if(slot>=3 || !lookup)return 0;
    uint64_t key;uint8_t row[256];memcpy(&key,body+slot*0x178,8);
    if(lookup(ctx,key,row,sizeof row)<8)return 0;memcpy(&key,row,8);
    if(key==UINT64_MAX)return 1;int size=lookup(ctx,key,row,sizeof row);
    if(!size)return 1;if(size<0xa0)return 0;memcpy(&key,row+0x98,8);
    *result=key==UINT64_MAX;return 1;
}

/* 00440B50, jtbl_006A6890: key-only installation branches. Return zero
   without mutation for branches that require SpecDB-derived defaults. */
int gt4_career_install_key_only(uint8_t body[0x4b8],uint64_t key){
    uint32_t type=(uint32_t)(key>>32),slot;unsigned offset=0;int all=0;
    switch(type){
    case 1:offset=0x20;break;case 13:offset=0xa8;break;
    case 18:offset=0xa0;break;case 19:offset=0xd8;break;
    case 20:offset=0xd0;break;case 21:offset=0xc0;break;
    case 22:offset=0xb8;break;case 23:offset=0xc8;break;
    case 25:offset=0x60;break;case 26:offset=0x68;break;
    case 28:offset=0x108;break;
    case 8:offset=0x78;all=1;break;case 15:offset=0x88;all=1;break;
    case 16:offset=0x90;all=1;break;case 17:offset=0x98;all=1;break;
    case 29:offset=0xf0;all=1;break;case 30:offset=0x100;all=1;break;
    /* 00440E58 -> 004468F8, jtbl_006A6B60 entry 31 -> 00446AC8:
       rigidity installation sets +0x168 on all three sheets, no piece key. */
    case 31:for(unsigned i=0;i<3;i++)body[i*0x178+0x168]=1;return 1;
    case 6:case 7:case 9:case 12:return 1;
    default:return type<1 || type>31;
    }
    memcpy(&slot,body+0x488,4);if(slot>=3)return 0;
    for(unsigned i=0;i<(all?3u:1u);i++)memcpy(body+(all?i:slot)*0x178+offset,&key,8);
    return 1;
}

/* 00440268 / 004403C0 / 004408D8. a2 is the existing wrapper piece
   buffer; getters preserve it if the key has no row. 004452A8 clamps
   lower first, then upper, including inverted bounds. */
static uint8_t setting_clamp_byte(unsigned lo,unsigned value,unsigned hi){
    if(value<lo)value=lo;if(value>hi)value=hi;return (uint8_t)value;
}
int gt4_career_install_defaults(uint8_t body[0x4b8],uint64_t key,
    uint8_t buffer[0x142],gt4_career_setting_lookup lookup,void *ctx){
    unsigned type=(unsigned)(key>>32),dest,width,offset;uint32_t slot;
    switch(type){case 11:dest=0xc;width=0x2d;offset=0x48;break;case 2:dest=0;width=0xc;offset=0x28;break;
    case 3:dest=0x84;width=0x70;offset=0x50;break;
    case 4:dest=0x11a;width=0x1a;offset=0xe0;break;
    case 5:dest=0x110;width=0xa;offset=0xe8;break;
    case 14:dest=0x70;width=0x14;offset=0xb0;break;
    case 24:dest=0xf4;width=0x1c;offset=0x58;break;
    case 10:dest=0x3a;width=0x16;offset=0x40;break;
    case 27:dest=0x13a;width=8;offset=0xf8;break;default:return 0;}
    memcpy(&slot,body+0x488,4);if(slot>=3 || !lookup)return 0;
    uint8_t row[256];int size=lookup(ctx,key,row,sizeof row);
    /* 00440268 +0x68 reads through row +0x0a; BRAKECONTROLLER.dbt
       on the US disc has 11 bytes, not its 12-byte wrapper allocation.
       00443ED0 copies the decoded row length, preserving wrapper padding.
       The other minima likewise end at the last byte these setters read. */
    unsigned required=type==2?11:type==3?0x6e:type==4?0x19:
        type==14?0x10:type==24?0x1b:type==10?0x15:type==27?6:type==11?0x29:width;
    if(size<0 || (size>0 && ((unsigned)size<required || dest+(unsigned)size>0x142)))return 0;
    if(size>0)memcpy(buffer+dest,row,(size_t)size);
    uint8_t *setup=body+slot*0x178;memcpy(setup+offset,&key,8);
    /* 004402F0 +0x44..0xb8: gearbox defaults before 00443240. */
    if(type==11){
        memcpy(setup+0x110,buffer+0x22,2);memcpy(setup+0x112,buffer+0xc,14);
        uint16_t lo,hi,value;memcpy(&lo,buffer+0x24,2);memcpy(&hi,buffer+0x26,2);memcpy(&value,buffer+0x28,2);
        if(value<lo)value=lo;if(value>hi)value=hi;
        memcpy(setup+0x128,&value,2);memcpy(setup+0x12c,&value,2);
        setup[0x12a]=setting_clamp_byte(buffer[0x32],buffer[0x34],buffer[0x33]);
    }
    else if(type==2){setup[0x12f]=setting_clamp_byte(1,buffer[6],buffer[3]);
        setup[0x130]=setting_clamp_byte(1,buffer[0xa],buffer[7]);}
    else if(type==10)setup[0x12e]=setting_clamp_byte(buffer[0x4c],buffer[0x4e],buffer[0x4d]);
    else if(type==27)setup[0x167]=buffer[0x13f];
    /* 004404B8 suspension, 004407B0 TCSC, 00440820 ASCC,
       004406C8 LSD and 00440430 steering: original piece-buffer offsets. */
    else if(type==14)memcpy(setup+0x133,buffer+0x7a,6);
    else if(type==5)setup[0x156]=setting_clamp_byte(1,buffer[0x119],buffer[0x116]);
    else if(type==4){const unsigned def[]={0x121,0x127,0x12c,0x132},hi[]={0x11e,0x124,0x129,0x12f};
        for(unsigned j=0;j<4;j++)setup[0x157+j]=setting_clamp_byte(1,buffer[def[j]],buffer[hi[j]]);}
    else if(type==24){const unsigned lo[]={0xf8,0x104,0xfc,0x108,0x100,0x10c};
        for(unsigned j=0;j<6;j++)setup[0x150+j]=setting_clamp_byte(buffer[lo[j]],buffer[lo[j]+2],buffer[lo[j]+1]);}
    else if(type==3){
        setup[0x140]=buffer[0xad];setup[0x141]=buffer[0xb0];
        setup[0x139]=setting_clamp_byte(buffer[0x93],buffer[0x95],buffer[0x94]);
        setup[0x13a]=setting_clamp_byte(buffer[0x96],buffer[0x98],buffer[0x97]);
        for(unsigned j=0;j<2;j++){uint16_t lo,hi,value;unsigned at=0x84+j*6;
            memcpy(&lo,buffer+at,2);memcpy(&hi,buffer+at+2,2);memcpy(&value,buffer+at+4,2);
            if(value<lo)value=lo;if(value>hi)value=hi;memcpy(setup+0x13c+j*2,&value,2);}
        setup[0x142]=setting_clamp_byte(buffer[0xb8],buffer[0xba],buffer[0xb9]);
        setup[0x143]=setting_clamp_byte(buffer[0xbb],buffer[0xbd],buffer[0xbc]);
        setup[0x144]=buffer[0xbe];setup[0x145]=buffer[0xbf];
        const unsigned def[]={0xcf,0xd2,0xd6,0xd9,0xdd,0xe0,0xe4,0xe7,0xed,0xf1};
        const unsigned hi[]={0xcc,0xcc,0xd3,0xd3,0xda,0xda,0xe1,0xe1,0xea,0xee};
        for(unsigned j=0;j<10;j++)setup[0x146+j]=setting_clamp_byte(1,buffer[def[j]],buffer[hi[j]]);
    }
    return 1;
}
