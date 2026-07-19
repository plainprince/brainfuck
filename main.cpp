#include <unordered_map>
#include <fstream>
#include <string>
#include <vector>
#include <iostream>
#include <cstring>

static void print_help(const char* prog) {
    std::cerr << "Usage: " << prog << " <file.bf>\n";
    std::cerr << "  Simple brainfuck interpreter. Reads file and executes.\n";
}

int main(int argc, char* argv[]) {
    if (argc < 2 || strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "help") == 0) {
        print_help(argc > 0 ? argv[0] : "bfs");
        return 1;
    }

    std::ifstream file(argv[1]);
    if (!file) {
        std::cerr << "error: could not open " << argv[1] << std::endl;
        return 1;
    }

    std::string code(
        (std::istreambuf_iterator<char>(file)),
        std::istreambuf_iterator<char>()
    );

    std::unordered_map<int, unsigned char> tape;
    std::unordered_map<int, int> jumps;
    std::vector<int> stack;

    // Find matching brackets
    for (int i = 0; i < code.size(); i++) {
        if (code[i] == '[') {
            stack.push_back(i);
        } else if (code[i] == ']') {
            if (stack.empty()) {
                std::cerr << "Unmatched ]" << std::endl;
                return 1;
            }

            int start = stack.back();
            stack.pop_back();

            jumps[start] = i;
            jumps[i] = start;
        }
    }

    if (!stack.empty()) {
        std::cerr << "Unmatched [" << std::endl;
        return 1;
    }

    std::cout << std::unitbuf;

    int p = 0;
    int ip = 0;

    while (ip < code.size()) {
        char c = code[ip];

        if (c == '>') {
            p++;
        } else if (c == '<') {
            p--;
        } else if (c == '+') {
            tape[p]++;
        } else if (c == '-') {
            tape[p]--;
        } else if (c == '.') {
            std::cout << static_cast<char>(tape[p]);
        } else if (c == ',') {
            int ch = getchar();
            tape[p] = (ch == EOF) ? 0 : (unsigned char)ch;
        } else if (c == '[' && tape[p] == 0) {
            ip = jumps[ip];
        } else if (c == ']' && tape[p] != 0) {
            ip = jumps[ip];
        }
        ip++;
    }
    
    std::cout << std::endl;
    return 0;
}
