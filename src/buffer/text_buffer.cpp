#include "text_buffer.hpp"

#include <fstream>
#include <iterator>

TextBuffer::TextBuffer(std::string name) : Buffer(std::move(name)) {}

TextBuffer::TextBuffer(std::string name, std::filesystem::path const& path)
    : Buffer(std::move(name)), _path(path) {
    std::ifstream file(path, std::ios::binary);
    if (file)
        _data.assign(std::istreambuf_iterator<char>(file),
                      std::istreambuf_iterator<char>());
}
