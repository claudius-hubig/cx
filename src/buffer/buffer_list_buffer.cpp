#include "buffer_list_buffer.hpp"

#include <string>

BufferListBuffer::BufferListBuffer(std::vector<std::string> names, std::size_t startLine)
    : Buffer("*buffers*") {
    refresh(std::move(names), startLine);
}

void BufferListBuffer::refresh(std::vector<std::string> names, std::size_t startLine) {
    _names = std::move(names);
    _data.clear();
    for (std::size_t i = 0; i < _names.size(); ++i) {
        for (char c : _names[i])
            _data.push_back(c);
        if (i + 1 < _names.size())
            _data.push_back('\n');
    }
    // Place point at the start of startLine.
    _point = 0;
    for (std::size_t line = 0; line < startLine && _point < _data.size(); ++line) {
        while (_point < _data.size() && _data[_point] != '\n')
            ++_point;
        if (_point < _data.size())
            ++_point;
    }
    _viewFrom = 0;
    _desiredCol.reset();
}

std::string BufferListBuffer::selectedName() const {
    // Find which line the cursor is on.
    std::size_t line = 0;
    for (std::size_t i = 0; i < _point && i < _data.size(); ++i)
        if (_data[i] == '\n') ++line;
    if (line < _names.size())
        return _names[line];
    return {};
}

std::tuple<RowIdx, ColIdx> BufferListBuffer::render(
        std::string& out, RowIdx rows, ColIdx cols) const {

    static constexpr auto kHighlight = "\x1b[7m"; // reverse video
    static constexpr auto kReset     = "\x1b[0m";

    std::size_t c = cols.value;

    // Find which line the cursor is on.
    std::size_t curLine = 0;
    for (std::size_t i = 0; i < _point && i < _data.size(); ++i)
        if (_data[i] == '\n') ++curLine;

    // Count how many lines _viewFrom has scrolled past.
    std::size_t scrollLine = 0;
    for (std::size_t i = 0; i < _viewFrom && i < _data.size(); ++i)
        if (_data[i] == '\n') ++scrollLine;

    for (std::size_t r = 0; r < rows.value; ++r) {
        std::size_t lineIdx = scrollLine + r;
        out += "\x1b[";
        out += std::to_string(r + 1);
        out += ";1H";

        if (lineIdx < _names.size()) {
            bool highlight = (lineIdx == curLine);
            if (highlight) out += kHighlight;
            out += _names[lineIdx];
            std::size_t nameLen = _names[lineIdx].size();
            if (nameLen < c)
                out.append(c - nameLen, ' ');
            if (highlight) out += kReset;
        } else {
            out.append(c, ' ');
        }
    }

    std::size_t curScreenRow = curLine >= scrollLine ? curLine - scrollLine : 0;
    return {RowIdx{curScreenRow}, ColIdx{0}};
}
