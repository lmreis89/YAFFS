// the lock server implementation

#include "lock_server.h"
#include <sstream>
#include <stdio.h>
#include <unistd.h>
#include <arpa/inet.h>

using namespace std;

lock_server::lock_server(class rsm *_rsm):
rsm(_rsm), nacquire (0)
{
	pthread_mutex_init(&mut, NULL);
	rsm->set_state_transfer(this);
}

lock_protocol::status
lock_server::stat(int clt, lock_protocol::lockid_t lid, int &r)
{
	lock_protocol::status ret = lock_protocol::OK;
	printf("stat request from clt %d\n", clt);
	r = nacquire;
	return ret;
}

lock_protocol::status
lock_server::acquire(int clt, lock_protocol::lockid_t lid, int seqno, int &r)
{
	printf("lock_server::acquire: from clt %d with seqno %d to file %d\n", clt, seqno , lid);
    int ret;

    if(clt != 9)
    {
        pthread_mutex_lock(&mut);
        if(locks.count(lid) == 0)
        {
            printf("\tlock_server::acquire: no locks for file %d\n", lid);
            locks[lid] = pair<int, int>(clt,seqno);
            printf("\tlock_server::acquire: clt %d with seqno %u gained access to file %d\n",clt,seqno,lid);
            ret = lock_protocol::OK;
            pthread_mutex_unlock(&mut);

                return ret;
        }
        else
        {
            if(locks[lid].first == clt && locks[lid].second == seqno)
            {
                printf("\tlock_server::acquire -> revoke on lock for client %d with seqno %d, granting lock\n", clt, seqno);
                pthread_mutex_unlock(&mut);
                return lock_protocol::OK;
            }
            printf("\tlock_server::acquire: client %d with seqno %d has lock on file %d\n",locks[lid].first, locks[lid].second, lid);
            printf("\tlock_server::acquire: return retry to %d with seqno %d\n",clt,seqno);
            ret = lock_protocol::RETRY;
            pthread_mutex_unlock(&mut);
            return ret;
        }
    }



}

lock_protocol::status
lock_server::release(int clt, lock_protocol::lockid_t lid, int &)
{
	printf("lock_server::release: from clt %d to file %d\n", clt,lid);

    lock_protocol::status ret = lock_protocol::OK;

    pthread_mutex_lock(&mut);

    if(locks[lid].first != clt)
    {
        printf("\tlock_server::release: client %d trying to release lock for file %d that he doesnt own\n",clt,lid);
        pthread_mutex_unlock(&mut);
        return ret;
    }

    locks.erase( locks.find(lid) );
    printf("\tlock_server::release: released lock from file %d by client %d\n",lid,clt);
    pthread_mutex_unlock(&mut);
    return ret;

}

std::string
lock_server::marshal_state()
{

    pthread_mutex_lock(&mut);
    marshall rep;
    rep << locks.size();
    std::map<lock_protocol::lockid_t, pair<int, int> >::iterator it;

    for(it = locks.begin(); it != locks.end(); it++)
    {
        rep << it->first;
        rep << it->second.first;
        rep << it->second.second;
    }
    pthread_mutex_unlock(&mut);

    printf("lock_server::marshall_state: printing state\n");
    std::string rep_print = rep.str();
    printf("\t%s\n", rep_print.c_str());
    return rep.str();
}

void
lock_server::unmarshal_state(std::string state)
{
    pthread_mutex_lock(&mut);
    unmarshall rep(state);
    unsigned int locks_size;
    rep >> locks_size;

    printf("UNMARSHALL STATE\n");
    for(unsigned int i = 0; i < locks_size; i++)
    {
        lock_protocol::lockid_t lid;
        rep >> lid;
        int clt;
        int seqno;
        rep >> clt;
        rep >> seqno;
        locks[lid] = pair<int, int>(clt,seqno);

        printf("Client %d in request %d has file %d was unmarshalled\n",clt,seqno,lid);
    }

    pthread_mutex_unlock(&mut);
}
