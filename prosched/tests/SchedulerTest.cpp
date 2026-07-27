#include "scheduler/Scheduler.h"
#include "Config.h"
#include "Context.h"
#include "scheduler/process/Process.h"
#include <algorithm>
#include <chrono>
#include <gtest/gtest.h>
#include <sstream>
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

// Small-instruction context with suppressed auto-generation (batch_freq=1M).
// algo: "fcfs" or "rr"; quantum only relevant for rr.
static AlgoContext makeSmallCtx(const std::string &algo, int num_cpu = 1,
                                int quantum = 3) {
  ConfigStruct *cs = makeDefault();
  cs->scheduler = algo;
  cs->num_cpu = num_cpu;
  cs->rr_quantum_cycles = quantum;
  cs->batch_process_freq = 1000000;
  cs->min_ins = 1;
  cs->max_ins = 5;
  AlgoContext ctx = AlgoContext::buildConfig(cs);
  delete cs;
  return ctx;
}

namespace SchedulerAddProcess {

// AddProcess returns the same pointer that was passed in
// NOTE: Process objects are declared BEFORE the Scheduler so the Scheduler
// (destroyed first, in reverse order) doesn't read freed process pointers in
// ~Scheduler -> IsOwnedByScheduler(). (ASan: heap-use-after-free otherwise.)
TEST(SchedulerAddProcess, ReturnsSameProcess) {
  prosched::Process p("test_process", 1, 0);
  prosched::Scheduler scheduler(makeTestCtx());

  prosched::Process *result = scheduler.AddProcess(&p);

  ASSERT_NE(result, nullptr);
  EXPECT_EQ(result->GetPID(), p.GetPID());
  EXPECT_EQ(result->GetName(), p.GetName());
}

// Adding multiple processes should each return their own identity
TEST(SchedulerAddProcess, MultipleProcessesReturnCorrectly) {
  prosched::Process p1("proc_alpha", 1, 0);
  prosched::Process p2("proc_beta", 2, 1);
  prosched::Scheduler scheduler(makeTestCtx());

  prosched::Process *r1 = scheduler.AddProcess(&p1);
  prosched::Process *r2 = scheduler.AddProcess(&p2);

  ASSERT_NE(r1, nullptr);
  ASSERT_NE(r2, nullptr);
  EXPECT_EQ(r1->GetPID(), 1);
  EXPECT_EQ(r1->GetName(), "proc_alpha");
  EXPECT_EQ(r2->GetPID(), 2);
  EXPECT_EQ(r2->GetName(), "proc_beta");
}

// Empty process name is still accepted
TEST(SchedulerAddProcess, EmptyProcessName) {
  prosched::Process p("", 1, 0);
  prosched::Scheduler scheduler(makeTestCtx());

  prosched::Process *result = scheduler.AddProcess(&p);

  ASSERT_NE(result, nullptr);
  EXPECT_EQ(result->GetName(), "");
}

// PID 0 is a valid boundary value
TEST(SchedulerAddProcess, PIDZero) {
  prosched::Process p("pid_zero", 0, 0);
  prosched::Scheduler scheduler(makeTestCtx());

  prosched::Process *result = scheduler.AddProcess(&p);

  ASSERT_NE(result, nullptr);
  EXPECT_EQ(result->GetPID(), 0);
}

// Same pointer added twice — scheduler must not crash or corrupt
TEST(SchedulerAddProcess, DuplicatePID) {
  prosched::Process p("dup", 5, 0);
  prosched::Scheduler scheduler(makeTestCtx());

  prosched::Process *r1 = scheduler.AddProcess(&p);
  prosched::Process *r2 = scheduler.AddProcess(&p);

  ASSERT_NE(r1, nullptr);
  ASSERT_NE(r2, nullptr);
  EXPECT_EQ(r1->GetPID(), r2->GetPID());
  EXPECT_EQ(r1->GetName(), r2->GetName());
}

// INT_MAX arrival tick must not overflow or crash
TEST(SchedulerAddProcess, LargeArrivalTick) {
  prosched::Process p("late_proc", 1, INT_MAX);
  prosched::Scheduler scheduler(makeTestCtx());

  prosched::Process *result = scheduler.AddProcess(&p);

  ASSERT_NE(result, nullptr);
  EXPECT_EQ(result->GetPID(), 1);
  EXPECT_EQ(result->GetName(), "late_proc");
}

// Adding more processes than num_cpu must not drop any
TEST(SchedulerAddProcess, MoreProcessesThanCores) {
  const int count = 10;
  std::vector<prosched::Process> procs;
  procs.reserve(count);
  for (int i = 0; i < count; i++)
    procs.emplace_back("p" + std::to_string(i), i, i);
  prosched::Scheduler scheduler(makeTestCtx());

  for (int i = 0; i < count; i++) {
    prosched::Process *result = scheduler.AddProcess(&procs[i]);
    ASSERT_NE(result, nullptr) << "Failed at index " << i;
    EXPECT_EQ(result->GetPID(), i);
    EXPECT_EQ(result->GetName(), "p" + std::to_string(i));
  }
}

// Out-of-chronological-order arrival ticks must both be accepted
TEST(SchedulerAddProcess, OutOfOrderArrivalTick) {
  prosched::Process late("late_proc", 1, 100);
  prosched::Process early("early_proc", 2, 0);
  prosched::Scheduler scheduler(makeTestCtx());

  prosched::Process *r_late = scheduler.AddProcess(&late);
  prosched::Process *r_early = scheduler.AddProcess(&early);

  ASSERT_NE(r_late, nullptr);
  ASSERT_NE(r_early, nullptr);
  EXPECT_EQ(r_late->GetName(), "late_proc");
  EXPECT_EQ(r_early->GetName(), "early_proc");
}

// nullptr input must return nullptr and not crash
TEST(SchedulerAddProcess, NullProcessReturnsNull) {
  prosched::Scheduler scheduler(makeTestCtx());

  prosched::Process *result = scheduler.AddProcess(nullptr);

  EXPECT_EQ(result, nullptr);
}

// AddProcess acquires schedulerMutex; SchedulerLoop also acquires it for
// process generation and FCFS — must return the same pointer even under
// contention
TEST(SchedulerAddProcess, AddWhileRunningReturnsSamePointer) {
  prosched::Process p("live_add", 1, 0);
  AddRaw(p, "PRINT(\"hi\")");

  prosched::Scheduler scheduler(makeTestCtx());
  scheduler.Start();

  prosched::Process *result = scheduler.AddProcess(&p);
  EXPECT_EQ(result, &p);

  scheduler.Stop();
}

} // namespace SchedulerAddProcess

namespace SchedulerStartStop {

// Scheduler thread is not started on construction
TEST(SchedulerStartStop, IsRunningFalseOnConstruction) {
  prosched::Scheduler scheduler(makeTestCtx());
  EXPECT_FALSE(scheduler.IsRunning());
}

// Start() returns true on a fresh scheduler
TEST(SchedulerStartStop, StartReturnsTrueOnSuccess) {
  prosched::Scheduler scheduler(makeTestCtx());

  bool result = scheduler.Start();
  scheduler.Stop();

  EXPECT_TRUE(result);
}

// IsRunning reflects the scheduler being active after Start
TEST(SchedulerStartStop, IsRunningTrueAfterStart) {
  prosched::Scheduler scheduler(makeTestCtx());

  scheduler.Start();
  EXPECT_TRUE(scheduler.IsRunning());

  scheduler.Stop();
}

// Start() returns false and leaves state unchanged when already running
TEST(SchedulerStartStop, StartReturnsFalseIfAlreadyRunning) {
  prosched::Scheduler scheduler(makeTestCtx());

  scheduler.Start();
  bool result = scheduler.Start(); // second call

  EXPECT_FALSE(result);
  EXPECT_TRUE(scheduler.IsRunning()); // still running

  scheduler.Stop();
}

// IsRunning drops to false once Stop completes
TEST(SchedulerStartStop, IsRunningFalseAfterStop) {
  prosched::Scheduler scheduler(makeTestCtx());

  scheduler.Start();
  scheduler.Stop();

  EXPECT_FALSE(scheduler.IsRunning());
}

// Stop() on a scheduler that never started must not crash
TEST(SchedulerStartStop, StopWithoutStartDoesNotCrash) {
  prosched::Scheduler scheduler(makeTestCtx());
  EXPECT_NO_THROW(scheduler.Stop());
  EXPECT_FALSE(scheduler.IsRunning());
}

// Stop() while the SchedulerLoop is actively ticking must not deadlock
TEST(SchedulerStartStop, StopWhileSchedulingDoesNotDeadlock) {
  prosched::Scheduler scheduler(makeTestCtx());

  scheduler.Start();
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  scheduler.Stop();

  EXPECT_FALSE(scheduler.IsRunning());
}

// Stop() while workers are mid-execution must still clean up cleanly
TEST(SchedulerStartStop, StopWhileWorkersExecutingDoesNotDeadlock) {
  prosched::Scheduler scheduler(makeTestCtx());

  // Pre-load processes before starting so workers get busy immediately
  for (int i = 1; i <= 4; i++) {
    prosched::Process *p =
        new prosched::Process("pre_" + std::to_string(i), i, 0);
    for (int j = 0; j < 20; j++)
      AddRaw(*p, "PRINT(\"tick\")");
    scheduler.AddProcess(p);
  }

  scheduler.Start();
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  scheduler.Stop();

  EXPECT_FALSE(scheduler.IsRunning());
}

// Start/Stop can cycle multiple times without deadlock or corrupted state
TEST(SchedulerStartStop, MultipleRestartCycles) {
  prosched::Scheduler scheduler(makeTestCtx());

  for (int i = 0; i < 3; i++) {
    bool started = scheduler.Start();
    EXPECT_TRUE(started) << "Start failed on cycle " << i;
    EXPECT_TRUE(scheduler.IsRunning());

    scheduler.Stop();
    EXPECT_FALSE(scheduler.IsRunning());
  }
}

} // namespace SchedulerStartStop

namespace SchedulerPrintProcesses {

// Printing with no processes registered must not crash
TEST(SchedulerPrintProcesses, EmptyListDoesNotCrash) {
  prosched::Scheduler scheduler(makeTestCtx());
  EXPECT_NO_THROW(scheduler.PrintProcesses());
}

// Printing after AddProcess must not crash
TEST(SchedulerPrintProcesses, PrintAfterAddDoesNotCrash) {
  prosched::Scheduler scheduler(makeTestCtx());
  prosched::Process p("print_test", 1, 0);
  scheduler.AddProcess(&p);

  EXPECT_NO_THROW(scheduler.PrintProcesses());
}

// PrintProcesses while the scheduler is running must not deadlock
TEST(SchedulerPrintProcesses, PrintWhileRunningDoesNotDeadlock) {
  prosched::Scheduler scheduler(makeTestCtx());

  scheduler.Start();
  std::this_thread::sleep_for(std::chrono::milliseconds(10));

  EXPECT_NO_THROW(scheduler.PrintProcesses());

  scheduler.Stop();
}

// PrintProcesses holds schedulerMutex then acquires workerMutex via
// GetCurrentProcess — two concurrent callers must not produce lock-order
// inversion; scheduler must still be running correctly after both complete
TEST(SchedulerPrintProcesses, ConcurrentPrintLeavesSchedulerRunning) {
  prosched::Scheduler scheduler(makeTestCtx());
  scheduler.Start();
  std::this_thread::sleep_for(std::chrono::milliseconds(10));

  std::thread t1([&] { scheduler.PrintProcesses(); });
  std::thread t2([&] { scheduler.PrintProcesses(); });

  t1.join();
  t2.join();

  EXPECT_TRUE(scheduler.IsRunning());

  scheduler.Stop();
}

} // namespace SchedulerPrintProcesses

namespace SchedulerGenerateProcess {

// generateProcess() is public and does NOT increment nextPID — that happens
// only inside SchedulerLoop. A fresh Scheduler has nextPID == 1, so the first
// call always produces name "process1" regardless of the pid argument.

// generateProcess always returns a valid heap-allocated Process
TEST(SchedulerGenerateProcess, ReturnsNonNull) {
  prosched::Scheduler scheduler(makeTestCtx());
  AlgoContext ctx = makeTestCtx();
  prosched::Process *p = scheduler.generateProcess(&ctx, 1, 0);
  ASSERT_NE(p, nullptr);
  delete p;
}

// Name uses nextPID (starts at 1), not the pid parameter
TEST(SchedulerGenerateProcess, NameFollowsProcessNPattern) {
  prosched::Scheduler scheduler(makeTestCtx());
  AlgoContext ctx = makeTestCtx();
  prosched::Process *p = scheduler.generateProcess(&ctx, 1, 0);
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(p->GetName(), "process1");
  delete p;
}

// Instruction count falls within the configured min_ins/max_ins range
TEST(SchedulerGenerateProcess, InstructionCountInConfigRange) {
  prosched::Scheduler scheduler(makeTestCtx());
  AlgoContext ctx = makeTestCtx();
  prosched::Process *p = scheduler.generateProcess(&ctx, 1, 0);
  ASSERT_NE(p, nullptr);
  int total = p->GetTotalInstructions();
  EXPECT_GE(total, ctx.min_ins);
  EXPECT_LE(total, ctx.max_ins);
  delete p;
}

// Processes are flagged for cleanup by the Scheduler destructor
TEST(SchedulerGenerateProcess, IsOwnedByScheduler) {
  prosched::Scheduler scheduler(makeTestCtx());
  AlgoContext ctx = makeTestCtx();
  prosched::Process *p = scheduler.generateProcess(&ctx, 1, 0);
  ASSERT_NE(p, nullptr);
  EXPECT_TRUE(p->IsOwnedByScheduler());
  delete p;
}

} // namespace SchedulerGenerateProcess

namespace SchedulerSleepRelinquish {

// A process containing a SLEEP statement must relinquish the CPU core,
// increment its sleep ticks in the scheduler loop, and wake up to finish.
TEST(SchedulerSleepRelinquish, ProcessSleepsAndWakesUpCorrectly) {
  AlgoContext ctx = makeTestCtx();
  ctx.num_cpu = 1; // single core to make core occupancy issues obvious
  prosched::Scheduler scheduler(ctx);

  prosched::Process *p = new prosched::Process("sleepy_proc", 1, 0);
  AddRaw(*p, "PRINT(\"before\")");
  AddRaw(*p, "SLEEP(5)");
  AddRaw(*p, "PRINT(\"after\")");

  scheduler.AddProcess(p);
  scheduler.Start();
  scheduler.StopGenerating();

  // Wait long enough for the process to run, sleep for 5 ticks, wake, and
  // complete. 50 ms is generous with kTickDurationMs = 0.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  scheduler.Stop();

  EXPECT_TRUE(p->IsFinished());
  EXPECT_EQ(p->GetState(), prosched::FINISHED);

  auto logs = p->GetLogs();
  bool found_before = false;
  bool found_sleep = false;
  bool found_after = false;

  for (const auto &log : logs) {
    if (log.find("before") != std::string::npos)
      found_before = true;
    if (log.find("SLEEP") != std::string::npos)
      found_sleep = true;
    if (log.find("after") != std::string::npos)
      found_after = true;
  }

  EXPECT_TRUE(found_before);
  EXPECT_TRUE(found_sleep);
  EXPECT_TRUE(found_after);
}

} // namespace SchedulerSleepRelinquish

namespace SchedulerFCFS {

// A queued process gets dispatched and runs to completion under FCFS
TEST(SchedulerFCFS, DispatchesToIdleWorker) {
  prosched::Scheduler scheduler(makeSmallCtx("fcfs"));
  prosched::Process *p = new prosched::Process("task", 1, 0);
  AddRaw(*p, "PRINT(\"a\")");
  AddRaw(*p, "PRINT(\"b\")");
  p->SetOwnedByScheduler(true);
  scheduler.AddProcess(p);

  scheduler.Start();

  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
  while (!p->IsFinished() && std::chrono::steady_clock::now() < deadline)
    std::this_thread::yield();

  scheduler.Stop();
  EXPECT_TRUE(p->IsFinished());
}

// With one CPU and two queued processes both must finish (FCFS skips busy core)
TEST(SchedulerFCFS, BothProcessesFinishWithOneCPU) {
  prosched::Scheduler scheduler(makeSmallCtx("fcfs"));
  prosched::Process *p1 = new prosched::Process("first", 1, 0);
  prosched::Process *p2 = new prosched::Process("second", 2, 0);
  AddRaw(*p1, "PRINT(\"a\")");
  AddRaw(*p2, "PRINT(\"b\")");
  p1->SetOwnedByScheduler(true);
  p2->SetOwnedByScheduler(true);
  scheduler.AddProcess(p1);
  scheduler.AddProcess(p2);

  scheduler.Start();

  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
  while ((!p1->IsFinished() || !p2->IsFinished()) &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::yield();

  scheduler.Stop();
  EXPECT_TRUE(p1->IsFinished());
  EXPECT_TRUE(p2->IsFinished());
}

} // namespace SchedulerFCFS

namespace SchedulerRoundRobin {

// A process with more instructions than the quantum finishes after preemptions
TEST(SchedulerRoundRobin, ProcessFinishesAfterPreemptions) {
  prosched::Scheduler scheduler(makeSmallCtx("rr", 1, 2));
  prosched::Process *p = new prosched::Process("rr_task", 1, 0);
  for (int i = 0; i < 6; i++)
    AddRaw(*p, "PRINT(\"tick\")");
  p->SetOwnedByScheduler(true);
  scheduler.AddProcess(p);

  scheduler.Start();

  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
  while (!p->IsFinished() && std::chrono::steady_clock::now() < deadline)
    std::this_thread::yield();

  scheduler.Stop();
  EXPECT_TRUE(p->IsFinished());
  EXPECT_EQ(p->GetCurrentInstructionIndex(), 6);
}

// Two processes share one CPU via time slices and both finish
TEST(SchedulerRoundRobin, MultipleProcessesFinish) {
  prosched::Scheduler scheduler(makeSmallCtx("rr", 1, 2));
  prosched::Process *p1 = new prosched::Process("rr1", 1, 0);
  prosched::Process *p2 = new prosched::Process("rr2", 2, 0);
  for (int i = 0; i < 4; i++)
    AddRaw(*p1, "PRINT(\"a\")");
  for (int i = 0; i < 4; i++)
    AddRaw(*p2, "PRINT(\"b\")");
  p1->SetOwnedByScheduler(true);
  p2->SetOwnedByScheduler(true);
  scheduler.AddProcess(p1);
  scheduler.AddProcess(p2);

  scheduler.Start();

  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
  while ((!p1->IsFinished() || !p2->IsFinished()) &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::yield();

  scheduler.Stop();
  EXPECT_TRUE(p1->IsFinished());
  EXPECT_TRUE(p2->IsFinished());
}

} // namespace SchedulerRoundRobin

namespace SchedulerCollectPreemptedCycle {

// CollectPreemptedCycle with no workers must not crash
TEST(SchedulerCollectPreemptedCycle, NoWorkersNoCrash) {
  prosched::Scheduler scheduler(makeTestCtx());
  EXPECT_NO_THROW(scheduler.CollectPreemptedCycle());
}

} // namespace SchedulerCollectPreemptedCycle

namespace SchedulerUpdateSleepingProcessesCycle {

// cyclesRemainingForSleep decrements by 1 on each call while still > 0
TEST(SchedulerUpdateSleepingProcessesCycle, DecrementsEachCall) {
  prosched::Scheduler scheduler(makeTestCtx());
  prosched::Process p("sleeper", 1, 0);
  AddRaw(p, "SLEEP(3)");
  AddRaw(p, "PRINT(\"x\")");
  scheduler.AddProcess(&p);
  p.ExecuteInstructions(0); // execute SLEEP → WAITING, cyclesRemaining=3

  ASSERT_EQ(p.GetState(), prosched::WAITING);
  ASSERT_EQ(p.GetCyclesRemainingForSleep(), 3);

  scheduler.UpdateSleepingProcessesCycle();
  EXPECT_EQ(p.GetCyclesRemainingForSleep(), 2);

  scheduler.UpdateSleepingProcessesCycle();
  EXPECT_EQ(p.GetCyclesRemainingForSleep(), 1);
}

// State transitions WAITING → READY when countdown reaches 0 and instructions
// remain
TEST(SchedulerUpdateSleepingProcessesCycle, BecomesReadyWhenSleepEnds) {
  prosched::Scheduler scheduler(makeTestCtx());
  prosched::Process p("sleeper", 1, 0);
  AddRaw(p, "SLEEP(2)");
  AddRaw(p, "PRINT(\"after\")");
  scheduler.AddProcess(&p);
  p.ExecuteInstructions(0); // WAITING, cyclesRemaining=2

  scheduler.UpdateSleepingProcessesCycle(); // 2 → 1, still WAITING
  EXPECT_EQ(p.GetState(), prosched::WAITING);

  scheduler.UpdateSleepingProcessesCycle(); // 1 → 0, no more sleep → READY
  EXPECT_EQ(p.GetState(), prosched::READY);
}

// State transitions WAITING → FINISHED when countdown reaches 0 and no
// instructions remain
TEST(SchedulerUpdateSleepingProcessesCycle, BecomesFinishedWhenNoInstructions) {
  prosched::Scheduler scheduler(makeTestCtx());
  prosched::Process p("sleeper", 1, 0);
  AddRaw(p, "SLEEP(1)");
  scheduler.AddProcess(&p);
  p.ExecuteInstructions(0); // WAITING, cyclesRemaining=1, index=1==total

  ASSERT_EQ(p.GetState(), prosched::WAITING);

  scheduler.UpdateSleepingProcessesCycle(); // 1 → 0, index >= total → FINISHED
  EXPECT_EQ(p.GetState(), prosched::FINISHED);
}

} // namespace SchedulerUpdateSleepingProcessesCycle

namespace SchedulerGenerateProcessesCycle {

// A process is added when tick falls on the batch_process_frequency boundary
TEST(SchedulerGenerateProcessesCycle, AddsProcessOnMatchingTick) {
  AlgoContext ctx = makeTestCtx();
  ctx.batch_process_frequency = 5;
  prosched::Scheduler scheduler(ctx);
  scheduler.ResumeGenerating();

  int before = (int)scheduler.GetAllProcesses().size();
  scheduler.GenerateProcessesCycle(5); // 5 % 5 == 0
  EXPECT_EQ((int)scheduler.GetAllProcesses().size(), before + 1);
}

// No process is added when tick does not fall on the frequency boundary
TEST(SchedulerGenerateProcessesCycle, SkipsNonMatchingTick) {
  AlgoContext ctx = makeTestCtx();
  ctx.batch_process_frequency = 5;
  prosched::Scheduler scheduler(ctx);
  scheduler.ResumeGenerating();

  int before = (int)scheduler.GetAllProcesses().size();
  scheduler.GenerateProcessesCycle(3); // 3 % 5 != 0
  EXPECT_EQ((int)scheduler.GetAllProcesses().size(), before);
}

// generatingProcesses defaults to false — no process created without
// ResumeGenerating
TEST(SchedulerGenerateProcessesCycle, DefaultStatePreventsGeneration) {
  AlgoContext ctx = makeTestCtx();
  ctx.batch_process_frequency = 1;
  prosched::Scheduler scheduler(ctx);
  // generatingProcesses == false by default

  int before = (int)scheduler.GetAllProcesses().size();
  scheduler.GenerateProcessesCycle(1);
  EXPECT_EQ((int)scheduler.GetAllProcesses().size(), before);
}

} // namespace SchedulerGenerateProcessesCycle

namespace SchedulerGeneratingToggle {

// StopGenerating prevents new processes even when tick would match
TEST(SchedulerGeneratingToggle, StopGeneratingPreventsNewProcesses) {
  AlgoContext ctx = makeTestCtx();
  ctx.batch_process_frequency = 1;
  prosched::Scheduler scheduler(ctx);
  scheduler.ResumeGenerating();
  scheduler.StopGenerating();

  int before = (int)scheduler.GetAllProcesses().size();
  scheduler.GenerateProcessesCycle(1);
  EXPECT_EQ((int)scheduler.GetAllProcesses().size(), before);
}

// ResumeGenerating re-enables generation after StopGenerating
TEST(SchedulerGeneratingToggle, ResumeGeneratingAllowsNewProcesses) {
  AlgoContext ctx = makeTestCtx();
  ctx.batch_process_frequency = 1;
  prosched::Scheduler scheduler(ctx);
  scheduler.StopGenerating();
  scheduler.ResumeGenerating();

  int before = (int)scheduler.GetAllProcesses().size();
  scheduler.GenerateProcessesCycle(1);
  EXPECT_EQ((int)scheduler.GetAllProcesses().size(), before + 1);
}

} // namespace SchedulerGeneratingToggle

namespace SchedulerFindProcessByName {

TEST(SchedulerFindProcessByName, FindsExistingProcess) {
  prosched::Scheduler scheduler(makeTestCtx());
  prosched::Process p("target_proc", 1, 0);
  scheduler.AddProcess(&p);

  prosched::Process *found = scheduler.FindProcessByName("target_proc");
  ASSERT_NE(found, nullptr);
  EXPECT_EQ(found->GetName(), "target_proc");
}

TEST(SchedulerFindProcessByName, ReturnsNullForUnknownName) {
  prosched::Scheduler scheduler(makeTestCtx());

  prosched::Process *found = scheduler.FindProcessByName("nonexistent");
  EXPECT_EQ(found, nullptr);
}

// FindProcessByName returns nullptr for a process that has already finished
TEST(SchedulerFindProcessByName, ReturnsNullForFinishedProcess) {
  prosched::Scheduler scheduler(makeTestCtx());
  prosched::Process p("done_proc", 1, 0);
  AddRaw(p, "PRINT(\"x\")");
  scheduler.AddProcess(&p);
  p.ExecuteInstructions(0);
  ASSERT_TRUE(p.IsFinished());

  prosched::Process *found = scheduler.FindProcessByName("done_proc");
  EXPECT_EQ(found, nullptr);
}

} // namespace SchedulerFindProcessByName

namespace SchedulerCreateNamedProcess {

// MO2: CreateNamedProcess now takes a per-process memory size (screen -s
// <name> <size>). 256 bytes is a valid allocation (power of 2, >= 64).
TEST(SchedulerCreateNamedProcess, HasCorrectName) {
  prosched::Scheduler scheduler(makeTestCtx());
  prosched::Process *p = scheduler.CreateNamedProcess("my_process", 256);
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(p->GetName(), "my_process");
  delete p;
}

TEST(SchedulerCreateNamedProcess, InstructionCountInRange) {
  AlgoContext ctx = makeTestCtx();
  prosched::Scheduler scheduler(ctx);
  prosched::Process *p = scheduler.CreateNamedProcess("count_test", 256);
  ASSERT_NE(p, nullptr);
  EXPECT_GE(p->GetTotalInstructions(), ctx.min_ins);
  EXPECT_LE(p->GetTotalInstructions(), ctx.max_ins);
  delete p;
}

TEST(SchedulerCreateNamedProcess, IsOwnedByScheduler) {
  prosched::Scheduler scheduler(makeTestCtx());
  prosched::Process *p = scheduler.CreateNamedProcess("owned", 256);
  ASSERT_NE(p, nullptr);
  EXPECT_TRUE(p->IsOwnedByScheduler());
  delete p;
}

// Consecutive calls must produce strictly increasing PIDs
TEST(SchedulerCreateNamedProcess, PIDIncrements) {
  prosched::Scheduler scheduler(makeTestCtx());
  prosched::Process *p1 = scheduler.CreateNamedProcess("first", 256);
  prosched::Process *p2 = scheduler.CreateNamedProcess("second", 256);
  ASSERT_NE(p1, nullptr);
  ASSERT_NE(p2, nullptr);
  EXPECT_EQ(p2->GetPID(), p1->GetPID() + 1);
  delete p1;
  delete p2;
}

} // namespace SchedulerCreateNamedProcess

// ─── SchedulerCreateProcessWithInstructions (screen -c) ────────────────────
namespace SchedulerCreateProcessWithInstructions {

// MO2 screen -c: the process runs the caller-supplied program, not a random one
TEST(SchedulerCreateProcessWithInstructions, RunsGivenProgram) {
  prosched::Scheduler scheduler(makeTestCtx());
  prosched::Interpreter interp;
  std::vector<prosched::Statement> program =
      interp.Parse("DECLARE(varA, 10), PRINT(varA)");
  ASSERT_EQ(program.size(), 2u);

  prosched::Process *p =
      scheduler.CreateProcessWithInstructions("prog", 256, program);
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(p->GetName(), "prog");
  EXPECT_EQ(p->GetTotalInstructions(), 2);
  EXPECT_TRUE(p->IsOwnedByScheduler());
  delete p;
}

} // namespace SchedulerCreateProcessWithInstructions

namespace SchedulerTriggerTick {

// TriggerWorkersTick with no workers (empty vector) returns immediately
TEST(SchedulerTriggerTick, NoWorkersNoCrash) {
  prosched::Scheduler scheduler(makeTestCtx());
  // Scheduler not started → workers vector empty, running=false → exits
  // immediately
  EXPECT_NO_THROW(scheduler.TriggerWorkersTick(1));
}

// NotifyWorkerDone can be called on an idle scheduler without crashing
TEST(SchedulerTriggerTick, NotifyWorkerDoneNoCrash) {
  prosched::Scheduler scheduler(makeTestCtx());
  EXPECT_NO_THROW(scheduler.NotifyWorkerDone());
}

} // namespace SchedulerTriggerTick

namespace SchedulerPrintProcessesContent {

TEST(SchedulerPrintProcessesContent, ContainsRunningSection) {
  prosched::Scheduler scheduler(makeTestCtx());
  std::ostringstream out;
  scheduler.PrintProcesses(out);
  EXPECT_NE(out.str().find("Running processes:"), std::string::npos);
}

TEST(SchedulerPrintProcessesContent, ContainsFinishedSection) {
  prosched::Scheduler scheduler(makeTestCtx());
  std::ostringstream out;
  scheduler.PrintProcesses(out);
  EXPECT_NE(out.str().find("Finished processes:"), std::string::npos);
}

TEST(SchedulerPrintProcessesContent, ContainsCPUUtilizationFields) {
  prosched::Scheduler scheduler(makeTestCtx());
  std::ostringstream out;
  scheduler.PrintProcesses(out);
  EXPECT_NE(out.str().find("CPU utilization:"), std::string::npos);
  EXPECT_NE(out.str().find("Cores used:"), std::string::npos);
  EXPECT_NE(out.str().find("Cores available:"), std::string::npos);
}

// No workers started → utilization must be 0%
TEST(SchedulerPrintProcessesContent, ZeroUtilizationWhenIdle) {
  prosched::Scheduler scheduler(makeTestCtx());
  std::ostringstream out;
  scheduler.PrintProcesses(out);
  EXPECT_NE(out.str().find("0%"), std::string::npos);
}

TEST(SchedulerPrintProcessesContent, ShowsProcessNameInOutput) {
  prosched::Scheduler scheduler(makeTestCtx());
  prosched::Process p("named_proc", 1, 0);
  AddRaw(p, "PRINT(\"x\")");
  scheduler.AddProcess(&p);

  std::ostringstream out;
  scheduler.PrintProcesses(out);
  EXPECT_NE(out.str().find("named_proc"), std::string::npos);
}

// A finished process must appear after the "Finished processes:" label
TEST(SchedulerPrintProcessesContent, FinishedProcessAppearsAfterFinishedLabel) {
  prosched::Scheduler scheduler(makeTestCtx());
  prosched::Process p("done", 1, 0);
  AddRaw(p, "PRINT(\"x\")");
  scheduler.AddProcess(&p);
  p.ExecuteInstructions(0);
  ASSERT_TRUE(p.IsFinished());

  std::ostringstream out;
  scheduler.PrintProcesses(out);
  std::string result = out.str();
  size_t fin_pos = result.find("Finished processes:");
  size_t name_pos = result.find("done");
  EXPECT_NE(fin_pos, std::string::npos);
  EXPECT_NE(name_pos, std::string::npos);
  EXPECT_GT(name_pos, fin_pos);
}

} // namespace SchedulerPrintProcessesContent

// ─── SchedulerSleepLifecycle ──────────────────────────────────────────────
// Three tests covering the full sleep pipeline:
//   1. SLEEP puts the process in WAITING state (not just READY or RUNNING)
//   2. After sleep cycles exhaust, the process re-enters the ready queue at the
//      BACK and eventually finishes (not dropped, not jumped to front)
//   3. While a process sleeps, the CPU dispatches the next process in the ready
//      queue — the CPU is never blocked by a sleeping process

namespace SchedulerSleepLifecycle {

// After a process executes a SLEEP instruction via the live scheduler, its
// state must become WAITING — it is held in the sleeping collection, not
// running or ready.
TEST(SchedulerSleepLifecycle, SleepingProcessTransitionsToWaiting) {
  prosched::Scheduler scheduler(makeSmallCtx("fcfs"));
  prosched::Process *p = new prosched::Process("sleeper", 1, 0);
  AddRaw(*p, "SLEEP(100)"); // long sleep — stays WAITING long enough to observe
  AddRaw(*p, "PRINT(\"done\")");
  p->SetOwnedByScheduler(true);
  scheduler.AddProcess(p);
  scheduler.Start();

  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
  while (p->GetState() != prosched::WAITING &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::yield();

  EXPECT_EQ(p->GetState(), prosched::WAITING);
  scheduler.Stop();
}

// After sleep cycles exhaust, the process goes back to READY, is dispatched,
// runs its remaining instruction, and finishes — proving it was re-queued
// rather than dropped.  The post-sleep log entry confirms the instruction
// after SLEEP actually executed (not just that the process reached FINISHED).
TEST(SchedulerSleepLifecycle, WokenProcessReturnsToReadyAndFinishes) {
  prosched::Scheduler scheduler(makeSmallCtx("fcfs"));
  prosched::Process *p = new prosched::Process("sleeper", 1, 0);
  AddRaw(*p, "SLEEP(3)");
  AddRaw(*p, "PRINT(\"after_sleep\")");
  p->SetOwnedByScheduler(true);
  scheduler.AddProcess(p);
  scheduler.Start();

  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
  while (!p->IsFinished() && std::chrono::steady_clock::now() < deadline)
    std::this_thread::yield();

  scheduler.Stop();
  EXPECT_TRUE(p->IsFinished());
  bool found_post_sleep = false;
  for (const auto &log : p->GetLogs())
    if (log.find("after_sleep") != std::string::npos)
      found_post_sleep = true;
  EXPECT_TRUE(found_post_sleep);
}

// With 1 CPU: A (SLEEP(5) then PRINT) is first in queue, B (PRINT) is second.
// A dispatches, hits SLEEP, releases the CPU — B must finish while A is still
// WAITING.  We verify this by waiting only for B, then asserting A has NOT
// finished yet (it is still sleeping).  Only after that do we wait for A.
TEST(SchedulerSleepLifecycle, ReadyQueueProcessRunsWhileOtherSleeps) {
  prosched::Scheduler scheduler(makeSmallCtx("fcfs", 1));

  prosched::Process *a = new prosched::Process("sleeper", 1, 0);
  AddRaw(*a, "SLEEP(5)");
  AddRaw(*a, "PRINT(\"a_done\")");
  a->SetOwnedByScheduler(true);

  prosched::Process *b = new prosched::Process("runner", 2, 0);
  AddRaw(*b, "PRINT(\"b_done\")");
  b->SetOwnedByScheduler(true);

  scheduler.AddProcess(a); // A is first in queue
  scheduler.AddProcess(b); // B is behind A
  scheduler.Start();

  // Wait for B to finish
  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
  while (!b->IsFinished() && std::chrono::steady_clock::now() < deadline)
    std::this_thread::yield();

  // B has finished — A must still be sleeping (not yet finished)
  EXPECT_TRUE(b->IsFinished());
  EXPECT_FALSE(a->IsFinished()); // proves B finished BEFORE A woke up

  // Now wait for A to wake, re-enter the queue, and finish
  deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
  while (!a->IsFinished() && std::chrono::steady_clock::now() < deadline)
    std::this_thread::yield();

  scheduler.Stop();
  EXPECT_TRUE(a->IsFinished());
}

// Scenario: P1 sleeps, P2 runs and finishes, P3 starts running, and while P3
// is still executing P1's sleep expires and it re-enters the ready queue.
//
// Queue order: [P1, P2, P3]
// Tick sequence (1 CPU):
//   P1 dispatches → SLEEP(3) → WAITING, CPU free
//   P2 dispatches → PRINT → FINISHED, CPU free
//   P3 dispatches → runs many PRINTs (still executing)
//   After 3 sleep ticks → P1 becomes READY (back in queue behind P3)
//   P3 finishes → P1 dispatches, finishes
TEST(SchedulerSleepLifecycle, P1WakesIntoReadyQueueWhileP3StillRunning) {
  prosched::Scheduler scheduler(makeSmallCtx("fcfs", 1));

  // P1: sleeps 3 ticks, then prints
  prosched::Process *p1 = new prosched::Process("p1_sleeper", 1, 0);
  AddRaw(*p1, "SLEEP(3)");
  AddRaw(*p1, "PRINT(\"p1_after_sleep\")");
  p1->SetOwnedByScheduler(true);

  // P2: single instruction — finishes while P1 is sleeping
  prosched::Process *p2 = new prosched::Process("p2_fast", 2, 0);
  AddRaw(*p2, "PRINT(\"p2_done\")");
  p2->SetOwnedByScheduler(true);

  // P3: many instructions — still running when P1 wakes up
  prosched::Process *p3 = new prosched::Process("p3_slow", 3, 0);
  for (int i = 0; i < 20; i++)
    AddRaw(*p3, "PRINT(\"p3\")");
  p3->SetOwnedByScheduler(true);

  scheduler.AddProcess(p1);
  scheduler.AddProcess(p2);
  scheduler.AddProcess(p3);
  scheduler.Start();

  // Step 1: wait for P2 to finish — P1 must still be sleeping at this point
  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
  while (!p2->IsFinished() && std::chrono::steady_clock::now() < deadline)
    std::this_thread::yield();
  ASSERT_TRUE(p2->IsFinished());
  EXPECT_EQ(p1->GetState(), prosched::WAITING); // P1 still in sleep

  // Step 2: wait for P1's sleep to expire (state flips to READY)
  // P3 is on the CPU with 20 instructions, so it won't finish before P1 wakes
  deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
  while (p1->GetState() != prosched::READY &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::yield();

  // P1 is READY (back in queue) — P3 is still running and not yet finished
  EXPECT_EQ(p1->GetState(), prosched::READY);
  EXPECT_FALSE(p3->IsFinished()); // P3 still on CPU when P1 woke

  // Step 3: let everything finish
  deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
  while ((!p1->IsFinished() || !p3->IsFinished()) &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::yield();

  scheduler.Stop();
  EXPECT_TRUE(p1->IsFinished());
  EXPECT_TRUE(p3->IsFinished());
}

} // namespace SchedulerSleepLifecycle

// ─── SchedulerReadyQueueTieBreak ──────────────────────────────────────────
// Tie-break rules for ready-queue ordering when several events land on the
// SAME scheduler tick.
//
// SchedulerLoop pushes to processQueue in this fixed order each tick:
//   1. GenerateProcessesCycle      → newly generated / added processes
//   2. TriggerWorkersTick          → (workers run; RR may flag a preemption)
//   3. RoundRobin dispatch         → preemption is stored on the worker only
//   4. CollectPreemptedCycle       → RR-preempted processes re-queued
//   5. UpdateSleepingProcessesCycle→ woken-from-sleep processes re-queued
//
// Therefore, among events that coincide on one tick, the ready queue holds
// them front→back in the order:
//     newly-added  <  preempted  <  woken-from-sleep
// and within a single source, FIFO by insertion order (processes-vector order).
//
// Worked answer to "RR preempts process 2, process 1 wakes, process 4 is added
// — what is the queue order?":  front→back = process 4, process 2, process 1.
//
// processQueue is private, so these tests observe order via the read-only
// GetReadyQueueSnapshot() accessor (index 0 = front / next dispatched).

namespace SchedulerReadyQueueTieBreak {

// Small context: generation enabled, every tick, exactly 1 instruction per
// generated process so generation is cheap and deterministic.
static AlgoContext makeGenCtx() {
  ConfigStruct *cs = makeDefault();
  cs->scheduler = "rr";
  cs->num_cpu = 1;
  cs->rr_quantum_cycles = 2;
  cs->batch_process_freq = 1; // every tick is a generation tick
  cs->min_ins = 1;
  cs->max_ins = 1;
  AlgoContext ctx = AlgoContext::buildConfig(cs);
  delete cs;
  return ctx;
}

// Helper: index of the LAST occurrence of p in the snapshot (-1 if absent).
static long lastIndexOf(const std::vector<prosched::Process *> &v,
                        prosched::Process *p) {
  long idx = -1;
  for (long i = 0; i < (long)v.size(); ++i)
    if (v[i] == p)
      idx = i;
  return idx;
}

// Plain AddProcess ordering is FIFO: first added is at the front of the queue.
TEST(SchedulerReadyQueueTieBreak, ArrivalOrderIsFIFO) {
  prosched::Scheduler scheduler(makeGenCtx());
  prosched::Process a("a", 1, 0), b("b", 2, 0), c("c", 3, 0);

  scheduler.AddProcess(&a);
  scheduler.AddProcess(&b);
  scheduler.AddProcess(&c);

  auto q = scheduler.GetReadyQueueSnapshot();
  ASSERT_EQ(q.size(), 3u);
  EXPECT_EQ(q[0], &a); // front = first added
  EXPECT_EQ(q[1], &b);
  EXPECT_EQ(q[2], &c); // back = last added
}

// A process generated during a tick is appended to the BACK, behind any
// process already waiting in the ready queue.
TEST(SchedulerReadyQueueTieBreak, GeneratedProcessAppendsToBack) {
  prosched::Scheduler scheduler(makeGenCtx());
  scheduler.ResumeGenerating();

  prosched::Process a("already_ready", 1, 0);
  scheduler.AddProcess(&a); // queue: [a]

  scheduler.GenerateProcessesCycle(1); // pushes a generated process to back

  auto q = scheduler.GetReadyQueueSnapshot();
  ASSERT_EQ(q.size(), 2u);
  EXPECT_EQ(q[0], &a); // pre-existing ready process stays at the front
  EXPECT_NE(q[1], &a); // generated process is behind it
}

// THE tie-break (generate vs wake on the same tick): a process generated in
// GenerateProcessesCycle (tick step 1) is enqueued AHEAD of a process woken in
// UpdateSleepingProcessesCycle (tick step 5).
//
// Note: because no worker dispatches in this synchronous test, the sleeper's
// original AddProcess entry is still parked at the front of the queue. We
// assert on the woken (re-queued) entry, which is the LAST element after
// waking.
TEST(SchedulerReadyQueueTieBreak, GeneratedEnqueuedBeforeWokenSleeper) {
  prosched::Scheduler scheduler(makeGenCtx());
  scheduler.ResumeGenerating();

  // Sleeper: SLEEP(1) then PRINT — wakes after one UpdateSleeping call and has
  // a remaining instruction, so it re-enters the queue (does not FINISH).
  prosched::Process *s = new prosched::Process("sleeper", 1, 0);
  AddRaw(*s, "SLEEP(1)");
  AddRaw(*s, "PRINT(\"x\")");
  s->SetOwnedByScheduler(true);
  scheduler.AddProcess(s);
  s->ExecuteInstructions(0); // run SLEEP → WAITING, cyclesRemaining = 1
  ASSERT_EQ(s->GetState(), prosched::WAITING);

  // Reproduce one SchedulerLoop tick's enqueue order (the relevant steps):
  scheduler.GenerateProcessesCycle(1);      // step 1: push generated G
  scheduler.UpdateSleepingProcessesCycle(); // step 5: wake s, push to back

  auto q = scheduler.GetReadyQueueSnapshot();
  // Find the generated process: the only pointer in the queue that isn't s.
  prosched::Process *g = nullptr;
  for (auto *p : q)
    if (p != s) {
      g = p;
      break;
    }
  ASSERT_NE(g, nullptr) << "no generated process found in queue";

  long gPos = std::find(q.begin(), q.end(), g) - q.begin();
  long wokenPos = lastIndexOf(q, s); // the re-queued (woken) entry is last
  EXPECT_EQ(q.back(), s) << "woken sleeper should be at the back of the queue";
  EXPECT_LT(gPos, wokenPos)
      << "generated process must be enqueued ahead of the woken sleeper";
}

// FIFO is preserved AMONG processes woken on the same tick: two sleepers that
// expire in the same UpdateSleepingProcessesCycle re-enter in their original
// insertion order (processes-vector order), not reversed.
TEST(SchedulerReadyQueueTieBreak, MultipleWokenPreserveFIFOOrder) {
  prosched::Scheduler scheduler(makeGenCtx());

  prosched::Process *s1 = new prosched::Process("sleeper1", 1, 0);
  AddRaw(*s1, "SLEEP(1)");
  AddRaw(*s1, "PRINT(\"x\")");
  s1->SetOwnedByScheduler(true);

  prosched::Process *s2 = new prosched::Process("sleeper2", 2, 0);
  AddRaw(*s2, "SLEEP(1)");
  AddRaw(*s2, "PRINT(\"y\")");
  s2->SetOwnedByScheduler(true);

  scheduler.AddProcess(s1); // inserted first
  scheduler.AddProcess(s2); // inserted second
  s1->ExecuteInstructions(0);
  s2->ExecuteInstructions(0);
  ASSERT_EQ(s1->GetState(), prosched::WAITING);
  ASSERT_EQ(s2->GetState(), prosched::WAITING);

  scheduler.UpdateSleepingProcessesCycle(); // both wake; pushed s1 then s2

  auto q = scheduler.GetReadyQueueSnapshot();
  // The two woken (re-queued) entries are the last two; order must be s1, s2.
  ASSERT_GE(q.size(), 2u);
  EXPECT_EQ(q[q.size() - 2], s1)
      << "first-inserted sleeper must wake ahead of the second";
  EXPECT_EQ(q.back(), s2);
}

} // namespace SchedulerReadyQueueTieBreak

// ─── SchedulerRoundRobinTimeSlice ─────────────────────────────────────────
// Faithful reproduction of the GIVEN round-robin time-slice scenario:
//
//   time slice (quantum) = 3
//
//   p1:
//     loop 5:
//       print 1
//       print 2
//       print 3
//       print 4
//
//   START:
//     p1 runs print 1, print 2, print 3   (one quantum = 3 instructions)
//     (pre-empt) ── p1 goes to the back of the ready queue
//     p2 runs and finishes while p1 waits
//     p1 resumes ── print 4, then loop continues (print 1, print 2, ...)
//     ...until p1 completes all of its instructions.
//
// Each PRINT inside the FOR counts as ONE statement toward the quantum, so the
// FOR([PRINT x4], 5) body flattens to 5 * 4 = 20 individual instructions.
// With delay_per_exec = 0, the process executes exactly `quantum` instructions
// per turn before being preempted.

namespace SchedulerRoundRobinTimeSlice {

// The core of the scenario: with quantum 3 and one CPU, p1 must yield the core
// after its first time slice so that p2 — queued behind it — runs and FINISHES
// while p1 is still only partway through its instructions.  If preemption did
// NOT happen, FCFS-style, p1 would run all of its instructions first and p2
// would finish last.  p1 is given a long loop so it cannot possibly complete in
// the tiny window between p2 finishing and our observation of p1.
TEST(SchedulerRoundRobinTimeSlice,
     PreemptedProcessYieldsSoNextProcessFinishesFirst) {
  prosched::Scheduler scheduler(makeSmallCtx("rr", 1, 3));

  // p1: loop 100 { print 1; print 2; print 3; print 4 } → 400 instructions
  prosched::Process *p1 = new prosched::Process("p1", 1, 0);
  AddRaw(*p1, R"(FOR([PRINT("1"), PRINT("2"), PRINT("3"), PRINT("4")], 100))");
  p1->SetOwnedByScheduler(true);

  // p2: a single instruction — finishes in one quantum once it gets the core
  prosched::Process *p2 = new prosched::Process("p2", 2, 0);
  AddRaw(*p2, R"(PRINT("p2_done"))");
  p2->SetOwnedByScheduler(true);

  scheduler.AddProcess(p1); // p1 is first in the ready queue
  scheduler.AddProcess(p2); // p2 is queued behind p1
  scheduler.Start();

  // Wait for p2 to finish.
  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
  while (!p2->IsFinished() && std::chrono::steady_clock::now() < deadline)
    std::this_thread::yield();

  // p2 finished — and p1 must NOT have finished, proving p1 was preempted after
  // its time slice and p2 ran ahead of p1's remaining instructions.
  EXPECT_TRUE(p2->IsFinished());
  EXPECT_FALSE(p1->IsFinished());

  scheduler.Stop();
}

// Faithful "loop 5" version: p1 is preempted at quantum boundaries, but always
// resumes from exactly where it left off and ultimately executes every one of
// its 20 instructions (5 iterations * 4 prints).  p2 also finishes.  This
// proves the preempted process is re-queued (never dropped) and that the FOR
// body is correctly counted as individual statements.
TEST(SchedulerRoundRobinTimeSlice,
     PreemptedProcessResumesAndCompletesAllInstructions) {
  prosched::Scheduler scheduler(makeSmallCtx("rr", 1, 3));

  // p1: loop 5 { print 1; print 2; print 3; print 4 } → 20 instructions
  prosched::Process *p1 = new prosched::Process("p1", 1, 0);
  AddRaw(*p1, R"(FOR([PRINT("1"), PRINT("2"), PRINT("3"), PRINT("4")], 5))");
  p1->SetOwnedByScheduler(true);
  ASSERT_EQ(p1->GetTotalInstructions(), 20)
      << "FOR body must flatten to 5 * 4 = 20 individual statements";

  prosched::Process *p2 = new prosched::Process("p2", 2, 0);
  AddRaw(*p2, R"(PRINT("p2_done"))");
  p2->SetOwnedByScheduler(true);

  scheduler.AddProcess(p1);
  scheduler.AddProcess(p2);
  scheduler.Start();

  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
  while ((!p1->IsFinished() || !p2->IsFinished()) &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::yield();

  scheduler.Stop();

  EXPECT_TRUE(p1->IsFinished());
  EXPECT_TRUE(p2->IsFinished());
  // Every instruction ran — resumed exactly from each preemption point.
  EXPECT_EQ(p1->GetCurrentInstructionIndex(), 20);
}

} // namespace SchedulerRoundRobinTimeSlice

namespace SchedulerFreeFinishedProcesses {

// FreeFinishedProcesses is private and runs once per scheduler tick, so it is
// tested black-box through PagingManager's public page-residency API.

// A finished process's pages are released by the scheduler loop.
TEST(SchedulerFreeFinishedProcesses, FreedAfterProcessFinishes) {
  prosched::PagingManager pm(16, 16);
  prosched::Scheduler scheduler(makeSmallCtx("rr"), &pm);

  prosched::Process *p = new prosched::Process("task", 1, 0);
  AddRaw(*p, R"(PRINT("a"))");
  AddRaw(*p, R"(PRINT("b"))");
  p->SetOwnedByScheduler(true);
  ASSERT_TRUE(pm.PageIn(1, 0));
  ASSERT_TRUE(pm.IsPageResident(1, 0));
  scheduler.AddProcess(p);
  scheduler.Start();

  // Freeing lags up to one tick behind IsFinished(), so poll on residency
  // itself while the scheduler is still running.
  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
  while ((!p->IsFinished() || pm.IsPageResident(1, 0)) &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::yield();

  scheduler.Stop();

  EXPECT_TRUE(p->IsFinished());
  EXPECT_FALSE(pm.IsPageResident(1, 0));
}

// A dispatched-but-unfinished process keeps its pages.
TEST(SchedulerFreeFinishedProcesses, PagesRemainWhileRunning) {
  prosched::PagingManager pm(16, 16);
  prosched::Scheduler scheduler(makeSmallCtx("rr"), &pm);

  prosched::Process *p = new prosched::Process("sleeper", 1, 0);
  AddRaw(*p, R"(PRINT("start"))");
  AddRaw(*p, "SLEEP(200)");
  AddRaw(*p, R"(PRINT("end"))");
  p->SetOwnedByScheduler(true);
  ASSERT_TRUE(pm.PageIn(1, 0));
  scheduler.AddProcess(p);
  scheduler.Start();

  // Wait until the process has been dispatched.
  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
  while (p->GetState() != prosched::WAITING && !p->IsFinished() &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::yield();

  // Mid-SLEEP: still running and its pages must not have been released.
  EXPECT_TRUE(pm.IsPageResident(1, 0));
  EXPECT_FALSE(p->IsFinished());

  scheduler.Stop();
}

// Without a PagingManager the scheduler still completes non-memory work.
TEST(SchedulerFreeFinishedProcesses, NullPagingManagerRunsToCompletion) {
  prosched::Scheduler scheduler(makeSmallCtx("rr"));

  prosched::Process *p = new prosched::Process("task", 1, 0);
  AddRaw(*p, R"(PRINT("a"))");
  p->SetOwnedByScheduler(true);
  scheduler.AddProcess(p);
  scheduler.Start();

  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
  while (!p->IsFinished() && std::chrono::steady_clock::now() < deadline)
    std::this_thread::yield();

  scheduler.Stop();
  EXPECT_TRUE(p->IsFinished());
}

// Round-robin dispatch is not gated by physical-memory availability.
TEST(SchedulerFreeFinishedProcesses,
     AllQueuedProcessesFinishWithoutPreallocation) {
  prosched::PagingManager pm(16, 16);
  prosched::Scheduler scheduler(makeSmallCtx("rr"), &pm);

  std::vector<prosched::Process *> procs;
  for (int pid = 1; pid <= 3; ++pid) {
    prosched::Process *p =
        new prosched::Process("p" + std::to_string(pid), pid, 0);
    AddRaw(*p, R"(PRINT("x"))");
    AddRaw(*p, R"(PRINT("y"))");
    p->SetOwnedByScheduler(true);
    scheduler.AddProcess(p);
    procs.push_back(p);
  }

  scheduler.Start();

  auto allFinished = [&procs] {
    for (prosched::Process *p : procs)
      if (!p->IsFinished())
        return false;
    return true;
  };
  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
  while (!allFinished() && std::chrono::steady_clock::now() < deadline)
    std::this_thread::yield();

  scheduler.Stop();

  for (prosched::Process *p : procs)
    EXPECT_TRUE(p->IsFinished()) << p->GetName() << " did not finish";
}

} // namespace SchedulerFreeFinishedProcesses

// ─── SchedulerTerminatedProcessDisplay ────────────────────────────────────

// MO2 requires a process that shut down from a memory access violation to be
// reported as such ("Process <name> shut down due to memory access violation
// error..."), i.e. distinguishable from one that finished normally.
namespace SchedulerTerminatedProcessDisplay {

// A normally FINISHED process is listed
TEST(SchedulerTerminatedProcessDisplay, FinishedProcessAppearsInOutput) {
  prosched::Scheduler scheduler(makeTestCtx());
  prosched::Process *p = new prosched::Process("normal_finish", 1, 0);
  p->SetState(prosched::FINISHED);
  scheduler.AddProcess(p);

  std::ostringstream out;
  scheduler.PrintProcesses(out);
  EXPECT_NE(out.str().find("normal_finish"), std::string::npos);
}

// A TERMINATED process must still appear in the screen -ls listing (before, it
// showed under "Finished processes:"). REGRESSION: commit 994b5f8 split
// terminated processes into their own bucket in PrintProcesses but never prints
// that bucket, so they now vanish from the listing entirely. This test guards
// against that. (The access-violation notice itself is a screen -r concern —
// see ControllerAccessViolationNotice.MatchesSpecFormat + SchedulerFindTerminatedProcess.)
TEST(SchedulerTerminatedProcessDisplay, TerminatedProcessStillAppearsInListing) {
  prosched::Scheduler scheduler(makeTestCtx());
  prosched::Process *p = new prosched::Process("crashed_proc", 2, 0);
  p->SetState(prosched::TERMINATED);
  scheduler.AddProcess(p);

  std::ostringstream out;
  scheduler.PrintProcesses(out);
  EXPECT_NE(out.str().find("crashed_proc"), std::string::npos);
}

} // namespace SchedulerTerminatedProcessDisplay

// ─── SchedulerFindTerminatedProcessByName (screen -r) ──────────────────────
// screen -r uses this to report a memory-access-violation shutdown instead of
// the generic "not found".
namespace SchedulerFindTerminatedProcess {

// A TERMINATED process is found by name
TEST(SchedulerFindTerminatedProcess, ReturnsTerminatedProcess) {
  prosched::Scheduler scheduler(makeTestCtx());
  prosched::Process *p = new prosched::Process("crashed", 1, 0);
  p->SetState(prosched::TERMINATED);
  scheduler.AddProcess(p);
  EXPECT_EQ(scheduler.FindTerminatedProcessByName("crashed"), p);
}

// A live (non-terminated) process is NOT returned by the terminated lookup
TEST(SchedulerFindTerminatedProcess, IgnoresLiveProcess) {
  prosched::Scheduler scheduler(makeTestCtx());
  prosched::Process *p = new prosched::Process("running", 1, 0);
  p->SetState(prosched::READY);
  scheduler.AddProcess(p);
  EXPECT_EQ(scheduler.FindTerminatedProcessByName("running"), nullptr);
}

// A name that was never created returns null
TEST(SchedulerFindTerminatedProcess, ReturnsNullForUnknownName) {
  prosched::Scheduler scheduler(makeTestCtx());
  EXPECT_EQ(scheduler.FindTerminatedProcessByName("nope"), nullptr);
}

} // namespace SchedulerFindTerminatedProcess

// ─── SchedulerConfigValidation ─────────────────────────────────────────────
// MO2: config values must be validated. There is no production validation, so
// bad ranges flow into rand() % (max - min + 1). With max-ins = min-ins - 1 the
// divisor is 0 -> rand() % 0 -> SIGFPE (Scheduler.h ~401, also generateProcess).
namespace SchedulerConfigValidation {

// MO2: config must be validated at STARTUP, not deferred. min-ins > max-ins is a
// static config error — invalid the moment the config is loaded. It should be
// rejected while the config is being built / the scheduler constructed (which is
// what Controller::initialize does at startup), with a clear error and a non-zero
// exit — NOT silently accepted and left to crash later via rand() % 0 when the
// first process is created (Scheduler.h:331/401).
//
// The fix belongs in Controller::initialize / config validation (the project's
// test-only validateOrDie already checks max_ins < min_ins; production never
// calls it). This test drives the earliest testable startup seam: building the
// config and constructing the scheduler. It reaches std::exit(0) — and so fails
// the ExitedWithCode(1) expectation — only because nothing rejects the bad range
// at startup today.
TEST(SchedulerConfigValidation, InvalidInsRangeRejectedAtStartup) {
  EXPECT_EXIT(
      {
        ConfigStruct *cs = makeDefault();
        cs->min_ins = 100;
        cs->max_ins = 99; // max < min: invalid range
        AlgoContext ctx = AlgoContext::buildConfig(cs);
        delete cs;
        prosched::Scheduler scheduler(ctx);
        (void)scheduler;
        std::exit(0); // reached only if startup did NOT reject the bad config
      },
      ::testing::ExitedWithCode(1), ".*");
}

// ─── validateOrDie's checks, against PRODUCTION ─────────────────────────────
// ConfigTest.cpp:414 defines `validateOrDie`, a complete 7-check config
// validator with clear messages and exit(1), covered by 19 green
// ConfigValidation.* tests. It is `static`, in an anonymous namespace, in the
// TEST file — `grep validateOrDie src/` returns nothing. So those 19 tests
// prove the mirror works, not that prosched validates anything.
//
// The tests below apply the same checks to real production code. Each corrupts
// one config field and asserts that constructing a real prosched::Scheduler
// refuses to proceed, mirroring InvalidInsRangeRejectedAtStartup — the pattern
// that went green only once a genuine guard landed at Scheduler.h:47.
//
// WHY THE SCHEDULER CONSTRUCTOR: it is the only production seam a unit test can
// hand an arbitrary AlgoContext. Controller::initialize reads the hardcoded
// path in CONFIG_FILENAME, so injecting a bad config there would mean writing
// to the repo's real config.txt. If validation is later centralised (e.g. a
// shared validate() in Config.h called from Controller::initialize), have the
// Scheduler constructor call it too rather than retargeting these — guarding at
// the point of use is what the existing min-ins check already does.
//
// The requirement being asserted is MO1's documented parameter ranges:
// num-cpu [1,128], quantum-cycles >= 1, batch-process-freq >= 1,
// min-ins >= 1, max-ins >= min-ins, delays-per-exec >= 0, scheduler in
// {fcfs, rr}.

namespace {

// Builds a valid default config, applies one corruption, and asserts the real
// Scheduler rejects it with exit(1). Reaching exit(0) means nothing validated.
template <typename Corrupt>
void ExpectConfigRejectedAtStartup(const char *what, Corrupt corrupt) {
  SCOPED_TRACE(what);
  EXPECT_EXIT(
      {
        ConfigStruct *cs = makeDefault();
        corrupt(cs);
        AlgoContext ctx = AlgoContext::buildConfig(cs);
        delete cs;
        prosched::Scheduler scheduler(ctx);
        (void)scheduler;
        std::exit(0); // reached only if startup did NOT reject the bad config
      },
      ::testing::ExitedWithCode(1), ".*");
}

} // namespace

// HIGH IMPACT — silent death. num-cpu 0 creates zero workers, so nothing is
// ever dispatched: the console accepts every command and quietly executes
// nothing, with no error pointing at the config.
TEST(SchedulerConfigValidation, NumCpuBelowOneRejectedAtStartup) {
  ExpectConfigRejectedAtStartup("num-cpu 0", [](ConfigStruct *cs) {
    cs->num_cpu = 0;
  });
}

// MO1 caps num-cpu at 128. Above it, Start() spawns that many real OS threads.
TEST(SchedulerConfigValidation, NumCpuAboveMaxRejectedAtStartup) {
  ExpectConfigRejectedAtStartup("num-cpu 129", [](ConfigStruct *cs) {
    cs->num_cpu = 129;
  });
}

// HIGH IMPACT — silent death. A scheduler string that is neither "fcfs" nor
// "rr" maps to SchedulerType::UNKNOWN, and SchedulerLoop runs neither branch,
// so no process is ever dispatched.
//
// Second defect on this path: AlgoContext::buildConfig sets rr_quantum_cycles
// only in the fcfs and rr branches (Context.h:29-36). On the UNKNOWN branch it
// is left uninitialised, and AlgoContext declares it as a plain `int` with no
// default member initialiser — so reading ctx.rr_quantum_cycles afterwards is
// undefined behaviour.
TEST(SchedulerConfigValidation, UnknownSchedulerTypeRejectedAtStartup) {
  ExpectConfigRejectedAtStartup("scheduler \"bogus\"", [](ConfigStruct *cs) {
    cs->scheduler = "bogus";
  });
}

// HIGH IMPACT — crash. GenerateProcessesCycle does
// `cpuCycles % ctx.batch_process_frequency` (Scheduler.h:534), so 0 divides by
// zero and raises SIGFPE. It sits behind `generatingProcesses &&`, so the
// program boots fine, serves screen -ls / vmstat / process-smi fine, and dies
// the instant the user types "scheduler-start".
TEST(SchedulerConfigValidation, ZeroBatchProcessFreqRejectedAtStartup) {
  ExpectConfigRejectedAtStartup("batch-process-freq 0", [](ConfigStruct *cs) {
    cs->batch_process_freq = 0;
  });
}

// MO1 range is [1, 2^32]. A 0 quantum makes CheckAndIncrementQuantum preempt on
// every tick (1 >= 0), degenerating to quantum 1 — benign, but out of spec.
TEST(SchedulerConfigValidation, ZeroQuantumCyclesRejectedAtStartup) {
  ExpectConfigRejectedAtStartup("quantum-cycles 0", [](ConfigStruct *cs) {
    cs->scheduler = "rr";
    cs->rr_quantum_cycles = 0;
  });
}

// MO1 range is [1, 2^32]. min-ins 0 yields zero-instruction processes that
// finish the moment they are dispatched.
TEST(SchedulerConfigValidation, MinInsBelowOneRejectedAtStartup) {
  ExpectConfigRejectedAtStartup("min-ins 0", [](ConfigStruct *cs) {
    cs->min_ins = 0;
    cs->max_ins = 0; // keep max >= min so this isolates the min-ins check
  });
}

// MO1 range is [0, 2^32]. A negative delay makes cyclesLeft start below zero,
// so TickExecution's `<= 0` test passes immediately every tick.
TEST(SchedulerConfigValidation, NegativeDelayPerExecRejectedAtStartup) {
  ExpectConfigRejectedAtStartup("delay-per-exec -1", [](ConfigStruct *cs) {
    cs->delay_per_exec = -1;
  });
}

} // namespace SchedulerConfigValidation

// ─── FOR loops are never generated (MO1 §Process instructions) ──────────────
// MO1 lists FOR as one of the six instruction types a generated process draws
// from, "nestable up to 3 levels". Statement.cpp drops kFor from the pool when
// `max_depth >= kMaxNestingDepth` (3) — correct for recursion — but BOTH
// production call sites pass 3 as the INITIAL depth (Scheduler.h:350 in
// generateProcess, :419 in CreateNamedProcess). The pool is therefore already
// at max depth on the first call, so FOR is excluded from every top-level
// draw and a generated process contains zero loops. The initial call should
// pass 0.

namespace SchedulerForGeneration {

// Observed via the instruction count, not by scanning for kFor: AddInstruction
// UNROLLS a FOR into `repeats` copies of its body, so a kFor never survives
// into the statements vector even when one is generated. With min-ins ==
// max-ins == 500 the draw is exactly 500 statements, so any FOR among them
// pushes the stored total above 500. It stays at exactly 500 today because no
// FOR is ever drawn.
//
// NOTE for whoever fixes this: the fix is the initial depth at the two call
// sites (3 -> 0), NOT removing the `max_depth >= kMaxNestingDepth` guard —
// that guard is correct and is covered by
// InterpreterGetRandomStatement.NeverReturnsForAtMaxDepth. This test also
// assumes unrolling stays; if FOR is ever changed to execute via
// Interpreter::ExecuteFor instead, retarget it rather than deleting it.
TEST(SchedulerForGeneration, GeneratedProcessCanContainForLoops) {
  ConfigStruct *cs = makeDefault();
  cs->scheduler = "fcfs";
  cs->batch_process_freq = 1000000;
  cs->min_ins = 500;
  cs->max_ins = 500;
  AlgoContext ctx = AlgoContext::buildConfig(cs);
  delete cs;

  prosched::Scheduler scheduler(ctx);
  prosched::Process *p = scheduler.CreateNamedProcess("for_gen", 256);
  ASSERT_NE(p, nullptr);

  EXPECT_GT(p->GetTotalInstructions(), 500)
      << "500 drawn instructions produced exactly "
      << p->GetTotalInstructions()
      << " stored instructions, so no FOR was ever generated: both call sites "
         "pass max_depth=3 (== kMaxNestingDepth), which removes kFor from the "
         "pool before the very first draw";

  delete p;
}

} // namespace SchedulerForGeneration

// ─── What a finished process keeps alive (footprint, not correctness) ───────
// Scheduler::processes never shrinks, and that is CORRECT — MO1 requires
// finished processes to stay listed in screen -ls / report-util. The waste is
// what each finished process still holds: its entire `statements` vector, which
// is needed only while it is running. The report needs a name, timestamps and
// two counts.
//
// This is a MEASUREMENT, not a pass/fail assertion, and it is DISABLED for that
// reason. It cannot be a real test today: GetTotalInstructions() returns
// statements.size(), so a test has no way to tell "instructions released" from
// "process had none". Caching the count in an int and clearing the vector on
// completion would fix the footprint AND make this assertable — at which point
// this becomes: after finishing, retained statements == 0 while
// GetTotalInstructions() still reports the original count.
//
// Run it deliberately:
//   ./build/prosched/prosched_tests --gtest_also_run_disabled_tests \
//       --gtest_filter='*RetentionPerf*'

namespace SchedulerRetentionPerf {

TEST(SchedulerRetentionPerf, DISABLED_FinishedProcessesReleaseTheirInstructions) {
  const int kProcesses = 200;
  const int kInstructions = 1000; // demo 2 uses min-ins == max-ins == 1000

  ConfigStruct *cs = makeDefault();
  cs->scheduler = "fcfs";
  cs->batch_process_freq = 1000000;
  cs->min_ins = kInstructions;
  cs->max_ins = kInstructions;
  AlgoContext ctx = AlgoContext::buildConfig(cs);
  delete cs;

  prosched::Scheduler scheduler(ctx);
  long retained = 0;
  for (int i = 0; i < kProcesses; ++i) {
    prosched::Process *p =
        scheduler.CreateNamedProcess("held" + std::to_string(i), 1024);
    p->SetState(prosched::ProcessState::FINISHED);
    retained += p->GetTotalInstructions();
    scheduler.AddProcess(p); // scheduler owns and frees it
  }

  // Rough lower bound: each Statement is at least its own size, and every one
  // owns a vector<string> of arguments whose heap blocks are not counted here.
  const long bytes = retained * static_cast<long>(sizeof(prosched::Statement));
  std::cout << "  " << kProcesses << " finished processes retain " << retained
            << " Statement objects, >= " << (bytes / 1024 / 1024) << " MiB\n"
            << "  (demo 2 generates one process per tick for 10 s, so this is a"
               " small fraction of the real total)\n";

  EXPECT_EQ(retained, 0)
      << retained
      << " instructions are still held by processes that already finished; "
         "only the name, timestamps and instruction counts are needed for the "
         "screen -ls / report-util listing";
}

} // namespace SchedulerRetentionPerf
