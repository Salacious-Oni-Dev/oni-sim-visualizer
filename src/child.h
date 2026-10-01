// Running one of this program's own experiments as a SEPARATE PROCESS, with its output
// arriving in the window's pane a line at a time.
//
// WHY NOT JUST CALL THE FUNCTION. `CheckpointTest`, `TimelineTest`, `TimelineDiff` and
// `LoadStress` are already right here in the same binary, and calling one from a button would
// be four lines. Every one of those four lines would be wrong:
//
//   * THEY DESTROY THE SESSION. Each one captures, restores and re-runs the world dozens of
//     times. The world on screen afterwards is not the world that was on screen before, the
//     scrub timeline's checkpoints describe a history that no longer happened, and there is no
//     way back short of re-booting the corpus. A debugger that silently replaces what you were
//     looking at is worse than one that cannot run the test at all.
//   * THEY TAKE SECONDS TO MINUTES. `--load-stress 2000` is thousands of full save/load
//     cycles. In-process that is a window that stops repainting, stops answering the mouse,
//     and is indistinguishable from a hang -- including to Windows, which would grey it out
//     and offer to close it.
//   * ONE OF THEM IS A CRASH REPRO ON PURPOSE. `--load-stress` exists because the third
//     in-place Load takes the process down (the driver's own note). Running it
//     in-process means the debugger dies of the bug it is demonstrating, taking the pane with
//     the evidence in it along with it.
//
// So the button spawns `simviz.exe` again with the flags that test is spelled as, reads its
// stdout through a pipe, and feeds the lines into the same capture every in-process feature
// prints to. The window keeps painting, the world stays put, a crash kills the child and is
// reported as an exit code, and there is a stop button.
//
// It also means the command shown beside the button is not a description of what runs. It IS
// what runs -- the same string, handed to CreateProcess.

#pragma once

#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

#include "out.h"

namespace simviz {

// The path of the running executable, which is what a spawned experiment has to be: a bare
// "simviz.exe" would resolve against the working directory, and the working directory is
// wherever the person happened to be when they started this, not where the binary lives.
inline std::string OwnExePath() {
  char buf[MAX_PATH];
  const DWORD n = GetModuleFileNameA(nullptr, buf, sizeof(buf));
  if (n == 0 || n >= sizeof(buf)) return "simviz.exe";
  return std::string(buf, n);
}

class Child {
 public:
  ~Child() { Close(); }

  bool running() const { return proc_ != nullptr; }
  const std::string& command() const { return command_; }
  // Valid once running() has gone false after a run. STILL_ACTIVE never escapes: Poll only
  // records a code once the process has actually exited.
  DWORD exit_code() const { return exit_code_; }
  bool exited_badly() const { return exit_code_ != 0; }

  // `args` is everything after the executable. Returns false and fills `err` if the process
  // could not be started; a process that starts and then fails is a normal run with a
  // non-zero exit code, which is a result rather than an error.
  bool Start(const std::string& args, std::string* err) {
    if (running()) {
      *err = "one experiment is already running";
      return false;
    }
    Close();

    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE read = nullptr, write = nullptr;
    if (!CreatePipe(&read, &write, &sa, 1 << 16)) {
      *err = "could not open a pipe for the child's output";
      return false;
    }
    // The READ end must not be inherited, or the child holds a copy of it and the pipe never
    // reports end-of-file when the child exits -- the classic way this goes wrong.
    SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);

    const std::string exe = OwnExePath();
    command_ = exe + " " + args;
    // CreateProcess writes into its command line, so it cannot be a string literal or a
    // c_str(). A vector, terminated, is the documented way to do this.
    std::vector<char> line(command_.begin(), command_.end());
    line.push_back('\0');

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = write;
    si.hStdError = write;
    si.hStdInput = nullptr;

    PROCESS_INFORMATION pi = {};
    // CREATE_NO_WINDOW: the child's stdout is this pipe, so a console would be an empty black
    // rectangle appearing over the window for the length of the run.
    const BOOL ok = CreateProcessA(exe.c_str(), line.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    // Our copy of the write end goes NOW, whether or not the spawn worked. While this process
    // holds one, the pipe has a writer and a read at the end of the run would block forever.
    CloseHandle(write);
    if (!ok) {
      CloseHandle(read);
      char msg[128];
      snprintf(msg, sizeof(msg), "could not start %s (error %lu)", exe.c_str(),
               static_cast<unsigned long>(GetLastError()));
      *err = msg;
      return false;
    }
    CloseHandle(pi.hThread);
    proc_ = pi.hProcess;
    pipe_ = read;
    exit_code_ = 0;
    return true;
  }

  // Called once a frame. Moves whatever the child has written into `into`, and notices when
  // it has finished. Never blocks: a read on a pipe with nothing in it would stall the
  // window for as long as the child was thinking, which is the whole thing this avoids.
  //
  // Returns true on the frame the child finished, so the caller can print a verdict once.
  bool Poll(out::Capture* into) {
    if (proc_ == nullptr) return false;
    Drain(into);

    if (WaitForSingleObject(proc_, 0) != WAIT_OBJECT_0) return false;
    // The child is gone, but the pipe may still hold what it wrote just before exiting.
    Drain(into);
    if (!into->partial.empty()) {
      out::PushLine(into, into->partial);
      into->partial.clear();
    }
    DWORD code = 0;
    GetExitCodeProcess(proc_, &code);
    exit_code_ = code;
    Close();
    return true;
  }

  // Kills the child. Its output up to this point is already in the capture; what it would
  // have printed next is lost, which is what asking to stop means.
  void Stop() {
    if (proc_ == nullptr) return;
    TerminateProcess(proc_, 1);
    WaitForSingleObject(proc_, 2000);
    exit_code_ = 1;
    Close();
  }

 private:
  void Drain(out::Capture* into) {
    if (pipe_ == nullptr) return;
    for (;;) {
      DWORD avail = 0;
      if (!PeekNamedPipe(pipe_, nullptr, 0, nullptr, &avail, nullptr)) return;
      if (avail == 0) return;
      char buf[4097];
      DWORD got = 0;
      const DWORD want = avail < sizeof(buf) - 1 ? avail : sizeof(buf) - 1;
      if (!ReadFile(pipe_, buf, want, &got, nullptr) || got == 0) return;
      buf[got] = '\0';
      // Echoed as well as captured, so a windowed run started from a shell still fills that
      // shell -- the same contract out::Printf has for everything drawn in-process.
      if (into->echo) fputs(buf, stdout);
      out::Absorb(into, buf);
    }
  }

  void Close() {
    if (pipe_ != nullptr) CloseHandle(pipe_);
    if (proc_ != nullptr) CloseHandle(proc_);
    pipe_ = nullptr;
    proc_ = nullptr;
  }

  HANDLE proc_ = nullptr;
  HANDLE pipe_ = nullptr;
  DWORD exit_code_ = 0;
  std::string command_;
};

}  // namespace simviz
