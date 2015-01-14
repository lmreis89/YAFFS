// this is the extent server

#ifndef extent_server_h
#define extent_server_h

#include <string>
#include <map>
#include "extent_protocol.h"

using namespace std;

class extent_server {

protected:
  map<extent_protocol::extentid_t,pair<std::string,extent_protocol::attr> > files;

 public:
  extent_server();


  int put(extent_protocol::extentid_t id, std::string, int &);
  int get(extent_protocol::extentid_t id, std::string &);
  int getattr(extent_protocol::extentid_t id, extent_protocol::attr &);
  int remove(extent_protocol::extentid_t id, int &);
  int exists(extent_protocol::extentid_t id, int &);
  int setattr(extent_protocol::extentid_t id, unsigned int size,int&);
};

#endif







