#include "paxos.h"
#include "handle.h"
// #include <signal.h>
#include <stdio.h>

// This module implements the proposer and acceptor of the Paxos
// distributed algorithm as described by Lamport's "Paxos Made
// Simple".  To kick off an instance of Paxos, the caller supplies a
// list of nodes, a proposed value, and invokes the proposer.  If the
// majority of the nodes agree on the proposed value after running
// this instance of Paxos, the acceptor invokes the upcall
// paxos_commit to inform higher layers of the agreed value for this
// instance.


bool
operator> (const prop_t &a, const prop_t &b)
{
  return (a.n > b.n || (a.n == b.n && a.m > b.m));
}

bool
operator>= (const prop_t &a, const prop_t &b)
{
  return (a.n > b.n || (a.n == b.n && a.m >= b.m));
}

std::string
print_members(const std::vector<std::string> &nodes)
{
  std::string s;
  s.clear();
  for (unsigned i = 0; i < nodes.size(); i++) {
    s += nodes[i];
    if (i < (nodes.size()-1))
      s += ",";
  }
  return s;
}

bool isamember(std::string m, const std::vector<std::string> &nodes)
{
  for (unsigned i = 0; i < nodes.size(); i++) {
    if (nodes[i] == m) return 1;
  }
  return 0;
}

bool
proposer::isrunning()
{
  bool r;
  assert(pthread_mutex_lock(&pxs_mutex)==0);
  r = !stable;
  assert(pthread_mutex_unlock(&pxs_mutex)==0);
  return r;
}

// check if the servers in l2 contains a majority of servers in l1
bool
proposer::majority(const std::vector<std::string> &l1,
		const std::vector<std::string> &l2)
{
  unsigned n = 0;

  for (unsigned i = 0; i < l1.size(); i++) {
    if (isamember(l1[i], l2))
      n++;
  }
  return n >= (l1.size() >> 1) + 1;
}

proposer::proposer(class paxos_change *_cfg, class acceptor *_acceptor,
		   std::string _me)
  : cfg(_cfg), acc (_acceptor), me (_me), break1 (false), break2 (false),
    stable (true)
{
  my_n.n = 0;
  my_n.m=me;
  assert (pthread_mutex_init(&pxs_mutex, NULL) == 0);

}

void
proposer::setn()
{
  my_n.n = acc->get_n_h().n + 1 > my_n.n + 1 ? acc->get_n_h().n + 1 : my_n.n + 1;
}

bool
proposer::run(int instance, std::vector<std::string> newnodes, std::string newv)
{
  std::vector<std::string> accepts;
  std::vector<std::string> nodes;
  std::vector<std::string> nodes1;
  std::string v;
  bool r = false;

  pthread_mutex_lock(&pxs_mutex);
  printf("start: initiate paxos for %s w. i=%d v=%s stable=%d\n",
	 print_members(newnodes).c_str(), instance, newv.c_str(), stable);
  if (!stable) {  // already running proposer?
    printf("proposer::run: already running\n");
    pthread_mutex_unlock(&pxs_mutex);
    return false;
  }
  stable = false;
  setn();
  accepts.clear();
  nodes.clear();
  v.clear();

  c_nodes = newnodes;
  c_v = newv;

  nodes = c_nodes;
  if (prepare(instance, accepts, nodes, v)) {

    if (majority(c_nodes, accepts)) {
      printf("paxos::manager: received a majority of prepare responses\n");

      if (v.size() == 0) {
            v = c_v;
      }

      breakpoint1();

      nodes1 = accepts;
      accepts.clear();
      accept(instance, accepts, nodes1, v);

      if (majority(c_nodes, accepts)) {
        printf("paxos::manager: received a majority of accept responses\n");

        breakpoint2();

        decide(instance, accepts, v);
        r = true;
      } else {
        printf("paxos::manager: no majority of accept responses\n");
      }
    } else {
      printf("paxos::manager: no majority of prepare responses\n");
    }
  } else {
    printf("paxos::manager: prepare is rejected %d\n", stable);
  }
  stable = true;
  pthread_mutex_unlock(&pxs_mutex);
  return r;
}

bool
proposer::prepare(unsigned instance, std::vector<std::string> &accepts,
         std::vector<std::string> nodes,
         std::string &v)
{
    prop_t max;
    max.m.clear();
    std::vector<std::string>::iterator it;

    for(it = nodes.begin(); it != nodes.end(); it++)
    {
        handle h(*it);
        if(h.get_rpcc())
        {
            paxos_protocol::preparearg arg;
            paxos_protocol::prepareres res;
            int ret;

            arg.instance = instance;
            arg.n = my_n;

            assert(pthread_mutex_unlock(&pxs_mutex) == 0);
            ret = h.get_rpcc()->call(paxos_protocol::preparereq,me,arg,res,rpcc::to(1000));
            assert(pthread_mutex_lock(&pxs_mutex) == 0);

            if(ret == paxos_protocol::OK)
            {
                if(res.oldinstance == 0)
                {
                    if(res.accept)
                    {
                        accepts.push_back(*it);
                    }

                    if(res.n_a > max || max.m.size() == 0)
                    {
                        max = res.n_a;
                        v = res.v_a;
                    }
                }
                else
                {
                    printf("OLD INSTANCE BEING COMMITTED, I WAS ALERTED BY %s\n", (*it).c_str());
                    acc->commit(instance,res.v_a);
                    return false;
                }
            }else printf("Failed to contact: %s", (*it).c_str());
        }
    }

    return true;
}


void
proposer::accept(unsigned instance, std::vector<std::string> &accepts,
        std::vector<std::string> nodes, std::string v)
{
    paxos_protocol::acceptarg arg;
    arg.instance = instance;
    arg.n = my_n;
    arg.v = v;

    std::vector<std::string>::iterator it;

    for(it = nodes.begin(); it != nodes.end(); it++)
    {
        handle h(*it);

        int ret;
        int r;

        if(h.get_rpcc())
        {
            pthread_mutex_unlock(&pxs_mutex);
            ret = h.get_rpcc()->call(paxos_protocol::acceptreq,me,arg,r,rpcc::to(1000));
            pthread_mutex_lock(&pxs_mutex);
            if(ret == paxos_protocol::OK)
            {
                if(r == 1)
                {
                    accepts.push_back(*it);
                } else printf("ACCEPTAL REJECTED\n");
            }
        }
    }
}

void
proposer::decide(unsigned instance, std::vector<std::string> accepts,
	      std::string v)
{
    paxos_protocol::decidearg arg;
    arg.instance = instance;
    arg.v = v;
    std::vector<std::string>::iterator it;

    for(it = accepts.begin(); it != accepts.end(); it++)
    {
       handle h(*it);

        int ret;
        int r;
        if(h.get_rpcc())
        {
            pthread_mutex_unlock(&pxs_mutex);
            ret = h.get_rpcc()->call(paxos_protocol::decidereq,me,arg,r, rpcc::to(1000));
            pthread_mutex_lock(&pxs_mutex);
        }
    }

}

acceptor::acceptor(class paxos_change *_cfg, bool _first, std::string _me,
	     std::string _value)
  : cfg(_cfg), me (_me), instance_h(0)
{
  assert (pthread_mutex_init(&pxs_mutex, NULL) == 0);

  n_h.n = 0;
  n_h.m = me;
  n_a.n = 0;
  n_a.m = me;
  v_a.clear();

  l = new log (this, me);

  if (instance_h == 0 && _first) {
    values[1] = _value;
    l->loginstance(1, _value);
    instance_h = 1;
  }

  pxs = new rpcs(atoi(_me.c_str()));
  pxs->reg(paxos_protocol::preparereq, this, &acceptor::preparereq);
  pxs->reg(paxos_protocol::acceptreq, this, &acceptor::acceptreq);
  pxs->reg(paxos_protocol::decidereq, this, &acceptor::decidereq);
}

paxos_protocol::status
acceptor::preparereq(std::string src, paxos_protocol::preparearg a,
    paxos_protocol::prepareres &r)
{
    pthread_mutex_lock(&pxs_mutex);
    if(instance_h >= a.instance)
    {
        r.oldinstance = 1;
        r.v_a = value(a.instance);
    }
    else{
        r.oldinstance = 0;
        if(a.n > n_h)
        {
            n_h = a.n;
            r.accept = 1;
            l->loghigh(n_h);
        }
        else{
            r.accept = 0;
        }
        r.n_a = n_a;
        r.v_a = v_a;
    }
    pthread_mutex_unlock(&pxs_mutex);
    return paxos_protocol::OK;

}

paxos_protocol::status
acceptor::acceptreq(std::string src, paxos_protocol::acceptarg a, int &r)
{
    pthread_mutex_lock(&pxs_mutex);
    if(a.instance > instance_h)
    {
        if(a.n >= n_h)
        {
          n_a = a.n;
          v_a = a.v;
          l->logprop(n_a,v_a);
          r = 1;

        }
        else{
            r = 0;
        }

        pthread_mutex_unlock(&pxs_mutex);
        return paxos_protocol::OK;

    }
    else{
        r = 0;
        pthread_mutex_unlock(&pxs_mutex);
        return paxos_protocol::ERR;
    }
}

paxos_protocol::status
acceptor::decidereq(std::string src, paxos_protocol::decidearg a, int &r)
{
    printf("DECIDE REQUEST FROM %s\n",src.c_str());
  commit(a.instance,a.v);

  return paxos_protocol::OK;
}

void
acceptor::commit_wo(unsigned instance, std::string value)
{
  //assume pxs_mutex is held
  printf("acceptor::commit: instance=%d has v= %s\n", instance, value.c_str());
  if (instance > instance_h) {
    printf("commit: highestaccepteinstance = %d\n", instance);
    values[instance] = value;
    l->loginstance(instance, value);
    instance_h = instance;
    n_h.n = 0;
    n_h.m = me;
    n_a.n = 0;
    n_a.m = me;
    v_a.clear();
    if (cfg) {
      pthread_mutex_unlock(&pxs_mutex);
      cfg->paxos_commit(instance, value);
      pthread_mutex_lock(&pxs_mutex);
    }
  }
}

void
acceptor::commit(unsigned instance, std::string value)
{
  pthread_mutex_lock(&pxs_mutex);
  commit_wo(instance, value);
  pthread_mutex_unlock(&pxs_mutex);
}

std::string
acceptor::dump()
{
  return l->dump();
}

void
acceptor::restore(std::string s)
{
  l->restore(s);
  l->logread();
}



// For testing purposes

// Call this from your code between phases prepare and accept of proposer
void
proposer::breakpoint1()
{
  if (break1) {
    printf("Dying at breakpoint 1!\n");
    exit(1);
  }
}

// Call this from your code between phases accept and decide of proposer
void
proposer::breakpoint2()
{
  if (break2) {
    printf("Dying at breakpoint 2!\n");
    exit(1);
  }
}

void
proposer::breakpoint(int b)
{
  if (b == 3) {
    printf("Proposer: breakpoint 1\n");
    break1 = true;
  } else if (b == 4) {
    printf("Proposer: breakpoint 2\n");
    break2 = true;
  }
}
