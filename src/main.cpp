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

const std::vector<std::string> builtins = {"echo", "exit", "type", "pwd", "cd", "complete", "jobs", "history", "declare"};
static std::unordered_map<std::string, std::string> completion_paths;
static std::vector<std::string> complete_args;
static std::unordered_map<std::string, std::string> declared_variables;
namespace {
  struct BackgroundJob{
    int job_id;
    std::vector<pid_t> pids;
    std::string command;
    std::string status = "Running";
    bool reported_done = false;
  };
}

static bool is_job = false;
static int n_append{};

static std::vector<BackgroundJob> active_jobs;
static int next_job_id = 1;

// Cleans up background processes that have naturally terminated
static void update_background_jobs() {
  for (auto& job : active_jobs) {
    if (job.status != "Running") continue;

    // A pipelined background job isn't "Done" until every stage has exited.
    bool all_finished = true;
    bool any_signaled = false;
    for (const pid_t pid : job.pids) {
      int status;
      const pid_t result = waitpid(pid, &status, WNOHANG);
      if (result == 0) {
        // Still running.
        all_finished = false;
      } else if (result > 0 && WIFSIGNALED(status)) {
        any_signaled = true;
      }
      // result < 0 (ECHILD) means this pid was already reaped earlier;
      // treat it as finished rather than blocking the whole job forever.
    }

    if (all_finished) {
      job.status = any_signaled ? "Terminated" : "Done";
    }
  }
}

static void reap_jobs() {
  if (active_jobs.empty()) {
    return;
  }
  update_background_jobs();
  // PHASE 1: Stable printing layout matching POSIX rules
  for (size_t i = 0; i < active_jobs.size(); ++i) {
    std::string current_marker = " ";

    if (i == active_jobs.size() - 1) {
      current_marker = "+ ";
    } else if (i == active_jobs.size() - 2) {
      current_marker = "- ";
    }

    if (active_jobs[i].status == "Done") {
      std::cout << "[" << active_jobs[i].job_id << "]"
              << current_marker << " "
              << "Done" << std::string(17, ' ')
              << active_jobs[i].command
              <<"\n";

      // Flag that this specific finished job has now been printed to stdout
      if (active_jobs[i].status == "Done") {
        active_jobs[i].reported_done = true;
      }
    }
  }
  // PHASE 2: Erase elements ONLY if they were processed and printed as Done
  std::erase_if(active_jobs, [](const BackgroundJob& job) {
    return job.reported_done;
  });
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
  std::vector<std::string> args_;
  std::string current_token;
  std::string d_var;

  bool in_single_quote = false;
  bool in_double_quote = false;
  bool escape = false;
  bool token_has_content = false;
  bool is_variable = false;


  for (size_t i{}; i < input_line.size(); ++i) {
    const char ch = input_line[i];

    if (ch == '$' && !escape) {
      if (i + 1 < input_line.size() && std::isalpha(input_line[i + 1])) {
        is_variable = true;
        continue;
      }
    }

    if (is_variable) {
      if (ch == ' ') {
        if (declared_variables.contains(d_var)) {
          trim(d_var);
          std::cout << d_var << " -->  : ";
          d_var = declared_variables[d_var];
          current_token = d_var;
          args_.push_back(current_token);
          current_token.clear();
          d_var.clear();
          token_has_content = false;
          is_variable = false;
          continue;
        }
      }
      d_var.push_back(ch);
    }

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
        args_.push_back(current_token);
        current_token.clear();
        token_has_content = false;
      }
      continue;
    }

    if (!in_single_quote && !in_double_quote) {
      if (ch == '>' || ch == '|' ||
         ((ch == '1' || ch == '2') && i + 1 < input_line.size() && input_line[i + 1] == '>')) {

        if (!current_token.empty() || token_has_content) {
          args_.push_back(current_token);
          current_token.clear();
          token_has_content = false;
        }

        if (ch == '|') {
          args_.emplace_back("|");
        }
        else if (ch == '>') {
          if (i + 1 < input_line.size() && input_line[i + 1] == '>') { args_.emplace_back(">>"); i++; }
          else args_.emplace_back(">");
        }
        else if (ch == '1') {
          if (i + 2 < input_line.size() && input_line[i + 1] == '>' && input_line[i + 2] == '>') { args_.emplace_back("1>>"); i += 2; }
          else { args_.emplace_back("1>"); i++; }
        }
        else if (ch == '2') {
          if (i + 2 < input_line.size() && input_line[i + 1] == '>' && input_line[i + 2] == '>') { args_.emplace_back("2>>"); i += 2; }
          else { args_.emplace_back("2>"); i++; }
        }
        continue;
         }
    }

    // Normal character
    current_token.push_back(ch);
    token_has_content = true;
  }

  if (!current_token.empty() || token_has_content) {
    args_.push_back(current_token);
  }
  return args_;
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

static void hist_append(const char* file) {
  const auto filename = file;
  const int pos = where_history();
  const int n = pos - n_append + 1;
  n_append = n;
  append_history(n, filename);
}

static void execute_builtin(const std::string& command, const std::vector<std::string>& args_) {

  if (command == "echo") {
    for (size_t i{1}; i < args_.size(); ++i) {
      std::cout << args_[i] << (i + 1 < args_.size() ? " " : "");
    }
    std::cout << "\n";
  }
  else if (command == "type") {
    if (args_.size() < 2) std::cout << "type: missing operand \n";
    else {
      const std::string& target = args_[1];
      if (const auto it = std::ranges::find(builtins, target); it != builtins.end()) std::cout << *it << " is a shell builtin\n";
      else if (const std::string path = find_command_in_path(target); !path.empty()) std::cout << target << " is " << path << "\n";
      else std::cout << target << ": not found\n";
    }
  }
  else if (command == "pwd") {
      std::cout << fs::current_path().string() << "\n";
  }
  else if (command == "history") {
    if (args_.size() == 1) {
      history_set_pos(0);
      while (true) {
        if (current_history() == nullptr) break;
        const int pos = where_history();
        std::cout << pos + 1 << " " << current_history()->line << std::endl;
        next_history();
      }
    }
    else{
      if (std::ranges::all_of(args_[1].begin(), args_[1].end(), [](const char& a){return std::isdigit(a);})) {
        if ( args_.size() > 2) {
          std::cout <<"Shell: history: too many arguments\n";
          return;
        }
        if (const int start = std::stoi(args_[1]); start == 0) {
          // Do nothing
        }
        else {
          history_set_pos(where_history());
          for (size_t i{}; i < start - 1; ++i) {
            if (current_history() == nullptr) break;
            previous_history();
          }
          while (true) {
            if (current_history() == nullptr) break;
            const int pos = where_history();
            std::cout << pos + 1 << " " << current_history()->line << std::endl;
            next_history();
          }
        }
      }
      else if (args_[1] == "-r") {
        if ( args_.size() > 3) {
          std::cout <<"Shell: history: too many arguments\n";
          return;
        }
        const char* filename = args_[2].c_str();
        read_history(filename);
      }
      else if (args_[1] == "-w") {
        if ( args_.size() > 3) {
          std::cout <<"Shell: history: too many arguments\n";
          return;
        }
        const char* filename = args_[2].c_str();
        write_history(filename);
      }
      else if (args_[1] == "-a") {
        if ( args_.size() > 3) {
          std::cout <<"Shell: history: too many arguments\n";
          return;
        }
        hist_append(args_[2].c_str());
      }
      else {
        std::cout << "Shell: " << args_[1] << ": numeric argument required\n";
      }
    }
  }
  else if (command == "declare") {
    if (args_.size() == 1) {
      // No implementation
    }
    else if (args_[1] == "-p") {
      if (args_.size() > 2) {
        for (size_t i{2}; i < args_.size(); ++i) {
          if (declared_variables.contains(args_[i])) {
            std::cout << "declare -- " << args_[i] << "=\"" <<declared_variables[args_[i]] << "\"" << "\n";
          }
          else {
            std::cout << "declare: " << args_[i] << ": not found\n";
          }
        }
      }
    }
    else{
      for (int i{1}; i < args_.size(); ++i) {
        if (const bool has_hyphen = std::ranges::any_of(args_[i], [](const unsigned char c){return c == '-';}); (isalpha(args_[i].front()) || args_[i].front() == '_') && !has_hyphen) {
          if (const auto it = args_[i].find('='); it != std::string::npos) {
            const std::string param = args_[i].substr(0, it);
            std::string value = args_[i].substr(it + 1);
            declared_variables.insert_or_assign(param, value);
          }
          else {
            declared_variables.insert_or_assign(args_[i], args_[i]);
          }
        }
        else {
          std::cout << "declare: `" << args_[i] << "': not a valid identifier\n";
        }
      }
    }
  }
  else if (command == "jobs") {
    if (active_jobs.empty()) {
      return;
    }
    update_background_jobs();
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
      const auto&[args_, out_file, err_file, out_redir, err_redir, out_app, err_app] = stages[i];
      if (out_redir) std::freopen(out_file.c_str(), out_app ? "a" : "w", stdout);
      if (err_redir) std::freopen(err_file.c_str(), err_app ? "a" : "w", stderr);

      // 4. Route binary path discovery
      const std::string command = args_.front();
      if (std::ranges::find(builtins, command) != builtins.end()) {
        execute_builtin(command, args_);
        std::exit(0);
      }
      if (const std::string binary_path = find_command_in_path(command); !binary_path.empty()) {
        std::vector<char*> c_args;
        for (const auto& arg : args_) c_args.push_back(const_cast<char*>(arg.c_str()));
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
  if (is_job) {
    std::string full_cmd;
    for (size_t i{}; i < complete_args.size(); ++i) {
      full_cmd += complete_args[i] + (i + 1 < complete_args.size() ? " " : "");
    }

    if (active_jobs.empty()) {
      next_job_id = 1;
    }
    else {
      const auto max_it = std::ranges::max_element(active_jobs, [](const BackgroundJob& a, const BackgroundJob& b) {
        return a.job_id < b.job_id;
      });
      next_job_id = max_it->job_id + 1;
    }

    active_jobs.push_back({.job_id = next_job_id, .pids = child_pids, .command = full_cmd});
    std::cout << "[" << next_job_id << "] " << child_pids.back() << "\n";

    next_job_id++;
    is_job = false; // Reset background state trigger
  }
  else {
    // Foreground: block until every stage in the pipeline finishes.
    for (const pid_t pid : child_pids) {
      waitpid(pid, nullptr, 0);
    }
    reap_jobs();
  }
}


int main() {
  // Flush after every std::cout / std:cerr
  std::cout << std::unitbuf;
  std::cerr << std::unitbuf;

  initialize_readline();

  setup_shell_signals();

  using_history();

  auto hist_file_opt = get_env_var("HISTFILE");
  if (hist_file_opt.has_value()) {
    read_history(hist_file_opt->c_str());
  }


  // TODO: Uncomment the code below to pass the first stage
  //REPL Read-Eval-Print-Loop
  while (true) {
    char* raw_input = readline("$ ");
    if (raw_input == nullptr) break;

    std::string line(raw_input);
    free(raw_input);

    if (line.find_first_not_of(" \t\n\r") == std::string::npos) continue;
    add_history(line.c_str());

    complete_args = Tokenize_input(line);
    if (complete_args.empty()) {
      reap_jobs();
      continue;
    }

    // Bash treats '&' as a command SEPARATOR/TERMINATOR, not something
    // that only matters when it's the very last token on the line. Split
    // the line into segments at every top-level '&': each segment before
    // an '&' backgrounds independently, and a trailing segment with no
    // '&' after it runs in the foreground as usual.
    struct LineSegment {
      std::vector<std::string> tokens;
      bool is_in_background;
    };
    std::vector<LineSegment> segments;
    {
      std::vector<std::string> current_segment;
      for (const auto& token : complete_args) {
        if (token == "&") {
          segments.push_back({.tokens = current_segment, .is_in_background = true});
          current_segment.clear();
        } else {
          current_segment.push_back(token);
        }
      }
      // Anything left after the last '&' (or the whole line, if there was
      // no '&' at all) runs in the foreground.
      if (!current_segment.empty() || segments.empty()) {
        segments.push_back({current_segment, false});
      }
    }

    bool should_exit_shell = false;

    // validate every segment (pipe structure, redirection syntax)
    // before executing ANY of them. Real bash parses the whole line first;
    // a syntax error anywhere means nothing on the line runs at all -- so
    // "echo hi & &" must not background "echo hi" just because the error
    // is in the second segment.
    struct ValidatedSegment {
      std::vector<std::string> raw_tokens;
      std::vector<CommandStage> pipeline_stages;
      bool is_in_background;
    };
    std::vector<ValidatedSegment> validated;
    bool line_has_error = false;

    for (auto& segment : segments) {
      if (segment.tokens.empty()) {
        // e.g. "cmd &  & cmd2" or a bare leading "&" -- nothing to background.
        std::cout << "Shell: syntax error near unexpected token '&'\n";
        line_has_error = true;
        break;
      }

      std::vector<CommandStage> pipeline_stages;
      CommandStage current_stage;
      bool abort = false;

      for (size_t i{}; i < segment.tokens.size(); ++i) {
        if (segment.tokens[i] == "|") {
          if (current_stage.args.empty()) {
            std::cout << "Shell: syntax error near unexpected token '|'\n";
            abort = true;
            break;
          }
          pipeline_stages.push_back(current_stage);
          current_stage = CommandStage();
        }
        else {
          current_stage.args.push_back(segment.tokens[i]);
        }
      }

      if (abort) { line_has_error = true; break; }

      if (!current_stage.args.empty()) {
        pipeline_stages.push_back(current_stage);
      }

      if (pipeline_stages.empty()) { line_has_error = true; break; }

      if (segment.tokens.back() == "|") {
        std::cout << "Shell: syntax error near unexpected token '|'\n";
        line_has_error = true;
        break;
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
          trim(stage.args[0]);
          // TODO: Expanding variables
        }
      }

      if (syntax_error) { line_has_error = true; break; }

      validated.push_back({segment.tokens, std::move(pipeline_stages), segment.is_in_background});
    }

    if (line_has_error) continue; // whole line rejected -- nothing executes, matching bash

    // PASS 2: the line parsed cleanly end to end, so now actually run
    // each segment in order.
    for (auto& validated_segment : validated) {
      complete_args = validated_segment.raw_tokens;
      is_job = validated_segment.is_in_background;
      std::vector<CommandStage>& pipeline_stages = validated_segment.pipeline_stages;

      std::string command = pipeline_stages.front().args.front();

      if (command == "exit") {
        if (hist_file_opt.has_value()) {
          write_history(hist_file_opt->c_str());
        }
        should_exit_shell = true;
        break;
      }
      if (command == "declare" && pipeline_stages.size() == 1 &&
          !pipeline_stages.front().out_redir && !pipeline_stages.front().err_redir) {
        execute_builtin(command, pipeline_stages.front().args);
        continue;
      }
      if (command == "jobs" && pipeline_stages.size() == 1 &&
          !pipeline_stages.front().out_redir && !pipeline_stages.front().err_redir) {
        execute_builtin(command, pipeline_stages.front().args);
        continue;
      }
      if (command == "history" && pipeline_stages.size() == 1 &&
          !pipeline_stages.front().out_redir && !pipeline_stages.front().err_redir) {
        execute_builtin(command, pipeline_stages.front().args);
        continue;
          }
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
        if (complete_args[1] == "-p") {
          if (complete_args.size() < 3) std::cout << "complete: missing operand \n";
          else {
            if (completion_paths.contains(complete_args[2])) {
              std::cout << "complete -C '" << completion_paths[complete_args[2]] << "' " << complete_args[2] << "\n";
            }
            else {
              std::cout << "complete: " << complete_args[2] << ": no completion specification\n";
            }
          }
        }
        else if (complete_args[1] == "-C") {
          if (complete_args.size() < 4) std::cout << "complete: missing operand \n";
          else {
            completion_paths.insert_or_assign(complete_args[3], complete_args[2]);
          }
        }
        else if (complete_args[1] == "-r") {
          if (complete_args.size() < 3) std::cout << "complete: missing operand \n";
          else {
            if (completion_paths.contains(complete_args[2])) {
              completion_paths.erase(complete_args[2]);
            }
          }
        }
      }
      else {
        execute_pipeline(pipeline_stages);
      }
    }

    if (should_exit_shell) break;
  }
}