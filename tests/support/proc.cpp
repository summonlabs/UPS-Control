#include "proc.hpp"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

#if defined(_WIN32)
#include <process.h>
#else
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace uc_test {
namespace {

void sleep_briefly() { std::this_thread::sleep_for(std::chrono::milliseconds(5)); }

#if defined(_WIN32)
/// `_spawnv` joins its argument array with single spaces and does not quote, so
/// an argument containing a space is split into several arguments by the child's
/// command-line parser. Every argument therefore has to be quoted here, which
/// matters most for `argv[0]`: this repository's own path contains spaces.
std::string quote_argument(const std::string& value) {
  if (value.find_first_of(" \t\"") == std::string::npos) {
    return value;
  }
  std::string quoted = "\"";
  for (const char character : value) {
    if (character == '"') {
      quoted += "\\\"";
    } else {
      quoted += character;
    }
  }
  quoted += "\"";
  return quoted;
}

/// Builds the quoted argument storage and the pointer array over it. The storage
/// must outlive the pointer array, so both are returned together.
struct SpawnArguments {
  std::vector<std::string> storage;
  std::vector<const char*> pointers;

  explicit SpawnArguments(const std::string& executable,
                          const std::vector<std::string>& arguments) {
    storage.reserve(arguments.size() + 2);
    storage.push_back(quote_argument(executable));
    for (const std::string& argument : arguments) {
      storage.push_back(quote_argument(argument));
    }
    pointers.reserve(storage.size() + 1);
    for (const std::string& value : storage) {
      pointers.push_back(value.c_str());
    }
    pointers.push_back(nullptr);
  }
};
#endif

}  // namespace

std::uint64_t current_pid() {
#if defined(_WIN32)
  return static_cast<std::uint64_t>(_getpid());
#else
  return static_cast<std::uint64_t>(::getpid());
#endif
}

std::string probe_path() { return std::string(UC_PROBE_EXECUTABLE); }

std::string cli_path() { return std::string(UC_CLI_EXECUTABLE); }

bool spawn_nowait(const std::string& executable, const std::vector<std::string>& arguments,
                  std::string& error) {
#if defined(_WIN32)
  const SpawnArguments spawn(executable, arguments);
  // _P_NOWAIT returns the process handle as an intptr_t on Windows. The handle is
  // intentionally not closed: the test process owns at most a handful of them and
  // the child's identity is recovered from the pid it writes to its ready file.
  const intptr_t result = _spawnv(_P_NOWAIT, executable.c_str(), spawn.pointers.data());
  if (result == -1) {
    error = "could not start the probe process";
    return false;
  }
  return true;
#else
  std::vector<char*> argv;
  argv.push_back(const_cast<char*>(executable.c_str()));
  for (const std::string& argument : arguments) {
    argv.push_back(const_cast<char*>(argument.c_str()));
  }
  argv.push_back(nullptr);
  pid_t pid = 0;
  const int result = posix_spawn(&pid, executable.c_str(), nullptr, nullptr, argv.data(), environ);
  if (result != 0) {
    error = "could not start the probe process";
    return false;
  }
  return true;
#endif
}

bool spawn_wait(const std::string& executable, const std::vector<std::string>& arguments,
                int& exit_code, std::string& error) {
#if defined(_WIN32)
  const SpawnArguments spawn(executable, arguments);
  const intptr_t result = _spawnv(_P_WAIT, executable.c_str(), spawn.pointers.data());
  if (result == -1) {
    error = "could not start the probe process";
    return false;
  }
  exit_code = static_cast<int>(result);
  return true;
#else
  std::vector<char*> argv;
  argv.push_back(const_cast<char*>(executable.c_str()));
  for (const std::string& argument : arguments) {
    argv.push_back(const_cast<char*>(argument.c_str()));
  }
  argv.push_back(nullptr);
  pid_t pid = 0;
  const int spawn_result = posix_spawn(&pid, executable.c_str(), nullptr, nullptr, argv.data(),
                                      environ);
  if (spawn_result != 0) {
    error = "could not start the probe process";
    return false;
  }
  int status = 0;
  if (::waitpid(pid, &status, 0) < 0) {
    error = "could not wait for the probe process";
    return false;
  }
  exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128;
  return true;
#endif
}

bool run_captured(const std::string& executable, const std::vector<std::string>& arguments,
                  const std::filesystem::path& output_file, int& exit_code, std::string& error) {
  std::string command;
#if defined(_WIN32)
  command = quote_argument(executable);
#else
  command = "'" + executable + "'";
#endif
  for (const std::string& argument : arguments) {
#if defined(_WIN32)
    command += " " + quote_argument(argument);
#else
    command += " '" + argument + "'";
#endif
  }
  command += " > ";
#if defined(_WIN32)
  command += quote_argument(output_file.string());
#else
  command += "'" + output_file.string() + "'";
#endif
  command += " 2>&1";
#if defined(_WIN32)
  // std::system runs the line through `cmd /c`. When the line contains more than
  // two quote characters, cmd's legacy rule strips the first and last quote of the
  // whole line, which splits the executable path at its first space and the child
  // never starts. Wrapping the entire command in one more pair of quotes satisfies
  // that rule and leaves every argument intact, so a value containing a space,
  // a tab, or a double quote survives.
  command = "\"" + command + "\"";
#endif
  const int result = std::system(command.c_str());
  exit_code = result;
  if (result == -1) {
    error = "could not run '" + executable + "'";
    return false;
  }
  return true;
}

bool terminate_process(std::uint64_t pid, std::string& error) {
#if defined(_WIN32)
  const std::string command = "taskkill /F /PID " + std::to_string(pid) + " > NUL 2>&1";
#else
  const std::string command = "kill -9 " + std::to_string(pid) + " > /dev/null 2>&1";
#endif
  const int result = std::system(command.c_str());
  if (result != 0) {
    error = "taskkill reported failure for pid " + std::to_string(pid);
    return false;
  }
  return true;
}

bool wait_for_file(const std::filesystem::path& path, int attempts, std::string& error) {
  for (int attempt = 0; attempt < attempts; ++attempt) {
    std::error_code status_error;
    const auto size = std::filesystem::file_size(path, status_error);
    if (!status_error && size > 0) {
      return true;
    }
    sleep_briefly();
  }
  error = "the probe never produced '" + path.string() + "'";
  return false;
}

std::vector<std::string> read_lines(const std::filesystem::path& path) {
  std::vector<std::string> lines;
  std::ifstream stream(path, std::ios::binary);
  std::string line;
  while (std::getline(stream, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    lines.push_back(line);
  }
  return lines;
}

std::map<std::string, std::string> read_key_values(const std::filesystem::path& path) {
  std::map<std::string, std::string> values;
  std::ifstream stream(path, std::ios::binary);
  std::string line;
  while (std::getline(stream, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    const std::size_t equals = line.find('=');
    if (equals == std::string::npos) {
      continue;
    }
    values[line.substr(0, equals)] = line.substr(equals + 1);
  }
  return values;
}

std::uint64_t read_pid(const std::filesystem::path& ready_file) {
  const std::map<std::string, std::string> values = read_key_values(ready_file);
  const auto found = values.find("pid");
  if (found == values.end()) {
    return 0;
  }
  return std::strtoull(found->second.c_str(), nullptr, 10);
}

}  // namespace uc_test
