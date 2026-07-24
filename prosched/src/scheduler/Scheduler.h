#pragma once

#include <condition_variable>
#include <cstdint>
#include <iomanip>
#include <mutex>
#include <ostream>
#include <queue>
#include <random>
#include <sstream>
#include <stdio.h>
#include <thread>
#include <vector>

#include "Config.h"
#include "memory/PagingManager.h"
#include "process/Process.h"
#include "src/Constants.hpp"
#include "src/Context.h"
#include "src/scheduler/worker/Worker.h"

namespace prosched {

class Scheduler {
public:
  /**
   * @brief Contains cumulative CPU-core tick statistics.
   *
   * totalCpuTicks is always the sum of activeCpuTicks and idleCpuTicks.
   */
  struct CpuTickStats {
    std::uint64_t totalCpuTicks = 0;
    std::uint64_t activeCpuTicks = 0;
    std::uint64_t idleCpuTicks = 0;
  };

  /**
   * @brief Creates a scheduler instance
   *
   * Initializes the scheduler using the provided scheduling configuration
   *
   * @param ctx Scheduling configuration and algorithm settings
   */
  Scheduler(AlgoContext ctx, PagingManager *pagingManager = nullptr)
      : ctx(ctx), pagingManager(pagingManager) {}

  /**
   * @brief Destroys the scheduler instance
   *
   * Stops the scheduler if running and cleans up all allocated resources
   */
  ~Scheduler() {
    if (running) {
      Stop();
    }
    for (Process *p : processes) {
      if (p && p->IsOwnedByScheduler()) {
        delete p;
      }
    }
    for (Worker *w : workers) {
      delete w;
    }
  }

  /**
   * @brief starts the main scheduler loop
   *
   * @return a boolean value, where true if all worker threads
   * have been joined, else false
   */
  bool Start() {
    if (running)
      return false;

    printSchedulerSpecs();

    running = true;
    generatingProcesses = false;

    for (int i = 0; i < this->ctx.num_cpu; i++) {
      Worker *w = new Worker(i, ctx);
      w->SetCycleCompleteCallback([this] { this->NotifyWorkerDone(); });

      try {
        workers.push_back(w);
      } catch (const std::bad_alloc &e) {
        std::cerr << "Allocation failed: " << e.what();
        return false;
      }

      w->Start();
    }

    schedulerThread = std::thread(&Scheduler::SchedulerLoop, this);
    std::cout << "Scheduler started with " << this->ctx.num_cpu << " cores\n";
    return true;
  }

  /**
   * @brief Starts process generation
   */
  void ResumeGenerating() { generatingProcesses = true; }

  /**
   * @brief Stops generating new dummy processes
   */
  void StopGenerating() {
    generatingProcesses = false;
    std::cout
        << "Stopped generating processes. Scheduler is still running.\n\n";
  }

  /**
   * @brief Stops the Scheduler from running
   *
   * running = false, stops all worker threads and the scheduler thread
   */
  void Stop() {
    {
      std::lock_guard<std::mutex> lock(tickMutex);
      running = false;
      generatingProcesses = false;
      tickCv.notify_all();
    }

    if (schedulerThread.joinable()) {
      schedulerThread.join();
    }

    for (Worker *w : workers) {
      w->Stop();
      delete w;
    }
    workers.clear();

    // Empty the queue without deleting since destructor will handle processes
    while (!processQueue.empty()) {
      processQueue.pop();
    }

    std::cout << "Scheduler stopped\n\n";
  }

  /**
   * @brief Adds a Process to the processQueue
   *
   * Registers the specified process with the scheduler and places it into the
   * appropriate scheduling structure
   *
   * @param p A pointer to the process to be added
   * @return self if success, else returns none
   */
  prosched::Process *AddProcess(prosched::Process *p) {
    if (p == nullptr)
      return nullptr;
    std::lock_guard<std::mutex> lock(schedulerMutex);
    try {
      processes.push_back(p);
      processQueue.push(p);
      return p;
    } catch (const std::bad_alloc &e) {
      std::cerr << "Allocation failed: " << e.what();
      return nullptr;
    }
  }

  /**
   * @brief prints the current processes for "screen -ls" and "report-util"
   *
   * Outputs CPU utilization, cores used and available, then the running and
   * finished process lists. Both commands render the exact same report; the
   * only difference is that "report-util" also writes it to csopesy-log.txt.
   *
   * @param out output stream
   */
  void PrintProcesses(std::ostream &out = std::cout) {

    std::lock_guard<std::mutex> lock(schedulerMutex);

    int coresUsed = 0, coresAvail = 0;
    for (Worker *w : workers) {
      if (w->IsBusy())
        coresUsed++;
      else
        coresAvail++;
    }
    double utilization =
        workers.empty()
            ? 0.0
            : (static_cast<double>(coresUsed) / workers.size()) * 100.0;

    out << "\nCPU utilization: " << utilization << "%\n";
    out << "Cores used: " << coresUsed << "\n";
    out << "Cores available: " << coresAvail << "\n";

    std::vector<Process *> running, finished, terminated;
    for (Process *p : processes) {
      if (p == nullptr)
        continue;
      if (p->IsTerminated())
        terminated.push_back(p);
      else if (p->IsFinished())
        finished.push_back(p);
      else
        running.push_back(p);
    }

    // Fixed timestamp width: "(mm/dd/yyyy hh:mm:ssAM/PM)" = 23 chars
    const size_t GAP = 5;
    const size_t TIME_W = 23;

    size_t nameW = 0, coreW = 0, progW = 0;
    auto measure = [&](Process *p, bool fin) {
      nameW = std::max(nameW, p->GetName().size());
      std::string c = "Core: " + std::to_string(p->GetAssignedCore());
      coreW = std::max(coreW, c.size());
      int cur =
          fin ? p->GetTotalInstructions() : p->GetCurrentInstructionIndex();
      std::string g = std::to_string(cur) + " / " +
                      std::to_string(p->GetTotalInstructions());
      progW = std::max(progW, g.size());
    };
    for (Process *p : running)
      measure(p, false);
    for (Process *p : finished)
      measure(p, true);
    for (Process *p : terminated)
      measure(p, true);

    size_t rowLen = nameW + GAP + TIME_W + GAP + coreW + GAP + progW;
    std::string sep(rowLen, '-');

    auto padR = [](const std::string &s, size_t w) -> std::string {
      return s.size() < w ? s + std::string(w - s.size(), ' ') : s;
    };

    auto printRow = [&](Process *p, bool fin) {
      int cur =
          fin ? p->GetTotalInstructions() : p->GetCurrentInstructionIndex();
      std::string prog = std::to_string(cur) + " / " +
                         std::to_string(p->GetTotalInstructions());
      std::string coreStr = "Core: " + std::to_string(p->GetAssignedCore());
      std::string ts = p->GetProcessTimeStart();
      if (ts.empty())
        ts = "N/A";
      out << padR(p->GetName(), nameW + GAP) << padR(ts, TIME_W + GAP)
          << padR(coreStr, coreW + GAP) << prog << "\n";
    };

    out << "\n" << sep << "\n";
    out << "Running processes:\n";
    if (running.empty())
      out << "(none)\n";
    else
      for (Process *p : running)
        printRow(p, false);

    out << "\nFinished processes:\n";
    if (finished.empty())
      out << "(none)\n";
    else
      for (Process *p : finished)
        printRow(p, true);
    
    out << "\nTerminated processes:\n";
    if (terminated.empty())
      out << "(none)\n";
    else 
      for (Process *p : terminated)
        printRow(p, true);

    out << sep << "\n" << std::endl;
  }

  /**
   * @brief wires a process's interpreter into the paging subsystem
   *
   * Registers the interpreter with the paging manager and installs the
   * page-fault handler. The handler receives a page number and returns true
   * when that page was not resident, which abandons the current instruction so
   * the process can retry it once the page has been paged in.
   *
   * Does nothing when the scheduler is running without a paging manager, in
   * which case the interpreter never faults.
   *
   * @param p process whose interpreter should become paging-aware
   */
  void attachPaging(Process *p) {
    if (pagingManager == nullptr) {
      return;
    }

    auto *pagingMgr = pagingManager;
    pagingMgr->RegisterProcessInterpreter(p->GetPID(), &p->GetInterpreter());
    p->GetInterpreter().SetPageSize(
        static_cast<uint32_t>(this->ctx.mem_per_frame));
    p->GetInterpreter().SetPageFaultHandler(
        [pagingMgr, pid = p->GetPID()](int pageNum) {
          if (pagingMgr->IsPageResident(pid, pageNum)) {
            return false;
          }
          return pagingMgr->PageIn(pid, pageNum);
        });
  }

  /**
   * @brief generates a process
   *
   * @param ctx algorithm context
   * @param id process id
   * @param tick scheduler tick
   * @return Process generated
   */
  prosched::Process *generateProcess(AlgoContext *ctx, int pid, int tick) {
    std::ostringstream oss;
    oss << "process" << nextPID;
    std::string name = oss.str();
    Process *p = new Process(name, pid, tick);
    int rolledSize =
        ctx->min_mem_per_proc +
        rand() % (ctx->max_mem_per_proc - ctx->min_mem_per_proc + 1);
    p->SetMemoryBounds(0, rolledSize);
    attachPaging(p);
    p->SetOwnedByScheduler(true);
    Statement instruction;

    // std::cout << p->GetName() << "\n";

    int commandAmount =
        this->ctx.min_ins + rand() % (ctx->max_ins - ctx->min_ins + 1);

    for (int i = 0; i < commandAmount; i++) {
      instruction = prosched::GetRandomStatement(name, 3);
      p->AddInstruction(instruction);
      // std::cout << p->GetName() << " added an instruction\n";
    }

    return p;
  }

  /**
   * @brief getter for running boolean
   *
   * @return boolean value if scheduler is running or not
   */
  bool IsRunning() { return running == true; }

  /**
   * @brief Gets cumulative CPU-core ticks observed by the scheduler.
   *
   * @return Total, active, and idle worker-core tick counts.
   */
  CpuTickStats GetCpuTickStats() const {
    std::lock_guard<std::mutex> lock(cpuStatsMutex);
    return {totalCpuTicks, activeCpuTicks, idleCpuTicks};
  }

  /**
   * @brief Gets the share of CPU cores that are currently executing a process.
   *
   * @return Utilization as a percentage in [0, 100]; 0 when no cores exist.
   */
  double GetCpuUtilization() const {
    if (workers.empty()) {
      return 0.0;
    }

    int coresUsed = 0;
    for (Worker *w : workers) {
      if (w->IsBusy()) {
        coresUsed++;
      }
    }
    return (static_cast<double>(coresUsed) / workers.size()) * 100.0;
  }

  /**
   * @brief Creates a new named process with random instructions and assigns it
   * the next available PID.
   *
   * @param name The name to give the new process
   * @param memoryBytes Memory to allocate to the process, in bytes
   * @return Pointer to the newly created process
   *
   * @note [for: screen -s] The caller is responsible for validating 
   * memoryBytes.
   */
  prosched::Process *CreateNamedProcess(const std::string &name,
                                        long memoryBytes) {
    int pid;
    {
      std::lock_guard<std::mutex> lock(schedulerMutex);
      pid = nextPID++;
    }
    Process *p = new Process(name, pid, 0);
    p->SetMemoryBounds(0, static_cast<size_t>(memoryBytes));
    attachPaging(p);
    p->SetOwnedByScheduler(true);
    int commandAmount = this->ctx.min_ins +
                        rand() % (this->ctx.max_ins - this->ctx.min_ins + 1);
    for (int i = 0; i < commandAmount; i++) {
      Statement instruction = prosched::GetRandomStatement(name, 3);
      p->AddInstruction(instruction);
    }
    return p;
  }

  /**
   * @brief Creates a new process running a caller-supplied program.
   *
   * Identical to CreateNamedProcess except that the instructions come from the
   * caller instead of the random generator. Used by "screen -c".
   *
   * @note The caller is responsible for validating memoryBytes and for
   * enforcing the instruction-count limits.
   *
   * @param name The name to give the new process
   * @param memoryBytes Memory to allocate to the process, in bytes
   * @param instructions The program the process should execute
   * @return Pointer to the newly created process
   */
  prosched::Process *
  CreateProcessWithInstructions(const std::string &name, long memoryBytes,
                                const std::vector<Statement> &instructions) {
    int pid;
    {
      std::lock_guard<std::mutex> lock(schedulerMutex);
      pid = nextPID++;
    }
    Process *p = new Process(name, pid, 0);
    p->SetMemoryBounds(0, static_cast<size_t>(memoryBytes));
    attachPaging(p);
    p->SetOwnedByScheduler(true);
    for (Statement instruction : instructions) {
      p->AddInstruction(instruction);
    }
    return p;
  }

  /**
   * @brief Finds the first non-finished process matching the given name.
   *
   * @param name The process name to search for
   * @return Pointer to the matching process, or nullptr if not found or
   * finished
   */
  prosched::Process *FindProcessByName(const std::string &name) {
    std::lock_guard<std::mutex> lock(schedulerMutex);
    for (Process *p : processes) {
      if (p && p->GetName() == name && !p->IsFinished()) {
        return p;
      }
    }
    return nullptr;
  }

  /**
   * @brief Finds the most recent process with the given name that was shut
   * down by a memory access violation.
   *
   * FindProcessByName deliberately ignores such processes because they can no
   * longer be attached to; 
   * 
   * @param name The process name to search for
   * @return Pointer to the matching terminated process, or nullptr if none
   *
   * @note "screen -r" uses this to tell a violation apart
   * from a name that was simply never created.
   */
  prosched::Process *FindTerminatedProcessByName(const std::string &name) {
    std::lock_guard<std::mutex> lock(schedulerMutex);
    for (auto it = processes.rbegin(); it != processes.rend(); ++it) {
      Process *p = *it;
      if (p && p->GetName() == name && p->IsTerminated()) {
        return p;
      }
    }
    return nullptr;
  }

  /**
   * @brief Returns a snapshot of all known processes (running and finished).
   *
   * @return Vector of process pointers
   */
  std::vector<prosched::Process *> GetAllProcesses() {
    std::lock_guard<std::mutex> lock(schedulerMutex);
    return processes;
  }

  /**
   * @brief Returns a non-destructive front-to-back snapshot of the ready queue.
   *
   * Index 0 is the front of the queue (next to be dispatched); the last index
   * is the back (most recently enqueued). Used to observe enqueue tie-break
   * ordering when several events land on the same scheduler tick.
   *
   * @return Vector of process pointers in queue order (front first)
   */
  std::vector<prosched::Process *> GetReadyQueueSnapshot() {
    std::lock_guard<std::mutex> lock(schedulerMutex);
    std::vector<prosched::Process *> snapshot;
    std::queue<prosched::Process *> copy = processQueue;
    while (!copy.empty()) {
      snapshot.push_back(copy.front());
      copy.pop();
    }
    return snapshot;
  }

  /**
   * @brief Handles periodic batch process generation.
   *
   * @param cpuCycles Current master clock tick cycle count.
   */
  void GenerateProcessesCycle(int cpuCycles) {
    if (generatingProcesses && cpuCycles % ctx.batch_process_frequency == 0) {
      Process *p = generateProcess(&this->ctx, nextPID, cpuCycles);
      std::lock_guard<std::mutex> lock(schedulerMutex);
      processQueue.push(p);
      processes.push_back(p);

      nextPID++;
    }
  }

  /**
   * @brief Gathers processes preempted by CPU workers and places them back in
   * the queue.
   */
  void CollectPreemptedCycle() {
    std::vector<Process *> preemptedList;
    for (Worker *w : workers) {
      Process *preempted = w->GetAndClearPreemptedProcess();
      if (preempted) {
        preemptedList.push_back(preempted);
      }
    }
    if (!preemptedList.empty()) {
      std::lock_guard<std::mutex> lock(schedulerMutex);
      for (Process *preempted : preemptedList) {
        processQueue.push(preempted);
      }
    }
  }

  /**
   * @brief Updates sleeping processes and re-queues them when their sleep ticks
   * expire.
   */
  void UpdateSleepingProcessesCycle() {
    std::lock_guard<std::mutex> lock(schedulerMutex);
    for (Process *p : processes) {
      if (p != nullptr && p->GetState() == ProcessState::WAITING) {
        p->DecrementSleepCycles();
        if (p->GetCyclesRemainingForSleep() <= 0) {
          if (p->GetCurrentInstructionIndex() >= p->GetTotalInstructions()) {
            p->SetState(ProcessState::FINISHED);
          } else {
            p->SetState(ProcessState::READY);
            processQueue.push(p);
          }
        }
      }
    }
  }

  /**
   * @brief Callback invoked by each worker core thread when its cycle execution
   * finishes.
   *
   * Increments the count of completed workers for the current tick and notifies
   * the scheduler thread waiting inside TriggerWorkersTick. Thread-safe.
   */
  void NotifyWorkerDone() {
    std::lock_guard<std::mutex> lock(tickMutex);
    workersCompleted++;
    tickCv.notify_one();
  }

  /**
   * @brief Triggers one clock cycle of execution for all active workers.
   *
   * Signals all worker cores to start the cycle and blocks the scheduler thread
   * until all worker threads have completed their cycle execution. Thread-safe.
   *
   * @param cpuCycles The current master clock cycle count.
   */
  void TriggerWorkersTick(int cpuCycles) {
    {
      std::lock_guard<std::mutex> lock(tickMutex);
      workersCompleted = 0;
    }

    for (Worker *w : workers) {
      {
        std::lock_guard<std::mutex> statsLock(cpuStatsMutex);
        totalCpuTicks++;
        if (w->IsBusy()) {
          activeCpuTicks++;
        } else {
          idleCpuTicks++;
        }
      }
      w->SignalNewTick(cpuCycles);
    }

    std::unique_lock<std::mutex> lock(tickMutex);
    tickCv.wait(lock, [this] {
      return !running || workersCompleted >= (int)workers.size();
    });
  }

  /**
   * @brief The main scheduler control loop running in a dedicated thread.
   *
   * Drives the master scheduler clock and invokes sub-cycle operations.
   */
  void SchedulerLoop() {
    int cpuCycles = 0;

    while (running) {
      cpuCycles++;

      GenerateProcessesCycle(cpuCycles);

      TriggerWorkersTick(cpuCycles);

      if (ctx.schedulerType == SchedulerType::FCFS) {
        FCFS();
      } else if (ctx.schedulerType == SchedulerType::RR) {
        RoundRobin();
      }

      CollectPreemptedCycle();
      UpdateSleepingProcessesCycle();
      FreeFinishedProcesses();

      std::this_thread::sleep_for(std::chrono::milliseconds(kTickDurationMs));
    }
  }

private:
  AlgoContext ctx;
  std::thread schedulerThread;
  std::vector<prosched::Process *> processes;
  std::queue<prosched::Process *> processQueue;
  std::vector<Worker *> workers;
  std::mutex schedulerMutex;
  bool running = false;
  bool generatingProcesses = false;
  int nextPID = 1;
  std::mutex tickMutex;
  std::condition_variable tickCv;
  int workersCompleted = 0;
  PagingManager *pagingManager = nullptr;
  mutable std::mutex cpuStatsMutex;
  std::uint64_t totalCpuTicks = 0;
  std::uint64_t activeCpuTicks = 0;
  std::uint64_t idleCpuTicks = 0;

  /**
   * @brief Frees memory allocated to finished processes.
   *
   * @note this is a new function @Stephen <----
   */
  void FreeFinishedProcesses() {
    if (!pagingManager) {
      return;
    }
    std::lock_guard<std::mutex> lock(schedulerMutex);

    for (Process *p : processes) {
      if (p != nullptr && p->IsFinished()) {
        pagingManager->FreeAllPagesForProcess(p->GetPID());
      }
    }
  }

  /**
   * @brief converts the scheduler type enum to a string for printing
   *
   * @return a string representation of the scheduler type
   */
  std::string schedulerTypeToString() {
    switch (ctx.schedulerType) {
    case SchedulerType::FCFS:
      return "fcfs";
    case SchedulerType::RR:
      return "rr";
    default:
      return "unknown";
    }
  }

  /**
   * @brief prints the scheduler specs when "screen -ls" is called
   */
  void printSchedulerSpecs() {
    std::cout << "\n\nScheduler specs\n";
    std::cout << "\nNumber of cores: " << this->ctx.num_cpu;
    std::cout << "\nScheduler: " << schedulerTypeToString();
    std::cout << "\nBatch process freq: " << this->ctx.batch_process_frequency;
    std::cout << "\nMin-ins & Max-ins: " << this->ctx.min_ins << "-"
              << this->ctx.max_ins;
    std::cout << "\nDelay per execution: " << this->ctx.delay_per_execution;
    std::cout << "\nQuantum cycle: " << this->ctx.rr_quantum_cycles << "\n\n";
  }

  /**
   * @brief First-Come First-Serve Scheduler Algorithm
   *
   * A non-preemptive algorithm in which processes are attended to in
   * the order they arrive in the process queue. For each CPU worker
   * in the workers vector assign processes from the process queue to
   * a specific Worker to be executed.
   */
  void FCFS() {
    std::lock_guard<std::mutex> lock(schedulerMutex);
    for (Worker *w : workers) {
      if (!processQueue.empty() && !w->IsBusy()) {
        Process *p = processQueue.front();
        processQueue.pop();
        w->AssignProcess(p);
      }
    }
  }

  /**
   * @brief Round Robin Scheduler Algorithm
   *
   * A preemptive algorithm in which processes are given a fixed time slice
   * (quantum). Increments quantum used for running processes on cores and
   * preempts them if the quantum limit is reached. Then dispatches ready
   * processes to available cores.
   */
  void RoundRobin() {
    // 1. Quantum preemption check
    for (Worker *w : workers) {
      if (w->CheckAndIncrementQuantum(ctx.rr_quantum_cycles)) {
        w->PreemptProcess();
      }
    }

    // 2. Dispatch ready processes
    std::lock_guard<std::mutex> lock(schedulerMutex);
    for (Worker *w : workers) {
      if (w->IsBusy()) {
        continue;
      }

      if (!processQueue.empty()) {
        Process *p = processQueue.front();
        processQueue.pop();
        w->AssignProcess(p);
      }
    }
  }
};

} // namespace prosched
