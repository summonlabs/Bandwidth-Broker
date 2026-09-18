// Bandwidth Broker - vendor-neutral fabric bandwidth arbitration runtime.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Real child processes with real pipes.
//
// Multiprocess claims require multiprocess evidence: these helpers spawn an
// actual OS process with redirected stdio, read its output line by line, and
// can terminate it with the platform's hard-kill primitive. No thread, no
// in-process substitute.

#ifndef BB_TESTS_SUPPORT_CHILD_PROCESS_HPP
#define BB_TESTS_SUPPORT_CHILD_PROCESS_HPP

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace bb_child {

class ChildProcess final {
 public:
  ChildProcess() = default;
  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;
  ChildProcess(ChildProcess&& other) noexcept { move_from(other); }
  ChildProcess& operator=(ChildProcess&& other) noexcept {
    if (this != &other) {
      move_from(other);
    }
    return *this;
  }
  ~ChildProcess() { close_handles(); }

  // Spawns p executable with p arguments, capturing stdout and feeding stdin.
  [[nodiscard]] bool spawn(const std::string& executable, const std::vector<std::string>& arguments) {
#if defined(_WIN32)
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;

    HANDLE child_stdout_read = nullptr;
    HANDLE child_stdout_write = nullptr;
    HANDLE child_stdin_read = nullptr;
    HANDLE child_stdin_write = nullptr;
    if (!CreatePipe(&child_stdout_read, &child_stdout_write, &attributes, 0)) {
      return false;
    }
    SetHandleInformation(child_stdout_read, HANDLE_FLAG_INHERIT, 0);
    if (!CreatePipe(&child_stdin_read, &child_stdin_write, &attributes, 0)) {
      CloseHandle(child_stdout_read);
      CloseHandle(child_stdout_write);
      return false;
    }
    SetHandleInformation(child_stdin_write, HANDLE_FLAG_INHERIT, 0);

    std::string command = "\"" + executable + "\"";
    for (const std::string& argument : arguments) {
      command += " \"" + argument + "\"";
    }
    std::vector<char> mutable_command(command.begin(), command.end());
    mutable_command.push_back('\0');

    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = child_stdout_write;
    startup.hStdError = child_stdout_write;
    startup.hStdInput = child_stdin_read;
    PROCESS_INFORMATION info{};
    const BOOL created = CreateProcessA(nullptr, mutable_command.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info);
    CloseHandle(child_stdout_write);
    CloseHandle(child_stdin_read);
    if (!created) {
      CloseHandle(child_stdout_read);
      CloseHandle(child_stdin_write);
      return false;
    }
    CloseHandle(info.hThread);
    process_ = info.hProcess;
    stdout_read_ = child_stdout_read;
    stdin_write_ = child_stdin_write;
    return true;
#else
    int stdout_pipe[2] = {-1, -1};
    int stdin_pipe[2] = {-1, -1};
    if (::pipe(stdout_pipe) != 0 || ::pipe(stdin_pipe) != 0) {
      return false;
    }
    const pid_t child = ::fork();
    if (child < 0) {
      return false;
    }
    if (child == 0) {
      ::dup2(stdout_pipe[1], STDOUT_FILENO);
      ::dup2(stdout_pipe[1], STDERR_FILENO);
      ::dup2(stdin_pipe[0], STDIN_FILENO);
      ::close(stdout_pipe[0]);
      ::close(stdout_pipe[1]);
      ::close(stdin_pipe[0]);
      ::close(stdin_pipe[1]);
      std::vector<char*> argv;
      argv.push_back(const_cast<char*>(executable.c_str()));
      for (const std::string& argument : arguments) {
        argv.push_back(const_cast<char*>(argument.c_str()));
      }
      argv.push_back(nullptr);
      ::execv(executable.c_str(), argv.data());
      ::_exit(127);
    }
    ::close(stdout_pipe[1]);
    ::close(stdin_pipe[0]);
    pid_ = child;
    stdout_read_ = stdout_pipe[0];
    stdin_write_ = stdin_pipe[1];
    return true;
#endif
  }

  // Reads one line from the child's stdout. Blocks until a line or EOF arrives;
  // this is a real blocking read, never a polling timeout.
  [[nodiscard]] std::string read_line() {
    std::string line;
    char byte = 0;
    for (;;) {
      const int read = read_byte(&byte);
      if (read <= 0) {
        break;
      }
      if (byte == '\n') {
        break;
      }
      if (byte != '\r') {
        line.push_back(byte);
      }
    }
    return line;
  }

  void write_line(const std::string& line) {
    const std::string payload = line + "\n";
#if defined(_WIN32)
    DWORD written = 0;
    if (stdin_write_ != nullptr) {
      WriteFile(static_cast<HANDLE>(stdin_write_), payload.data(), static_cast<DWORD>(payload.size()), &written, nullptr);
    }
#else
    if (stdin_write_ >= 0) {
      ssize_t ignored = ::write(stdin_write_, payload.data(), payload.size());
      (void)ignored;
    }
#endif
  }

  void close_stdin() {
#if defined(_WIN32)
    if (stdin_write_ != nullptr) {
      CloseHandle(static_cast<HANDLE>(stdin_write_));
      stdin_write_ = nullptr;
    }
#else
    if (stdin_write_ >= 0) {
      ::close(stdin_write_);
      stdin_write_ = -1;
    }
#endif
  }

  // Terminates the process without giving it a chance to clean up: this is the
  // hard kill the multiprocess proof requires.
  void kill_hard() {
#if defined(_WIN32)
    if (process_ != nullptr) {
      TerminateProcess(static_cast<HANDLE>(process_), 137);
    }
#else
    if (pid_ > 0) {
      ::kill(pid_, SIGKILL);
    }
#endif
  }

  [[nodiscard]] int wait() {
    int code = -1;
#if defined(_WIN32)
    if (process_ != nullptr) {
      WaitForSingleObject(static_cast<HANDLE>(process_), INFINITE);
      DWORD exit_code = 0;
      GetExitCodeProcess(static_cast<HANDLE>(process_), &exit_code);
      code = static_cast<int>(exit_code);
    }
#else
    if (pid_ > 0) {
      int status = 0;
      ::waitpid(pid_, &status, 0);
      code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
#endif
    close_handles();
    return code;
  }

  [[nodiscard]] bool valid() const noexcept {
#if defined(_WIN32)
    return process_ != nullptr;
#else
    return pid_ > 0;
#endif
  }

 private:
  [[nodiscard]] int read_byte(char* out) {
#if defined(_WIN32)
    if (stdout_read_ == nullptr) {
      return 0;
    }
    DWORD read = 0;
    if (!ReadFile(static_cast<HANDLE>(stdout_read_), out, 1, &read, nullptr) || read == 0) {
      return 0;
    }
    return 1;
#else
    if (stdout_read_ < 0) {
      return 0;
    }
    const ssize_t read = ::read(stdout_read_, out, 1);
    return read == 1 ? 1 : 0;
#endif
  }

  void close_handles() {
#if defined(_WIN32)
    if (stdout_read_ != nullptr) {
      CloseHandle(static_cast<HANDLE>(stdout_read_));
      stdout_read_ = nullptr;
    }
    if (stdin_write_ != nullptr) {
      CloseHandle(static_cast<HANDLE>(stdin_write_));
      stdin_write_ = nullptr;
    }
    if (process_ != nullptr) {
      CloseHandle(static_cast<HANDLE>(process_));
      process_ = nullptr;
    }
#else
    if (stdout_read_ >= 0) {
      ::close(stdout_read_);
      stdout_read_ = -1;
    }
    if (stdin_write_ >= 0) {
      ::close(stdin_write_);
      stdin_write_ = -1;
    }
    pid_ = -1;
#endif
  }

  void move_from(ChildProcess& other) {
    close_handles();
#if defined(_WIN32)
    process_ = other.process_;
    stdout_read_ = other.stdout_read_;
    stdin_write_ = other.stdin_write_;
    other.process_ = nullptr;
    other.stdout_read_ = nullptr;
    other.stdin_write_ = nullptr;
#else
    pid_ = other.pid_;
    stdout_read_ = other.stdout_read_;
    stdin_write_ = other.stdin_write_;
    other.pid_ = -1;
    other.stdout_read_ = -1;
    other.stdin_write_ = -1;
#endif
  }

#if defined(_WIN32)
  void* process_{nullptr};
  void* stdout_read_{nullptr};
  void* stdin_write_{nullptr};
#else
  int pid_{-1};
  int stdout_read_{-1};
  int stdin_write_{-1};
#endif
};

}  // namespace bb_child

#endif  // BB_TESTS_SUPPORT_CHILD_PROCESS_HPP
