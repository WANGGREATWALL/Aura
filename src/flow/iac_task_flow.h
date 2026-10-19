#pragma once

#include <functional>
#include <future>
#include <memory>
#include <string>

#include "iac_default_defs.h"

namespace iac {
namespace vaif {

using TaskFuncT = std::shared_ptr<std::packaged_task<int()>>;

/* Task */
class Task;

/* TaskFlow */
class TaskFlow final {
public:
    TaskFlow(int maxParallelNum, const std::string& threadName, int logLevel = 3 /* VDKDebugLevelInfo */);
    ~TaskFlow();

    // task should be a function that return int(VDKResult) value.
    template<typename F, typename... DataT>
    Task* add(const std::string& name, F&& taskFunc, DataT&&... params) {
        static_assert(std::is_same_v<typename std::invoke_result_t<F, DataT...>, int>, "Task function must return int(VDKResult)!");
        return createTask(name, std::make_shared<std::packaged_task<int()>>(std::bind(std::forward<F>(taskFunc), std::forward<DataT>(params)...)));
    }

    // if t1 is executed, t2 must be executed.
    int pair(const Task* t1, const Task* t2);

    // these tasks should be executed sequentially.
    int link(const std::initializer_list<Task*>& tasks, bool runInSameThread = false);

    // bind these tasks with these threads, this means, these tasks can only be executed on one of these threads.
    int bind(const std::initializer_list<Task*>& tasks, std::initializer_list<int> threadIds /* values should between [0, maxParallelNum) */);

    int run();

    void abort();

private:
    Task* createTask(const std::string& name, TaskFuncT taskFunc);

private:
    void* mImpl = nullptr;
};

}
}
