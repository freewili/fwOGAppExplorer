#include "platform/fwHttp.h"

#include "core/fwSettingsIo.h"   // hasHttpScheme -- the ONE scheme predicate

#include <cstring>

namespace fwog {
namespace {

/// Returns an error when the URL is unusable, or an empty value when it is
/// fine. Callers write
/// `if (auto bad = checkUrl(url); !bad.has_value()) return std::unexpected(bad.error());`.
///
/// The scheme test is fwog::hasHttpScheme() (fwog_core), not a local one.
/// This file used to carry its own `rfind("https://", 0) == 0`, which is
/// case-SENSITIVE, while the settings layer that validates what the user types
/// compares the scheme case-INsensitively as RFC 3986 requires. The two
/// disagreed: pasting "HTTPS://example.com/apps.json" was accepted by the App
/// Explorer tab's control, written to settings.ini, and then failed every
/// fetch -- this session and every future launch -- with "only http and https
/// URLs are supported", for a URL that plainly is https. Both layers now share
/// one predicate so they cannot drift apart again.
std::expected<std::string, std::string> checkUrl(const std::string& url)
{
    if (url.empty())          return std::unexpected("the URL is empty");
    if (!hasHttpScheme(url))  return std::unexpected("only http and https URLs are supported");
    return std::string{};
}

} // namespace

bool mayRedirectToPlainHttp(std::string_view url)
{
    // An https request must not be walked down to plain http by a redirect.
    // What travels over this transport is a catalog document, and a catalog
    // entry is AUTHORITATIVE OVER THE TARGET CPU -- see the long note on
    // normalizeRemoteCatalogUrl() in fwSettingsIo.h. That layer refuses to let
    // a user configure a plain-http catalog URL at all; a redirect that drops
    // TLS re-opens the same hole from the server side, where the user cannot
    // see it.
    //
    // A request that was already plain http has no TLS to lose, so it keeps
    // both schemes -- refusing http -> http would break ordinary redirects for
    // the non-catalog things fetched with this transport.
    //
    // Same predicate as the settings layer (hasHttpsScheme, case-insensitive
    // per RFC 3986), so this cannot drift from what the user was allowed to
    // type. Pure and platform-independent on purpose: WinHTTP enforces the
    // equivalent natively and never calls this, but the RULE is then testable
    // on every platform with no network and no libcurl.
    return !hasHttpsScheme(url);
}

} // namespace fwog

#if defined(_WIN32)
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

namespace fwog {

bool httpAvailable() { return true; }

std::expected<std::string, std::string> httpGet(const std::string& url)
{
    if (auto bad = checkUrl(url); !bad.has_value()) return std::unexpected(bad.error());

    const std::wstring wurl(url.begin(), url.end());

    URL_COMPONENTS uc = {};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {}, path[2048] = {};
    uc.lpszHostName = host; uc.dwHostNameLength = DWORD(std::size(host));
    uc.lpszUrlPath  = path; uc.dwUrlPathLength  = DWORD(std::size(path));
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc))
        return std::unexpected("the URL could not be parsed");

    struct Handle {
        HINTERNET h = nullptr;
        ~Handle() { if (h) WinHttpCloseHandle(h); }
        Handle() = default;
        explicit Handle(HINTERNET handle) : h(handle) {}
        Handle(const Handle&) = delete;
        Handle& operator=(const Handle&) = delete;
    };

    Handle session{ WinHttpOpen(L"fwOGAppExplorer/1.0",
                                WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0) };
    if (!session.h) return std::unexpected("the HTTP session could not be opened");

    Handle conn{ WinHttpConnect(session.h, host, uc.nPort, 0) };
    if (!conn.h) return std::unexpected("cannot reach the host");

    const DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    Handle req{ WinHttpOpenRequest(conn.h, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                   WINHTTP_DEFAULT_ACCEPT_TYPES, flags) };
    if (!req.h) return std::unexpected("the request could not be created");

    if (!WinHttpSendRequest(req.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req.h, nullptr))
        return std::unexpected("the request failed; check the network connection");

    DWORD status = 0, statusLen = sizeof(status);
    WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusLen,
                        WINHTTP_NO_HEADER_INDEX);
    if (status < 200 || status >= 300)
        return std::unexpected("the server returned HTTP " + std::to_string(status));

    std::string body;
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req.h, &avail) || avail == 0) break;
        std::string chunk(avail, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(req.h, chunk.data(), avail, &read)) break;
        body.append(chunk, 0, read);
    }
    return body;
}

} // namespace fwog

#elif defined(__EMSCRIPTEN__)
#include <emscripten/fetch.h>

namespace fwog {

bool httpAvailable() { return true; }

std::expected<std::string, std::string> httpGet(const std::string& url)
{
    if (auto bad = checkUrl(url); !bad.has_value()) return std::unexpected(bad.error());

    emscripten_fetch_attr_t attr;
    emscripten_fetch_attr_init(&attr);
    std::strcpy(attr.requestMethod, "GET");
    // Synchronous because this already runs on a worker (fwCatalogRemote), and
    // blocking there keeps one code shape across all three platforms.
    attr.attributes = EMSCRIPTEN_FETCH_LOAD_TO_MEMORY | EMSCRIPTEN_FETCH_SYNCHRONOUS;

    emscripten_fetch_t* f = emscripten_fetch(&attr, url.c_str());
    if (!f) return std::unexpected("the request could not be created");

    std::expected<std::string, std::string> result =
        (f->status >= 200 && f->status < 300)
            ? std::expected<std::string, std::string>(
                  std::string(f->data, size_t(f->numBytes)))
            : std::unexpected("the server returned HTTP " + std::to_string(f->status));

    emscripten_fetch_close(f);
    return result;
}

} // namespace fwog

#else   // Linux and other POSIX
#include <csignal>
#include <dlfcn.h>

// VERIFICATION STATUS (Linux/x86-64, GCC, libcurl 8.21.0 / OpenSSL 3.6.3).
// This branch had never been compiled or run before the Linux port; what
// follows is what was actually exercised, and nothing else is claimed.
//
//  * Both halves of the dlopen design were run. With libcurl present, a real
//    https GET returns a body and a 2xx; a 404 URL reports "the server
//    returned HTTP 404" without a body. With libcurl absent -- forced by
//    running the probe under a mount namespace with the libcurl sonames made
//    unresolvable -- httpAvailable() returns false and httpGet() returns the
//    "libcurl is not installed" message rather than crashing.
//  * The option numbers below were compared against /usr/include/curl/curl.h
//    on this machine, one by one; the static_asserts underneath now do that
//    on any machine that has the header.
//  * The SIGPIPE claim in curl()'s comment was MEASURED, by reading the
//    signal disposition from inside a progress callback during a live
//    transfer, not taken from documentation.
//  * The time limits were measured against a local server, both ways. A
//    2,000,000-byte body delivered at ~50 KB/s (40s, i.e. past the 30s TOTAL
//    cap this branch used to have) now COMPLETES -- it aborted at exactly 30s
//    before. A peer that sends headers and then goes silent still fails, at
//    30s, with "Timeout was reached". An unroutable address fails at 60s,
//    which is CURLOPT_CONNECTTIMEOUT doing its job now that the total no
//    longer bounds a hung connect.
//  * writeCb's catch was exercised by fetching a multi-GiB stream under a
//    700 MB RLIMIT_AS: it reports CURLE_WRITE_ERROR, where the same callback
//    without the catch aborts the process (SIGABRT).
//
// Not verified here: behaviour against a libcurl older than 8.x, and the
// per-hop corner of WinHTTP's redirect rule noted in httpGet().

// Cross-check the hard-coded option numbers against the real header on any
// machine that happens to have libcurl's development headers -- a mistyped
// number is otherwise invisible, because setting an option libcurl does not
// recognise changes nothing and looks exactly like success.
//
// Including the header does NOT link libcurl and does not make it a build
// requirement: only enumerators are used, no function is called by name, so no
// undefined symbol is created and the dlopen design is untouched. Where the
// header is missing the numbers stand alone and these checks vanish.
#if __has_include(<curl/curl.h>)
#include <curl/curl.h>
#define FWOG_CURL_HEADER_PRESENT 1
#endif

namespace fwog {
namespace {

// libcurl is dlopen'd, never linked. Linking would make libcurl.so a hard
// runtime dependency of a binary that is supposed to have none. When it is
// absent the failure is contained: the remote catalog reports unavailable and
// embedded firmware, the local catalog, device detection, flashing and
// Recovery all keep working.
struct Curl {
    void* lib = nullptr;
    int         (*global_init)(long)             = nullptr;
    void*       (*easy_init)()                  = nullptr;
    int         (*easy_setopt)(void*, int, ...) = nullptr;
    int         (*easy_perform)(void*)          = nullptr;
    int         (*easy_getinfo)(void*, int, ...) = nullptr;
    void        (*easy_cleanup)(void*)          = nullptr;
    const char* (*easy_strerror)(int)           = nullptr;

    bool ok() const {
        return lib && global_init && easy_init && easy_setopt && easy_perform
               && easy_getinfo && easy_cleanup && easy_strerror;
    }
};

// Option values from curl.h, hard-coded because the header is only optionally
// available. Each is a stable part of libcurl's ABI -- the numbers are baked
// into every compiled caller, so libcurl cannot renumber them -- and each is
// static_asserted against the header below wherever that header exists.
constexpr int  CURLOPT_URL                    = 10002;
constexpr int  CURLOPT_WRITEDATA              = 10001;
constexpr int  CURLOPT_USERAGENT              = 10018;
constexpr int  CURLOPT_WRITEFUNCTION          = 20011;
constexpr int  CURLOPT_FOLLOWLOCATION         = 52;
constexpr int  CURLOPT_TIMEOUT                = 13;
constexpr int  CURLOPT_LOW_SPEED_LIMIT        = 19;
constexpr int  CURLOPT_LOW_SPEED_TIME         = 20;
constexpr int  CURLOPT_CONNECTTIMEOUT         = 78;
constexpr int  CURLOPT_NOSIGNAL               = 99;
constexpr int  CURLOPT_MAXREDIRS              = 68;
constexpr int  CURLOPT_REDIR_PROTOCOLS        = 182;    // deprecated in 7.85, still honoured
constexpr int  CURLOPT_REDIR_PROTOCOLS_STR    = 10319;  // the replacement, 7.85+
constexpr int  CURLINFO_RESPONSE_CODE         = 2097154; // CURLINFO_LONG + 2
constexpr long kCurlGlobalDefault             = 3;       // CURL_GLOBAL_ALL
constexpr long kProtoHttp                     = 1L << 0; // CURLPROTO_HTTP
constexpr long kProtoHttps                    = 1L << 1; // CURLPROTO_HTTPS

#if defined(FWOG_CURL_HEADER_PRESENT)
// The names above shadow curl.h's enumerators inside this namespace, so the
// header's own values are reached through ::.
static_assert(CURLOPT_URL                 == ::CURLOPT_URL);
static_assert(CURLOPT_WRITEDATA           == ::CURLOPT_WRITEDATA);
static_assert(CURLOPT_USERAGENT           == ::CURLOPT_USERAGENT);
static_assert(CURLOPT_WRITEFUNCTION       == ::CURLOPT_WRITEFUNCTION);
static_assert(CURLOPT_FOLLOWLOCATION      == ::CURLOPT_FOLLOWLOCATION);
static_assert(CURLOPT_TIMEOUT             == ::CURLOPT_TIMEOUT);
static_assert(CURLOPT_LOW_SPEED_LIMIT     == ::CURLOPT_LOW_SPEED_LIMIT);
static_assert(CURLOPT_LOW_SPEED_TIME      == ::CURLOPT_LOW_SPEED_TIME);
static_assert(CURLOPT_CONNECTTIMEOUT      == ::CURLOPT_CONNECTTIMEOUT);
static_assert(CURLOPT_NOSIGNAL            == ::CURLOPT_NOSIGNAL);
static_assert(CURLOPT_MAXREDIRS           == ::CURLOPT_MAXREDIRS);
// Naming the deprecated enumerator is the point of this line -- it is the
// pre-7.85 fallback in httpGet() and its number has to be right too -- so the
// deprecation notice is suppressed for exactly this check and nothing else.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
static_assert(CURLOPT_REDIR_PROTOCOLS     == ::CURLOPT_REDIR_PROTOCOLS);
#pragma GCC diagnostic pop
static_assert(CURLOPT_REDIR_PROTOCOLS_STR == ::CURLOPT_REDIR_PROTOCOLS_STR);
static_assert(CURLINFO_RESPONSE_CODE      == ::CURLINFO_RESPONSE_CODE);
static_assert(kCurlGlobalDefault          == CURL_GLOBAL_DEFAULT);
static_assert(kProtoHttp                  == CURLPROTO_HTTP);
static_assert(kProtoHttps                 == CURLPROTO_HTTPS);
#endif

const Curl& curl()
{
    // A function-local static: C++ guarantees exactly one thread runs this
    // initialiser while every other thread blocks until it finishes. That
    // guarantee is the reason the two process-wide setup steps at the end
    // belong HERE and not at each call site -- done once, they cannot race.
    static const Curl c = [] {
        Curl x;
#if defined(__APPLE__)
        // /usr/lib/libcurl.4.dylib ships with macOS itself (dyld shared cache;
        // no file on disk since Big Sur, but dlopen resolves it regardless), so
        // unlike Linux the "libcurl absent" arm should be unreachable in
        // practice. The ABSOLUTE path is deliberate and load-bearing: a
        // slash-less dlopen on macOS searches DYLD_LIBRARY_PATH and the
        // CURRENT WORKING DIRECTORY before the fallback path -- and v2 fetches
        // the remote catalog at first launch with no user action, so a bare
        // (un-hardened) binary started from a directory holding a planted
        // libcurl.4.dylib would load and run it. Linux never searches CWD,
        // which is why the leaf spelling is safe below but not here. The leaf
        // fallbacks remain for a hypothetical system without the shared-cache
        // path -- by then CWD planting is the least of that machine's oddness.
        for (const char* name : { "/usr/lib/libcurl.4.dylib",
                                  "libcurl.4.dylib", "libcurl.dylib" }) {
#else
        for (const char* name : { "libcurl.so.4", "libcurl.so", "libcurl.so.3" }) {
#endif
            x.lib = dlopen(name, RTLD_LAZY | RTLD_LOCAL);
            if (x.lib) break;
        }
        if (!x.lib) return x;
        x.global_init   = (decltype(x.global_init))   dlsym(x.lib, "curl_global_init");
        x.easy_init     = (decltype(x.easy_init))     dlsym(x.lib, "curl_easy_init");
        x.easy_setopt   = (decltype(x.easy_setopt))   dlsym(x.lib, "curl_easy_setopt");
        x.easy_perform  = (decltype(x.easy_perform))  dlsym(x.lib, "curl_easy_perform");
        x.easy_getinfo  = (decltype(x.easy_getinfo))  dlsym(x.lib, "curl_easy_getinfo");
        x.easy_cleanup  = (decltype(x.easy_cleanup))  dlsym(x.lib, "curl_easy_cleanup");
        x.easy_strerror = (decltype(x.easy_strerror)) dlsym(x.lib, "curl_easy_strerror");
        if (!x.ok()) return x;

        // curl_easy_init() calls curl_global_init() itself if nobody has, but
        // libcurl documents that implicit call as NOT thread-safe below 7.84,
        // and TWO worker threads here can reach httpGet at once -- a catalog
        // refresh (RemoteCatalog::run) overlapping a firmware download from a
        // catalog URL (FlashController's worker). Whichever of them arrives
        // first, this has already run. No matching curl_global_cleanup: the
        // library is never dlclose'd, and tearing global state down while
        // another thread might still be inside a transfer is worse than
        // letting the process exit with it.
        x.global_init(kCurlGlobalDefault);

        // Measured on this machine, not assumed: with CURLOPT_NOSIGNAL left at
        // its default of 0, libcurl sets SIGPIPE to SIG_IGN for the duration
        // of each transfer and restores the previous disposition afterwards.
        // With two threads that save/restore is a bug -- whichever transfer
        // finishes first restores SIG_DFL while the other is still running --
        // and libcurl-thread(3) says exactly that: setting NOSIGNAL to 0
        // "does not work in a threaded situation as there is a race condition
        // where libcurl risks restoring the former signal handler while
        // another thread should still ignore it".
        //
        // So every handle sets NOSIGNAL=1, and the protection libcurl was
        // providing is installed once and never taken back down -- once-only
        // is what removes the race. Without this the trade would be a bad one:
        // libcurl warns that the OpenSSL backend can still raise SIGPIPE, and
        // at SIG_DFL that kills the process outright.
        //
        // An existing handler is left alone. This only fills in the default
        // disposition; it does not overrule a choice something else made.
        struct sigaction cur = {};
        if (sigaction(SIGPIPE, nullptr, &cur) == 0 && cur.sa_handler == SIG_DFL) {
            struct sigaction ign = {};
            ign.sa_handler = SIG_IGN;
            sigemptyset(&ign.sa_mask);
            sigaction(SIGPIPE, &ign, nullptr);
        }
        return x;
    }();
    return c;
}

/// libcurl calls this from C, so nothing may escape it: an exception unwinding
/// through libcurl's own frames is undefined behaviour. The one thing here
/// that can throw is the append -- std::bad_alloc or std::length_error on a
/// response larger than this process can hold, and the server alone chooses
/// that size. Returning a short count is libcurl's documented way to say
/// "stop", which arrives back in httpGet() as an ordinary CURLE_WRITE_ERROR
/// and then as an error string. The alternative is a crash.
size_t writeCb(char* ptr, size_t size, size_t nmemb, void* userdata)
{
    const size_t n = size * nmemb;
    try {
        static_cast<std::string*>(userdata)->append(ptr, n);
    } catch (...) {
        return 0;   // differs from n whenever n > 0, which aborts the transfer
    }
    return n;
}

} // namespace

bool httpAvailable() { return curl().ok(); }

std::expected<std::string, std::string> httpGet(const std::string& url)
{
    if (auto bad = checkUrl(url); !bad.has_value()) return std::unexpected(bad.error());

    const Curl& c = curl();
    if (!c.ok())
        return std::unexpected("libcurl is not installed, so the remote catalog is "
                               "unavailable; embedded and local firmware still work");

    void* h = c.easy_init();
    if (!h) return std::unexpected("libcurl could not be initialised");

    std::string body;

    // Every setopt result is checked. Handing libcurl an option number it does
    // not recognise fails and CHANGES NOTHING, which is indistinguishable from
    // success at the call site -- the transfer would then run with a default
    // in place of the bound that was asked for, and for the redirect options
    // below that is the difference between a bounded fetch and an unbounded
    // one. Stopping is better than fetching under settings nobody chose.
    //
    // The literal suffixes matter and are not decoration: libcurl reads each
    // value off a varargs list using the type its option NUMBER implies, with
    // no way to check. Numbers under 10000 are read as `long`, so 1L/10L/30L
    // must be long and not int; 10000-19999 are read as a pointer; 20000+ as a
    // function pointer. An `int` where a `long` is expected reads adjacent
    // stack as the high half of the value.
    int rc = 0;
    auto set = [&](auto... args) { if (rc == 0) rc = c.easy_setopt(h, args...); };

    set(CURLOPT_URL,           url.c_str());
    set(CURLOPT_WRITEFUNCTION, writeCb);
    set(CURLOPT_WRITEDATA,     &body);
    set(CURLOPT_USERAGENT,     "fwOGAppExplorer/1.0");
    set(CURLOPT_NOSIGNAL,      1L);   // see curl(): required off the main thread
    set(CURLOPT_FOLLOWLOCATION, 1L);

    // Time limits. The failure condition has to be "the transfer stalled", not
    // "the file was big", because this function does not only fetch apps.json
    // -- fwFlashController's loadImageProd() pulls FIRMWARE IMAGES through it,
    // and those are the largest thing the app moves: the two images in this
    // repo are 4,917,760 and 16,424,448 bytes.
    //
    // A single CURLOPT_TIMEOUT of 30s, which is what this branch used to have,
    // is a TOTAL wall-clock cap, so it turned size into a deadline:
    // 16,424,448 / 30 = 547,481 B/s, i.e. the 16 MB image needed a sustained
    // ~535 KiB/s or Linux aborted a download Windows completes. WinHTTP has no
    // total at all; its limits are per-phase and reset on progress (connect
    // 60s, send 30s, receive 30s), so a slow-but-alive transfer runs as long
    // as it needs and only a stalled one dies. These three options model that.
    //
    //  * Stall detector, the one that does the real work. libcurl aborts when
    //    the average rate stays BELOW 1024 B/s for 30 continuous seconds. The
    //    30 is WinHTTP's receive timeout exactly. The 1024 is the "definitely
    //    dead, not merely slow" line: a live TCP transfer does not average
    //    under 1 KiB/s for thirty unbroken seconds unless the peer has stopped
    //    sending. What it costs on a genuinely slow link is nothing -- any
    //    link that sustains 1 KiB/s or better never trips this, however long
    //    it takes.
    //  * Connect timeout, 60s, matching WinHTTP's default. This is newly
    //    REQUIRED rather than merely tidy: the old 30s total was what bounded
    //    a connect to an unreachable host, and raising the total takes that
    //    away. Neither the stall detector nor the total is a good bound here
    //    -- the stall detector only runs once a transfer is underway.
    //  * Total backstop, one hour. Kept for one specific reason: both callers
    //    JOIN their worker in a destructor (RemoteCatalog::shutdown, and
    //    FlashController's, whose comment already names "whatever blocking
    //    call is in flight" as the cost), so an unbounded httpGet is an
    //    unbounded app shutdown. It is sized so it can never be the thing that
    //    fails a real download: 16,424,448 / 3600 = 4,562 B/s, so the largest
    //    image still completes on any link averaging 4.5 KiB/s -- far below
    //    the 1 KiB/s floor the stall detector already tolerates. It therefore
    //    only ever catches a peer trickling just fast enough to dodge the
    //    stall detector, which is the case that would otherwise hang exit.
    //
    //    State the cost of that plainly rather than leaving it implied: in
    //    exactly that case, the worst-case app exit went from 30 seconds to an
    //    hour. httpGet has no cancellation hook, so FlashController's
    //    m_cancelRequested -- documented as a "best-effort cooperative stop"
    //    -- cannot reach a transfer already in flight, on any platform. The
    //    same is true of the WinHTTP branch, so this is not a regression
    //    against the reference; it is a pre-existing design limit whose worst
    //    case this change made 120x longer on one path.
    //
    //    The fix is not a smaller number here -- that would just reintroduce
    //    "big download killed for being big" at a different size. It is
    //    CURLOPT_XFERINFOFUNCTION returning non-zero when a stop is asked for,
    //    which would abort promptly and make BOTH this backstop and the
    //    Cancel button real. That needs a cancellation signal plumbed from the
    //    controllers into this function, which is an API change belonging to
    //    the flash component rather than to the transport, so it is recorded
    //    here rather than done here.
    set(CURLOPT_LOW_SPEED_LIMIT, 1024L);
    set(CURLOPT_LOW_SPEED_TIME,  30L);
    set(CURLOPT_CONNECTTIMEOUT,  60L);
    set(CURLOPT_TIMEOUT,         3600L);

    // Match the two redirect guarantees the WinHTTP branch has always had for
    // free, neither of which this branch had:
    //
    //  * At most 10 hops. WinHTTP's WINHTTP_OPTION_MAX_HTTP_AUTOMATIC_REDIRECTS
    //    defaults to 10. libcurl's own MAXREDIRS default is documented as "30
    //    (since 8.3.0), it was previously unlimited" -- so leaving it unset
    //    made the bound depend on which libcurl the user happened to have, and
    //    on an older one there was no bound at all.
    //  * No redirect off https onto plain http. WinHTTP's redirect policy
    //    defaults to DISALLOW_HTTPS_TO_HTTP. libcurl's default allows "HTTP,
    //    HTTPS, FTP and FTPS" on redirect, so an https catalog URL could be
    //    walked down to plain http, or sideways onto ftp, by the server.
    //
    // Where this is still NOT identical: WinHTTP applies the https-to-http
    // rule per hop, so it refuses the last leg of http -> https -> http, which
    // the allow-list here permits. libcurl has no per-hop form of the rule, and
    // hand-rolling the redirect loop with CURLINFO_REDIRECT_URL to gain it
    // would be a great deal of machinery for no protection, which is worth
    // spelling out because the obvious reason is the wrong one.
    //
    // The wrong reason: "a catalog URL cannot be plain http anyway." True of
    // the CATALOG -- normalizeRemoteCatalogUrl refuses http and fwApp.cpp
    // re-normalizes on load, so a hand-edited settings.ini cannot smuggle one
    // in -- but httpGet's other caller is loadImageProd
    // (fwFlashController.cpp), which fetches FIRMWARE IMAGES from an
    // ImageRef::url taken verbatim out of catalog JSON with no scheme check
    // anywhere. Plain-http image URLs are reachable today. That path is the
    // one where bytes end up on a board, so it is precisely the path a per-hop
    // rule would be for.
    //
    // The real reason: a chain that STARTS at http has already sent hop 0 in
    // plaintext. Refusing to come back down to http later protects nothing an
    // attacker could not have had at the first hop. Per-hop enforcement earns
    // its keep only when the chain starts encrypted, and that case the
    // allow-list already refuses outright.
    //
    // What would actually close the image path is validating ImageRef::url's
    // scheme where the catalog is parsed, not more machinery here. That is a
    // catalog concern and is left to it, deliberately and not by oversight.
    set(CURLOPT_MAXREDIRS, 10L);

    // REDIR_PROTOCOLS_STR is the current option and REDIR_PROTOCOLS the
    // deprecated one it replaced; this code dlopen's whatever libcurl the
    // system has, which may predate 7.85 or may one day drop the old option
    // entirely. Try the modern one, fall back to the old one, and let rc carry
    // a genuine failure of both rather than quietly redirecting unrestricted.
    const bool plainOk = mayRedirectToPlainHttp(url);
    if (rc == 0) {
        rc = c.easy_setopt(h, CURLOPT_REDIR_PROTOCOLS_STR,
                           plainOk ? "http,https" : "https");
        if (rc != 0)
            rc = c.easy_setopt(h, CURLOPT_REDIR_PROTOCOLS,
                               plainOk ? (kProtoHttp | kProtoHttps) : kProtoHttps);
    }

    if (rc != 0) {
        const std::string why = c.easy_strerror(rc);
        c.easy_cleanup(h);
        return std::unexpected("libcurl rejected a required option: " + why);
    }

    rc = c.easy_perform(h);
    long status = 0;
    if (rc == 0) c.easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
    // easy_strerror returns a pointer into libcurl's own static tables, which
    // outlive the handle, but copy before cleanup anyway rather than depend on
    // that. The handle is released on every path, including the error ones.
    const std::string why = (rc != 0) ? c.easy_strerror(rc) : std::string{};
    c.easy_cleanup(h);

    if (rc != 0)
        return std::unexpected("the request failed: " + why);
    if (status < 200 || status >= 300)
        return std::unexpected("the server returned HTTP " + std::to_string(status));
    return body;
}

} // namespace fwog

#endif
