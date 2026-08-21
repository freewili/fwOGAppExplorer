#include "platform/fwPaths.h"

#include <cstdlib>
#include <iterator>
#include <string>
#include <utility>

#if defined(_WIN32)
  #include <windows.h>
#elif !defined(__EMSCRIPTEN__)
  #include <errno.h>
  #include <limits.h>
  #include <pwd.h>
  #include <unistd.h>
  #if defined(__APPLE__)
    #include <mach-o/dyld.h>   // _NSGetExecutablePath: no /proc on macOS
  #endif
#endif

// VERIFIED ON LINUX -- and what was OBSERVED, not merely which cases were
// tried. x86-64, GCC 16.1.1, glibc 2.44, kernel 6.18.43, 4 KiB pages,
// PATH_MAX 4096. The POSIX branch below has been compiled and RUN, by a
// throwaway probe linked against this very library (so it exercised the
// shipped code, not a re-typed copy of it) plus the cases in
// tests/test_fwPaths.cpp. Where the outcome is a WRONG answer, it says so.
//
// This paragraph used to add that fwSerialPorts.cpp "still carries" the
// unverified state this file had just left. That is no longer true -- its
// Linux branch has since been measured against an attached board -- and the
// sentence went stale without anything failing. Each file states its own
// verification status and nothing here restates another file's; that is the
// point of removing it rather than merely correcting it.
//
//   * Four invocation styles: absolute path, through a symlink living in a
//     DIFFERENT directory, by relative path from a different working
//     directory, and by bare name found on $PATH.
//     OUTCOME: all four returned the directory holding the real binary.
//     The symlink case is where this branch legitimately differs from
//     Windows: /proc/self/exe reports the resolved target, so a distro that
//     drops a symlink in /usr/bin pointing into /opt gets /opt -- which is
//     where catalog/ actually sits -- whereas GetModuleFileNameW reports the
//     path the process was launched with.
//
//   * A running probe unlinking its own binary and re-reading the link (what
//     a package upgrade mid-session does).
//     OUTCOME: the link reads back as "<path>/probe (deleted)". exeDir()
//     returned the correct, unchanged directory, because the suffix lands on
//     the file component and parent_path() drops it. Not stripped on purpose;
//     see readProcSelfExe().
//
//   * /proc shadowed by an empty tmpfs in a private mount namespace
//     (`unshare -Urm sh -c 'mount -t tmpfs none /proc; ./probe'`).
//     OUTCOME: readlink fails with ENOENT and exeDir() returns the WORKING
//     DIRECTORY, which is not the executable's directory unless they happen
//     to coincide. This case is NOT handled. It is only made non-silent in
//     the weak sense that nothing truncated or invented is returned; the
//     answer is still confidently wrong. exeDir()'s own comment states what
//     that costs and why an error channel was not run to its call sites.
//
//   * An executable whose absolute path is longer than 4095 bytes, launched
//     by a relative path from a working directory one level above it.
//     OUTCOME: readlink("/proc/self/exe") returns -1/ENAMETOOLONG -- for a
//     4 KiB buffer and for a 128 KiB one alike, so no amount of buffer helps
//     -- and exeDir() again returns the WORKING DIRECTORY. Measured: true exe
//     directory 4223 bytes / 42 components, working directory 4102 bytes / 41
//     components, and exeDir() answered with the latter. WRONG, and this
//     branch does not detect it. (Note current_path() succeeds at 4102 bytes:
//     glibc's getcwd allocates rather than filling a caller's PATH_MAX
//     buffer, which is why the cwd fallback still produces an answer here at
//     all.)
//
//   * Boundary sweep on that same tree, exe path length varied one byte at a
//     time. OUTCOME: 4094 and 4095 bytes read back complete (readlink
//     returned 4094 / 4095 into a 4096-byte buffer, i.e. strictly short);
//     4096 bytes and above returned ENAMETOOLONG. So the "came back exactly
//     full" guard in readProcSelfExe() never fires HERE. Whether it can fire
//     anywhere else is not claimed; see that function for why it is kept
//     regardless.
//
// NOT verified, and not claimed: any POSIX system that is not Linux and not
// macOS. The /proc-reading branch is Linux-shaped, and elsewhere degrades
// straight to the working-directory answer of last resort. The Windows branch
// is untouched by this work and remains the reference; the one place that was
// tempting to change is flagged in tempDir().
//
// The macOS branch (_NSGetExecutablePath) is BOARD-VERIFIED as of 2026-08-14:
// it ran inside the app for a real flash, so exeDir() found catalog/ and the
// firmware on a Mac. That run predates the v2 rebase; on the rebased branch
// only the build and test suite were re-verified.

namespace fwog {
namespace {
constexpr const char* kAppName = "fwOGAppExplorer";

#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)

/// The executable's own path from /proc/self/exe, or an empty path meaning
/// "cannot tell" -- never a guess and never a partial answer.
///
/// One readlink() call. An earlier version grew the buffer and re-read until
/// it got a strictly short answer; that was deleted, because the case it was
/// built for cannot be helped from here. Measured on this kernel (4 KiB
/// pages): a /proc symlink longer than 4095 bytes comes back as
/// -1/ENAMETOOLONG for a 4 KiB buffer and for a 128 KiB buffer alike, so the
/// second pass never had anything new to learn. And a longer path would be of
/// no use if it were returned: PATH_MAX is 4096 here and bounds the pathname
/// ARGUMENTS that catalogDir()'s users go on to pass, so the syscalls refuse
/// it.
/// (Directly observed here, on the over-long tree: chdir() into a 4102-byte
/// directory fails with ENAMETOOLONG even though getcwd() reports that same
/// directory happily -- getcwd allocates rather than filling a PATH_MAX
/// buffer, which is also why the cwd fallback still produces an answer there.)
/// Carrying a growth loop in order to return a path that nothing downstream
/// can then use is complexity bought for nothing.
///
/// What is kept is the guard, because it costs one comparison and it is the
/// part that actually protects a caller: readlink() reports NEITHER truncation
/// nor the length it wanted -- it fills at most `size` bytes and returns how
/// many it wrote -- so a return of exactly the buffer size is indistinguishable
/// from a path that happens to be exactly that long. Only a STRICTLY short
/// read is provably complete, so only that is accepted. Anything else is
/// reported as "cannot tell" rather than handed back, because a truncated path
/// is the worst shape of failure available here: still absolute, still
/// syntactically fine, its parent_path() still a real-looking directory, so
/// catalogDir() would point somewhere that simply has no catalog in it and the
/// app would show an empty catalog with nothing anywhere saying why.
///
/// MEASURED here, and only here: the guard never fires on this machine (4095
/// bytes reads back short, 4096 and up are refused outright). Whether some
/// other kernel can return a full buffer is deliberately NOT asserted either
/// way. Two drafts of this comment tried, each naming a kernel version as the
/// reason, and each was wrong -- the second was disproved from the tagged
/// sources in about a minute. Nothing here needs the answer: the guard is
/// against readlink()'s CONTRACT, which permits a truncated return and gives
/// no way to detect one, so a single comparison is the cheap way to never
/// depend on that permission going unexercised by whatever kernel this
/// actually runs on. A claim about another platform's internals would buy
/// nothing and has already cost two rounds of being wrong.
///
/// A binary replaced or deleted while it runs (a package upgrade during a
/// session) makes the kernel append " (deleted)" to this link. That is
/// deliberately NOT stripped: the suffix lands on the file component, so the
/// parent_path() that every caller here actually wants is unaffected, and
/// stripping it would instead corrupt the answer for a legitimate executable
/// whose name really does end that way.
#if defined(__APPLE__)
/// macOS has no /proc; dyld itself answers instead. Same contract as the Linux
/// reader below it replaces: a complete absolute path or "cannot tell", never a
/// guess. _NSGetExecutablePath reports the size it wanted when the buffer is
/// too small, so unlike readlink() there is no truncation ambiguity to guard
/// against -- the second call with the reported size either fits or fails.
/// The path is as-invoked and may contain symlinks or "..", so it is
/// canonicalised; weakly_canonical rather than canonical so a deleted-under-us
/// binary degrades to the lexical answer instead of an error.
std::filesystem::path readProcSelfExe()
{
    uint32_t size = 0;
    ::_NSGetExecutablePath(nullptr, &size);           // asks for the needed size
    if (size == 0) return {};
    std::string buf(size, '\0');
    if (::_NSGetExecutablePath(buf.data(), &size) != 0) return {};
    buf.resize(std::char_traits<char>::length(buf.c_str()));
    if (buf.empty()) return {};
    std::error_code ec;
    auto canon = std::filesystem::weakly_canonical(buf, ec);
    return ec ? std::filesystem::path(std::move(buf)) : canon;
}
#else
std::filesystem::path readProcSelfExe()
{
    std::string  buf(PATH_MAX, '\0');
    const ssize_t n = ::readlink("/proc/self/exe", buf.data(), buf.size());
    if (n <= 0) return {};                            // no /proc, or the kernel refused
    if (static_cast<size_t>(n) >= buf.size()) return {};   // may be truncated: not an answer
    buf.resize(static_cast<size_t>(n));
    return std::filesystem::path(std::move(buf));
}
#endif

/// True when an environment variable holds something usable as a base
/// directory for user data.
///
/// Set-but-empty and set-but-relative both count as UNSET, which is not
/// leniency but the XDG Base Directory specification's own rule: "All paths
/// set in these environment variables must be absolute. If an implementation
/// encounters a relative path in any of these variables it should consider
/// the path invalid and ignore it." Taking a relative value at face value is
/// what makes the failure silent -- userDataDir() would return a RELATIVE
/// path, create it under whatever the working directory happened to be, and
/// settings.ini plus the catalog cache would quietly follow the user around
/// instead of persisting, with every individual write succeeding.
bool isUsableBaseDir(const char* value)
{
    return value && *value && std::filesystem::path(value).is_absolute();
}

/// The account database's idea of this user's home directory, for when $HOME
/// is unset or unusable -- which is normal, not exotic, for a process started
/// by a service manager, a cron job, or a desktop launcher with a sanitised
/// environment.
///
/// getpwuid_r rather than getpwuid: userDataDir() is called from the catalog
/// worker thread (RemoteCatalog::run caches into it) as well as from the UI
/// thread, and getpwuid hands back a pointer into one process-wide static
/// passwd struct that a concurrent call would overwrite underneath us.
///
/// The ERANGE retry is not defensive padding. _SC_GETPW_R_SIZE_MAX is 1024 on
/// this machine -- measured, not assumed -- which is an ordinary size, not a
/// generous one: it is the whole passwd entry, so a gecos field, a long shell
/// path and a network home directory from an NSS/LDAP or AD backend can
/// exceed it without anything being unusual about the account. getpwuid_r
/// returning ERANGE was reproduced directly (a probe calling it with 1-, 8-
/// and 64-byte buffers got ERANGE=34 each time, and 128 bytes and up
/// succeeded), so this is a real return value, not a hypothetical one.
/// Without the retry, such a user falls straight through to the exeDir()
/// answer in userDataDir(), which for an installed build is a read-only
/// directory where create_directories() fails and settings plus the catalog
/// cache are silently lost -- the exact defect this whole tier exists to
/// close, so closing it only for small passwd entries would be half a fix.
///
/// The loop is bounded by an attempt count rather than by a size, so a strange
/// sysconf value cannot make it spin or skip: seven attempts starting at 1024
/// end at 64 KiB, and then it gives up. pw.pw_dir points INTO buf, so the path
/// is built inside the loop, while that buffer is still alive.
std::filesystem::path passwdHomeDir()
{
    const long want = ::sysconf(_SC_GETPW_R_SIZE_MAX);
    size_t     cap  = (want > 0) ? static_cast<size_t>(want) : 1024u;

    for (int attempt = 0; attempt < 7; ++attempt, cap *= 2) {
        std::string buf(cap, '\0');
        passwd      pw{};
        passwd*     found = nullptr;

        const int rc = ::getpwuid_r(::getuid(), &pw, buf.data(), buf.size(), &found);
        if (rc == ERANGE) continue;               // entry did not fit; ask again with more
        if (rc != 0 || !found) return {};         // no entry, or the database is unreachable
        if (!pw.pw_dir || !*pw.pw_dir) return {};

        std::filesystem::path home(pw.pw_dir);
        // Same rule as the environment: a relative home directory is not a
        // home directory, it is a misconfiguration, and honouring it would put
        // user data somewhere that depends on where the app was started from.
        return home.is_absolute() ? home : std::filesystem::path{};
    }
    // 64 KiB was not enough for one passwd entry. Nothing useful is left to
    // try, and the caller's next fallback is exeDir() -- see userDataDir().
    return {};
}

#endif   // POSIX helpers

} // namespace

std::filesystem::path exeDir()
{
#if defined(_WIN32)
    wchar_t buf[MAX_PATH * 4] = {};
    GetModuleFileNameW(nullptr, buf, DWORD(std::size(buf)));
    return std::filesystem::path(buf).parent_path();
#elif defined(__EMSCRIPTEN__)
    return std::filesystem::path("/");
#else
    // What the kernel says this process is running, resolved and complete or
    // not accepted at all.
    if (const auto exe = readProcSelfExe(); !exe.empty()) return exe.parent_path();

    // Everything past here is the answer of last resort, and it is worth being
    // blunt about it: THE WORKING DIRECTORY IS NOT THE EXECUTABLE'S DIRECTORY.
    // Returning it is a known-wrong answer, kept only because the alternatives
    // are worse. Two states reach this line, both measured (see the block at
    // the top of this file):
    //
    //   * /proc is not readable -- a chroot, a container image built without
    //     it, a private mount namespace.
    //   * The executable's path is longer than 4095 bytes, which makes the
    //     kernel refuse the readlink outright with ENAMETOOLONG no matter how
    //     much buffer it is offered. This one is NOT handled anywhere in this
    //     file: an earlier draft claimed a second mechanism covered it, and
    //     that claim was false -- AT_EXECFN's resolution walks the same
    //     PATH_MAX wall. The mechanism was deleted rather than left in place
    //     looking like a fix.
    //
    // What the wrong answer costs, plainly: catalogDir() names `catalog/`
    // under the working directory, so the app shows an EMPTY local catalog
    // with no error anywhere saying why, and "Open catalog folder" in
    // fwTabAppExplorer.cpp opens that wrong directory. That button also calls
    // create_directories() on it first (fwTabAppExplorer.cpp:253), so pressing
    // it MAKES an empty `catalog/` in the working directory -- the one write
    // that follows from this answer, and it is a stray empty directory rather
    // than data put somewhere it will be lost. No user data rides on it:
    // userDataDir() reaches exeDir() only after $XDG_DATA_HOME, $HOME and the
    // account database have all failed as well. No device is touched.
    //
    // Why not the alternatives:
    //
    //   * An empty path is the same wrongness with worse ergonomics.
    //     catalogDir() would become the relative path "catalog", which the OS
    //     resolves against this very working directory anyway -- the identical
    //     files get touched -- while additionally breaking the "Open catalog
    //     folder" button, whose file:/// URL needs an absolute path to mean
    //     anything.
    //   * An error channel would have to ripple through userDataDir() below
    //     and through catalogDir(), whose own four users -- fwApp.cpp:602 and
    //     fwTabAppExplorer.cpp:253, 257 and 260 -- would each then have to
    //     handle it, all of them treating this as infallible today, in order
    //     to describe a state the app cannot proceed usefully in anyway.
    //     That is a judgement call, not a certainty, and it is
    //     recorded here as one: if the empty-catalog symptom ever shows up in
    //     a real report, an error channel is the right fix and this paragraph
    //     is the note saying so. It was not judged worth it for a state that
    //     took a private mount namespace or a 4 KiB pathname to reach.
    //
    // current_path() failing in turn -- the working directory unlinked out
    // from under the process -- leaves the empty path, the very thing argued
    // against two paragraphs up. It is returned here only because in that
    // state there is no third option to prefer, and a process whose working
    // directory no longer exists has already lost more than its catalog.
    std::error_code ec;
    return std::filesystem::current_path(ec);   // empty path if even this fails
#endif
}

std::filesystem::path userDataDir()
{
    std::filesystem::path base;
#if defined(_MSC_VER)
#  pragma warning(push)
#  pragma warning(disable: 4996)   // getenv: the returned pointer is copied
#endif                             // into a path immediately; no ownership kept
#if defined(_WIN32)
    if (const char* local = std::getenv("LOCALAPPDATA")) base = local;
    else base = exeDir();
#elif defined(__EMSCRIPTEN__)
    base = "/data";
#else
    // Each candidate has to be absolute to be used at all -- see
    // isUsableBaseDir for why that is the spec's rule and not this project
    // being fussy. The order is the XDG order, with one addition: when $HOME
    // is missing or unusable the account database is consulted before giving
    // up, because "no $HOME" is a normal environment for a launcher-started
    // or service-started process, and the exeDir() answer under it is usually
    // a read-only install directory (/usr/bin) where create_directories fails
    // and the user silently gets no saved settings and no catalog cache.
    const char* xdg  = std::getenv("XDG_DATA_HOME");
    const char* home = std::getenv("HOME");
#if defined(__APPLE__)
    // Same tier order as Linux below, but the per-user data directory under a
    // home is the platform's own: ~/Library/Application Support. An explicit
    // absolute $XDG_DATA_HOME still wins -- a user who sets it on macOS has
    // said where they want data, and ignoring it would be inventing a rule the
    // XDG spec does not have.
    const auto dataDirUnder = [](const std::filesystem::path& h) {
        return h / "Library" / "Application Support";
    };
#else
    const auto dataDirUnder = [](const std::filesystem::path& h) {
        return h / ".local" / "share";
    };
#endif
    if (isUsableBaseDir(xdg))       base = xdg;
    else if (isUsableBaseDir(home)) base = dataDirUnder(home);
    else if (auto pwHome = passwdHomeDir(); !pwHome.empty())
                                    base = dataDirUnder(pwHome);
    else                            base = exeDir();
#endif
#if defined(_MSC_VER)
#  pragma warning(pop)
#endif
    auto dir = base / kAppName;
    // create_directories is best-effort: a failure here (e.g. a read-only
    // parent) is not reported, because the first real write into this
    // directory will fail loudly on its own.
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

std::filesystem::path tempDir()
{
    std::error_code ec;
    auto p = std::filesystem::temp_directory_path(ec);
    if (ec) return userDataDir();

#if !defined(_WIN32)
    // The is_absolute() check is not belt-and-braces, it closes a real hole
    // that the error code alone does not. Measured on this libstdc++ (and
    // pinned by a test in tests/test_fwPaths.cpp so a future upgrade cannot
    // change it unnoticed): $TMPDIR that is empty, missing, or not a directory
    // all set ec as hoped -- but a $TMPDIR holding a RELATIVE path that
    // happens to exist is returned as-is, relative, with no error at all.
    //
    // Every caller of this function stages a file into it and then hands the
    // resulting path to something else: fwFlashController.cpp:69 stages the
    // UF2 it is about to copy onto a mounted board, fwCpuProbeController.cpp:20
    // stages the probe image. A relative staging directory means those files
    // land under whatever the working directory is rather than in system temp
    // -- writes that succeed, into the wrong place, which is exactly the
    // silently-wrong outcome this port is meant not to have.
    //
    // Falling back to userDataDir() rather than making the relative path
    // absolute is the deliberate choice: it matches what this function already
    // does for every other unusable $TMPDIR, and it lands on a directory that
    // is known absolute and already created. Honouring a relative $TMPDIR by
    // anchoring it to the working directory would be inventing an intent the
    // user did not express.
    //
    // Guarded to the non-Windows branch ON PURPOSE, and not because the hole
    // is known to be absent on Windows -- it is not known either way. The
    // defect above was reproduced against libstdc++ and $TMPDIR; the Windows
    // equivalent would be GetTempPath2W's %TMP%/%TEMP% search inside the MSVC
    // STL, which cannot be compiled or run from here, so whether it can return
    // a relative path is a question this change is in no position to answer.
    // Applying the check there anyway would be changing verified-and-working
    // code on the strength of a guess about a library implementation read from
    // memory. That is the same trade fwSerialPorts.cpp's Linux branch used to
    // make about usbId, and it is worth noting why that one was reversed while
    // this one stands: usbId became verifiable the moment a board was plugged
    // in, and this does not become verifiable without a Windows machine.
    // The bounded consequence of leaving it: if a Windows user sets %TMP% to a
    // relative path that exists, staged firmware lands under the working
    // directory there. Nobody has reported it and it was not reproduced.
    if (!p.is_absolute()) return userDataDir();
#endif
    return p;
}

std::filesystem::path catalogDir()
{
    return exeDir() / "catalog";
}

} // namespace fwog
