// lock client interface.

#ifndef lock_client_h
#define lock_client_h

#include <string>
#include "lock_protocol.h"
#include "rpc.h"
#include <vector>
#include "rsm_client.h"

// Client interface to the lock server
class lock_client {
 protected:
  rsm_client *cl;
  unsigned int id;
  unsigned int seqno;
  pthread_mutex_t seq_mutex;
 public:
  lock_client(std::string d, unsigned int cltID);
  virtual ~lock_client() {};
  virtual lock_protocol::status acquire(lock_protocol::lockid_t);
  virtual lock_protocol::status release(lock_protocol::lockid_t);
  virtual lock_protocol::status stat(lock_protocol::lockid_t);
  private:
  unsigned int gen_id();
};


#endif
