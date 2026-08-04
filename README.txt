================================================================
CSOPESY MO2 - OS Emulator: Multitasking OS (prosched)
================================================================

GROUP MEMBERS
-------------
  Stephen Borja
  Erin Chua
  Zhean Ganituen
  Aaron Go


PROJECTS IN THIS REPOSITORY
---------------------------
  This is a monorepo. If this list is out of date, see CMakeLists.txt.

  prosched - the MO2 deliverable: process scheduler and demand-paging
             memory manager. Entry point: prosched/src/main.cpp
  mockos   - GUI-based OS mockup using ImGui (separate project, not
             part of the MO2 submission).


REQUIREMENTS
------------
  build-essential (g++ with C++20 support)
  cmake >= 3.20

  On Ubuntu/WSL:
      sudo apt update
      sudo apt install -y build-essential cmake


STEP 1 - USE THE MO2 BRANCH
---------------------------
  The "main" branch is still the MO1 version. The current MO2 build
  (demand paging, screen -c, READ/WRITE, vmstat, process-smi) is on
  the "MO2" branch:

      git fetch origin
      git checkout MO2


STEP 2 - BUILD AND RUN
----------------------
  Run everything from the REPOSITORY ROOT.

      cmake -S . -B build
      cmake --build build --target prosched
      ./build/prosched/prosched

  Or use the helper script and pick "prosched" from the menu:

      chmod +x s.sh
      ./s.sh r

  s.sh also takes b (build only), t (build and run that project's
  tests), and f (clang-format every C/C++ file).

  IMPORTANT: launch the program from the repository root. The config
  file is read from the path "prosched/config.txt" relative to the
  current directory. If it is not found, "initialize" will print
  "Failed to find config file".


STEP 3 - EDIT THE CONFIG (BEFORE TYPING "initialize")
-----------------------------------------------------
  File: prosched/config.txt
  All memory values are in bytes and must be powers of 2.

      num-cpu 8              number of CPU cores (1 to 128)
      scheduler rr           "fcfs" or "rr"
      quantum-cycles 1       RR time slice (>= 1)
      batch-process-freq 1   ticks between generated processes (>= 1)
      min-ins 1000           instructions per process (min <= max)
      max-ins 1000
      delay-per-exec 0       delay between instructions (>= 0)
      max-overall-mem 1024   total physical memory
      mem-per-frame 256      frame size / page size
      min-mem-per-proc 1024  memory per generated process (min <= max)
      max-mem-per-proc 1024


STEP 4 - COMMANDS
-----------------
  The program opens on a banner. Only "initialize", "clear", and
  "exit" work until the emulator is initialized.

      initialize
          Loads config.txt and starts the scheduler.

  After initialize:

      screen -s <name> <mem_size>
          Create a process and open its screen. The memory size is
          optional; if given it must be a power of 2 from 64 to 65536.

      screen -c <name> <mem_size> "<instructions>"
          Create a process from your own instructions (1 to 50,
          separated by semicolons) and open its screen.
          Example:
              screen -c p2 256 "DECLARE varA 10; DECLARE varB 5; ADD varA varA varB; WRITE 0x40 varA; READ varC 0x40; PRINT(\"Result: \" + varC)"

      screen -r <name>
          Re-attach to an existing process.

      screen -ls
          Show CPU utilization, cores used, and the running and
          finished processes.

      scheduler-start   (also accepts scheduler-test)
          Start generating dummy processes.

      scheduler-stop
          Stop generating new processes.

      process-smi
          CPU utilization, memory used / total, and the memory used
          by each running process.

      vmstat
          Total/used/free memory, idle/active/total CPU ticks, and
          the number of pages paged in and out.

      report-util
          Prints the screen -ls report and saves it to csopesy-log.txt.

      clear
          Clear the screen.

      exit
          Quit the emulator.

  Inside a process screen:

      process-smi     reprint the process view (name, ID, logs,
                      current instruction / total lines)
      exit            go back to the main command line
      Up/Down arrows or PageUp/PageDown scroll the logs


FILES PRODUCED
--------------
  csopesy-log.txt             written by report-util
  csopesy-backing-store.txt   pages evicted from physical memory


RUNNING THE UNIT TESTS (OPTIONAL)
---------------------------------
  From the repository root:

      ./run_tests.sh

  or:

      ctest --test-dir build/prosched --output-on-failure


RUNNING MOCKOS (NOT PART OF MO2)
--------------------------------
  mockos needs extra graphics libraries:

      sudo apt update && sudo apt upgrade -y
      sudo apt install -y xorg-dev libgl1-mesa-dev libglu1-mesa-dev libglfw3-dev

  Then build and run it with "./s.sh r" and pick "mockos" from the menu.
