#include "memory/PagingManager.h"
#include "commands/Interpreter.h"
#include <gtest/gtest.h>

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

TEST(PagingManagerBackingStore, EvictedValueSurvivesRoundTrip) {
  prosched::PagingManager pm(16, 16); // exactly ONE frame
  prosched::Interpreter interp;
  interp.SetMemoryBounds(0, 256);
  pm.RegisterProcessInterpreter(1, &interp);
  interp.SetPageSize(16);
  interp.SetPageFaultHandler([&pm](int pageNum) {
    if (pm.IsPageResident(1, pageNum))
      return false;
    return pm.PageIn(1, pageNum);
  });

  // Store 123 at address 0x32 (page 3). Two attempts: fault-in, then write.
  interp.ExecuteWrite(pmWrite("0x32", "123"));
  interp.ExecuteWrite(pmWrite("0x32", "123"));

  // Force page 3 out of its only frame -> serialized to the backing store.
  pm.PageIn(1, 0);
  ASSERT_FALSE(pm.IsPageResident(1, 3));

  // Reading 0x32 faults it back in from the backing store, then returns 123.
  interp.ExecuteRead(pmRead("x", "0x32"));
  auto result = interp.ExecuteRead(pmRead("x", "0x32"));
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->second, 123);
}

} // namespace PagingManagerBackingStore
