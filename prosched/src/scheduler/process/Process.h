#pragma once

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <new>
#include <optional>
#include <sstream>
#include <stdio.h>
#include <string>
#include <vector>

#include "Config.h"
#include "commands/Interpreter.h"

namespace prosched {

enum ProcessState { READY, RUNNING, WAITING, FINISHED, TERMINATED };

class Process {
private:
  std::string processName;
  int pid;
  int coreNum = -1;
  int currentInstructionIndex = 0;
  int arrivalTick = 0;
  ProcessState currentState = READY;
  std::vector<std::string> logs;
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

  size_t memStart = 0;
  size_t memEnd = 0;

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
    std::tm *tm = std::localtime(&t);
    std::ostringstream oss;
    oss << std::put_time(tm, "(%m/%d/%Y %I:%M:%S%p)");
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
    std::tm *tm = std::localtime(&t);
    std::ostringstream oss;
    oss << std::put_time(tm, "%H:%M:%S");
    return oss.str();
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
  Statement *AddInstruction(Statement &stmt) {
    try {
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
            lastAdded = AddInstruction(nested);
          }
        }
        return lastAdded;
      }

      statements.push_back(stmt);
      return &statements.back();
    } catch (const std::bad_alloc &e) {
      std::cerr << "Allocation failed: " << e.what();
      return nullptr;
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
    currentState = RUNNING;

    if (StartTime.empty())
      StartTime = GetTimestamp();

    // std::cerr << "[DEBUG] " << processName << " idx=" <<
    // currentInstructionIndex << " size=" << statements.size() << "\n";

    if (currentInstructionIndex >= (int)statements.size()) {
      currentState = FINISHED;
      if (finishTime.empty())
        finishTime = GetTimestamp();
      return {};
    }

    const Statement &stmt = statements[currentInstructionIndex];
    if (stmt.keyword == Keyword::kSleep) {
      currentState = WAITING;
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

      logs.push_back(GetTimestamp() + " Core:" + std::to_string(coreNum) +
                     " \"SLEEP " + (stmt.args.empty() ? "0" : stmt.args[0]) +
                     "\"");

      if (currentInstructionIndex >= (int)statements.size() &&
          cyclesRemainingForSleep == 0) {
        currentState = FINISHED;
        if (finishTime.empty())
          finishTime = GetTimestamp();
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

    if (interpreter.GetLastInstructionAccessViolation()) {
      lastViolationTime = GetTimestamp();
      lastViolationClockTime = GetClockTime();
      lastViolationAddress = interpreter.GetLastViolationAddress();
      currentState = TERMINATED;
      if (finishTime.empty()) {
        finishTime = lastViolationTime;
      }
      return statements;
    }

    if (!interpreter.GetLastInstructionPageFault()) {
      currentInstructionIndex++;
    }

    auto output = interpreter.FlushBuffer();
    for (const auto &line : output) {
      logs.push_back(GetTimestamp() + " Core:" + std::to_string(coreNum) +
                     " \"" + line + "\"");
    }

    if (currentInstructionIndex >= (int)statements.size()) {
      currentState = FINISHED;
      if (finishTime.empty())
        finishTime = GetTimestamp();
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
      for (const auto &log : logs) {
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
    return currentState == FINISHED || currentState == TERMINATED;
  }

  /** @brief Checks if the process terminated because of an access violation. */
  bool IsTerminated() const { return currentState == TERMINATED; }

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
  std::vector<std::string> GetLogs() { return logs; }

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
  int GetTotalInstructions() {
    // return (int)statements.size();

    int total = 0;
    for (const auto &stmt : statements) {
      if (stmt.keyword == Keyword::kFor) {
        total += 1;

        int m = 1;
        if (stmt.args.size() >= 2) {
          try {
            m = std::stoi(stmt.args[1]);
          } catch (...) {
            m = 1;
          }
        }

        int n = (int)stmt.nested.size();

        total += (m * n);
      } else {
        total += 1;
      }
    }
    return total;
  }

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
  ProcessState GetState() const { return currentState; }

  /**
   * @brief Sets the current state of the process
   *
   * @param state the ProcessState to set
   */
  void SetState(ProcessState state) { currentState = state; }

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
};

} // namespace prosched