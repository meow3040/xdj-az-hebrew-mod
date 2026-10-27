/* SPDX-License-Identifier: GPL-3.0-only */
/* Single touch-reader producer, UI consumer. No callbacks, allocation or IO. */
#define SECOND_SLOTS 16
#define SECOND_QUEUE 16
struct second_contact {int down,x,y,xy,decided;};
static struct second_contact second_contacts[SECOND_SLOTS];
static int second_slot,second_was_primary,second_lost;
static unsigned second_epoch,second_active,second_write,second_read;
static struct {uint64_t point;unsigned mode_seq;} second_queue[SECOND_QUEUE];
static void second_reset(void){
 memset(second_contacts,0,sizeof second_contacts);second_was_primary=0;second_lost=1;
 __atomic_store_n(&second_active,0,__ATOMIC_RELEASE);
}
static void second_feed(unsigned type,unsigned code,int value){
 if(type==EV_SYN&&code==SYN_DROPPED){second_reset();return;}
 if(type!=EV_ABS)return;
 if(code==ABS_MT_SLOT){second_slot=value;return;}
 if(second_slot<=0||second_slot>=SECOND_SLOTS)return;
 struct second_contact *s=&second_contacts[second_slot];
 if(code==ABS_MT_TRACKING_ID){s->down=value>=0;s->decided=0;s->xy=0;}
 else if(code==ABS_MT_POSITION_X){s->x=value;s->xy|=1;}
 else if(code==ABS_MT_POSITION_Y){s->y=value;s->xy|=2;}
}
static void second_report(int primary_owned){
 if(second_lost){if(primary_owned)return;second_lost=0;}
 if(primary_owned!=second_was_primary){
  if(primary_owned){if(!++second_epoch)++second_epoch;}
  __atomic_store_n(&second_active,primary_owned?second_epoch:0,__ATOMIC_RELEASE);
  second_was_primary=primary_owned;
 }
 for(int slot=1;slot<SECOND_SLOTS;slot++){
  struct second_contact *s=&second_contacts[slot];
  if(!s->down){s->decided=0;continue;}
  if(s->decided)continue;
  s->decided=1;
  if(!primary_owned||s->xy!=3||s->x<0||s->x>=1280||s->y<0||s->y>=800)continue;
  unsigned w=__atomic_load_n(&second_write,__ATOMIC_RELAXED);
  if(w-__atomic_load_n(&second_read,__ATOMIC_ACQUIRE)>=SECOND_QUEUE)continue;
  second_queue[w%SECOND_QUEUE].point=((uint64_t)second_epoch<<22)|((unsigned)s->y<<11)|(unsigned)s->x;
  second_queue[w%SECOND_QUEUE].mode_seq=__atomic_load_n(&g_mode_seq,__ATOMIC_ACQUIRE);
  __atomic_store_n(&second_write,w+1,__ATOMIC_RELEASE);
 }
}
static int second_pop(int *out){
 for(int i=0;i<SECOND_QUEUE;i++){
  unsigned r=__atomic_load_n(&second_read,__ATOMIC_RELAXED);
  if(r==__atomic_load_n(&second_write,__ATOMIC_ACQUIRE))return 0;
  uint64_t p=second_queue[r%SECOND_QUEUE].point;unsigned mode=second_queue[r%SECOND_QUEUE].mode_seq;
  __atomic_store_n(&second_read,r+1,__ATOMIC_RELEASE);
  if((unsigned)(p>>22)!=__atomic_load_n(&second_active,__ATOMIC_ACQUIRE)||mode!=__atomic_load_n(&g_mode_seq,__ATOMIC_ACQUIRE))continue;
  out[0]=(int)(p&2047);out[1]=(int)((p>>11)&2047);return 1;
 }
 return 0;
}
