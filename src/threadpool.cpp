#include <threadpool.hpp>

#include <memory>
#include <sched.h>
#include <iostream>
#include <sys/mman.h>
#include <stdexcept>
#include <atomic>

namespace kcpp
{
    using TASK = std::function<void()>;
    using QUEUE = std::queue<TASK>;
    using THREAD = std::thread;

    // RTTaskInterface
    RTTaskInterface::RTTaskInterface()
    {
        CPU_ZERO(&cpuAffinity);
        setPriority(40);
        timeOffset_ns = 0;
        loop_ns = 0;
    }
    RTTaskInterface::~RTTaskInterface(){};

    void RTTaskInterface::setTimeOffset(int64_t _time)
    {
        timeOffset_ns = _time;
    }
    void RTTaskInterface::setLoop_ns(int64_t _time)
    {
        loop_ns = _time;
    }

    // 95~1 사이의 값만 허용한다.
    void RTTaskInterface::setPriority(int _priority)
    {
        if (_priority > 95)
            _priority = 95;
        if (_priority < 1)
            _priority = 1;
        priority.sched_priority = _priority;
    }
    void RTTaskInterface::setCpuAffinity(int _cpuAffinity)
    {
        CPU_SET(_cpuAffinity, &cpuAffinity);
    }

    int64_t RTTaskInterface::getLoop_ns() { return loop_ns; }
    int64_t RTTaskInterface::getTimeOffset_ns() { return timeOffset_ns; }
    struct sched_param RTTaskInterface::getPriority() { return priority; }
    cpu_set_t RTTaskInterface::getCpuAffinity() { return cpuAffinity; }
    void RTTaskInterface::initialize() {}

    // ThreadPool
    ThreadPool::ThreadPool(int cnt, bool fempty_break) : fempty_break(fempty_break)
    {
        if (cnt > MAX_THREAD)
            cnt = MAX_THREAD;
        for (int i = 0; i < cnt; i++)
        {
            is_ready[i].store(true, std::memory_order_release);
            v.emplace_back(&ThreadPool::_PoolThreadMain, this, i);
        }
    }
    ThreadPool::ThreadPool(bool fempty_break)
    {
        int cnt = std::thread::hardware_concurrency() - 1;
        if (cnt > MAX_THREAD)
            cnt = MAX_THREAD;
        if (cnt < 1)
            cnt = 1;
        for (int i = 0; i < cnt; i++)
        {
            is_ready[i].store(true, std::memory_order_release);
            v.emplace_back(&ThreadPool::_PoolThreadMain, this, i);
        }
    }
    ThreadPool *ThreadPool::create() { return new ThreadPool; }
    ThreadPool::~ThreadPool()
    {
        if (!fStop)
        {
            {
                std::lock_guard<std::mutex> g(m);
                fStop = true;
            }
            cv.notify_all();
            for (auto &t : v)
            {
                if (t.joinable())
                {
                    t.join();
                }
                else
                {
                    std::cout << "unalbe to join thread" << std::endl;
                }
            }
        }
    }

    // if flag "fempty_break" is true, queue is empty => close thread;
    void ThreadPool::Setfempty_break(bool flag) { fempty_break = flag; }
    // Stop ThreadPool
    void ThreadPool::Stop()
    {
        // delete this;
        std::cout << "스래드풀 정지 " << std::endl;
        {
            std::lock_guard<std::mutex> g(m);
            fStop = true;
        }
        cv.notify_all();
    }
    // get workers (thread cnt)
    int ThreadPool::GetThreadCnt() { return v.size(); }
    // get thread status | input thread # , ex) 0,1,2,3...
    int ThreadPool::GetThreadStatus(int cnt)
    {
        if (cnt > v.size() || cnt < 0)
            return false;
        return _IsThreadReady(cnt);
    }
    void ThreadPool::_PoolThreadMain(int i)
    {
        is_ready[i].store(true, std::memory_order_release);
        while (true)
        {
            is_ready[i].store(false, std::memory_order_release);
            TASK task;
            {
                std::unique_lock<std::mutex> ul(m);
                cv.wait(ul, [this]()
                        { return fStop || !task_q.empty(); });

                // if (fStop == true && _IsEmptyBreak())
                if (fStop == true)
                {
                    break;
                }
                task = task_q.front();
                task_q.pop();
            }
            task();
            is_ready[i].store(true, std::memory_order_release);
        }
    }
    // check fempty_break
    bool ThreadPool::_IsEmptyBreak() { return fempty_break && task_q.empty(); }
    void ThreadPool::_Initialize_is_ready()
    {
        for (int i = 0; i < MAX_THREAD; i++)
            is_ready[i].store(false, std::memory_order_release);
    }
    bool ThreadPool::_IsThreadReady(int cnt)
    {
        if (cnt >= MAX_THREAD)
            return false;
        if (cnt < 0)
            cnt = 0;
        return is_ready[cnt].load(std::memory_order_acquire);
    }

    // RTThreadPool

    // 공간 할당과 함께 초기화.
    RTThreadPool::RTThreadPool(int cnt, bool fempty_break) : ThreadPool(cnt, fempty_break)
    {
        RTthread.clear();
        rtThread_isReady.clear();
        rtTask.clear();
        RTthread.reserve(MAX_RT_THREAD);
        rtThread_isReady.reserve(MAX_RT_THREAD);
        rtTask.reserve(MAX_RT_THREAD);
        LockMemory();
    }
    // 기본 초기화.
    RTThreadPool::RTThreadPool(bool fempty_break) : ThreadPool(fempty_break)
    {
        RTthread.clear();
        rtThread_isReady.clear();
        rtTask.clear();
        RTthread.reserve(MAX_RT_THREAD);
        rtThread_isReady.reserve(MAX_RT_THREAD);
        rtTask.reserve(MAX_RT_THREAD);
        LockMemory();
    }
    RTThreadPool *RTThreadPool::create() { return new RTThreadPool; } // static create function
    // Stop Thread
    void RTThreadPool::Stop()
    {
        if (!fRTStop)
        {
            std::cout << "RT 스래드풀 정지" << std::endl;
            {
                std::lock_guard<std::mutex> _m(rtTaskM);
                fRTStop = true;
            }
            for (auto &t : RTthread)
            {
                if (t.joinable())
                    t.join();
            }

            bool abletoEnd = true;
            while (true)
            {
                abletoEnd = true;
                std::atomic_thread_fence(std::memory_order_seq_cst);
                std::this_thread::sleep_for(std::chrono::microseconds(200));
                for (int i = 0; i < rtThread_isReady.size(); i++)
                {
                    std::lock_guard<std::mutex> lu(rtTaskM);
                    if (rtThread_isReady.at(i) == true)
                    {
                        abletoEnd = false;
                    }
                }
                if (abletoEnd)
                {
                    break; // 종료 가능시 true;
                }
            }

            // 기록들 출력
            for (auto _latency : latency)
            {
                std::cout << "RT:" << _latency.RTThreadID << " > Average Duration(us): " << _latency.avgDuration << "| MaxDuration(us) : " << _latency.maxDuration << "| Total Fail : " << _latency.excced_time << std::endl;
            }
        }
    }

    // Start RT Thread
    void RTThreadPool::startRT()
    {
        // 할당된 모든 RT 스래드들의 동작이 완료되었음을 기다린다.
        while (true)
        {
            std::this_thread::sleep_for(std::chrono::microseconds(500));
            for (auto _f : rtThread_isReady)
            {
                if (!_f)
                    continue;
            }
            break;
        }
        {
            std::unique_lock<std::mutex> lock(rtStartM);
            startFlag = true;
        }
        _setRTBaseClock();
        rtStartCv.notify_all();
    }

    // get workers
    int RTThreadPool::GetRTThreadCnt()
    {
        return RTthread.size();
    }
    int RTThreadPool::GetRTThreadStatus(int cnt)
    {
        if (cnt > RTthread.size() || cnt < 0)
            return false;
        return rtThread_isReady.at(cnt);
    }

    void RTThreadPool::addRTTask(RTTaskInterface *_task)
    {
        this->__addRTTask(_task);
    }
    RTThreadPool::~RTThreadPool()
    {
        this->Stop();
    }

    // RT의 기준 시계를 현제 시계로 동작하는 코드
    void RTThreadPool::_setRTBaseClock()
    {
        clock_gettime(CLOCK_MONOTONIC, &baseTime_ns);
        baseTime_ns = _addTimeSpecByNs(baseTime_ns, baseOffset_ns);
    }
    // RTTask 추가 내부 함수
    void RTThreadPool::__addRTTask(RTTaskInterface *_task)
    {
        while (true)
        {
            if (is_addTask)
            {
                std::this_thread::sleep_for(std::chrono::microseconds(100));
                continue;
            }
            break;
        }
        std::atomic_thread_fence(std::memory_order_seq_cst);
        is_addTask = true;
        std::atomic_thread_fence(std::memory_order_seq_cst);
        rtTask.emplace_back(_task);
        {
            std::lock_guard<std::mutex> lu(rtTaskM);
            rtThread_isReady.emplace_back(false);
        }
        std::thread _newThread;
        // 작업 우선순위 및 RT 정책 설정
        pthread_attr_t attr;
        struct sched_param params;
        params = _task->getPriority();

        pthread_attr_init(&attr);
        pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
        pthread_attr_setschedparam(&attr, &params);

        // cpu 할당
        cpu_set_t mask;
        mask = _task->getCpuAffinity();
        bool _isCpuAssigned = false;
        for (int j = 0; j < CPU_SETSIZE; j++)
        {
            if (CPU_ISSET(j, &mask))
            {
                _isCpuAssigned = true;
            }
        }
        if (!_isCpuAssigned)
        {
            std::cout << "RTThreadPool Warring : Cpu Affinity is not set for RTTask " << std::endl;
            std::cout << "- automatically SET Cpu to 0 " << std::endl;
            CPU_SET(0, &mask);
        }

        pthread_attr_setaffinity_np(&attr, sizeof(mask), &mask);

        // pthread 생성 후 std::thread에 할당
        pthread_t native_thread = _newThread.native_handle();

        pthread_create(&native_thread, &attr, &RTThreadPool::threadMainHelper, this);
        RTthread.emplace_back(std::move(_newThread));

        pthread_attr_destroy(&attr);
    }
    // 스래드 풀 main - start시 1초 정지후 동시 시작
    void *RTThreadPool::_PoolRTThreadMain(int i)
    {
        int _i = i;
        std::atomic_thread_fence(std::memory_order_seq_cst);
        int64_t thisTaskLoop_ns = this->rtTask.at(_i)->getLoop_ns();

        // 초기화 부분
        this->rtTask.at(_i)->initialize();
        {
            std::lock_guard<std::mutex> lu(rtTaskM);
            rtThread_isReady.at(_i) = true;
        }
        // logger 초기화
        {
            std::lock_guard<std::mutex> _latency(latencyM);
            RTlatency _temp;
            latency.emplace_back(std::move(_temp));
        }

        // For latency test
        unsigned long long N = 0;
        double maxDuration = 0.0;
        double avgDuration = 0.0;
        double Duration = 0.0;
        double DurationLimit = (double)thisTaskLoop_ns / 1000;
        long _excced_time = 0;

        // 코드 시작을 위한 중단점 여기서부터 동작을 수행한다.
        {
            std::unique_lock<std::mutex> loc(rtStartM);
            rtStartCv.wait(loc, [&]
                           { return startFlag; });
        }
        struct timespec thisThreadBaseTime;
        // struct timespec deltaBaseTime;
        struct timespec nextwakeup_time;
        nextwakeup_time = _addTimeSpecByNs(baseTime_ns, this->rtTask.at(_i)->getTimeOffset_ns()); // offset을 더해줌
        // clock_gettime(CLOCK_MONOTONIC,&thisThreadBaseTime);
        // deltaBaseTime = __subtractTimeSpecByTimeSpec(baseTime_ns,thisThreadBaseTime);
        //
        //// ( (지금시간-기준시간) % 주기 * 주기 ) + offset + 기준시간
        // thisThreadBaseTime = _NsToTimeSpec(_quotientTimeSpecByNs(deltaBaseTime, thisTaskLoop_ns) * thisTaskLoop_ns
        //						+ this->rtTask.at(i)->getTimeOffset_ns() + _TimeSpecToNs(thisThreadBaseTime));
        // nextwakeup_time = thisThreadBaseTime;
        // 지정 시간까지 sleep
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &nextwakeup_time, NULL);

        // RT 루프 부분
        while (true)
        {
            N++;
            auto start_time = std::chrono::high_resolution_clock::now();
            this->rtTask.at(_i)->loop();
            auto end_time = std::chrono::high_resolution_clock::now();
            Duration = std::chrono::duration<double, std::micro>(end_time - start_time).count();
            if (maxDuration < Duration)
            {
                maxDuration = Duration;
            }
            if (Duration > DurationLimit)
            {
                _excced_time++;
            }
            avgDuration += (Duration - avgDuration) / static_cast<double>(N);
            {
                std::lock_guard<std::mutex> ul(rtTaskM);
                if (fRTStop)
                {
                    break;
                }
            }
            nextwakeup_time = _addTimeSpecByNs(nextwakeup_time, thisTaskLoop_ns);
            clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &nextwakeup_time, NULL);
        }

        RTlatency _latency;
        _latency.RTThreadID = i;
        _latency.maxDuration = maxDuration;
        _latency.avgDuration = avgDuration;
        _latency.excced_time = _excced_time;

        {
            std::lock_guard<std::mutex> _lock_latency(latencyM);
            this->latency.at(_i) = _latency;
        }

        // std::cout << "RT:"<<i<<" > Average Duration(us): " << avgDuration << "| MaxDuration(us) : " << maxDuration << "Total Fail : " << _excced_time << std::endl;

        std::atomic_thread_fence(std::memory_order_seq_cst);
        {
            std::lock_guard<std::mutex> lu(rtTaskM);
            rtThread_isReady.at(_i) = false;
        }
    }

    void *RTThreadPool::threadMainHelper(void *context)
    {
        std::atomic_thread_fence(std::memory_order_seq_cst);
        int target = ((RTThreadPool *)context)->rtTask.size() - 1;
        std::atomic_thread_fence(std::memory_order_seq_cst);
        ((RTThreadPool *)context)->is_addTask = false;
        return ((RTThreadPool *)context)->_PoolRTThreadMain(target);
    }

    // 나노초 연산 라이브러리
    struct timespec RTThreadPool::_addTimeSpecByNs(struct timespec ts, int64_t ns)
    {
        ts.tv_nsec += ns;
        while (ts.tv_nsec >= 1000000000)
        {
            ++ts.tv_sec;
            ts.tv_nsec -= 1000000000;
        }

        while (ts.tv_nsec < 0)
        {
            --ts.tv_sec;
            ts.tv_nsec += 1000000000;
        }
        return ts;
    }
    struct timespec RTThreadPool::__subtractTimeSpecByTimeSpec(struct timespec ts1, struct timespec ts2)
    {
        ts1.tv_nsec -= ts2.tv_nsec;
        ts1.tv_sec -= ts2.tv_sec;
        while (ts1.tv_nsec >= 1000000000)
        {
            ++ts1.tv_sec;
            ts1.tv_nsec -= 1000000000;
        }

        while (ts1.tv_nsec < 0)
        {
            --ts1.tv_sec;
            ts1.tv_nsec += 1000000000;
        }
        return ts1;
    }
    struct timespec RTThreadPool::__addTimeSpecByTimeSpec(struct timespec ts1, struct timespec ts2)
    {
        ts1.tv_nsec += ts2.tv_nsec;
        ts1.tv_sec += ts2.tv_sec;
        while (ts1.tv_nsec >= 1000000000)
        {
            ++ts1.tv_sec;
            ts1.tv_nsec -= 1000000000;
        }

        while (ts1.tv_nsec < 0)
        {
            --ts1.tv_sec;
            ts1.tv_nsec += 1000000000;
        }
        return ts1;
    }
    int64_t RTThreadPool::_quotientTimeSpecByNs(struct timespec ts, int64_t ns)
    {
        if (ns <= 0)
            return -1;
        int64_t t_ns = static_cast<int64_t>(ts.tv_sec) * 1000000000 + ts.tv_nsec;
        return t_ns % ns;
    }
    int64_t RTThreadPool::_TimeSpecToNs(struct timespec ts)
    {
        return static_cast<int64_t>(ts.tv_sec) * 1000000000 + ts.tv_nsec;
    }
    struct timespec RTThreadPool::_NsToTimeSpec(int64_t ns)
    {
        struct timespec ts;
        ts.tv_sec = ns / 1000000000;
        ts.tv_nsec = ns / 1000000000;
        return ts;
    }
    void RTThreadPool::LockMemory()
    {
        int ret = mlockall(MCL_CURRENT | MCL_FUTURE);
        if (ret)
        {
            throw std::runtime_error{"Fail to Lock Memory"};
        }
    }

}
