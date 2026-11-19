// Build with -static-libstdc++ -Wl,--wrap=pthread_create for partial startup failure injection.
#include "infer/NpuInferenceScheduler.hpp"
#include <pthread.h>
#include <cerrno>
#include <array>
#include <atomic>
#include <cassert>
#include <iostream>
extern "C" MPP_RET mpp_buffer_put_with_caller(MppBuffer, const char*) { std::abort(); }
static std::atomic<int> fail_after{0};
extern "C" int __real_pthread_create(pthread_t*, const pthread_attr_t*, void*(*)(void*), void*);
extern "C" int __wrap_pthread_create(pthread_t* t, const pthread_attr_t* a, void*(*f)(void*), void* x) {
    if (fail_after>0 && --fail_after==0) return EAGAIN;
    return __real_pthread_create(t,a,f,x);
}
int main() {
    NpuSchedulerConfig config; config.max_frame_age_ms=0; config.stats_interval_sec=100;
    NpuInferenceScheduler scheduler(config);
    std::array<std::atomic<int>,4> active{},executing{},completed{};
    std::array<std::atomic<uint64_t>,4> previous{};
    std::atomic<int> concurrent{0},peak{0};
    std::vector<NpuInferenceScheduler::Executor> executors;
    for (int worker=0;worker<3;++worker) executors.push_back([&,worker](const NpuChannelConfig& ch,const Frame&) {
        int id=std::stoi(ch.channel_id)-1;
        assert(active[id].fetch_add(1)==0); ++executing[id];
        int n=++concurrent,old=peak;
        while(n>old && !peak.compare_exchange_weak(old,n)) {}
        std::this_thread::sleep_for(std::chrono::milliseconds(3+worker*3));
        --executing[id];--concurrent;
        return NpuInferenceResult{};
    });
    scheduler.set_executors(executors);
    fail_after=2;
    bool failed=false;
    try {scheduler.start();} catch (const std::system_error&) {failed=true;}
    assert(failed && !scheduler.is_running());
    scheduler.stop();
    for(int id=0;id<4;++id) scheduler.register_channel({std::to_string(id+1),1000,1},[&,id](const NpuInferenceResult& result) {
        assert(active[id]==1 && result.frame_id>previous[id]);previous[id]=result.frame_id;
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
        ++completed[id];assert(active[id].fetch_sub(1)==1);
    });
    scheduler.start();
    for(uint64_t f=1;f<=400;++f) {
        for(int id=0;id<4;++id) {
            if(id==2 && f>80 && f<160) continue;
            Frame frame;frame.frame_id=f;frame.pts_ms=f;
            assert(scheduler.submit(std::to_string(id+1),std::move(frame)));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    scheduler.unregister_channel("4");assert(executing[3]==0);
    int count=completed[3];std::this_thread::sleep_for(std::chrono::milliseconds(20));assert(completed[3]==count);
    assert(!scheduler.submit("4",Frame{}));
    scheduler.stop();scheduler.stop();assert(!scheduler.is_running() && peak==3);
    for(int i=0;i<4;++i) assert(completed[i]>10 && executing[i]==0);
    // Legacy single-executor entry point and restart.
    scheduler.set_executor([](const NpuChannelConfig&,const Frame&) {return NpuInferenceResult{};});
    scheduler.start();scheduler.stop();
    std::cout << "PASS: partial startup cleanup, restart, three concurrent executors, ordered callbacks, source pause/resume, unregister, stop and legacy API\n";
}
