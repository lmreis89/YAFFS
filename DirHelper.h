#ifndef DIRHELPER_H
#define DIRHELPER_H


#include <string>
#include <sstream>
#include <iostream>
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <map>

#include <fcntl.h>

using namespace std;



class DirHelper
{

    typedef unsigned long long inum;

    public:
        DirHelper(string marshalled = "[]");
        std::map< std::string, inum > getListing();
        inum insert( inum ino, string name);
        void remove(string name);
        string marshallDir();
        inum get(string key);
        bool contains(string key);


    protected:
        std::map< std::string, inum > contents;
    private:
        void unmarshallDir(string marshalled);

};

#endif // DIRHELPER_H
