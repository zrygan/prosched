#include "memory/PagingManager.h"
#include "commands/Interpreter.h"
#include <chrono>
#include <cstdio>
#include <gtest/gtest.h>
#include <iostream>

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
