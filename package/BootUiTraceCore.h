// SPDX-License-Identifier: MIT
// Bounded stack slices, not a stopped-task or atomic stack snapshot.
#ifndef SHENG_BOOT_UI_TRACE_CORE_H
#define SHENG_BOOT_UI_TRACE_CORE_H
#define BT_MAX_THREADS 64
#define BT_MAX_GROUPS (2*BT_MAX_THREADS)
#define BT_FRAMES 8
#define BT_ROWS 14
#define BT_HALF 60
#define BT_MAX_THR 10
struct bt_msg {unsigned bits,id,size,own;};
// Per-thread wait forensics: the saved x16 trap number and registers of the system/mach trap the thread is
// blocked in, plus the kernel link register where it sleeps and the raw kernel-context fields it was taken
// from (a thread that blocks on a continuation has no saved kernel register state, so those read as zero).
// cont names the kernel continuation the thread is parked on even then; frames is its user FP chain.
struct bt_thr {uint64_t tid,pc,lr,trap,event,kstack,kpcb,kstackptr,kbase,ksp,kfp,cont,fp,frames[BT_FRAMES];unsigned index,state,hint,kzero;int pri;unsigned regs[8];};
// A message still queued on a port reached through another port's destination. local is its reply port; the
// reply port's receiver task is the process that sent the message, so no stack walk is needed to name it.
struct bt_dmsg {uint64_t local;unsigned bits,id,size,l_flags;int l_pid;char l_kind,l_state,l_owner;};
struct bt_entry {
 uint64_t pc,lr,fp,frames[BT_FRAMES],regs[8],trap;
 int peer;unsigned peer_code;
 unsigned index,count,error,port_q,recv_q;
 unsigned send_kind,send_kotype,send_qlimit,send_srights,send_mscount;
 unsigned recv_pid,recv_qlimit,msg_n;
 uint64_t recv_port,send_kobj,recv_sync_port;
 unsigned recv_link;
 char recv_sync_match;
 struct bt_msg msgs[6];
 int msg_pid[6];
 unsigned send_flags,send_rname,send_spr,send_pdr,arg_bits,arg_id,arg_size;
 unsigned send_klist,send_link,recv_flags,recv_srpid,dst_klist,dst_link,dst_checkin,dst_lflags,dst_lname;
 unsigned dst_flags,dst_kind,dst_rname,dst_kt,dst_q,pdr_rname;
 int dst_pid,pdr_pid;
 uint64_t dst_port,pdr_port;
 char send_state,dst_state,pdr_state,dst_owner,pdr_kind,pdr_owner,svc[20];
 char role;
};
// One receive port in the traced launchd's own space: its name, queue, knote and service-label state.
// first_id is the id of the oldest queued header (w00t identifies launchd's stranded service port); stop
// owner/count come from a semaphore kobject when the probed port is one.
#define BT_SCAN 6
#define BT_QP 20
struct bt_sport {unsigned name,score,q,qlimit,flags,kl,link,checkin,lflags,lname,kt,first_id,srights,mscount,sorights,wait;int pid,sem_owner,sem_count;char state,kind,owner,svc[20];};
struct bt_scan {struct bt_sport ports[BT_SCAN],wport,mport,qp[BT_QP];unsigned n,e,r,q,kl,wl,sr,sv,cv,wq,qn,wps,wpd,wpg,wpr;};
// dyld_all_image_infos as dyld publishes it: version and count packed into the first word, the image array
// second. img0 is the probe's image= field (infoArray[0]); img_match names the /sbin/launchd entry by path,
// so a shared-cache image that precedes it cannot be mistaken for the process image.
struct bt_deep {unsigned version,img_count,img_error,img_match,img_index,thr_type;uint64_t img_array,img_base,img0;char img_path[40],img0_path[40],thr_name[40];};
struct bt_groups {struct bt_entry entries[BT_MAX_GROUPS];unsigned count,threads,read_failures;};
typedef int (*bt_reader)(void *,uint64_t,void *,unsigned);
static int bt_user(uint64_t address,unsigned size) {
 return address>=4096 && address<(UINT64_C(1)<<39) && size<=(UINT64_C(1)<<39)-address;
}
static void bt_walk_n(struct bt_entry *e,bt_reader read,void *context,uint64_t *frames,unsigned limit) {
 uint64_t fp=e->fp;
 for(unsigned i=0;i<limit && fp;++i) {
  uint64_t pair[2]={0};
  if(!bt_user(fp,sizeof(pair)) || (fp&15)){e->error=2;break;}
  if(read(context,fp,pair,sizeof(pair))){e->error=1;break;}
  if(pair[1] && !bt_user(pair[1],4)){e->error=2;break;}
  frames[i]=pair[1];
  if(!pair[0])break;
  if(pair[0]<=fp || pair[0]-fp>1048576){e->error=3;break;}
  fp=pair[0];
 }
}
static void bt_walk(struct bt_entry *e,bt_reader read,void *context) {
 bt_walk_n(e,read,context,e->frames,BT_FRAMES);
}
// Exact integer encoding, not truncated pointers. Seven radix-64 digits cover
// all native 39-bit user addresses. This alphabet avoids punctuation separators.
static void bt_address(char out[9],uint64_t value) {
 static const char digits[]="0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz-_";
 char reverse[11];unsigned n=0;
 do{reverse[n++]=digits[value&63];value>>=6;}while(value && n<sizeof(reverse));
 if(n>8){out[0]='?';out[1]=0;return;}
 for(unsigned i=0;i<n;++i)out[i]=reverse[n-i-1];out[n]=0;
}
static int bt_same(const struct bt_entry *a,const struct bt_entry *b) {
 return a->role==b->role && a->pc==b->pc && a->lr==b->lr && a->error==b->error && a->peer==b->peer && a->peer_code==b->peer_code &&
  !memcmp(a->frames,b->frames,sizeof(a->frames));
}
static void bt_add(struct bt_groups *g,const struct bt_entry *entry) {
 ++g->threads;g->read_failures+=entry->error==1;
 for(unsigned i=0;i<g->count;++i)if(bt_same(&g->entries[i],entry)){++g->entries[i].count;return;}
 if(g->count<BT_MAX_GROUPS){g->entries[g->count]=*entry;g->entries[g->count++].count=1;}
}
static void bt_format(const struct bt_entry *e,unsigned half,char out[BT_HALF]) {
 char a[6][9];
 if(!half) {
  bt_address(a[0],e->pc);bt_address(a[1],e->lr);
  for(unsigned i=0;i<4;++i)bt_address(a[i+2],e->frames[i]);
  snprintf(out,BT_HALF,"%c%02ux%u %s %s %s %s %s %s !%u",
   e->role,e->index,e->count,a[0],a[1],a[2],a[3],a[4],a[5],e->error);
 } else {
  for(unsigned i=0;i<3;++i)bt_address(a[i],e->frames[i+4]);
  snprintf(out,BT_HALF,"%c%02ux%u f5 %s %s %s q%u/%u p%d/e%u",
   e->role,e->index,e->count,a[0],a[1],a[2],e->port_q,e->recv_q,e->peer,e->peer_code);
 }
}
static void bt_rows(const struct bt_groups *g,unsigned page,char rows[BT_ROWS][120]) {
 unsigned start=page*BT_ROWS;
 for(unsigned row=0;row<BT_ROWS;++row) {
  memset(rows[row],' ',119);rows[row][119]=0;
  for(unsigned column=0;column<2;++column) {
   unsigned n=start+(row/2)*2+column;
   if(n>=g->count)continue;
   char text[BT_HALF];bt_format(&g->entries[n],row%2,text);
   memcpy(rows[row]+column*BT_HALF,text,strlen(text));
  }
 }
}
#endif
