#include "Config.h"
#include "Context.h"
#include "commands/Interpreter.h"
#include "memory/PagingManager.h"
#include "scheduler/Scheduler.h"
#include "scheduler/process/Process.h"
#include "scheduler/worker/Worker.h"
#include <chrono>
#include <gtest/gtest.h>
#include <thread>
#include <vector>

// A pin exists to stop the pager from evicting a page out from under an access
// that is happening right now. It is therefore bounded by the access: the
// moment the process is not mid-access - the instruction retired, the
// instruction was abandoned on a fault, or the process left its core - the pin
// has no one to protect and must be gone.
//
// When pins outlive that window they do not merely waste a frame, they stop the
// machine. PageIn evicts by walking the load-order queue for an unpinned victim
// and gives up when it finds none (PagingManager.h). A process that keeps a pin
// while sitting in the ready queue is holding a frame it is not using and
// cannot be asked to give back, because it is not running. With as many
// resident processes as there are frames - which is exactly what
// Scheduler::CanAdmitAnotherProcess admits - a handful of preemptions is enough
// to leave every frame pinned by a process that is off-core, and from that
// point no page fault anywhere in the system can ever be satisfied again.
//
// MEASURED on the MO2 demo-2 config (num-cpu 8, quantum 1, 1024 bytes total,
// 256-byte frames, 1024 bytes per process => 4 frames), 1 s of generation then
// 30 s of draining:
//     pins outliving the access:  pagein_ok=2,119   pagein_fail=121,552
//                                 98.3% of page-ins refused, 1,618 instructions
//                                 retired across 433 processes
//     pins bounded by the access: pagein_ok=60,120  pagein_fail=1
//                                 51,548 instructions retired
// Same code either way apart from where the pins are dropped.

namespace {

prosched::Statement PinWrite(const std::string &addr, const std::string &val) {
  prosched::Statement s;
  s.keyword = prosched::Keyword::kWrite;
  s.args = {addr, val};
  return s;
}

// The page-fault handler Scheduler::attachPaging installs, reproduced so these
// tests exercise the real pin/fault contract rather than an approximation:
// a resident page is pinned and the access proceeds; a fault pins whatever it
// managed to bring in and abandons the instruction either way.
void InstallSchedulerFaultHandler(prosched::PagingManager &pm,
                                  prosched::Interpreter &interp, int pid) {
  interp.SetPageFaultHandler([&pm, pid](int pageNum) {
    if (pm.IsPageResident(pid, pageNum)) {
      pm.PinPage(pid, pageNum);
      return false;
    }
    if (pm.PageIn(pid, pageNum)) {
      pm.PinPage(pid, pageNum);
    }
    return true;
  });
}

// A process wired up the way the scheduler wires one: bounds, page size, pager,
// fault handler and interpreter registration.
void AttachPaging(prosched::PagingManager &pm, prosched::Process &p,
                  int pageSize, std::size_t memBytes) {
  p.SetMemoryBounds(0, memBytes);
  p.SetPagingManager(&pm);
  p.GetInterpreter().SetPageSize(static_cast<uint32_t>(pageSize));
  pm.RegisterProcessInterpreter(p.GetPID(), &p.GetInterpreter());
  InstallSchedulerFaultHandler(pm, p.GetInterpreter(), p.GetPID());
}

AlgoContext MakePagingCtx(int num_cpu, int quantum) {
  ConfigStruct *cs = makeDefault();
  cs->scheduler = "rr";
  cs->num_cpu = num_cpu;
  cs->rr_quantum_cycles = quantum;
  cs->delay_per_exec = 0;
  cs->batch_process_freq = 1000000; // no auto-generation
  AlgoContext ctx = AlgoContext::buildConfig(cs);
  delete cs;
  return ctx;
}

} // namespace

namespace PinLifecycle {

// The abandoned-instruction case. A WRITE reaches for its symbol-table page
// first, pins it, then faults on the data page and gives up. The instruction
// will start over from the beginning, so nothing is mid-access: the frame it
// pinned on the way is not being read or written by anybody and must be
// available to the rest of the machine.
TEST(PinLifecycle, InstructionAbandonedOnAFaultReleasesItsFrames) {
  prosched::PagingManager pm(64, 64); // exactly ONE frame
  prosched::Process p("faulter", 1, 0);
  AttachPaging(pm, p, /*pageSize=*/64, /*memBytes=*/512);

  prosched::Statement write = PinWrite("0x100", "7"); // page 4, not page 0
  p.AddInstruction(write);

  p.ExecuteInstructions(0);
  ASSERT_TRUE(p.GetLastInstructionWasPageFault())
      << "precondition: one frame cannot satisfy a WRITE that needs its data "
         "page and its symbol-table page, so the instruction must abandon";
  ASSERT_EQ(p.GetCurrentInstructionIndex(), 0)
      << "precondition: an abandoned instruction does not advance";

  // Any other process must still be able to claim the machine's only frame.
  prosched::Interpreter other;
  other.SetMemoryBounds(0, 512);
  pm.RegisterProcessInterpreter(2, &other);
  EXPECT_TRUE(pm.PageIn(2, 0))
      << "the only frame is still pinned by an instruction that was abandoned "
         "and will restart from the beginning. Nothing is reading or writing "
         "that page, but PageIn can find no unpinned victim, so every fault in "
         "the system now fails.";
}

// The preemption case, and the one that takes the machine down: quantum-1 round
// robin preempts a process on essentially every tick, and a process carrying a
// pin into the ready queue holds that frame until it is scheduled again and
// happens to finish the instruction.
TEST(PinLifecycle, PreemptedProcessReleasesItsFrames) {
  prosched::PagingManager pm(64, 64); // exactly ONE frame
  AlgoContext ctx = MakePagingCtx(/*num_cpu=*/1, /*quantum=*/1);

  prosched::Process p("pinner", 1, 0);
  AttachPaging(pm, p, /*pageSize=*/64, /*memBytes=*/512);
  prosched::Statement write = PinWrite("0x100", "7");
  p.AddInstruction(write);

  prosched::Worker w(0, ctx);
  ASSERT_NE(w.AssignProcess(&p), nullptr);
  w.RunCycle(); // faults, pins what it could get, abandons the instruction
  ASSERT_TRUE(p.GetLastInstructionWasPageFault())
      << "precondition: the access must have faulted";

  ASSERT_NE(w.PreemptProcess(), nullptr);
  ASSERT_FALSE(w.IsBusy()) << "precondition: the process is off its core";

  prosched::Interpreter other;
  other.SetMemoryBounds(0, 512);
  pm.RegisterProcessInterpreter(2, &other);
  EXPECT_TRUE(pm.PageIn(2, 0))
      << "a process sitting in the ready queue is still pinning a frame. It is "
         "not running, so it cannot be asked to give the frame back, and "
         "PageIn refuses every request that would need it.";
}

// The whole-system consequence, on the shape of the MO2 demo-2 config: more
// runnable processes than frames, quantum 1, every instruction touching memory.
// Forward progress must not stop. MEASURED over this window, same binary apart
// from where pins are dropped: 41 instructions retired with pins outliving
// their access, 2,747 with them bounded by it. The threshold sits between the
// two with room on both sides for a loaded machine.
TEST(PinLifecycle, ProcessesOutnumberingFramesStillMakeProgress) {
  // The queue has to be deeper than the core count for this to bite: a process
  // that carries a pin into the ready queue blocks the machine for as long as
  // it waits there, and with only a handful of processes it is rescheduled
  // again immediately. The demo config queues thousands.
  const int kFrames = 4;
  const int kCores = 8;
  const int kProcesses = 600;
  prosched::PagingManager pm(256, 256 * kFrames);
  ASSERT_EQ(pm.GetTotalFrameCount(), kFrames) << "precondition: 4 frames";

  prosched::Scheduler sched(MakePagingCtx(/*num_cpu=*/kCores, /*quantum=*/1),
                            &pm);

  // READ touches two pages - the data page for the address and the
  // symbol-table page for the variable it assigns - which is what makes a pin
  // outlive its access: the first page is pinned, the second faults, and the
  // instruction restarts holding the first. The addresses stay off page 0 so
  // the two are never the same page.
  std::vector<prosched::Statement> program;
  for (int i = 0; i < 200; ++i) {
    prosched::Statement read;
    read.keyword = prosched::Keyword::kRead;
    read.args = {"v", "0x" + std::to_string((i % 3) + 1) + "00"};
    program.push_back(read);
  }

  std::vector<prosched::Process *> procs;
  for (int i = 0; i < kProcesses; ++i) {
    prosched::Process *p = sched.CreateProcessWithInstructions(
        "pin" + std::to_string(i), 1024, program);
    ASSERT_NE(p, nullptr);
    procs.push_back(p);
    ASSERT_NE(sched.AddProcess(p), nullptr);
  }

  ASSERT_TRUE(sched.Start());
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));
  int retired = 0;
  for (prosched::Process *p : procs) {
    retired += p->GetCurrentInstructionIndex();
  }
  sched.Stop();

  EXPECT_GE(retired, 1000)
      << kProcesses << " processes sharing " << kFrames << " frames retired "
      << retired
      << " instructions in 1.5 s. Every frame is pinned by a process that is "
         "waiting in the ready queue rather than using it, so PageIn cannot "
         "find an unpinned victim and no fault in the system is ever "
         "satisfied - the machine burns ticks without executing anything.";
}

} // namespace PinLifecycle
