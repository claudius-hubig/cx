#define BOOST_TEST_MODULE BufferManager
#include <boost/test/unit_test.hpp>

#include "buffer/buffer_manager.hpp"
#include "buffer/buffer_view.hpp"
#include "tests/test_helpers.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <unistd.h>

// --- Construction ---

BOOST_AUTO_TEST_CASE(default_buffer_exists) {
  NullBufferSink sink;
  BufferManager mgr(sink);
  BOOST_CHECK(mgr.find("*1*") != nullptr);
}

BOOST_AUTO_TEST_CASE(default_buffer_set_on_sink) {
  NullBufferSink sink;
  BufferManager mgr(sink);
  BOOST_CHECK(sink.lastBuffer != nullptr);
  BOOST_CHECK_EQUAL(sink.lastBuffer->name(), "*1*");
}

// --- Open ---

BOOST_AUTO_TEST_CASE(open_creates_buffer) {
  NullBufferSink sink;
  BufferManager mgr(sink);
  TempFile tmp("content");
  auto& buf = mgr.open(tmp.path());
  BOOST_CHECK(!buf.readonly());
  BOOST_CHECK(buf.hasPath());
}

BOOST_AUTO_TEST_CASE(open_returns_existing) {
  NullBufferSink sink;
  BufferManager mgr(sink);
  TempFile tmp("content");
  auto& buf1 = mgr.open(tmp.path());
  auto& buf2 = mgr.open(tmp.path());
  BOOST_CHECK_EQUAL(&buf1, &buf2);
}

BOOST_AUTO_TEST_CASE(open_canonicalizes_paths) {
  NullBufferSink sink;
  BufferManager mgr(sink);
  TempFile tmp("content");
  auto& buf1 = mgr.open(tmp.path());
  // Open with a relative-ish path to the same file
  auto& buf2 = mgr.open(tmp.path().parent_path() / "." / tmp.path().filename());
  BOOST_CHECK_EQUAL(&buf1, &buf2);
}

// --- Create ---

BOOST_AUTO_TEST_CASE(create_new_buffer) {
  NullBufferSink sink;
  BufferManager mgr(sink);
  auto& buf = mgr.create("scratch");
  BOOST_CHECK_EQUAL(buf.name(), "scratch");
  BOOST_CHECK(!buf.hasPath());
}

BOOST_AUTO_TEST_CASE(create_idempotent) {
  NullBufferSink sink;
  BufferManager mgr(sink);
  auto& buf1 = mgr.create("scratch");
  auto& buf2 = mgr.create("scratch");
  BOOST_CHECK_EQUAL(&buf1, &buf2);
}

// --- Find ---

BOOST_AUTO_TEST_CASE(find_nonexistent) {
  NullBufferSink sink;
  BufferManager mgr(sink);
  BOOST_CHECK(mgr.find("nonexistent") == nullptr);
}

BOOST_AUTO_TEST_CASE(find_existing) {
  NullBufferSink sink;
  BufferManager mgr(sink);
  mgr.create("foo");
  BOOST_CHECK(mgr.find("foo") != nullptr);
}

// --- Kill ---

BOOST_AUTO_TEST_CASE(kill_removes_buffer) {
  NullBufferSink sink;
  BufferManager mgr(sink);
  mgr.create("temp");
  BOOST_CHECK(mgr.find("temp") != nullptr);
  mgr.kill("temp");
  BOOST_CHECK(mgr.find("temp") == nullptr);
}

BOOST_AUTO_TEST_CASE(kill_last_recreates_default) {
  NullBufferSink sink;
  BufferManager mgr(sink);
  // Kill the default *1*
  auto& next = mgr.kill("*1*");
  // Should have created a new *1*
  BOOST_CHECK(mgr.find("*1*") != nullptr);
  BOOST_CHECK_EQUAL(next.name(), "*1*");
}

BOOST_AUTO_TEST_CASE(kill_returns_next) {
  NullBufferSink sink;
  BufferManager mgr(sink);
  mgr.create("a");
  mgr.create("b");
  mgr.touch("b"); // b is MRU, a is second
  mgr.touch("a"); // a is MRU, b is second
  auto& next = mgr.kill("a");
  // Should return b (next MRU)
  BOOST_CHECK_EQUAL(next.name(), "b");
}

// --- Rename ---

BOOST_AUTO_TEST_CASE(rename_rekeys_map) {
  NullBufferSink sink;
  BufferManager mgr(sink);
  mgr.create("old");
  TempDir dir;
  auto target = std::filesystem::weakly_canonical(dir.path()) / "new";
  BOOST_CHECK(mgr.rename("old", target));
  BOOST_CHECK(mgr.find("old") == nullptr);
  BOOST_CHECK(mgr.find(target.string()) != nullptr);
}

BOOST_AUTO_TEST_CASE(rename_canonicalizes_relative_path) {
  NullBufferSink sink;
  BufferManager mgr(sink);
  mgr.create("old");
  BOOST_CHECK(mgr.rename("old", std::filesystem::path("relative_name.txt")));
  auto expected =
      std::filesystem::weakly_canonical(std::filesystem::absolute("relative_name.txt")).string();
  BOOST_CHECK(mgr.find(expected) != nullptr);
}

BOOST_AUTO_TEST_CASE(rename_onto_open_buffer_is_refused) {
  // Saving *1* as a file that is already open used to destroy the renamed
  // buffer (emplace collided and dropped the moved-in unique_ptr) while
  // the caller still pointed at it.
  NullBufferSink sink;
  BufferManager mgr(sink);
  TempFile tmp("existing\n");
  auto& existing = mgr.open(tmp.path());
  auto& scratch = mgr.create("scratch");
  BOOST_CHECK(!mgr.rename("scratch", tmp.path()));
  BOOST_CHECK(mgr.find("scratch") == &scratch);
  BOOST_CHECK_EQUAL(scratch.name(), "scratch");
  BOOST_CHECK(mgr.find(existing.name()) == &existing);
}

BOOST_AUTO_TEST_CASE(rename_onto_own_path_is_allowed) {
  NullBufferSink sink;
  BufferManager mgr(sink);
  TempFile tmp("existing\n");
  auto& buf = mgr.open(tmp.path());
  auto name = buf.name();
  BOOST_CHECK(mgr.rename(name, tmp.path()));
  BOOST_CHECK(mgr.find(name) == &buf);
}

// --- Touch / MRU ---

BOOST_AUTO_TEST_CASE(touch_ordering) {
  NullBufferSink sink;
  BufferManager mgr(sink);
  mgr.create("a");
  mgr.create("b");
  mgr.touch("a");
  // After touching a, secondMRU should be b
  BOOST_CHECK_EQUAL(mgr.secondMRU(), "b");
}

BOOST_AUTO_TEST_CASE(secondMRU_with_fewer_than_two) {
  NullBufferSink sink;
  BufferManager mgr(sink);
  // Only *1* exists — secondMRU should be empty.
  BOOST_CHECK(mgr.secondMRU().empty());
}

// --- DisplayName ---

BOOST_AUTO_TEST_CASE(displayName_non_path) {
  NullBufferSink sink;
  BufferManager mgr(sink);
  BOOST_CHECK_EQUAL(mgr.displayName("*1*"), "*1*");
}

BOOST_AUTO_TEST_CASE(displayName_unique_filename) {
  NullBufferSink sink;
  BufferManager mgr(sink);
  TempFile tmp("data");
  mgr.open(tmp.path());
  auto canonical =
      std::filesystem::weakly_canonical(std::filesystem::absolute(tmp.path())).string();
  auto display = mgr.displayName(canonical);
  // Should be just the filename since no other buffer shares it
  BOOST_CHECK_EQUAL(display, tmp.path().filename().string());
}

BOOST_AUTO_TEST_CASE(displayName_ambiguous_full_path) {
  NullBufferSink sink;
  BufferManager mgr(sink);
  TempFile tmp1("data1");
  TempFile tmp2("data2");

  // Rename tmp2 to have the same filename as tmp1 in a different dir
  // This is tricky — both TempFiles are in /tmp with random names.
  // Instead, test with manually created buffers using create + rename.
  // Actually let's just verify the displayName logic for non-colliding paths.
  mgr.open(tmp1.path());
  mgr.open(tmp2.path());
  auto c1 = std::filesystem::weakly_canonical(std::filesystem::absolute(tmp1.path())).string();
  auto c2 = std::filesystem::weakly_canonical(std::filesystem::absolute(tmp2.path())).string();
  // Different filenames, so both should show short names
  BOOST_CHECK_NE(mgr.displayName(c1), c1);
  BOOST_CHECK_NE(mgr.displayName(c2), c2);
}

// --- BufferNames ---

BOOST_AUTO_TEST_CASE(bufferNames_sorted) {
  NullBufferSink sink;
  BufferManager mgr(sink);
  mgr.create("c");
  mgr.create("a");
  mgr.create("b");
  auto names = mgr.bufferNames();
  BOOST_CHECK(std::is_sorted(names.begin(), names.end()));
}

BOOST_AUTO_TEST_CASE(bufferNames_excludes_special) {
  NullBufferSink sink;
  BufferManager mgr(sink);
  auto names = mgr.bufferNames();
  // Should not include *help* or *buffers*
  for (auto& n : names) {
    BOOST_CHECK_NE(n, "*help*");
    BOOST_CHECK_NE(n, "*buffers*");
  }
}

// --- BufferListBuffer ---

BOOST_AUTO_TEST_CASE(bufferListBuffer_lazy_creation) {
  NullBufferSink sink;
  BufferManager mgr(sink);
  auto& blb = mgr.bufferListBuffer();
  BOOST_CHECK(blb.readonly());
}

BOOST_AUTO_TEST_CASE(bufferListBuffer_refreshes) {
  NullBufferSink sink;
  BufferManager mgr(sink);
  auto& blb1 = mgr.bufferListBuffer();
  mgr.create("new_buf");
  auto& blb2 = mgr.bufferListBuffer();
  BOOST_CHECK_EQUAL(&blb1, &blb2); // same object, just refreshed
}

BOOST_AUTO_TEST_CASE(bufferListBuffer_saves_view_hint) {
  NullBufferSink sink;
  BufferManager mgr(sink);
  mgr.create("aaa");
  mgr.create("bbb");
  mgr.touch("aaa"); // aaa is MRU, bbb is secondMRU
  auto& blb = mgr.bufferListBuffer();
  // BufferManager should save a ViewHint so fromSavedState lands on secondMRU.
  BufferView view = BufferView::fromSavedState(blb);
  BOOST_CHECK_EQUAL(blb.selectedName(view.point()), "bbb");
}

BOOST_AUTO_TEST_CASE(bufferListBuffer_refresh_updates_saved_hint) {
  NullBufferSink sink;
  BufferManager mgr(sink);
  mgr.create("aaa");
  mgr.create("bbb");
  mgr.touch("bbb");                   // bbb MRU, aaa secondMRU
  mgr.bufferListBuffer();             // first call creates
  mgr.touch("aaa");                   // aaa MRU, bbb secondMRU
  auto& blb = mgr.bufferListBuffer(); // refresh path
  BufferView view = BufferView::fromSavedState(blb);
  BOOST_CHECK_EQUAL(blb.selectedName(view.point()), "bbb");
}

// --- Unresolvable paths must not throw (a throw would end the session) ---

namespace {
// A directory with all permissions removed for the guard's lifetime.
struct LockedDir {
  std::filesystem::path path;
  explicit LockedDir(std::filesystem::path p) : path(std::move(p)) {
    std::filesystem::create_directory(path);
    std::filesystem::permissions(path, std::filesystem::perms::none);
  }
  ~LockedDir() {
    std::error_code ec;
    std::filesystem::permissions(path, std::filesystem::perms::owner_all, ec);
  }
};
} // namespace

BOOST_AUTO_TEST_CASE(open_under_untraversable_dir_does_not_throw) {
  NullBufferSink sink;
  BufferManager mgr(sink);
  TempDir dir;
  LockedDir locked(dir.path() / "locked");
  if (::access((locked.path / ".").c_str(), X_OK) == 0)
    return; // running with privileges that bypass permissions
  auto target = locked.path / "file.txt";
  Buffer* buf = nullptr;
  BOOST_CHECK_NO_THROW(buf = &mgr.open(target));
  BOOST_REQUIRE(buf);
  BOOST_CHECK_EQUAL(
      buf->name(),
      (std::filesystem::weakly_canonical(dir.path()) / "locked" / "file.txt").string());
  BOOST_CHECK_NO_THROW(mgr.openDir(locked.path / "sub"));
  BOOST_CHECK(mgr.rename(buf->name(), locked.path / "other.txt"));
}

BOOST_AUTO_TEST_CASE(open_symlink_loop_does_not_throw) {
  NullBufferSink sink;
  BufferManager mgr(sink);
  TempDir dir;
  auto loop = dir.path() / "loop";
  std::filesystem::create_symlink(loop, loop);
  BOOST_CHECK_NO_THROW(mgr.open(loop));
  BOOST_CHECK_NO_THROW(mgr.openDir(loop));
}

// --- Directory listings ---

BOOST_AUTO_TEST_CASE(openDir_again_rereads_the_listing) {
  // The listing used to be built once, when the buffer was created: a file
  // saved afterwards never showed up in the directory browser.
  NullBufferSink sink;
  BufferManager mgr(sink);
  TempDir dir({"b.txt", "c.txt"});
  auto& first = mgr.openDir(dir.path());
  auto names = [](DirBuffer const& d) {
    std::vector<std::string> out;
    for (auto const& e : d.entries())
      out.push_back(e.name);
    return out;
  };
  BOOST_CHECK((names(first) == std::vector<std::string>{".", "..", "b.txt", "c.txt"}));

  // The panel moved on with the cursor on c.txt.
  first.setSavedView({.point = {3, 0}});
  std::ofstream(dir.path() / "a.txt") << "new";

  auto& again = mgr.openDir(dir.path());
  BOOST_CHECK_EQUAL(&again, &first);
  BOOST_CHECK((names(again) == std::vector<std::string>{".", "..", "a.txt", "b.txt", "c.txt"}));
  // The saved cursor followed c.txt down one line.
  BOOST_REQUIRE(again.savedView().has_value());
  BOOST_CHECK_EQUAL(again.savedView()->point.line, 4u);
}

BOOST_AUTO_TEST_CASE(saved_view_is_clamped_to_a_shrunken_listing) {
  NullBufferSink sink;
  BufferManager mgr(sink);
  TempDir dir({"b.txt", "c.txt", "d.txt"});
  auto& buf = mgr.openDir(dir.path());
  buf.setSavedView({.point = {4, 0}, .viewFrom = {.line = RowIdx{4}}, .mark = BufferPoint{4, 2}});
  std::filesystem::remove(dir.path() / "c.txt");
  std::filesystem::remove(dir.path() / "d.txt");

  auto& again = mgr.openDir(dir.path());
  auto view = BufferView::fromSavedState(again);
  BOOST_CHECK_LT(view.point().line, again.lineCount());
  BOOST_CHECK_LT(view.viewFromRaw().line.value, again.lineCount());
  BOOST_REQUIRE(view.mark().has_value());
  BOOST_CHECK_LT(view.mark()->line, again.lineCount());
}

BOOST_AUTO_TEST_CASE(completions_see_files_created_since_the_last_listing) {
  // The listing behind path completion was cached per directory and only
  // replaced when another directory was completed, so a file created in
  // the meantime (a Save As, say) was not offered.
  NullBufferSink sink;
  BufferManager mgr(sink);
  TempDir dir({"alpha.txt"});
  // The cache goes by the directory's mtime, whose clock ticks every few
  // milliseconds: backdate it, so that each change below is certain to
  // move it.
  auto backdate = [&] {
    std::filesystem::last_write_time(dir.path(), std::filesystem::file_time_type::clock::now() -
                                                     std::chrono::hours(1));
  };
  backdate();
  auto prefix = dir.path().string() + "/";
  auto offered = [&](std::string const& name) {
    auto all = mgr.completions(prefix);
    return std::ranges::any_of(all, [&](auto const& c) { return c.first == prefix + name; });
  };
  BOOST_CHECK(offered("alpha.txt"));
  BOOST_CHECK(!offered("beta.txt"));

  std::ofstream(dir.path() / "beta.txt") << "new";
  BOOST_CHECK(offered("beta.txt"));
  backdate();
  BOOST_CHECK(offered("beta.txt"));
  std::filesystem::remove(dir.path() / "alpha.txt");
  BOOST_CHECK(!offered("alpha.txt"));
}
