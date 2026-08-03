#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <ostream>
#include <sstream>
#include <stdio.h>
#include <string>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include "Config.h"
#include "Context.h"
#include "Controller.h"
#include "src/Constants.hpp"
#include "src/scheduler/Scheduler.h"

Controller::~Controller() {
  delete scheduler;
  delete pagingManager;
}

AlgoContext Controller::initialize() {
  ConfigStruct *cs = fromFile();

  if (cs == nullptr) {
    std::cout << "Failed to find config file\n";
    this->ctx = {};
    return this->ctx;
  }

  this->ctx = AlgoContext::buildConfig(cs);

  if (this->ctx.num_cpu <= 0 || this->ctx.num_cpu >= 129) {
    std::cerr << "Invalid config: num-cpu count invalid\n";
    std::exit(1);
  }

  if (this->ctx.schedulerType == SchedulerType::UNKNOWN) {
    std::cerr << "Invalid config: Unknown scheduler type\n";
    std::exit(1);
  }

  if (ctx.schedulerType == SchedulerType::RR && ctx.rr_quantum_cycles <= 0) {
    std::cerr << "Invalid config: Invalid quantum cycles, must be >= 1\n";
    std::exit(1);
  }

  if (this->ctx.batch_process_frequency <= 0) {
    std::cerr << "Invalid config: Invalid batch-process-frequency number\n";
    std::exit(1);
  }

  if (this->ctx.min_ins > this->ctx.max_ins) {
    std::cerr << "Invalid config: min-ins must be lesser than max-ins\n";
    std::exit(1);
  }

  if(this->ctx.min_mem_per_proc > this->ctx.max_mem_per_proc) {
    std::cerr << "Invalid config: min-mem-per-proc must be lesser than max-mem-per-proc\n";
    std::exit(1);
  }

  if (this->ctx.delay_per_execution < 0) {
    std::cerr << "Invalid config: delay per execution cannot be below 0\n";
    std::exit(1);
  }

  if(this->ctx.mem_per_frame <= 0) {
    std::cerr << "Invalid config: mem-per-frame must be > 0, got " << this->ctx.mem_per_frame << "\n";
    std::exit(1);
  }

  this->pagingManager = new prosched::PagingManager(this->ctx.mem_per_frame,
                                                    this->ctx.max_overall_mem);
  this->scheduler = new prosched::Scheduler(this->ctx, this->pagingManager);
  this->isInitialized = true;

  if (!this->scheduler->IsRunning()) {
    this->scheduler->Start();
  }

  this->view.DisplayCommands();

  return this->ctx;
}

void Controller::HandleView() {
  this->view = View();
  view.DisplayMenu();
}

bool Controller::HandlePreInit(const std::string &input) {
  if (input == "initialize") {
    initialize();
    if (!isInitialized) {
      std::cout << "Initialization failed. Check config.txt.\n";
    }
    return true;
  }

  if (input == "exit") {
    ExitOS();
  }

  if (input == "clear") {
    view.DisplayClear();
    view.DisplayMenu();
    return true;
  }

  std::cout << "Type \"initialize\" to access commands.\n";
  return false;
}

void Controller::HandlePostInit(const std::string &input) {
  if (input.empty())
    return;

  Command command = GetParsedInput(input);
  ExecuteCommand(command);
}

void Controller::run() {
  HandleView();
  std::string input;

  while (GetParsedInput(input).cliCommand != CLI_COMMAND::CLI_EXIT) {
    std::cout << "Enter Input: ";
    std::getline(std::cin, input);

    if (!isInitialized) {
      HandlePreInit(input);
    } else {
      HandlePostInit(input);
    }
  }
}

std::vector<std::string> trim(const std::string &s, char separator) {
  std::vector<std::string> tokens;
  std::string token;
  std::istringstream tokenStream(s);

  while (std::getline(tokenStream, token, separator)) {
    tokens.push_back(token);
  }
  return tokens;
}

void Controller::ExitOS() {
  std::cout << "Stopping prosched...\n";
  std::_Exit(EXIT_SUCCESS);
}

Command Controller::GetParsedInput(const std::string &input) {
  Command commands;

  if (input.empty()) {
    commands.cliCommand = CLI_COMMAND::UNKNOWN;
    return commands;
  }

  const std::size_t quote = input.find('"');
  const std::string head = (quote == std::string::npos) ? input
                                                        : input.substr(0, quote);

  std::vector<std::string> trimmed = trim(head, ' ');

  if (trimmed.empty()) {
    commands.cliCommand = CLI_COMMAND::UNKNOWN;
    return commands;
  }

  commands.cliCommand = IdentifyCommand(trimmed);

  if (commands.cliCommand == CLI_COMMAND::UNKNOWN) {
    commands.cliCommand = CLI_COMMAND::UNKNOWN;
    return commands;
  }

  if (commands.cliCommand == CLI_COMMAND::CLI_SCREEN_S ||
      commands.cliCommand == CLI_COMMAND::CLI_SCREEN_R ||
      commands.cliCommand == CLI_COMMAND::CLI_SCREEN_C) {
    if (trimmed.size() >= 3) {
      commands.processName = trimmed[2];
    }
    if (commands.cliCommand != CLI_COMMAND::CLI_SCREEN_R &&
        trimmed.size() >= 4) {
      commands.memorySize = ParseMemorySize(trimmed[3]);
    }
    if (commands.cliCommand == CLI_COMMAND::CLI_SCREEN_C) {
      commands.instructions = ExtractQuotedInstructions(input);
    }
    return commands;
  }

  return commands;
}

std::string Controller::ExtractQuotedInstructions(const std::string &input) {
  const std::size_t first = input.find('"');
  const std::size_t last = input.rfind('"');
  if (first == std::string::npos || last <= first) {
    return "";
  }

  const std::string quoted = input.substr(first + 1, last - first - 1);

  std::string unescaped;
  unescaped.reserve(quoted.size());
  for (std::size_t i = 0; i < quoted.size(); ++i) {
    if (quoted[i] == '\\' && i + 1 < quoted.size() && quoted[i + 1] == '"') {
      continue;
    }
    unescaped += quoted[i];
  }
  return unescaped;
}

long Controller::ParseMemorySize(const std::string &token) {
  if (token.empty()) {
    return 0;
  }

  for (char c : token) {
    if (!std::isdigit(static_cast<unsigned char>(c))) {
      return 0;
    }
  }

  try {
    return std::stol(token);
  } catch (const std::exception &) {
    // Out of range for a long — far past the upper bound anyway.
    return 0;
  }
}

std::string Controller::FormatAccessViolationNotice(const std::string &name,
                                                    const std::string &clockTime,
                                                    uint32_t address) {
  std::ostringstream oss;
  oss << "Process " << name
      << " shut down due to memory access violation error that occurred at "
      << clockTime << ". 0x" << std::uppercase << std::hex << address
      << " invalid.";
  return oss.str();
}

bool Controller::IsValidMemoryAllocation(long bytes) {
  if (bytes < prosched::kMinProcessMemoryBytes ||
      bytes > prosched::kMaxProcessMemoryBytes) {
    return false;
  }
  return (bytes & (bytes - 1)) == 0;
}

void Controller::ExecuteCommand(const Command &command) {
  try {
    switch (command.cliCommand) {
    case CLI_COMMAND::CLI_EXIT:
      ExitOS();
      break;

    case CLI_COMMAND::CLI_CLEAR:
      view.DisplayClear();
      view.DisplayCommands();
      break;

    case CLI_COMMAND::CLI_SCREEN_LS:
      this->scheduler->PrintProcesses(std::cout);
      break;

    case CLI_COMMAND::CLI_SCREEN_S: {
      if (command.processName.empty()) {
        std::cout << "screen -s: usage: screen -s <process_name> "
                     "<process_memory_size>\n\n";
        break;
      }
      if (!IsValidMemoryAllocation(command.memorySize)) {
        std::cout << "invalid memory allocation\n\n";
        break;
      }
      prosched::Process *p = this->scheduler->CreateNamedProcess(
          command.processName, command.memorySize);
      this->scheduler->AddProcess(p);
      EnterProcessScreen(p);
      break;
    }

    case CLI_COMMAND::CLI_SCREEN_C: {
      if (command.processName.empty()) {
        std::cout << "screen -c: usage: screen -c <process_name> "
                     "<process_memory_size> \"<instructions>\"\n\n";
        break;
      }
      if (!IsValidMemoryAllocation(command.memorySize)) {
        std::cout << "invalid memory allocation\n\n";
        break;
      }

      std::vector<prosched::Statement> program;
      prosched::Interpreter parser;
      if (!parser.ParseUserProgram(command.instructions, program) ||
          program.size() < prosched::kMinUserInstructions ||
          program.size() > prosched::kMaxUserInstructions) {
        std::cout << "invalid command\n\n";
        break;
      }

      prosched::Process *p = this->scheduler->CreateProcessWithInstructions(
          command.processName, command.memorySize, program);
      this->scheduler->AddProcess(p);
      EnterProcessScreen(p);
      break;
    }

    case CLI_COMMAND::CLI_SCREEN_R: {
      if (command.processName.empty()) {
        std::cout << "screen -r: a process name is required.\n\n";
        break;
      }
      prosched::Process *p =
          this->scheduler->FindProcessByName(command.processName);
      if (p != nullptr) {
        EnterProcessScreen(p);
        break;
      }

      // A process killed by a memory access violation reports why it is gone
      // instead of the generic "not found".
      prosched::Process *terminated =
          this->scheduler->FindTerminatedProcessByName(command.processName);
      if (terminated != nullptr) {
        std::cout << FormatAccessViolationNotice(
                         terminated->GetName(),
                         terminated->GetLastViolationClockTime(),
                         terminated->GetLastViolationAddress())
                  << "\n\n";
      } else {
        std::cout << "Process " << command.processName << " not found.\n\n";
      }
      break;
    }

    case CLI_COMMAND::CLI_SCHEDULER_START:
      if (!this->scheduler->IsRunning()) {
        this->scheduler->Start();
      }
      this->scheduler->ResumeGenerating();
      std::cout << "Started generating dummy processes.\n\n";
      break;

    case CLI_COMMAND::CLI_SCHEDULER_STOP:
      if (this->scheduler->IsRunning()) {
        this->scheduler->StopGenerating();
      } else {
        std::cout << "Scheduler has not started, run \"scheduler-start\" "
                     "to start\n\n";
      }
      break;

    case CLI_COMMAND::CLI_REPORT_UTIL:
      PrintReportUtil();
      break;

    case CLI_COMMAND::CLI_VMSTAT:
      PrintVmStat();
      break;

    case CLI_COMMAND::CLI_PROCESS_SMI:
      PrintProcessSmi();
      break;

    case CLI_COMMAND::UNKNOWN:
      std::cout << "Command unknown. Try again. \n";
      break;
    }

  } catch (const std::exception &e) {
    std::cerr << "Controller: Could not execute command" << e.what();
    return;
  }
}

void Controller::EnterProcessScreen(prosched::Process *p) {
  std::cout << "\033[?1049h" << std::flush;

  struct termios old_termios;
  tcgetattr(STDIN_FILENO, &old_termios);
  struct termios raw = old_termios;
  raw.c_lflag &= ~(ICANON | ECHO);
  raw.c_iflag &= ~(ICRNL);
  raw.c_cc[VMIN] = 1;
  raw.c_cc[VTIME] = 0;
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);

  auto restore = [&]() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &old_termios);
    std::cout << "\033[?1049l" << std::flush;
  };

  struct winsize ws;
  ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws);
  int rows = (ws.ws_row > 0) ? ws.ws_row : 24;

  std::vector<std::string> output_lines;
  int scroll_offset = 0;
  std::string cmd;

  auto push_output = [&](const std::string &text) {
    std::istringstream ss(text);
    std::string line;
    while (std::getline(ss, line))
      output_lines.push_back(line);
  };

  auto render_full = [&]() {
    std::cout << "\033[2J\033[H";
    int content_rows = rows - 1;
    int total = (int)output_lines.size();
    int start = total - content_rows - scroll_offset;
    if (start < 0)
      start = 0;
    int end = std::min(start + content_rows, total);
    for (int i = start; i < end; i++)
      std::cout << output_lines[i] << "\r\n";
    std::cout << "\033[" << rows << ";1H\033[2Kroot:\\> " << cmd << std::flush;
  };

  auto render_prompt = [&]() {
    std::cout << "\033[" << rows << ";1H\033[2Kroot:\\> " << cmd << std::flush;
  };

  render_full();

  while (true) {
    char c;
    if (read(STDIN_FILENO, &c, 1) <= 0)
      break;

    if (c == '\r' || c == '\n') {
      std::string trimmed = cmd;
      size_t s = trimmed.find_first_not_of(" \t");
      size_t e = trimmed.find_last_not_of(" \t");
      trimmed = (s == std::string::npos) ? "" : trimmed.substr(s, e - s + 1);
      cmd.clear();
      scroll_offset = 0;

      if (trimmed == "process-smi") {
        int total = p->GetTotalInstructions();
        int current = p->GetCurrentInstructionIndex();
        std::ostringstream oss;
        oss << "Process name: " << p->GetName();
        oss << "\nID: " << p->GetPID();
        oss << "\nLogs:";
        for (const auto &log : p->GetLogs())
          oss << "\n" << log;
        oss << "\n";
        if (current >= total)
          oss << "\nFinished!";
        else {
          oss << "\nCurrent instruction line: " << current + 1;
          oss << "\nLines of code: " << total;
        }
        push_output(oss.str());
      } else if (trimmed == "exit") {
        restore();
        return;
      } else if (!trimmed.empty()) {
        push_output("Unknown command. Use 'process-smi' or 'exit'.");
      }

      render_full();
    } else if (c == 127 || c == '\b') {
      if (!cmd.empty()) {
        cmd.pop_back();
        render_prompt();
      }
    } else if (c == '\033') {
      char seq[2] = {0};
      if (read(STDIN_FILENO, &seq[0], 1) <= 0 || seq[0] != '[')
        continue;
      if (read(STDIN_FILENO, &seq[1], 1) <= 0)
        continue;

      int max_scroll = std::max(0, (int)output_lines.size() - (rows - 1));
      if (seq[1] == 'A') {
        if (scroll_offset < max_scroll) {
          scroll_offset++;
          render_full();
        }
      } else if (seq[1] == 'B') {
        if (scroll_offset > 0) {
          scroll_offset--;
          render_full();
        }
      } else if (seq[1] == '5') {
        char tilde;
        read(STDIN_FILENO, &tilde, 1);
        scroll_offset = std::min(scroll_offset + (rows - 1), max_scroll);
        render_full();
      } else if (seq[1] == '6') {
        char tilde;
        read(STDIN_FILENO, &tilde, 1);
        scroll_offset = std::max(0, scroll_offset - (rows - 1));
        render_full();
      }
    } else if (c == 3 || c == 4) { // Ctrl+C / Ctrl+D
      restore();
      return;
    } else if (c >= 32) {
      cmd += c;
      render_prompt();
    }
  }

  restore();
}

namespace {

constexpr int kVmStatValueWidth = 13;
constexpr int kVmStatUnitWidth = 3;

const std::string kProcessSmiRule(44, '-');

// Emits one vmstat row. Pass an empty unit for quantities that have none.
void PrintVmStatRow(std::ostream &out, std::uint64_t value,
                    const std::string &unit, const std::string &description) {
  out << std::setw(kVmStatValueWidth) << std::right << value << " "
      << std::setw(kVmStatUnitWidth) << std::left << unit << " " << description
      << "\n";
}

} // namespace

void Controller::PrintVmStat() {
  // The store is kept in memory while the scheduler runs; make the file match
  // what is about to be reported.
  this->pagingManager->FlushBackingStore();

  const prosched::PagingManager::MemoryStats memory =
      this->pagingManager->GetMemoryStats();
  const prosched::Scheduler::CpuTickStats ticks =
      this->scheduler->GetCpuTickStats();

  PrintVmStatRow(std::cout, memory.totalMemoryBytes, "KB", "total memory");
  PrintVmStatRow(std::cout, memory.usedMemoryBytes, "KB", "used memory");
  PrintVmStatRow(std::cout, memory.freeMemoryBytes, "KB", "free memory");
  PrintVmStatRow(std::cout, ticks.idleCpuTicks, "", "idle CPU ticks");
  PrintVmStatRow(std::cout, ticks.activeCpuTicks, "", "active CPU ticks");
  PrintVmStatRow(std::cout, ticks.totalCpuTicks, "", "total CPU ticks");
  PrintVmStatRow(std::cout, memory.pagesPagedIn, "",
                 "accumulated number of pages paged in");
  PrintVmStatRow(std::cout, memory.pagesPagedOut, "",
                 "accumulated number of pages paged out");
  std::cout << "\n";
}

void Controller::PrintProcessSmi() {
  this->pagingManager->FlushBackingStore();

  const prosched::PagingManager::MemoryStats memory =
      this->pagingManager->GetMemoryStats();

  double memoryUtil =
      memory.totalMemoryBytes == 0
          ? 0.0
          : (static_cast<double>(memory.usedMemoryBytes) /
             static_cast<double>(memory.totalMemoryBytes)) * 100.0;

  std::cout << kProcessSmiRule << "\n"
            << "| PROCESS-SMI V01.00 Driver Version: 01.00 |\n"
            << kProcessSmiRule << "\n\n";

  std::cout << "CPU-Util: " << static_cast<int>(this->scheduler->GetCpuUtilization())
            << "%\n";
  std::cout << "Memory Usage: " << memory.usedMemoryBytes << "KB / "
            << memory.totalMemoryBytes << "KB\n";
  std::cout << "Memory Util: " << static_cast<int>(memoryUtil) << "%\n\n";

  std::cout << std::string(kProcessSmiRule.size(), '=') << "\n";
  std::cout << "Running processes and memory usage:\n";
  std::cout << kProcessSmiRule << "\n";

  bool anyRunning = false;
  for (prosched::Process *p : this->scheduler->GetAllProcesses()) {
    if (p == nullptr || p->IsFinished()) {
      continue;
    }
    anyRunning = true;
    std::cout << p->GetName() << " "
              << this->pagingManager->GetProcessMemoryBytes(p->GetPID())
              << "KB\n";
  }
  if (!anyRunning) {
    std::cout << "(none)\n";
  }

  std::cout << kProcessSmiRule << "\n\n";
}

void Controller::PrintReportUtil() {
  this->scheduler->PrintProcesses(std::cout);

  std::ofstream file("csopesy-log.txt");
  if (file.is_open()) {
    this->scheduler->PrintProcesses(file);
    file.close();
    std::cout << "Report saved to csopesy-log.txt\n\n";
  } else {
    std::cerr << "Warning: could not write csopesy-log.txt\n";
  }
}

CLI_COMMAND
Controller::IdentifyCommand(const std::vector<std::string> &command) {
  if (command[0] == "screen") {
    if (command.size() < 2)
      return CLI_COMMAND::UNKNOWN;
    if (command[1] == "-ls") {
      return CLI_COMMAND::CLI_SCREEN_LS;
    } else if (command[1] == "-s") {
      return CLI_COMMAND::CLI_SCREEN_S;
    } else if (command[1] == "-r") {
      return CLI_COMMAND::CLI_SCREEN_R;
    } else if (command[1] == "-c") {
      return CLI_COMMAND::CLI_SCREEN_C;
    } else {
      return CLI_COMMAND::UNKNOWN;
    }
  } else if (command[0] == "exit") {
    return CLI_COMMAND::CLI_EXIT;
  } else if (command[0] == "clear") {
    return CLI_COMMAND::CLI_CLEAR;
  } else if (command[0] == "scheduler-start") {
    return CLI_COMMAND::CLI_SCHEDULER_START;
  } else if (command[0] == "scheduler-stop") {
    return CLI_COMMAND::CLI_SCHEDULER_STOP;
  } else if (command[0] == "report-util") {
    return CLI_COMMAND::CLI_REPORT_UTIL;
  } else if (command[0] == "vmstat") {
    return CLI_COMMAND::CLI_VMSTAT;
  } else if (command[0] == "process-smi") {
    return CLI_COMMAND::CLI_PROCESS_SMI;
  } else {
    return CLI_COMMAND::UNKNOWN;
  }
}
