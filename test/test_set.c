#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include <string.h>
#include <getopt.h>

#include "libmemcached/memcached.h"

#define KEY_PREFIX "key"
#define VAL_PREFIX "val"

void usage(void)
{
	printf("usage: ./test_set <host> <port> <num>\n");
	exit(1);
}

void pexit(const char* msg)
{
	perror(msg);
	exit(1);
}

int main(int argc, char* argv[])
{
	if(argc < 4) { usage(); }

	char* host = argv[1];

	unsigned short port = atoi(argv[2]);
	if(port == 0) { usage(); }

	uint32_t num = atoi(argv[3]);
	if(num == 0) { usage(); }

	memcached_st* mc = memcached_create(NULL);
	if(mc == NULL) { pexit("memcached_create"); }

	memcached_server_add(mc, host, port);

//	memcached_behavior_set(mc, MEMCACHED_BEHAVIOR_BINARY_PROTOCOL, 1);

	char kbuf[strlen(KEY_PREFIX) + 11];
	char vbuf[strlen(VAL_PREFIX) + 11];

while(1) {
	uint32_t i;
	for(i=0; i < num; ++i) {
		int klen = sprintf(kbuf, KEY_PREFIX "%d", i);
		int vlen = sprintf(vbuf, VAL_PREFIX "%d", i);
		printf("set '%s' = '%s'\n", kbuf, vbuf);
		memcached_set(mc, kbuf, klen, vbuf, vlen, 0, 0);
	}
}

	return 0;
}

