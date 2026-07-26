#pragma once

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <map>
#include <mutex>
#include <queue>
#include <sstream>
#include <utility>
#include <vector>

#include "../scheduler/process/Process.h"

namespace prosched {

class PagingManager {
public:
  struct Frame {
    int frameNumber = -1;
    std::size_t sizeBytes = 0;
    bool allocated = false;
  };

  struct PageTableEntry {
    bool resident = false;
    int frameNumber = -1;
  };

  /**
   * @brief Represents the current state and ownership of a physical frame.
   *
   * A free frame uses -1 for both pid and pageNumber.
   */
  struct FrameSnapshot {
    int frameNumber = -1;
    std::size_t sizeBytes = 0;
    bool allocated = false;
    int pid = -1;
    int pageNumber = -1;
  };

  /**
   * @brief Contains aggregate physical-memory and paging statistics.
   *
   * The paging counters are cumulative for the lifetime of the manager.
   */
  struct MemoryStats {
    std::size_t totalMemoryBytes = 0;
    std::size_t usedMemoryBytes = 0;
    std::size_t freeMemoryBytes = 0;
    int totalFrames = 0;
    int usedFrames = 0;
    int freeFrames = 0;
    std::uint64_t pagesPagedIn = 0;
    std::uint64_t pagesPagedOut = 0;
  };

  /**
   * @brief Constructs a new PagingManager instance.
   *
   * @param memPerFrame The amount of memory per frame.
   * @param maxOverallMem The maximum overall memory size.
   */
  PagingManager(int memPerFrame, int maxOverallMem)
      : memPerFrame(memPerFrame), maxOverallMem(maxOverallMem),
        totalFrames(0) {
    
    if (memPerFrame <= 0) {
      std::cerr << "mem-per-frame must be > 0, got " << memPerFrame << "\n";
      std::exit(1);
      return;
    }

    totalFrames = std::max(1, maxOverallMem/memPerFrame);
    frames.resize(totalFrames);
    for (int i = 0; i < totalFrames; ++i) {
      frames[i].frameNumber = i;
      frames[i].sizeBytes = static_cast<std::size_t>(memPerFrame);
      frames[i].allocated = false;
    }
  }
  /**
   * @brief Checks if a page is currently resident in physical memory.
   *
   * @param pid The process ID.
   * @param pageNum The page number.
   * @return True if the page is resident, false otherwise.
   */
  bool IsPageResident(int pid, int pageNum) {
    std::lock_guard<std::recursive_mutex> lock(pagingMutex);
    auto pidIt = pageTables.find(pid);
    if (pidIt == pageTables.end()) {
      return false;
    }

    auto pageIt = pidIt->second.find(pageNum);
    if (pageIt == pidIt->second.end()) {
      return false;
    }

    return pageIt->second.resident;
  }

  /**
   * @brief Gets the frame number for a given process and page.
   *
   * @param pid The process ID.
   * @param pageNum The page number.
   * @return The frame number if the page is resident, -1 otherwise.
   */
  int GetFrame(int pid, int pageNum) {
    std::lock_guard<std::recursive_mutex> lock(pagingMutex);
    auto pidIt = pageTables.find(pid);
    if (pidIt == pageTables.end()) {
      return -1;
    }

    auto pageIt = pidIt->second.find(pageNum);
    if (pageIt == pidIt->second.end() || !pageIt->second.resident) {
      return -1;
    }

    return pageIt->second.frameNumber;
  }

  /**
   * @brief Pages in a page into physical memory.
   * @note This function uses a simple first-fit strategy.
   *
   * @param pid The process ID.
   * @param pageNum The page number.
   * @return True if the page was successfully paged in, false otherwise.
   */
  bool PageIn(int pid, int pageNum) {
    std::lock_guard<std::recursive_mutex> lock(pagingMutex);
    auto pidIt = pageTables.find(pid);
    if (pidIt != pageTables.end()) {
      auto pageIt = pidIt->second.find(pageNum);
      if (pageIt != pidIt->second.end() && pageIt->second.resident) {
        return true;
      }
    }

    for (auto &frame : frames) {
      if (!frame.allocated) {
        frame.allocated = true;

        auto &processPages = pageTables[pid];
        processPages[pageNum] = PageTableEntry{true, frame.frameNumber};
        if (HasPageInBackingStore(pid, pageNum)) {
          ReadPageFromBackingStore(pid, pageNum);
        }
        loadOrderQueue.push({pid, pageNum});
        pagesPagedIn++;
        return true;
      }
    }

    while (!loadOrderQueue.empty()) {
      auto [victimPid, victimPageNum] = loadOrderQueue.front();
      loadOrderQueue.pop();

      auto victimPidIt = pageTables.find(victimPid);
      if (victimPidIt == pageTables.end()) {
        continue;
      }

      auto victimPageIt = victimPidIt->second.find(victimPageNum);
      if (victimPageIt == victimPidIt->second.end() ||
          !victimPageIt->second.resident) {
        continue;
      }

      WritePageToBackingStore(victimPid, victimPageNum);
      pagesPagedOut++;

      int victimFrame = victimPageIt->second.frameNumber;
      if (victimFrame >= 0 && victimFrame < static_cast<int>(frames.size())) {
        frames[victimFrame].allocated = false;
      }

      victimPageIt->second.resident = false;
      victimPageIt->second.frameNumber = -1;

      for (auto &frame : frames) {
        if (!frame.allocated) {
          frame.allocated = true;

          auto &processPages = pageTables[pid];
          processPages[pageNum] = PageTableEntry{true, frame.frameNumber};
          if (HasPageInBackingStore(pid, pageNum)) {
            ReadPageFromBackingStore(pid, pageNum);
          }
          loadOrderQueue.push({pid, pageNum});
          pagesPagedIn++;
          return true;
        }
      }
    }

    return false;
  }

  /**
   * @brief Frees all frames used by a process and clears its page table
   * entries.
   *
   * @param pid The process ID.
   */
  void FreeAllPagesForProcess(int pid) {
    std::lock_guard<std::recursive_mutex> lock(pagingMutex);
    auto pidIt = pageTables.find(pid);
    if (pidIt == pageTables.end()) {
      return;
    }

    for (auto &entry : pidIt->second) {
      if (entry.second.resident && entry.second.frameNumber >= 0 &&
          entry.second.frameNumber < static_cast<int>(frames.size())) {
        frames[entry.second.frameNumber].allocated = false;
      }
    }

    pidIt->second.clear();
    pageTables.erase(pidIt);

    RemoveAllBackingStoreEntriesForProcess(pid);
  }

  /**
   * @brief Writes a page to backing store during eviction.
   *
   * @param pid The process ID.
   * @param pageNum The page number.
   */
  void WritePageToBackingStore(int pid, int pageNum) {
    std::lock_guard<std::recursive_mutex> lock(pagingMutex);
    auto interpreterIt = processInterpreters.find(pid);
    if (interpreterIt == processInterpreters.end() ||
        interpreterIt->second == nullptr) {
      return;
    }

    uint32_t pageBase = static_cast<uint32_t>(pageNum * memPerFrame);
    std::vector<std::pair<uint32_t, uint16_t>> pageEntries =
        interpreterIt->second->GetPageSnapshot(
            pageBase, static_cast<uint32_t>(memPerFrame));

    std::ostringstream serialized;
    for (size_t i = 0; i < pageEntries.size(); ++i) {
      if (i != 0) {
        serialized << ';';
      }
      serialized << pageEntries[i].first << '=' << pageEntries[i].second;
    }

    backingStore[{pid, pageNum}] = serialized.str();
    PersistBackingStoreToFile();
  }

  /**
   * @brief Reads a page from backing store and restores it to the owning
   * process.
   *
   * @param pid The process ID.
   * @param pageNum The page number.
   * @return True if a page entry was found and restored, false otherwise.
   */
  bool ReadPageFromBackingStore(int pid, int pageNum) {
    std::lock_guard<std::recursive_mutex> lock(pagingMutex);
    auto storeIt = backingStore.find({pid, pageNum});
    if (storeIt == backingStore.end()) {
      return false;
    }

    auto interpreterIt = processInterpreters.find(pid);
    if (interpreterIt == processInterpreters.end() ||
        interpreterIt->second == nullptr) {
      backingStore.erase(storeIt);
      PersistBackingStoreToFile();
      return false;
    }

    uint32_t pageBase = static_cast<uint32_t>(pageNum * memPerFrame);
    std::vector<std::pair<uint32_t, uint16_t>> restoredValues =
        ParseSerializedPage(storeIt->second);

    interpreterIt->second->ClearPageRange(pageBase,
                                          static_cast<uint32_t>(memPerFrame));
    interpreterIt->second->RestorePageSnapshot(restoredValues);

    backingStore.erase(storeIt);
    PersistBackingStoreToFile();
    return true;
  }

  /**
   * @brief Registers the interpreter for a process so paging can persist state.
   *
   * @param pid The process ID.
   * @param interpreter The interpreter owning the process's address space.
   */
  void RegisterProcessInterpreter(int pid, Interpreter *interpreter) {
    std::lock_guard<std::recursive_mutex> lock(pagingMutex);
    processInterpreters[pid] = interpreter;
  }

  /**
   * @brief Gets a point-in-time view of physical frames and their resident
   * pages.
   *
   * @return A vector containing one entry for every physical frame.
   */
  std::vector<FrameSnapshot> GetFrameSnapshot() const {
    std::lock_guard<std::recursive_mutex> lock(pagingMutex);
    std::vector<FrameSnapshot> snapshot;
    snapshot.reserve(frames.size());

    for (const Frame &frame : frames) {
      FrameSnapshot entry{frame.frameNumber, frame.sizeBytes, frame.allocated};
      for (const auto &[pid, pages] : pageTables) {
        for (const auto &[pageNumber, page] : pages) {
          if (page.resident && page.frameNumber == frame.frameNumber) {
            entry.pid = pid;
            entry.pageNumber = pageNumber;
            break;
          }
        }
        if (entry.pid != -1) {
          break;
        }
      }
      snapshot.push_back(entry);
    }

    return snapshot;
  }

  /**
   * @brief Gets aggregate physical-memory usage and cumulative paging activity.
   *
   * @return Current memory usage together with page-in and page-out totals.
   */
  MemoryStats GetMemoryStats() const {
    std::lock_guard<std::recursive_mutex> lock(pagingMutex);
    MemoryStats stats;
    stats.totalMemoryBytes = static_cast<std::size_t>(totalFrames) *
                             static_cast<std::size_t>(memPerFrame);
    stats.totalFrames = totalFrames;
    stats.pagesPagedIn = pagesPagedIn;
    stats.pagesPagedOut = pagesPagedOut;

    for (const Frame &frame : frames) {
      if (frame.allocated) {
        stats.usedFrames++;
        stats.usedMemoryBytes += frame.sizeBytes;
      }
    }
    stats.freeFrames = stats.totalFrames - stats.usedFrames;
    stats.freeMemoryBytes = stats.totalMemoryBytes - stats.usedMemoryBytes;
    return stats;
  }

  /**
   * @brief Gets the number of pages currently resident for a process.
   *
   * @param pid The process ID.
   * @return The number of resident pages owned by the process.
   */
  int GetResidentPageCount(int pid) const {
    std::lock_guard<std::recursive_mutex> lock(pagingMutex);
    auto pidIt = pageTables.find(pid);
    if (pidIt == pageTables.end()) {
      return 0;
    }

    int count = 0;
    for (const auto &[pageNumber, page] : pidIt->second) {
      (void)pageNumber;
      if (page.resident) {
        count++;
      }
    }
    return count;
  }

  /**
   * @brief Gets the physical memory currently occupied by a process.
   *
   * Only resident pages count; pages that have been evicted to the backing
   * store hold no frame and so contribute nothing.
   *
   * @param pid The process ID.
   * @return The number of bytes of physical memory held by the process.
   */
  std::size_t GetProcessMemoryBytes(int pid) const {
    std::lock_guard<std::recursive_mutex> lock(pagingMutex);
    return static_cast<std::size_t>(GetResidentPageCount(pid)) *
           static_cast<std::size_t>(memPerFrame);
  }

  /**
   * @brief Gets the total number of frames.
   *
   * @return The total number of frames.
   */
  int GetTotalFrameCount() const {
    std::lock_guard<std::recursive_mutex> lock(pagingMutex);
    return totalFrames;
  }

private:
  int memPerFrame = 0;
  int maxOverallMem = 0;
  int totalFrames = 0;
  std::vector<Frame> frames;
  std::map<int, std::map<int, PageTableEntry>> pageTables;
  std::queue<std::pair<int, int>> loadOrderQueue;
  std::map<int, Interpreter *> processInterpreters;
  std::map<std::pair<int, int>, std::string> backingStore;
  std::uint64_t pagesPagedIn = 0;
  std::uint64_t pagesPagedOut = 0;
  mutable std::recursive_mutex pagingMutex;
  const std::string backingStoreFile = "csopesy-backing-store.txt";

  /**
   * @brief Checks if a page for a given process is present in the backing
   * store.
   *
   * @param pid The process ID.
   * @param pageNum The page number.
   * @return True if the page is in the backing store, false otherwise.
   */
  bool HasPageInBackingStore(int pid, int pageNum) const {
    return backingStore.find({pid, pageNum}) != backingStore.end();
  }
  /**
   * @brief Persists the current backing store state to a file.
   */
  void PersistBackingStoreToFile() const {
    std::ofstream out(backingStoreFile, std::ios::trunc);
    if (!out.is_open()) {
      return;
    }

    for (const auto &entry : backingStore) {
      out << entry.first.first << ',' << entry.first.second << ','
          << entry.second << '\n';
    }
  }

  /**
   * @brief Parses a serialized page string into address/value pairs.
   *
   * @param payload The serialized page string.
   * @return A vector of address/value pairs.
   * @note The expected format is "address1=value1;address2=value2;..."
   *       where each address and value are unsigned integers.
   */
  std::vector<std::pair<uint32_t, uint16_t>>
  ParseSerializedPage(const std::string &payload) const {
    std::vector<std::pair<uint32_t, uint16_t>> entries;
    if (payload.empty()) {
      return entries;
    }

    std::stringstream ss(payload);
    std::string token;
    while (std::getline(ss, token, ';')) {
      if (token.empty()) {
        continue;
      }

      std::size_t separator = token.find('=');
      if (separator == std::string::npos) {
        continue;
      }

      std::string addrText = token.substr(0, separator);
      std::string valueText = token.substr(separator + 1);
      entries.emplace_back(static_cast<uint32_t>(std::stoul(addrText)),
                           static_cast<uint16_t>(std::stoul(valueText)));
    }

    return entries;
  }

  /**
   * @brief Removes all backing store entries for a given process.
   *
   * @param pid The process ID.
   */
  void RemoveAllBackingStoreEntriesForProcess(int pid) {
    for (auto it = backingStore.begin(); it != backingStore.end();) {
      if (it->first.first == pid) {
        it = backingStore.erase(it);
      } else {
        ++it;
      }
    }

    PersistBackingStoreToFile();
  }
};

} // namespace prosched
