#include "Config.h"
#include "Context.h"
#include "memory/PagingManager.h"
#include "scheduler/Scheduler.h"
#include "scheduler/process/Process.h"
#include "scheduler/worker/Worker.h"
#include <chrono>
#include <gtest/gtest.h>
#include <thread>
#include <vector>

// MO2 grading scenario, question 4:
//
//     num-cpu 16 / scheduler rr / quantum-cycles 5 / batch-process-freq 1
//     min-ins 1000 / max-ins 2000 / delay-per-exec 1
//     max-overall-mem 32768 / mem-per-frame 32768
//     min-mem-per-proc 32768 / max-mem-per-proc 32768
//
//   "For screen -ls, it should show < 100% CPU utilization printed in the
//    screen-ls command, as only one process is allowed to run due to memory
//    limits."
//
// max-overall-mem / mem-per-frame = 1, so the machine has exactly ONE frame.
// Each process is 32768 bytes = one page, and its symbol table (bytes 0-64,
// Interpreter.cpp) sits on that same page. One frame therefore means one
// process can hold memory at a time, and the expected utilization is 1/16 =
// 6.25%, not 100%.
//
// These tests cover the three reasons the current implementation reports 100%:
//   1. Dispatch never consults memory  - Scheduler::RoundRobin / ::FCFS pop the
//      ready queue onto any free worker without asking the PagingManager
//      whether a frame is available.
//   2. A page fault does not release the core - Worker::TickExecution leaves
//      currentProcess set, and Worker::CheckAndIncrementQuantum returns early
//      (suppressing quantum expiry) whenever the last instruction faulted, so a
//      process that cannot get a frame occupies its core indefinitely.
//   3. Utilization counts assignment, not progress - Scheduler::PrintProcesses
//      and ::GetCpuUtilization count workers whose currentProcess != nullptr.

namespace {

prosched::Statement WriteStmt(const std::string &addr, const std::string &val) {
  prosched::Statement s;
  s.keyword = prosched::Keyword::kWrite;
  s.args = {addr, val};
  return s;
}

void AddRaw(prosched::Process &p, const std::string &src) {
  prosched::Interpreter interp;
  auto stmts = interp.Parse(src);
  for (auto &s : stmts)
    p.AddInstruction(s);
}

AlgoContext makeMemoryCtx(int num_cpu, int quantum, int memPerProc,
                          int memPerFrame, int maxOverallMem, int minIns,
                          int maxIns, int delay, int batchFreq = 1000000) {
  ConfigStruct *cs = makeDefault();
  cs->scheduler = "rr";
  cs->num_cpu = num_cpu;
  cs->rr_quantum_cycles = quantum;
  cs->batch_process_freq = batchFreq;
  cs->min_ins = minIns;
  cs->max_ins = maxIns;
  cs->delay_per_exec = delay;
  cs->min_mem_per_proc = memPerProc;
  cs->max_mem_per_proc = memPerProc;
  cs->mem_per_frame = memPerFrame;
  cs->max_overall_mem = maxOverallMem;
  AlgoContext ctx = AlgoContext::buildConfig(cs);
  delete cs;
  return ctx;
}

// Highest utilization observed while the scheduler runs. Sampling the peak
// rather than a single reading is what "screen -ls" does - the grader types the
// command at an arbitrary moment - and it keeps the test from depending on
// which instant the sample lands on.
double PeakUtilization(prosched::Scheduler &scheduler,
                       std::chrono::milliseconds window) {
  double peak = 0.0;
  const auto deadline = std::chrono::steady_clock::now() + window;
  while (std::chrono::steady_clock::now() < deadline) {
    peak = std::max(peak, scheduler.GetCpuUtilization());
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return peak;
}

} // namespace

namespace SchedulerMemoryAdmission {

// The grading configuration, reproduced verbatim, driven the way the grader
// drives it: "scheduler-test" is Start() + ResumeGenerating() (Controller.cpp,
// CLI_SCHEDULER_START), so batch-process-freq 1 keeps feeding the ready queue
// for the whole run. One frame exists, so at most one process can be resident
// and utilization must stay below 100%.
TEST(SchedulerMemoryAdmission, GradingConfigStaysBelowFullUtilization) {
  const int kCores = 16;
  const int kProcMem = 32768;

  prosched::PagingManager pm(32768, 32768);
  ASSERT_EQ(pm.GetTotalFrameCount(), 1)
      << "precondition: mem-per-frame == max-overall-mem means a single frame";

  prosched::Scheduler scheduler(
      makeMemoryCtx(kCores, /*quantum=*/5, kProcMem, /*memPerFrame=*/32768,
                    /*maxOverallMem=*/32768, /*minIns=*/1000, /*maxIns=*/2000,
                    /*delay=*/1, /*batchFreq=*/1),
      &pm);

  ASSERT_TRUE(scheduler.Start());
  scheduler.ResumeGenerating();
  const double peak = PeakUtilization(scheduler, std::chrono::milliseconds(400));
  scheduler.StopGenerating();
  scheduler.Stop();

  std::cout << "[ INFO     ] peak CPU utilization: " << peak << "%\n";
  EXPECT_LT(peak, 100.0)
      << "CPU utilization peaked at " << peak
      << "% with a single 32768-byte frame for 16 processes of 32768 bytes "
         "each. Scheduler::RoundRobin dispatches from the ready queue to any "
         "idle worker without checking whether the PagingManager can give the "
         "process a frame, so all 16 cores are occupied at once. The MO2 "
         "grading scenario expects < 100% here (1 of 16 cores = 6.25%), "
         "because only one process fits in memory.";
}

// The stronger form of the same rule, with the "PRINT needs no memory" escape
// removed: every instruction is a WRITE, so every process needs a frame on
// every instruction. Cores in use must not exceed frames available.
TEST(SchedulerMemoryAdmission, CoresInUseNeverExceedsAvailableFrames) {
  const int kCores = 4;
  const int kProcMem = 64;

  prosched::PagingManager pm(64, 64);
  ASSERT_EQ(pm.GetTotalFrameCount(), 1) << "precondition: a single frame";

  prosched::Scheduler scheduler(
      makeMemoryCtx(kCores, /*quantum=*/5, kProcMem, /*memPerFrame=*/64,
                    /*maxOverallMem=*/64, /*minIns=*/1, /*maxIns=*/1000,
                    /*delay=*/0),
      &pm);

  std::vector<prosched::Statement> program;
  for (int i = 0; i < 400; ++i) {
    program.push_back(WriteStmt("0x0", std::to_string(i % 256)));
  }

  for (int i = 0; i < kCores; ++i) {
    prosched::Process *p = scheduler.CreateProcessWithInstructions(
        "writer" + std::to_string(i + 1), kProcMem, program);
    ASSERT_NE(p, nullptr);
    ASSERT_NE(scheduler.AddProcess(p), nullptr);
  }

  ASSERT_TRUE(scheduler.Start());
  const double peak = PeakUtilization(scheduler, std::chrono::milliseconds(300));
  scheduler.Stop();

  const double peakCores = peak * kCores / 100.0;
  EXPECT_LE(peakCores, 1.0)
      << "up to " << peakCores << " of " << kCores
      << " cores held a process at once, against 1 available frame. Every "
         "instruction in these processes is a WRITE, so each one needs its "
         "page resident to make progress - more running processes than frames "
         "means the extras are occupying cores they cannot use.";
}

// The other side of admission control. This test used to assert the opposite -
// that a process bigger than physical memory still runs, because demand paging
// exists precisely so that it can - and it passed: a pure-PRINT program touches
// no data pages, so nothing stopped it.
//
// MO2 grading question 7 rules that out. Its config gives every process twice
// as much declared memory as the machine has and expects 0% utilization
// indefinitely, and no working-set rule can separate that config from this one:
// both are "declared size exceeds physical memory", and in both the instruction
// actually executing needs one page or none. The size comparison is the only
// discriminator available, so the rule is now Scheduler::FitsInPhysicalMemory
// and such a process is never dispatched. What demand paging still buys is
// covered by DemandPagingStillOversubscribesMemory below.
TEST(SchedulerMemoryAdmission, ProcessLargerThanPhysicalMemoryIsNeverDispatched) {
  prosched::PagingManager pm(256, 256);
  ASSERT_EQ(pm.GetTotalFrameCount(), 1) << "precondition: a single frame";

  prosched::Scheduler scheduler(
      makeMemoryCtx(/*num_cpu=*/1, /*quantum=*/10, /*memPerProc=*/256,
                    /*memPerFrame=*/256, /*maxOverallMem=*/256, /*minIns=*/1,
                    /*maxIns=*/1000, /*delay=*/0),
      &pm);

  prosched::Statement print;
  print.keyword = prosched::Keyword::kPrint;
  print.args = {"\"hello\""};
  const std::vector<prosched::Statement> program(5, print);

  // 2048 bytes = 8 pages on a machine that owns 1.
  prosched::Process *p =
      scheduler.CreateProcessWithInstructions("big", 2048, program);
  ASSERT_NE(p, nullptr);
  ASSERT_NE(scheduler.AddProcess(p), nullptr);

  ASSERT_TRUE(scheduler.Start());
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (std::chrono::steady_clock::now() < deadline && !p->IsFinished()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  const int reached = p->GetCurrentInstructionIndex();
  scheduler.Stop();

  EXPECT_EQ(reached, 0)
      << "a 2048-byte process on a 256-byte machine retired " << reached
      << " of 5 instructions. It must retire none: it can never hold the page "
         "it is executing, and question 7's expected deadlock depends on that "
         "being true for every process in the system.";
}

// The guarantee that survives: memory is still oversubscribed across processes.
// Four 1024-byte processes fit individually but need 64 pages between them
// against 16 frames, so every one of them can only finish if pages are brought
// in and evicted on demand. This is what the inverted test above used to
// protect, expressed in a way question 7 does not contradict.
TEST(SchedulerMemoryAdmission, DemandPagingStillOversubscribesMemory) {
  const int kProcs = 4;
  const int kProcMem = 1024;

  prosched::PagingManager pm(/*memPerFrame=*/64, /*maxOverallMem=*/1024);
  ASSERT_EQ(pm.GetTotalFrameCount(), 16);

  prosched::Scheduler scheduler(
      makeMemoryCtx(/*num_cpu=*/4, /*quantum=*/5, kProcMem,
                    /*memPerFrame=*/64, /*maxOverallMem=*/1024, /*minIns=*/1,
                    /*maxIns=*/1000, /*delay=*/0),
      &pm);

  // Writes spread across the address space, so the processes really do compete
  // for frames instead of all living on page 0.
  std::vector<prosched::Statement> program;
  for (int addr = 0; addr < kProcMem - 2; addr += 64) {
    program.push_back(WriteStmt("0x" + [addr] {
                                  std::ostringstream oss;
                                  oss << std::hex << addr;
                                  return oss.str();
                                }(),
                                "7"));
  }

  std::vector<prosched::Process *> procs;
  for (int i = 0; i < kProcs; ++i) {
    prosched::Process *p = scheduler.CreateProcessWithInstructions(
        "writer" + std::to_string(i + 1), kProcMem, program);
    ASSERT_NE(p, nullptr);
    ASSERT_NE(scheduler.AddProcess(p), nullptr);
    procs.push_back(p);
  }

  ASSERT_TRUE(scheduler.Start());
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  auto allDone = [&procs] {
    for (prosched::Process *p : procs) {
      if (!p->IsFinished()) {
        return false;
      }
    }
    return true;
  };
  while (std::chrono::steady_clock::now() < deadline && !allDone()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  const bool done = allDone();
  const auto stats = pm.GetMemoryStats();
  scheduler.Stop();

  EXPECT_TRUE(done)
      << "4 processes of " << kProcMem << " bytes each (64 pages in total) did "
      << "not all finish on a 16-frame machine in 5 s. Each one fits in "
         "memory on its own, so the admission rule must let them all run and "
         "the pager must multiplex the frames between them.";
  EXPECT_GT(stats.pagesPagedIn, 0u)
      << "no page was ever brought in, so this configuration did not exercise "
         "demand paging at all";
}

// The handler Scheduler::attachPaging installs answers one question - "may this
// access proceed?" - about two different failures. A page that was just brought
// in still costs the instruction a restart; a page that could NOT be brought in
// because every frame is pinned elsewhere is not there to touch at all. Both
// must abandon the instruction.
//
// Returning PageIn's own result conflated them: a failed page-in read as "no
// fault" and the access went ahead. Interpreter::CheckAccess has a
// `|| IsPagedOut(page)` clause that catches this, but only for a page that was
// once resident and got evicted - paged_out_pages_ has no entry for a page that
// never made it into a frame in the first place, which is exactly the case a
// failed page-in produces.
TEST(SchedulerMemoryAdmission, WriteToAPageThatCannotBePagedInDoesNotSucceed) {
  prosched::PagingManager pm(64, 64);
  ASSERT_EQ(pm.GetTotalFrameCount(), 1) << "precondition: a single frame";

  prosched::Scheduler scheduler(
      makeMemoryCtx(/*num_cpu=*/1, /*quantum=*/10, /*memPerProc=*/128,
                    /*memPerFrame=*/64, /*maxOverallMem=*/64, /*minIns=*/1,
                    /*maxIns=*/1000, /*delay=*/0),
      &pm);

  // 0x40 = 64 is page 1 at a 64-byte page size, and the operand is a literal,
  // so this instruction touches that one page and nothing else.
  prosched::Process *writer = scheduler.CreateProcessWithInstructions(
      "writer", 128, {WriteStmt("0x40", "7")});
  prosched::Process *holder = scheduler.CreateProcessWithInstructions(
      "holder", 64, {WriteStmt("0x0", "1")});
  ASSERT_NE(writer, nullptr);
  ASSERT_NE(holder, nullptr);

  // Hand the machine's only frame to the holder and pin it, so the writer's
  // page can never be brought in. Nothing here has ever made page 1 resident.
  ASSERT_TRUE(pm.PageIn(holder->GetPID(), 0));
  pm.PinPage(holder->GetPID(), 0);
  ASSERT_FALSE(pm.IsPageResident(writer->GetPID(), 1));

  writer->ExecuteInstructions(0);

  EXPECT_TRUE(writer->GetLastInstructionWasPageFault())
      << "the only frame is pinned by another process, so paging in the "
         "writer's page must have failed - that is a fault, not a clear "
         "access";
  EXPECT_EQ(writer->GetCurrentInstructionIndex(), 0)
      << "the WRITE retired against a page that holds no frame";

  const auto written =
      writer->GetInterpreter().GetPageSnapshot(/*pageBase=*/64, /*size=*/64);
  EXPECT_TRUE(written.empty())
      << "the WRITE stored " << written.size()
      << " value(s) into a page with no frame behind it. Physical memory was "
         "never allocated for it, so this is a write to memory the process "
         "does not own; it also survives into the backing store the moment "
         "that page is ever paged out.";

  // Control: with the pin released the same instruction completes, which shows
  // the fault above came from the unavailable frame and not from the address.
  pm.UnpinPage(holder->GetPID(), 0);
  for (int tick = 0; tick < 5 && writer->GetCurrentInstructionIndex() == 0;
       ++tick) {
    writer->ExecuteInstructions(0);
  }
  EXPECT_EQ(writer->GetCurrentInstructionIndex(), 1)
      << "once a frame could be freed the WRITE should have retired";
}

} // namespace SchedulerMemoryAdmission

namespace WorkerPageFaultRelease {

// A process wired to a pager that never satisfies the fault. The handler
// contract (Scheduler::attachPaging) is "true == the page was not usable,
// abandon and retry the instruction", so returning true always models a machine
// whose only frame is held by somebody else.
void MakePermanentFaulter(prosched::Process &p) {
  p.SetMemoryBounds(0, 64);
  p.GetInterpreter().SetPageSize(64);
  p.GetInterpreter().SetPageFaultHandler([](int) { return true; });
  AddRaw(p, "DECLARE(x, 1)");
  AddRaw(p, "DECLARE(y, 2)");
  AddRaw(p, "DECLARE(z, 3)");
}

// Quantum expiry is the only thing that can take a core back from a process
// that is not sleeping or finished. Worker::CheckAndIncrementQuantum returns
// false when the last instruction page-faulted, which means the quantum of a
// faulting process never advances and never expires.
TEST(WorkerPageFaultRelease, QuantumStillExpiresWhileAProcessIsFaulting) {
  const int kQuantum = 2;
  ConfigStruct *cs = makeDefault();
  cs->scheduler = "rr";
  cs->rr_quantum_cycles = kQuantum;
  cs->delay_per_exec = 0;
  AlgoContext ctx = AlgoContext::buildConfig(cs);
  delete cs;

  prosched::Process p("faulter", 1, 0);
  MakePermanentFaulter(p);

  prosched::Worker w(0, ctx);
  ASSERT_NE(w.AssignProcess(&p), nullptr);

  bool quantumExpired = false;
  for (int tick = 0; tick < 20 && !quantumExpired; ++tick) {
    quantumExpired = w.CheckAndIncrementQuantum(kQuantum);
    w.RunCycle();
  }

  ASSERT_EQ(p.GetCurrentInstructionIndex(), 0)
      << "precondition: the process must be making no progress, otherwise this "
         "is not testing the fault path";
  EXPECT_TRUE(quantumExpired)
      << "20 ticks of a quantum-2 process that page-faults on every "
         "instruction, and the quantum never expired. "
         "Worker::CheckAndIncrementQuantum returns early when "
         "GetLastInstructionWasPageFault() is set, so a process that cannot "
         "get a frame is exempt from preemption.";
}

// The consequence: the core is never handed back, so it counts as "used" for
// the whole run while retiring zero instructions.
TEST(WorkerPageFaultRelease, FaultingProcessGivesUpItsCore) {
  const int kQuantum = 2;
  ConfigStruct *cs = makeDefault();
  cs->scheduler = "rr";
  cs->rr_quantum_cycles = kQuantum;
  cs->delay_per_exec = 0;
  AlgoContext ctx = AlgoContext::buildConfig(cs);
  delete cs;

  prosched::Process p("faulter", 2, 0);
  MakePermanentFaulter(p);

  prosched::Worker w(0, ctx);
  ASSERT_NE(w.AssignProcess(&p), nullptr);

  // Mirrors one iteration of Scheduler::SchedulerLoop for a single core:
  // quantum check, then the tick, then collection of anything preempted.
  for (int tick = 0; tick < 20; ++tick) {
    if (w.CheckAndIncrementQuantum(kQuantum)) {
      w.PreemptProcess();
    }
    w.RunCycle();
    w.GetAndClearPreemptedProcess();
  }

  ASSERT_EQ(p.GetCurrentInstructionIndex(), 0)
      << "precondition: the process must be making no progress";
  EXPECT_FALSE(w.IsBusy())
      << "after 20 ticks the core is still held by a process that has not "
         "completed a single instruction and cannot obtain a frame. A blocked "
         "process should be taken off the core and returned to the ready "
         "queue; holding it makes the core count as busy in "
         "Scheduler::GetCpuUtilization while no work is being done.";
}

} // namespace WorkerPageFaultRelease

// MO2 grading scenario, question 7:
//
//     num-cpu 8 / scheduler rr / quantum-cycles 4 / batch-process-freq 1
//     min-ins 10000 / max-ins 10000 / delay-per-exec 0
//     max-overall-mem 16384 / mem-per-frame 8
//     min-mem-per-proc 32768 / max-mem-per-proc 32768
//
//   "0% CPU utilization occurs indefinitely because the memory manager will
//    attempt to page in and out on the processes but can no longer progress
//    because there's insufficient memory. The system reaches a deadlock (no
//    processes executing)."
//
// Every process declares 32768 bytes = 4096 pages against 2048 frames, i.e.
// twice the whole machine. Demand paging on its own does not notice: a process
// only ever touches a handful of pages, so it runs happily and the config
// reported 100% utilization. Scheduler::FitsInPhysicalMemory is the rule that
// makes the shortfall visible - a process that can never be resident is never
// queued, and with every process in that state the machine idles.
namespace SchedulerOversizedProcess {

// The rule itself, at the boundary. Equal-to-memory must still fit: that is the
// question-4 configuration, which has to keep running one process at a time.
TEST(SchedulerOversizedProcess, FitRuleIsExclusiveAtTotalMemory) {
  prosched::PagingManager pm(/*memPerFrame=*/8, /*maxOverallMem=*/16384);
  prosched::Scheduler scheduler(
      makeMemoryCtx(/*num_cpu=*/8, /*quantum=*/4, /*memPerProc=*/32768,
                    /*memPerFrame=*/8, /*maxOverallMem=*/16384,
                    /*minIns=*/1, /*maxIns=*/1, /*delay=*/0),
      &pm);

  prosched::Process smaller("smaller", 1, 0);
  smaller.SetMemoryBounds(0, 16383);
  prosched::Process exact("exact", 2, 0);
  exact.SetMemoryBounds(0, 16384);
  prosched::Process oversized("oversized", 3, 0);
  oversized.SetMemoryBounds(0, 16385);

  EXPECT_TRUE(scheduler.FitsInPhysicalMemory(&smaller));
  EXPECT_TRUE(scheduler.FitsInPhysicalMemory(&exact))
      << "a process exactly the size of physical memory is admissible - the "
         "question-4 config (max-overall-mem == mem-per-proc) depends on it";
  EXPECT_FALSE(scheduler.FitsInPhysicalMemory(&oversized));
}

// A process that cannot ever be resident is still created and reported, it just
// never reaches the ready queue: "no processes executing", not "no processes".
TEST(SchedulerOversizedProcess, OversizedProcessIsRecordedButNeverQueued) {
  prosched::PagingManager pm(/*memPerFrame=*/8, /*maxOverallMem=*/16384);
  prosched::Scheduler scheduler(
      makeMemoryCtx(/*num_cpu=*/8, /*quantum=*/4, /*memPerProc=*/32768,
                    /*memPerFrame=*/8, /*maxOverallMem=*/16384,
                    /*minIns=*/1, /*maxIns=*/1, /*delay=*/0),
      &pm);

  prosched::Process *fits = scheduler.CreateNamedProcess("fits", 16384);
  prosched::Process *oversized = scheduler.CreateNamedProcess("oversized", 32768);
  ASSERT_NE(scheduler.AddProcess(fits), nullptr);
  ASSERT_NE(scheduler.AddProcess(oversized), nullptr);

  const auto all = scheduler.GetAllProcesses();
  EXPECT_EQ(all.size(), 2u)
      << "both processes must remain visible to screen -ls / process-smi";

  const auto queued = scheduler.GetReadyQueueSnapshot();
  ASSERT_EQ(queued.size(), 1u)
      << "a 32768-byte process was queued against 16384 bytes of physical "
         "memory; it can never hold the page it is executing, so it must not "
         "be dispatchable";
  EXPECT_EQ(queued[0]->GetName(), "fits");
}

// The scenario end to end: the grading config, driven the way the grader drives
// it, must idle at 0% for the whole observation window.
TEST(SchedulerOversizedProcess, GradingConfigIdlesAtZeroUtilization) {
  prosched::PagingManager pm(/*memPerFrame=*/8, /*maxOverallMem=*/16384);
  ASSERT_EQ(pm.GetTotalFrameCount(), 2048);

  prosched::Scheduler scheduler(
      makeMemoryCtx(/*num_cpu=*/8, /*quantum=*/4, /*memPerProc=*/32768,
                    /*memPerFrame=*/8, /*maxOverallMem=*/16384,
                    /*minIns=*/10000, /*maxIns=*/10000, /*delay=*/0,
                    /*batchFreq=*/1),
      &pm);

  ASSERT_TRUE(scheduler.Start());
  scheduler.ResumeGenerating();
  const double peak = PeakUtilization(scheduler, std::chrono::milliseconds(500));
  scheduler.StopGenerating();
  const std::size_t created = scheduler.GetAllProcesses().size();
  scheduler.Stop();

  ASSERT_GT(created, 0u)
      << "precondition: the scheduler must have generated processes, otherwise "
         "0% utilization proves nothing";
  EXPECT_DOUBLE_EQ(peak, 0.0)
      << "peak CPU utilization was " << peak << "% across " << created
      << " processes of 32768 bytes each on a 16384-byte machine. Every one of "
         "them declares twice as much memory as exists, so none can ever hold "
         "the page it is executing; the MO2 question-7 scenario expects 0% "
         "utilization indefinitely (deadlock, no processes executing).";
}

// The guard must not cost anything when memory is adequate: the same driver
// with processes that fit has to keep the cores busy.
TEST(SchedulerOversizedProcess, ProcessesThatFitStillRun) {
  prosched::PagingManager pm(/*memPerFrame=*/8, /*maxOverallMem=*/16384);
  prosched::Scheduler scheduler(
      makeMemoryCtx(/*num_cpu=*/8, /*quantum=*/4, /*memPerProc=*/512,
                    /*memPerFrame=*/8, /*maxOverallMem=*/16384,
                    /*minIns=*/1000, /*maxIns=*/1000, /*delay=*/0,
                    /*batchFreq=*/1),
      &pm);

  ASSERT_TRUE(scheduler.Start());
  scheduler.ResumeGenerating();
  const double peak = PeakUtilization(scheduler, std::chrono::milliseconds(500));
  scheduler.StopGenerating();
  scheduler.Stop();

  EXPECT_GT(peak, 0.0)
      << "512-byte processes fit comfortably in 16384 bytes of memory, so the "
         "oversized-process guard must leave them dispatchable";
}

} // namespace SchedulerOversizedProcess
