namespace kcpp
{
    using TASK = std::function<void()>;
    using QUEUE = std::queue<TASK>;
    using THREAD = std::thread;

    // ThreadPool
    template <typename _Func, typename... _BoundArgs>
    inline decltype(auto) ThreadPool::AddTask(_Func &&task, _BoundArgs &&...args)
    {
        auto ret = __AddTask(std::bind(task, std::forward<_BoundArgs>(args)...));
        return ret;
    }

    template <typename F, typename... ARGS>
    decltype(auto) ThreadPool::__AddTask(F task, ARGS &&...args)
    {
        using RT = decltype(task(std::forward<ARGS>(args)...));
        auto p = std::make_shared<std::packaged_task<RT()>>(std::bind(task, std::forward<ARGS>(args)...));
        std::future<RT> ret = p->get_future();
        {
            std::lock_guard<std::mutex> g(m);
            task_q.push([p]()
                        { (*p)(); });
        }
        cv.notify_one();
        return ret;
    }
}