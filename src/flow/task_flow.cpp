#include "iac_task_flow.h"
#include "iac_vaif_def.h"
#include "iac_default_defs.h"

#include <list>
#include <queue>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <future>
#include <algorithm>
#include <mutex>
#include <unistd.h>
#include <condition_variable>
#include <stdatomic.h>

NS_BEG

enum TaskFlowState {
    IS_INIT = 0,
    IS_RUNABLE,
};

/* Task */
class Task final {
public:
    Task(int id, const std::string& name, const TaskFuncT& func) : mId(id), mLinkId(-1), mWeight(0), mIsDone(false), mIsLinked(false), mName(name), mFunc(func), mPrevs({}), mNexts({}) {
    }

    ~Task() {
    }

    bool isReady() const {
        for (auto& task: mPrevs) {
            if (!task->mIsDone) {
                return false;
            }
        }
        return true;
    }

    void run() {
        (*mFunc)();

        try {
            mRet = mFunc->get_future().get();
        } catch (const std::exception& e) {
            LOGE("Errorcode = 0x%x catchinfo = %s", VDKResultEExpired, e.what());
        } catch (...) {
            LOGE("Errorcode = 0x%x", VDKResultEExpired);
        }

        mIsDone = true;
    }

    void precede(Task* task) {
        bool isNewEle = std::find(this->mNexts.begin(), this->mNexts.end(), task) == this->mNexts.end();
        if (isNewEle) {
            this->mNexts.push_back(task);
            task->mPrevs.push_back(this);
        }
    }

    int weight() const {
        return mWeight;
    }

    void setWeight(int weight) {
        mWeight = weight;
    }

    const int getResult() const {
        return mRet;
    }

    const int getId() const {
        return mId;
    }

    const int getLinkId() const {
        return mLinkId;
    }

    const std::string getName() const {
        return mName;
    }

    const std::vector<Task*>& getPrevs() const {
        return mPrevs;
    }

    const std::vector<Task*>& getNexts() const {
        return mNexts;
    }

    const bool getIsLinked() const {
        return mIsLinked;
    }

    void setIsLinked(bool isLinked, int linkId) {
        mIsLinked = isLinked;
        mLinkId   = linkId;
    }

    const bool getIsDone() const {
        return mIsDone;
    }

private:
    int               mId;
    int               mRet;
    int               mLinkId;
    int               mWeight;
    bool              mIsDone;
    bool              mIsLinked;
    const std::string mName;
    const TaskFuncT   mFunc;
    std::vector<Task*> mPrevs;
    std::vector<Task*> mNexts;
};

/* Flow */
class Flow {
public:
    Flow(int maxParallelNum, const std::string& threadName, int logLevel) : mState(IS_INIT), mTaskNum(0), mLinkNum(0), mMaxParallelNum(maxParallelNum), mThreadName(threadName) {
        createThreadPool(mMaxParallelNum);
    }

    ~Flow() {
        destroyThreadPool();

        for (auto& task: mTasks) {
            delete task;
        }
        mTasks.clear();

        mThreads.clear();
        mThreadIds.clear();
        mReadyTasks.clear();
    }

    Task* createTask(const std::string& name, TaskFuncT taskFunc) {
        std::lock_guard<std::mutex> lock(mMutex);
        auto task = new Task{mTaskNum++, name, taskFunc};
        if (task) {
            mTasks.push_back(task);
        }
        return task;
    }

    int link(const std::initializer_list<Task*>& tasks, bool runInSameThread) {
        std::lock_guard<std::mutex> lock(mMutex);
        for (auto it = tasks.begin(); it != tasks.end(); it++) {
            if (std::find(mTasks.begin(), mTasks.end(), *it) == mTasks.end()) {
                LOGE("not added task %p cannot linked!", *it);
                return VDKResultEInvalidParam;
            }
        }

        // build task dependencies
        auto taskList = std::vector<Task*>(tasks);
        int size = taskList.size();
        for (int i = 0; i < size - 1; i++) {
            taskList[i]->precede(taskList[i + 1]);
        }

        // task which in other Links should be eliminated
        if (runInSameThread) {
            std::unordered_map<Task*, int> taskLinkIds;
            for (auto& task : tasks) {
                if (task->getIsLinked()) {
                    taskLinkIds[task] = task->getLinkId();
                }
            }

            int linkId = mLinkNum++;
            for (auto& pair : taskLinkIds) {
                linkId = std::min(linkId, pair.second);
            }
            for (auto& pair : taskLinkIds) {
                for (auto& task : mTasks) {
                    if (task->getLinkId() == pair.second) {
                        task->setIsLinked(true, linkId);
                    }
                }
            }
            for (auto& task : tasks) {
                task->setIsLinked(true, linkId);
            }
        }
        return VDKResultSuccess;
    }

    int run() {
        std::lock_guard<std::mutex> lock(mMutex);

        std::unordered_set<int> linkIds;
        for (auto& task : mTasks) {
            if (task->getLinkId() != -1) {
                linkIds.insert(task->getLinkId());
            }
        }
        mLinkNum = linkIds.size();

        LOGV("taskNum %-d", mTaskNum);
        for (auto& task : mTasks) {
            for (auto& it : task->getNexts()) {
                LOGV("taskId %2d iDegree %d edge: %-2d -> %-2d (%-25s -> %-25s)", task->getId(), task->getPrevs().size(), task->getId(), it->getId(), task->getName().c_str(), it->getName().c_str());
            }
        }

        LOGV("taskNum %-d linkNum %-d", mTaskNum, mLinkNum);
        for (auto& task : mTasks) {
            LOGV("taskId %-2d linkId %-2d taskName %-30s", task->getId(), task->getLinkId(), task->getName().c_str());
        }

        if (mState == IS_INIT) {
            if (isDAG()) {
                mState = IS_RUNABLE;
                assignWeight();
            } else {
                LOGE("not DAG graph!");
                return VDKResultEExpired;
            }
        }

        if (mState != IS_RUNABLE) {
            return VDKResultEBadState;
        }

        return taskLooper();
    }

    void abort() {
        atomic_store(&mAbortFlag, true);
        {
            std::lock_guard<std::mutex> lock(mThreadMutex);
            mThreadCond.notify_all();
        }

        {
            std::unique_lock<std::mutex> lock(mCallerMutex);
            mCallerCond.notify_one();
        }
    }

    bool isDAG() {
        std::vector<int> iDegree(mTaskNum, 0);
        std::vector<std::pair<int, int>> edges;
        for (int i = 0; i < mTaskNum; i++) {
            iDegree[i] = mTasks[i]->getPrevs().size();
            for (auto& it: mTasks[i]->getNexts()) {
                edges.push_back(std::make_pair(mTasks[i]->getId(), it->getId()));
            }
        }

        std::queue<int> zeroDegree;
        for (int i = 0; i < mTaskNum; i++) {
            if (iDegree[i] == 0) {
                zeroDegree.push(mTasks[i]->getId());
            }
        }

        int count = 0;
        while (!zeroDegree.empty()) {
            int id = zeroDegree.front();
            zeroDegree.pop();
            ++count;

            for (auto& edge : edges) {
                if (edge.first == id) {
                    --iDegree[edge.second];
                    if (iDegree[edge.second] == 0) {
                        zeroDegree.push(edge.second);
                    }
                }
            }
        }
        LOGV("count %d mTaskNum %d", count, mTaskNum);

        return count == mTaskNum;
    }

    void assignWeight() {
        std::vector<int> oDegree(mTaskNum, 0);
        std::vector<std::pair<int, int>> edges;
        for (int i = 0; i < mTaskNum; i++) {
            oDegree[i] = mTasks[i]->getNexts().size();
            for (auto& it: mTasks[i]->getPrevs()) {
                edges.push_back(std::make_pair(it->getId(), mTasks[i]->getId()));
            }
        }

        std::queue<int> zeroDegree;
        for (int i = 0; i < mTaskNum; i++) {
            if (oDegree[i] == 0) {
                zeroDegree.push(mTasks[i]->getId());
            }
        }

        int weight;
        while (!zeroDegree.empty()) {
            int id = zeroDegree.front();
            zeroDegree.pop();
            for (auto& edge : edges) {
                if (edge.second == id) {
                    --oDegree[edge.first];
                    if (oDegree[edge.first] == 0) {
                        zeroDegree.push(edge.first);
                        weight = std::max(mTasks[edge.first]->weight(), mTasks[id]->weight() + 1);
                        mTasks[edge.first]->setWeight(weight);
                    }
                }
            }
        }
    }

    int taskLooper() {
        std::list<Task*> todoTasks(mTasks.begin(), mTasks.end());
        std::list<Task*> doneTasks(mTasks.begin(), mTasks.end());

        mThreadIds.assign(mLinkNum, -1);
        mHasTaskDone = true;
        while (!doneTasks.empty()) {
            std::unique_lock<std::mutex> lock(mCallerMutex);
            mCallerCond.wait(lock, [&] { return mHasTaskDone || isAbort(); });
            if (isAbort()) {
                break;
            }

            mHasTaskDone = false;
            for (auto it = todoTasks.begin(); it != todoTasks.end();) {
                if ((*it)->isReady()) {
                    {
                        std::lock_guard<std::mutex> lock(mThreadMutex);
                        mReadyTasks.emplace_back(*it);
                        mThreadCond.notify_all();
                    }
                    it = todoTasks.erase(it);
                } else {
                    ++it;
                }
            }
            for (auto it = doneTasks.begin(); it != doneTasks.end();) {
                if ((*it)->getIsDone()) {
                    it = doneTasks.erase(it);
                } else {
                    ++it;
                }
            }
        }

        return VDKResultSuccess;
    }

    void createThreadPool(size_t threadCount) {
        for (int i = 0; i < threadCount; ++i) {
            mThreads.emplace_back([&] {
                if (mThreadName.size() > 0) {
                    pthread_setname_np(pthread_self(), mThreadName.substr(0, 15).c_str());
                }
                Task* task       = nullptr;
                bool  isHardLink = false;
                bool  isSoftLink = false;
                int   linkId     = -1;
                for (;;) {
                    {
                        std::unique_lock<std::mutex> lock(mThreadMutex);
                        mThreadCond.wait(lock, [&] {
                            if (isAbort())
                                return true;

                            if (mReadyTasks.empty()) {
                                return mThreadStop;
                            }
                            for (auto it = mReadyTasks.begin(); it != mReadyTasks.end();) {
                                linkId     = (*it)->getLinkId();
                                isHardLink = linkId != -1 && (mThreadIds[linkId] == -1 || mThreadIds[linkId] == gettid());
                                if (isHardLink) {
                                    task = *it;
                                    it   = mReadyTasks.erase(it);
                                    break;
                                } else {
                                    ++it;
                                }
                            }
                            if (isHardLink) {
                                return mThreadStop || isHardLink;
                            }
                            for (auto it = mReadyTasks.begin(); it != mReadyTasks.end();) {
                                linkId     = (*it)->getLinkId();
                                isSoftLink = linkId == -1;
                                if (isSoftLink) {
                                    task = *it;
                                    it   = mReadyTasks.erase(it);
                                    break;
                                } else {
                                    ++it;
                                }
                            }
                            return mThreadStop || isSoftLink;
                        });
                    }
                    if (mThreadStop && mReadyTasks.empty()) return;

                    if (linkId != -1) {
                        mThreadIds[linkId] = gettid();
                    }

                    if (!isAbort()) {
                        task->run();

                        {
                            std::unique_lock<std::mutex> lock(mCallerMutex);
                            mHasTaskDone = true;
                            mCallerCond.notify_one();
                        }
                    }
                    else {
                        return;
                    }
                }
            });
        }
    }

    void destroyThreadPool() {
        {  // 此处大括号必须加，否则其他thread获取锁，导致其他线程没有执行完而卡住
            std::lock_guard<std::mutex> lock(mThreadMutex);
            mThreadStop = true;
        }
        mThreadCond.notify_all();
        for (auto& thread : mThreads) {
            thread.join();
        }
    }

private:
    bool isAbort() {
        return atomic_load(&mAbortFlag);
    }

private:
    int                     mState;
    int                     mTaskNum;
    int                     mLinkNum;
    int                     mMaxParallelNum;
    const std::string       mThreadName;
    bool                    mThreadStop = false;
    bool                    mHasTaskDone = false;
    std::vector<Task*>      mTasks;
    std::vector<std::thread> mThreads;
    std::vector<int>        mThreadIds;
    std::list<Task*>        mReadyTasks;
    std::mutex              mMutex;
    std::mutex              mThreadMutex;
    std::condition_variable mThreadCond;
    std::mutex              mCallerMutex;
    std::condition_variable mCallerCond;
    atomic_bool             mAbortFlag = false;
};

/* TaskFlow */
TaskFlow::TaskFlow(int maxParallelNum, const std::string& threadName, int logLevel) {
    mImpl = new Flow(maxParallelNum, threadName, logLevel);
}

TaskFlow::~TaskFlow() {
    if (mImpl) {
        delete static_cast<Flow*>(mImpl);
        mImpl = nullptr;
    }
}

Task* TaskFlow::createTask(const std::string& name, TaskFuncT taskFunc) {
    return static_cast<Flow*>(mImpl)->createTask(name, taskFunc);
}

int TaskFlow::link(const std::initializer_list<Task*>& tasks, bool runInSameThread) {
    return static_cast<Flow*>(mImpl)->link(tasks, runInSameThread);
}

int TaskFlow::run() {
    return static_cast<Flow*>(mImpl)->run();
}

void TaskFlow::abort() {
    static_cast<Flow*>(mImpl)->abort();
}

NS_END
