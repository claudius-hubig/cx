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

void TextBuffer::setPath(std::filesystem::path p) {
    _path = std::move(p);
    _name = _path->string();
}

bool TextBuffer::save() {
    if (!_path) return false;
    std::ofstream file(*_path, std::ios::binary);
    if (!file) return false;
    file.write(_data.data(), static_cast<std::streamsize>(_data.size()));
    if (file.good()) {
        _modified = false;
        return true;
    }
    return false;
}
