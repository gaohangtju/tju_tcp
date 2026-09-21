#include "tju_tcp.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
static double now_s(void){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts); return ts.tv_sec + ts.tv_nsec/1e9; }
void sleep_no_wake(int sec){ do{ sec=sleep(sec);}while(sec>0); }
int main(int argc,char**argv){
    long target = (argc>1)? atol(argv[1]) : (20L*1024*1024);
    startSimulation();
    tju_tcp_t* s = tju_socket();
    tju_sock_addr a; a.ip=inet_network("172.17.0.3"); a.port=1234;
    if(tju_connect(s,a)<0){ printf("[BENCH-CLI] connect fail\n"); return 1; }
    static char buf[32768]; memset(buf,0x41,sizeof(buf));
    long sent=0; double t0=now_s();
    while(sent<target){
        int n = (int)(((target-sent) < (long)sizeof(buf)) ? (target-sent) : (long)sizeof(buf));
        int r = tju_send(s,buf,n);
        if(r<=0) break;
        sent+=r;
    }
    double dt=now_s()-t0;
    printf("[BENCH-CLI] sent=%ld bytes in %.3f s => %.3f MB/s\n", sent, dt, sent/dt/1e6);
    fflush(stdout);
    sleep_no_wake(3);
    tju_close(s);
    return 0;
}
