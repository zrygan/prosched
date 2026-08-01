#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <stdio.h>
#include <thread>

#include "Config.h"
#include "Constants.hpp"
#include "Context.h"
#include "scheduler/process/Process.h"

namespace prosched {

class Worker {
private:
  int coreNum;
  AlgoContext ctx;
  prosched::Process *currentProcess = nullptr;
  std::thread workerThread;
  mutable std::mutex workerMutex;

  // The scheduler thread clears this in Stop() while this worker's own thread
  // is reading it at the top of its loop, outside workerMutex.
  std::atomic<bool> running{false};

  prosched::Process *preemptedProcess = nullptr;

  int lastProcessedTick = 0;
  int currentMasterTick = 0;
  std::condition_variable cv;
  std::function<void()> onCycleComplete;

  /**
   * @brief Preempts the current process from the worker (lock-free version)
   *
   * Must be called while holding workerMutex.
   *
   * @return the preempted process, or nullptr if none
   */
  prosched::Process *PreemptProcessUnlocked() {
    prosched::Process *p = currentProcess;
    if (p != nullptr) {
      p->SetState(prosched::ProcessState::READY);
      currentProcess = nullptr;
      preemptedProcess = p;

      if (ctx.schedulerType == SchedulerType::RR) {
        p->ResetQuantumUsed();
      }
    }
    return p;
  }

public:
  /**
   * @brief Creates a worker associated with a CPU core
   *
   * @param coreNum the identifier of the CPU core assigned to this worker
   * @param ctx Scheduling configuration
   */
  Worker(int coreNum, AlgoContext ctx)
      : coreNum(coreNum), ctx(ctx), currentProcess(nullptr), running(false) {}

  /**
   * @brief Starts worker execution
   *
   * Launches the worker thread and begins processing the assigned process
   * @return true if start is successful
   */
  bool Start() {
    std::lock_guard<std::mutex> lock(workerMutex);

    if (running) {
      std::cout << "Worker on core " << coreNum << " is already running\n";
      return false;
    }
    running = true;

    workerThread = std::thread(&Worker::ThreadTask, this);
    return true;
  }

  /**
   * @brief Stops worker execution
   *
   * Signals the worker thread to terminate
   * @return true if stop is successful
   */
  bool Stop() {
    {
      std::lock_guard<std::mutex> lock(workerMutex);
      if (!running) {
        std::cout << "Worker on core " << coreNum << " is not running\n";
        return false;
      }
      running = false;
      cv.notify_all();
    }

    if (workerThread.joinable()) {
      workerThread.join();
    }

    return true;
  }

  /**
   * @brief Assigns a process to the worker for execution
   *
   * The assigned process becomes the worker's current task and may
   * be executed when the worker is running.
   *
   * @param p a pointer to the Process to assign
   * @return the assigned process if successful, or nullptr if busy
   */
  prosched::Process *AssignProcess(prosched::Process *p) {
    std::lock_guard<std::mutex> lock(workerMutex);
    if (currentProcess != nullptr) {
      // std::cout << "Worker on core " << coreNum << " is already assigned a
      // process\n";
      return nullptr;
    }
    currentProcess = p;
    if (p) {
      p->AssignCore(coreNum);
      p->SetState(ProcessState::RUNNING);

      if (ctx.schedulerType == SchedulerType::RR) {
        p->ResetQuantumUsed();
      }
      if (p->GetCurrentInstructionIndex() == 0) {
        p->SetCurrentInstructionCyclesLeft(0);
      }
    }
    return p;
  }

  /**
   * @brief Preempts the current process from the worker
   *
   * Thread-safe wrapper around PreemptProcessUnlocked.
   *
   * @return the preempted process, or nullptr if none
   */
  prosched::Process *PreemptProcess() {
    std::lock_guard<std::mutex> lock(workerMutex);
    return PreemptProcessUnlocked();
  }

  /**
   * @brief Checks if the worker is currently running
   *
   * @return true if the worker thread is active
   */
  bool IsRunning() const { return running; }

  /**
   * @brief Checks if the worker is currently executing a process
   *
   * @return true if a process is assigned and executing
   */
  bool IsBusy() const {
    std::lock_guard<std::mutex> lock(workerMutex);
    return currentProcess != nullptr;
  }

  /**
   * @brief Gets the core number associated with this worker
   *
   * @return The CPU core identifier
   */
  int GetCoreNum() const { return coreNum; }

  /**
   * @brief Gets the process currently assigned to this worker
   *
   * @return A pointer to the current Process, or nullptr if idle
   */
  prosched::Process *GetCurrentProcess() const {
    std::lock_guard<std::mutex> lock(workerMutex);
    return currentProcess;
  }
  /**
   * @brief Returns and clears the last preempted process (if any)
   *
   * Must be called from the scheduler to re-queue preempted processes.
   * Thread-safe.
   */
  prosched::Process *GetAndClearPreemptedProcess() {
    std::lock_guard<std::mutex> lock(workerMutex);
    prosched::Process *p = preemptedProcess;
    preemptedProcess = nullptr;
    return p;
  }

  /**
   * @brief Registers a callback to invoke when this worker completes a tick
   * cycle.
   *
   * Used by the scheduler to hook worker completion into the tick barrier sync.
   *
   * @param callback Function to execute upon completing a cycle.
   */
  void SetCycleCompleteCallback(std::function<void()> callback) {
    onCycleComplete = callback;
  }

  /**
   * @brief Signals the worker thread that a new master clock tick cycle has
   * started.
   *
   * Updates the worker's master tick clock and notifies the wait condition
   * variable to wake up the worker thread task. Thread-safe.
   *
   * @param masterTick The current master clock cycle count.
   */
  void SignalNewTick(int masterTick) {
    std::lock_guard<std::mutex> lock(workerMutex);
    currentMasterTick = masterTick;
    cv.notify_one();
  }

  /**
   * @brief Increments quantum of the current running process and checks if it
   * has expired.
   *
   * Thread-safe.
   * @param limit The quantum cycles limit.
   * @return true if quantum is exceeded, false otherwise.
   */
  bool CheckAndIncrementQuantum(int limit) {
    std::lock_guard<std::mutex> lock(workerMutex);
    if (currentProcess != nullptr &&
        currentProcess->GetState() == ProcessState::RUNNING) {

          if(currentProcess->GetLastInstructionWasPageFault()) {
            return false;
          }

          currentProcess->IncrementQuantumUsed();
          return currentProcess->GetQuantumUsed() >= limit;
      }
    return false;
  }

  /**
   * @brief Manages execution delay cycles and triggers instruction execution.
   *
   * @param p Pointer to the process running on the core.
   */
  void TickExecution(Process *p) {
    // std::cerr << "[DEBUG] [Worker " << coreNum << "] " << p->GetName() << "
    // cyclesLeft=" << p->GetCurrentInstructionCyclesLeft()
    //           << " state=" << p->GetState() << "\n";

    if ((p->GetState() == ProcessState::RUNNING ||
         p->GetState() == ProcessState::READY) &&
        p->GetCurrentInstructionCyclesLeft() <= 0) {
      p->ExecuteInstructions(coreNum);

      // Detach immediately if process transitioned to WAITING (SLEEP) or
      // FINISHED
      if (p->GetState() == ProcessState::WAITING ||
          p->GetState() == ProcessState::FINISHED ||
          p->GetState() == ProcessState::TERMINATED) {
        currentProcess = nullptr;
        return;
      }

      if(!p->GetLastInstructionWasPageFault()){
        p->SetCurrentInstructionCyclesLeft(ctx.delay_per_execution);
      }

      return;
    }

    p->DecrementInstructionCyclesLeft();
  }

  /**
   * @brief Performs a single CPU clock cycle of execution for the assigned
   * process.
   */
  void RunCycle() {
    Process *p = currentProcess;
    if (p == nullptr) {
      return; // Idle core
    }

    if (p->GetState() == ProcessState::FINISHED ||
        p->GetState() == ProcessState::WAITING ||
        p->GetState() == ProcessState::TERMINATED) {
      currentProcess = nullptr;
      return;
    }

    TickExecution(p);
  }

  /**
   * @brief Worker thread's main execution loop representing a single CPU core.
   *
   * Ticks logical CPU cycles at a set interval and invokes core execution.
   *
   * @return A pointer to this Worker instance upon termination.
   */
  Worker *ThreadTask() {
    while (running) {
      if (onCycleComplete) {
        std::unique_lock<std::mutex> lock(workerMutex);
        cv.wait(lock, [this] {
          return !running || currentMasterTick > lastProcessedTick;
        });

        if (!running)
          break;

        RunCycle();
        lastProcessedTick = currentMasterTick;
        onCycleComplete();
      } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(kTickDurationMs));

        std::lock_guard<std::mutex> lock(workerMutex);

        if (!running)
          break;

        RunCycle();
      }
    }
    return this;
  }
};

} // namespace prosched