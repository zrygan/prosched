#include "scheduler/worker/Worker.h"
#include "scheduler/process/Process.h"
#include <atomic>
#include <chrono>
#include <gtest/gtest.h>
#include <thread>

static void AddRaw(prosched::Process &p, const std::string &src) {
  prosched::Interpreter interp;
  auto stmts = interp.Parse(src);
  for (auto &s : stmts)
    p.AddInstruction(s);
}

static AlgoContext makeTestCtx() {
  ConfigStruct *cs = makeDefault();
  cs->scheduler = "fcfs";
  AlgoContext ctx = AlgoContext::buildConfig(cs);
  delete cs;
  return ctx;
}

namespace WorkerAssignProcess {

// Idle worker — returns the process that was passed in
TEST(WorkerAssignProcess, ReturnsProcessWhenIdle) {
  prosched::Worker w(1, makeTestCtx());
  prosched::Process p("assign_1", 1, 0);

  prosched::Process *result = w.AssignProcess(&p);

  EXPECT_EQ(result, &p);
}

// Nullptr input on an idle worker — returns nullptr, worker stays idle
TEST(WorkerAssignProcess, NullInputOnIdleWorkerReturnsNull) {
  prosched::Worker w(1, makeTestCtx());

  prosched::Process *result = w.AssignProcess(nullptr);

  EXPECT_EQ(result, nullptr);
  EXPECT_FALSE(w.IsBusy());
}

// Worker already has a process — second assign must return nullptr (busy guard)
TEST(WorkerAssignProcess, ReturnsNullWhenBusy) {
  prosched::Worker w(1, makeTestCtx());
  prosched::Process p1("assign_busy_1", 1, 0);
  prosched::Process p2("assign_busy_2", 2, 0);

  w.AssignProcess(&p1);
  prosched::Process *result = w.AssignProcess(&p2);

  EXPECT_EQ(result, nullptr);
}

// Running but idle worker (no process yet) — assign should succeed
TEST(WorkerAssignProcess, AssignToRunningIdleWorkerSucceeds) {
  prosched::Worker w(1, makeTestCtx());
  prosched::Process p("assign_running", 1, 0);

  w.Start();
  prosched::Process *result = w.AssignProcess(&p);
  w.Stop();

  EXPECT_EQ(result, &p);
}

// After process finishes and thread clears it, a new process can be assigned
TEST(WorkerAssignProcess, CanAssignAgainAfterProcessFinishes) {
  prosched::Worker w(1, makeTestCtx());
  prosched::Process p1("assign_after_1", 1, 0);
  prosched::Process p2("assign_after_2", 2, 0);
  AddRaw(p1, "PRINT(\"done\")");

  w.AssignProcess(&p1);
  w.Start();
  std::this_thread::sleep_for(std::chrono::milliseconds(10));

  // p1 should be finished and currentProcess should be cleared
  ASSERT_TRUE(p1.IsFinished());

  prosched::Process *result = w.AssignProcess(&p2);
  w.Stop();

  EXPECT_EQ(result, &p2);
}

// Two threads race to assign at the same time — workerMutex must ensure exactly
// one succeeds and neither thread blocks forever
TEST(WorkerAssignProcess, ConcurrentAssignFromTwoThreadsDoesNotDeadlock) {
  prosched::Worker w(1, makeTestCtx());
  prosched::Process p1("conc_1", 1, 0);
  prosched::Process p2("conc_2", 2, 0);

  prosched::Process *r1 = nullptr, *r2 = nullptr;
  std::thread t1([&] { r1 = w.AssignProcess(&p1); });
  std::thread t2([&] { r2 = w.AssignProcess(&p2); });

  t1.join();
  t2.join();

  // Exactly one succeeds — the other is blocked by the busy guard
  EXPECT_TRUE((r1 == nullptr) != (r2 == nullptr));
}

} // namespace WorkerAssignProcess

namespace WorkerIsBusy {

// Worker has no process on construction — not busy
TEST(WorkerIsBusy, FalseOnConstruction) {
  prosched::Worker w(1, makeTestCtx());
  EXPECT_FALSE(w.IsBusy());
}

// Assigning a non-null process marks the worker as busy
TEST(WorkerIsBusy, TrueAfterValidAssign) {
  prosched::Worker w(1, makeTestCtx());
  prosched::Process p("busy_1", 1, 0);

  w.AssignProcess(&p);
  EXPECT_TRUE(w.IsBusy());
}

// Assigning nullptr on an idle worker changes nothing — stays not busy
TEST(WorkerIsBusy, FalseAfterNullAssignOnIdleWorker) {
  prosched::Worker w(1, makeTestCtx());

  w.AssignProcess(nullptr);
  EXPECT_FALSE(w.IsBusy());
}

// Once the process finishes, the thread clears currentProcess — worker goes
// idle
TEST(WorkerIsBusy, FalseAfterProcessFinishes) {
  prosched::Worker w(1, makeTestCtx());
  prosched::Process p("busy_thread", 1, 0);
  AddRaw(p, "PRINT(\"hi\")");

  w.AssignProcess(&p);
  w.Start();
  std::this_thread::sleep_for(std::chrono::milliseconds(10));

  EXPECT_FALSE(w.IsBusy());

  w.Stop();
}

} // namespace WorkerIsBusy

namespace WorkerIsRunning {

// Worker thread is not started on construction
TEST(WorkerIsRunning, FalseOnConstruction) {
  prosched::Worker w(1, makeTestCtx());
  EXPECT_FALSE(w.IsRunning());
}

// IsRunning reflects the thread being active after Start
TEST(WorkerIsRunning, TrueAfterStart) {
  prosched::Worker w(1, makeTestCtx());

  w.Start();
  EXPECT_TRUE(w.IsRunning());

  w.Stop();
}

// IsRunning drops to false once the thread joins after Stop
TEST(WorkerIsRunning, FalseAfterStop) {
  prosched::Worker w(1, makeTestCtx());

  w.Start();
  w.Stop();

  EXPECT_FALSE(w.IsRunning());
}

// Starting an already-running worker returns false and stays running
TEST(WorkerIsRunning, StartReturnsFalseIfAlreadyRunning) {
  prosched::Worker w(1, makeTestCtx());

  w.Start();
  bool result = w.Start(); // second start

  EXPECT_FALSE(result);
  EXPECT_TRUE(w.IsRunning()); // still running

  w.Stop();
}

// Stopping a worker that was never started returns false
TEST(WorkerIsRunning, StopReturnsFalseIfNotRunning) {
  prosched::Worker w(1, makeTestCtx());

  bool result = w.Stop();

  EXPECT_FALSE(result);
  EXPECT_FALSE(w.IsRunning());
}

// Start/Stop cycle can repeat without deadlock
TEST(WorkerIsRunning, CanRestartAfterStop) {
  prosched::Worker w(1, makeTestCtx());

  w.Start();
  w.Stop();
  bool result = w.Start();

  EXPECT_TRUE(result);
  EXPECT_TRUE(w.IsRunning());

  w.Stop();
}

// Start() returns true on a fresh worker
TEST(WorkerIsRunning, StartReturnsTrueOnSuccess) {
  prosched::Worker w(1, makeTestCtx());

  bool result = w.Start();

  EXPECT_TRUE(result);

  w.Stop();
}

// Stop() returns true when the worker was running
TEST(WorkerIsRunning, StopReturnsTrueOnSuccess) {
  prosched::Worker w(1, makeTestCtx());

  w.Start();
  bool result = w.Stop();

  EXPECT_TRUE(result);
}

// Multiple start/stop cycles must not deadlock or corrupt state
TEST(WorkerIsRunning, MultipleRestartCycles) {
  prosched::Worker w(1, makeTestCtx());

  for (int i = 0; i < 3; i++) {
    EXPECT_TRUE(w.Start()) << "Start failed on cycle " << i;
    EXPECT_TRUE(w.IsRunning());
    EXPECT_TRUE(w.Stop()) << "Stop failed on cycle " << i;
    EXPECT_FALSE(w.IsRunning());
  }
}

// Stop() while the worker is actively executing a process must not deadlock
TEST(WorkerIsRunning, StopWhileProcessingDoesNotDeadlock) {
  prosched::Worker w(1, makeTestCtx());
  prosched::Process p("stop_mid", 1, 0);

  // 500 instructions — enough that the worker won't finish before we call
  // Stop() (with kTickDurationMs=0, 50 could complete before Stop() is invoked)
  for (int i = 0; i < 500; i++)
    AddRaw(p, "PRINT(\"tick\")");

  w.AssignProcess(&p);
  w.Start();
  w.Stop(); // should return cleanly even mid-execution

  EXPECT_FALSE(w.IsRunning());
}

} // namespace WorkerIsRunning

namespace WorkerGetCoreNum {

// GetCoreNum returns exactly the value passed to the constructor
TEST(WorkerGetCoreNum, ReturnsConstructedCoreNum) {
  prosched::Worker w(2, makeTestCtx());
  EXPECT_EQ(w.GetCoreNum(), 2);
}

// Each Worker instance keeps its own independent core number
TEST(WorkerGetCoreNum, DifferentWorkersHaveDifferentCoreNums) {
  prosched::Worker w1(1, makeTestCtx());
  prosched::Worker w2(3, makeTestCtx());

  EXPECT_EQ(w1.GetCoreNum(), 1);
  EXPECT_EQ(w2.GetCoreNum(), 3);
}

} // namespace WorkerGetCoreNum

namespace WorkerGetCurrentProcess {

// No process has been assigned — returns nullptr
TEST(WorkerGetCurrentProcess, NullWhenIdle) {
  prosched::Worker w(1, makeTestCtx());
  EXPECT_EQ(w.GetCurrentProcess(), nullptr);
}

// Returns the exact pointer that was assigned
TEST(WorkerGetCurrentProcess, ReturnsAssignedProcess) {
  prosched::Worker w(1, makeTestCtx());
  prosched::Process p("cur_proc", 1, 0);

  w.AssignProcess(&p);

  EXPECT_EQ(w.GetCurrentProcess(), &p);
}

// After process finishes, thread clears it — GetCurrentProcess returns nullptr
TEST(WorkerGetCurrentProcess, NullAfterProcessFinishes) {
  prosched::Worker w(1, makeTestCtx());
  prosched::Process p("cur_done", 1, 0);
  AddRaw(p, "PRINT(\"bye\")");

  w.AssignProcess(&p);
  w.Start();
  std::this_thread::sleep_for(std::chrono::milliseconds(10));

  EXPECT_EQ(w.GetCurrentProcess(), nullptr);

  w.Stop();
}

} // namespace WorkerGetCurrentProcess

// ─── WorkerAssignProcess (additional) ────────────────────────────────────

namespace WorkerAssignProcess {

// Assigned process state becomes RUNNING
TEST(WorkerAssignProcess, AssignedProcessStateIsRunning) {
  prosched::Worker w(1, makeTestCtx());
  prosched::Process p("state_check", 1, 0);

  w.AssignProcess(&p);

  EXPECT_EQ(p.GetState(), prosched::ProcessState::RUNNING);
}

// Assigned process gets the worker's core number stamped onto it
TEST(WorkerAssignProcess, AssignedProcessCoreIsStamped) {
  prosched::Worker w(3, makeTestCtx());
  prosched::Process p("core_stamp", 1, 0);

  w.AssignProcess(&p);

  EXPECT_EQ(p.GetAssignedCore(), 3);
}

} // namespace WorkerAssignProcess

// ─── WorkerPreemptProcess ─────────────────────────────────────────────────

namespace WorkerPreemptProcess {

// Returns the previously assigned process
TEST(WorkerPreemptProcess, ReturnsPreviousProcess) {
  prosched::Worker w(1, makeTestCtx());
  prosched::Process p("preempt_1", 1, 0);

  w.AssignProcess(&p);
  prosched::Process *result = w.PreemptProcess();

  EXPECT_EQ(result, &p);
}

// Worker becomes idle after preemption
TEST(WorkerPreemptProcess, WorkerBecomesIdleAfterPreempt) {
  prosched::Worker w(1, makeTestCtx());
  prosched::Process p("preempt_2", 2, 0);

  w.AssignProcess(&p);
  w.PreemptProcess();

  EXPECT_FALSE(w.IsBusy());
}

// Process state is set back to READY after preemption
TEST(WorkerPreemptProcess, ProcessStateIsReadyAfterPreempt) {
  prosched::Worker w(1, makeTestCtx());
  prosched::Process p("preempt_3", 3, 0);

  w.AssignProcess(&p); // sets state to RUNNING
  w.PreemptProcess();

  EXPECT_EQ(p.GetState(), prosched::ProcessState::READY);
}

// Returns nullptr when no process is assigned
TEST(WorkerPreemptProcess, ReturnsNullWhenNothingAssigned) {
  prosched::Worker w(1, makeTestCtx());

  prosched::Process *result = w.PreemptProcess();

  EXPECT_EQ(result, nullptr);
}

} // namespace WorkerPreemptProcess

// ─── WorkerGetAndClearPreemptedProcess ───────────────────────────────────

namespace WorkerGetAndClearPreemptedProcess {

// Returns nullptr when no preemption has occurred
TEST(WorkerGetAndClearPreemptedProcess, ReturnsNullWhenNothingPreempted) {
  prosched::Worker w(1, makeTestCtx());

  EXPECT_EQ(w.GetAndClearPreemptedProcess(), nullptr);
}

// Returns the process that was preempted
TEST(WorkerGetAndClearPreemptedProcess, ReturnsProcessAfterPreemptProcess) {
  prosched::Worker w(1, makeTestCtx());
  prosched::Process p("clear_1", 1, 0);

  w.AssignProcess(&p);
  w.PreemptProcess();

  EXPECT_EQ(w.GetAndClearPreemptedProcess(), &p);
}

// Second call returns nullptr — the slot is cleared on the first call
TEST(WorkerGetAndClearPreemptedProcess, ClearedOnSecondCall) {
  prosched::Worker w(1, makeTestCtx());
  prosched::Process p("clear_2", 2, 0);

  w.AssignProcess(&p);
  w.PreemptProcess();
  w.GetAndClearPreemptedProcess(); // first call consumes it

  EXPECT_EQ(w.GetAndClearPreemptedProcess(), nullptr);
}

} // namespace WorkerGetAndClearPreemptedProcess

// ─── WorkerCheckAndIncrementQuantum ──────────────────────────────────────

namespace WorkerCheckAndIncrementQuantum {

// Returns false on an idle worker (no process assigned)
TEST(WorkerCheckAndIncrementQuantum, ReturnsFalseOnIdleWorker) {
  prosched::Worker w(1, makeTestCtx());

  EXPECT_FALSE(w.CheckAndIncrementQuantum(3));
}

// Returns false while the quantum counter is below the limit
TEST(WorkerCheckAndIncrementQuantum, ReturnsFalseWhileUnderLimit) {
  prosched::Worker w(1, makeTestCtx());
  prosched::Process p("quantum_1", 1, 0);

  w.AssignProcess(&p); // state = RUNNING, quantumUsed = 0
  bool result =
      w.CheckAndIncrementQuantum(3); // quantum becomes 1, 1 >= 3 false

  EXPECT_FALSE(result);
}

// Returns true at exactly the limit
TEST(WorkerCheckAndIncrementQuantum, ReturnsTrueAtExactlyLimit) {
  prosched::Worker w(1, makeTestCtx());
  prosched::Process p("quantum_2", 2, 0);

  w.AssignProcess(&p);
  w.CheckAndIncrementQuantum(3);               // quantum = 1
  w.CheckAndIncrementQuantum(3);               // quantum = 2
  bool result = w.CheckAndIncrementQuantum(3); // quantum = 3, 3 >= 3 true

  EXPECT_TRUE(result);
}

} // namespace WorkerCheckAndIncrementQuantum

// ─── WorkerCallbackSync ───────────────────────────────────────────────────

namespace WorkerCallbackSync {

// Callback fires after SignalNewTick triggers a cycle
TEST(WorkerCallbackSync, CallbackFiresAfterSignalNewTick) {
  prosched::Worker w(1, makeTestCtx());
  std::atomic<int> callback_count{0};
  w.SetCycleCompleteCallback([&callback_count] { callback_count++; });
  w.Start();

  w.SignalNewTick(1);

  // Spin-wait up to 50 ms for the callback to fire
  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
  while (callback_count.load() == 0 &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }

  EXPECT_EQ(callback_count.load(), 1);
  w.Stop();
}

// Callback does not fire without a tick signal
TEST(WorkerCallbackSync, CallbackDoesNotFireBeforeTickSignaled) {
  prosched::Worker w(1, makeTestCtx());
  std::atomic<int> callback_count{0};
  w.SetCycleCompleteCallback([&callback_count] { callback_count++; });
  w.Start();

  std::this_thread::sleep_for(std::chrono::milliseconds(10));

  EXPECT_EQ(callback_count.load(), 0);
  w.Stop();
}

// Worker runs exactly once per tick — the process advances by exactly one
// instruction per signal, not more, even though the worker loop is continuous
TEST(WorkerCallbackSync, WorkerRunsExactlyOncePerTick) {
  prosched::Worker w(1, makeTestCtx());
  prosched::Process p("sync_once", 1, 0);
  for (int i = 0; i < 10; i++)
    AddRaw(p, "PRINT(\"x\")");

  std::atomic<int> callback_count{0};
  w.SetCycleCompleteCallback([&callback_count] { callback_count++; });
  w.AssignProcess(&p);
  w.Start();

  // Send one tick and wait for the callback
  w.SignalNewTick(1);
  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
  while (callback_count.load() < 1 &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::yield();

  // Exactly one instruction should have run — worker did not run ahead
  EXPECT_EQ(p.GetCurrentInstructionIndex(), 1);
  EXPECT_EQ(callback_count.load(), 1);

  w.Stop();
}

// All N workers call back before the simulated scheduler proceeds —
// models the TriggerWorkersTick barrier that the real Scheduler relies on
TEST(WorkerCallbackSync, AllWorkersCallBackBeforeBarrierCompletes) {
  const int num_workers = 4;
  std::atomic<int> completed{0};
  std::vector<prosched::Worker *> workers;

  for (int i = 0; i < num_workers; i++) {
    auto *w = new prosched::Worker(i, makeTestCtx());
    w->SetCycleCompleteCallback([&completed] { completed++; });
    w->Start();
    workers.push_back(w);
  }

  // Signal all workers — equivalent to one TriggerWorkersTick call
  for (int i = 0; i < num_workers; i++)
    workers[i]->SignalNewTick(1);

  // Spin-wait until all N workers have called back (the barrier condition)
  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
  while (completed.load() < num_workers &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::yield();

  EXPECT_EQ(completed.load(), num_workers);

  for (auto *w : workers) {
    w->Stop();
    delete w;
  }
}

} // namespace WorkerCallbackSync

// ─── WorkerTickExecution ──────────────────────────────────────────────────

namespace WorkerTickExecution {

// When cycles left is 0 and state is RUNNING, the instruction executes
TEST(WorkerTickExecution, ExecutesInstructionWhenCyclesLeftIsZero) {
  prosched::Worker w(1, makeTestCtx()); // delay_per_execution = 0
  prosched::Process p("te_1", 1, 0);
  AddRaw(p, "PRINT(\"hi\")");

  w.AssignProcess(&p); // state = RUNNING, cycles_left = 0
  w.TickExecution(&p);

  EXPECT_EQ(p.GetCurrentInstructionIndex(), 1);
}

// When cycles left is positive, the instruction does not execute and cycles
// decrements
TEST(WorkerTickExecution, DecrementsInstructionCyclesLeftWhenPositive) {
  prosched::Worker w(1, makeTestCtx());
  prosched::Process p("te_2", 2, 0);
  AddRaw(p, "PRINT(\"hi\")");

  p.SetState(prosched::ProcessState::RUNNING);
  p.SetCurrentInstructionCyclesLeft(3);
  w.TickExecution(&p);

  EXPECT_EQ(p.GetCurrentInstructionIndex(), 0); // not executed
  EXPECT_EQ(p.GetCurrentInstructionCyclesLeft(), 2);
}

// Worker detaches when the instruction transitions the process to WAITING
TEST(WorkerTickExecution, DetachesOnWaitingState) {
  prosched::Worker w(1, makeTestCtx());
  prosched::Process p("te_3", 3, 0);
  AddRaw(p, "SLEEP(5)");

  w.AssignProcess(&p); // state = RUNNING, cycles_left = 0
  w.TickExecution(&p); // SLEEP executes → WAITING → currentProcess = nullptr

  EXPECT_FALSE(w.IsBusy());
}

// Worker detaches when the instruction transitions the process to FINISHED
TEST(WorkerTickExecution, DetachesOnFinishedState) {
  prosched::Worker w(1, makeTestCtx());
  prosched::Process p("te_4", 4, 0);
  AddRaw(p, "PRINT(\"bye\")");

  w.AssignProcess(&p); // state = RUNNING, cycles_left = 0
  w.TickExecution(&p); // PRINT executes → FINISHED → currentProcess = nullptr

  EXPECT_FALSE(w.IsBusy());
}

// The delay-per-exec stall must hand the core back instead of holding it: a
// stalling process executes nothing, so counting its stall as a busy core both
// overstates CPU utilization and starves the ready queue. With min-ins 30 and
// batch-process-freq 60, holding the core makes a single-core run saturate at
// 100% forever for any delay >= 1, which no amount of waiting recovers from.
TEST(WorkerTickExecution, DelayPerExecReleasesTheCore) {
  AlgoContext ctx = makeTestCtx();
  ctx.delay_per_execution = 3;
  prosched::Worker w(1, ctx);
  prosched::Process p("te_delay_1", 11, 0);
  AddRaw(p, "PRINT(\"a\")");
  AddRaw(p, "PRINT(\"b\")");

  w.AssignProcess(&p);
  w.TickExecution(&p); // first PRINT executes, then the stall begins

  EXPECT_EQ(p.GetCurrentInstructionIndex(), 1);
  EXPECT_FALSE(w.IsBusy());
  EXPECT_EQ(p.GetState(), prosched::ProcessState::WAITING);
  EXPECT_EQ(p.GetCyclesRemainingForSleep(), 3);
}

// A zero delay is the "execute every cycle" case, so the process keeps the core
// and runs its next instruction on the next tick.
TEST(WorkerTickExecution, ZeroDelayPerExecKeepsTheCore) {
  AlgoContext ctx = makeTestCtx();
  ctx.delay_per_execution = 0;
  prosched::Worker w(1, ctx);
  prosched::Process p("te_delay_2", 12, 0);
  AddRaw(p, "PRINT(\"a\")");
  AddRaw(p, "PRINT(\"b\")");

  w.AssignProcess(&p);
  w.TickExecution(&p);

  EXPECT_TRUE(w.IsBusy());
  EXPECT_EQ(p.GetState(), prosched::ProcessState::RUNNING);
}

} // namespace WorkerTickExecution

// ─── WorkerRunCycle ───────────────────────────────────────────────────────

namespace WorkerRunCycle {

// Idle core (no process) returns without crashing
TEST(WorkerRunCycle, IdleCoreReturnsWithoutCrash) {
  prosched::Worker w(1, makeTestCtx());
  EXPECT_NO_THROW(w.RunCycle());
  EXPECT_FALSE(w.IsBusy());
}

// FINISHED process is cleared from the worker on RunCycle
TEST(WorkerRunCycle, FinishedProcessClearsCurrentProcess) {
  prosched::Worker w(1, makeTestCtx());
  prosched::Process p("rc_2", 2, 0);

  w.AssignProcess(&p);
  p.SetState(
      prosched::ProcessState::FINISHED); // mark finished without executing
  w.RunCycle();

  EXPECT_FALSE(w.IsBusy());
}

// Running process causes TickExecution to be invoked (instruction advances)
TEST(WorkerRunCycle, RunningProcessInvokesTickExecution) {
  prosched::Worker w(1, makeTestCtx());
  prosched::Process p("rc_3", 3, 0);
  AddRaw(p, "PRINT(\"a\")");
  AddRaw(p, "PRINT(\"b\")");

  w.AssignProcess(&p); // state = RUNNING, cycles_left = 0
  w.RunCycle();

  EXPECT_EQ(p.GetCurrentInstructionIndex(), 1);
}

} // namespace WorkerRunCycle

// ─── Page faults vs. the round-robin quantum (MO2) ──────────────────────────
// MO2: "an instruction executes only once its page is valid; page-fault
// handling repeats until a valid frame is found, THEN the instruction runs."
//
// A page fault correctly does not advance the instruction index — the
// instruction is retried. But the faulting tick still counts against the RR
// quantum, so when quantum-cycles is 1 the process is preempted immediately
// after servicing the fault and the retry never happens. Its page is evicted
// while it waits for its next turn, so the next dispatch faults on the same
// instruction, and so on without end.
//
// This is not hypothetical: the MO2 demo config is num-cpu 8 / quantum-cycles 1
// / max-overall-mem 1024 / mem-per-frame 256 — 4 frames shared by 8 cores, so a
// page is always gone before its owner runs again.

namespace WorkerPageFaultQuantum {

static void AddWrite(prosched::Process &p, const std::string &addr,
                     const std::string &value) {
  prosched::Statement s;
  s.keyword = prosched::Keyword::kWrite;
  s.args = {addr, value};
  p.AddInstruction(s);
}

static AlgoContext makeRrCtx(int quantum) {
  AlgoContext ctx = makeTestCtx();
  ctx.schedulerType = SchedulerType::RR;
  ctx.rr_quantum_cycles = quantum;
  ctx.delay_per_execution = 0;
  return ctx;
}

// A single-frame pager: servicing a fault makes that page resident, and any
// later page evicts it. `residentPage` is reset to -1 between turns by the
// caller, modelling the other seven cores taking every frame while this process
// sits in the ready queue.
static void attachSingleFramePager(prosched::Process &p, int *residentPage,
                                   int *pageIns) {
  p.SetMemoryBounds(0, 1024);
  p.GetInterpreter().SetPageSize(256);
  p.GetInterpreter().SetPageFaultHandler([residentPage, pageIns](int pageNum) {
    if (*residentPage == pageNum) {
      return false; // resident -> no fault, instruction proceeds
    }
    *residentPage = pageNum; // pager brings it in
    ++*pageIns;
    return true; // faulted this tick; instruction is retried
  });
}

// Drives the scheduler's per-tick sequence (SchedulerLoop runs
// TriggerWorkersTick, then RoundRobin's quantum check) for one process on one
// core, for at most `turns` dispatches. Stops early once the process makes
// forward progress.
static void RunDispatchTurns(prosched::Worker &w, prosched::Process &p,
                             int quantum, int *residentPage, int turns) {
  for (int turn = 0; turn < turns; ++turn) {
    *residentPage = -1;  // evicted while the process waited for this turn
    w.AssignProcess(&p); // dispatch; RR resets the quantum here
    while (w.IsBusy()) {
      w.RunCycle();
      if (w.CheckAndIncrementQuantum(quantum)) {
        w.PreemptProcess();
        w.GetAndClearPreemptedProcess(); // scheduler re-queues it
        break;
      }
    }
    if (p.GetCurrentInstructionIndex() > 0) {
      break;
    }
  }
}

// FAILING: with quantum-cycles 1 the fault consumes the entire quantum, so the
// retry never runs and the process is frozen on its first memory instruction.
TEST(WorkerPageFaultQuantum, FaultingInstructionRetiresWithQuantumOne) {
  AlgoContext ctx = makeRrCtx(1);
  int residentPage = -1;
  int pageIns = 0;

  prosched::Worker w(0, ctx);
  prosched::Process p("quantum_fault_1", 1, 0);
  AddWrite(p, "0x100", "42");
  attachSingleFramePager(p, &residentPage, &pageIns);

  RunDispatchTurns(w, p, ctx.rr_quantum_cycles, &residentPage, 50);

  EXPECT_GT(p.GetCurrentInstructionIndex(), 0)
      << "instruction never retired across 50 dispatches (" << pageIns
      << " page-ins): the page-fault tick burns the whole quantum, so the "
         "retry the spec requires never gets to run";
}

// Control: the same process and the same pager, with room for one tick after
// the fault. This is the behaviour the test above should have, and it isolates
// the quantum — not the retry model — as the cause.
TEST(WorkerPageFaultQuantum, FaultingInstructionRetiresWhenQuantumAllowsRetry) {
  AlgoContext ctx = makeRrCtx(2);
  int residentPage = -1;
  int pageIns = 0;

  prosched::Worker w(0, ctx);
  prosched::Process p("quantum_fault_2", 2, 0);
  AddWrite(p, "0x100", "42");
  attachSingleFramePager(p, &residentPage, &pageIns);

  RunDispatchTurns(w, p, ctx.rr_quantum_cycles, &residentPage, 50);

  EXPECT_GT(p.GetCurrentInstructionIndex(), 0)
      << "with a spare tick after the fault the retry should have executed";
}

} // namespace WorkerPageFaultQuantum
