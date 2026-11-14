// Reproducer for an open finding: typing with a cursor at every occurrence on
// one long line is quadratic in the number of cursors.
// usage: bench_multicursor_line   (release library)
#include "buffer/buffer_view.hpp"
#include "buffer/text_buffer.hpp"
#include <chrono>
#include <cstdio>
int main() {
  for (std::size_t n : {2000u, 10000u, 40000u, 160000u}) {
    std::string s;
    for (std::size_t i = 0; i < n; ++i)
      s += "{\"id\":1},";
    TextBuffer b("t");
    b.insertText(s, BufferPoint{0, 0});
    BufferView v(b);
    v.setPoint(0, 3);
    auto t0 = std::chrono::steady_clock::now();
    auto c = v.selectAllOccurrences();
    auto t1 = std::chrono::steady_clock::now();
    v.insert('x');
    auto t2 = std::chrono::steady_clock::now();
    v.deleteBeforePoint();
    auto t3 = std::chrono::steady_clock::now();
    auto ms = [](auto a, auto b) {
      return std::chrono::duration<double, std::milli>(b - a).count();
    };
    std::printf("line %7zu bytes, %6zu cursors: select %6.0f ms, type one char %7.0f ms, backspace "
                "%7.0f ms\n",
                s.size(), c, ms(t0, t1), ms(t1, t2), ms(t2, t3));
  }
}
