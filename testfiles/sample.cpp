// sample.cpp — C/C++ mode manual highlight check
/* A block comment
   spanning multiple lines */
#include <vector>
#include <string>
#define MAX_LEN 256

template <typename T>
struct Box {
  T value;
};

int main() {
  int count = 42;
  double ratio = 3.14e-2;
  unsigned long big = 1'000'000UL;
  char ch = 'x';
  char esc = '\n';
  std::string s = "a \"quoted\" string with \\ backslash";
  auto raw = R"delim(raw string: no \n escapes, )" not closed yet)delim";
  Box<int> b{count};

  for (int i = 0; i < count; ++i) {
    if (i % 2 == 0 && ratio > 0) {
      s += "even";
    } else {
      s += "odd";
    }
  }
  return (big > MAX_LEN) ? 0 : 1;
}
