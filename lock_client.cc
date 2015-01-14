// RPC stubs for clients to talk to lock_server

#include "lock_client.h"
#include "rpc.h"
#include <arpa/inet.h>
#include <unistd.h>
#include <sstream>
#include <iostream>
#include <stdio.h>
#include "gettime.h"
#include <time.h>
#include <netdb.h>
#include <fstream>



//inline
//void set_rand_seed()
//{
//	struct timespec ts;
//	clock_gettime(CLOCK_REALTIME, &ts);
//	srandom((int)ts.tv_nsec^((int)getpid()));
//}

unsigned int lock_client::gen_id()
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
        usleep(5000);
        return gen_id();
    }
    random_seed_b = time(0);
    random_seed = random_seed_a xor random_seed_b;
    return random_seed;
}

lock_client::lock_client(std::string dst, unsigned int cltID)
{
	cl = new rsm_client(dst);
//	set_rand_seed();
    id = cltID;
    seqno = 1;
    printf("lock_client::.ctor: my id is %u\n",id);
    assert(pthread_mutex_init(&seq_mutex,NULL) == 0);
}


int
lock_client::stat(lock_protocol::lockid_t lid)
{
	int r;
	int ret = cl->call(lock_protocol::stat, lid, r);
	return r;
}

lock_protocol::status
lock_client::acquire(lock_protocol::lockid_t lid)
{
	int r;
	int ret;
	pthread_mutex_lock(&seq_mutex);
    int x = seqno++;
    pthread_mutex_unlock(&seq_mutex);

    printf("lock_client::acquire: lid %d client %u seqno %u\n", lid, id, x);
	while((ret = cl->call(lock_protocol::acquire,id,lid,x, r)) != lock_protocol::OK)
	{

       printf("lock_client::acquire: failed, sleeping for 50ms\n");
	   usleep(50000);

	}

    printf("lock_client::acquire: client %u locked file %d\n",id , lid);
	return r;
}

lock_protocol::status
lock_client::release(lock_protocol::lockid_t lid)
{
	int r;
	printf("lock_client::release: lid %d client %d\n", lid, id );
	int ret = cl->call(lock_protocol::release,id, lid, r);

    printf("lock_client::release: release for file %d returned %d\n",lid,ret);
	return r;
}

