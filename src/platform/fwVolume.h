#pragma once

#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fwog {

/// Every mounted RP2040 bootrom volume.
///
/// Returning more than one is meaningful and must never be collapsed to the
/// first: two mounted volumes cannot be told apart, and the caller refuses on
/// exactly that basis.
///
/// A volume qualifies by being a bootrom volume, NOT by being mounted at a path
/// that happens to be spelled `RPI-RP2`: Windows asks `GetDriveTypeA() ==
/// DRIVE_REMOVABLE` and reads the volume label, and Linux reads the Board-ID out
/// of the volume's own INFO_UF2.TXT. Both are properties of the device. A mount
/// POINT is a name some automounter chose, and on Linux it is neither necessary
/// nor sufficient -- see detail::selectRpiRp2Volumes() in fwVolume.cpp for the
/// two measured ways it goes wrong and for what the undercount actually cost.
///
/// The Linux branch is a thin wrapper: it supplies the real /proc/mounts and the
/// real INFO_UF2.TXT to detail::selectRpiRp2Volumes(), which makes the decision.
/// This signature is unchanged and is what every caller uses.
std::vector<std::string> findRpiRp2Volumes();

/// How many CPUs are sitting in the RP2040 bootrom right now, counted from the
/// USB device tree rather than from anything having mounted them.
///
/// This is the other half of "no volume appeared". A board in BOOTSEL whose
/// drive nothing mounted is INDISTINGUISHABLE, to findRpiRp2Volumes(), from no
/// board at all -- and the honest message for the two cases is not the same
/// one. Returns 0 where the question cannot be asked (Windows, Emscripten),
/// which makes unmountedBootselNotice() below say nothing.
int countBootselDevices();

namespace detail {

/// One parsed `/proc/mounts` line: the first three fields, unescaped.
struct MountLine {
    std::string device;      ///< e.g. /dev/sda1
    std::string mountPoint;  ///< e.g. /run/media/you/RPI-RP2
    std::string fsType;      ///< e.g. vfat
};

/// Split one `/proc/mounts` line. nullopt when it has fewer than three fields.
std::optional<MountLine> parseMountLine(std::string_view line);

/// Is this the content of an RP2040 bootrom's `INFO_UF2.TXT`?
///
/// True only for a `Board-ID:` line naming `RPI-RP2`. Other vendors' UF2
/// bootloaders publish an INFO_UF2.TXT with the same banner and a different
/// Board-ID, and must not be offered as a FreeWili to write to.
bool isRp2BootromInfo(std::string_view infoUf2Txt);

/// Which mount tooling the remedy half of unmountedBootselNotice() should
/// name. A value rather than an #ifdef inside the function, so both wordings
/// compile and are tested on every platform; kNativeMountRemedy below is the
/// one decision point that picks per-OS.
enum class MountRemedy { Udisks, Diskutil };

/// The remedy this build's OS can actually follow. macOS has no udisksctl and
/// no lsblk; Linux has no diskutil. (Windows never shows the notice --
/// countBootselDevices() answers 0 there -- so its value is moot.)
inline constexpr MountRemedy kNativeMountRemedy =
#if defined(__APPLE__)
    MountRemedy::Diskutil;
#else
    MountRemedy::Udisks;
#endif

/// Explain BOOTSEL devices that no mounted volume accounts for; empty when
/// there is nothing to explain. Pure, so the wording is tested directly.
std::string unmountedBootselNotice(int bootselDevices, size_t volumesFound,
                                   MountRemedy remedy = kNativeMountRemedy);

/// Decode `/proc/mounts`' octal escaping of space, tab, newline and
/// backslash (`\040`, `\011`, `\012`, `\134`). Pure, so it is testable on
/// Windows even though its only caller -- the Linux branch of
/// findRpiRp2Volumes -- is not. A malformed or truncated escape (too few
/// digits, or a non-octal digit) is left untouched rather than guessed at.
std::string unescapeMount(std::string_view s);

/// The exact inverse: encode space, tab, newline and backslash the way the
/// kernel writes them into `/proc/mounts`. Its only caller is the macOS
/// readMounts, which renders getmntinfo()'s table into /proc/mounts's line
/// format so the shared parser and filters need no second implementation --
/// but it is pure string logic, so it lives here and the round trip is
/// pinned by tests on every platform.
std::string escapeMount(std::string_view s);

/// Everything the Linux volume scan reads from the machine, in one injectable
/// place -- the same discipline, and for the same reason, as ProbeIo
/// (fwCpuProbe.h) and FlashIo (fwFlashEngine.h). findRpiRp2Volumes() supplies
/// the real readers; tests supply fakes.
///
/// This exists because the two FILTERS are the whole safety content of the scan
/// and they were previously untestable. With `/proc/mounts` opened inline there
/// was no seam, so both filters could be deleted outright and the suite stayed
/// green -- measured, by deleting each in turn. A filter no test can kill is a
/// filter no test is protecting.
struct VolumeIo {
    /// The entire text of `/proc/mounts`, or empty when it cannot be read.
    /// Whole-file rather than line-by-line so a test can hand over a fixture
    /// and so the reading is one syscall's worth of a consistent snapshot.
    std::function<std::string()> readMounts;

    /// The contents of `<mountPoint>/INFO_UF2.TXT`, or nullopt when there is no
    /// such file or it cannot be opened. The production reader bounds the read
    /// (the real file is 62 bytes; nothing about a volume the user plugged in
    /// should be trusted to be small).
    ///
    /// Called ONLY for mounts that already passed the filesystem-type filter.
    /// That ordering is a property tests assert directly, not an incidental one:
    /// this is the only I/O in the scan that can block, and the scan runs in a
    /// ~250 ms poll loop on the flash worker.
    std::function<std::optional<std::string>(const std::string& mountPoint)> readInfoUf2;
};

/// The mount-table half of findRpiRp2Volumes(), with the machine behind `io`.
///
/// Deliberately compiled on every platform even though only the Linux branch of
/// findRpiRp2Volumes() calls it: it contains no platform API, and the filters it
/// applies are exactly the ones whose deletion must fail a test. Keeping it
/// unconditional means those tests run in the Windows CI too.
std::vector<std::string> selectRpiRp2Volumes(const VolumeIo& io);

} // namespace detail

/// Copy `src` into `volume`, confirm the result, and do not return until the
/// bytes have reached the device. See fwVolume.cpp for what "reached the
/// device" was measured to mean here.
std::expected<void, std::string> copyToVolume(const std::filesystem::path& src,
                                              const std::string& volume);

} // namespace fwog
