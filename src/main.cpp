#include <iostream>
#include <string>
#include <vector>
#include <optional>
#include <filesystem>
#include <sstream>
#include <memory>
#include <array>
#include <readline/readline.h>
#include <readline/history.h>

namespace fs = std::filesystem;

const std::vector<std::string> builtins = {"echo", "exit", "type", "pwd"};

#ifdef _WIN32
  const char PATH_DELIM = ';';
  const std::string EXE_EXT = ".exe";
  const char PATH_SEP = '\';
#else
  constexpr char PATH_DELIM = ':';
  constexpr std::string EXE_EXT;
  constexpr char PATH_SEP = '/';
#endif

static char* command_generator(const char* text, const int state) {
  static size_t list_index, len;
  const std::string prefix(text);

  if (!state) {
    list_index = 0;
    len = prefix.length();
  }
  while (list_index < builtins.size()) {
    const std::string& cmd = builtins[list_index];
    list_index++;
    if (cmd.compare(0, len, prefix) == 0) {
      const auto match = static_cast<char *>(malloc(cmd.length() + 1));
      strcpy(match, cmd.c_str());
      return match;
    }
  }
  return nullptr;
}

static char** shell_completion(const char* text, int start, int end) {
  char** matches = nullptr;
  rl_attempted_completion_over = 1;
  if (start == 0) {
    matches = rl_completion_matches(text, command_generator);
  }
  else {
    matches = rl_completion_matches(text, rl_filename_completion_function);
  }
  return matches;
}

static void initialize_readline() {
  rl_attempted_completion_function = shell_completion;
  rl_bind_key('\t', rl_complete);
  rl_completion_append_character = ' ';
}

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
static void trim_right(std::string& s) {
  const std::string WHITESPACE = " \n\r\t\f\v";

  // Find the last character that is NOT a whitespace character
  if (const size_t end = s.find_last_not_of(WHITESPACE); end != std::string::npos) {
    s.erase(end + 1);
  } else {
    // If the string is not entirely whitespace, erase everything after 'end'
    s.clear(); // The string was completely whitespace
  }

}

static void wrap_string(std::string& s, char ch) {
  s.insert(0, 1, ch);
  s.push_back(ch);
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

static void process_input(const char delim, std::string& input, std::string& line, std::string& command, std::string& formatted_command) {
    trim_left(line);
    const size_t pos = line.find_first_of(delim, 1);
    command = line.substr(0, pos + 1);
    input = line.substr(pos + 1);
    std::ostringstream f_command;
    stream_print_logic(command, f_command);
    formatted_command = f_command.str();
    trim_left(formatted_command);
    trim_right(formatted_command);
    command = formatted_command;
    if (size_t space = formatted_command.find_first_of(' '); space != std::string::npos) {
      wrap_string(formatted_command, delim);
    }
}

static void get_input_and_format_input(std::string& input, std::string& line, std::string& command, std::string& fcmd, std::stringstream& ss_) {
  std::getline(ss_, line);
  if (line.starts_with('\'')) {
    process_input('\'', input,line, command, fcmd);
  }
  else if (line.starts_with('\"')) {
    process_input('"', input,line, command, fcmd);
  }
  else {
    std::stringstream ss(line);
    ss >> command;
    std::getline(ss, input);
    fcmd = command;
  }
}

int main() {
  // Flush after every std::cout / std:cerr
  std::cout << std::unitbuf;
  std::cerr << std::unitbuf;

  initialize_readline();

  using_history();

  // TODO: Uncomment the code below to pass the first stage
  //REPL Read-Eval-Print-Loop
  while (true) {
    std::string command;
    std::string input;
    std::string line;
    std::string formatted_command;

    //std::cout << "$ ";

    char* raw_input = readline("$ ");
    if (raw_input == nullptr) break;

    std::string command_(raw_input);
    free(raw_input);

    if (command_.empty()) continue;
    add_history(command_.c_str());

    std::stringstream ss_(command_);

   get_input_and_format_input(input, line, command, formatted_command, ss_);


    if (formatted_command == "echo") {
      trim_left(input);

      std::string result = execute_command(command.append(" " + input));
      std::cout << result;
      continue;
    }
    if (formatted_command == "type") {
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
    if (formatted_command == "exit") {
      break;
    }
    if (formatted_command == "pwd") {
      try {
        fs::path cwd = fs::current_path();
        std::cout << cwd.string() << "\n";
      }
      catch (const fs::filesystem_error& e) {
        std::cerr << "Error getting path : " << e.what() << "\n";
      }
      continue;
    }
    if (formatted_command == "cd") {
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
      std::string result= execute_command(formatted_command + input);
      std::cout << result;
      continue;
    }
    std::cout << command << ": command not found\n";
  }
}
