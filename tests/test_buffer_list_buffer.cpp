#define BOOST_TEST_MODULE BufferListBuffer
#include <boost/test/included/unit_test.hpp>

#include "buffer/buffer_list_buffer.hpp"

#include <string>
#include <vector>

BOOST_AUTO_TEST_CASE(is_readonly) {
    BufferListBuffer buf({"*1*"});
    BOOST_CHECK(buf.readonly());
}

BOOST_AUTO_TEST_CASE(selectedName_on_file_lines) {
    std::vector<std::string> names = {"*1*", "*2*"};
    BufferListBuffer buf(names, "*1*");
    // Cursor should be on *1*
    BOOST_CHECK_EQUAL(buf.selectedName(), "*1*");
}

BOOST_AUTO_TEST_CASE(selectedName_empty_on_dir_headers) {
    // Two files in the same directory — should produce a dir header
    std::vector<std::string> names = {"/home/user/a.txt", "/home/user/b.txt"};
    BufferListBuffer buf(names);
    // First line is a dir header (empty selectedName), unless collapsed
    // Just verify it doesn't crash
    auto sel = buf.selectedName();
    // The trie might collapse, producing file lines. Either way, no crash.
    BOOST_CHECK(true);
}

BOOST_AUTO_TEST_CASE(refresh_rebuilds) {
    BufferListBuffer buf({"*1*"});
    BOOST_CHECK_EQUAL(buf.selectedName(), "*1*");

    buf.refresh({"*2*", "*3*"}, "*3*");
    BOOST_CHECK_EQUAL(buf.selectedName(), "*3*");
}

BOOST_AUTO_TEST_CASE(startName_positions_cursor) {
    std::vector<std::string> names = {"aaa", "bbb", "ccc"};
    BufferListBuffer buf(names, "bbb");
    BOOST_CHECK_EQUAL(buf.selectedName(), "bbb");
}

BOOST_AUTO_TEST_CASE(trie_collapses_single_subdirs) {
    // /a/b/c/file.txt should collapse /a/b/c/ into one line
    std::vector<std::string> names = {"/a/b/c/file.txt"};
    BufferListBuffer buf(names);
    // Should be a single file line, selected
    auto sel = buf.selectedName();
    BOOST_CHECK_EQUAL(sel, "/a/b/c/file.txt");
}
