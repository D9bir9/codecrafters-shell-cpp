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
  const char PATH_SEP = '\';
#else
  constexpr char PATH_DELIM = ':';
  constexpr std::string EXE_EXT;
  constexpr char PATH_SEP = '/';
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

static void stream_print_logic(const std::string& input, std::ostream& out) {
  std::string output, word;
  bool in_quote = false;
  bool in_double_quote = false;
  bool escape = false;
  char ch, pv_ch = 0;

  std::stringstream ss(input);

  while (ss >> std::noskipws >> ch) {
    if (ch == '\\' && !escape && !in_quote) {
      escape = true;
      continue;
    }

    if (ch == '\'' && !in_double_quote && !escape) {
      in_quote = !in_quote;
      if (in_quote == false) {
        output += word;
      }
      word = "";
      continue;
    }
    if (ch == '\"' && !in_quote && !escape) {
      in_double_quote = !in_double_quote;
      if (in_double_quote == false) {
        output += word;
      }
      word = "";
      continue;
    }
    if (in_quote || in_double_quote) {
      word.push_back(ch);
      pv_ch = ch;
      escape = false;
      continue;
    }
    if (ch == ' ' && !in_quote && !in_double_quote && pv_ch == ' '&&!escape) {
      continue;
    }

    output.push_back(ch);
    pv_ch = ch;
    escape = false;
  }
  out << output;
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
    std::string line;

    std::cout << "$ ";
    std::getline(std::cin, line);
    trim_left(line);
    if (line.starts_with('\'')) {
      size_t pos = line.find_first_of('\'', 1);
      command = line.substr(0, pos + 1);
      input = line.substr(pos + 1);
      std::ostringstream formatted_command;
      stream_print_logic(command, formatted_command);
      command = formatted_command.str();
    }
    else if (line.starts_with('\"')) {
      size_t pos = line.find_first_of('\"', 1);
      command = line.substr(0, pos + 1);
      input = line.substr(pos + 1);
      std::ostringstream formatted_command;
      stream_print_logic(command, formatted_command);
      command = formatted_command.str();
    }
    else {
      std::stringstream ss(line);
      ss >> command;
      std::getline(ss, input);
    }


    if (command == "echo") {
      trim_left(input);

      std::string result = execute_command(command.append(" " + input));
      std::cout << result;
      continue;
    }
    if (command == "type") {
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
    if (command == "exit") {
      break;
    }
    if (command == "pwd") {
      try {
        fs::path cwd = fs::current_path();
        std::cout << cwd.string() << "\n";
      }
      catch (const fs::filesystem_error& e) {
        std::cerr << "Error getting path : " << e.what() << "\n";
      }
      continue;
    }
    if (command == "cd") {
      trim_left(input);

      std::ostringstream oss;
      stream_print_logic(input, oss);
      auto in = std::string(oss.str());

      if (const size_t pos = in.find('~'); pos != std::string::npos) {
        auto path = get_env_var("HOME");
        in.replace(pos, 1,*path);
      }

      auto new_dir = fs::path(in);
      try {
        fs::current_path(new_dir);
      }
      catch (const fs::filesystem_error&) {
        std::cout << command <<  ": " << in << ": No such file or directory" << "\n";
      }
      continue;
    }
    if (std::string path = find_command_in_path(command); !path.empty()) {
      std::string result= execute_command(command + input);
      std::cout << result;
      continue;
    }
    std::cout << command << ": command not found\n";
  }
}
