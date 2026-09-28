#pragma once

// Independent-process helpers for the multiprocess and crash-recovery tests.
//
// These spawn real operating-system processes. Nothing here simulates process
// death with a thread, and nothing here establishes a fact by waiting: the
// readiness loops below can only fail, never turn a missing fact into a pass.

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace uc_test {

/// The identifier of the current process.
std::uint64_t current_pid();

/// Absolute path of the probe executable, injected by the build.
std::string probe_path();

/// Absolute path of the installed-style command line tool, injected by the build.
std::string cli_path();

/// Starts a process and returns immediately.
bool spawn_nowait(const std::string& executable, const std::vector<std::string>& arguments,
                  std::string& error);

/// Starts a process and waits for it to finish. Returns its exit code.
bool spawn_wait(const std::string& executable, const std::vector<std::string>& arguments,
                int& exit_code, std::string& error);

/// Force-kills a process at the operating-system level.
bool terminate_process(std::uint64_t pid, std::string& error);

/// Runs a program to completion with its combined output redirected to a file,
/// so the result never depends on capturing a child's stdio through a pipe.
bool run_captured(const std::string& executable, const std::vector<std::string>& arguments,
                  const std::filesystem::path& output_file, int& exit_code, std::string& error);

/// Waits for a file to appear and be non-empty. Returns false when the attempts
/// are exhausted; it never reports success for a file that is not there.
bool wait_for_file(const std::filesystem::path& path, int attempts, std::string& error);

std::map<std::string, std::string> read_key_values(const std::filesystem::path& path);

std::vector<std::string> read_lines(const std::filesystem::path& path);

std::uint64_t read_pid(const std::filesystem::path& ready_file);

}  // namespace uc_test
