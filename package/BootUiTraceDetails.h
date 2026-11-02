// SPDX-License-Identifier: MIT
#ifndef SHENG_BOOT_UI_TRACE_DETAILS_H
#define SHENG_BOOT_UI_TRACE_DETAILS_H
struct bt_main {struct bt_entry entry;uint64_t frames[32],image;unsigned error,image_error;int pid;unsigned slide,q_max;};
static char bt_role_char(unsigned role,const struct bt_main *m) {
 return role==0?(m->entry.role?m->entry.role:'B'):"BSTU"[role&3];
}
// Two roles per page (first_role, first_role+1); roles beyond the captured set stay blank.
static void bt_main_rows(const struct bt_main *main,unsigned first_role,char rows[BT_ROWS][120]) {
 memset(rows,0,BT_ROWS*120);
 for(unsigned pair=0;pair<2;++pair) {
  unsigned role=first_role+pair;
  const struct bt_main *m=&main[role];const struct bt_entry *e=&m->entry;unsigned row=pair*7;
  if(!m->pid)continue;
  snprintf(rows[row],120,"MAIN %c p%d image=%llx err=%u cache+=%x PC=%llx LR=%llx FPerr=%u",
   bt_role_char(role,m),m->pid,m->image,m->image_error,m->slide,e->pc,e->lr,m->error);
  snprintf(rows[row+1],120,"trap=%llx opt=%llx remote=%x recv=%x msgid=%x q=%u rq=%u peer=%d e%u (sampled, not atomic)",
   e->trap,e->regs[1],(unsigned)e->regs[3],(unsigned)(e->regs[5]>>32),(unsigned)(e->regs[4]>>32),e->port_q,e->recv_q,e->peer,e->peer_code);
  for(unsigned slice=0;slice<4;++slice) {
   char a[8][9];for(unsigned i=0;i<8;++i)bt_address(a[i],m->frames[slice*8+i]);
   snprintf(rows[row+2+slice],120,"%c FP%02u %s %s %s %s %s %s %s %s",
    bt_role_char(role,m),slice*8+1,a[0],a[1],a[2],a[3],a[4],a[5],a[6],a[7]);
  }
  snprintf(rows[row+6],120,"peer e0=owner(-1=none) e1=not Mach2 e2=no remote e3=no space e4=lookup failed e5=receive arg candidate; FPerr 0/1/2/3");
 }
}
// One port-forensics page per traced role pair: state and flags of the blocked RPC's destination port, the
// port it is in transit to, its port-destroyed backup, its launchd service name, and up to six queued headers.
static char bt_kind_char(unsigned kind) {
 return kind==1?'K':(kind==2?'R':(kind==3?'T':(kind==4?'N':'?')));
}
static void bt_port_rows(const struct bt_main *main,unsigned first_role,char rows[BT_ROWS][120]) {
 memset(rows,0,BT_ROWS*120);
 for(unsigned pair=0;pair<2;++pair) {
  unsigned role=first_role+pair;const struct bt_main *m=&main[role];const struct bt_entry *e=&m->entry;unsigned row=pair*7;
  if(!m->pid)continue;
  const char c=bt_role_char(role,m);
  snprintf(rows[row],120,"PORTS %c p%d %s=%x rx=%c st=%c rn=%x kt=%u q=%u max=%u ql=%u sr=%u ms=%u kl=%u lk=%u",
   c,m->pid,(e->regs[1]&1)?"snd":"lnk",(unsigned)e->regs[3],bt_kind_char(e->send_kind),(char)(e->send_state?e->send_state:'?'),e->send_rname,e->send_kotype,
   e->port_q,m->q_max,e->send_qlimit,e->send_srights,e->send_mscount,e->send_klist,e->send_link);
  snprintf(rows[row+1],120,"PORTS %c rcv=%x rfl=%x pid=%d rq=%u/%u rspr=%d msgs=%u fl=%x spr=%d pdr=%u sl=%c lk=%u",
   c,(unsigned)(e->regs[5]>>32),e->recv_flags,(int)e->recv_pid,e->recv_q,e->recv_qlimit,(int)e->recv_srpid,e->msg_n,
   e->send_flags,(int)e->send_spr,e->send_pdr,(char)(e->recv_sync_match?e->recv_sync_match:'.'),e->recv_link);
  snprintf(rows[row+2],120,"PORTS %c dst=%llx dn=%x dk=%c ds=%c df=%x dq=%u dp=%d dkt=%u do=%c svc=%.19s",
   c,(unsigned long long)e->dst_port,e->dst_rname,e->dst_kind?(char)e->dst_kind:'-',(char)(e->dst_state?e->dst_state:'-'),
   e->dst_flags,e->dst_q,(int)e->dst_pid,e->dst_kt,(char)(e->dst_owner?e->dst_owner:'-'),e->svc);
  for(unsigned i=0;i<6;i+=2) {
   int at=snprintf(rows[row+3+i/2],120,"PORTS %c",c);
   for(unsigned k=i;k<i+2 && at>0 && at<119;++k) {
    if(k>=e->msg_n)at+=snprintf(rows[row+3+i/2]+at,120-at," m%u -",k);
    else at+=snprintf(rows[row+3+i/2]+at,120-at," m%u b=%x i=%x s=%u o%u p=%d",k,e->msgs[k].bits,e->msgs[k].id,e->msgs[k].size,
     e->msgs[k].own,e->msg_pid[k]);
   }
  }
  snprintf(rows[row+6],120,"PORTS %c pd=%llx pr=%c ps=%c pn=%x pp=%d po=%c kl=%u lk=%u ck=%u lf=%x ln=%u",
   c,(unsigned long long)e->pdr_port,e->pdr_kind?(char)e->pdr_kind:'-',(char)(e->pdr_state?e->pdr_state:'-'),
   e->pdr_rname,(int)e->pdr_pid,(char)(e->pdr_owner?e->pdr_owner:'-'),
   e->dst_klist,e->dst_link,e->dst_checkin,e->dst_lflags,e->dst_lname);
 }
}
// The traced launchd's own IPC space: every receive right counted (queued messages q, armed queue knote kl,
// workloop knote wl, special reply sr, service label sv), then the six most significant ports. A port with
// queued messages and no knote of either kind and no receiving thread is one nobody will ever drain; cv says
// whether service names were compiled in.
static void bt_scan_rows(char c,const struct bt_scan *s,char rows[BT_ROWS][120],unsigned row0) {
 for(unsigned r=0;r<7 && row0+r<BT_ROWS;++r)memset(rows[row0+r],0,120);
 snprintf(rows[row0],120,"SP%c e%u r%u q=%u kl=%u wl=%u sr=%u sv=%u cv=%u sh=%u w=%x/%u/%x/%c/%u m=%x/%u/%x/%u/%d/%d",
  c,s->e,s->r,s->q,s->kl,s->wl,s->sr,s->sv,s->cv,s->n,
  s->wport.name,s->wport.q,s->wport.flags,s->wport.kind?s->wport.kind:'-',s->wport.link,
  s->mport.name,s->mport.q,s->mport.flags,s->mport.kt,s->mport.sem_owner,s->mport.sem_count);
 for(unsigned i=0;i<s->n && row0+1+i<BT_ROWS;++i) {
  const struct bt_sport *p=&s->ports[i];
  snprintf(rows[row0+1+i],120,"SP%c%u n=%x q=%u/%u f=%x st=%c kl=%u lk=%u ck=%u lf=%x ln=%u kt=%u",
   c,i,p->name,p->q,p->qlimit,p->flags,p->state?p->state:'-',p->kl,p->link,p->checkin,p->lflags,p->lname,p->kt);
 }
}
// The launchd page after the space scan: the dyld image array itself (version, count, array address, images[0]
// path and the /sbin/launchd entry by path), the same stop-signal thread's second user frame attributed to that
// image, the stop semaphore port's rights and queue state, every receive port that has a message queued
// (name:q:flags:kl:link:checkin:kt), and the service names of the first six. w is the w00t port's name, scans
// seen, queue drops and drains: a queue that only grows and never drains is a receiver that never ran.
static void bt_deep_rows(char c,const struct bt_deep *d,const struct bt_scan *s,const struct bt_thr *sem,char rows[BT_ROWS][120]) {
 memset(rows,0,BT_ROWS*120);
 snprintf(rows[0],120,"IMG%c v=%x n=%u e%u a=%llx i=%u la=%llx",c,d->version,d->img_count,d->img_error,
  (unsigned long long)d->img_array,d->img_index,(unsigned long long)d->img_base);
 snprintf(rows[1],120,"IMG%c0 %llx %s",c,(unsigned long long)d->img0,d->img0_path);
 snprintf(rows[2],120,"IMG%cM %s",c,d->img_match?d->img_path:"(no /sbin/launchd image entry)");
 const uint64_t f1=sem?sem->frames[1]:0;
 const unsigned in=(f1 && d->img_base && f1>=d->img_base && f1-d->img_base<0x100000)?1:0;
 snprintf(rows[3],120,"IMG%cF f1=%llx in=%u o=%llx",c,(unsigned long long)f1,in,in?(unsigned long long)(f1-d->img_base):0);
 snprintf(rows[4],120,"SEM%c n=%x sr=%u ms=%u so=%u c=%d wq=%u f=%x w=%x/%u/%u/%u ty=%u th=%.24s",
  c,s->mport.name,s->mport.srights,s->mport.mscount,s->mport.sorights,s->mport.sem_count,s->mport.wait,s->mport.flags,
  s->wport.name,s->wps,s->wpd,s->wpr,d->thr_type,d->thr_name);
 for(unsigned i=0;i<s->qn && i<BT_QP;++i) {
  unsigned r=5+i/5;if(r>=9)break;
  int at=(int)strlen(rows[r]);
  if(!at)at=snprintf(rows[r],120,"SPQ%c",c);
  if(at>0 && at<119)snprintf(rows[r]+at,120-at," %x:%u:%x:%u:%u:%u:%u",s->qp[i].name,s->qp[i].q,s->qp[i].flags,
   s->qp[i].kl,s->qp[i].link,s->qp[i].checkin,s->qp[i].kt);
 }
 for(unsigned i=0;i<6 && i<s->qn && 9+i<BT_ROWS;++i)
  snprintf(rows[9+i],120,"SPN%c %x %.24s",c,s->qp[i].name,s->qp[i].svc);
}
#endif
