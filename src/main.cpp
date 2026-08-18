#include <iostream>
#include <string>

int main() {
  // Flush after every std::cout / std:cerr
  std::cout << std::unitbuf;
  std::cerr << std::unitbuf;

  // TODO: Uncomment the code below to pass the first stage
  std::string command;
  //REPL Read-Eval-Print-Loop
  while (true) {
    std::cout << "$ ";
    std::cin >> command;
    if (command == "exit") {
      break;
    }
    std::cout << command << ": command not found\n";
  }
}
