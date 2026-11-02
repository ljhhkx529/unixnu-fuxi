// SPDX-License-Identifier: MIT
// Cross-task reads use the native XNU VM API, outside task/scheduler locks.
// A user-page fault can block this isolated capture worker, never the monitor.
#include <kern/task.h>
#include <kern/thread.h>
#include <kern/simple_lock.h>
#include <kern/clock.h>
#include <kern/misc_protos.h>
#include <machine/machine_routines.h>
#include <arm/thread.h>
#include <vm/vm_map.h>
#include <ipc/ipc_space.h>
#include <ipc/ipc_entry.h>
#include <ipc/ipc_port.h>
#include <ipc/ipc_kmsg.h>
#include <ipc/ipc_service_port.h>
#include <kern/ipc_kobject.h>
#include <kern/sync_sema.h>
#include <kern/circle_queue.h>
#include <string.h>
#include "BootUiTraceCore.h"
#include "BootUiTraceDetails.h"
// Set by the fuxi USB debug adapter (FuxiUsbCompat.c): 0 not reached, 1 init enter, -1 init failed,
// 2 init ok, 3 worker started, 4 worker start failed, -2 boot arg disabled.
extern int fuxi_usbdbg_phase;
extern int fuxi_usbdbg_result;
static struct bt_entry samples[BT_MAX_THREADS];
// Hot threads: the context-switch rate of each sampled thread between two snapshots, so a thread that is woken hundreds of times a second
// shows even though every single stack sample finds it idle in mach_msg.
static uint64_t sample_tid[BT_MAX_THREADS];static uint32_t sample_cs[BT_MAX_THREADS];
struct bt_prev {uint64_t tid,at;uint32_t cs;};
#define BT_HOT 4
#define BT_HOT_MIN_RATE 50
#define BT_HOT_FRAMES 18
#define BT_KFRAMES 12
struct bt_hotrow {unsigned rate;char role;int peer;uint64_t tid,pc,lr,frames[BT_HOT_FRAMES],kernel[BT_KFRAMES];unsigned state,hint;int pri;uint64_t event;};
static uint64_t sample_k[BT_MAX_THREADS][BT_KFRAMES],sample_event[BT_MAX_THREADS];
static unsigned sample_state[BT_MAX_THREADS],sample_hint[BT_MAX_THREADS];static int sample_pri[BT_MAX_THREADS];
// Where a blocked thread sits in the kernel: the link register and frame chain its context switch saved (the thread is not running while
// it is blocked; for a running one the chain is merely stale, and every step stays inside its kernel stack).
static void bt_kernel_stack(thread_t thread,uint64_t *out) {
 memset(out,0,BT_KFRAMES*sizeof(uint64_t));
 if(!thread->kernel_stack)return;
 const uint64_t low=(uint64_t)thread->kernel_stack,high=low+(uint64_t)kernel_stack_size;
 thread_kernel_state_t state=thread_get_kernel_state(thread);
 uint64_t fp=state->machine.ss.fp;out[0]=state->machine.ss.lr;
 for(unsigned i=1;i<BT_KFRAMES && fp>=low && fp+16<=high && !(fp&15);++i) {
  const uint64_t *frame=(const uint64_t *)fp;out[i]=frame[1];
  const uint64_t next=frame[0];if(next<=fp)break;fp=next;
 }
}
// The raw fields bt_kernel_stack reads, kept even when they are all zero: a thread that blocks through a
// continuation is switched out without saving the kernel registers, so its saved state stays at the values
// machine_stack_attach wrote and the kernel link register reads as zero. kzero flags that case.
static void bt_kernel_raw(thread_t thread,struct bt_thr *t) {
 t->kpcb=(uint64_t)(uintptr_t)thread->machine.kpcb;
 t->kstackptr=(uint64_t)(uintptr_t)thread->machine.kstackptr;
 t->kbase=(uint64_t)thread->kernel_stack;
 t->ksp=0;t->kfp=0;t->kzero=1;
 if(!thread->kernel_stack)return;
 thread_kernel_state_t state=thread_get_kernel_state(thread);
 t->ksp=state->machine.ss.sp;t->kfp=state->machine.ss.fp;
 t->kzero=(state->machine.ss.sp|state->machine.ss.fp|state->machine.ss.lr)?0:1;
}
#define BT_ROLES 4
// Last space seen per role, compared by value only (never dereferenced): names the receiver space of
// a port the traced processes block on without locking or referencing that space.
static ipc_space_t role_space[BT_ROLES];
static char role_char_of[BT_ROLES];
static struct bt_main draft_main[BT_ROLES],published_main[BT_ROLES];
static unsigned qmax_persist[BT_ROLES];
static struct bt_prev hot_prev[BT_ROLES][BT_MAX_THREADS];
static struct bt_hotrow hot_draft[BT_ROLES][BT_HOT],hot_published[BT_ROLES][BT_HOT];
static struct bt_groups draft,published;
static unsigned trace_lock;
static char draft_names[BT_ROLES][32],published_names[BT_ROLES][32];
static unsigned draft_pid[BT_ROLES],published_pid[BT_ROLES],draft_slide[BT_ROLES],published_slide[BT_ROLES];
static uint64_t draft_task[BT_ROLES];
static unsigned draft_busy,published_busy,draft_omitted,published_omitted;
static struct bt_thr thr_draft[BT_ROLES][BT_MAX_THR],thr_published[BT_ROLES][BT_MAX_THR];
static unsigned thr_n_draft[BT_ROLES],thr_n_published[BT_ROLES],thr_om_draft[BT_ROLES],thr_om_published[BT_ROLES];
static struct bt_dmsg dq_draft[BT_ROLES][6],dq_published[BT_ROLES][6];
static unsigned dq_n_draft[BT_ROLES],dq_n_published[BT_ROLES],dq_flags_draft[BT_ROLES],dq_flags_published[BT_ROLES];
static struct bt_scan scan_draft,scan_published;
// The w00t port's queue across successive scans of the same space: a queue that only grows and never drains
// names a receiver that never ran, independent of any single snapshot.
static uint32_t wp_name;static unsigned wp_q,wp_samples,wp_drops,wp_grows,wp_drained;
static struct bt_deep draft_deep,published_deep;
char boot_ui_trace_rows[BT_ROWS][120],boot_ui_trace_label[120];
unsigned boot_ui_trace_ready,boot_ui_trace_groups,boot_ui_trace_threads,boot_ui_trace_pages;
volatile unsigned boot_ui_trace_phase,boot_ui_trace_index;

void boot_ui_trace_begin(void) {
 memset(draft_main,0,sizeof(draft_main));memset(hot_draft,0,sizeof(hot_draft));
 memset(&draft,0,sizeof(draft));memset(draft_pid,0,sizeof(draft_pid));memset(draft_task,0,sizeof(draft_task));
 memset(draft_slide,0,sizeof(draft_slide));memset(draft_names,0,sizeof(draft_names));
 memset(thr_draft,0,sizeof(thr_draft));memset(thr_n_draft,0,sizeof(thr_n_draft));memset(thr_om_draft,0,sizeof(thr_om_draft));
 memset(dq_draft,0,sizeof(dq_draft));memset(dq_n_draft,0,sizeof(dq_n_draft));memset(dq_flags_draft,0,sizeof(dq_flags_draft));
 memset(&scan_draft,0,sizeof(scan_draft));memset(&draft_deep,0,sizeof(draft_deep));
 draft_busy=0;draft_omitted=0;boot_ui_trace_phase=1;
}
static int read_user(void *context,uint64_t address,void *bytes,unsigned size) {
 return vm_map_read_user((vm_map_t)context,address,bytes,size)!=KERN_SUCCESS;
}
static char bt_space_char(ipc_space_t space) {
 return space==ipc_space_kernel?'K':(space==ipc_space_reply?'R':(space?'T':'-'));
}
static char bt_space_owner(ipc_space_t space) {
 if(!space)return '-';
 for(unsigned role=0;role<BT_ROLES;++role)if(role_space[role]==space)return role_char_of[role];
 return '?';
}
// Everything one port's own queue lock can say about it. klist is the decisive one: a process with no thread
// in mach_msg receive can only ever drain a port through a kqueue knote armed on its message queue, so an
// empty klist on a port that queued messages means no live receiver was ever registered for it.
struct bt_pinfo {unsigned flags,rname,kt,q,qlimit,klist,link,checkin,lflags,first_id,srights,mscount,sorights;uint64_t sem_task;int pid,sem_count;unsigned sem_wait;char state,kind,owner,lname,svc[20];};
// The port fields under its own queue lock only; the caller guarantees the port stays alive (a held
// reference, or the owning space's read lock), so no reference is taken or dropped here.
static void bt_port_fields(ipc_port_t p,struct bt_pinfo *pi) {
 memset(pi,0,sizeof(*pi));pi->pid=-1;pi->owner='-';
 if(p==IP_NULL)return;
 ip_mq_lock(p);
 pi->flags=(ip_active(p)?1:0)|(ip_in_a_space(p)?2:0)|(p->ip_specialreply?4:0)|(p->ip_service_port?8:0);
 pi->srights=p->ip_srights;pi->mscount=p->ip_mscount;pi->sorights=p->ip_sorights;
 pi->state=ip_in_a_space(p)?'s':(ip_in_transit(p)?'t':(ip_in_limbo(p)?'l':'d'));
 pi->rname=ip_get_receiver_name(p);
 pi->kt=(unsigned)ip_kotype(p);pi->q=p->ip_messages.imq_msgcount;pi->qlimit=p->ip_messages.imq_qlimit;
 pi->link=(unsigned)p->ip_sync_link_state;
 pi->checkin=p->ip_sync_bootstrap_checkin?1:0;
 pi->klist=ipc_port_has_klist(p) && SLIST_FIRST(&p->ip_klist)!=NULL;
 if(pi->q) {
  ipc_kmsg_t first=ipc_kmsg_queue_first(&p->ip_messages.imq_messages);
  if(first){const mach_msg_header_t *h=ikm_header(first);pi->first_id=(unsigned)h->msgh_id;}
 }
 if(pi->kt==IKOT_SEMAPHORE) {
  const semaphore_t sem=(semaphore_t)ipc_kobject_get_raw(p,(ipc_kobject_type_t)pi->kt);
  // Raw owner pointer only: pid_from_task would take the task lock inside the port's queue lock.
  if(sem){pi->sem_task=(uint64_t)(uintptr_t)sem->owner;pi->sem_count=sem->count;pi->sem_wait=circle_queue_empty(&sem->waitq.waitq_queue)?0:1;}
 }
 if(p->ip_service_port && p->ip_splabel) {
  const ipc_service_port_label_t label=(ipc_service_port_label_t)p->ip_splabel;
  pi->lflags=(unsigned)label->ispl_flags;pi->lname=label->ispl_launchd_name!=MACH_PORT_NULL;
  if(label->ispl_service_name)strlcpy(pi->svc,label->ispl_service_name,sizeof(pi->svc));
 }
 if(pi->rname) {
  ipc_space_t s=ip_get_receiver(p);
  pi->kind=bt_space_char(s);pi->owner=bt_space_owner(s);
  task_t t=s?s->is_task:TASK_NULL;
  pi->pid=t?task_pid(t):-1;
 }
 ip_mq_unlock(p);
}
// A port reached through another port's fields is only known to be alive, not to still be in a space: its
// receiver union may already hold the next in-transit destination. Keep a real reference, drop the source
// lock, then read the target under its own queue lock so the union is interpreted in the state it is in.
static void bt_port_probe(ipc_port_t p,struct bt_pinfo *pi) {
 if(p==IP_NULL){bt_port_fields(IP_NULL,pi);return;}
 bt_port_fields(p,pi);
 ipc_port_release(p);
}
// The headers still queued on a port reached through another port, and for each its reply port: the reply
// port's receiver space names the task that sent the message. References are taken under the queue lock,
// then each reply port is probed and released after it is dropped, so no port lock is nested in another.
static void bt_port_queue(ipc_port_t p,struct bt_dmsg *out,unsigned *n,unsigned *flags,unsigned max) {
 *n=0;*flags=0;
 if(p==IP_NULL)return;
 ip_mq_lock(p);
 *flags=(ip_active(p)?1:0)|(ip_in_a_space(p)?2:0)|(p->ip_specialreply?4:0)|(p->ip_service_port?8:0);
 ipc_kmsg_t kmsg=ipc_kmsg_queue_first(&p->ip_messages.imq_messages);
 for(unsigned i=0;kmsg && i<max;i++,kmsg=ipc_kmsg_queue_next(&p->ip_messages.imq_messages,kmsg)) {
  mach_msg_header_t *h=ikm_header(kmsg);
  out[i].local=0;out[i].bits=(unsigned)h->msgh_bits;out[i].id=(unsigned)h->msgh_id;out[i].size=(unsigned)h->msgh_size;
  out[i].l_flags=0;out[i].l_pid=-1;out[i].l_kind=0;out[i].l_state=0;out[i].l_owner=0;
  if(IP_VALID(h->msgh_local_port)){out[i].local=(uint64_t)(uintptr_t)h->msgh_local_port;ipc_port_reference(h->msgh_local_port);}
  *n=i+1;
 }
 ip_mq_unlock(p);
 for(unsigned i=0;i<*n;++i)if(out[i].local) {
  struct bt_pinfo pi;bt_port_probe((ipc_port_t)(uintptr_t)out[i].local,&pi);
  out[i].l_flags=pi.flags;out[i].l_pid=pi.pid;out[i].l_kind=pi.kind;out[i].l_state=pi.state;out[i].l_owner=pi.owner;
 }
}
// The destination and reply ports of a blocked mach_msg2 call, plus how many messages each queue holds. The
// destination also carries its state (in space / in transit to a port / in limbo), its flags, the port it is
// in transit to, an armed port-destroyed backup, its launchd service name and the headers still queued, so a
// port the kernel never drains names itself. own marks queued messages whose reply port is this thread's.
static void lookup_peer(struct bt_entry *e,ipc_space_t space,unsigned role) {
 e->peer=-1;e->peer_code=1;e->port_q=0;
 e->send_kind=0;e->send_kotype=0;e->send_qlimit=0;e->send_srights=0;e->send_mscount=0;e->send_kobj=0;e->msg_n=0;
 e->send_flags=0;e->send_rname=0;e->send_spr=~0u;e->send_pdr=0;e->arg_bits=0;e->arg_id=0;e->arg_size=0;
 e->send_klist=0;e->send_link=0;
 e->dst_flags=0;e->dst_kind=0;e->dst_rname=0;e->dst_kt=0;e->dst_q=0;e->dst_port=0;e->dst_pid=-1;
 e->dst_klist=0;e->dst_link=0;e->dst_checkin=0;e->dst_lflags=0;e->dst_lname=0;
 e->pdr_rname=0;e->pdr_port=0;e->send_state=0;e->dst_state=0;e->pdr_state=0;e->dst_owner='-';e->pdr_kind=0;e->pdr_owner='-';e->pdr_pid=-1;e->svc[0]=0;
 for(unsigned i=0;i<6;++i)e->msg_pid[i]=-1;
 if(e->trap!=UINT64_MAX-46)return;
 e->peer_code=2;if(!(unsigned)e->regs[3])return;
 e->peer_code=3;if(!space)return;
 ipc_port_t port=IP_NULL;e->peer_code=4;
 if(ipc_port_translate_send(space,(unsigned)e->regs[3],&port)!=KERN_SUCCESS)return;
 if(e->recv_sync_port)e->recv_sync_match=(e->recv_sync_port==(uint64_t)(uintptr_t)port)?'+':'-';
 ipc_space_t receiver=ip_get_receiver(port);
 e->send_kind=receiver==ipc_space_kernel?1:(receiver==ipc_space_reply?2:(receiver?3:4));
 e->peer=ipc_port_get_receiver_task_locked(port,NULL);
 e->send_kotype=(unsigned)ip_kotype(port);
 if(e->send_kotype!=IKOT_NONE)e->send_kobj=(uint64_t)(uintptr_t)ipc_kobject_get_raw(port,(ipc_kobject_type_t)e->send_kotype);
 e->port_q=port->ip_messages.imq_msgcount;e->send_qlimit=port->ip_messages.imq_qlimit; e->send_srights=port->ip_srights;e->send_mscount=port->ip_mscount;
 e->send_flags=(ip_active(port)?1:0)|(ip_in_a_space(port)?2:0)|(port->ip_specialreply?4:0)|(port->ip_service_port?8:0)|
  (port->ip_tempowner?16:0)|(port->ip_has_watchport?32:0)|(port->ip_immovable_receive?64:0)|(port->ip_pinned?128:0)|
  (port->ip_guarded?256:0)|(port->ip_no_grant?512:0)|(port->ip_splabel?1024:0);
 e->send_rname=ip_get_receiver_name(port);
 e->send_state=ip_in_a_space(port)?'s':(ip_in_transit(port)?'t':(ip_in_limbo(port)?'l':'d'));
 e->send_spr=port->ip_specialreply?(unsigned)port->ip_pid:~0u;
 e->send_klist=ipc_port_has_klist(port) && SLIST_FIRST(&port->ip_klist)!=NULL;
 e->send_link=(unsigned)port->ip_sync_link_state;
 e->arg_bits=(unsigned)e->regs[2];e->arg_size=(unsigned)(e->regs[2]>>32);e->arg_id=(unsigned)(e->regs[4]>>32);
 ipc_port_t backup=IP_NULL,dst=IP_NULL;
 if(!port->ip_specialreply && !port->ip_has_watchport) {
  backup=port->ip_pdrequest;
  if(backup!=IP_NULL){e->send_pdr=1;e->pdr_port=(uint64_t)(uintptr_t)backup;ipc_port_reference(backup);}
 }
 dst=ip_get_destination(port);
 if(dst!=IP_NULL){e->dst_port=(uint64_t)(uintptr_t)dst;ipc_port_reference(dst);}
#if CONFIG_SERVICE_PORT_INFO
 if(port->ip_service_port && port->ip_splabel) {
  const ipc_service_port_label_t label=(ipc_service_port_label_t)port->ip_splabel;
  if(label->ispl_service_name)strlcpy(e->svc,label->ispl_service_name,sizeof(e->svc));
 }
#endif
 // Reply ports of the queued headers: each one's receiver is the task that sent that header. They are
 // referenced under the queue lock, then probed after it is dropped, so no port lock nests in another.
 ipc_port_t mreply[6]={IP_NULL,IP_NULL,IP_NULL,IP_NULL,IP_NULL,IP_NULL};
 ipc_kmsg_t kmsg=ipc_kmsg_queue_first(&port->ip_messages.imq_messages);
 for(unsigned n=0;kmsg && n<6;n++,kmsg=ipc_kmsg_queue_next(&port->ip_messages.imq_messages,kmsg)) {
  mach_msg_header_t *h=ikm_header(kmsg);
  e->msgs[n].bits=(unsigned)h->msgh_bits;e->msgs[n].id=(unsigned)h->msgh_id;e->msgs[n].size=(unsigned)h->msgh_size;
  e->msgs[n].own=(e->recv_port && (uint64_t)(uintptr_t)h->msgh_local_port==e->recv_port);
  if(IP_VALID(h->msgh_local_port)){mreply[n]=h->msgh_local_port;ipc_port_reference(mreply[n]);}
  e->msg_n=n+1;
 }
 e->peer_code=(e->regs[1]&1)?0:5;ip_mq_unlock(port);
 {
  struct bt_pinfo pi;
  if(e->index==1)bt_port_queue(dst,dq_draft[role],&dq_n_draft[role],&dq_flags_draft[role],6);
  bt_port_probe(dst,&pi);
  e->dst_flags=pi.flags;e->dst_rname=pi.rname;e->dst_kt=pi.kt;e->dst_q=pi.q;e->dst_pid=pi.pid;e->dst_state=pi.state;
  e->dst_kind=(unsigned char)pi.kind;e->dst_owner=pi.owner;
  e->dst_klist=pi.klist;e->dst_link=pi.link;e->dst_checkin=pi.checkin;e->dst_lflags=pi.lflags;e->dst_lname=pi.lname;
  bt_port_probe(backup,&pi);
  e->pdr_rname=pi.rname;e->pdr_pid=pi.pid;e->pdr_state=pi.state;e->pdr_kind=pi.kind;e->pdr_owner=pi.owner;
  for(unsigned i=0;i<e->msg_n;++i)if(mreply[i]!=IP_NULL){bt_port_probe(mreply[i],&pi);e->msg_pid[i]=pi.pid;}
 }
}
static void lookup_recv(struct bt_entry *e,ipc_space_t space) {
 e->recv_q=0;e->recv_qlimit=0;e->recv_pid=-1;e->recv_port=0;e->recv_flags=0;e->recv_srpid=~0u;
 e->recv_sync_port=0;e->recv_link=0;e->recv_sync_match=0;
 if(!space)return;
 unsigned name=0;
 if(e->trap==UINT64_MAX-46)name=(unsigned)(e->regs[5]>>32);
 // mach_msg_trap/mach_msg_overwrite_trap carry the receive name in x4; only when MACH_RCV_MSG (2) is asked.
 else if((e->trap==UINT64_MAX-31 || e->trap==UINT64_MAX-32) && (e->regs[1]&2))name=(unsigned)e->regs[4];
 if(!name)return;
 ipc_port_t port=IP_NULL;
 if(ipc_port_translate_receive(space,(mach_port_name_t)name,&port)!=KERN_SUCCESS)return;
 e->recv_q=port->ip_messages.imq_msgcount;e->recv_qlimit=port->ip_messages.imq_qlimit;
 e->recv_flags=(ip_active(port)?1:0)|(ip_in_a_space(port)?2:0)|(port->ip_specialreply?4:0)|(port->ip_service_port?8:0);
 // A special reply port names the server pid its owner is blocked on; other ports leave it unset.
 e->recv_srpid=port->ip_specialreply?(unsigned)ipc_special_reply_get_pid_locked(port):~0u;
 e->recv_pid=ipc_port_get_receiver_task_locked(port,NULL);
 // The linked inheritor port is only this union member while the link state says so; keep the value to
 // compare against the sync port the same trap passes in x3.
 e->recv_link=(unsigned)port->ip_sync_link_state;
 if(port->ip_sync_link_state==PORT_SYNC_LINK_PORT && port->ip_sync_inheritor_port)
  e->recv_sync_port=(uint64_t)(uintptr_t)port->ip_sync_inheritor_port;
 e->recv_port=(uint64_t)(uintptr_t)port;
 ip_mq_unlock(port);
}
// The traced launchd's own IPC space, walked under its read lock exactly like mach_port_names: every entry
// with a right is counted, and each receive right is probed under that same lock, so the port cannot go away
// between the lookup and the probe and this needs no reference. The six most significant ports are kept
// (queued messages, an armed knote, a special reply, a service label): a port that queued messages with no
// knote and no receiving thread is one nobody will ever drain.
static void bt_space_scan(ipc_space_t space,uint32_t sem_name) {
 memset(&scan_draft,0,sizeof(scan_draft));
#if CONFIG_SERVICE_PORT_INFO
 scan_draft.cv=1;
#endif
 if(!space)return;
 is_read_lock(space);
 if(is_active(space)) {
  const ipc_entry_table_t table=is_active_table(space);
  const ipc_entry_num_t count=ipc_entry_table_count(table);
  for(ipc_entry_num_t index=1;index<count && index<16384;++index) {
   ipc_entry_t entry=ipc_entry_table_get_nocheck(table,index);
   const ipc_entry_bits_t bits=entry->ie_bits;
   if(IE_BITS_TYPE(bits)==MACH_PORT_TYPE_NONE)continue;
   ++scan_draft.e;
   const mach_port_name_t full=MACH_PORT_MAKE(index,IE_BITS_GEN(bits));
   if(!entry->ie_object)continue;
   if(!(bits&MACH_PORT_TYPE_RECEIVE)) {
    if(sem_name && full==sem_name) {
     struct bt_pinfo pi;bt_port_fields(ip_object_to_port(entry->ie_object),&pi);
     struct bt_sport *m=&scan_draft.mport;memset(m,0,sizeof(*m));
     int sem_owner=-1;
     if(pi.sem_task)for(unsigned r=0;r<BT_ROLES;++r)if(draft_task[r]==pi.sem_task){sem_owner=(int)draft_pid[r];break;}
     m->name=full;m->q=pi.q;m->qlimit=pi.qlimit;m->flags=pi.flags;m->state=pi.state;m->kt=pi.kt;
     m->kind=pi.kind;m->owner=pi.owner;m->pid=pi.pid;m->sem_owner=sem_owner;m->sem_count=pi.sem_count;
    }
    continue;
   }
   struct bt_pinfo pi;bt_port_fields(ip_object_to_port(entry->ie_object),&pi);
   // The w00t port is found by what it queued, not by a name that changes every boot.
   if(pi.q && pi.first_id==0x77303074u) {
    ++scan_draft.wq;
    if(!scan_draft.wport.name) {
     struct bt_sport *w=&scan_draft.wport;memset(w,0,sizeof(*w));
     w->name=full;w->q=pi.q;w->qlimit=pi.qlimit;w->flags=pi.flags;w->state=pi.state;w->kl=pi.klist;w->link=pi.link;
     w->kt=pi.kt;w->first_id=pi.first_id;w->kind=pi.kind;w->owner=pi.owner;w->pid=pi.pid;
    }
   }
   ++scan_draft.r;
   scan_draft.q+=pi.q>0;scan_draft.kl+=pi.klist!=0;scan_draft.wl+=pi.link==PORT_SYNC_LINK_WORKLOOP_KNOTE;scan_draft.sr+=(pi.flags&4)!=0;scan_draft.sv+=(pi.flags&8)!=0;
   // The whole set, not only the scored top six: a truncated list cannot say what the rest of the queued
   // ports did with their knotes.
   if(pi.q && scan_draft.qn<BT_QP) {
    struct bt_sport *q=&scan_draft.qp[scan_draft.qn++];memset(q,0,sizeof(*q));
    q->name=full;q->q=pi.q;q->qlimit=pi.qlimit;q->flags=pi.flags;q->state=pi.state;q->kl=pi.klist;q->link=pi.link;
    q->checkin=pi.checkin;q->lflags=pi.lflags;q->lname=pi.lname;q->kt=pi.kt;q->pid=pi.pid;
    strlcpy(q->svc,pi.svc,sizeof(q->svc));
   }
   const unsigned score=(pi.q?8:0)+(pi.klist?4:0)+(pi.link==PORT_SYNC_LINK_WORKLOOP_KNOTE?4:0)+((pi.flags&4)?2:0)+((pi.flags&8)?1:0);
   if(!score)continue;
   unsigned at=scan_draft.n;
   for(unsigned k=0;k<scan_draft.n;++k)if(score>scan_draft.ports[k].score){at=k;break;}
   if(at>=BT_SCAN)continue;
   const unsigned last=scan_draft.n<BT_SCAN?scan_draft.n:BT_SCAN-1;
   for(unsigned k=last;k>at;--k)scan_draft.ports[k]=scan_draft.ports[k-1];
   struct bt_sport *p=&scan_draft.ports[at];memset(p,0,sizeof(*p));
   p->score=score;p->name=MACH_PORT_MAKE(index,IE_BITS_GEN(bits));
   p->q=pi.q;p->qlimit=pi.qlimit;p->flags=pi.flags;p->state=pi.state;p->kl=pi.klist;p->link=pi.link;
   p->checkin=pi.checkin;p->lflags=pi.lflags;p->lname=pi.lname;p->kt=pi.kt;
   if(scan_draft.n<BT_SCAN)++scan_draft.n;
  }
  if(scan_draft.wport.name) {
   if(wp_name==scan_draft.wport.name){++wp_samples;if(scan_draft.wport.q<wp_q)++wp_drops;else if(scan_draft.wport.q>wp_q)++wp_grows;}
   else {wp_name=scan_draft.wport.name;wp_samples=0;wp_drops=0;wp_grows=0;}
   wp_q=scan_draft.wport.q;
  } else if(wp_name){++wp_drained;wp_name=0;wp_q=0;}
  scan_draft.wps=wp_samples;scan_draft.wpd=wp_drops;scan_draft.wpg=wp_grows;scan_draft.wpr=wp_drained;
 }
 is_read_unlock(space);
}
// A bounded user string: the caller only needs a path tail, and a fault past the mapping is a read failure,
// not a partially decoded name.
static int bt_user_text(vm_map_t map,uint64_t address,char *out,unsigned size) {
 out[0]=0;
 for(unsigned i=0;i+1<size;++i) {
  char c=0;
  if(read_user(map,address+i,&c,1))return 1;
  if(!c)return 0;
  out[i]=c;out[i+1]=0;
 }
 return 0;
}
// dyld_all_image_infos: {version,count} in the first word, the image array in the second. imageArray[0] is what
// the main rows print as image=; the /sbin/launchd entry is found by path, so the shared-cache images that may
// precede it are never taken for the process image. Cache images are skipped by address, so only the few images
// outside the shared cache are opened.
static void bt_deep_images(vm_map_t map,uint64_t images,unsigned slide,struct bt_deep *d) {
 memset(d,0,sizeof(*d));d->img_error=1;
 uint64_t info[2]={0};
 if(!images || !bt_user(images,sizeof(info)) || read_user(map,images,info,sizeof(info)))return;
 const unsigned count=(unsigned)(info[0]>>32);
 d->version=(unsigned)info[0];d->img_array=info[1];
 if(!count || !info[1]){d->img_error=2;return;}
 d->img_count=count;
 const uint64_t cache_lo=UINT64_C(0x180000000)+slide,cache_hi=cache_lo+UINT64_C(0x80000000);
 for(unsigned i=0;i<count && i<4096;++i) {
  uint64_t ent[3]={0};
  if(read_user(map,info[1]+(uint64_t)i*sizeof(ent),ent,sizeof(ent))){d->img_error=3;break;}
  if(!i){d->img0=ent[0];bt_user_text(map,ent[1],d->img0_path,sizeof(d->img0_path));}
  if(ent[0]>=cache_lo && ent[0]<cache_hi)continue;
  char head[8]={0};
  if(!bt_user(ent[1],sizeof(head)) || read_user(map,ent[1],head,sizeof(head)))continue;
  if(memcmp(head,"/sbin/la",8))continue;
  if(bt_user_text(map,ent[1],d->img_path,sizeof(d->img_path)))continue;
  if(strncmp(d->img_path,"/sbin/launchd",13)){d->img_path[0]=0;continue;}
  d->img_base=ent[0];d->img_index=i;d->img_match=1;break;
 }
}
void boot_ui_trace_task(task_t task,unsigned index,int pid,const char *name,unsigned slide) {
 if(!ml_get_interrupts_enabled()){++draft_busy;return;}
 if(index<1 || index>BT_ROLES)return;
 unsigned role=index-1;
 draft_pid[role]=(unsigned)pid;draft_slide[role]=slide;draft_task[role]=(uint64_t)(uintptr_t)task;
 strlcpy(draft_names[role],name,sizeof(draft_names[role]));
 if(!task_lock_try(task)){++draft_busy;return;}
 vm_map_t map=task->map;
 if(!map){task_unlock(task);++draft_busy;return;}
 vm_map_reference(map);
 ipc_space_t space=task->itk_space;if(space)is_reference(space);
 uint64_t sem_arg=0;
 const char rc=role?"BSTU"[role&3]:(!strcmp(name,"chronod")?'C':(!strcmp(name,"liveactivitiesd")?'L':'B'));
 role_char_of[role]=rc;if(space)role_space[role]=space;
 uint64_t images=task_get_all_image_info_addr(task);
 unsigned count=0,visited=0;thread_t thread;
 queue_iterate(&task->threads,thread,thread_t,task_threads) {
  if(visited++>=BT_MAX_THREADS){draft_omitted+=(unsigned)task->thread_count-BT_MAX_THREADS;break;}
  bool irq=ml_set_interrupts_enabled(false);
  if(!simple_lock_try(&thread->sched_lock,&thread_lck_grp)){++draft_busy;ml_set_interrupts_enabled(irq);continue;}
  arm_saved_state_t *state=thread->machine.upcb;
  if(state && is_saved_state64(state)) {
   struct bt_entry *e=&samples[count];memset(e,0,sizeof(*e));sample_tid[count]=thread->thread_id;sample_cs[count]=thread->c_switch;bt_kernel_stack(thread,sample_k[count]);sample_event[count]=(uint64_t)thread->wait_event;sample_state[count]=(unsigned)thread->state;sample_hint[count]=(unsigned)thread->block_hint;sample_pri[count]=thread->sched_pri;++count;
   e->index=visited;e->role=rc;
   e->pc=(uint64_t)get_saved_state_pc(state);e->lr=(uint64_t)get_saved_state_lr(state);
   e->fp=(uint64_t)get_saved_state_fp(state);
   e->trap=(uint64_t)get_saved_state_reg(state,16);
   if(role==2 && e->trap==UINT64_MAX-35)sem_arg=(uint64_t)get_saved_state_reg(state,19);
   for(unsigned reg=0;reg<8;++reg)e->regs[reg]=(uint64_t)get_saved_state_reg(state,reg);
   if((e->pc && !bt_user(e->pc,4)) || (e->lr && !bt_user(e->lr,4)))e->error=2;
   if(thr_n_draft[role]<BT_MAX_THR) {
    struct bt_thr *t=&thr_draft[role][thr_n_draft[role]++];memset(t,0,sizeof(*t));
    t->tid=thread->thread_id;t->index=visited;t->trap=e->trap;t->pc=e->pc;t->lr=e->lr;
    t->event=sample_event[count-1];t->kstack=sample_k[count-1][0];t->state=sample_state[count-1];t->hint=sample_hint[count-1];t->pri=sample_pri[count-1];
    t->cont=(uint64_t)thread->continuation;t->fp=e->fp;
    bt_kernel_raw(thread,t);
    for(unsigned r=0;r<8;++r)t->regs[r]=(unsigned)e->regs[r];
   } else ++thr_om_draft[role];
  }
  thread_unlock(thread);ml_set_interrupts_enabled(irq);
 }
 task_unlock(task);
 // No thread pointers or suspended threads survive collection. The copied FP
 // can race with a running task, so malformed/changed chains are reported.
 for(unsigned i=0;i<count;++i) {
  boot_ui_trace_phase=role?5:4;boot_ui_trace_index=samples[i].index;
  lookup_recv(&samples[i],space);
  lookup_peer(&samples[i],space,role);
  boot_ui_trace_phase=role?3:2;
  if(samples[i].index==1) {
   struct bt_main *main=&draft_main[role];main->entry=samples[i];main->pid=pid;main->slide=slide;
   if(samples[i].port_q>main->q_max)main->q_max=samples[i].port_q;
   if(!main->entry.error)bt_walk_n(&main->entry,read_user,map,main->frames,32);
   main->error=main->entry.error;
   uint64_t info[2]={0},image[3]={0};main->image_error=1;
   if(bt_user(images,sizeof(info)) && !read_user(map,images,info,sizeof(info)) &&
      (unsigned)(info[0]>>32) && bt_user(info[1],sizeof(image)) &&
      !read_user(map,info[1],image,sizeof(image)) && bt_user(image[0],32)) {
    main->image=image[0];main->image_error=0;
   }
  }
  if(!samples[i].error)bt_walk(&samples[i],read_user,map);
  if(role==2) {
   for(unsigned j=0;j<thr_n_draft[2];++j)if(thr_draft[2][j].index==samples[i].index) {
    struct bt_entry deep=samples[i];deep.error=0;
    bt_walk_n(&deep,read_user,map,thr_draft[2][j].frames,BT_FRAMES);
   }
  }
  bt_add(&draft,&samples[i]);
 }
 {
  // Rates against the previous snapshot of the same thread; the four hottest of the role are kept with their stacks.
  const uint64_t now=mach_absolute_time();
  for(unsigned i=0;i<count;++i) {
   struct bt_prev *slot=NULL,*free_slot=NULL;
   for(unsigned k=0;k<BT_MAX_THREADS;++k) {
    if(hot_prev[role][k].tid==sample_tid[i]){slot=&hot_prev[role][k];break;}
    if(!free_slot && !hot_prev[role][k].tid)free_slot=&hot_prev[role][k];
   }
   if(slot && now>slot->at) {
    uint64_t ns=0;absolutetime_to_nanoseconds(now-slot->at,&ns);
    const uint32_t switches=sample_cs[i]-slot->cs;
    const unsigned rate=ns?(unsigned)((uint64_t)switches*UINT64_C(1000000000)/ns):0;
    if(rate>=BT_HOT_MIN_RATE) {
     unsigned at=BT_HOT;
     for(unsigned h=0;h<BT_HOT;++h)if(rate>hot_draft[role][h].rate){at=h;break;}
     if(at<BT_HOT) {
      for(unsigned h=BT_HOT-1;h>at;--h)hot_draft[role][h]=hot_draft[role][h-1];
      struct bt_hotrow *row=&hot_draft[role][at];memset(row,0,sizeof(*row));
      row->rate=rate;row->role=samples[i].role;row->peer=samples[i].peer;row->tid=sample_tid[i];
      row->pc=samples[i].pc;row->lr=samples[i].lr;
      memcpy(row->kernel,sample_k[i],sizeof(row->kernel));row->event=sample_event[i];row->state=sample_state[i];row->hint=sample_hint[i];row->pri=sample_pri[i];
      {struct bt_entry deep=samples[i];deep.error=0;bt_walk_n(&deep,read_user,map,row->frames,BT_HOT_FRAMES);}   /* deeper than the grouped 8 frames: names the thread */
     }
    }
   }
   if(!slot)slot=free_slot?free_slot:&hot_prev[role][i%BT_MAX_THREADS];
   slot->tid=sample_tid[i];slot->cs=sample_cs[i];slot->at=now;
  }
 }
 // launchd's own space is scanned once per pass, while this task's reference is still held.
 if(space && role==2 && !strcmp(name,"launchd")){
  uint32_t sem_name=0;
  for(unsigned i=0;i<thr_n_draft[2];++i)if(thr_draft[2][i].trap==UINT64_MAX-35)sem_name=(uint32_t)thr_draft[2][i].regs[0];
  bt_space_scan(space,sem_name);
  bt_deep_images(map,images,slide,&draft_deep);
  // The wait argument is the launch diagnostic thread's own struct: +8 type, +0xc the pthread name
  // ("<service> throttled"), so the throttled service names itself without any launchd-side instrumentation.
  if(sem_arg){
   unsigned type=0;
   if(!read_user(map,sem_arg+0x8,&type,sizeof(type)))draft_deep.thr_type=type;
   (void)bt_user_text(map,sem_arg+0xc,draft_deep.thr_name,sizeof(draft_deep.thr_name));
  }
 }
 if(space)is_release(space);
 vm_map_deallocate(map);boot_ui_trace_phase=1;
}
void boot_ui_trace_publish(void) {
 if(!__atomic_exchange_n(&trace_lock,1,__ATOMIC_ACQUIRE)) {
  published=draft;memcpy(published_main,draft_main,sizeof(draft_main));memcpy(hot_published,hot_draft,sizeof(hot_draft));memcpy(published_pid,draft_pid,sizeof(draft_pid));
  memcpy(thr_published,thr_draft,sizeof(thr_draft));memcpy(thr_n_published,thr_n_draft,sizeof(thr_n_draft));memcpy(thr_om_published,thr_om_draft,sizeof(thr_om_draft));
  memcpy(dq_published,dq_draft,sizeof(dq_draft));memcpy(dq_n_published,dq_n_draft,sizeof(dq_n_draft));memcpy(dq_flags_published,dq_flags_draft,sizeof(dq_flags_draft));
  scan_published=scan_draft;published_deep=draft_deep;
  for(unsigned r=0;r<BT_ROLES;++r){if(draft_main[r].q_max>qmax_persist[r])qmax_persist[r]=draft_main[r].q_max;published_main[r].q_max=qmax_persist[r];}
  memcpy(published_slide,draft_slide,sizeof(draft_slide));memcpy(published_names,draft_names,sizeof(draft_names));
  published_busy=draft_busy;published_omitted=draft_omitted;
  __atomic_store_n(&trace_lock,0,__ATOMIC_RELEASE);
 }
 boot_ui_trace_phase=0;
}
// The live banner's view of one traced role (1 = the boot-ui-focus2 process, SpringBoard): with detail, the main thread's saved registers and
// its first 24 frame-pointer return addresses, then every other thread group; without, a name row and every group including the main
// thread's. A group row is its Mach peer (pid of the receiver it sends to, -1 none), PC, LR and first 7 frames; groups without a user frame
// chain (idle workqueue threads) are left out. Raw hex user addresses; cache+ is the shared cache slide and image the main executable's
// header. Returns the number of rows written (0 while the trace is being published).
unsigned boot_ui_trace_role_rows(unsigned role,char rows[][120],unsigned max,unsigned detail) {
 if(role>=BT_ROLES || max<6 || __atomic_exchange_n(&trace_lock,1,__ATOMIC_ACQUIRE))return 0;
 unsigned n=0;const struct bt_main *m=&published_main[role];const struct bt_entry *e=&m->entry;const char c=bt_role_char(role,m);
 if(m->pid && detail) {
  snprintf(rows[n++],120,"STK%c p%d cache+%x image=%llx pc=%llx lr=%llx fperr=%u peer=%d/e%u",c,m->pid,m->slide,m->image,e->pc,e->lr,m->error,
   e->peer,e->peer_code);
  snprintf(rows[n++],120,"STK%c trap=%lld x0=%llx x1=%llx x2=%llx x3=%llx x4=%llx",c,(long long)e->trap,e->regs[0],e->regs[1],e->regs[2],
   e->regs[3],e->regs[4]);
  for(unsigned row=0;row<3;++row) {
   int at=snprintf(rows[n],120,"STK%c %02u",c,row*8+1);
   for(unsigned i=0;i<8 && at>0 && at<119;++i)at+=snprintf(rows[n]+at,120-at," %llx",m->frames[row*8+i]);
   ++n;
  }
 } else if(published_pid[role]) {
  snprintf(rows[n++],120,"STK%c %.31s p%u cache+%x image=%llx",c,published_names[role],published_pid[role],published_slide[role],m->image);
 }
 for(unsigned g=0;g<published.count && n<max;++g) {
  const struct bt_entry *t=&published.entries[g];
  if(t->role!=c || (detail && t->index==1 && t->count==1) || !t->frames[0])continue;   // a main group of several threads names the others
  int at=snprintf(rows[n],120,"STK%c#%u x%u p%d %llx %llx",t->role,t->index,t->count,t->peer,t->pc,t->lr);
  for(unsigned i=0;i<7 && at>0 && at<119;++i)at+=snprintf(rows[n]+at,120-at," %llx",t->frames[i]);
  ++n;
 }
 __atomic_store_n(&trace_lock,0,__ATOMIC_RELEASE);
 return n;
}
// The hottest threads of every traced role (context switches per second between the last two snapshots, at least BT_HOT_MIN_RATE): rate,
// thread id, Mach peer, PC, LR and the first frames. Returns the number of rows (0 while the trace is being published).
unsigned boot_ui_trace_hot_rows(char rows[][120],unsigned max) {
 if(__atomic_exchange_n(&trace_lock,1,__ATOMIC_ACQUIRE))return 0;
 unsigned n=0;
 for(unsigned role=0;role<BT_ROLES;++role)
  for(unsigned h=0;h<BT_HOT && n<max;++h) {
   const struct bt_hotrow *r=&hot_published[role][h];if(!r->rate)continue;
   int at=snprintf(rows[n],120,"HOT%c p%u tid=%llx sw/s=%u peer=%d pc=%llx lr=%llx",r->role?r->role:'?',published_pid[role],r->tid,r->rate,r->peer,r->pc,r->lr);
   for(unsigned i=0;i<4 && at>0 && at<119;++i)at+=snprintf(rows[n]+at,120-at," %llx",r->frames[i]);
   ++n;
   if(n<max) {
    int more=snprintf(rows[n],120,"HOT+ %llx",r->tid);
    for(unsigned i=4;i<BT_HOT_FRAMES && more>0 && more<110;++i)more+=snprintf(rows[n]+more,120-more," %llx",r->frames[i]);
    ++n;
   }
   if(n<max && r->kernel[0]) {
    int kernel=snprintf(rows[n],120,"HOTK %llx st=%x hint=%u pri=%d ev=%llx",r->tid,r->state,r->hint,r->pri,r->event);
    for(unsigned i=0;i<BT_KFRAMES && kernel>0 && kernel<105;++i)kernel+=snprintf(rows[n]+kernel,120-kernel," %llx",r->kernel[i]);
    ++n;
   }
  }
 __atomic_store_n(&trace_lock,0,__ATOMIC_RELEASE);
 return n;
}
// The process a traced role is waiting on: the first of its thread groups that sends a Mach message to another task (peer above launchd,
// not itself). 0 = none, -1 = the trace is being published (keep the previous answer).
int boot_ui_trace_role_peer(unsigned role) {
 if(role>=BT_ROLES || __atomic_exchange_n(&trace_lock,1,__ATOMIC_ACQUIRE))return -1;
 const char c=bt_role_char(role,&published_main[role]);int own=(int)published_pid[role],found=0;
 for(unsigned g=0;g<published.count && !found;++g) {
  const struct bt_entry *t=&published.entries[g];
  if(t->role==c && t->peer>1 && t->peer!=own && !t->peer_code)found=t->peer;
 }
 __atomic_store_n(&trace_lock,0,__ATOMIC_RELEASE);
 return own?found:0;
}
// The lower half of the second main page: every thread of the traced launchd, one row each, so the seventh
// thread is not lost to a header row. t is the syscall it is parked in, ev the wait event, pc the user PC,
// klr the kernel link register it sleeps at, x0 the trap's first argument (0x100 = WQOPS_THREAD_WORKLOOP_RETURN
// for workq_kernreturn), k0=1 the proof that the thread blocked on a continuation with nothing saved.
static void bt_thr_rows(unsigned role,unsigned row0) {
 if(role>=BT_ROLES || !thr_n_published[role])return;
 const char c=bt_role_char(role,&published_main[role]);
 for(unsigned i=0;i<thr_n_published[role] && row0+i<BT_ROWS;++i) {
  const struct bt_thr *t=&thr_published[role][i];
  snprintf(boot_ui_trace_rows[row0+i],120,"THR%c%02u t=%llx st=%x h=%u ev=%llx ct=%llx pc=%llx x0=%x k0=%u",
   c,t->index,(unsigned long long)t->trap,t->state,t->hint,(unsigned long long)t->event,
   (unsigned long long)t->cont,(unsigned long long)t->pc,t->regs[0],t->kzero);
 }
 if(role!=2)return;
 const struct bt_thr *sem=NULL;
 for(unsigned i=0;i<thr_n_published[role];++i)if(thr_published[role][i].trap==UINT64_MAX-35)sem=&thr_published[role][i];
 if(!sem)sem=&thr_published[role][thr_n_published[role]-1];
 const unsigned row=row0+thr_n_published[role];
 if(!sem || row+1>=BT_ROWS)return;
 char a[8][9];for(unsigned i=0;i<8;++i)bt_address(a[i],sem->frames[i]);
 snprintf(boot_ui_trace_rows[row],120,"THR%c%02u S1 %s %s %s %s",c,sem->index,a[0],a[1],a[2],a[3]);
 snprintf(boot_ui_trace_rows[row+1],120,"THR%c%02u S2 %s %s %s %s",c,sem->index,a[4],a[5],a[6],a[7]);
}
// One page for the traced launchd's whole thread set: trap, state, continuation, user PC, kernel stack base and
// the eight user frame pointers each thread had when sampled. Two rows per thread keep the frame chain intact
// for the sigsuspend threads (dispatch_main vs manager) and for the parked throttling thread.
static void bt_thr_stack_rows(unsigned role,unsigned row0) {
 if(role>=BT_ROLES || !thr_n_published[role])return;
 const char c=bt_role_char(role,&published_main[role]);
 unsigned row=row0;
 for(unsigned i=0;i<thr_n_published[role] && row+1<BT_ROWS;++i,row+=2) {
  const struct bt_thr *t=&thr_published[role][i];
  char a[8][9];for(unsigned k=0;k<8;++k)bt_address(a[k],t->frames[k]);
  snprintf(boot_ui_trace_rows[row],120,"THS%c%02u t=%llx st=%x ct=%llx pc=%llx k=%llx",
   c,t->index,(unsigned long long)t->trap,t->state,(unsigned long long)t->cont,(unsigned long long)t->pc,(unsigned long long)t->kstack);
  snprintf(boot_ui_trace_rows[row+1],120,"THS%c%02u %s %s %s %s %s %s %s %s",
   c,t->index,a[0],a[1],a[2],a[3],a[4],a[5],a[6],a[7]);
 }
 if(row<BT_ROWS)snprintf(boot_ui_trace_rows[row],120,"USB ph=%d res=%d",fuxi_usbdbg_phase,fuxi_usbdbg_result);
}
static void bt_dq_rows(unsigned role,unsigned row0) {
 if(role>=BT_ROLES)return;
 snprintf(boot_ui_trace_rows[row0],120,"DQ%c dst=%llx fl=%x n%u l=reply port; its receiver is the sender",
  bt_role_char(role,&published_main[role]),(unsigned long long)published_main[role].entry.dst_port,dq_flags_published[role],dq_n_published[role]);
 for(unsigned i=0;i<dq_n_published[role] && row0+1+i<BT_ROWS;++i) {
  const struct bt_dmsg *m=&dq_published[role][i];
  snprintf(boot_ui_trace_rows[row0+1+i],120,"DQ m%u b=%x i=%x s=%u l=%llx lf=%x lk=%c ls=%c lp=%d lo=%c",
   i,m->bits,m->id,m->size,(unsigned long long)m->local,m->l_flags,
   m->l_kind?m->l_kind:'-',m->l_state?m->l_state:'-',m->l_pid,m->l_owner?m->l_owner:'-');
 }
}
void boot_ui_trace_snapshot(unsigned seconds) {
 if(__atomic_exchange_n(&trace_lock,1,__ATOMIC_ACQUIRE))return;
 unsigned groups_pages=(published.count+BT_ROWS-1)/BT_ROWS;if(!groups_pages)groups_pages=1;
 unsigned main_pages=0;
 if(published_main[0].pid || published_main[1].pid)main_pages=1;
 if(published_main[2].pid || published_main[3].pid)main_pages=2;
 unsigned port_pages=main_pages;
 unsigned deep_pages=published_main[2].pid?1:0;
 unsigned thr_pages=deep_pages;
 unsigned pages=groups_pages+main_pages+port_pages+thr_pages+deep_pages;
 unsigned page=(seconds/8)%pages;
 if(page>=groups_pages+main_pages+port_pages+thr_pages) {
  const struct bt_thr *sem=NULL;
  for(unsigned i=0;i<thr_n_published[2];++i)if(thr_published[2][i].trap==UINT64_MAX-35)sem=&thr_published[2][i];
  if(!sem && thr_n_published[2])sem=&thr_published[2][thr_n_published[2]-1];
  bt_deep_rows(bt_role_char(2,&published_main[2]),&published_deep,&scan_published,sem,boot_ui_trace_rows);
 } else if(page>=groups_pages+main_pages+port_pages) {
  memset(boot_ui_trace_rows,0,sizeof(boot_ui_trace_rows));
  bt_thr_stack_rows(2,0);
 } else if(page>=groups_pages+main_pages) {
  const unsigned first=(page-groups_pages-main_pages)*2;
  bt_port_rows(published_main,first,boot_ui_trace_rows);
  if(first==2) {
   bt_scan_rows(bt_role_char(2,&published_main[2]),&scan_published,boot_ui_trace_rows,0);
   if(!published_main[3].pid)bt_dq_rows(0,7);
  }
 } else if(page>=groups_pages) {
  const unsigned first=(page-groups_pages)*2;
  bt_main_rows(published_main,first,boot_ui_trace_rows);
  if(first==2 && !published_main[3].pid)bt_thr_rows(2,7);
 } else bt_rows(&published,page,boot_ui_trace_rows);
 const char *tail=page<groups_pages?"PC/LR/FP8; last=PORTS32":(page<groups_pages+main_pages?"MAIN32":
  (page<groups_pages+main_pages+port_pages?
   ((page-groups_pages-main_pages)*2==2?"LSCAN/DQ":"PORTS32"):
   (page<groups_pages+main_pages+port_pages+thr_pages?"THRSTK":"IMG/SEM/SPQ")));
 snprintf(boot_ui_trace_label,sizeof(boot_ui_trace_label),
  "STACK64 %u/%u %c=p%u+%x S=p%u+%x T=p%u U=p%u T%u G%u b%u o%u rd%u t%u/%u %s",
  page+1,pages,!strcmp(published_names[0],"chronod")?'C':(!strcmp(published_names[0],"liveactivitiesd")?'L':'B'),published_pid[0],published_slide[0],published_pid[1],published_slide[1],published_pid[2],published_pid[3],
  published.threads,published.count,published_busy,published_omitted,published.read_failures,
  thr_n_published[2],thr_om_published[2],tail);
 boot_ui_trace_ready=published.threads!=0;boot_ui_trace_groups=published.count;
 boot_ui_trace_threads=published.threads;boot_ui_trace_pages=pages;
 __atomic_store_n(&trace_lock,0,__ATOMIC_RELEASE);
}
