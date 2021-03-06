//
// Replicated state machine implementation with a primary and several
// backups. The primary receives requests, assigns each a view stamp (a
// vid, and a sequence number) in the order of reception, and forwards
// them to all backups. A backup executes requests in the order of that
// the primary stamps them and replies with an OK to the primary. The
// primary executes the request after it receives OKs from all backups,
// and sends the reply back to the client.
//
// The config module will tell the RSM about a new view. If the
// primary in the previous view is a member of the new view, then it
// will stay the primary.  Otherwise, the smallest numbered node of
// the previous view will be the new primary.  In either case, the new
// primary will be a node from the previous view.  The configuration
// module constructs the sequence of views for the RSM and the RSM
// ensures there will be always one primary, who was a member of the
// last view.
//
// When a new node starts, the recovery thread is in charge of joining
// the RSM.  It will collect the internal RSM state from the primary;
// the primary asks the config module to add the new node and returns
// to the joining the internal RSM state (e.g., paxos log). Since
// there is only one primary, all joins happen in well-defined total
// order.
//
// The recovery thread also runs during a view change (e.g, when a node
// has failed).  After a failure some of the backups could have
// processed a request that the primary has not, but those results are
// not visible to clients (since the primary responds).  If the
// primary of the previous view is in the current view, then it will
// be the primary and its state is authoritive: the backups download
// from the primary the current state.  A primary waits until all
// backups have downloaded the state.  Once the RSM is in sync, the
// primary accepts requests again from clients.  If one of the backups
// is the new primary, then its state is authoritative.  In either
// scenario, the next view uses a node as primary that has the state
// resulting from processing all acknowledged client requests.
// Therefore, if the nodes sync up before processing the next request,
// the next view will have the correct state.
//
// While the RSM in a view change (i.e., a node has failed, a new view
// has been formed, but the sync hasn't completed), another failure
// could happen, which complicates a view change.  During syncing the
// primary or backups can timeout, and initiate another Paxos round.
// There are 2 variables that RSM uses to keep track in what state it
// is:
//    - inviewchange: a node has failed and the RSM is performing a view change
//    - insync: this node is syncing its state
//
// If inviewchange is false and a node is the primary, then it can
// process client requests. If it is true, clients are told to retry
// later again.  While inviewchange is true, the RSM may go through several
// member list changes, one by one.   After a member list
// change completes, the nodes tries to sync. If the sync complets,
// the view change completes (and inviewchange is set to false).  If
// the sync fails, the node may start another member list change
// (inviewchange = true and insync = false).
//
// The implementation should be used only with servers that run all
// requests run to completion; in particular, a request shouldn't
// block.  If a request blocks, the backup won't respond to the
// primary, and the primary won't execute the request.  A request may
// send an RPC to another host, but the RPC should be a one-way
// message to that host; the backup shouldn't do anything based on the
// response or execute after the response, because it is not
// guaranteed that all backup will receive the same response and
// execute in the same order.
//
// The implementation can be viewed as a layered system:
//       RSM module     ---- in charge of replication
//       config module  ---- in charge of view management
//       Paxos module   ---- in charge of running Paxos to agree on a value
//
// Each module has threads and internal locks. Furthermore, a thread
// may call down through the layers (e.g., to run Paxos's proposer).
// When Paxos's acceptor accepts a new value for an instance, a thread
// will invoke an upcall to inform higher layers of the new value.
// The rule is that a module releases its internal locks before it
// upcalls, but can keep its locks when calling down.

#include <fstream>
#include <iostream>
#include <unistd.h>

#include "handle.h"
#include "rsm.h"
#include "rsm_client.h"

static void *
recoverythread(void *x)
{
  rsm *r = (rsm *) x;
  r->recovery();
  return 0;
}



rsm::rsm(std::string _first, std::string _me)
  : stf(0), primary(_first), insync (false), inviewchange (false), nbackup (0), partitioned (false), dopartition(false), break1(false), break2(false)
{
  pthread_t th;

  last_myvs.vid = 0;
  last_myvs.seqno = 0;
  myvs = last_myvs;
  myvs.seqno = 1;

  pthread_mutex_init(&rsm_mutex, NULL);
  pthread_mutex_init(&invoke_mutex, NULL);
  pthread_cond_init(&recovery_cond, NULL);
  pthread_cond_init(&sync_cond, NULL);
  pthread_cond_init(&join_cond, NULL);

  cfg = new config(_first, _me, this);

  rsmrpc = cfg->get_rpcs();
  rsmrpc->reg(rsm_client_protocol::invoke, this, &rsm::client_invoke);
  rsmrpc->reg(rsm_client_protocol::members, this, &rsm::client_members);
  rsmrpc->reg(rsm_protocol::invoke, this, &rsm::invoke);
  rsmrpc->reg(rsm_protocol::transferreq, this, &rsm::transferreq);
  rsmrpc->reg(rsm_protocol::transferdonereq, this, &rsm::transferdonereq);
  rsmrpc->reg(rsm_protocol::joinreq, this, &rsm::joinreq);

  // tester must be on different port, otherwise it may partition itself
  testsvr = new rpcs(atoi(_me.c_str()) + 1);
  testsvr->reg(rsm_test_protocol::net_repair, this, &rsm::test_net_repairreq);
  testsvr->reg(rsm_test_protocol::breakpoint, this, &rsm::breakpointreq);

  assert(pthread_mutex_lock(&rsm_mutex)==0);

  assert(pthread_create(&th, NULL, &recoverythread, (void *) this) == 0);

  assert(pthread_mutex_unlock(&rsm_mutex)==0);
}

void
rsm::reg1(int proc, handler *h)
{
  assert(pthread_mutex_lock(&rsm_mutex)==0);
  procs[proc] = h;
  assert(pthread_mutex_unlock(&rsm_mutex)==0);
}

// The recovery thread runs this function
void
rsm::recovery()
{
  bool r = true;

  assert(pthread_mutex_lock(&rsm_mutex)==0);

  while (1)
  {
    while (!cfg->ismember(cfg->myaddr()))
    {
        if (join(primary))
        {
            printf("recovery: joined\n");
        } else {
            printf("recovery: not joined\n");
            assert(pthread_mutex_unlock(&rsm_mutex)==0);
            sleep (30); // XXX make another node in cfg primary?
            assert(pthread_mutex_lock(&rsm_mutex)==0);
        }
        insync = true;
        inviewchange = true;
    }


    if(insync)
    {
        printf("recovery: going to synch\n");
        insync = false;
        if(cfg->myaddr() == primary)
        {//i'm the primary
            printf("recovery: synching as primary\n");
            sync_with_backups();
        }
        else
        {
            printf("recovery: synching as backup\n");
            while(!sync_with_primary() )
            {
                assert(pthread_mutex_unlock(&rsm_mutex)==0);
                usleep (50000);
                assert(pthread_mutex_lock(&rsm_mutex)==0);
                if(insync)
                    break;
            }

            if(!insync)
                printf("recovery: synched with primary\n");
            else
                printf("recovery: synch with primary aborted because of viewchange\n");
        }



    }

    if(insync) // there was a view change while synching
    {
        printf("recovery: view change while synching; synching again\n");
        continue;
    }
    else
    {
        inviewchange = false;
    }



    printf("recovery: go to sleep %d %d\n", insync, inviewchange);
    pthread_cond_wait(&recovery_cond, &rsm_mutex);
  }
  assert(pthread_mutex_unlock(&rsm_mutex)==0);
}

bool
rsm::sync_with_backups()
{
//    pthread_mutex_unlock(&rsm_mutex);
//    {
        // I need to be sure no client_invoke is on-going
//        printf("recovery::sync_with_backups -> waiting for end of client_invoke(s)\n");
//        ScopedLock ml(&invoke_mutex);
 //   }
//    pthread_mutex_lock(&rsm_mutex);

    printf("recovery::sync_with_backups -> waiting for backups to sync\n");

    while(1)
    {
        if(insync)
        {
            printf("recovery::sync_with_backup: aborted because there is a new view\n");
            break;
        }



        if(nbackup == 0) {
          printf("recovery::sync_with_backup: successfully synced, exiting\n");
          break;
        }

        printf("recovery::sync_with_backup: going to sleep\n");
        pthread_cond_wait(&sync_cond, &rsm_mutex);

    }

    printf("recovery::sync_with_backup: done\n");

    return true;
}


bool
rsm::sync_with_primary()
{

    if(cfg->myaddr() == primary)
    {
        printf("recovery::sync_with_primary -> i'm the primary now; aborting sync\n");
        return true;
    }

    if(statetransfer(primary))
        printf("recovery::sync_with_primary -> state transfer OK\n");
    else
    {
        printf("recovery::sync_with_primary -> state transfer FAILED\n");
        return false;
    }

    if(statetransferdone(primary))
        printf("recovery::sync_with_primary -> state transferdone OK\n");
    else
    {
        printf("recovery::sync_with_primary -> state transferdone FAILED\n");
        return false;
    }




    return true;
}


/**
 * Call to transfer state from m to the local node.
 * Assumes that rsm_mutex is already held.
 */
bool
rsm::statetransfer(std::string m)
{
  rsm_protocol::transferres r;
  handle h(m);
  int ret;
  printf("rsm::statetransfer: contact %s w. my last_myvs(%d,%d)\n",
	 m.c_str(), last_myvs.vid, last_myvs.seqno);
  if (h.get_rpcc()) {
    assert(pthread_mutex_unlock(&rsm_mutex)==0);
    ret = h.get_rpcc()->call(rsm_protocol::transferreq, cfg->myaddr(),
			     last_myvs, r, rpcc::to(1000));
    assert(pthread_mutex_lock(&rsm_mutex)==0);
  }
  if (h.get_rpcc() == 0 || ret != rsm_protocol::OK) {
    printf("rsm::statetransfer: couldn't reach %s %lx %d\n", m.c_str(),
	   (long unsigned) h.get_rpcc(), ret);
    return false;
  }
  if (stf && last_myvs != r.last) {
    stf->unmarshal_state(r.state);
  }
  last_myvs = r.last;
  printf("rsm::statetransfer transfer from %s success, vs(%d,%d)\n",
	 m.c_str(), last_myvs.vid, last_myvs.seqno);
  return true;
}

bool
rsm::statetransferdone(std::string m) {

  handle h(m);

  if(h.get_rpcc())
  {
      int r;
      int ret;
      pthread_mutex_unlock(&rsm_mutex);
      ret = h.get_rpcc()->call(rsm_protocol::transferdonereq,cfg->myaddr(),r,rpcc::to(1000));
      pthread_mutex_lock(&rsm_mutex);

      if(ret != rsm_protocol::OK)
        return false;

  } else return false;

  return true;
}


bool
rsm::join(std::string m) {
  handle h(m);
  int ret ;
  rsm_protocol::joinres r;

  if (h.get_rpcc() != 0) {
    printf("rsm::join: %s mylast (%d,%d)\n", m.c_str(), last_myvs.vid,
	   last_myvs.seqno);
    assert(pthread_mutex_unlock(&rsm_mutex)==0);
    ret = h.get_rpcc()->call(rsm_protocol::joinreq, cfg->myaddr(), last_myvs,
			     r, rpcc::to(120000));
    assert(pthread_mutex_lock(&rsm_mutex)==0);
  }
  if (h.get_rpcc() == 0 || ret != rsm_protocol::OK) {
    printf("rsm::join: couldn't reach %s %p %d\n", m.c_str(),
	   h.get_rpcc(), ret);
    return false;
  }
  printf("rsm::join: succeeded %s\n", r.log.c_str());
  cfg->restore(r.log);
  return true;
}


/*
 * Config informs rsm whenever it has successfully
 * completed a view change
 */

void
rsm::commit_change()
{
    printf("COMMIT CHANGE: LOCKING MUTEX\n");
    pthread_mutex_lock(&rsm_mutex);
    printf("COMMIT CHANGE: LOCKED MUTEX\n");

    if(!cfg->ismember(primary))
        set_primary();

     nbackup = cfg->get_curview().size() - 1;

        //não estou sincronizado
      insync = true;
      //mudança de view
      inviewchange = true;


      pthread_cond_signal(&recovery_cond);
      pthread_cond_signal(&sync_cond);


    pthread_mutex_unlock(&rsm_mutex);

    if(cfg->ismember(cfg->myaddr()))
        breakpoint2();
}


std::string
rsm::execute(int procno, std::string req)
{
  printf("rsm::execute\n");
  handler *h = procs[procno];
  assert(h);
  unmarshall args(req);
  marshall rep;
  std::string reps;
  rsm_protocol::status ret = h->fn(args, rep);
  marshall rep1;
  rep1 << ret;
  rep1 << rep.str();
  return rep1.str();
}

//
// Clients call client_invoke to invoke a procedure on the replicated state
// machine: the primary receives the request, assigns it a sequence
// number, and invokes it on all members of the replicated state
// machine.
//
rsm_client_protocol::status
rsm::client_invoke(int procno, std::string req, std::string &r)
{
    printf("rsm::client_invoke called\n");
    printf("rsm::client_invoke locking invoke_mutex\n");
    pthread_mutex_lock(&invoke_mutex);
    printf("rsm::client_invoke locked invoke_mutex OK\n");

    if(inviewchange)
    {
        printf("rsm::client_invoke -> rsm is inviewchange : responding busy\n");
        pthread_mutex_unlock(&invoke_mutex);
        return rsm_client_protocol::BUSY;
    }
    else if(!amiprimary())
    {
        printf("rsm::client_invoke -> rsm is not the primary : responding notprimary\n");
        pthread_mutex_unlock(&invoke_mutex);
        return rsm_client_protocol::NOTPRIMARY;
    }
    else
    {

        printf("rsm::client_invoke locking rsm_mutex\n");
        pthread_mutex_lock(&rsm_mutex); // Updating critical variables, need to lock mutex
        printf("rsm::client_invoke locked rsm_mutex OK\n");
        if(myvs.vid != cfg->vid())
        {
            myvs.vid = cfg->vid();
            myvs.seqno = 1;
        }

        last_myvs = myvs;
        myvs.seqno = myvs.seqno + 1;
        viewstamp vs = last_myvs;
        std::vector<std::string> view =  cfg->get_curview();

        pthread_mutex_unlock(&rsm_mutex); // critical variables updated, mutex unlocked
        std::vector<std::string>::iterator it;

        for(it = view.begin(); it != view.end(); it++)
        {

            if(*it != cfg->myaddr())
            {
                printf("rsm::client_invoke -> executing for %s with vid:%d and seqno:%d\n", (*it).c_str(), vs.vid, vs.seqno);
                handle h(*it);
                int ret = -1;
                if(h.get_rpcc())
                {
                    int dummy;

                    ret = h.get_rpcc()->call(rsm_protocol::invoke, procno,vs,req,dummy,rpcc::to(1000));

                    printf("rsm::client_invoke -> execution for %s %s\n",
                           (*it).c_str(), ret == rsm_protocol::OK? "was ok" : "was BAD");

                }


                if(ret != rsm_protocol::OK) // couldn't reach client, can't synchronize RSM
                {
                    printf("rsm::client_invoke -> Couldn't reach client %s , it must be either busy or crashed\n", (*it).c_str() );
                    pthread_mutex_unlock(&invoke_mutex);
                    return rsm_client_protocol::BUSY;
                }

                breakpoint1();
                partition1();

            }
        }
    }

    r = execute(procno,req);
    pthread_mutex_unlock(&invoke_mutex);


    return rsm_client_protocol::OK;

}

//
// The primary calls the internal invoke at each member of the
// replicated state machine
//
// the replica must execute requests in order (with no gaps)
// according to requests' seqno

rsm_protocol::status
rsm::invoke(int proc, viewstamp vs, std::string req, int &dummy)
{
    printf("rsm::invoke called\n");
    printf("rsm::invoke locking rsm_mutex\n");
    pthread_mutex_lock(&rsm_mutex);
    printf("rsm::invoke lock rsm_mutex OK\n");

    printf("rsm::invoke my last vid -> %d // his vid -> %d\n", last_myvs.vid, vs.vid);
    printf("rsm::invoke my last seqno -> %d && his seqno -> %d\n", last_myvs.seqno, vs.seqno);

    if(myvs.vid != cfg->vid())
    {
        myvs.vid = cfg->vid();
        myvs.seqno = 1;
    }


    if(!cfg->ismember(cfg->myaddr()))
    {
        printf("rsm::invoke i'm not a member of the current view, error\n");
        pthread_mutex_unlock(&rsm_mutex);
        return rsm_protocol::ERR;
    }

    if(!(myvs.vid == vs.vid && myvs.seqno == vs.seqno))
    {
        printf("rsm::invoke viewstamp error me(%d,%d) him(%d,%d)\n",myvs.vid, myvs.seqno, vs.vid, vs.seqno);
        pthread_mutex_unlock(&rsm_mutex);
        return rsm_protocol::ERR;
    }

    printf("rsm::invoke passed initial conditions\n");


    last_myvs = myvs;
    myvs.seqno = myvs.seqno +1;
    execute(proc,req);
    pthread_mutex_unlock(&rsm_mutex);

    printf("rsm::invoke replica executed successfully\n");
    breakpoint1();

    return rsm_protocol::OK;

}

/**
 * RPC handler: Send back the local node's state to the caller
 */
rsm_protocol::status
rsm::transferreq(std::string src, viewstamp last, rsm_protocol::transferres &r)
{
  printf("transferreq locking mutex\n");
  assert(pthread_mutex_lock(&rsm_mutex)==0);
  printf("transferreq ACQUIRED mutex\n");

  int ret = rsm_protocol::OK;
  printf("transferreq from %s (%d,%d) vs (%d,%d)\n", src.c_str(),
	 last.vid, last.seqno, last_myvs.vid, last_myvs.seqno);

  if( !inviewchange || !cfg->ismember(src))
  {
      printf("transferrequest received but i'm not syncing yet\n");
      pthread_mutex_unlock(&rsm_mutex);
      return rsm_protocol::BUSY;
  }


  if (stf && last != last_myvs)
    r.state = stf->marshal_state();

  r.last = last_myvs;
  assert(pthread_mutex_unlock(&rsm_mutex)==0);
  return ret;
}

/**
  * RPC handler: Send back the local node's latest viewstamp
  */
rsm_protocol::status
rsm::transferdonereq(std::string m, int &r)
{
  int ret = rsm_client_protocol::OK;
  printf("rsm::transferdonereq from %s\n", m.c_str());
  assert (pthread_mutex_lock(&rsm_mutex) == 0);


  if(!inviewchange || !cfg->ismember(m))
  {
      printf("rsm::transferdonereq from %s -> i'm not synching right now reason:"
             , m.c_str());
      if(inviewchange)
        printf("inviewchange\n");
      else printf("not a member of current view");
      pthread_mutex_unlock(&rsm_mutex);
      return rsm_protocol::BUSY;
  }


  --nbackup;
  pthread_cond_signal(&sync_cond);
  assert (pthread_mutex_unlock(&rsm_mutex) == 0);

  return ret;

}

rsm_protocol::status
rsm::joinreq(std::string m, viewstamp last, rsm_protocol::joinres &r)
{
  int ret = rsm_client_protocol::OK;

  assert (pthread_mutex_lock(&rsm_mutex) == 0);
  printf("joinreq: src %s last (%d,%d) mylast (%d,%d)\n", m.c_str(),
	 last.vid, last.seqno, last_myvs.vid, last_myvs.seqno);
  if (cfg->ismember(m)) {
    printf("joinreq: is still a member\n");
    r.log = cfg->dump();
  } else if (cfg->myaddr() != primary) {
    printf("joinreq: busy\n");
    ret = rsm_client_protocol::BUSY;
  } else {

        if (cfg->add(m)) {
            printf("joinreq: success\n");
            r.log = cfg->dump();
        } else {
            ret = rsm_client_protocol::ERR;
        }


  }
  assert (pthread_mutex_unlock(&rsm_mutex) == 0);
  return ret;
}

/*
 * RPC handler: Send back all the nodes this local knows about to client
 * so the client can switch to a different primary
 * when it existing primary fails
 */
rsm_client_protocol::status
rsm::client_members(int i, std::vector<std::string> &r)
{
  std::vector<std::string> m;
  printf("CLIENT MEMBERS BEFORE MUTEX\n");
  assert(pthread_mutex_lock(&rsm_mutex)==0);
  m = cfg->get_curview();
  m.push_back(primary);
  r = m;
  printf("rsm::client_members return %s m %s\n", cfg->print_curview().c_str(),
	 primary.c_str());
  assert(pthread_mutex_unlock(&rsm_mutex)==0);
  return rsm_protocol::OK;
}

// if primary is member of new view, that node is primary
// otherwise, the lowest number node of the previous view.
// caller should hold rsm_mutex
void
rsm::set_primary()
{
  std::vector<std::string> c = cfg->get_curview();
  std::vector<std::string> p = cfg->get_prevview();
  assert (c.size() > 0);

  if (isamember(primary,c)) {
    printf("set_primary: primary stays %s\n", primary.c_str());
    return;
  }

  assert(p.size() > 0);
  for (unsigned i = 0; i < p.size(); i++) {
    if (isamember(p[i], c)) {
      primary = p[i];
      printf("set_primary: primary is %s\n", primary.c_str());
      return;
    }
  }
  assert(0);
}

// Assume caller holds rsm_mutex
bool
rsm::amiprimary_wo()
{
  return primary == cfg->myaddr() && !inviewchange;
}

bool
rsm::amiprimary()
{
  assert(pthread_mutex_lock(&rsm_mutex)==0);
  bool r = amiprimary_wo();
  assert(pthread_mutex_unlock(&rsm_mutex)==0);
  return r;
}


// Testing server

// Simulate partitions

// assumes caller holds rsm_mutex
void
rsm::net_repair_wo(bool heal)
{
  std::vector<std::string> m;
  m = cfg->get_curview();
  for (unsigned i  = 0; i < m.size(); i++) {
    if (m[i] != cfg->myaddr()) {
        handle h(m[i]);
	printf("rsm::net_repair_wo: %s %d\n", m[i].c_str(), heal);
	if (h.get_rpcc()) h.get_rpcc()->set_reachable(heal);
    }
  }
  rsmrpc->set_reachable(heal);
}

rsm_test_protocol::status
rsm::test_net_repairreq(int heal, int &r)
{
  assert(pthread_mutex_lock(&rsm_mutex)==0);
  printf("rsm::test_net_repairreq: %d (dopartition %d, partitioned %d)\n",
	 heal, dopartition, partitioned);
  if (heal) {
    net_repair_wo(heal);
    partitioned = false;
  } else {
    dopartition = true;
    partitioned = false;
  }
  r = rsm_test_protocol::OK;
  assert(pthread_mutex_unlock(&rsm_mutex)==0);
  return r;
}

// simulate failure at breakpoint 1 and 2

void
rsm::breakpoint1()
{
  if (break1) {
    printf("Dying at breakpoint 1 in rsm!\n");
    exit(1);
  }
}

void
rsm::breakpoint2()
{
  if (break2) {
    printf("Dying at breakpoint 2 in rsm!\n");
    exit(1);
  }
}

void
rsm::partition1()
{
  if (dopartition) {
    net_repair_wo(false);
    dopartition = false;
    partitioned = true;
  }
}

rsm_test_protocol::status
rsm::breakpointreq(int b, int &r)
{
  r = rsm_test_protocol::OK;
  assert(pthread_mutex_lock(&rsm_mutex)==0);
  printf("rsm::breakpointreq: %d\n", b);
  if (b == 1) break1 = true;
  else if (b == 2) break2 = true;
  else if (b == 3 || b == 4) cfg->breakpoint(b);
  else r = rsm_test_protocol::ERR;
  assert(pthread_mutex_unlock(&rsm_mutex)==0);
  return r;
}




