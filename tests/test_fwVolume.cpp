#include <doctest/doctest.h>
#include "platform/fwVolume.h"

#include <algorithm>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

using namespace fwog;

TEST_CASE("unescapeMount leaves a path with no escapes unchanged") {
    CHECK(detail::unescapeMount("/media/usb/RPI-RP2") == "/media/usb/RPI-RP2");
}

TEST_CASE("unescapeMount decodes an octal space escape") {
    CHECK(detail::unescapeMount("/media/john\\040doe/RPI-RP2") == "/media/john doe/RPI-RP2");
}

TEST_CASE("unescapeMount leaves a trailing backslash with too few digits alone") {
    // Only two digits follow the backslash before the string ends, so there
    // are not enough characters to form a 3-digit octal escape.
    CHECK(detail::unescapeMount("/media/foo\\04") == "/media/foo\\04");
}

TEST_CASE("unescapeMount leaves a non-octal digit after the backslash alone") {
    // '8' and '9' are not valid octal digits, so this is not a real escape
    // and must be passed through verbatim rather than guessed at.
    CHECK(detail::unescapeMount("/media/foo\\089bar") == "/media/foo\\089bar");
    CHECK(detail::unescapeMount("/media/foo\\099bar") == "/media/foo\\099bar");
}

TEST_CASE("escapeMount encodes exactly the characters the kernel escapes") {
    // A path with none of the four is untouched -- most mount points.
    CHECK(detail::escapeMount("/Volumes/RPI-RP2") == "/Volumes/RPI-RP2");
    CHECK(detail::escapeMount("a b") == "a\\040b");
    CHECK(detail::escapeMount("a\tb") == "a\\011b");
    CHECK(detail::escapeMount("a\nb") == "a\\012b");
    CHECK(detail::escapeMount("a\\b") == "a\\134b");
}

TEST_CASE("escapeMount and unescapeMount round-trip the second-volume path") {
    // The path the macOS readMounts comment stakes its claim on: a second
    // bootrom volume with the same label mounts at "/Volumes/RPI-RP2 1", and
    // the space must survive the render into /proc/mounts's line format and
    // back out of it -- truncating at the space would name the OTHER board's
    // volume.
    CHECK(detail::escapeMount("/Volumes/RPI-RP2 1") == "/Volumes/RPI-RP2\\0401");
    CHECK(detail::unescapeMount(detail::escapeMount("/Volumes/RPI-RP2 1")) ==
          "/Volumes/RPI-RP2 1");
    CHECK(detail::unescapeMount(detail::escapeMount("a \t\n\\z")) == "a \t\n\\z");
}

// --- parseMountLine ---------------------------------------------------------

TEST_CASE("parseMountLine splits the three fields it needs") {
    const auto m = detail::parseMountLine(
        "/dev/sda1 /run/media/you/RPI-RP2 vfat rw,nosuid,flush 0 0");
    REQUIRE(m.has_value());
    CHECK(m->device == "/dev/sda1");
    CHECK(m->mountPoint == "/run/media/you/RPI-RP2");
    CHECK(m->fsType == "vfat");
}

TEST_CASE("parseMountLine unescapes the device and the mount point") {
    // Verified end to end against a real bind mount in a private mount
    // namespace: the kernel writes `my\040drive` and the decoded path is the
    // one that exists on disk.
    const auto m = detail::parseMountLine(
        "tmpfs /home/you/my\\040drive/RPI-RP2 tmpfs rw,noatime 0 0");
    REQUIRE(m.has_value());
    CHECK(m->mountPoint == "/home/you/my drive/RPI-RP2");
    CHECK(m->fsType == "tmpfs");
}

TEST_CASE("parseMountLine rejects a line with fewer than three fields") {
    // A half-parsed line must not yield an entry with an empty mount point:
    // that would later be used as a relative path.
    CHECK_FALSE(detail::parseMountLine("").has_value());
    CHECK_FALSE(detail::parseMountLine("/dev/sda1").has_value());
    CHECK_FALSE(detail::parseMountLine("/dev/sda1 /mnt").has_value());
}

// --- isRp2BootromInfo -------------------------------------------------------

TEST_CASE("isRp2BootromInfo accepts the real bootrom INFO_UF2.TXT") {
    // Byte-for-byte what the attached FreeWili 1-OG's MAIN CPU presented in
    // BOOTSEL (62 bytes, LF endings).
    CHECK(detail::isRp2BootromInfo(
        "UF2 Bootloader v3.0\nModel: Raspberry Pi RP2\nBoard-ID: RPI-RP2\n"));
}

TEST_CASE("isRp2BootromInfo tolerates CRLF and a missing final newline") {
    CHECK(detail::isRp2BootromInfo(
        "UF2 Bootloader v3.0\r\nModel: Raspberry Pi RP2\r\nBoard-ID: RPI-RP2\r\n"));
    CHECK(detail::isRp2BootromInfo("Board-ID: RPI-RP2"));
}

TEST_CASE("isRp2BootromInfo rejects another vendor's UF2 bootloader") {
    // The banner and the Model line are not the board's identity. Adafruit's
    // and Microchip's UF2 bootloaders publish an INFO_UF2.TXT on a vfat volume
    // too, and an RP2040 image written to one is rejected by it as a foreign
    // family ID -- so it must never be offered as a FreeWili to write to.
    CHECK_FALSE(detail::isRp2BootromInfo(
        "UF2 Bootloader v3.0\nModel: Adafruit Feather M0\nBoard-ID: SAMD21G18A-Feather-v0\n"));
    CHECK_FALSE(detail::isRp2BootromInfo(
        "UF2 Bootloader v3.0\nModel: Raspberry Pi RP2\nBoard-ID: RP2350\n"));
}

TEST_CASE("isRp2BootromInfo rejects content with no Board-ID at all") {
    // An ordinary FAT filesystem that happens to have a file of this name.
    CHECK_FALSE(detail::isRp2BootromInfo(""));
    CHECK_FALSE(detail::isRp2BootromInfo("hello\n"));
    CHECK_FALSE(detail::isRp2BootromInfo("UF2 Bootloader v3.0\nModel: Raspberry Pi RP2\n"));
}

TEST_CASE("isRp2BootromInfo does not match a Board-ID that merely starts with RPI-RP2") {
    // udisks2 uniquifies a colliding MOUNT POINT by appending a digit
    // (RPI-RP21). The Board-ID inside the volume never changes, so a trailing
    // digit here is a different board, not a second FreeWili.
    CHECK_FALSE(detail::isRp2BootromInfo("Board-ID: RPI-RP21\n"));
    CHECK_FALSE(detail::isRp2BootromInfo("Board-ID: RPI-RP2-CLONE\n"));
}

TEST_CASE("isRp2BootromInfo ignores surrounding whitespace in the value") {
    CHECK(detail::isRp2BootromInfo("Board-ID:   RPI-RP2  \n"));
}

// --- selectRpiRp2Volumes ----------------------------------------------------
//
// The WIRING, which is the part that was previously untestable and therefore
// untested. Mutation testing on the old inline-/proc/mounts version found that
// BOTH filters could be deleted outright with the suite staying green:
//
//     fstype filter deleted from findRpiRp2Volumes         -> SURVIVED
//     isRp2BootromInfo call deleted from findRpiRp2Volumes -> SURVIVED
//
// The parser helpers above were well covered; nothing checked that
// findRpiRp2Volumes actually CALLED them. Each mutant now has a test below whose
// only job is to fail when that mutant is applied, and they are marked as such.

namespace {

/// A whole fake machine for detail::selectRpiRp2Volumes: a /proc/mounts text and
/// a set of volumes that have an INFO_UF2.TXT. It also RECORDS which mount
/// points were opened, because "the cheap filter runs first" is a property about
/// I/O that never happens and cannot be observed from the return value.
struct FakeMachine {
    std::string mounts;
    std::map<std::string, std::string> infoUf2;      ///< mount point -> file contents
    std::vector<std::string> infoReads;              ///< mount points readInfoUf2 was called for

    detail::VolumeIo io()
    {
        detail::VolumeIo v;
        v.readMounts = [this] { return mounts; };
        v.readInfoUf2 = [this](const std::string& mp) -> std::optional<std::string> {
            infoReads.push_back(mp);
            const auto it = infoUf2.find(mp);
            if (it == infoUf2.end()) return std::nullopt;
            return it->second;
        };
        return v;
    }

    bool opened(const std::string& mp) const
    {
        return std::find(infoReads.begin(), infoReads.end(), mp) != infoReads.end();
    }
};

/// Byte-for-byte what the attached FreeWili 1-OG's MAIN CPU presented in BOOTSEL.
constexpr const char* kRealBootromInfo =
    "UF2 Bootloader v3.0\nModel: Raspberry Pi RP2\nBoard-ID: RPI-RP2\n";

bool contains(const std::vector<std::string>& v, const std::string& s)
{
    return std::find(v.begin(), v.end(), s) != v.end();
}

} // namespace

TEST_CASE("selectRpiRp2Volumes returns BOTH bootrom volumes when two are mounted") {
    // The regression this whole change exists for. udisks2 uniquifies the second
    // colliding mount point by appending a digit, so the second FreeWili CPU in
    // BOOTSEL lands at .../RPI-RP21 -- which the old mount-point-spelling match
    // did not recognise, reporting one drive where there were two.
    FakeMachine m;
    m.mounts =
        "/dev/sda1 /run/media/you/RPI-RP2 vfat rw,nosuid,flush 0 0\n"
        "/dev/sdb1 /run/media/you/RPI-RP21 vfat rw,nosuid,flush 0 0\n";
    m.infoUf2["/run/media/you/RPI-RP2"]  = kRealBootromInfo;
    m.infoUf2["/run/media/you/RPI-RP21"] = kRealBootromInfo;

    const auto v = detail::selectRpiRp2Volumes(m.io());
    REQUIRE(v.size() == 2);
    CHECK(contains(v, "/run/media/you/RPI-RP2"));
    CHECK(contains(v, "/run/media/you/RPI-RP21"));
}

TEST_CASE("selectRpiRp2Volumes: the filesystem-type filter is load-bearing") {
    // KILLS THE MUTANT "fstype filter deleted".
    //
    // Two independent reasons the filter has to be there, asserted separately:
    //
    //  1. A non-FAT mount is not a bootrom volume however convincing its
    //     contents. The tmpfs bind mount below is the measured false positive
    //     that started all this -- a directory named RPI-RP2, with a file in it,
    //     that copyToVolume() reported a successful "flash" into.
    //  2. The filter is also the thing that keeps this scan off mounts that can
    //     BLOCK. An open() on a disconnected NFS or CIFS mount can hang for a
    //     long time, and this runs in a ~250 ms poll loop on the flash worker.
    //     That is a property of I/O NOT PERFORMED, so it is checked by asserting
    //     the reader was never called rather than by inspecting the result.
    FakeMachine m;
    m.mounts =
        "tmpfs /home/you/RPI-RP2 tmpfs rw,noatime 0 0\n"
        "server:/export /mnt/nfs nfs4 rw 0 0\n"
        "/dev/sda1 /run/media/you/RPI-RP2 vfat rw,flush 0 0\n";
    // Both impostors would pass the Board-ID check if they were ever opened.
    m.infoUf2["/home/you/RPI-RP2"] = kRealBootromInfo;
    m.infoUf2["/mnt/nfs"]          = kRealBootromInfo;
    m.infoUf2["/run/media/you/RPI-RP2"] = kRealBootromInfo;

    const auto v = detail::selectRpiRp2Volumes(m.io());
    CHECK(v == std::vector<std::string>{ "/run/media/you/RPI-RP2" });

    CHECK_FALSE(m.opened("/home/you/RPI-RP2"));
    CHECK_FALSE(m.opened("/mnt/nfs"));
    CHECK(m.opened("/run/media/you/RPI-RP2"));
}

TEST_CASE("selectRpiRp2Volumes: the Board-ID check is load-bearing") {
    // KILLS THE MUTANT "isRp2BootromInfo call deleted".
    //
    // Every mount here is vfat and every one of them HAS an INFO_UF2.TXT, so the
    // filesystem-type filter and the file's mere existence cannot separate them.
    // Only the Board-ID inside can. Another vendor's UF2 bootloader presents
    // exactly this way, and an RP2040 image written to one is rejected by it as
    // a foreign family ID -- so it must never be offered as a FreeWili.
    FakeMachine m;
    m.mounts =
        "/dev/sdc1 /run/media/you/FEATHERBOOT vfat rw 0 0\n"
        "/dev/sdd1 /run/media/you/RP2350 vfat rw 0 0\n"
        "/dev/sda1 /run/media/you/RPI-RP2 vfat rw,flush 0 0\n";
    m.infoUf2["/run/media/you/FEATHERBOOT"] =
        "UF2 Bootloader v3.0\nModel: Adafruit Feather M0\nBoard-ID: SAMD21G18A-Feather-v0\n";
    m.infoUf2["/run/media/you/RP2350"] =
        "UF2 Bootloader v3.0\nModel: Raspberry Pi RP2\nBoard-ID: RP2350\n";
    m.infoUf2["/run/media/you/RPI-RP2"] = kRealBootromInfo;

    const auto v = detail::selectRpiRp2Volumes(m.io());
    CHECK(v == std::vector<std::string>{ "/run/media/you/RPI-RP2" });
}

TEST_CASE("selectRpiRp2Volumes skips a vfat volume with no INFO_UF2.TXT at all") {
    // An EFI system partition is vfat, is mounted on most machines here, and has
    // no INFO_UF2.TXT -- which is precisely why the fstype filter alone is not
    // the answer.
    FakeMachine m;
    m.mounts = "/dev/nvme0n1p1 /boot/efi vfat rw 0 0\n";

    CHECK(detail::selectRpiRp2Volumes(m.io()).empty());
    CHECK(m.opened("/boot/efi"));   // it was asked; it just had nothing to say
}

TEST_CASE("selectRpiRp2Volumes accepts a hand-mounted msdos volume") {
    // udisks2 mounts it "vfat"; "msdos" is the same filesystem mounted by hand
    // with the older driver name, and a manual mount must not be invisible.
    FakeMachine m;
    m.mounts = "/dev/sda1 /mnt/rp2 msdos rw 0 0\n";
    m.infoUf2["/mnt/rp2"] = kRealBootromInfo;

    CHECK(detail::selectRpiRp2Volumes(m.io()) == std::vector<std::string>{ "/mnt/rp2" });
}

TEST_CASE("selectRpiRp2Volumes unescapes the mount point before using it") {
    // End to end through the seam, not just through parseMountLine: the path
    // handed to the INFO_UF2.TXT reader -- and the path returned for
    // copyToVolume() to write into -- must be the one that exists on disk.
    FakeMachine m;
    m.mounts = "/dev/sda1 /run/media/john\\040doe/RPI-RP2 vfat rw 0 0\n";
    m.infoUf2["/run/media/john doe/RPI-RP2"] = kRealBootromInfo;

    CHECK(detail::selectRpiRp2Volumes(m.io()) ==
          std::vector<std::string>{ "/run/media/john doe/RPI-RP2" });
    CHECK(m.opened("/run/media/john doe/RPI-RP2"));
}

TEST_CASE("selectRpiRp2Volumes handles an empty, unterminated or unreadable mount table") {
    FakeMachine empty;
    CHECK(detail::selectRpiRp2Volumes(empty.io()).empty());

    // A /proc/mounts that could not be read at all yields no volumes, not a
    // crash -- and certainly not a volume.
    detail::VolumeIo none;
    CHECK(detail::selectRpiRp2Volumes(none).empty());

    // Final line with no trailing newline must still be considered.
    FakeMachine m;
    m.mounts = "/dev/sda1 /mnt/rp2 vfat rw 0 0";
    m.infoUf2["/mnt/rp2"] = kRealBootromInfo;
    CHECK(detail::selectRpiRp2Volumes(m.io()) == std::vector<std::string>{ "/mnt/rp2" });
}

// --- unmountedBootselNotice -------------------------------------------------

TEST_CASE("unmountedBootselNotice says nothing when every BOOTSEL device is mounted") {
    CHECK(detail::unmountedBootselNotice(0, 0).empty());
    CHECK(detail::unmountedBootselNotice(1, 1).empty());
    CHECK(detail::unmountedBootselNotice(2, 2).empty());
    // More volumes than BOOTSEL devices is not this function's problem to
    // report -- it means something else is mounted, not that something is
    // missing.
    CHECK(detail::unmountedBootselNotice(1, 2).empty());
}

TEST_CASE("unmountedBootselNotice explains a BOOTSEL device nothing mounted") {
    // The case this exists for: findRpiRp2Volumes() returns nothing while a
    // board sits in BOOTSEL, which is otherwise indistinguishable from no
    // board being attached at all.
    const auto s = detail::unmountedBootselNotice(1, 0, detail::MountRemedy::Udisks);
    CHECK_FALSE(s.empty());
    CHECK(s.find("BOOTSEL") != std::string::npos);
    CHECK(s.find("not mounted") != std::string::npos);
    // It must tell the user what to actually do about it.
    CHECK(s.find("udisksctl mount") != std::string::npos);
}

TEST_CASE("unmountedBootselNotice counts only the unaccounted-for devices") {
    // One of the two is mounted and visible; only the other needs explaining.
    const auto s = detail::unmountedBootselNotice(2, 1);
    CHECK(s.find("A CPU is in BOOTSEL") != std::string::npos);
    const auto both = detail::unmountedBootselNotice(2, 0);
    CHECK(both.find("2 CPUs are in BOOTSEL") != std::string::npos);
}

TEST_CASE("unmountedBootselNotice's remedy is one the plural case can actually follow") {
    // The singular remedy is right for one drive and WRONG for two. udev
    // publishes one /dev/disk/by-label/RPI-RP2 symlink per LABEL, and both
    // RP2040 bootrom volumes carry the identical label -- so with two of them
    // that path names exactly one, and a user following the instruction twice
    // mounts the same drive twice while the other stays invisible. A remedy the
    // situation cannot satisfy is worse than no remedy: it reads as the app
    // being broken.
    const auto one = detail::unmountedBootselNotice(1, 0, detail::MountRemedy::Udisks);
    CHECK(one.find("udisksctl mount -b /dev/disk/by-label/RPI-RP2") != std::string::npos);

    const auto two = detail::unmountedBootselNotice(2, 0, detail::MountRemedy::Udisks);
    // Device nodes, which are distinct, plus the command that lists them.
    CHECK(two.find("lsblk") != std::string::npos);
    CHECK(two.find("udisksctl mount -b /dev/<node>") != std::string::npos);
    // And it must say why the obvious command is not the one to use here,
    // because that is the command the user already knows.
    CHECK(two.find("Do not use /dev/disk/by-label/RPI-RP2") != std::string::npos);
    CHECK(two.find("only one symlink") != std::string::npos);
}

TEST_CASE("the remedy follows the number of DEVICES carrying the label, not the number unmounted") {
    // Two CPUs in BOOTSEL with one of them already mounted. This is not a
    // corner: it is the ordinary "now do the other CPU" state, and it is the
    // only place where the two counts in this message disagree -- ONE drive is
    // unaccounted for, but TWO devices are claiming the label RPI-RP2.
    //
    // An earlier version branched on the unaccounted count and so offered the
    // by-label command here. udev publishes one symlink per label, so that
    // command names one of the two at random: half the time it mounts the drive
    // the user wanted, and half the time it returns "already mounted at
    // .../RPI-RP21" and the user is back where they started.
    const auto twoDevicesOneMounted =
        detail::unmountedBootselNotice(2, 1, detail::MountRemedy::Udisks);

    // Singular situation -- one drive really is missing...
    CHECK(twoDevicesOneMounted.find("A CPU is in BOOTSEL") != std::string::npos);
    // ...but the plural remedy, because two devices share the label.
    CHECK(twoDevicesOneMounted.find("lsblk") != std::string::npos);
    CHECK(twoDevicesOneMounted.find("Do not use /dev/disk/by-label/RPI-RP2")
          != std::string::npos);
    // The by-label command must not appear as something to run. It is named
    // only in the warning against it, which the previous CHECK already pins,
    // so what must be absent is the imperative form.
    CHECK(twoDevicesOneMounted.find("for example: udisksctl mount -b /dev/disk/by-label")
          == std::string::npos);

    // And the one-device case is unaffected: there the symlink is unambiguous
    // and the short command is the better answer.
    CHECK(detail::unmountedBootselNotice(1, 0, detail::MountRemedy::Udisks)
              .find("lsblk") == std::string::npos);
}

TEST_CASE("unmountedBootselNotice's macOS remedy names tools macOS actually has") {
    // The Linux remedy strands a mac user at exactly the moment the notice
    // exists to rescue them: udisksctl and lsblk do not exist there. Same
    // shape as the udisks tests above -- a short by-name command when it is
    // unambiguous, disk identifiers plus the warning when two drives share
    // the name.
    const auto one = detail::unmountedBootselNotice(1, 0, detail::MountRemedy::Diskutil);
    CHECK(one.find("diskutil mount RPI-RP2") != std::string::npos);
    CHECK(one.find("udisksctl") == std::string::npos);

    const auto two = detail::unmountedBootselNotice(2, 0, detail::MountRemedy::Diskutil);
    CHECK(two.find("diskutil list") != std::string::npos);
    CHECK(two.find("diskutil mount /dev/<identifier>") != std::string::npos);
    CHECK(two.find("Do not mount by the name RPI-RP2") != std::string::npos);
    CHECK(two.find("for example: diskutil mount RPI-RP2") == std::string::npos);
    CHECK(two.find("lsblk") == std::string::npos);

    // The count logic is shared, not per-remedy: two devices with one mounted
    // still gets the singular situation and the plural remedy.
    const auto mixed = detail::unmountedBootselNotice(2, 1, detail::MountRemedy::Diskutil);
    CHECK(mixed.find("A CPU is in BOOTSEL") != std::string::npos);
    CHECK(mixed.find("diskutil list") != std::string::npos);
}

// --- countBootselDevices ----------------------------------------------------

TEST_CASE("countBootselDevices is board-free and never negative") {
    // Runs in the suite with no hardware attached, so the only thing that can
    // be asserted without a board is that it answers rather than throwing or
    // reporting nonsense. The measured non-zero case is recorded in
    // fwVolume.cpp; it cannot be asserted here without a FreeWili in BOOTSEL.
    CHECK(countBootselDevices() >= 0);
}

TEST_CASE("std::filesystem::file_size on a missing path maps to std::errc::no_such_file_or_directory") {
    // copyToVolume's "missing destination after copy means success" branch
    // depends on this mapping holding for the standard library actually in
    // use, so it is verified here directly rather than assumed.
    std::error_code ec;
    const auto sz = std::filesystem::file_size(
        "this-path-should-not-exist-9f8a7c2e/also-missing.uf2", ec);
    (void)sz;
    CHECK(ec);
    CHECK(ec == std::errc::no_such_file_or_directory);
}
