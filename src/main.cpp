#include <iostream>
#include <string>
#include <vector>
#include <optional>
#include <filesystem>
#include <sstream>
#include <memory>
#include <array>

namespace fs = std::filesystem;

#ifdef _WIN32
  const char PATH_DELIM = ';';
  const std::string EXE_EXT = ".exe";
#else
  constexpr char PATH_DELIM = ':';
  constexpr std::string EXE_EXT;
#endif

// Reads from environment variables
static std::optional<std::string> get_env_var(const std::string& key) {
  if (const char* var = std::getenv(key.c_str()); var == nullptr) {
    return std::nullopt;
  }
  else return std::string(var);
}

// Checks if a path is executable
static bool is_executable(const fs::path& path) {
  if (!fs::exists(path) || fs::is_directory(path)) {
    return false;
  }
  const fs::perms permissions = fs::status(path).permissions();
  return (permissions & (fs::perms::owner_exec |
                 fs::perms::group_exec |
                 fs::perms::others_exec)) != fs::perms::none;
}

// checks if a command is in path and returns its full path if found
static std::string find_command_in_path(const std::string& command) {
  const auto path = get_env_var("PATH");

  std::stringstream ss(*path);
  std::string dir;
  const std::string cmd_with_extension = command + EXE_EXT;
  while (std::getline(ss, dir, PATH_DELIM)) {
    if (dir.empty()) continue;

    if (auto fullpath = fs::path(dir) / cmd_with_extension; is_executable(fullpath)) {
      return fullpath.string();
    }
  }
  return "";
}

static std::string execute_command(const std::string& command) {
  std::array<char, 128> buffer{};
  std::string result;

  std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(command.c_str(), "r"), pclose);
  if (pipe == nullptr) {
    return "Error: popen() failed!";
  }
  while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
    result += buffer.data();
  }
  return result;
}

// Removes white space from the left
static void trim_left(std::string &s) {
  // Find the first character that is NOT a whitespace
  if (const size_t first_non_space = s.find_first_not_of(" \t\n\r\v\f"); first_non_space != std::string::npos) {
    s.erase(0, first_non_space);
  }
  else {
    // If the string is completely whitespace, clear it entirely
    s.clear();
  }
}

int main() {
  // Flush after every std::cout / std:cerr
  std::cout << std::unitbuf;
  std::cerr << std::unitbuf;

  // TODO: Uncomment the code below to pass the first stage
  std::vector<std::string> builtins = {"echo", "exit", "type", "pwd"};
  //REPL Read-Eval-Print-Loop
  while (true) {
    std::string command;
    std::string input;

    std::cout << "$ ";
    std::cin >> command;
    trim_left(command);

    std::getline(std::cin, input);
    if (command == "echo") {
      trim_left(input);
      std::cout << input << "\n";
      continue;
    }
    else if (command == "type") {
      trim_left(input);
      if (auto it = std::ranges::find(builtins, input); it != builtins.end()) {
        std::cout << *it << " is a shell builtin\n";
      }
      else {
        if (std::string path = find_command_in_path(input); !path.empty()) {
          std::cout << input << " is " << path << "\n";
        }
        else {
          std::cout << input << ": not found\n";
        }
      }
      continue;
    }
    else if (command == "exit") {
      break;
    }
    else if (command == "pwd") {
      try {
        fs::path cwd = fs::current_path();
        std::cout << cwd.string() << "\n";
      }
      catch (const fs::filesystem_error& e) {
        std::cerr << "Error getting path : " << e.what();
      }
      continue;
    }
    else if (std::string path = find_command_in_path(command); !path.empty()) {
      std::string result= execute_command(command + input);
      std::cout << result;
      continue;
    }
    std::cout << command << ": command not found\n";
  }
}
