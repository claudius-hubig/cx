#define BOOST_TEST_MODULE TextBuffer
#define BOOST_TEST_DYN_LINK
#include <boost/test/unit_test.hpp>

#include "buffer/text_buffer.hpp"
#include "tests/test_helpers.hpp"

#include <fstream>
#include <string>

// --- Constructor ---

BOOST_AUTO_TEST_CASE(name_only_no_path) {
    TextBuffer buf("scratch");
    BOOST_CHECK_EQUAL(buf.name(), "scratch");
    BOOST_CHECK(!buf.hasPath());
    BOOST_CHECK(!buf.modified());
    BOOST_CHECK_EQUAL(buf.point(), 0u);
}

BOOST_AUTO_TEST_CASE(from_file) {
    TempFile tmp("hello world");
    TextBuffer buf(tmp.path().string(), tmp.path());
    BOOST_CHECK(buf.hasPath());
    BOOST_CHECK(!buf.modified());
    // Data should match file contents — check via lineCol after moveEnd.
    buf.moveEnd();
    auto [line, col] = buf.lineCol();
    BOOST_CHECK_EQUAL(line, 1u);
    BOOST_CHECK_EQUAL(col, 12u); // "hello world" = 11 chars, 1-indexed
}

BOOST_AUTO_TEST_CASE(from_missing_file) {
    std::filesystem::path missing = "/tmp/cx_test_nonexistent_file_12345";
    TextBuffer buf(missing.string(), missing);
    BOOST_CHECK(buf.hasPath());
    BOOST_CHECK(!buf.modified());
    BOOST_CHECK_EQUAL(buf.point(), 0u);
    // Buffer should be empty
    auto [line, col] = buf.lineCol();
    BOOST_CHECK_EQUAL(line, 1u);
    BOOST_CHECK_EQUAL(col, 1u);
}

// --- Save ---

BOOST_AUTO_TEST_CASE(save_no_path_returns_false) {
    TextBuffer buf("scratch");
    buf.insert('x');
    BOOST_CHECK(!buf.save());
    BOOST_CHECK(buf.modified()); // still modified
}

BOOST_AUTO_TEST_CASE(save_writes_and_clears_modified) {
    TempFile tmp;
    TextBuffer buf(tmp.path().string(), tmp.path());
    buf.insert('A');
    buf.insert('B');
    BOOST_CHECK(buf.modified());
    BOOST_CHECK(buf.save());
    BOOST_CHECK(!buf.modified());

    // Verify file content
    std::ifstream f(tmp.path(), std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
    BOOST_CHECK_EQUAL(content, "AB");
}

BOOST_AUTO_TEST_CASE(save_truncates_existing) {
    TempFile tmp("old content that is longer");
    TextBuffer buf(tmp.path().string(), tmp.path());
    // Delete all characters by backspacing from the end.
    buf.moveEnd();
    while (buf.point() > 0)
        buf.deleteBeforePoint();
    buf.insert('X');
    BOOST_CHECK(buf.save());

    std::ifstream f(tmp.path(), std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
    BOOST_CHECK_EQUAL(content, "X");
}

// --- SetPath ---

BOOST_AUTO_TEST_CASE(setPath_updates_name_and_hasPath) {
    TextBuffer buf("scratch");
    BOOST_CHECK(!buf.hasPath());
    std::filesystem::path p = "/tmp/cx_test_setpath";
    buf.setPath(p);
    BOOST_CHECK(buf.hasPath());
    BOOST_CHECK_EQUAL(buf.name(), p.string());
}
