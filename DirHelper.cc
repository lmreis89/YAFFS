#include "DirHelper.h"

using namespace std;

DirHelper::DirHelper(string marshalled)
{
    unmarshallDir(marshalled);
}


std::map< string, DirHelper::inum >
DirHelper::getListing()
{
    return contents;
}

DirHelper::inum
DirHelper::insert( DirHelper::inum ino, string name)
{
    if(contents.count(name) != 0)
        remove(name);

    contents[name] = ino;
    return ino;
}

bool
DirHelper::contains(string key)
{
    return contents.count(key) > 0;
}

DirHelper::inum
DirHelper::get(string key)
{
    if( contains(key) )
        return contents[key];

    return -1;
}

void
DirHelper::remove(string name)
{
    contents.erase(contents.find(name));
}

string
DirHelper::marshallDir()
{
    map< string, DirHelper::inum >::iterator it ;

    stringstream stream ;
    stream << "[" ;
    for(it = contents.begin(); it != contents.end(); it++)
    {
        stream << (*it).second;
        stream << (*it).first;
        stream << "/";
    }
    stream << "]" ;
    return stream.str();
}

void
DirHelper::unmarshallDir(string marshalled)
{
    if( marshalled.length() < 3) //if marshalled is only "[]"
        return;

    marshalled = marshalled.substr(1);

    stringstream stream (marshalled);


    while(stream.peek() != ']')
    {

        DirHelper::inum ino;
        stringstream name;
        stream >> ino;

        char current;
        for(stream >> current; current != '/'; stream >> current)
            name << current;

        contents[name.str()] = ino;
    }
}

