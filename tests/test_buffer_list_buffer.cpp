#define BOOST_TEST_MODULE BufferListBuffer
#define BOOST_TEST_DYN_LINK
#include <boost/test/unit_test.hpp>

#include "buffer/buffer_list_buffer.hpp"

#include <string>
#include <vector>

BOOST_AUTO_TEST_CASE(is_readonly) {
  BufferListBuffer buf({"*1*"});
  BOOST_CHECK(buf.readonly());
}

BOOST_AUTO_TEST_CASE(selectedName_on_file_lines) {
  std::vector<std::string> names = {"*1*", "*2*"};
  BufferListBuffer buf(names, {}, "*1*");
  // Cursor should be on *1*
  BOOST_CHECK_EQUAL(buf.selectedName(), "*1*");
}

BOOST_AUTO_TEST_CASE(selectedName_empty_on_dir_headers) {
  // Two files in the same directory — produces a dir header as first line.
  std::vector<std::string> names = {"/home/user/a.txt", "/home/user/b.txt"};
  BufferListBuffer buf(names);
  // The cursor starts on the first line, which is the directory header.
  // selectedName should return empty for directory headers.
  BOOST_CHECK(buf.selectedName().empty());
}

BOOST_AUTO_TEST_CASE(refresh_rebuilds) {
  BufferListBuffer buf({"*1*"});
  BOOST_CHECK_EQUAL(buf.selectedName(), "*1*");

  buf.refresh({"*2*", "*3*"}, {}, "*3*");
  BOOST_CHECK_EQUAL(buf.selectedName(), "*3*");
}

BOOST_AUTO_TEST_CASE(startName_positions_cursor) {
  std::vector<std::string> names = {"aaa", "bbb", "ccc"};
  BufferListBuffer buf(names, {}, "bbb");
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

BOOST_AUTO_TEST_CASE(dir_buffer_merges_into_trie) {
  // A DirBuffer for /home/user should appear as a selectable directory
  // header rather than a duplicate leaf entry.
  std::vector<std::string> names = {"/home/user/a.txt", "/home/user/b.txt"};
  std::vector<std::string> dirNames = {"/home/user"};
  BufferListBuffer buf(names, dirNames);

  // First line is the directory header — should now be selectable.
  BOOST_CHECK_EQUAL(buf.selectedName(), "/home/user");
}

BOOST_AUTO_TEST_CASE(dir_buffer_prevents_collapse) {
  // A DirBuffer at an intermediate node should prevent collapsing.
  std::vector<std::string> names = {"/a/b/c/file.txt"};
  std::vector<std::string> dirNames = {"/a/b"};
  BufferListBuffer buf(names, dirNames);

  // /a/b/ should be a separate selectable header (not collapsed with /a/b/c/).
  BOOST_CHECK_EQUAL(buf.selectedName(), "/a/b");
}

BOOST_AUTO_TEST_CASE(standalone_dir_buffer_without_files) {
  // A DirBuffer with no files under it should still appear.
  std::vector<std::string> names = {"*1*"};
  std::vector<std::string> dirNames = {"/home/project"};
  BufferListBuffer buf(names, dirNames);

  // *1* is first (non-path), then the dir buffer.
  BOOST_CHECK_EQUAL(buf.selectedName(), "*1*");
  buf.movePhysicalDown();
  BOOST_CHECK_EQUAL(buf.selectedName(), "/home/project");
}
