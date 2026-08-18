#include <iostream>
#include <string>
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

  //REPL Read-Eval-Print-Loop
  while (true) {
    std::string command;
    std::string input;

    std::cout << "$ ";
    std::cin >> command;

    std::getline(std::cin, input);
    trim_left(input);
    if (command == "echo") {
      std::cout << input << "\n";
      continue;
    }
    if (command == "exit") {
      break;
    }
    std::cout << command << ": command not found\n";
  }
}
