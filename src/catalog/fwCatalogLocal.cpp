#include "catalog/fwCatalogLocal.h"

#include "catalog/fwCatalogJson.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <span>
#include <sstream>

namespace fwog {
namespace {

std::string readFile(const std::filesystem::path& p)
{
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// The ".uf2" check stays a case-insensitive string compare deliberately, on
// both platforms: it only decides what gets *considered* for parsing, and
// parseUf2 gates the actual content, so accepting "FOO.UF2" on Linux is a
// courtesy, not a correctness hazard. This is intentionally asymmetric with
// the file-identity dedup below, which cannot use a string compare at all.
bool hasUf2Extension(const std::filesystem::path& p)
{
    std::string ext = p.extension().string();
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == ".uf2";
}

} // namespace

CatalogEntry unlistedEntryFor(const std::filesystem::path& file, const Uf2Info& info)
{
    CatalogEntry e;
    e.slug   = file.stem().string();
    e.name   = e.slug;
    e.source = CatalogSource::Unlisted;
    e.category = "Unlisted";
    e.tagline  = "A UF2 found in the catalog folder, with no catalog entry.";

    std::ostringstream d;
    d << "This file is not described by catalog.json, so everything below comes "
         "from the UF2 header itself.\n\n"
      << "Blocks: " << info.numBlocks << "\n"
      << "Payload: " << info.payloadBytes << " bytes\n";
    if (info.familyPresent) {
        d << "Target address: 0x" << std::hex << info.targetAddr << std::dec << "\n"
          << "Family: RP2040\n";
    } else {
        d << "Target address: 0x" << std::hex << info.targetAddr << std::dec << "\n"
          << "Family: no family ID present in this image\n";
    }
    e.description = d.str();

    // OgApp is the safe default: it targets only the main CPU. Marked inferred
    // so the UI can say the scheme was guessed and offer the gated retarget.
    e.scheme = FlashScheme::OgApp;
    e.schemeInferred = true;

    Uf2Asset a;
    a.cpu = TargetCpu::Main;
    a.ref.localPath = file.string();
    a.size = info.numBlocks * 512ull;
    e.uf2.push_back(std::move(a));

    return e;
}

std::vector<CatalogEntry> loadLocalCatalog(const std::filesystem::path& dir)
{
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) return {};

    std::vector<CatalogEntry> out;
    std::vector<std::filesystem::path> describedPaths;

    if (const auto jsonPath = dir / "catalog.json"; std::filesystem::exists(jsonPath, ec)) {
        if (auto parsed = parseCatalogJson(readFile(jsonPath), CatalogSource::Local)) {
            for (auto& e : *parsed) {
                // Resolve relative paths against the catalog directory, so
                // catalog.json can say "blinky.uf2" and mean the file next to
                // it. A path that is already absolute is left alone.
                for (auto& a : e.uf2) {
                    if (a.ref.localPath.empty()) continue;
                    std::filesystem::path p(a.ref.localPath);
                    if (p.is_relative()) p = dir / p;
                    a.ref.localPath = p.string();
                    describedPaths.push_back(p);
                }
                out.push_back(std::move(e));
            }
        }
    }

    for (const auto& de : std::filesystem::directory_iterator(dir, ec)) {
        if (!de.is_regular_file()) continue;
        const auto& p = de.path();
        if (!hasUf2Extension(p)) continue;

        // Identity, not name, decides whether this loose file was already
        // described. std::filesystem::equivalent asks the filesystem itself
        // (same inode on Linux, same file ID on Windows), which is correct
        // on both: case-insensitive exactly where Windows filenames are,
        // case-sensitive exactly where Linux filenames are -- so
        // "Blinky.uf2" and "blinky.uf2" collapse to one file on Windows and
        // stay two distinct, both-listed files on Linux, with no #ifdef. It
        // also means an absolute path in catalog.json that names a file
        // outside `dir` can never falsely suppress an unrelated loose file
        // that merely happens to share a basename -- only a real match does.
        // equivalent() sets ec when either path doesn't exist; a described
        // entry pointing at a missing file must not suppress a real loose
        // one, so that is treated as "not the same file" and scanning
        // continues.
        bool alreadyDescribed = false;
        for (const auto& described : describedPaths) {
            std::error_code eqEc;
            if (std::filesystem::equivalent(p, described, eqEc) && !eqEc) {
                alreadyDescribed = true;
                break;
            }
        }
        if (alreadyDescribed) continue;

        const std::string bytes = readFile(p);
        auto info = parseUf2(std::span<const uint8_t>(
            reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()));
        if (!info) continue;   // not a usable UF2; leave it out rather than
                                // offering something that cannot be flashed
        out.push_back(unlistedEntryFor(p, *info));
    }

    return out;
}

std::string LocalCatalogCache::fingerprintOf(const std::filesystem::path& dir)
{
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) return {};

    // Every regular file, not only the .uf2 ones: catalog.json's contents
    // decide what loadLocalCatalog() produces (and which loose files it
    // suppresses as already-described) just as much as the images do, and an
    // edit to it must be picked up on the next frame like everything else.
    std::vector<std::string> lines;
    for (const auto& de : std::filesystem::directory_iterator(dir, ec)) {
        if (!de.is_regular_file()) continue;

        // Both queries can fail -- a file deleted between the iteration and
        // the stat, a permission problem -- and both are asked with an
        // error_code so that is not an exception. A failed query contributes a
        // sentinel rather than being skipped: "this file exists but could not
        // be measured" is itself a state that must be distinguishable from
        // "this file is not here", or a file that flickers in and out of
        // readability would silently keep serving a stale catalog.
        std::error_code sizeEc, timeEc;
        const auto size = std::filesystem::file_size(de.path(), sizeEc);
        const auto time = std::filesystem::last_write_time(de.path(), timeEc);

        std::string line = de.path().filename().string();
        line += '|';
        line += sizeEc ? std::string("?")
                        : std::to_string(static_cast<unsigned long long>(size));
        line += '|';
        // The cast is load-bearing on libc++, where file_clock's rep is
        // __int128 -- a type std::to_string has no overload for, making the
        // bare call ambiguous. Truncation is no concern for a fingerprint:
        // the value only has to change when the mtime changes.
        line += timeEc ? std::string("?")
                        : std::to_string(static_cast<long long>(
                              time.time_since_epoch().count()));
        lines.push_back(std::move(line));
    }

    // directory_iterator's order is unspecified, so it is not safe to treat as
    // part of the fingerprint -- sort, or an implementation that reorders
    // entries between calls would look like a change that never happened.
    std::sort(lines.begin(), lines.end());

    std::string out;
    for (const auto& line : lines) {
        out += line;
        out += '\n';
    }
    return out;
}

const std::vector<CatalogEntry>& LocalCatalogCache::entries(const std::filesystem::path& dir)
{
    std::string fingerprint = fingerprintOf(dir);
    if (m_primed && dir == m_dir && fingerprint == m_fingerprint)
        return m_entries;

    m_dir         = dir;
    m_fingerprint = std::move(fingerprint);
    m_entries     = loadLocalCatalog(dir);
    m_primed      = true;
    ++m_rescans;
    return m_entries;
}

} // namespace fwog
