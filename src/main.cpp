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
#include <complex>
#include <fstream>
#include <unistd.h>
#include <sys/wait.h>
#include <unordered_map>
#include <csignal>

namespace fs = std::filesystem;

#ifdef _WIN32
  constexpr char PATH_DELIM = ';';
  constexpr const char* EXE_EXT = ".exe";
  constexpr char PATH_SEP = '\\';
#else
  constexpr char PATH_DELIM = ':';
  constexpr const char* EXE_EXT = "";
  constexpr char PATH_SEP = '/';
#endif

namespace {
  struct CommandStage {
    std::vector<std::string> args;
    std::string out_file;
    std::string err_file;
    bool out_redir = false;
    bool err_redir = false;
    bool out_app = false;
    bool err_app = false;
  };
}

const std::vector<std::string> builtins = {"echo", "exit", "type", "pwd", "cd", "complete", "jobs"};
static std::unordered_map<std::string, std::string> completion_paths;
namespace {
  struct BackgroundJob{
    int job_id;
    pid_t pid;
    std::string command;
    std::string status = "Running";
    bool reported_done = false;
  };
}
static bool is_job = false;

static std::vector<BackgroundJob> active_jobs;
static int next_job_id = 1;

// Cleans up background processes that have naturally terminated
static void update_background_jobs() {
  auto it = active_jobs.begin();
  while (it != active_jobs.end()) {
    int status;

    // Only check background processes that are still actively running in our tracker
    if (it->status == "Running") {
      if (const pid_t result = waitpid(it->pid, &status, WNOHANG); result > 0) {
        std::string status_str = "Done";
        if (WIFSIGNALED(status)) {
          status_str = "Terminated";
        }
        it->status = status_str;
      }
    }
    ++it;
  }
}

static void setup_shell_signals() {
  // 1. Ignore SIGINT (Ctrl+C) in the parent shell process
  // This stops the shell itself from dying when you hit Ctrl+C
  std::signal(SIGINT, SIG_IGN);

  // 2. Ignore SIGTSTP (Ctrl+Z) in the parent shell process
  // This stops the shell from freezing/suspending in the background
  std::signal(SIGTSTP, SIG_IGN);
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

    if (fs::path full_path = fs::path(dir) / cmd_with_extension; is_executable(full_path)) {
      return full_path.string();
    }
  }
  return "";
}


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

static std::string wrap_string(const std::string &s, const char ch) {
  // If we aren't using single quotes, fall back to basic wrapping
  if (ch != '\'') {
    std::string out = s;
    out.insert(0, 1, ch);
    out.push_back(ch);
    return out;
  }

  // Robust Single-Quote Preservation
  std::string out = "'";
  for (const char c : s) {
    if (c == '\'') {
      // Close quote, append literal quote, reopen quote
      out += "'\\''";
    } else {
      out += c;
    }
  }
  out += '\'';
  return out;
}

static void trim(std::string &s) {
  trim_left(s);
  trim_right(s);
}

static std::vector<std::string> Tokenize_input(const std::string& input_line) {
  std::vector<std::string> args;
  std::string current_token;

  bool in_single_quote = false;
  bool in_double_quote = false;
  bool escape = false;
  bool token_has_content = false;
  bool completion_flag = false;


  for (size_t i{}; i < input_line.size(); ++i) {
    const char ch = input_line[i];

    if (escape) {
      current_token.push_back(ch);
      token_has_content = true;
      escape = false;
      continue;
    }

    if (ch == '\\' && !in_single_quote && !completion_flag) {
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

    if (!in_single_quote && !in_double_quote) {
      if (ch == '>' || ch == '|' ||
         ((ch == '1' || ch == '2') && i + 1 < input_line.size() && input_line[i + 1] == '>')) {

        if (!current_token.empty() || token_has_content) {
          args.push_back(current_token);
          current_token.clear();
          token_has_content = false;
        }

        if (ch == '|') {
          args.emplace_back("|");
        }
        else if (ch == '>') {
          if (i + 1 < input_line.size() && input_line[i + 1] == '>') { args.emplace_back(">>"); i++; }
          else args.emplace_back(">");
        }
        else if (ch == '1') {
          if (i + 2 < input_line.size() && input_line[i + 1] == '>' && input_line[i + 2] == '>') { args.emplace_back("1>>"); i += 2; }
          else { args.emplace_back("1>"); i++; }
        }
        else if (ch == '2') {
          if (i + 2 < input_line.size() && input_line[i + 1] == '>' && input_line[i + 2] == '>') { args.emplace_back("2>>"); i += 2; }
          else { args.emplace_back("2>"); i++; }
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

static char* custom_path_generator(const char* text, const int state) {
  static std::vector<std::string> script_matches;
  static size_t match_index = 0;

  if (!state) {
    script_matches.clear();
    match_index = 0;

    // Get the full command line to find out what command we are running
    // rl_line_buffer contains the raw characters currently typed in the shell
    const std::string line(rl_line_buffer);
    const std::vector<std::string> tokens = Tokenize_input(line);
    if (tokens.empty()) return nullptr;
    std::string complete_line;
    for (size_t i{}; i < tokens.size();++i) {
      complete_line += ((i > 0) ?  " " : "") + tokens[i];
    }
    setenv("COMP_LINE", complete_line.c_str(), 1);
    setenv("COMP_POINT", std::to_string(complete_line.length()).c_str(), 1);

    if (const std::string& cmd = tokens.front(); completion_paths.contains(cmd)) {
      std::string raw_script_path = completion_paths[cmd];

      // Strip out the wrapping single quotes we added before running via system shell
      if (raw_script_path.front() == '\'' && raw_script_path.back() == '\'') {
        raw_script_path = raw_script_path.substr(1, raw_script_path.length() - 2);
      }

      // 2. Identify the word being completed (argv[2]) and the previous word (argv[3])
      const std::string current_word = text; // e.g., "set"
      std::string prev_word;

      // Find where we are in the token stream to grab the previous word safely
      for (size_t i = 1; i < tokens.size(); ++i) {
        if (tokens[i] == current_word && i > 0) {
          prev_word = tokens[i - 1];
          break;
        }
      }
      // Fallback if the token stream layout differs slightly
      if (prev_word.empty() && tokens.size() >= 2) {
        prev_word = tokens[tokens.size() - 2];
      }
      if (prev_word.empty()) {
        prev_word = cmd; // Default fallback to the command itself
      }

      // 3. Build the full, standard arguments block: script command current_word prev_word
      const std::string exec_cmd = raw_script_path + " " +
                             wrap_string(cmd, '"') + " " +
                             wrap_string(current_word, '"') + " " +
                             wrap_string(prev_word, '"');

      // Open a pipe to read the script's stdout output line by line
      if (FILE* fp = popen(exec_cmd.c_str(), "r")) {
        char buffer[256];
        while (fgets(buffer, sizeof(buffer), fp) != nullptr) {
          std::string match(buffer);
          // Trim out trailing newlines emitted by the script
          trim_right(match);
          if (!match.empty()) {
            script_matches.push_back(match);
          }
        }
        pclose(fp);
      }
    }
  }

  // Return the matches to Readline one by one
  if (match_index < script_matches.size()) {
    return strdup(script_matches[match_index++].c_str());
  }

  return nullptr;
}


static char* command_generator(const char* text, const int state) {
  static size_t list_index, len;
  static std::vector<std::string> path_matches;
  static size_t path_match_index;
  const std::string prefix(text);

  if (!state) {
    list_index = 0;
    len = prefix.length();
    path_matches.clear();
    path_match_index = 0;

    // Collect all matching commands from the system PATH once per completion request
    if (const auto path_opt = get_env_var("PATH"); path_opt.has_value()) {
      std::stringstream ss(*path_opt);
      std::string dir;
      while (std::getline(ss, dir, PATH_DELIM)) {
        if (dir.empty()) continue;
        try {
          if (fs::exists(dir) && fs::is_directory(dir)) {
            for (const auto& entry : fs::directory_iterator(dir)) {
              std::string filename = entry.path().filename().string();
              // Verify the file begins with the prefix and is fully executable
              if (filename.compare(0, len, prefix) == 0 && is_executable(entry.path())) {
                path_matches.push_back(filename);
              }
            }
          }
        } catch (const std::exception&) {
          // Gracefully ignore directory permission errors during path scanning
        }
      }
      // Deduplicate commands found across different PATH folders
      std::ranges::sort(path_matches);
      auto [first, last] = std::ranges::unique(path_matches);
      path_matches.erase(first, last);
    }
  }

  // 1. Return matches from your custom shell built-ins
  while (list_index < builtins.size()) {
    const std::string& cmd = builtins[list_index++];
    if (cmd.compare(0, len, prefix) == 0) {
      return strdup(cmd.c_str());
    }
  }

  // 2. Return deduplicated matches from the system PATH variables
  if (path_match_index < path_matches.size()) {
    return strdup(path_matches[path_match_index++].c_str());
  }

  return nullptr;
}

static char** shell_completion(const char* text, const int start, int end) {

  if (start == 0) {
    return rl_completion_matches(text, command_generator);
  }
  const std::string line(rl_line_buffer);

  if (const std::vector<std::string> tokens = Tokenize_input(line); !tokens.empty()) {
    // Intercept: If this command has a registered autocompleter script, use it!
    if (const std::string& cmd = tokens.front(); completion_paths.contains(cmd)) {
      return rl_completion_matches(text, custom_path_generator);
    }
  }
  return rl_completion_matches(text, rl_filename_completion_function);
}

static void initialize_readline() {
  rl_attempted_completion_function = shell_completion;
  rl_bind_key('\t', rl_complete);
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
  else if (command == "jobs") {
    if (active_jobs.empty()) {
      return;
    }
    // PHASE 1: Stable printing layout matching POSIX rules
    for (size_t i = 0; i < active_jobs.size(); ++i) {
      std::string current_marker = " ";
      std::string end_marker = " &";

      if (i == active_jobs.size() - 1) {
        current_marker = "+ ";
      } else if (i == active_jobs.size() - 2) {
        current_marker = "- ";
      }

      if (active_jobs[i].status == "Done") {
        end_marker = "";
      }

      std::cout << "[" << active_jobs[i].job_id << "]"
                << current_marker << " "
                << active_jobs[i].status << std::string(17, ' ')
                << active_jobs[i].command
                << end_marker << "\n";

      // Flag that this specific finished job has now been printed to stdout
      if (active_jobs[i].status == "Done") {
        active_jobs[i].reported_done = true;
        std::cout << "yes\n";
      }
    }

    // PHASE 2: Erase elements ONLY if they were processed and printed as Done
    std::erase_if(active_jobs, [](const BackgroundJob& job) {
      return job.reported_done;
    });
  }
}

static void execute_pipeline(const std::vector<CommandStage>& stages) {
  const size_t num_commands = stages.size();
  int in_fd = 0;
  std::vector<pid_t> child_pids;
  for (size_t i{}; i < num_commands; ++i) {
    int pipe_fds[2];

    if (i < num_commands - 1) {
      if(pipe(pipe_fds) < 0) {
        std::cout << "Shell: pipeline allocation failed\n";
        return;
      }
    }

    if (const pid_t pid = fork(); pid == 0) {

      std::signal(SIGINT, SIG_DFL);
      std::signal(SIGTSTP, SIG_DFL);

      // Put the child in its own process group to isolate signals
      setpgid(0, 0);

      if (in_fd != 0) {
        dup2(in_fd, STDIN_FILENO);
        close(in_fd);
      }

      if (i < num_commands - 1) {
        close(pipe_fds[0]);
        dup2(pipe_fds[1], STDOUT_FILENO);
        close(pipe_fds[1]);
      }
      const auto&[args, out_file, err_file, out_redir, err_redir, out_app, err_app] = stages[i];
      if (out_redir) std::freopen(out_file.c_str(), out_app ? "a" : "w", stdout);
      if (err_redir) std::freopen(err_file.c_str(), err_app ? "a" : "w", stderr);

      // 4. Route binary path discovery
      const std::string command = args.front();
      if (std::ranges::find(builtins, command) != builtins.end()) {
        execute_builtin(command, args);
        std::exit(0);
      }
      if (const std::string binary_path = find_command_in_path(command); !binary_path.empty()) {
        std::vector<char*> c_args;
        for (const auto& arg : args) c_args.push_back(const_cast<char*>(arg.c_str()));
        c_args.push_back(nullptr);

        execv(binary_path.c_str(), c_args.data());
      }
      else std::cerr << command << ": command not found\n";
      std::exit(1);
    }
    else if (pid > 0) { // --- PARENT ENGINE ---
      child_pids.push_back(pid);

      // Close resource tracking handles inside the master loop context
      if (in_fd != 0) close(in_fd);
      if (i < num_commands - 1) {
        close(pipe_fds[1]);
        in_fd = pipe_fds[0];
      }
    }
  }
  // Master shell sync lock: Wait for ALL spawned processes to finish
  for (const pid_t pid : child_pids) {
    if (is_job) {
      // Re-assemble command name for readable tracking output
      std::string full_cmd ;
      auto args = stages.front().args;
      for (size_t i{}; i < args.size(); ++i) {
        full_cmd += args[i] + (i + 1 < args.size() ? " " : "");
      }

      active_jobs.push_back({.job_id = next_job_id, .pid = pid, .command = full_cmd});
      std::cout << "[" << next_job_id << "] " << pid << "\n";

      next_job_id++;
      is_job = false; // Reset background state trigger
    }
    else {
      waitpid(pid, nullptr, 0);
    }
  }
}


int main() {
  // Flush after every std::cout / std:cerr
  std::cout << std::unitbuf;
  std::cerr << std::unitbuf;

  initialize_readline();

  setup_shell_signals();

  using_history();

  // TODO: Uncomment the code below to pass the first stage
  //REPL Read-Eval-Print-Loop
  while (true) {
    char* raw_input = readline("$ ");
    update_background_jobs();
    if (raw_input == nullptr) break;

    std::string line(raw_input);
    free(raw_input);

    if (line.find_first_not_of(" \t\n\r") == std::string::npos) continue;
    add_history(line.c_str());

    std::vector<std::string> args = Tokenize_input(line);
    if (args.empty()) continue;

    if (args.back() == "&") {
      is_job = true;
      args.pop_back();
    }

    std::vector<CommandStage> pipeline_stages;
    CommandStage current_stage;

    for (size_t i{}; i < args.size(); ++i) {
      if (args[i] == "|") {
        if (current_stage.args.empty()) {
          std::cout << "Shell: syntax error near unexpected token '|'\n";
          pipeline_stages.clear();
          break;
        }
        pipeline_stages.push_back(current_stage);
        current_stage = CommandStage();
      }
      else {
        current_stage.args.push_back(args[i]);
      }
    }

    if (!current_stage.args.empty()) {
      pipeline_stages.push_back(current_stage);
    }

    if (pipeline_stages.empty()) continue;

    if (args.back() == "|") {
      std::cout << "Shell: syntax error near unexpected token '|'\n";
      continue;
    }

    bool syntax_error = false;
    for (auto& stage : pipeline_stages) {
      if (auto err_it = std::ranges::find_if(stage.args.begin(), stage.args.end(), [](auto& a){ return a == "2>" || a == "2>>"; }); err_it != stage.args.end()) {
        stage.err_app = (*err_it == "2>>");
        stage.err_redir = true;
        if (std::distance(stage.args.begin(), err_it) + 1 < stage.args.size()) {
          if (std::string filename = *(err_it + 1); filename == ">" || filename == ">>" || filename == "1>" || filename == "1>>" || filename == "2>" || filename == "2>>" || filename == "|") {
            std::cout << "Shell: syntax error near unexpected token '" << filename << "'\n";
            syntax_error = true;
            break;
          }
          stage.err_file = *(err_it + 1);
          stage.args.erase(err_it, err_it + 2);
        } else {
          std::cout << "Shell: syntax error near unexpected token 'newline'\n";
          syntax_error = true;
          break;
        }
      }
      if (auto out_it = std::ranges::find_if(stage.args.begin(), stage.args.end(), [](auto& a){ return a == ">" || a == "1>" || a == ">>" || a == "1>>"; }); out_it != stage.args.end()) {
        stage.out_app = (*out_it == ">>" || *out_it == "1>>");
        stage.out_redir = true;
        if (std::distance(stage.args.begin(), out_it) + 1 < stage.args.size()) {
          if (std::string filename = *(out_it + 1); filename == ">" || filename == ">>" || filename == "1>" || filename == "1>>" || filename == "2>" || filename == "2>>" || filename == "|") {
            std::cout << "Shell: syntax error near unexpected token '" << filename << "'\n";
            syntax_error = true;
            break;
          }
          stage.out_file = *(out_it + 1);
          stage.args.erase(out_it, out_it + 2);
        } else {
          std::cout << "Shell: syntax error near unexpected token 'newline'\n";
          syntax_error = true;
          break;
        }
      }
      if (!stage.args.empty()) {
        for (size_t k{1}; k < stage.args.size(); ++k) {
          trim(stage.args[k]);
        }
      }
    }

    if (syntax_error) continue;

    std::string command = pipeline_stages.front().args.front();



    if (command == "exit") break;
    if (command == "cd") {
      auto cd_arg = pipeline_stages.front().args;
      std::string target_path = (cd_arg.size() > 1) ? cd_arg[1] : "";
      bool home_error = false;
      if (target_path.empty() || target_path.starts_with('~')) {
        if (auto home = get_env_var("HOME"); home.has_value()) {
          if (target_path.empty() || target_path == "~") target_path = *home;
          else if (target_path.starts_with("~/")) target_path.replace(0, 1, *home);
        }
        else {
          std::cout << "HOME not set\n";
          home_error = true;
        }
      }

      if (!home_error) {
        try {
          fs::current_path(fs::path(target_path));
        }
        catch (const fs::filesystem_error&) {
          std::cout << "cd: " << (cd_arg.size() > 1? cd_arg[1] : "") << ": No such file or directory" << "\n";
        }
      }
    }
    else if (command == "complete") {
      if (args[1] == "-p") {
        if (args.size() < 3) std::cout << "complete: missing operand \n";
        else {
          if (completion_paths.contains(args[2])) {
            std::cout << "complete -C '" << completion_paths[args[2]] << "' " << args[2] << "\n";
          }
          else {
            std::cout << "complete: " << args[2] << ": no completion specification\n";
          }
        }
      }
      else if (args[1] == "-C") {
        if (args.size() < 4) std::cout << "complete: missing operand \n";
        else {
          completion_paths.insert_or_assign(args[3], args[2]);
        }
      }
      else if (args[1] == "-r") {
        if (args.size() < 3) std::cout << "complete: missing operand \n";
        else {
          if (completion_paths.contains(args[2])) {
            completion_paths.erase(args[2]);
          }
        }
      }
    }
    else {
      execute_pipeline(pipeline_stages);
    }
  }
}
