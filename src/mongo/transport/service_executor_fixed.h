/**
 *    Copyright (C) 2020-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <memory>

#include "mongo/base/status.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/mutex.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/thread.h"
#include "mongo/transport/service_executor.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/hierarchical_acquisition.h"

namespace mongo {
namespace transport {

/**
 * A service executor that uses a fixed (configurable) number of threads to execute tasks.
 * This executor always yields before executing scheduled tasks, and never yields before scheduling
 * new tasks (i.e., `ScheduleFlags::kMayYieldBeforeSchedule` is a no-op for this executor).
 */
class ServiceExecutorFixed : public ServiceExecutor,
                             public std::enable_shared_from_this<ServiceExecutorFixed> {
public:
    explicit ServiceExecutorFixed(ThreadPool::Options options);
    virtual ~ServiceExecutorFixed();

    Status start() override;
    Status shutdown(Milliseconds timeout) override;
    Status scheduleTask(Task task, ScheduleFlags flags) override;

    Mode transportMode() const override {
        return Mode::kSynchronous;
    }

    void appendStats(BSONObjBuilder* bob) const override;

private:
    // Maintains the execution state (e.g., recursion depth) for executor threads
    class ExecutorThreadContext {
    public:
        ExecutorThreadContext(std::weak_ptr<ServiceExecutorFixed> serviceExecutor)
            : _executor(std::move(serviceExecutor)) {
            _adjustRunningExecutorThreads(1);
        }

        ExecutorThreadContext(ExecutorThreadContext&&) = delete;
        ExecutorThreadContext(const ExecutorThreadContext&) = delete;

        ~ExecutorThreadContext() {
            _adjustRunningExecutorThreads(-1);
        }

        void run(ServiceExecutor::Task task) {
            // Yield here to improve concurrency, especially when there are more executor threads
            // than CPU cores.
            stdx::this_thread::yield();
            _recursionDepth++;
            task();
            _recursionDepth--;
        }

        int getRecursionDepth() const {
            return _recursionDepth;
        }

    private:
        void _adjustRunningExecutorThreads(int adjustment) {
            if (auto executor = _executor.lock()) {
                executor->_numRunningExecutorThreads.fetchAndAdd(adjustment);
            }
        }

        int _recursionDepth = 0;
        std::weak_ptr<ServiceExecutorFixed> _executor;
    };

private:
    AtomicWord<size_t> _numRunningExecutorThreads{0};
    AtomicWord<bool> _canScheduleWork{false};

    mutable Mutex _mutex =
        MONGO_MAKE_LATCH(HierarchicalAcquisitionLevel(0), "ServiceExecutorFixed::_mutex");
    stdx::condition_variable _shutdownCondition;

    /**
     * State transition diagram: kNotStarted ---> kRunning ---> kStopped
     * The service executor cannot be in "kRunning" when its destructor is invoked.
     */
    enum State { kNotStarted, kRunning, kStopped } _state = kNotStarted;

    ThreadPool::Options _options;
    std::unique_ptr<ThreadPool> _threadPool;

    static inline thread_local std::unique_ptr<ExecutorThreadContext> _executorContext;
};

}  // namespace transport
}  // namespace mongo
