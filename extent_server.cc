// the extent server implementation

#include "extent_server.h"
#include <sstream>
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>

using namespace std;

extent_server::extent_server()
{

    extent_protocol::attr a;

    time_t time_as_t =  time(NULL);
    unsigned int time = static_cast<unsigned int>( time_as_t);

    a.size = 0;
	a.atime = time;
	a.mtime = time;
	a.ctime = time;
    unsigned long long key = 1;

    files[key] = pair<std::string,extent_protocol::attr>("[]",a);
}


int extent_server::put(extent_protocol::extentid_t id, std::string buf, int &)
{
    map<extent_protocol::extentid_t, pair<std::string,extent_protocol::attr> >::iterator content = files.find(id);
	bool exists =  content != files.end();

    time_t time_as_t =  time(NULL);
    unsigned int time = static_cast<unsigned int>( time_as_t);

	if(!exists)
	{
	    extent_protocol::attr a;
        a.size = buf.size();
        a.atime = time;
        a.mtime = time;
        a.ctime = time;
        files[id] = pair<string,extent_protocol::attr>(buf,a);
	}
	else
	{
        content->second.second.mtime = time;
        content->second.second.ctime = time;
        content->second.second.size = buf.size();
        content->second.first = buf;
	}


	return extent_protocol::OK;
}

int extent_server::get(extent_protocol::extentid_t id, std::string &buf)
{
    time_t time_as_t =  time(NULL);
    unsigned int time = static_cast<unsigned int>( time_as_t);

    if(files.count(id) == 0)
        return extent_protocol::NOENT;

    std::string mybuf = files[id].first;
    files[id].second.atime = time;
    buf = mybuf;

	return extent_protocol::OK;
}

int extent_server::getattr(extent_protocol::extentid_t id, extent_protocol::attr &a)
{
    if(files.count(id) == 0)
        return extent_protocol::NOENT;

	extent_protocol::attr stored = files[id].second;
	a.size = stored.size;
	a.atime = stored.atime;
	a.mtime = stored.mtime;
	a.ctime = stored.ctime;
    return extent_protocol::OK;
}

int extent_server::remove(extent_protocol::extentid_t id, int &)
{
    if(files.count(id) == 0)
        return extent_protocol::NOENT;

    files.erase( files.find(id) );

	return extent_protocol::OK;
}

int extent_server::exists(extent_protocol::extentid_t id, int &r)
{
    r = files.count(id);
    return extent_protocol::OK;
}

int extent_server::setattr(extent_protocol::extentid_t id, unsigned int size, int&)
{
    map<extent_protocol::extentid_t, pair<std::string,extent_protocol::attr> >::iterator content = files.find(id);

    if(files.count(id) == 0 )
        return extent_protocol::NOENT;

    time_t time_as_t =  time(NULL);
    unsigned int time = static_cast<unsigned int>( time_as_t);

    std::string extent = files[id].first;
    char padding = '\0';

    if(extent.length() > size)
        extent = extent.substr(0,size);
    else if(extent.length() < size)
        extent = extent + std::string( extent.length() - size, padding);

    content->second.first = extent;
    content->second.second.mtime = time;
    content->second.second.ctime = time;
    content->second.second.size = extent.size();

    return extent_protocol::OK;
}

