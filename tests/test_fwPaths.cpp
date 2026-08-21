#include <doctest/doctest.h>
#include "platform/fwPaths.h"

#include <filesystem>
#include <fstream>
#include <string>

#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
  #include <limits.h>
  #include <pwd.h>
  #include <unistd.h>
  #if defined(__APPLE__)
    #include <mach-o/dyld.h>
  #endif

namespace {
/// Where userDataDir() puts per-user data under a given home directory --
/// the platform's own convention, mirrored from fwPaths.cpp so the
/// expectations below state it once.
std::filesystem::path dataDirUnder(const std::filesystem::path& home)
{
#if defined(__APPLE__)
    return home / "Library" / "Application Support";
#else
    return home / ".local" / "share";
#endif
}
} // namespace
#endif

using namespace fwog;

TEST_CASE("exeDir is an existing absolute directory") {
    auto p = exeDir();
    CHECK(p.is_absolute());
    CHECK(std::filesystem::is_directory(p));
}

TEST_CASE("userDataDir is absolute and ends with the app name") {
    auto p = userDataDir();
    CHECK(p.is_absolute());
    CHECK(p.filename() == "fwOGAppExplorer");
}

TEST_CASE("userDataDir is created on demand") {
    CHECK(std::filesystem::is_directory(userDataDir()));
}

TEST_CASE("tempDir is an existing absolute directory") {
    auto p = tempDir();
    CHECK(p.is_absolute());
    CHECK(std::filesystem::is_directory(p));
}

TEST_CASE("catalogDir sits beside the executable") {
    CHECK(catalogDir() == exeDir() / "catalog");
}

// ---------------------------------------------------------------------------
// POSIX-only. Everything below drives the branch of fwPaths.cpp that only
// exists off Windows, and does it through the real process environment, which
// is the one input to these functions a test CAN control: setenv/unsetenv are
// how a test asks "what would this do on a machine configured that way?"
// without needing that machine. The Windows branch is deliberately untouched
// by all of this -- it is the verified reference implementation.
// ---------------------------------------------------------------------------
#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)

namespace {

/// Saves one environment variable and puts it back exactly as it was --
/// including putting back "was not set at all", which is a different state
/// from "set to empty" and one that these very tests care about the
/// difference between.
///
/// This matters beyond politeness: the whole suite runs in ONE process, and
/// $HOME and $XDG_DATA_HOME decide where userDataDir() points. Leaking a
/// test's fake $HOME would silently relocate the cache file that
/// test_fwCatalogRemote.cpp saves and restores, and that test would then
/// "pass" while operating on a file the app will never read.
class EnvGuard {
public:
    explicit EnvGuard(const char* name) : m_name(name) {
        if (const char* v = std::getenv(name)) { m_had = true; m_value = v; }
    }
    ~EnvGuard() {
        if (m_had) ::setenv(m_name.c_str(), m_value.c_str(), 1);
        else       ::unsetenv(m_name.c_str());
    }
    EnvGuard(const EnvGuard&) = delete;
    EnvGuard& operator=(const EnvGuard&) = delete;

    void set(const char* value) const { ::setenv(m_name.c_str(), value, 1); }
    void unset() const                { ::unsetenv(m_name.c_str()); }

private:
    std::string m_name;
    bool        m_had = false;
    std::string m_value;
};

/// A real, empty directory under the system temp dir, removed again when the
/// test finishes. Used as a stand-in $HOME/$XDG_DATA_HOME so that the tests
/// below create their fwOGAppExplorer directories somewhere disposable rather
/// than in the developer's actual data directory.
class ScratchDir {
public:
    explicit ScratchDir(const char* tag) {
        std::error_code ec;
        m_path = std::filesystem::temp_directory_path(ec) /
                 (std::string("fwog-paths-test-") + tag);
        std::filesystem::remove_all(m_path, ec);
        std::filesystem::create_directories(m_path, ec);
    }
    ~ScratchDir() {
        std::error_code ec;
        std::filesystem::remove_all(m_path, ec);
    }
    ScratchDir(const ScratchDir&) = delete;
    ScratchDir& operator=(const ScratchDir&) = delete;

    const std::filesystem::path& path() const { return m_path; }

private:
    std::filesystem::path m_path;
};

/// Removes a relative path from the working directory on the way out, whether
/// or not the test passed.
///
/// The tests below assert that a RELATIVE $XDG_DATA_HOME or $HOME is ignored,
/// and they check it the only way that actually proves it: by confirming no
/// such directory appeared in the working directory. The trap is what happens
/// when that assertion FAILS. The failing code has by then created the
/// directory, doctest reports the failure and moves on, and the directory
/// stays -- so every LATER run of this suite, including runs against fixed
/// code, sees it and fails too. The suite goes permanently red on a defect
/// that no longer exists, and only `rm -rf` in the right working directory
/// clears it, which is not something the next person will guess.
///
/// A destructor rather than a cleanup call at the end of the test, for exactly
/// the reason destructors exist here: the end of the test is not reached when
/// a REQUIRE fires, and that is precisely the run whose mess matters most.
class StrayGuard {
public:
    explicit StrayGuard(std::filesystem::path relative)
        : m_path(std::move(relative)) {}
    ~StrayGuard() {
        std::error_code ec;
        std::filesystem::remove_all(m_path, ec);
    }
    StrayGuard(const StrayGuard&) = delete;
    StrayGuard& operator=(const StrayGuard&) = delete;

    const std::filesystem::path& path() const { return m_path; }

private:
    std::filesystem::path m_path;
};

/// The home directory from the account database, mirroring what
/// passwdHomeDir() inside fwPaths.cpp consults. Used to state the EXPECTED
/// answer for the no-$HOME cases -- checking only "the result is absolute"
/// would pass just as happily if the code had fallen through to exeDir(),
/// which is the outcome these tests exist to rule out.
std::filesystem::path passwdHome() {
    if (const passwd* pw = ::getpwuid(::getuid()); pw && pw->pw_dir && *pw->pw_dir)
        return std::filesystem::path(pw->pw_dir);
    return {};
}

} // namespace

TEST_CASE("exeDir names the directory that really contains this test binary") {
    // The strongest end-to-end assertion available from inside the process:
    // fwog_tests is itself an executable, CMake fixes its name, and it is
    // built into the same directory it runs from. If exeDir() ever falls back
    // to the working directory -- the failure mode that looks completely
    // plausible from the outside -- running the suite from anywhere other
    // than the build directory makes this fail, which is precisely when it
    // should.
    CHECK(std::filesystem::exists(exeDir() / "fwog_tests"));
}

#if defined(__APPLE__)
TEST_CASE("exeDir agrees with a direct ask of dyld") {
    // The macOS counterpart of the /proc/self/exe cross-check below it
    // replaces: same independent-source shape, same question. The path dyld
    // reports is as-invoked, so it is canonicalised here exactly as
    // fwPaths.cpp does before comparing.
    uint32_t size = 0;
    ::_NSGetExecutablePath(nullptr, &size);
    REQUIRE(size > 0);
    std::string buf(size, '\0');
    REQUIRE(::_NSGetExecutablePath(buf.data(), &size) == 0);
    buf.resize(std::char_traits<char>::length(buf.c_str()));
    CHECK(exeDir() ==
          std::filesystem::weakly_canonical(std::filesystem::path(buf)).parent_path());
}
#else
TEST_CASE("exeDir agrees with a direct read of /proc/self/exe") {
    char buf[PATH_MAX] = {};
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    REQUIRE(n > 0);
    CHECK(exeDir() == std::filesystem::path(std::string(buf, size_t(n))).parent_path());
}
#endif

// A test asserting that readlink() cannot signal truncation used to live here.
// It was deleted: it called no fwog function, so no change to fwPaths.cpp
// could ever have made it fail, and the behaviour it pinned is mandated by
// POSIX rather than chosen by an implementation that might change under us.
// That is the difference from the file_size test in test_fwVolume.cpp, which
// this test cited as precedent but does not match: there, a live branch of
// copyToVolume() compares against std::errc::no_such_file_or_directory, so the
// pin guards a real dependency on one standard library's mapping.

TEST_CASE("an absolute XDG_DATA_HOME is used as the base") {
    EnvGuard   xdg("XDG_DATA_HOME");
    ScratchDir scratch("xdg-abs");
    xdg.set(scratch.path().c_str());

    const auto p = userDataDir();
    CHECK(p == scratch.path() / "fwOGAppExplorer");
    CHECK(std::filesystem::is_directory(p));
}

TEST_CASE("a relative XDG_DATA_HOME is ignored, as the XDG spec requires") {
    EnvGuard   xdg("XDG_DATA_HOME");
    EnvGuard   home("HOME");
    ScratchDir scratch("xdg-rel");
    StrayGuard stray("fwog-relative-xdg-should-be-ignored");
    xdg.set(stray.path().c_str());
    home.set(scratch.path().c_str());

    const auto p = userDataDir();
    CHECK(p.is_absolute());
    CHECK(p == dataDirUnder(scratch.path()) / "fwOGAppExplorer");
    // And nothing was created under the working directory on the way past:
    // honouring the relative value would have made a real directory there.
    CHECK_FALSE(std::filesystem::exists(stray.path()));
}

TEST_CASE("an empty XDG_DATA_HOME is treated as unset, not as the empty path") {
    // Set-but-empty is a real environment, not a contrived one: it is what a
    // launcher that exports a variable it failed to compute leaves behind.
    // Taken at face value it makes the base path empty, and userDataDir()
    // returns the bare relative name "fwOGAppExplorer".
    EnvGuard   xdg("XDG_DATA_HOME");
    EnvGuard   home("HOME");
    ScratchDir scratch("xdg-empty");
    xdg.set("");
    home.set(scratch.path().c_str());

    const auto p = userDataDir();
    CHECK(p.is_absolute());
    CHECK(p == dataDirUnder(scratch.path()) / "fwOGAppExplorer");
    // No "did it pollute the working directory?" check here, unlike the
    // relative-XDG case above: an empty base produces the bare name
    // "fwOGAppExplorer", and the app's own executable is called exactly that
    // and sits in the directory ctest runs this from -- so such a check would
    // be answering a question about the build tree rather than about this
    // code. The equality above already proves the empty value was ignored.
}

TEST_CASE("a relative or empty HOME is ignored too, and the account database answers") {
    const auto expectedHome = passwdHome();
    REQUIRE_FALSE(expectedHome.empty());   // no getpwuid entry: see comment below

    EnvGuard   xdg("XDG_DATA_HOME");
    EnvGuard   home("HOME");
    StrayGuard stray("fwog-relative-home-should-be-ignored");
    xdg.unset();

    SUBCASE("relative HOME") { home.set(stray.path().c_str()); }
    SUBCASE("empty HOME")    { home.set(""); }
    SUBCASE("unset HOME")    { home.unset(); }

    const auto p = userDataDir();
    CHECK(p.is_absolute());
    CHECK(p == dataDirUnder(expectedHome) / "fwOGAppExplorer");
    CHECK_FALSE(std::filesystem::exists(stray.path()));
}

TEST_CASE("temp_directory_path returns a RELATIVE path for a relative TMPDIR") {
    // The library assumption tempDir() is built on, verified against the
    // standard library actually in use rather than assumed -- same reasoning
    // as the file_size test in test_fwVolume.cpp. An unusable $TMPDIR
    // normally reports an error code, which is why tempDir() looked safe; a
    // relative one that EXISTS does not, and comes back relative.
    EnvGuard tmp("TMPDIR");
    const std::filesystem::path rel = "fwog-relative-tmpdir";
    std::error_code ec;
    std::filesystem::remove_all(rel, ec);
    std::filesystem::create_directories(rel, ec);
    REQUIRE_FALSE(ec);
    tmp.set(rel.c_str());

    const auto raw = std::filesystem::temp_directory_path(ec);
    CHECK_FALSE(ec);                 // no error is reported...
    CHECK_FALSE(raw.is_absolute());  // ...yet the answer is not usable as-is

    // tempDir() must not pass that through: staged firmware images would land
    // under the working directory instead of system temp.
    const auto p = tempDir();
    CHECK(p.is_absolute());
    CHECK(p == userDataDir());

    std::filesystem::remove_all(rel, ec);
}

TEST_CASE("a TMPDIR that does not exist falls back to userDataDir") {
    EnvGuard tmp("TMPDIR");
    tmp.set("/nonexistent-fwog-tmpdir-6f2a1c9e");

    const auto p = tempDir();
    CHECK(p == userDataDir());
    CHECK(std::filesystem::is_directory(p));
}

TEST_CASE("a TMPDIR pointing at a regular file falls back to userDataDir") {
    ScratchDir scratch("tmp-file");
    const auto file = scratch.path() / "not-a-directory";
    { std::ofstream(file) << "x"; }

    EnvGuard tmp("TMPDIR");
    tmp.set(file.c_str());

    const auto p = tempDir();
    CHECK(p == userDataDir());
    CHECK(std::filesystem::is_directory(p));
}

#endif   // POSIX-only
