#ifndef yfs_client_h
#define yfs_client_h

#include <string>
//#include "yfs_protocol.h"
#include "extent_client.h"
#include <vector>
#include "DirHelper.h"
#include "mtrand.h"
#include "map"

#include "lock_protocol.h"
#include "lock_client.h"
//comment to make changes for PR (sorry balewey, I'm using this project to test a bugfix on codacy) 
  class yfs_client {
  extent_client *ec;
  lock_client *lc;
 public:

  typedef unsigned long long inum;
  enum xxstatus { OK, RPCERR, NOENT, IOERR, FBIG };
  typedef int status;

  struct fileinfo {
    unsigned long long size;
    unsigned long atime;
    unsigned long mtime;
    unsigned long ctime;
  };
  struct dirinfo {
    unsigned long atime;
    unsigned long mtime;
    unsigned long ctime;
  };
  struct attr {
      unsigned long atime;
    unsigned long mtime;
    unsigned long ctime;
        unsigned int size;
  };
  struct dirent {
    std::string name;
    unsigned long long inum;
  };

 private:
  static std::string filename(inum);
  static inum n2i(std::string);
  static unsigned int gen_seed();
  std::string readChunk(std::string extent, int offset, int size);

 public:

  yfs_client(std::string, std::string);
  inum generate_ino(bool b);
  bool isfile(inum);
  bool isdir(inum);
  inum ilookup(inum di, std::string name);
  inum mknod(inum parent, std::string name);
  std::map< std::string , yfs_client::inum> readDir(yfs_client::inum dir);
  int getfile(inum, fileinfo &);
  int getdir(inum, dirinfo &);
  int setattr(inum ino, unsigned int size);
  int write(inum ino, const char* buf, int size, int offset);
  std::pair<std::string,bool> read(inum ino, int size, int offset);
  bool exists(inum ino);
  status unlink(inum parent,std::string name);
  inum mkDir(inum parent, std::string name);

};

#endif
