// 작성자 : 곽동환
// 이메일 : arbiter1225@gmail.com
// License : GPLv3
// 요약 : 멀티코어 프로그래밍을 위한 쓰래드풀 (비동기 작업, RTTask-loop를 수행하는 RT스래드풀)
// 최조 작성일 : 230522
// 최종 수정일 : 230815

#ifndef THREADPOOLIMPL_HPP
#define THREADPOOLIMPL_HPP

#define MAX_THREAD 64 // 이 프로그램에서 가지고 있을 최대 스래드 숫자.
#define MAX_RT_THREAD 128
#include <vector>
#include <queue>
#include <thread>
#include <functional>

#include <future>
#include <condition_variable>
#include <mutex>
#include <ctime>
using namespace std::literals;

// 일반 스래드 풀의 경우 스래드를 미리 만들어두고 비동기적으로 Task를 할당후 수행한다.
// RT 스래드 풀의 경우 RTTask를 먼저 생성한후 이를 RT-Loop 스래드에 하나씩 할당하는 형태로 동작한다.
// 즉 주기적인 loop()와 비주기적인 initialize 부분을 가지고 있는 RTTask만 RTThreadPool에 할당이 가능하다.

namespace kcpp
{
    using TASK = std::function<void()>;
    using QUEUE = std::queue<TASK>;
    using THREAD = std::thread;

    // RT Task를 담는 원형
    class RTTaskInterface
    {
    private:
        int64_t timeOffset_ns; // 시스템 기본 시간에 얼마나 오프셋을 주고 시작할지
        int64_t loop_ns;
        struct sched_param priority;
        cpu_set_t cpuAffinity;

    public:
        RTTaskInterface();
        virtual ~RTTaskInterface();

        void setTimeOffset(int64_t _time);
        void setLoop_ns(int64_t _time);

        void setPriority(int _priority); // 95~1 사이의 값만 허용한다.
        void setCpuAffinity(int _cpuAffinity);

        inline int64_t getLoop_ns();
        inline int64_t getTimeOffset_ns();
        struct sched_param getPriority();
        inline cpu_set_t getCpuAffinity();
        virtual void initialize(); // 초기화 코드는 필수가 아니며, 클래스 밖에 선언하여도 된다.
        virtual void loop() = 0;
    };

    class ThreadPool
    {
    private:
        QUEUE task_q;
        bool fempty_break = false;
        std::atomic<bool> is_ready[MAX_THREAD]; // thread가 사용가능한지 확인, true 사용가능 , false 사용불가능
    protected:
        std::vector<THREAD> v;
        std::mutex m;
        bool fStop = false;
        std::condition_variable cv;

    public:
        ThreadPool(int cnt, bool fempty_break = false);
        ThreadPool(bool fempty_break = false);
        static ThreadPool *create(); // static create function

        void Setfempty_break(bool flag); // if flag "fempty_break" is true, queue is empty => close thread;
        void Stop();                     // Stop ThreadPool
        virtual int GetThreadCnt();      // get workers (thread cnt)
        int GetThreadStatus(int cnt);    // get thread status | input thread # , ex) 0,1,2,3...
        template <typename _Func, typename... _BoundArgs>
        inline decltype(auto) AddTask(_Func &&task, _BoundArgs &&...args);

        virtual ~ThreadPool();
        ThreadPool(const ThreadPool &_new) = delete;
        ThreadPool &operator=(const ThreadPool &_new) = delete;

    private:
        template <typename F, typename... ARGS>
        decltype(auto) __AddTask(F task, ARGS &&...args);
        void _PoolThreadMain(int i);
        bool _IsEmptyBreak(); // check fempty_break
        void _Initialize_is_ready();
        bool _IsThreadReady(int cnt);

    }; // ThreadPool

    // RT Latency 데이터 저장
    struct RTlatency
    {
        int RTThreadID;
        double maxDuration = 0.0;
        double avgDuration = 0.0;
        long excced_time = 0;
    };
    class RTThreadPool : public ThreadPool
    {
        // 추가로 코딩해야할 부분 23.08.27 -> RT Task Interface를 삭제 하는 부분. (삭제하고 nullprt을 붙여넣는다. )
        // 전역 stop fRTSTop 이외에도 std::vector<bool> localRTStop을 만들고 
        // RT 객체를 삭제하고 지금 if(fRTStop) 부분을 수정해서 개별 task를 정지하는 코드를 작성한다.
        // bool fStop을 이용하여 정지 신호 생성
        // 만약 rt task를 추가할때 rtTask 여기서 nullptr이며, rtThread_isREady = false 인 곳에 먼저 
        // 넣어서 loop를 설정한다. 이때 이 id를 할당받아서 신규 테스크를 실행시키도록 한다. 
        // 참고로 새롭게 시작하는 RT 스래드의 시작 시간 규칙은 바뀌어야 하므로 PoolRTThreadMain함수 말고 다른함수 하나 
        // 만들어서 새롭게 시작을 수행한다. 참고로 이건 thradMainHelper함수도 추가로 만들고 수정해서 코드를 작성한다.
    private:
        std::vector<THREAD> RTthread;
        std::vector<RTTaskInterface *> rtTask;
        std::vector<bool> rtThread_isReady; // true = 실행중. false = 실행 중지됨. 
        std::vector<RTlatency> latency;
        std::mutex rtTaskM; // RT 스래드들을 추가하고 삭제할때
        std::mutex latencyM;
        bool fRTStop = false;
        bool is_addTask = false;

        struct timespec baseTime_ns;            // RT Task의 기준이 되는 시간.
        int64_t baseOffset_ns = 1000000 * 1000; // 1s 이후를 Base로 두고 RT Loop 동작 시작

        // RT TASK 동시 시작을 위한 mutex와 conditional_variable
        std::mutex rtStartM;
        std::condition_variable rtStartCv;
        bool startFlag = false;

    public:
        RTThreadPool(int cnt, bool fempty_break = false); // 공간 할당과 함께 초기화.
        RTThreadPool(bool fempty_break = false);          // 기본 초기화.
        static RTThreadPool *create();                    // static create function

        void Stop();          // Stop Thread
        void startRT();       // Start RT Thread
        int GetRTThreadCnt(); // get workers
        int GetRTThreadStatus(int cnt);
        void addRTTask(RTTaskInterface *_task);

        ~RTThreadPool();
        RTThreadPool(const RTThreadPool &_new) = delete;
        RTThreadPool &operator=(const RTThreadPool &_new) = delete;

    private:
        void _setRTBaseClock();                   // RT의 기준 시계를 현제 시계로 동작하는 코드
        void __addRTTask(RTTaskInterface *_task); // RTTask 추가 내부 함수
        void *_PoolRTThreadMain(int i);           // 스래드 풀 main - start시 1초 정지후 동시 시작
        static void *threadMainHelper(void *context);

        // 나노초 연산 라이브러리
        struct timespec _addTimeSpecByNs(struct timespec ts, int64_t ns);
        struct timespec __subtractTimeSpecByTimeSpec(struct timespec ts1, struct timespec ts2);
        struct timespec __addTimeSpecByTimeSpec(struct timespec ts1, struct timespec ts2);
        int64_t _quotientTimeSpecByNs(struct timespec ts, int64_t ns);
        inline int64_t _TimeSpecToNs(struct timespec ts);
        inline struct timespec _NsToTimeSpec(int64_t ns);
        void LockMemory();
    };

}

#include "tpp/threadpool.tpp"

#endif

// THREADPOOLIMPL_HPP
//****************************************************************************************//
// your work function
/*
#include<threadpool.hpp>
int add(int a, int b)
{
   std::this_thread::sleep_for(5s);
   return a + b;
}
using namespace kcpp;
*/
//****************************************************************************************//
// example 1 get result ASAP
// 기본사용법. 스래드 풀은 작업이 없으면 작업완료와 함께 죽어버립니다.
/*
int main()
{
   ThreadPool tp(3);

   std::future<int> ft = tp.AddTask(add, 1, 2);  //add 함수 int a = 1, int b = 2를 넣어준다.
   std::cout << "continue main" << std::endl;
   int ret = ft.get(); //std::future<int>에서 get 객채를 이용하여 데이터 획득
   std::cout << ret << std::endl;
   ft = tp.AddTask(add, 1, 2);
   ret = ft.get();
   std::cout << ret << std::endl;
}
*/

//****************************************************************************************//
// example 2 get result vectors
// 결과를 vector 형태로 받아서 사용하기 .
/*
int main()
{
    ThreadPool tp;
    int a = 1;
    std::vector<std::future<int>> vout;
    std::cout << "input number : " << std::endl;
    while(a != 0)
    {
        std::cin >> a;
        if(a != 0 )	{ vout.emplace_back(tp.AddTask(add, a, 2)); }
    }
    for (auto& m:vout) 	{ std::cout << m.get() << std::endl; }
}
*/
//****************************************************************************************//
// example 3 threadpool is not die until main function is die
// 스래드 풀이 메인스래드가 죽거나 Stop을 부르기 전까지는 안죽습니다.
/*
int main()
{
    ThreadPool tp(false);
    int a = 1;
    std::cout << "total thread cnt : " << tp.GetThreadCnt() << std::endl;

    while (true)
    {
        std::cout << "insert number" << std::endl;
        {
            std::vector<std::future<int>> vout;
            while (a != 0)
            {
                std::cin >> a;
                if (a != 0)
                {
                    vout.emplace_back(tp.AddTask(add, a, 2));
                }
            }
            for (auto &m : vout)
            {
                std::cout << "result :" << m.get() << std::endl;
            }
        }
        std::cout << "again? (if want start enter 0) : ";
        std::cin >> a;
        if (a == 0)
            break;
    }
}
*/
//****************************************************************************************//
// example 4 with class memberfunction
// 클래스 맴버 함수를 작업으로 등록시키는방법
/*
#include <functional>
#include "headers.h"
#include <iostream>
class add_class
{
    int a, b;
    public:
    add_class() { a= 0; b = 0;}
    add_class(int a, int b) : a(a), b(b) {}

    int add(int x, int y)
    {
        std::this_thread::sleep_for(2s);
        return x + y;
    }
};
int main()
{
    ThreadPool tp(false);
    int a = 1;
    add_class add_instance;

    std::cout << "total thread cnt : " << tp.GetThreadCnt() << std::endl;
    std::cout <<  tp.GetThreadStatus(2) << std::endl; //number 2 thread status , 0 = false 1= true;

    while (true)
    {
        std::cout << "insert number" << std::endl;
        {
            std::vector<std::future<int>> vout;
            while (a != 0)
            {
                std::cin >> a;
                std::cin >> a;
                if (a != 0)
                {
                    vout.emplace_back(tp.AddTask(&add_class::add, &add_instance,2,a));
                    //vout.emplace_back(tp.AddTask(add,2,a));
                }
            }
            for (auto &m : vout)
            {
                std::cout << "result :" << m.get() << std::endl;
            }
        }
        std::cout << "again? (if want start enter 0) : ";
        std::cin >> a;
        if (a == 0)
            break;
    }
}
*/
//****************************************************************************************//
// example 5 use of static member pointer
// static 으로 객체를 생성하고 싶을때 <>를 써줘야 한다.
/*
#include "headers.h"
using namespace kcpp;
int main()
{
    ThreadPool* tp = ThreadPool::create(false);
    int a = 1;
    add_class add_instance;

    std::cout << "total thread cnt : " << tp->GetThreadCnt() << std::endl;
    std::cout <<  tp->GetThreadStatus(2) << std::endl; //number 2 thread status , 0 = false 1= true;

    while (true)
    {
        std::cout << "insert number" << std::endl;
        {
            std::vector<std::future<int>> vout;
            while (a != 0)
            {
                std::cin >> a;
                std::cin >> a;
                if (a != 0)
                {
                    vout.emplace_back(tp->AddTask(&add_class::add, &add_instance,2,a));
                    //vout.emplace_back(tp.AddTask(add,2,a));
                }
            }
            for (auto &m : vout)
            {
                std::cout << "result :" << m.get() << std::endl;
            }
        }
        std::cout << "again? (if want start enter 0) : ";
        std::cin >> a;
        if (a == 0)
            break;
    }
}
*/