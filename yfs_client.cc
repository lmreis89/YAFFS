// yfs client.  implements FS operations using extent and lock server
#include "yfs_client.h"
#include "extent_client.h"
#include "lock_client.h"
#include <sstream>
#include <iostream>
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <bitset>
#include <time.h>
#include <fstream>


#include <fcntl.h>

using namespace std;

yfs_client::yfs_client(std::string extent_dst, std::string lock_dst)
{
  ec = new extent_client(extent_dst);
  lc = new lock_client(lock_dst, 1);

}

yfs_client::inum
yfs_client::n2i(std::string n)
{
  std::istringstream ist(n);
  unsigned long long finum;
  ist >> finum;
  return finum;
}

 yfs_client::inum
 yfs_client::ilookup(yfs_client::inum di, std::string name)
 {
    if(name == "/")
        return 1;

    string extent = "";

    ec->get(di,extent);
    DirHelper dh (extent);

    return dh.get(name);
 }


std::string
yfs_client::filename(inum inum)
{
  std::ostringstream ost;
  ost << inum;
  return ost.str();
}

bool
yfs_client::isfile(inum inum)
{
  if(inum & 0x80000000)
    return true;
  return false;
}

bool
yfs_client::isdir(inum inum)
{
  return ! isfile(inum);
}

int
yfs_client::getfile(inum inum, fileinfo &fin)
{
  int r = OK;


  printf("getfile %016llx\n", inum);
  extent_protocol::attr a;

  if (ec->getattr(inum, a) != extent_protocol::OK) {
    r = IOERR;
    goto release;
  }

  fin.atime = a.atime;
  fin.mtime = a.mtime;
  fin.ctime = a.ctime;
  fin.size = a.size;
  printf("getfile %016llx -> sz %llu\n", inum, fin.size);

 release:

  return r;
}

int
yfs_client::getdir(inum inum, dirinfo &din)
{
  int r = OK;


  printf("getdir %016llx\n", inum);
  extent_protocol::attr a;


  if (ec->getattr(inum, a) != extent_protocol::OK) {
    r = IOERR;
    goto release;
  }
  din.atime = a.atime;
  din.mtime = a.mtime;
  din.ctime = a.ctime;

 release:

  return r;
}

unsigned int yfs_client::gen_seed()
{
    unsigned int random_seed, random_seed_a, random_seed_b;
    //linux defined random based on physical state of the devices
    std::ifstream file ("/dev/urandom", std::ios::binary);
    if (file.is_open())
    {
        char * memblock;
        int size = sizeof(int);
        memblock = new char [size];
        file.read (memblock, size);
        file.close();
        random_seed_a = *reinterpret_cast<int*>(memblock);
        delete[] memblock;
    }// end if
    else
    {
        random_seed_a = 0;
    }
    random_seed_b = time(0);
    random_seed = random_seed_a xor random_seed_b;
    return random_seed;
}


yfs_client::inum
yfs_client::generate_ino(bool isDir)
{

    MTRand_int32 irand;
    irand.seed(gen_seed()); // 32-bit int generator
    unsigned long rand = irand();

    if(isDir)
    {
        unsigned long mask =0 ;
        mask = 0x80000000 ;
        rand = ~mask & rand;
    }
    else
    {
        unsigned long mask = 0;
        mask = 0x80000000 ;
        rand = mask | rand;
    }

    yfs_client::inum ino = rand ;

    int r;
    ec->exists(ino, r);
    bool unique = r == 0;

    if(unique) //unique
        return ino;
    else
        return generate_ino(isDir);

}

std::map< std::string , yfs_client::inum>
yfs_client::readDir(yfs_client::inum dir)
{
    string extent;

    ec->get(dir,extent);
    DirHelper dh (extent);

    return dh.getListing();
}



yfs_client::inum
yfs_client::mknod(yfs_client::inum parent, std::string name)
{
    yfs_client::inum newino = generate_ino(false) ;

    lc->acquire(parent);
    string extent;
    ec->get(parent,extent);

    DirHelper dh (extent);
    dh.insert(newino,name);

    ec-> put(parent, dh.marshallDir());
    ec-> put(newino, "");

    lc->release(parent);

    return newino;
}


int
yfs_client::setattr(yfs_client::inum ino, unsigned int size)
{
    lc->acquire(ino);
    int r = ec->setattr(ino,size);

    lc->release(ino);

    return (r == extent_protocol::OK ? yfs_client::OK : yfs_client::NOENT);
}

std::string
yfs_client::readChunk(std::string extent, int offset, int size)
{
    std::stringstream ret;
    int maxPos =  (offset + size)-1; //ultima posicao lida

    bool start = extent.length() > offset;
    bool end = extent.length() > maxPos;

    if(start && end) //o bloco existe completamente
        ret << extent.substr(offset,size);
    else if(start && !end) //so existe uma parte inicial do bloco. ler ate onde dá
        ret << extent.substr(offset);

    return ret.str();
}

std::pair<std::string,bool>
yfs_client::read(yfs_client::inum ino, int size, int offset)
{
    int count;
    ec->exists(ino,count);
    bool error = (count == 0) ;

    if(error)
    {
        return pair<std::string, bool>("",error);

    }



    std::string extent;
    ec->get(ino,extent);
    std::string s = readChunk(extent,offset,size);

    return pair<std::string,bool>(s, (error && s.length() == 0) );
}

int
yfs_client::write(yfs_client::inum ino, const char *buf, int size, int offset)
{
    lc->acquire(ino);
    int count;
    ec->exists(ino,count);

    if(count == 0 )
    {
        lc->release(ino);
        return -1;
    }


    std::string extent;
    ec->get(ino,extent);

    int maxPos = (offset + size)-1 ;

    bool start = ( extent.length() >= offset);
    bool end = (extent.length() > maxPos);

    std::stringstream ret;
    std::string middle (buf,size);
    if(start)
    {
        std::string begin ( extent.substr(0,offset) );
        ret << begin;
    }
    else
        ret << extent << std::string(offset - extent.length(),'\0');

    ret << middle.substr(0,size);

    if(end)
    {
        std::string end ( extent.substr( offset+size ) );
        ret << end ;
    }

    string finalRet = ret.str();
    ec->put(ino,finalRet);

    lc->release(ino);
    return size;

}

bool
yfs_client::exists(yfs_client::inum ino)
{
    int count;
    ec->exists(ino,count);
    return count != 0;
}

yfs_client::status
yfs_client::unlink(yfs_client::inum parent,std::string name)
{
    lc->acquire(parent);
    string extent;
    extent_protocol::status ret = ec->get(parent,extent);
    if(ret != extent_protocol::OK)
    {
        lc->release(parent);
        return -1;
    }


    DirHelper dh (extent);

    if(!dh.contains(name))
    {
        lc->release(parent);
        return -1;
    }

    yfs_client::inum ino = dh.get(name);
    dh.remove(name);

    if(ec->remove(ino) != yfs_client::OK)
    {
        lc->release(parent);
        return -1;

    }

    if(ec->put(parent,dh.marshallDir()) != yfs_client::OK)
    {
        lc->release(parent);
        return -1;
    }

    lc->release(parent);
    return yfs_client::OK;
}

yfs_client::inum
yfs_client::mkDir(yfs_client::inum parent, std::string name)
{
    string extent;
    lc->acquire(parent);
    extent_protocol::status ret = ec->get(parent,extent);
    if(ret != extent_protocol::OK)
    {
        lc->release(parent);
        return -1;
    }


    DirHelper dh (extent);

    yfs_client::inum newino = generate_ino(true);
    dh.insert(newino,name);

    ret = ec->put(parent,dh.marshallDir());
    if(ret != yfs_client::OK)
    {
        lc->release(parent);
        return -1;
    }


    ret = ec->put(newino,"[]");
    lc->release(parent);

    return newino;
}


