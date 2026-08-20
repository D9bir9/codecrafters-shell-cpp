#include <iostream>
#include <string>
#include <vector>
#include <optional>
#include <filesystem>
#include <sstream>
#include <memory>
#include <readline/readline.h>
#include <readline/history.h>
#include <cstring>
#include <ranges>
#include <algorithm>
#include <cctype>
#include <fstream>
#include <unistd.h>
#include <sys/wait.h>

namespace fs = std::filesystem;

const std::vector<std::string> builtins = {"echo", "exit", "type", "pwd"};

#ifdef _WIN32
  constexpr char PATH_DELIM = ';';
  constexpr const char* EXE_EXT = ".exe";
  constexpr char PATH_SEP = '\\';
#else
  constexpr char PATH_DELIM = ':';
  constexpr const char* EXE_EXT = "";
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
  const auto path_opt = get_env_var("PATH");
  if (!path_opt) return "";

  std::stringstream ss(*path_opt);
  std::string dir;
  const std::string cmd_with_extension = command + EXE_EXT;
  while (std::getline(ss, dir, PATH_DELIM)) {
    if (dir.empty()) continue;

    if (fs::path fullpath = fs::path(dir) / cmd_with_extension; is_executable(fullpath)) {
      return fullpath.string();
    }
  }
  return "";
}

// static std::string execute_command(const std::string& command) {
//   /*
//   std::array<char, 128> buffer{};
//   const std::string complete_cmd = command + " 2>&1";
//   std::string result;
//
//   const std::unique_ptr<FILE, int(*)(FILE*)> pipe(popen(complete_cmd.c_str(), "r"), pclose);
//   if (pipe == nullptr) {
//     return "Error: popen() failed!";
//   }
//   while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
//     result += buffer.data();
//   }
//   return result;
//   */
//
//   int return_code = std::system(command.c_str());
//   if (return_code == -1) {
//     std::cout << "Error: command execution failed\n";
//   }
//   return "";
// }

// Trim helpers
static void trim_left(std::string &s) {
  static constexpr const char* WHITESPACE = " \t\n\r\v\f";
  if (const size_t first = s.find_first_not_of(WHITESPACE); first == std::string::npos) {
    s.clear();
  } else if (first > 0) {
    s.erase(0, first);
  }
}

static void trim_right(std::string &s) {
  static constexpr const char* WHITESPACE = " \t\n\r\v\f";
  if (const size_t last = s.find_last_not_of(WHITESPACE); last == std::string::npos) {
    s.clear();
  } else if (last + 1 < s.size()) {
    s.erase(last + 1);
  }
}

static void trim(std::string &s) {
  trim_left(s);
  trim_right(s);
}

static void wrap_string(std::string& s, const char ch) {
  s.insert(0, 1, ch);
  s.push_back(ch);
}

static std::vector<std::string> Tokenize_input(const std::string& input_line) {
  std::vector<std::string> args;
  std::string current_token;

  bool in_single_quote = false;
  bool in_double_quote = false;
  bool escape = false;
  bool token_has_content = false;


  for (size_t i{}; i < input_line.size(); ++i) {
    const char ch = input_line[i];

    if (escape) {
      current_token.push_back(ch);
      token_has_content = true;
      escape = false;
      continue;
    }

    if (ch == '\\' && !in_single_quote) {
      escape = true;
      continue;
    }

    if (ch == '\'' && !in_double_quote) {
      in_single_quote = !in_single_quote;
      token_has_content = true;
      continue; // do not include quotes in output
    }

    if (ch == '"' && !in_single_quote) {
      in_double_quote = !in_double_quote;
      token_has_content = true;
      continue; // do not include quotes in output
    }

    if (!in_single_quote && !in_double_quote && std::isspace(static_cast<unsigned char>(ch))) {
      // collapse consecutive whitespace outside quotes into a single space
      if (!current_token.empty() || token_has_content) {
        args.push_back(current_token);
        current_token.clear();
        token_has_content = false;
      }
      continue;
    }

    if (ch == '>' && !in_single_quote && !in_double_quote) {
      if (!current_token.empty() || token_has_content) {
        args.push_back(current_token);
        current_token.clear();
        token_has_content = false;
      }
      if (i + 1 < input_line.size() && input_line[i + 1] == '>') {
        args.emplace_back(">>");
        i++;
      }
      else {
        args.emplace_back(">");
      }
      continue;
    }

    if (ch == '1' && !in_single_quote && !in_double_quote) {
      if (i + 1 < input_line.size() && input_line[i + 1] == '>') {
        if (!current_token.empty() || token_has_content) {
          args.push_back(current_token);
          current_token.clear();
          token_has_content = false;
        }
        if (i + 2 < input_line.size() && input_line[i + 2] == '>') {
          args.emplace_back("1>>");
          i += 2;
        }
        else {
          args.emplace_back("1>");
          i++;
        }
        continue;
      }
    }

    if (ch == '2' && !in_single_quote && !in_double_quote) {
      if (i + 1 < input_line.size() && input_line[i + 1] == '>') {
        if (!current_token.empty() || token_has_content) {
          args.push_back(current_token);
          current_token.clear();
          token_has_content = false;
        }
        if (i + 2 < input_line.size() && input_line[i + 2] == '>') {
          args.emplace_back("2>>");
          i += 2;
        }
        else {
          args.emplace_back("2>");
          i++;
        }
        continue;
      }
    }

    // Normal character
    current_token.push_back(ch);
    token_has_content = true;
  }

  if (!current_token.empty() || token_has_content) {
    args.push_back(current_token);
  }
  return args;
}

static void execute_builtin(const std::string& command, const std::vector<std::string>& args) {

    if (command == "echo") {
      for (size_t i{1}; i < args.size(); ++i) {
        std::cout << args[i] << (i + 1 < args.size() ? " " : "");
      }
      std::cout << "\n";
    }
    else if (command == "type") {
      if (args.size() < 2) std::cout << "type: missing operand \n";
      else {
        const std::string& target = args[1];
        if (const auto it = std::ranges::find(builtins, target); it != builtins.end()) std::cout << *it << " is a shell builtin\n";
        else if (const std::string path = find_command_in_path(target); !path.empty()) std::cout << target << " is " << path << "\n";
        else std::cout << target << ": not found\n";
      }
    }
    else if (command == "pwd") {
        std::cout << fs::current_path().string() << "\n";
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
    char* raw_input = readline("$ ");
    if (raw_input == nullptr) break;

    std::string line(raw_input);
    free(raw_input);

    if (line.find_first_not_of(" \t\n\r") == std::string::npos) continue;
    add_history(line.c_str());

    std::vector<std::string> args = Tokenize_input(line);
    if (args.empty()) continue;

    std::string redirect_file;
    std::string err_redirect_file;
    bool out_redir = false;
    bool err_redir = false;
    bool out_app = false;
    bool err_app = false;

    auto err_redir_it = std::ranges::find_if(args.begin(), args.end(),[](const std::string& arg){return arg == "2>" || arg == "2>>";});

    if (err_redir_it != args.end()) {
      if (*err_redir_it == "2>>") err_app = true;
      if (std::distance(args.begin(), err_redir_it) + 1 < args.size()) {
        err_redirect_file = *(err_redir_it + 1);
        err_redir = true;
        args.erase(err_redir_it, err_redir_it + 2);
      }
      else {
        std::cout << "Shell: syntax error near unexpected token 'newline' \n";
      }
    }
    auto redir_it = std::ranges::find_if(args.begin(), args.end(),[](const std::string& arg){return arg == ">" || arg == "1>" || arg == ">>" || arg == "1>>";});

    if ( redir_it != args.end()) {
      if (*redir_it == ">>" || *redir_it == "1>>") out_app = true;

      if (std::distance(args.begin(), redir_it) + 1 < args.size()) {
        redirect_file = *(redir_it + 1);
        out_redir = true;
        args.erase(redir_it, redir_it + 2);
      }
      else {
        std::cout << "Shell: syntax error near unexpected token 'newline' \n";
      }
    }

    if (args.empty()) continue;
    for (size_t i{}; i < args.size(); ++i) {
      trim(args[i]);
    }
    std::string& command = args.front();


    std::ofstream out_file;
    std::ofstream err_file;



    if (command == "exit") break;
    if (command == "cd") {
      std::vector<std::string>::value_type target_path = (args.size() > 1) ? args[1] : "";
      bool home_error = false;
      if (target_path.empty() || target_path.starts_with('~')) {
        if (auto home = get_env_var("HOME"); home.has_value()) {
          if (target_path.empty()) target_path = *home;
          else target_path.replace(0, 1, *home);
        }
        else {
          home_error = true;
        }
      }

      if (!home_error) {
        try {
          fs::current_path(fs::path(target_path));
        }
        catch (const fs::filesystem_error&) {
          std::cout << "cd: " << (args.size() > 1? args[1] : "") << ": No such file or directory" << "\n";
        }
      }
      continue;
    }

    if (pid_t pid = fork(); pid == 0) {
      // CHILD PROCESS
      // Standard C file redirection takes care of internal and external processes perfectly
      if (out_redir) std::freopen(redirect_file.c_str(), out_app ? "a" : "w", stdout);
      if (err_redir) std::freopen(err_redirect_file.c_str(), err_app ? "a" : "w", stderr);

      if (std::ranges::find(builtins, command) != builtins.end()) {
        execute_builtin(command, args);
        std::exit(0);
      }
      if (std::string binary_path = find_command_in_path(command); !binary_path.empty()) {
        if (command.find(' ') != std::string::npos) {
          wrap_string(command, '\'');
        }
        std::vector<char*> c_args;
        for (auto& arg : args) c_args.push_back(const_cast<char*>(arg.c_str()));
        c_args.push_back(nullptr);
        execv(binary_path.c_str(), c_args.data());
      } else {
        std::cout << command << ": command not found\n";
      }
      std::exit(1);
    }else {
    waitpid(pid, nullptr, 0); // PARENT SHELL WAITS
    }
  }
}
