//#define _POSIX_C_SOURCE 199309
//#include <time.h>

#include <assert.h>

#define UCX_TIMERS 1

#ifdef __cplusplus
extern "C" {
#endif
extern uint64_t timer_nano();
#ifdef __cplusplus
}
#endif

//__inline__ unsigned long long timer(void)
//{
//    unsigned long long a, d;
//    unsigned long c;
//    __asm__ __volatile__ ("rdtscp" : "=a" (a), "=d" (d), "=c" (c) : : );
//    return (d << 32) + a;
//}

//__inline__ uint64_t timer()
//{
//    uint32_t low, high;
//    __asm volatile ("rdtsc" : "=a" (low), "=d" (high));
//    return ((uint64_t)high << 32) | (uint64_t)low;
//}

//static __inline__ uint64_t timer_nano() {
//    struct timespec time;
//    int result = clock_gettime(CLOCK_MONOTONIC, &time);
//    assert(!result);
//    return time.tv_sec * 1e9 + time.tv_nsec;
//}

enum ucx_timer_levels {
	UCXTL_HIGHLEVEL,
	UCXTL_PROTOBUF_PASSED,
	UCXTL_EPOLL_WAIT,
	UCXTL_CHTTP2,
	UCXTL_ENDPOINT,
	UCXTL_UCX,
	UCXTL_SIZE
};

extern uint64_t ucx_timer[UCXTL_SIZE];
extern uint64_t ucx_timer_mtx[UCXTL_SIZE];

#define UCX_TIMER_START(_x_) uint64_t ucx_timer##_x_ = timer_nano(); assert(0 == ucx_timer_mtx[_x_]); ucx_timer_mtx[_x_] = 1;
#define UCX_TIMER_END(_x_) ucx_timer[_x_] += timer_nano() - ucx_timer##_x_; assert(1 == ucx_timer_mtx[_x_]); ucx_timer_mtx[_x_] = 0;
