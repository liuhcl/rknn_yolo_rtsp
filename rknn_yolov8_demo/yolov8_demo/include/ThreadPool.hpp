#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <cassert>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>

namespace dpool
{

    class ThreadPool
    {
    public:
        using MutexGuard = std::lock_guard<std::mutex>; //互斥量包装器
        using UniqueLock = std::unique_lock<std::mutex>; //通用的互斥量管理类
        using Thread = std::thread;
        using ThreadID = std::thread::id;
        using Task = std::function<void()>;

        ThreadPool()
            : ThreadPool(Thread::hardware_concurrency())
        {
        }

        explicit ThreadPool(size_t maxThreads)
            : quit_(false), //线程池是否应该终止运行
              currentThreads_(0), //当前活动的线程数
              idleThreads_(0), //当前空闲的线程数
              maxThreads_(maxThreads) //线程池的最大线程数
        {
        }

        // disable the copy operations
        ThreadPool(const ThreadPool &) = delete;
        ThreadPool &operator=(const ThreadPool &) = delete;

        ~ThreadPool()
        {
            {
                MutexGuard guard(mutex_); //保护线程池的状态和任务队列的访问
                quit_ = true;
            }
            cv_.notify_all(); //等待所有线程结束

            for (auto &elem : threads_) //销毁所有创建的线程对象
            {
                assert(elem.second.joinable()); //能否被join
                elem.second.join();
            }
        }

        /*将任务提交给线程池，并返回一个未来对象*/
        template <typename Func, typename... Ts>
        auto submit(Func &&func, Ts &&...params) //接受一个类型参数 Func 和一个参数包 Ts (&&)右值引用
            -> std::future<typename std::result_of<Func(Ts...)>::type> 
            //表示 submit 函数的返回类型是一个 std::future 对象，它的模板参数是由 Func 的返回类型决定的。
        {
            /*使用(绑定器)bind将可调用对象和参数绑定在一起，形成一个无参的可执行对象*/
            auto execute = std::bind(std::forward<Func>(func), std::forward<Ts>(params)...);

            /*使用（类型萃取器）type 得到可调用对象的返回类型*/
            using ReturnType = typename std::result_of<Func(Ts...)>::type;
            /*并定义一个对应类型的打包任务（packaged task）。打包任务是一种将可执行对象和未来对象关联起来的工具*/
            using PackagedTask = std::packaged_task<ReturnType()>;

            auto task = std::make_shared<PackagedTask>(std::move(execute)); //创建一个打包任务的共享指针
            auto result = task->get_future(); //从打包任务中获取异步操作的结果

            MutexGuard guard(mutex_);
            assert(!quit_); //在互斥锁保护下，检查线程池是否已经终止
            //存储任务对象
            tasks_.emplace([task]()
                           { (*task)(); });
            //是否有空闲的线程
            if (idleThreads_ > 0) //有
            {
                cv_.notify_one(); //通知一个线程去执行任务
            }
            else if (currentThreads_ < maxThreads_) //没有，就检查是否达到了最大线程数
            {
                Thread t(&ThreadPool::worker, this); //创建一个新线程
                assert(threads_.find(t.get_id()) == threads_.end()); //确保线程的唯一性和正确性
                threads_[t.get_id()] = std::move(t); //将这个新线程加入到线程容器中
                ++currentThreads_; //线程数量加 1
            }

            return result; //返回异步操作的结果
        }

        //返回当前活动的线程数
        size_t threadsNum() const
        {
            MutexGuard guard(mutex_);
            return currentThreads_;
        }

    private:
        // 私有成员函数，它是每个线程执行的循环函数
        // 负责从任务队列中获取并执行任务，或者在没有任务时等待或退出
        void worker()
        {
            while (true)
            {
                Task task;
                {
                    //创建一个唯一锁对象（unique lock），并使用互斥锁初始化它。唯一锁是一种可以在作用域之外保持锁定状态的工具
                    UniqueLock uniqueLock(mutex_);
                    ++idleThreads_; //增加空闲线程数
/*
                    这行是用来让当前线程在条件变量（cv_）上等待一段时间（WAIT_SECONDS）
                    或者直到满足某个条件（quit_为真或者tasks_不为空）为止。
                    它使用了标准库中的std::condition_variable类，它是一种可以让线程在没有任务时等待，或者在有任务时唤醒的工具。
                    它有一个名为wait_for的方法，它接受三个参数：
*/
                    auto hasTimedout = !cv_.wait_for(uniqueLock,
                                                     std::chrono::seconds(WAIT_SECONDS), //指定一个时间段 
                                                     [this]()
                                                     {
                                                         return quit_ || !tasks_.empty(); //条件
                                                     });
                    --idleThreads_;
                    if (tasks_.empty()) 
                    {
                        if (quit_) //如果在等待过程中收到通知或者线程池终止，就减少空闲线程数（idleThreads_）
                        {
                            --currentThreads_;
                            return;
                        }
                        if (hasTimedout) //如果等待超时了
                        {
                            /*避免线程池中有过多的空闲线程占用资源*/
                            --currentThreads_; //减少线程数
                            joinFinishedThreads(); //回收已经结束的线程
                            finishedThreadIDs_.emplace(std::this_thread::get_id()); //并将自己的线程ID加入到一个队列中
                            return;
                        }
                    }
                    //如果任务队列不为空，就从任务队列中取出第一个任务
                    task = std::move(tasks_.front());
                    tasks_.pop(); //将队列中的第一个元素删除
                }
                task(); //执行取出的任务
            }
        }

        /*回收已经结束的线程的函数*/
        void joinFinishedThreads()
        {
            while (!finishedThreadIDs_.empty()) //检查已经结束的线程ID队列是否为空
            {
                auto id = std::move(finishedThreadIDs_.front()); //取出第一个线程ID
                finishedThreadIDs_.pop(); //删除这个线程
                auto iter = threads_.find(id); //在线程容器中查找这个线程

                assert(iter != threads_.end()); //这个线程不是最后一个
                assert(iter->second.joinable()); //该线程对象存在且可连接

                iter->second.join(); //等待该线程结束并回收资源
                threads_.erase(iter); //删除该线程对象
            }
        }

        static constexpr size_t WAIT_SECONDS = 2;

        bool quit_;
        size_t currentThreads_;
        size_t idleThreads_;
        size_t maxThreads_;

        mutable std::mutex mutex_;
        std::condition_variable cv_;
        std::queue<Task> tasks_;
        std::queue<ThreadID> finishedThreadIDs_;
        std::unordered_map<ThreadID, Thread> threads_;
    };

    constexpr size_t ThreadPool::WAIT_SECONDS;

} // namespace dpool

#endif /* THREADPOOL_H */