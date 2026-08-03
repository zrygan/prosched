#pragma once

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <sstream>
#include <stdio.h>
#include <string>
#include <vector>

#include "Config.h"
#include "commands/Interpreter.h"
#include "../../memory/PagingManager.h"

namespace prosched {

enum ProcessState { READY, RUNNING, WAITING, FINISHED, TERMINATED };

class Process {
private:
  std::string processName;
  int pid;
  int coreNum = -1;
  int currentInstructionIndex = 0;
  int arrivalTick = 0;
  /**
   * @brief The process's state, written by its worker and read by the CLI.
   *
   * A worker moves the process to RUNNING/WAITING/FINISHED/TERMINATED while
   * the CLI thread reads it through GetState/IsFinished/IsTerminated for
   * "screen -ls" and process-smi, so a plain enum member is a data race.
   *
   * Held by pointer for the same reason as logsMutex: an atomic cannot be
   * copied or moved, and processes are also held by value in vectors.
   */
  std::shared_ptr<std::atomic<ProcessState>> currentState =
      std::make_shared<std::atomic<ProcessState>>(READY);

  std::vector<std::string> logs;

  /**
   * @brief Held for every read of and append to logs.
   *
   * A worker appends to the log while the CLI thread copies it for
   * "screen -r"/process-smi. It lives on the heap so that Process stays
   * copyable, since processes are also held by value in vectors.
   */
  std::shared_ptr<std::mutex> logsMutex = std::make_shared<std::mutex>();

  bool ownedByScheduler = false;
  std::string StartTime;
  std::string lastViolationTime;
  std::string lastViolationClockTime;
  uint32_t lastViolationAddress = 0;

  int cyclesRemainingForSleep = 0;
  int currentInstructionCyclesLeft = 0;
  int quantumUsed = 0;
  std::string finishTime;

  prosched::Interpreter interpreter;
  std::vector<prosched::Statement> statements;

  /**
   * @brief Instructions this process was created with.
   *
   * Counted separately from statements because the statements are released
   * once the process can no longer run, while the count is still reported by
   * "screen -ls" and report-util for the rest of the session.
   */
  int totalInstructions = 0;

  size_t memStart = 0;
  size_t memEnd = 0;

  PagingManager* pagingManager = nullptr;

  /**
   * @brief Generates a formatted time string representing the current system
   * clock.
   *
   * This is a utility function used to timestamp log messages. It does not
   * alter the process's internal start time.
   *
   * @return A string containing the formatted timestamp, e.g. "(06/24/2026
   * 02:15:30PM)"
   */
  std::string GetTimestamp() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm local = LocalTime(t);
    std::ostringstream oss;
    oss << std::put_time(&local, "(%m/%d/%Y %I:%M:%S%p)");
    return oss.str();
  }

  /**
   * @brief Generates a time-of-day string for the current system clock.
   *
   * Used for the memory access violation notice, which reports only the
   * wall-clock time rather than a full timestamp.
   *
   * @return A string of the form "HH:MM:SS" in 24-hour time, e.g. "14:15:30"
   */
  std::string GetClockTime() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm local = LocalTime(t);
    std::ostringstream oss;
    oss << std::put_time(&local, "%H:%M:%S");
    return oss.str();
  }

  /**
   * @brief Converts a time_t to local calendar time without sharing state.
   *
   * std::localtime returns a pointer to a single shared std::tm, so every
   * worker that timestamps a log entry would be overwriting the struct the
   * others are still formatting.
   *
   * @param t The time to convert
   * @return The broken-down local time, owned by the caller
   */
  static std::tm LocalTime(std::time_t t) {
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &t);
#else
    localtime_r(&t, &local);
#endif
    return local;
  }

  /**
   * @brief Appends a log entry, serialised against readers of the log.
   */
  void AppendLog(std::string entry) {
    const std::lock_guard<std::mutex> guard(*logsMutex);
    logs.push_back(std::move(entry));
  }

  /**
   * @brief Releases the statements of a process that can no longer run.
   *
   * Finished and terminated processes are kept for the rest of the session so
   * that "screen -ls" and report-util can still list them, but only their
   * counters are read from then on. Holding on to the statement vector costs
   * roughly 130 KB per process at the demo's max-ins.
   */
  void ReleaseInstructions() {
    statements.clear();
    statements.shrink_to_fit();
  }

public:
  Process(std::string processName, int pid, int arrivalTick)
      : processName(processName), pid(pid), arrivalTick(arrivalTick) {}

  /**
   * @brief Returns the process interpreter for paging and testing hooks.
   */
  Interpreter &GetInterpreter() { return interpreter; }

  /**
   * @brief sets the memory bounds for the interpreter
   */
  void SetMemoryBounds(size_t start, size_t end) {
    memStart = start;
    memEnd = end;
    interpreter.SetMemoryBounds(static_cast<uint32_t>(start),
                                static_cast<uint32_t>(end));
  }

  /**
   * @brief adds a command to the process
   *
   * Appends the specified randomized command to Process' statements vector
   *
   * @param stmt
   * @return If adding a command is successful it returns the
   * specific statement, else unsuccessful return nullptr
   */
  Statement *AddInstruction(Statement &stmt, int cap = INT_MAX) {
    if ((int)statements.size() >= cap){
      return nullptr;
    }

    try {
      if (stmt.keyword == Keyword::kUnknown) {
        return nullptr;
      }

      if (stmt.keyword == Keyword::kFor) {
        int repeats = 1;
        if (stmt.args.size() >= 2) {
          try {
            repeats = std::stoi(stmt.args[1]);
          } catch (...) {
            repeats = 1;
          }
        }

        Statement *lastAdded = nullptr;
        for (int r = 0; r < repeats; r++) {
          for (auto &nested : stmt.nested) {
            if ((int)statements.size() >= cap){
              return nullptr;
            }
            lastAdded = AddInstruction(nested, cap);
          }
        }
        return lastAdded;
      }

      statements.push_back(stmt);
      totalInstructions++;
      return &statements.back();
    } catch (const std::bad_alloc &e) {
      std::cerr << "Allocation failed: " << e.what();
      return nullptr;
    }
  }

  /**
   * @brief Counts the instructions AddInstruction would append for a statement.
   *
   * FOR statements are unrolled, and the unrolling is recursive: a FOR nested
   * inside another FOR expands again on every repeat of the outer loop. A
   * caller that has to respect an instruction budget must ask for this count
   * rather than assume a statement costs one instruction.
   *
   * @param stmt The statement that would be added
   * @return The number of statements it would expand into
   */
  static int CountExpandedInstructions(const Statement &stmt) {
    if (stmt.keyword == Keyword::kUnknown) {
      return 0;
    }

    if (stmt.keyword != Keyword::kFor) {
      return 1;
    }

    int repeats = 1;
    if (stmt.args.size() >= 2) {
      try {
        repeats = std::stoi(stmt.args[1]);
      } catch (...) {
        repeats = 1;
      }
    }
    if (repeats <= 0) {
      return 0;
    }

    int body = 0;
    for (const Statement &nested : stmt.nested) {
      body += CountExpandedInstructions(nested);
    }
    return body * repeats;
  }

  /**
   * @brief Reserves room for an expected instruction count.
   *
   * A generated process is filled one statement at a time up to min-ins/max-ins,
   * and every reallocation on the way copies each Statement already stored,
   * along with the strings each one owns. The generator knows the final count
   * up front, so the growth can be paid once. Purely an allocation hint: it
   * changes no limit and no observable state.
   *
   * @param expectedInstructions The number of statements about to be added
   */
  void ReserveInstructions(int expectedInstructions) {
    if (expectedInstructions > 0) {
      statements.reserve(static_cast<std::size_t>(expectedInstructions));
    }
  }

  /**
   * @brief Drops every pin this process holds on physical frames.
   *
   * A pin exists only for the duration of the access that needs the page: it
   * stops the pager from evicting a page out from under a running instruction.
   * The moment the process is not mid-access - the instruction finished, the
   * instruction was abandoned, or the process left its core - the pins must go,
   * or the frames stay locked to a process that is not using them.
   *
   * Safe to call when the process has no pager or holds no pins.
   */
  void ReleasePins() {
    if (pagingManager != nullptr) {
      pagingManager->UnpinAllPagesForProcess(pid);
    }
  }

  /**
   * @brief Executes the current instruction of the process on a CPU core.
   *
   * Executes one parsed Statement instruction. If it is the first statement,
   * sets the process's StartTime. Handles SLEEP operations by moving the
   * process to WAITING state and detaching from the core.
   *
   * @param coreNum The index of the assigned CPU core executing the process.
   * @return A vector containing all statements of the process.
   */
  std::vector<prosched::Statement> ExecuteInstructions(int coreNum) {
    // A process that is already done keeps its state: its statements are gone,
    // so running it again would only mislabel a terminated process as finished.
    if (IsFinished()) {
      return {};
    }

    currentState->store(RUNNING);

    if (StartTime.empty())
      StartTime = GetTimestamp();

    if (currentInstructionIndex >= (int)statements.size()) {
      currentState->store(FINISHED);
      if (finishTime.empty())
        finishTime = GetTimestamp();
      ReleaseInstructions();
      return {};
    }

    const Statement &stmt = statements[currentInstructionIndex];
    if (stmt.keyword == Keyword::kSleep) {
      currentState->store(WAITING);
      if (!stmt.args.empty()) {
        try {
          cyclesRemainingForSleep = std::stoi(stmt.args[0]);
        } catch (const std::invalid_argument &) {
          cyclesRemainingForSleep = 0;
        } catch (const std::out_of_range &) {
          cyclesRemainingForSleep = 0;
        }
      } else {
        cyclesRemainingForSleep = 0;
      }
      currentInstructionIndex++;

      AppendLog(GetTimestamp() + " Core:" + std::to_string(coreNum) +
                " \"SLEEP " + (stmt.args.empty() ? "0" : stmt.args[0]) + "\"");

      if (currentInstructionIndex >= (int)statements.size() &&
          cyclesRemainingForSleep == 0) {
        currentState->store(FINISHED);
        if (finishTime.empty())
          finishTime = GetTimestamp();
        ReleaseInstructions();
        return {};
      }
      return statements;
    }

    if (stmt.keyword == Keyword::kFor) {
      currentInstructionIndex++;
      return statements;
    }

    interpreter.ResetLastInstructionPageFault();
    interpreter.ResetLastInstructionAccessViolation();
    interpreter.ExecuteStatements({stmt});

    if (interpreter.GetLastInstructionPageFault()) {
      // The instruction is abandoned and will start over, so the pages it did
      // manage to pin are not being read or written by anybody. Holding them
      // pinned across the restart - and across the preemption that follows -
      // takes those frames out of circulation while this process sits in the
      // ready queue, which is how every frame ends up pinned by a process that
      // is not running and PageIn starts refusing every request.
      ReleasePins();
      interpreter.FlushBuffer();
      return statements;
    }

    if (interpreter.GetLastInstructionAccessViolation()) {
      lastViolationTime = GetTimestamp();
      lastViolationClockTime = GetClockTime();
      lastViolationAddress = interpreter.GetLastViolationAddress();
      currentState->store(TERMINATED);
      if (finishTime.empty()) {
        finishTime = lastViolationTime;
      }
      ReleaseInstructions();
      return {};
    }

    if (!interpreter.GetLastInstructionPageFault() &&
      !interpreter.GetLastInstructionAccessViolation()) {
      currentInstructionIndex++;
      ReleasePins();
    }

    auto output = interpreter.FlushBuffer();
    for (const auto &line : output) {
      AppendLog(GetTimestamp() + " Core:" + std::to_string(coreNum) + " \"" +
                line + "\"");
    }

    if (currentInstructionIndex >= (int)statements.size()) {
      currentState->store(FINISHED);
      if (finishTime.empty())
        finishTime = GetTimestamp();
      ReleaseInstructions();
      return {};
    }

    return statements;
  }

  /**
   * @brief Assigns a CPU core to the Process
   *
   * Stores the identifier of the CPU core responsible for executing this
   * process
   *
   * @param coreNum indicates the CPU core to be assigned
   * @return coreNum is returned if the assignment was successful; otherwise -1
   */
  int AssignCore(int coreNum) {
    if (coreNum >= 0) {
      this->coreNum = coreNum;
      return coreNum;
    } else {
      return -1;
    }
  }

  /**
   * @brief saves the logs vector into a txt file
   *
   * ngl idk if this should be here or insoide the ExecutePrint() in
   * intepreter.hpp
   *
   * dw we dont need this function anymore lol
   */
  void SaveLogsToFile() {
    std::filesystem::create_directory("logs");
    std::string filename = "logs/" + processName + ".txt";

    std::ofstream outFile(filename);
    if (outFile.is_open()) {
      outFile << "Process name: " << processName << "\n";
      for (const auto &log : GetLogs()) {
        outFile << log << "\n";
      }

      outFile.close();
    } else {
      std::cerr << "Error: Could not make log file for " << processName << "\n";
    }
  }

  /**
   * @brief Checks if the Process has finished executing all its commands
   *
   * A process is considered finished when all commands have been executed
   *
   * @return true if the process is finished; otherwise false.
   */
  bool IsFinished() {
    const ProcessState state = currentState->load();
    return state == FINISHED || state == TERMINATED;
  }

  /** @brief Checks if the process terminated because of an access violation. */
  bool IsTerminated() const { return currentState->load() == TERMINATED; }

  /**
   * @brief Gets the Process ID
   *
   * @return the Process ID
   */
  int GetPID() { return pid; }

  /**
   * @brief Gets the Process name
   *
   * @return the Process name
   */
  std::string GetName() { return processName; }

  /**
   * @brief Gets the memory allocated to the Process
   *
   * @return the size of the Process' address space in bytes
   */
  size_t GetMemorySize() { return memEnd - memStart; }

  /**
   * @brief Gets the assigned CPU core number
   *
   * @return the assigned CPU core number
   */
  int GetCoreNum() { return coreNum; }

  /**
   * @brief Gets the process logs
   *
   * @return vector of logs
   */
  std::vector<std::string> GetLogs() {
    const std::lock_guard<std::mutex> guard(*logsMutex);
    return logs;
  }

  /**
   * @brief Gets the started time of a Process
   *
   * @return the formatted time date string
   */
  std::string GetProcessTimeStart() { return StartTime; }

  /**
   * @brief Gets the finish time of the process
   *
   * @return the formatted time date string, or empty if not yet finished
   */
  std::string GetProcessTimeFinish() { return finishTime; }
  /** @brief Gets the timestamp of the last access violation, if any. */
  std::string GetLastViolationTime() const { return lastViolationTime; }

  /**
   * @brief Gets the time of day of the last access violation, if any.
   *
   * @return "HH:MM:SS" in 24-hour time, or empty if the process never faulted
   */
  std::string GetLastViolationClockTime() const {
    return lastViolationClockTime;
  }

  /** @brief Gets the offending address of the last access violation, if any. */
  uint32_t GetLastViolationAddress() const { return lastViolationAddress; }
  /**
   * @brief gets the current index of an instruction being executed
   *
   * for the screen -ls progress count
   *
   * @return the current instruction index
   */
  int GetCurrentInstructionIndex() { return currentInstructionIndex; }

  /**
   * @brief get total number of statements/ instrcutions
   *
   * for screen -ls progress count
   *
   * @return total number of instrcutions in a process
   */
  int GetTotalInstructions() { return totalInstructions; }

  /**
   * @brief gets the core assigned
   *
   * @return coreNum assigned
   */
  int GetAssignedCore() { return coreNum; }

  /**
   * @brief Sets whether the process is owned by the scheduler
   *
   * @param owned boolean value indicating ownership
   */
  void SetOwnedByScheduler(bool owned) { ownedByScheduler = owned; }

  /**
   * @brief Checks if the process is owned by the scheduler
   *
   * @return true if owned by the scheduler; otherwise false
   */
  bool IsOwnedByScheduler() const { return ownedByScheduler; }

  /**
   * @brief Gets the current state of the process
   *
   * @return the current ProcessState
   */
  ProcessState GetState() const { return currentState->load(); }

  /**
   * @brief Sets the current state of the process
   *
   * @param state the ProcessState to set
   */
  void SetState(ProcessState state) {
    currentState->store(state);
    if (state == FINISHED || state == TERMINATED) {
      ReleaseInstructions();
    }
  }

  /**
   * @brief Gets the number of cycles remaining for the process to sleep
   *
   * @return the number of cycles remaining for sleep
   */
  int GetCyclesRemainingForSleep() const { return cyclesRemainingForSleep; }

  /**
   * @brief Decrements the number of cycles remaining for the process to sleep
   *
   * This function reduces the cyclesRemainingForSleep by one, ensuring it does
   * not go below zero.
   */
  void DecrementSleepCycles() {
    if (cyclesRemainingForSleep > 0)
      cyclesRemainingForSleep--;
  }

  /**
   * @brief Gets the number of cycles left for the current instruction
   *
   * @return the number of cycles left for the current instruction
   */
  int GetCurrentInstructionCyclesLeft() const {
    return currentInstructionCyclesLeft;
  }

  /**
   * @brief Sets the number of cycles left for the current instruction
   *
   * @param numCycles the number of cycles left for the current instruction
   */
  void SetCurrentInstructionCyclesLeft(int numCycles) {
    currentInstructionCyclesLeft = numCycles;
  }

  /**
   * @brief Decrements the number of cycles left for the current instruction
   *
   * This function reduces the currentInstructionCyclesLeft by one, ensuring it
   * does not go below zero.
   */
  void DecrementInstructionCyclesLeft() {
    if (currentInstructionCyclesLeft > 0)
      --currentInstructionCyclesLeft;
  }

  /**
   * @brief Gets the number of cycles used in the current quantum
   *
   * @return the number of cycles used in the current quantum
   */
  int GetQuantumUsed() const { return quantumUsed; }

  /**
   * @brief Increments the number of cycles used in the current quantum
   */
  void IncrementQuantumUsed() { ++quantumUsed; }

  /**
   * @brief Resets the number of cycles used in the current quantum to zero
   */
  void ResetQuantumUsed() { quantumUsed = 0; }

  /**
   * @brief Checks if the last executed instruction of the process was a page fault
   */
  bool GetLastInstructionWasPageFault() const { return interpreter.GetLastInstructionPageFault(); }

  /**
   * @brief setting the paging manager used by the process
   */
  void SetPagingManager(PagingManager* pm) {
    pagingManager = pm;
  }
};

} // namespace prosched