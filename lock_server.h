// this is the lock server
// the lock client has a similar interface

#ifndef lock_server_h
#define lock_server_h

#include <string>
#include "lock_protocol.h"
#include "lock_client.h"
#include "rpc.h"
#include <map>
#include "rsm.h"
#include "rsm_state_transfer.h"



class lock_server : rsm_state_transfer {



 private:
  class rsm *rsm;
 protected:
  int nacquire;
  std::map<lock_protocol::lockid_t, std::pair<int, int > > locks;
  pthread_mutex_t mut;

 public:
  lock_server();
  ~lock_server() {};
  lock_server(class rsm *rsm=0);
  lock_protocol::status stat(int clt, lock_protocol::lockid_t lid, int &);
  lock_protocol::status acquire(int clt, lock_protocol::lockid_t lid, int seqno, int &);
  lock_protocol::status release(int clt, lock_protocol::lockid_t lid, int &);
  std::string marshal_state();
  void unmarshal_state(std::string state);
};

#endif







