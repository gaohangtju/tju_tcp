#include "tju_tcp.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
static double now_s(void){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts); return ts.tv_sec + ts.tv_nsec/1e9; }
int main(int argc,char**argv){
    long target = (argc>1)? atol(argv[1]) : (10L*1024*1024);
    startSimulation();
    tju_tcp_t* s = tju_socket();
    tju_sock_addr a; a.ip=inet_network("172.17.0.3"); a.port=1234;
    tju_bind(s,a); tju_listen(s);
    tju_tcp_t* c = tju_accept(s);
    static char buf[32768];
    long got=0, next=1024*1024; double t0=now_s();
    while(got<target){
        int n=tju_recv(c,buf,sizeof(buf));
        if(n<=0) break;
        got+=n;
        while(got>=next){ fprintf(stderr,"[SRV] %ld MB @ %.3f s\n", next/1024/1024, now_s()-t0); next+=1024*1024; }
    }
    double dt=now_s()-t0;
    printf("[BENCH-SRV] recv=%ld bytes in %.3f s => %.3f MB/s\n", got, dt, got/dt/1e6);
    fflush(stdout); fflush(stderr);
    return 0;
}
