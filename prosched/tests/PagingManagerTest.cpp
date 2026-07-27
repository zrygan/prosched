#include "memory/PagingManager.h"
#include "commands/Interpreter.h"
#include "scheduler/process/Process.h"
#include <functional>
#include <chrono>
#include <cstdio>
#include <gtest/gtest.h>
#include <atomic>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <iostream>
#include <thread>

static prosched::Statement pmWrite(const std::string &addr,
                                   const std::string &val) {
  prosched::Statement s;
  s.keyword = prosched::Keyword::kWrite;
  s.args = {addr, val};
  return s;
}
static prosched::Statement pmRead(const std::string &var,
                                  const std::string &addr) {
  prosched::Statement s;
  s.keyword = prosched::Keyword::kRead;
  s.args = {var, addr};
  return s;
}

// MO2 config: memory sizes are powers of 2; total frames = max-overall-mem /
// mem-per-frame. Invalid memory config should be rejected, not crash.
namespace PagingManagerConstruction {

// MO2: total frames = max-overall-mem / mem-per-frame (16384 / 16 = 1024)
TEST(PagingManagerConstruction, ValidConfigProducesExpectedFrameCount) {
  prosched::PagingManager pm(16, 16384);
  EXPECT_EQ(pm.GetTotalFrameCount(), 1024);
}

// MO2: an invalid memory config must be reported as an error, not silently
// succeed. mem-per-frame <= 0 currently calls std::exit(0) (a SUCCESS code) with
// no message; per the project's config-validation convention it should exit
// non-zero with an error mentioning the offending parameter. (Death test: the
// child process must fail, not exit cleanly with 0.)
TEST(PagingManagerConstruction, ZeroMemPerFrameExitsWithErrorNotSilently) {
  EXPECT_EXIT(
      { prosched::PagingManager pm(0, 16384); (void)pm; },
      ::testing::ExitedWithCode(1), "mem-per-frame");
}

// A negative mem-per-frame is likewise invalid and must be reported as an error,
// not accepted or silently exited with success.
TEST(PagingManagerConstruction, NegativeMemPerFrameExitsWithErrorNotSilently) {
  EXPECT_EXIT(
      { prosched::PagingManager pm(-16, 16384); (void)pm; },
      ::testing::ExitedWithCode(1), "mem-per-frame");
}

// MO2: max-overall-mem smaller than mem-per-frame is an invalid config that must
// not silently yield zero usable frames
TEST(PagingManagerConstruction, OverallMemLessThanFrameSizeShouldHaveAtLeastOneFrame) {
  prosched::PagingManager pm(64, 32);
  EXPECT_GT(pm.GetTotalFrameCount(), 0);
}

} // namespace PagingManagerConstruction

// ─── PagingManagerBasic ───────────────────────────────────────────────────
// MO2 demand paging: pages load into frames on demand; when frames are full a
// replacement algorithm evicts a victim to the backing store.
namespace PagingManagerBasic {

// MO2: a referenced page is brought into a free frame (demand paging)
TEST(PagingManagerBasic, PageInLoadsPageIntoFrame) {
  prosched::PagingManager pm(16, 64);
  EXPECT_TRUE(pm.PageIn(1, 0));
  EXPECT_TRUE(pm.IsPageResident(1, 0));
  EXPECT_GE(pm.GetFrame(1, 0), 0);
}

// Paging in the same page twice is idempotent — only one resident page results
TEST(PagingManagerBasic, PageInSamePageTwiceIsIdempotent) {
  prosched::PagingManager pm(16, 64);
  EXPECT_TRUE(pm.PageIn(1, 0));
  EXPECT_TRUE(pm.PageIn(1, 0));
  EXPECT_EQ(pm.GetResidentPageCount(1), 1);
}

// Resident page count and process byte usage track the pages a process holds
TEST(PagingManagerBasic, ResidentPageCountAndProcessBytesTrackPages) {
  prosched::PagingManager pm(16, 64);
  pm.PageIn(1, 0);
  pm.PageIn(1, 1);
  EXPECT_EQ(pm.GetResidentPageCount(1), 2);
  EXPECT_EQ(pm.GetProcessMemoryBytes(1), 32u);
}

// Memory stats reflect a single allocation against the total frame pool
TEST(PagingManagerBasic, MemoryStatsReflectSingleAllocation) {
  prosched::PagingManager pm(16, 64);
  pm.PageIn(1, 0);
  auto stats = pm.GetMemoryStats();
  EXPECT_EQ(stats.totalFrames, 4);
  EXPECT_EQ(stats.usedFrames, 1);
  EXPECT_EQ(stats.freeFrames, 3);
  EXPECT_EQ(stats.usedMemoryBytes, 16u);
  EXPECT_EQ(stats.pagesPagedIn, 1u);
}

// Freeing a process releases its frames and clears its page table
TEST(PagingManagerBasic, FreeAllPagesReleasesFramesAndPages) {
  prosched::PagingManager pm(16, 64);
  pm.PageIn(1, 0);
  pm.PageIn(1, 1);
  pm.FreeAllPagesForProcess(1);
  EXPECT_EQ(pm.GetResidentPageCount(1), 0);
  EXPECT_FALSE(pm.IsPageResident(1, 0));
  EXPECT_EQ(pm.GetMemoryStats().usedFrames, 0);
}

// MO2: with no free frames, a page replacement evicts a victim to the backing
// store so the new page can be loaded
TEST(PagingManagerBasic, FifoEvictionReusesFrameForNewPage) {
  prosched::PagingManager pm(16, 16);
  EXPECT_TRUE(pm.PageIn(1, 0));
  EXPECT_TRUE(pm.PageIn(1, 1));
  EXPECT_TRUE(pm.IsPageResident(1, 1));
  EXPECT_FALSE(pm.IsPageResident(1, 0));
  EXPECT_EQ(pm.GetMemoryStats().pagesPagedOut, 1u);
}

// A frame snapshot reports which process and page own each resident frame
TEST(PagingManagerBasic, FrameSnapshotShowsResidentOwnership) {
  prosched::PagingManager pm(16, 64);
  pm.PageIn(1, 0);
  bool found = false;
  for (const auto &frame : pm.GetFrameSnapshot()) {
    if (frame.allocated) {
      EXPECT_EQ(frame.pid, 1);
      EXPECT_EQ(frame.pageNumber, 0);
      found = true;
    }
  }
  EXPECT_TRUE(found);
}

} // namespace PagingManagerBasic

// ─── Backing-store round-trip (MO2 demand paging) ───────────────────────────
// MO2: an evicted page is written to the backing store and restored on the next
// page-in. A value written before eviction must survive being paged out and back.
namespace PagingManagerBackingStore {

// Installs a demand pager for pid 1 backed by `pm`.
static void wirePager(prosched::Interpreter &interp, prosched::PagingManager &pm,
                      uint32_t pageSize) {
  interp.SetMemoryBounds(0, 256);
  pm.RegisterProcessInterpreter(1, &interp);
  interp.SetPageSize(pageSize);
  interp.SetPageFaultHandler([&pm](int pageNum) {
    if (pm.IsPageResident(1, pageNum))
      return false;
    return pm.PageIn(1, pageNum);
  });
}

// Verifies the round trip through the backing store ONLY: write, evict,
// fault back in, confirm the bytes returned.
//
// Deliberately checks the restored page via GetPageSnapshot rather than a
// second ExecuteRead. READ also touches the symbol-table segment, which with a
// single frame drags in the eviction fight covered by the next test — routing
// the check through the interpreter's own view keeps this test about
// serialization and nothing else.
TEST(PagingManagerBackingStore, EvictedValueSurvivesRoundTrip) {
  prosched::PagingManager pm(16, 16); // exactly ONE frame
  prosched::Interpreter interp;
  wirePager(interp, pm, 16);

  // Store 123 at address 0x32 (page 3). Two attempts: fault-in, then write.
  interp.ExecuteWrite(pmWrite("0x32", "123"));
  interp.ExecuteWrite(pmWrite("0x32", "123"));

  // Force page 3 out of its only frame -> serialized to the backing store.
  pm.PageIn(1, 0);
  ASSERT_FALSE(pm.IsPageResident(1, 3));

  // Touching 0x32 faults page 3 back in and restores it from the backing store.
  interp.ExecuteRead(pmRead("x", "0x32"));
  ASSERT_TRUE(pm.IsPageResident(1, 3));

  // Page 3 spans addresses 48..63; 0x32 == 50 must have come back as 123.
  bool found = false;
  for (const auto &entry : interp.GetPageSnapshot(48, 16)) {
    if (entry.first == 0x32) {
      found = true;
      EXPECT_EQ(entry.second, 123) << "value corrupted by the round trip";
    }
  }
  EXPECT_TRUE(found) << "address 0x32 did not come back from the backing store";
}

// One instruction can need TWO pages resident at the same time. Since commit
// b0cf116 the symbol table is paged: SetVariable calls CheckSymbolTableAccess
// (Interpreter.cpp:572-583), which faults the page holding symbol_table_start
// (== 0, so page 0). A READ therefore touches its DATA page for the value and
// then page 0 to store the variable.
//
// Nothing pins a page that the in-flight instruction already faulted in, so
// when frames < pages-per-instruction the two evict each other forever:
//
//   attempt 1: data page faults in    -> evicts page 0
//   attempt 2: data page hit, reads value, then symbol page faults in
//              -> evicts the data page, instruction abandoned for retry
//   attempt 3: data page faults in    -> evicts page 0 ... repeats forever
//
// FAILING: the READ never completes. This is the regression behind the
// original EvictedValueSurvivesRoundTrip failure — the backing store itself is
// fine, so do not go looking there.
//
// Demo-relevant: max-overall-mem 1024 / mem-per-frame 256 gives 4 frames shared
// by 8 cores, so a process can lose both pages between attempts the same way.
// Fixes: pin the pages an instruction has faulted in until it retires, or fault
// in everything the instruction needs before executing it.
TEST(PagingManagerBackingStore, TwoPageInstructionCompletesWithOneFrame) {
  prosched::PagingManager pm(16, 16); // exactly ONE frame
  prosched::Interpreter interp;
  wirePager(interp, pm, 16);

  interp.ExecuteWrite(pmWrite("0x32", "123"));
  interp.ExecuteWrite(pmWrite("0x32", "123"));

  // Retry the way Process::ExecuteInstructions does: a page fault does not
  // advance the instruction, so the same READ is attempted again next tick.
  std::optional<std::pair<std::string, uint16_t>> result;
  int attempts = 0;
  for (; attempts < 20 && !result.has_value(); ++attempts) {
    result = interp.ExecuteRead(pmRead("x", "0x32"));
  }

  ASSERT_TRUE(result.has_value())
      << "READ never completed in " << attempts
      << " attempts: its data page and the symbol-table page keep evicting "
         "each other, so the instruction can never retire";
  EXPECT_EQ(result->second, 123);
}

} // namespace PagingManagerBackingStore

// ─── Backing-store write cost (performance, not correctness) ────────────────
// PagingManager::PersistBackingStoreToFile (PagingManager.h:440-450) opens the
// file with ios::trunc and rewrites EVERY entry, and it is called on every
// eviction (WritePageToBackingStore) AND on every restore
// (ReadPageFromBackingStore) — two full rewrites per page fault, all while
// pagingMutex is held and the scheduler is blocked at the tick barrier.
//
// TWO separate costs are in play, and the measurements say the SECOND one is
// what actually hurts:
//
// (a) O(N) rewrite — the store grows by one entry per evicted-and-not-restored
//     page, so each write copies everything already there. Measured with -O2 on
//     a native Linux filesystem (/tmp), 1 frame, N sequential PageIns:
//         n=250   251 us/eviction     n=1000  315 us/eviction
//         n=500   217 us/eviction     n=2000  393 us/eviction
//     Real, but gentle — roughly 1.8x per-item cost for 4x the data.
//
// (b) Fixed per-write I/O — and this dominates by a wide margin. The same
//     measurement run from THIS repo's working directory:
//         n=500  8662 us/eviction     n=2000 10194 us/eviction
//     ~40x slower, and the growth ratio collapses to 1.18 because the constant
//     term swamps everything. The repo lives on /mnt/e, a Windows drive mounted
//     through WSL, where every open/truncate/flush crosses the 9p boundary.
//     At ~9 ms per eviction and two rewrites per fault, ~8 faults per tick puts
//     a tick at well over 100 ms — a handful of ticks per second.
//
// So the headline is not "the algorithm is quadratic", it is "a page fault does
// synchronous whole-file disk I/O twice, while holding pagingMutex with the
// scheduler blocked at the tick barrier". On the demo machine that is seconds
// of stall per hundred faults, which is almost certainly why the app looks
// frozen during demo 2's 30-second wait.
//
// Fixes, cheapest first: stop persisting on every eviction (keep the map in
// memory, flush on demand — MO2 only requires the file be readable "any time");
// or append-only; or one file per page.
//
// WHY DISABLED: this is a stopwatch. Wall-clock assertions flake, and the
// numbers swing 40x with the filesystem the repo sits on. It lives here so the
// measurement is repeatable and reviewed, not so CI can fail on it. Run it
// deliberately:
//
//     ./build/prosched/prosched_tests \
//         --gtest_also_run_disabled_tests \
//         --gtest_filter='*BackingStorePerf*'

namespace PagingManagerBackingStorePerf {

// Returns microseconds per eviction for `pageCount` sequential page-ins
// through a single frame — every one of them evicts its predecessor.
static double MicrosPerEviction(int pageCount) {
  std::remove("csopesy-backing-store.txt");
  prosched::PagingManager pm(16, 16); // exactly ONE frame
  prosched::Interpreter interp;
  interp.SetMemoryBounds(0, 1u << 20);
  interp.SetPageSize(16);
  pm.RegisterProcessInterpreter(1, &interp);

  const auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < pageCount; ++i) {
    pm.PageIn(1, i);
  }
  const auto end = std::chrono::steady_clock::now();

  const double ms =
      std::chrono::duration<double, std::milli>(end - start).count();
  return ms / pageCount * 1000.0;
}

TEST(PagingManagerBackingStorePerf, DISABLED_PageFaultDoesNotCostDiskIo) {
  MicrosPerEviction(50); // warm the filesystem cache

  const double small = MicrosPerEviction(150);
  const double large = MicrosPerEviction(600);
  std::remove("csopesy-backing-store.txt");

  std::cout << "  n=150  " << small << " us/eviction\n"
            << "  n=600  " << large << " us/eviction\n"
            << "  growth " << (large / small) << "x\n";

  // The number that decides whether the demo looks responsive. A page fault is
  // supposed to be a memory operation; anything in the millisecond range means
  // it is really a synchronous whole-file disk write. Scale it up: ~8 faults
  // per tick across 8 cores, two rewrites each.
  //
  // Filesystem-dependent by nature — ~200 us on a native Linux disk, ~9000 us
  // from a WSL-mounted Windows drive. The point is to put the real number in
  // front of whoever runs it, not to defend a universal constant.
  EXPECT_LT(large, 1000.0)
      << "each eviction costs " << large
      << " us of synchronous disk I/O (two full rewrites of the backing store "
         "per fault, holding pagingMutex while the scheduler waits at the tick "
         "barrier)";

  // Secondary: the O(N) rewrite. Near 1.0 if cost is independent of store size.
  EXPECT_LT(large / small, 1.5)
      << "per-eviction cost grew when the store got 4x bigger, so every "
         "eviction is paying for the entries already in the file";
}

} // namespace PagingManagerBackingStorePerf

// ─── Cross-process eviction race ─────────────────────────────────────────────
//
// INVARIANT: a process's address space must only be mutated under
// synchronisation that its own executing thread also respects.
//
// PagingManager::PageIn runs on the FAULTING process's worker thread. When no
// frame is free it picks a VICTIM off loadOrderQueue — any (pid, page), owned
// by a different process — and calls WritePageToBackingStore(victimPid, ...),
// which reaches into the VICTIM's interpreter (PagingManager.h:247/260):
//
//     interpreterIt->second->GetPageSnapshot(pageBase, memPerFrame);
//     interpreterIt->second->ClearPageRange(pageBase, memPerFrame);
//
// Those iterate and erase from the victim's `address_space_`
// (std::unordered_map, Interpreter.h:437). Interpreter has no mutex, and the
// victim's own worker thread is concurrently running ExecuteWrite, which does
// `address_space_[address] = value` — an insert that can rehash.
//
// pagingMutex does NOT cover this: the victim's worker holds no paging lock
// while executing ordinary instructions. Concurrent iterate/erase/insert on
// one unordered_map is a data race and undefined behaviour.
//
// Reachable on the MO2 demo config (max-overall-mem 1024 / mem-per-frame 256 =
// 4 frames, 8 cores): with fewer frames than running processes, essentially
// every page fault evicts a page belonging to a process running right now.
//
// DISABLED_ because a data race is not deterministic — this is a sanitizer
// probe, not a CI gate. Run it under ThreadSanitizer:
//   cmake -S . -B build-tsan -DCMAKE_CXX_FLAGS="-fsanitize=thread -g"
//   cmake --build build-tsan --target prosched_tests
//   ./build-tsan/prosched/prosched_tests --gtest_also_run_disabled_tests \
//       --gtest_filter='*EvictionRace*'
namespace PagingManagerEvictionRace {

TEST(PagingManagerEvictionRace,
     DISABLED_EvictingAPageDoesNotRaceWithItsOwnersExecution) {
  // One frame, so every PageIn by the faulting process evicts the other's page.
  prosched::PagingManager pm(256, 256);

  prosched::Interpreter victim;
  victim.SetMemoryBounds(0, 1024);
  pm.RegisterProcessInterpreter(1, &victim);

  prosched::Interpreter faulter;
  faulter.SetMemoryBounds(0, 1024);
  pm.RegisterProcessInterpreter(2, &faulter);

  // The victim owns page 0 and is resident, so it is first in loadOrderQueue.
  ASSERT_TRUE(pm.PageIn(1, 0));

  std::atomic<bool> stop{false};

  // The victim's worker thread: ordinary execution, writing into page 0 of its
  // own address space. Takes no paging lock, exactly like Worker::TickExecution.
  std::thread victimThread([&] {
    for (int i = 0; !stop.load(std::memory_order_relaxed) && i < 200000; ++i) {
      victim.ExecuteWrite(pmWrite("0x" + std::to_string(i % 128), "7"));
    }
  });

  // The faulting process's worker thread: forces eviction of the victim's page
  // over and over, which snapshots and clears the victim's address space.
  std::thread faulterThread([&] {
    for (int i = 0; i < 20000; ++i) {
      pm.PageIn(2, 0); // evicts pid 1 page 0
      pm.PageIn(1, 0); // victim faults it back in, evicting pid 2
    }
    stop.store(true, std::memory_order_relaxed);
  });

  faulterThread.join();
  victimThread.join();

  SUCCEED() << "no crash observed; run under ThreadSanitizer for the verdict";
}

} // namespace PagingManagerEvictionRace

// ─── Demand-paging state machine, driven as a property test ──────────────────
//
// The existing PagingManagerBackingStore tests each move ONE page and check one
// value. These drive the state machine the way the scheduler does — through the
// page-fault handler installed by Scheduler::attachPaging — under real thrash
// (far more pages than frames), then check invariants over the whole state.
namespace PagingManagerStateMachine {

// Byte-for-byte the handler Scheduler::attachPaging installs (Scheduler.h:314).
static std::function<bool(int)> productionHandler(prosched::PagingManager *pm,
                                                  int pid) {
  return [pm, pid](int pageNum) {
    if (pm->IsPageResident(pid, pageNum)) {
      return false;
    }
    return pm->PageIn(pid, pageNum);
  };
}

// The production retry model: "page fault handling continuously occurs until a
// valid page has been returned, before an instruction is performed" (MO2).
// Returns false if the instruction never retired.
static bool RunToRetire(prosched::Interpreter &in, const prosched::Statement &s,
                        bool isWrite, int max_attempts = 50) {
  for (int i = 0; i < max_attempts; ++i) {
    if (isWrite) {
      in.ExecuteWrite(s);
    } else {
      in.ExecuteRead(s);
    }
    if (!in.GetLastInstructionPageFault()) {
      return true;
    }
  }
  return false;
}

static std::string Hex(uint32_t v) {
  std::ostringstream o;
  o << "0x" << std::hex << v;
  return o.str();
}

// P4-I1: with 2 frames and 8 pages, every write is evicted several times over.
// Every value must come back exactly.
TEST(PagingManagerStateMachine, EveryValueSurvivesRepeatedThrashing) {
  constexpr int kFrameSize = 64;
  constexpr int kFrames = 2;
  constexpr uint32_t kMemSize = 512; // 8 pages over 2 frames
  prosched::PagingManager pm(kFrameSize, kFrameSize * kFrames);

  prosched::Interpreter in;
  in.SetMemoryBounds(0, kMemSize);
  in.SetPageSize(kFrameSize);
  pm.RegisterProcessInterpreter(1, &in);
  in.SetPageFaultHandler(productionHandler(&pm, 1));

  // One even address per page, plus a second address in the same page, so a
  // page carries more than a single entry.
  std::map<uint32_t, uint16_t> expected;
  for (uint32_t page = 0; page < kMemSize / kFrameSize; ++page) {
    for (uint32_t off : {uint32_t(0), uint32_t(kFrameSize - 2)}) {
      const uint32_t addr = page * kFrameSize + off;
      const uint16_t val = static_cast<uint16_t>(1000 + addr);
      expected[addr] = val;
      ASSERT_TRUE(RunToRetire(in, pmWrite(Hex(addr), std::to_string(val)), true))
          << "WRITE to " << Hex(addr) << " never retired";
    }
  }

  // Guard against a vacuous pass: if nothing was ever evicted, this test would
  // prove nothing about the backing store at all.
  ASSERT_GT(pm.GetMemoryStats().pagesPagedOut, 0u)
      << "no eviction happened, so this test never exercised the backing store";

  // Read everything back, in an order that keeps forcing eviction.
  for (auto it = expected.rbegin(); it != expected.rend(); ++it) {
    const std::string var = "v" + std::to_string(it->first);
    ASSERT_TRUE(RunToRetire(in, pmRead(var, Hex(it->first)), false))
        << "READ of " << Hex(it->first) << " never retired";
    auto mem = in.ExecuteDebug();
    ASSERT_NE(mem.find(var), mem.end()) << "READ stored no variable";
    EXPECT_EQ(mem[var], it->second)
        << "value at " << Hex(it->first)
        << " did not survive the evict/restore round trip";
  }
}

// P4-I2: backingStore is keyed by {pid, pageNum}. Two processes both using
// page 0 must not see each other's data.
TEST(PagingManagerStateMachine, TwoProcessesDoNotAliasAtTheSamePageNumber) {
  constexpr int kFrameSize = 64;
  prosched::PagingManager pm(kFrameSize, kFrameSize); // ONE frame: forced thrash

  prosched::Interpreter a, b;
  for (auto *p : {&a, &b}) {
    p->SetMemoryBounds(0, 128);
    p->SetPageSize(kFrameSize);
  }
  pm.RegisterProcessInterpreter(1, &a);
  pm.RegisterProcessInterpreter(2, &b);
  a.SetPageFaultHandler(productionHandler(&pm, 1));
  b.SetPageFaultHandler(productionHandler(&pm, 2));

  ASSERT_TRUE(RunToRetire(a, pmWrite("0x10", "1111"), true));
  ASSERT_TRUE(RunToRetire(b, pmWrite("0x10", "2222"), true)); // evicts a's page
  ASSERT_TRUE(RunToRetire(a, pmRead("va", "0x10"), false));   // evicts b's page
  ASSERT_TRUE(RunToRetire(b, pmRead("vb", "0x10"), false));

  auto ma = a.ExecuteDebug();
  auto mb = b.ExecuteDebug();
  ASSERT_NE(ma.find("va"), ma.end());
  ASSERT_NE(mb.find("vb"), mb.end());
  EXPECT_EQ(ma["va"], 1111) << "process 1 read process 2's data at page 0";
  EXPECT_EQ(mb["vb"], 2222) << "process 2 read process 1's data at page 0";
}

// P4-I3: every allocated frame is owned by exactly one resident page, and the
// used-frame count agrees with the per-process resident page counts.
TEST(PagingManagerStateMachine, FrameOwnershipStaysConsistentUnderThrash) {
  constexpr int kFrameSize = 64;
  constexpr int kFrames = 3;
  prosched::PagingManager pm(kFrameSize, kFrameSize * kFrames);

  prosched::Interpreter in[3];
  for (int i = 0; i < 3; ++i) {
    in[i].SetMemoryBounds(0, 512);
    in[i].SetPageSize(kFrameSize);
    pm.RegisterProcessInterpreter(i + 1, &in[i]);
    in[i].SetPageFaultHandler(productionHandler(&pm, i + 1));
  }

  for (int round = 0; round < 6; ++round) {
    for (int i = 0; i < 3; ++i) {
      const uint32_t addr = static_cast<uint32_t>(round * kFrameSize);
      ASSERT_TRUE(RunToRetire(
          in[i], pmWrite(Hex(addr), std::to_string(100 + round)), true));
    }
  }

  auto snap = pm.GetFrameSnapshot();
  auto stats = pm.GetMemoryStats();

  // Guard against a vacuous pass: 3 processes x 6 pages over 3 frames must
  // have forced evictions, otherwise frame reuse was never exercised.
  ASSERT_GT(stats.pagesPagedOut, 0u)
      << "no eviction happened, so frame reuse was never exercised";

  int allocated = 0, owned = 0;
  std::set<std::pair<int, int>> seen;
  for (const auto &f : snap) {
    if (!f.allocated) {
      EXPECT_EQ(f.pid, -1) << "free frame " << f.frameNumber << " has an owner";
      continue;
    }
    ++allocated;
    if (f.pid != -1) {
      ++owned;
      EXPECT_TRUE(seen.insert({f.pid, f.pageNumber}).second)
          << "page {" << f.pid << "," << f.pageNumber
          << "} is resident in more than one frame";
    }
  }
  EXPECT_EQ(allocated, owned)
      << allocated - owned
      << " frame(s) are marked allocated but no resident page claims them "
         "(leaked frames)";
  EXPECT_EQ(stats.usedFrames, allocated);
  EXPECT_EQ(stats.usedFrames + stats.freeFrames, stats.totalFrames);

  int residentTotal = 0;
  for (int i = 0; i < 3; ++i) {
    residentTotal += pm.GetResidentPageCount(i + 1);
  }
  EXPECT_EQ(residentTotal, allocated)
      << "sum of per-process resident pages disagrees with allocated frames";
}

// P4-I5: freeing a process must drop its frames and its backing-store entries.
TEST(PagingManagerStateMachine, FreeingAProcessReleasesFramesAndBackingStore) {
  constexpr int kFrameSize = 64;
  prosched::PagingManager pm(kFrameSize, kFrameSize * 2);

  prosched::Interpreter in;
  in.SetMemoryBounds(0, 512);
  in.SetPageSize(kFrameSize);
  pm.RegisterProcessInterpreter(7, &in);
  in.SetPageFaultHandler(productionHandler(&pm, 7));

  for (uint32_t page = 0; page < 5; ++page) {
    ASSERT_TRUE(
        RunToRetire(in, pmWrite(Hex(page * kFrameSize), "9"), true));
  }
  ASSERT_GT(pm.GetResidentPageCount(7), 0);

  pm.FreeAllPagesForProcess(7);

  EXPECT_EQ(pm.GetResidentPageCount(7), 0);
  EXPECT_EQ(pm.GetProcessMemoryBytes(7), 0u);
  EXPECT_EQ(pm.GetMemoryStats().usedFrames, 0)
      << "frames still allocated after the owning process was freed";

  // MO2: "The backing store is represented as a text file that can be accessed
  // at any given time." So the file is the observable surface — check it
  // directly rather than through the private membership helper. Every line is
  // "<pid>,<page>,<payload>", so no line may still belong to pid 7.
  std::ifstream store("csopesy-backing-store.txt");
  ASSERT_TRUE(store.is_open())
      << "backing store file missing; MO2 requires it to be readable at any "
         "time (note: it is written to the CWD, so run the suite from the "
         "repo root)";
  std::string line;
  while (std::getline(store, line)) {
    EXPECT_NE(line.rfind("7,", 0), 0u)
        << "backing store still holds an entry for the freed process: " << line;
  }
}

} // namespace PagingManagerStateMachine

// ─── Lifecycle seams: scheduler state changes x paging ───────────────────────
//
// Each component is covered on its own — SchedulerSleepLifecycle has 9 tests
// for WAITING/READY transitions, PagingManagerStateMachine covers eviction and
// restore. Neither covers the HANDOFF: a process's pages can be evicted while
// it is descheduled (sleeping or preempted), because eviction is driven by
// whichever process is currently faulting, not by the page's owner.
//
// MO2: "Memory allocation and page fault handling only occur when the process
// is assigned a CPU worker" — a descheduled process does no paging of its own,
// but it is still a valid eviction VICTIM. Its data must survive that.
//
// These drive Process::ExecuteInstructions directly and use the exact calls
// UpdateSleepingProcessesCycle makes (DecrementSleepCycles / SetState), so the
// scheduler's sleep model is reproduced rather than re-implemented.
namespace PagingLifecycleSeams {

static std::function<bool(int)> pager(prosched::PagingManager *pm, int pid) {
  return [pm, pid](int pageNum) {
    if (pm->IsPageResident(pid, pageNum))
      return false;
    return pm->PageIn(pid, pageNum);
  };
}

static void AddStmt(prosched::Process &p, prosched::Keyword kw,
                    std::vector<std::string> args) {
  prosched::Statement s;
  s.keyword = kw;
  s.args = std::move(args);
  p.AddInstruction(s);
}

// Ticks the process until its instruction index moves past `from`, honouring
// the production retry model (a faulting tick does not advance the index).
static bool AdvancePast(prosched::Process &p, int from, int max_ticks = 40) {
  for (int i = 0; i < max_ticks; ++i) {
    if (p.GetCurrentInstructionIndex() > from || p.IsFinished())
      return true;
    p.ExecuteInstructions(0);
  }
  return p.GetCurrentInstructionIndex() > from;
}

// A value written before SLEEP must still be readable after waking, even
// though the owning page was evicted to the backing store while the process
// sat in WAITING and held no core.
TEST(PagingLifecycleSeams, DataSurvivesEvictionWhileTheOwnerIsAsleep) {
  prosched::PagingManager pm(64, 128); // 2 frames
  prosched::Process p("sleeper", 1, 0);
  p.SetMemoryBounds(0, 512);
  p.GetInterpreter().SetPageSize(64);
  pm.RegisterProcessInterpreter(1, &p.GetInterpreter());
  p.GetInterpreter().SetPageFaultHandler(pager(&pm, 1));

  AddStmt(p, prosched::Keyword::kWrite, {"0x40", "999"}); // page 1
  AddStmt(p, prosched::Keyword::kSleep, {"1"});
  AddStmt(p, prosched::Keyword::kRead, {"v", "0x40"});

  ASSERT_TRUE(AdvancePast(p, 0)) << "the WRITE never retired";
  p.ExecuteInstructions(0); // SLEEP -> WAITING, index advances
  ASSERT_EQ(p.GetState(), prosched::ProcessState::WAITING);

  // Another process takes both frames while the sleeper is descheduled.
  prosched::Interpreter other;
  other.SetMemoryBounds(0, 512);
  other.SetPageSize(64);
  pm.RegisterProcessInterpreter(2, &other);
  ASSERT_TRUE(pm.PageIn(2, 0));
  ASSERT_TRUE(pm.PageIn(2, 1));
  ASSERT_EQ(pm.GetResidentPageCount(1), 0)
      << "precondition: the sleeper's pages must have been evicted";

  // What UpdateSleepingProcessesCycle does when the sleep expires.
  p.DecrementSleepCycles();
  ASSERT_LE(p.GetCyclesRemainingForSleep(), 0);
  p.SetState(prosched::ProcessState::READY);

  ASSERT_TRUE(AdvancePast(p, 2)) << "the READ after waking never retired";

  auto mem = p.GetInterpreter().ExecuteDebug();
  ASSERT_NE(mem.find("v"), mem.end()) << "READ stored no variable";
  EXPECT_EQ(mem["v"], 999)
      << "the value written before SLEEP did not survive being evicted while "
         "the process was descheduled";
}

// The instruction index must not move while the process is descheduled, and
// must resume exactly where it left off rather than replaying or skipping.
TEST(PagingLifecycleSeams, SleepDoesNotDisturbTheInstructionIndex) {
  prosched::PagingManager pm(64, 128);
  prosched::Process p("sleeper2", 3, 0);
  p.SetMemoryBounds(0, 512);
  p.GetInterpreter().SetPageSize(64);
  pm.RegisterProcessInterpreter(3, &p.GetInterpreter());
  p.GetInterpreter().SetPageFaultHandler(pager(&pm, 3));

  AddStmt(p, prosched::Keyword::kWrite, {"0x40", "7"});
  AddStmt(p, prosched::Keyword::kSleep, {"2"});
  AddStmt(p, prosched::Keyword::kWrite, {"0x80", "8"});

  ASSERT_TRUE(AdvancePast(p, 0));
  p.ExecuteInstructions(0); // SLEEP
  const int indexAtSleep = p.GetCurrentInstructionIndex();
  ASSERT_EQ(indexAtSleep, 2) << "SLEEP should have advanced past itself";

  // Ticks pass while WAITING; the scheduler never executes a waiting process.
  for (int i = 0; i < 5; ++i)
    p.DecrementSleepCycles();
  EXPECT_EQ(p.GetCurrentInstructionIndex(), indexAtSleep)
      << "the instruction index moved while the process was descheduled";

  p.SetState(prosched::ProcessState::READY);
  ASSERT_TRUE(AdvancePast(p, 2));
  EXPECT_TRUE(p.IsFinished());
  EXPECT_FALSE(p.IsTerminated()) << "process ended TERMINATED, not FINISHED";
}

// A process shut down by an access violation must release its frames, exactly
// like a normally finished one — FreeFinishedProcesses keys on IsFinished(),
// which covers TERMINATED.
TEST(PagingLifecycleSeams, TerminatedProcessReleasesItsFrames) {
  prosched::PagingManager pm(64, 256);
  prosched::Process p("crasher", 4, 0);
  p.SetMemoryBounds(0, 128); // valid: [0, 128)
  p.GetInterpreter().SetPageSize(64);
  pm.RegisterProcessInterpreter(4, &p.GetInterpreter());
  p.GetInterpreter().SetPageFaultHandler(pager(&pm, 4));

  AddStmt(p, prosched::Keyword::kWrite, {"0x40", "5"});   // in bounds
  AddStmt(p, prosched::Keyword::kWrite, {"0x400", "5"});  // out of bounds

  ASSERT_TRUE(AdvancePast(p, 0));
  ASSERT_GT(pm.GetResidentPageCount(4), 0) << "precondition: holds a frame";

  for (int i = 0; i < 10 && !p.IsFinished(); ++i)
    p.ExecuteInstructions(0);

  ASSERT_TRUE(p.IsTerminated())
      << "an out-of-bounds WRITE must shut the process down (MO2)";
  ASSERT_TRUE(p.IsFinished())
      << "IsFinished() must cover TERMINATED, or the scheduler never frees it";

  pm.FreeAllPagesForProcess(p.GetPID()); // what FreeFinishedProcesses does
  EXPECT_EQ(pm.GetResidentPageCount(4), 0);
  EXPECT_EQ(pm.GetMemoryStats().usedFrames, 0)
      << "a terminated process leaked its frames";
}

} // namespace PagingLifecycleSeams
