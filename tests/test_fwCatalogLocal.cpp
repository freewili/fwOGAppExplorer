#include <doctest/doctest.h>
#include "catalog/fwCatalogLocal.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

using namespace fwog;

TEST_CASE("an undescribed uf2 becomes an Unlisted entry named after the file") {
    Uf2Info info;
    info.numBlocks = 1024;
    info.targetAddr = 0x10000000;
    info.familyPresent = true;
    info.payloadBytes = 1024 * 256;

    auto e = unlistedEntryFor("C:/x/catalog/my-cool-app.uf2", info);
    CHECK(e.source == CatalogSource::Unlisted);
    CHECK(e.slug == "my-cool-app");
    CHECK(e.name == "my-cool-app");
    REQUIRE(e.uf2.size() == 1);
    CHECK(e.uf2[0].ref.localPath == "C:/x/catalog/my-cool-app.uf2");
}

TEST_CASE("an unlisted entry defaults to the safe scheme and marks it inferred") {
    // OgApp targets only the main CPU. An arbitrary .uf2 dropped in a folder
    // carries no evidence of which CPU it belongs to, so it must never default
    // to anything that can write the display.
    Uf2Info info;
    info.numBlocks = 4;
    auto e = unlistedEntryFor("x/foo.uf2", info);
    CHECK(e.scheme == FlashScheme::OgApp);
    CHECK(e.schemeInferred == true);
    CHECK(e.uf2[0].cpu == TargetCpu::Main);
}

TEST_CASE("the description records what the header actually said") {
    Uf2Info info;
    info.numBlocks = 512;
    info.targetAddr = 0x10000000;
    info.familyPresent = true;
    auto e = unlistedEntryFor("x/foo.uf2", info);
    CHECK(e.description.find("512") != std::string::npos);
    CHECK(e.description.find("0x10000000") != std::string::npos);
}

TEST_CASE("an image with no family ID says so in the description") {
    Uf2Info info;
    info.numBlocks = 4;
    info.familyPresent = false;
    auto e = unlistedEntryFor("x/foo.uf2", info);
    CHECK(e.description.find("no family ID") != std::string::npos);
}

TEST_CASE("scanning a directory that does not exist yields nothing") {
    CHECK(loadLocalCatalog("C:/definitely/not/here").empty());
}

// ---------------------------------------------------------------------------
// loadLocalCatalog: catalog.json + directory-scan behaviour. These need real
// files on disk, since parseUf2 (called on every loose .uf2) validates real
// header bytes and std::filesystem::equivalent (the dedup mechanism) needs
// paths that actually exist.
// ---------------------------------------------------------------------------
namespace {

constexpr uint32_t kMagic0        = 0x0A324655;
constexpr uint32_t kMagic1        = 0x9E5D5157;
constexpr uint32_t kMagicEnd      = 0x0AB16F30;
constexpr uint32_t kRp2040        = 0xE48BFF56;
constexpr uint32_t kFamilyPresent = 0x00002000;

void put32(std::vector<uint8_t>& v, size_t off, uint32_t val) {
    v[off + 0] = uint8_t(val & 0xFF);
    v[off + 1] = uint8_t((val >> 8) & 0xFF);
    v[off + 2] = uint8_t((val >> 16) & 0xFF);
    v[off + 3] = uint8_t((val >> 24) & 0xFF);
}

/// Build a syntactically valid, single-family UF2 of `blocks` blocks (same
/// shape as tests/test_fwUf2Header.cpp's helper -- kept local since this file
/// cannot include another test's translation unit).
std::vector<uint8_t> makeUf2(uint32_t blocks) {
    std::vector<uint8_t> v(size_t(blocks) * 512, 0);
    for (uint32_t i = 0; i < blocks; ++i) {
        size_t b = size_t(i) * 512;
        put32(v, b + 0, kMagic0);
        put32(v, b + 4, kMagic1);
        put32(v, b + 8, kFamilyPresent);
        put32(v, b + 12, 0x10000000 + i * 256);
        put32(v, b + 16, 256);
        put32(v, b + 20, i);
        put32(v, b + 24, blocks);
        put32(v, b + 28, kRp2040);
        put32(v, b + 508, kMagicEnd);
    }
    return v;
}

void writeFile(const std::filesystem::path& p, const std::vector<uint8_t>& bytes) {
    std::ofstream out(p, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), (std::streamsize)bytes.size());
}

void writeText(const std::filesystem::path& p, const std::string& text) {
    std::ofstream out(p, std::ios::binary);
    out << text;
}

/// A directory under the system temp dir, unique to this test file, emptied
/// before every use so tests never see a previous run's leftovers.
std::filesystem::path freshTestDir(const char* name) {
    auto dir = std::filesystem::temp_directory_path() / "fwog_test_fwCatalogLocal" / name;
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

} // namespace

TEST_CASE("a relative localPath in catalog.json resolves against the catalog directory") {
    auto dir = freshTestDir("relative");
    writeFile(dir / "blinky.uf2", makeUf2(2));
    writeText(dir / "catalog.json",
        R"({"apps":[{"slug":"blinky","name":"Blinky","uf2":[{"cpu":"main","path":"blinky.uf2"}]}]})");

    auto entries = loadLocalCatalog(dir);

    REQUIRE(entries.size() == 1);
    CHECK(entries[0].slug == "blinky");
    REQUIRE(entries[0].uf2.size() == 1);
    CHECK(entries[0].uf2[0].ref.localPath == (dir / "blinky.uf2").string());
    CHECK(std::filesystem::path(entries[0].uf2[0].ref.localPath).is_absolute());

    std::filesystem::remove_all(dir);
}

TEST_CASE("an absolute localPath in catalog.json passes through unchanged") {
    auto dir = freshTestDir("absolute");
    auto absPath = dir / "abs-app.uf2";
    writeFile(absPath, makeUf2(3));

    std::ostringstream json;
    json << R"({"apps":[{"slug":"abs-app","name":"Abs App","uf2":[{"cpu":"main","path":")"
         << absPath.generic_string() << R"("}]}]})";
    writeText(dir / "catalog.json", json.str());

    auto entries = loadLocalCatalog(dir);

    REQUIRE(entries.size() == 1);
    CHECK(entries[0].slug == "abs-app");
    REQUIRE(entries[0].uf2.size() == 1);
    CHECK(entries[0].uf2[0].ref.localPath == absPath.generic_string());

    std::filesystem::remove_all(dir);
}

TEST_CASE("a file described by catalog.json is not also listed as Unlisted") {
    auto dir = freshTestDir("described-not-doubled");
    writeFile(dir / "blinky.uf2", makeUf2(2));
    writeText(dir / "catalog.json",
        R"({"apps":[{"slug":"blinky","name":"Blinky","uf2":[{"cpu":"main","path":"blinky.uf2"}]}]})");

    auto entries = loadLocalCatalog(dir);

    // Exactly one entry total: the described one. No second, Unlisted entry
    // for the same physical file.
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].slug == "blinky");
    CHECK(entries[0].source != CatalogSource::Unlisted);

    std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------------------
// LocalCatalogCache
//
// Final review (Fix 6): App::run() called loadLocalCatalog() inside the tab-bar
// loop, so it ran at FRAME RATE whenever App Explorer was the active tab --
// which it is by default. loadLocalCatalog() reads every loose .uf2 into memory
// in full and walks every 512-byte block; a real display image is 16.4 MB /
// 32,079 blocks, and dropping your own UF2 into catalog/ is the documented way
// to flash it. The comment calling that "cheap enough at this catalog's scale"
// was true when it described a directory listing and became false when UF2
// parsing moved into it.
//
// The requirement the fix has to keep: a file dropped into catalog/ while the
// app is running still appears, without a restart and without a delay. That is
// why this is a directory fingerprint and not a timer.
// ---------------------------------------------------------------------------

TEST_CASE("the cache returns the same catalog without re-reading an unchanged directory") {
    auto dir = freshTestDir("cache-unchanged");
    writeFile(dir / "blinky.uf2", makeUf2(2));

    LocalCatalogCache cache;
    CHECK(cache.rescans() == 0);

    const auto first = cache.entries(dir);   // copied: the next call may rebuild
    CHECK(cache.rescans() == 1);
    REQUIRE(first.size() == 1);

    // Sixty more frames' worth of asking. Nothing on disk changed, so nothing
    // is read or parsed again -- this is the whole point of the class.
    for (int i = 0; i < 60; ++i) {
        const auto& again = cache.entries(dir);
        CHECK(again.size() == 1);
    }
    CHECK(cache.rescans() == 1);

    std::filesystem::remove_all(dir);
}

TEST_CASE("a file dropped into the directory still appears on the very next call") {
    // The behaviour a rescan throttle would have broken: no restart, and no
    // "wait a second and it will turn up" either.
    auto dir = freshTestDir("cache-newfile");
    writeFile(dir / "blinky.uf2", makeUf2(2));

    LocalCatalogCache cache;
    REQUIRE(cache.entries(dir).size() == 1);
    REQUIRE(cache.rescans() == 1);

    writeFile(dir / "second.uf2", makeUf2(3));

    CHECK(cache.entries(dir).size() == 2);
    CHECK(cache.rescans() == 2);

    std::filesystem::remove_all(dir);
}

TEST_CASE("a file removed from the directory disappears on the very next call") {
    auto dir = freshTestDir("cache-removed");
    writeFile(dir / "blinky.uf2", makeUf2(2));
    writeFile(dir / "second.uf2", makeUf2(3));

    LocalCatalogCache cache;
    REQUIRE(cache.entries(dir).size() == 2);

    std::filesystem::remove(dir / "second.uf2");

    CHECK(cache.entries(dir).size() == 1);
    CHECK(cache.rescans() == 2);

    std::filesystem::remove_all(dir);
}

TEST_CASE("rewriting a file's contents at the same size is still picked up") {
    // Size alone is not the fingerprint: overwriting one 2-block UF2 with a
    // different 2-block UF2 leaves the size identical, and only the
    // last-write-time distinguishes them. A size-only key would serve a stale
    // parse of firmware that is no longer the file on disk.
    auto dir = freshTestDir("cache-samesize");
    writeFile(dir / "blinky.uf2", makeUf2(2));

    LocalCatalogCache cache;
    REQUIRE(cache.entries(dir).size() == 1);
    REQUIRE(cache.rescans() == 1);

    // Filesystem timestamps are not infinitely fine-grained; set it explicitly
    // rather than racing the clock, which is what a real edit does anyway.
    const auto path = dir / "blinky.uf2";
    writeFile(path, makeUf2(2));
    std::filesystem::last_write_time(
        path, std::filesystem::last_write_time(path) + std::chrono::seconds(5));

    CHECK(cache.entries(dir).size() == 1);
    CHECK(cache.rescans() == 2);

    std::filesystem::remove_all(dir);
}

TEST_CASE("editing catalog.json alone is picked up, not only the images") {
    // catalog.json decides what the images BECOME, and which loose files are
    // suppressed as already-described, so it has to be part of the fingerprint
    // even though it is not a .uf2.
    auto dir = freshTestDir("cache-json");
    writeFile(dir / "blinky.uf2", makeUf2(2));

    LocalCatalogCache cache;
    auto first = cache.entries(dir);
    REQUIRE(first.size() == 1);
    REQUIRE(first[0].source == CatalogSource::Unlisted);   // no JSON describes it yet

    writeText(dir / "catalog.json",
        R"({"apps":[{"slug":"blinky","name":"Blinky","uf2":[{"cpu":"main","path":"blinky.uf2"}]}]})");

    const auto& second = cache.entries(dir);
    REQUIRE(second.size() == 1);
    CHECK(second[0].source == CatalogSource::Local);        // now described
    CHECK(cache.rescans() == 2);

    std::filesystem::remove_all(dir);
}

TEST_CASE("pointing the cache at a different directory always rescans") {
    auto a = freshTestDir("cache-dir-a");
    auto b = freshTestDir("cache-dir-b");
    writeFile(a / "one.uf2", makeUf2(2));
    writeFile(b / "one.uf2", makeUf2(2));
    writeFile(b / "two.uf2", makeUf2(3));

    LocalCatalogCache cache;
    CHECK(cache.entries(a).size() == 1);
    // Same file count is not the question -- b is a DIFFERENT directory, and a
    // cache keyed only on a fingerprint could otherwise serve a's answer.
    CHECK(cache.entries(b).size() == 2);
    CHECK(cache.entries(a).size() == 1);
    CHECK(cache.rescans() == 3);

    std::filesystem::remove_all(a);
    std::filesystem::remove_all(b);
}

TEST_CASE("a directory that does not exist caches an empty catalog without churning") {
    LocalCatalogCache cache;
    CHECK(cache.entries("C:/definitely/not/here").empty());
    CHECK(cache.entries("C:/definitely/not/here").empty());
    CHECK(cache.rescans() == 1);
}

TEST_CASE("files whose names differ only in case are reported exactly as the filesystem stores them") {
    auto dir = freshTestDir("case-identity");

#if defined(__APPLE__)
    // macOS is the platform where case sensitivity is a PROPERTY OF THE
    // VOLUME, not the OS: APFS defaults to case-insensitive (the Windows
    // behaviour) but a case-sensitive APFS volume is a formatting checkbox
    // away, and this test directory lands on whichever kind the machine has.
    // So the expectation is read off the filesystem itself -- write both
    // spellings, count what actually exists -- and the assertion is that
    // loadLocalCatalog reports exactly the files that are really there,
    // which is the invariant both fixed-expectation branches below pin.
    writeFile(dir / "Blinky.uf2", makeUf2(1));
    writeFile(dir / "blinky.uf2", makeUf2(2));
    size_t onDisk = 0;
    for ([[maybe_unused]] const auto& de : std::filesystem::directory_iterator(dir))
        ++onDisk;
    auto entries = loadLocalCatalog(dir);
    REQUIRE(entries.size() == onDisk);
    for (const auto& e : entries) CHECK(e.source == CatalogSource::Unlisted);
#elif defined(_WIN32)
    // Windows filesystems are case-insensitive: "Blinky.uf2" and "blinky.uf2"
    // name the SAME physical file, so the second write overwrites the first
    // rather than creating a second one. This asserts that platform fact
    // holds through loadLocalCatalog -- exactly one entry, not two. The
    // Linux branch below is the actual regression test for Finding 1 (a
    // case-insensitive dedup key would have silently dropped one of two
    // genuinely distinct files); on Windows there is only ever one file to
    // begin with, so that scenario cannot be constructed here.
    writeFile(dir / "Blinky.uf2", makeUf2(1));
    writeFile(dir / "blinky.uf2", makeUf2(2));   // overwrites the same file
    auto entries = loadLocalCatalog(dir);
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].source == CatalogSource::Unlisted);
#else
    // Linux filesystems are case-sensitive: "Blinky.uf2" and "blinky.uf2" are
    // two distinct, unrelated firmware images that both genuinely exist.
    // Regression test for Finding 1: a case-insensitive dedup key would
    // treat these as the same file and silently drop one from the catalog.
    writeFile(dir / "Blinky.uf2", makeUf2(1));
    writeFile(dir / "blinky.uf2", makeUf2(2));
    auto entries = loadLocalCatalog(dir);
    REQUIRE(entries.size() == 2);
    CHECK(entries[0].source == CatalogSource::Unlisted);
    CHECK(entries[1].source == CatalogSource::Unlisted);
#endif

    std::filesystem::remove_all(dir);
}
