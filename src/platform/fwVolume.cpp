#include "platform/fwVolume.h"

#include "core/fwTypes.h"   // platformLimitationNotice (the Emscripten branch below)

#include <array>
#include <fstream>
#include <sstream>
#include <system_error>

#if defined(_WIN32)
  #include <windows.h>
#elif !defined(__EMSCRIPTEN__)
  #include <cerrno>
  #include <cstring>
  #include <fcntl.h>
  #include <unistd.h>
  #if defined(__APPLE__)
    // getmntinfo() -- macOS has no /proc/mounts -- and IOKit, which answers
    // the USB-tree question /sys/bus/usb/devices answers on Linux.
    #include <sys/param.h>
    #include <sys/ucred.h>
    #include <sys/mount.h>
    #include <CoreFoundation/CoreFoundation.h>
    #include <IOKit/IOKitLib.h>
    #include <DiskArbitration/DiskArbitration.h>
  #endif
#endif

// VERIFIED ON LINUX against a real FreeWili 1-OG, on the MAIN CPU.
//
// What was actually run, and what it showed:
//
//  - findRpiRp2Volumes() found the bootrom volume udisks2 mounted at
//    /run/media/<user>/RPI-RP2 (/dev/sda1, vfat, removable), roughly 2-3 s after
//    the 2e8a:0003 RP2 Boot device enumerated. TWO observations here read
//    2.38 s and 2.41 s, and an earlier revision of this comment quoted that
//    pair as the range -- which was a range of two samples of a varying
//    quantity, not a bound. A reviewer's six observations of the same event
//    spanned 1.93-3.12 s. Nothing in this file has a timeout keyed to it, so
//    the number is documentation only; treat it as "a couple of seconds, and
//    sometimes three" and do not tighten it again without more samples than the
//    tightening implies.
//  - copyToVolume() wrote probe/probe.uf2 to it; the CPU accepted the image,
//    rebooted, enumerated as 2e8a:000a on MAIN's own hub port with MAIN's
//    serial, and printed "main" on its CDC. It was then restored to
//    FreeWiliMainV92.uf2 by the same function and came back as 093c:2054.
//
// Three defects were found by that run and by the experiments around it, and
// all three are fixed below. They are described where they were fixed:
// the volume match (findRpiRp2Volumes), the durability of the write
// (copyToVolume), and BOOTSEL devices nothing has mounted (countBootselDevices).
//
// NOT verified: the DISPLAY CPU (deliberately -- MAIN has a physical BOOTSEL
// button as a human fallback and DISPLAY does not, so the destructive work was
// done on MAIN), and two bootrom volumes mounted AT ONCE from two real boards.
// The two-volume behaviour below was measured with loopback FAT filesystems
// carrying the RPI-RP2 label, which is what establishes udisks2's mount-point
// naming, but no second FreeWili was attached.
//
// BOARD-VERIFIED ON MACOS 2026-08-14, same board: the __APPLE__ branches below
// (getmntinfo, F_FULLFSYNC, the DiskArbitration unmount and nobrowse remount)
// flashed it end to end. That run predates the v2 rebase; on the rebased
// branch only the build and test suite were re-verified, the flash was not.

namespace fwog {

// The RP2040 bootrom's identity, and the one string this file matches on. It is
// the FAT volume label (what the Windows branch reads), and it is also the
// Board-ID INFO_UF2.TXT reports (what the Linux branch reads). Declared here
// rather than in the anonymous namespace below because detail:: needs it too.
constexpr const char* kLabel = "RPI-RP2";

namespace detail {

std::string unescapeMount(std::string_view s)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 3 < s.size() &&
            s[i+1] >= '0' && s[i+1] <= '7' &&
            s[i+2] >= '0' && s[i+2] <= '7' &&
            s[i+3] >= '0' && s[i+3] <= '7') {
            out.push_back(char((s[i+1] - '0') * 64 + (s[i+2] - '0') * 8 + (s[i+3] - '0')));
            i += 3;
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

std::string escapeMount(std::string_view s)
{
    std::string out;
    out.reserve(s.size());
    for (char ch : s) {
        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\\') {
            const unsigned c = static_cast<unsigned char>(ch);
            out += { '\\', char('0' + (c >> 6)), char('0' + ((c >> 3) & 7)),
                     char('0' + (c & 7)) };
        } else {
            out.push_back(ch);
        }
    }
    return out;
}

std::optional<MountLine> parseMountLine(std::string_view line)
{
    // /proc/mounts is "device mountpoint fstype options dump pass". Anything
    // with fewer than three fields is not a mount and is skipped rather than
    // half-parsed -- a truncated line must not be able to produce an entry
    // with an empty mount point, which would later resolve to a relative path.
    std::istringstream ls{ std::string(line) };
    std::string dev, mnt, fs;
    if (!(ls >> dev >> mnt >> fs)) return std::nullopt;

    // The kernel escapes space, tab, newline and backslash in the device and
    // mount-point fields as octal; decode before using either as a filesystem
    // path, or a mount point containing a space (e.g. a display name) yields a
    // path that does not exist on disk. The fstype field is a kernel-supplied
    // identifier with no escaping to undo.
    return MountLine{ unescapeMount(dev), unescapeMount(mnt), fs };
}

bool isRp2BootromInfo(std::string_view infoUf2Txt)
{
    // Every RP2040 bootrom volume carries an INFO_UF2.TXT that names the board.
    // Measured on the attached FreeWili 1-OG, exactly (LF endings, 62 bytes):
    //
    //     UF2 Bootloader v3.0
    //     Model: Raspberry Pi RP2
    //     Board-ID: RPI-RP2
    //
    // Board-ID is the line to key on. Model is prose that has changed between
    // bootrom revisions, and the "UF2 Bootloader" banner is shared with every
    // OTHER vendor's UF2 bootloader -- Adafruit's and Microchip's boards
    // present an INFO_UF2.TXT too, on a vfat volume, and an RP2040 image
    // written to one of those is rejected by it as a foreign family ID. Keying
    // on the banner would turn any such board plugged into the same machine
    // into a volume this app offers to flash.
    //
    // Requiring exactly RPI-RP2 is the same question the Windows branch asks of
    // the volume LABEL, so the two platforms accept the same set of devices.
    for (size_t pos = 0; pos < infoUf2Txt.size();) {
        const size_t eol = infoUf2Txt.find('\n', pos);
        std::string_view line = infoUf2Txt.substr(
            pos, eol == std::string_view::npos ? std::string_view::npos : eol - pos);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);

        constexpr std::string_view kKey = "Board-ID:";
        if (line.size() > kKey.size() && line.substr(0, kKey.size()) == kKey) {
            std::string_view value = line.substr(kKey.size());
            while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
                value.remove_prefix(1);
            while (!value.empty() && (value.back() == ' ' || value.back() == '\t'))
                value.remove_suffix(1);
            return value == kLabel;
        }

        if (eol == std::string_view::npos) break;
        pos = eol + 1;
    }
    return false;
}

std::string unmountedBootselNotice(int bootselDevices, size_t volumesFound,
                                   MountRemedy remedy)
{
    // Nothing to explain unless a CPU is sitting in the bootrom that no mounted
    // volume accounts for.
    if (bootselDevices <= 0 || static_cast<size_t>(bootselDevices) <= volumesFound)
        return {};

    const int unaccounted = bootselDevices - static_cast<int>(volumesFound);

    // THE REMEDY HAS TO WORK FOR THE NUMBER OF DRIVES IT IS TALKING ABOUT --
    // and the two halves of this message count DIFFERENT things.
    //
    // The situation is about how many CPUs are unaccounted for. The remedy is
    // about how many devices carry the label RPI-RP2, which is every CPU in
    // BOOTSEL whether or not its drive got mounted. Those numbers are equal
    // except in the one state that matters most: two CPUs in BOOTSEL with only
    // one of them mounted -- the ordinary "now do the other CPU" state. There,
    // one drive is unaccounted for while TWO devices are claiming the label.
    //
    // Getting that wrong is not cosmetic. udev publishes ONE
    // /dev/disk/by-label/RPI-RP2 symlink per LABEL, and both RP2040 bootrom
    // volumes carry the identical label, so with two devices present that path
    // names exactly one of them -- whichever udev linked last, observed moving
    // between devices seconds apart. Offer the by-label command in that state
    // and the user has a coin flip between mounting the drive they wanted and
    // getting "Device /dev/loop0 is already mounted at .../RPI-RP21", which
    // leaves them exactly where this notice was written to rescue them from.
    // (It is the same collapsing that makes by-label useless for COUNTING them,
    // which is why countBootselDevices() walks the USB tree instead.)
    //
    // So: the count below comes from `unaccounted`, and the remedy from
    // `bootselDevices`.
    const std::string situation =
        unaccounted == 1
            ? std::string("A CPU is in BOOTSEL but its RPI-RP2 drive is not mounted, so "
                          "there is nothing to copy to. This app writes a UF2 as a FILE "
                          "and cannot mount the drive itself.")
            : std::to_string(unaccounted) +
                  " CPUs are in BOOTSEL but their RPI-RP2 drives are not mounted, so "
                  "there is nothing to copy to. This app writes a UF2 as a FILE and "
                  "cannot mount the drives itself.";

    // The macOS wording exists because this state is REACHABLE there by the
    // app's own hand: copyToVolume()'s mac arm unmounts the volume before it
    // writes, so a copy the bootrom never consumed (a foreign UF2 dropped in
    // by hand, say) leaves the device attached with its drive mounted nowhere
    // -- a state Linux and Windows never enter on their own. The by-name
    // collapsing hazard is the same on both: `diskutil mount RPI-RP2` picks
    // one of the identically-labelled drives just as by-label does.
    if (remedy == MountRemedy::Diskutil) {
        if (bootselDevices == 1)
            return situation + " Mount it and try again -- for example: "
                               "diskutil mount RPI-RP2";
        return situation +
               " Mount it and try again: run diskutil list to find each drive's "
               "disk identifier (an external DOS_FAT volume named RPI-RP2), then "
               "diskutil mount /dev/<identifier>. Do not mount by the name "
               "RPI-RP2 here -- " +
               std::to_string(bootselDevices) +
               " drives carry that same name, so it picks just one of them at "
               "random.";
    }

    if (bootselDevices == 1)
        return situation + " Mount it and try again -- for example: udisksctl mount -b "
                           "/dev/disk/by-label/RPI-RP2";

    return situation +
           " Mount it and try again: run lsblk -o NAME,LABEL,MOUNTPOINT to find the "
           "device node, then udisksctl mount -b /dev/<node>. Do not use "
           "/dev/disk/by-label/RPI-RP2 here -- " +
           std::to_string(bootselDevices) +
           " drives carry that same label and udev publishes only one symlink for it, "
           "so that path names just one of them at random.";
}

std::vector<std::string> selectRpiRp2Volumes(const VolumeIo& io)
{
    // This used to accept any mount whose last path component was spelled
    // RPI-RP2, on the reasoning that udisks names the mount point after the
    // label. Both halves of that are wrong, and both were measured on this
    // machine:
    //
    //  NOT SUFFICIENT. A bind-mounted directory called RPI-RP2 -- no block
    //  device, no FAT, tmpfs -- was returned as a bootrom volume, and
    //  copyToVolume() then reported OK for a "flash" into it. Windows cannot
    //  produce this: it enumerates drive letters and demands DRIVE_REMOVABLE
    //  plus the label. Reporting a completed flash that never reached a board
    //  is the exact failure copyToVolume's own comment calls the worst thing
    //  it can do.
    //
    //  NOT NECESSARY. udisks2 uniquifies a colliding mount point by appending a
    //  decimal integer, so a SECOND RPI-RP2 volume mounts at .../RPI-RP21, a
    //  third at .../RPI-RP22. Measured with four such volumes mounted at once,
    //  the old match returned ONE.
    //
    // WHAT THAT UNDERCOUNT ACTUALLY COST, traced through the real
    // classifyVolumes()/decideAction() and the engine's switch rather than
    // assumed. An earlier revision of this comment asserted that the old code
    // "would see a single unambiguous drive and write to whichever CPU happened
    // to mount first", and that RefuseAmbiguous was the guard being defeated.
    // Both are false, and the second is structurally impossible: the hub-location
    // arm in classifyVolumes() precedes the `volumes.size() >= 2` test, so with
    // hub identity RefuseAmbiguous is never the answer either way. What the two
    // finders really produce, with both CPUs of one board in BOOTSEL and neither
    // publishing a CDC port:
    //
    //  WITH hub-location identity -- the normal FreeWili case, since both CPUs
    //  hang off the board's own internal hub and identifyCpus() locates their
    //  drives by port. Which drive got the plain .../RPI-RP2 mount point is a
    //  race, so the old finder had two outcomes:
    //    - the TARGET's drive won the name -> MappedToTarget -> WriteMappedVolume
    //      -> writes the target's own drive. Correct, by luck.
    //    - the OTHER CPU's drive won it -> MappedToOtherCpu -> RefuseWrongCpu
    //      -> writes nothing, and tells the user to flash the other CPU first.
    //      A spurious refusal, in the one state the user most needs to act in.
    //  The new finder answers MappedToTarget in both, and the engine copies to
    //  *volumeForCpu(identity, step.cpu) -- the identity's drive, never
    //  volumes.front() -- so the fix converts a coin-flip refusal into the
    //  correct targeted write. It does NOT rescue a write from the wrong CPU,
    //  because with hub identity the old code could not perform one.
    //
    //  WITHOUT any identity -- two separate boards, an external hub, or a hub
    //  port fwfinder did not resolve:
    //    - old: one drive -> ForeignMounted -> RequireTypedConfirmation. Nothing
    //      is written until the user types MAIN or DISPLAY; if they do, the
    //      engine copies to volumes.front() (fwFlashEngine.cpp:535).
    //    - new: two drives -> Ambiguous -> RefuseAmbiguous. Nothing is written.
    //  This is the arm where the fix removes a real wrong-CPU write: the drive
    //  the confirmation writes to is the one that won the mount race, and the
    //  user is being asked to vouch for a mapping neither of them can see. A
    //  main image on the DISPLAY CPU drives GPIO 29 against the PDM microphone's
    //  own output -- see probe/README.md -- so that one path, and only that one,
    //  is where "can physically damage the board" belongs.
    //
    // RESIDUAL, and not closed by anything here: the typed-confirmation path
    // still writes volumes.front() whenever exactly one drive is visible with no
    // identity to check it against -- e.g. both CPUs in BOOTSEL but only one of
    // them mounted. Seeing both drives is what this function can fix; being
    // right about a single anonymous one is not, and remains the confirmation
    // prompt's problem.
    //
    // So ask the device, not the path. Two filters, cheapest first:
    std::vector<std::string> out;
    const std::string mounts = io.readMounts ? io.readMounts() : std::string{};

    size_t pos = 0;
    while (pos <= mounts.size()) {
        const size_t eol = mounts.find('\n', pos);
        const std::string_view line(mounts.data() + pos,
                                    (eol == std::string::npos ? mounts.size() : eol) - pos);
        pos = (eol == std::string::npos) ? mounts.size() + 1 : eol + 1;

        const auto entry = parseMountLine(line);
        if (!entry) continue;

        // 1. Filesystem type, which costs no I/O at all. The bootrom volume is
        //    always FAT. This is also what keeps the INFO_UF2.TXT read below
        //    off every network mount on the machine -- an open() on a
        //    disconnected NFS or CIFS mount can block for a long time, and
        //    this function runs in a ~250ms poll loop on the flash worker.
        //    udisks2 mounts it "vfat"; "msdos" is the same filesystem mounted by
        //    hand with the older driver name, and is accepted so that a manual
        //    mount is not silently invisible to this app.
        //
        //    IT IS NOT THE EQUAL OF THE WINDOWS GUARD, and should not be
        //    described as one. GetDriveTypeA() == DRIVE_REMOVABLE is a cached
        //    property of the volume that the Windows branch answers without
        //    touching the media; this only narrows WHICH mounts get opened. A
        //    vfat filesystem on stalled removable media -- a card reader whose
        //    card was yanked, a USB stick mid-reset -- still gets an open() and
        //    can still hold the worker up. Cheap and effective against the
        //    common case (network mounts, the machine's own ext4), not a
        //    guarantee that this scan cannot block.
        if (entry->fsType != "vfat" && entry->fsType != "msdos") continue;

        // 2. The volume's own account of itself. Present on every RP2040
        //    bootrom volume; absent from the ordinary FAT filesystems this
        //    machine also has mounted (an EFI system partition is vfat too,
        //    and is the reason step 1 alone is not the answer). Requiring the
        //    Board-ID to be RPI-RP2 -- not merely that the file exists -- is
        //    what keeps another vendor's UF2 bootloader off the list.
        const auto info = io.readInfoUf2 ? io.readInfoUf2(entry->mountPoint) : std::nullopt;
        if (!info) continue;
        if (isRp2BootromInfo(*info)) out.push_back(entry->mountPoint);
    }
    return out;
}

} // namespace detail

namespace {
#if defined(_WIN32)
// SetErrorMode is a process-global setting, and findRpiRp2Volumes runs in a
// ~250ms poll loop while waiting for a board to appear. A scope guard, not a
// manual save/restore pair, ensures a future early `return` added inside the
// loop can never silently leave the suppressed mode in effect.
struct ErrorModeGuard {
    UINT prev;
    explicit ErrorModeGuard(UINT mode) : prev(SetErrorMode(mode)) {}
    ~ErrorModeGuard() { SetErrorMode(prev); }
};
#endif

#if defined(__APPLE__)
/// Cleanly unmount the volume through DiskArbitration, waiting for the verdict.
///
/// Why this exists: the RP2040 bootrom detaches its device the instant the
/// last UF2 block arrives, and a volume that vanishes while mounted makes
/// macOS post "Disk Not Ejected Properly" at the user -- on every single
/// flash, for a disappearance that is the SUCCESS signal. Unmounting first
/// fixes both halves at once: unmount(2) semantics flush every dirty page to
/// the device before detaching (so this replaces flushToDevice()'s
/// F_FULLFSYNC as the durability step, it does not skip it), and by the time
/// the bootrom reboots the system has already let go of the volume, so there
/// is nothing improper to complain about.
///
/// DiskArbitration rather than unmount(2) because the syscall needs root for
/// a diskarbitrationd-owned mount; DADiskUnmount is how an ordinary console
/// user ejects a USB drive, no privilege required.
///
/// false means "could not unmount" -- volume already gone (the bootrom won
/// the race; the notice already fired and nothing here can recall it), or
/// something holds the volume open (Spotlight indexing it). The caller falls
/// back to the fsync path, which was the whole behaviour before this
/// function existed; the flash outcome is identical either way, only the
/// notification differs.
namespace da {

struct Result { bool done = false; bool ok = false; };

void callback(DADiskRef, DADissenterRef dissenter, void* ctx)
{
    auto* r = static_cast<Result*>(ctx);
    r->ok   = (dissenter == nullptr);
    r->done = true;
    CFRunLoopStop(CFRunLoopGetCurrent());
}

/// How long to wait for one request's verdict. It bounds the writeback a
/// request can imply. Generous on purpose: FreeWiliDisplayV67 is 16 MB and a
/// FAT volume over full-speed USB moves ~1 MB/s, so a tight budget would turn
/// the largest legitimate image into a spurious failure; the deadline exists
/// only so a wedged diskarbitrationd cannot park the flash worker forever.
///
/// Recorded, not fixed: the remount waits in remountNoBrowse() reuse this
/// writeback-sized budget though a mount implies no writeback, so a truly
/// wedged diskarbitrationd can hold one copy step for several deadlines in a
/// row (and FlashController's cooperative cancel never reaches these loops).
/// Shortening those waits is a tuning question for a machine that exhibits
/// the wedge, not something to guess at from a healthy one. Note for any
/// future refactor: on timeout the pending callback still holds a pointer to
/// the caller's stack Result -- safe today only because the session is
/// unscheduled, released and never pumped again on this thread.
constexpr CFAbsoluteTime kDaVerdictDeadlineSec = 120.0;

/// Pump the scheduled run loop until the callback lands or the deadline
/// passes. True only for an actual clean verdict -- a timeout is a failure.
bool awaitVerdict(Result& r)
{
    const CFAbsoluteTime deadline = CFAbsoluteTimeGetCurrent() + kDaVerdictDeadlineSec;
    while (!r.done && CFAbsoluteTimeGetCurrent() < deadline)
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 1.0, true);
    return r.done && r.ok;
}

/// Run one DiskArbitration request against the volume at `mountPoint` and
/// wait for its verdict.
template <typename Fn>
bool request(const std::string& mountPoint, Fn&& start)
{
    DASessionRef session = DASessionCreate(kCFAllocatorDefault);
    if (!session) return false;

    bool ok = false;
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(
        kCFAllocatorDefault, reinterpret_cast<const UInt8*>(mountPoint.c_str()),
        static_cast<CFIndex>(mountPoint.size()), true);
    DADiskRef disk =
        url ? DADiskCreateFromVolumePath(kCFAllocatorDefault, session, url) : nullptr;
    if (disk) {
        DASessionScheduleWithRunLoop(session, CFRunLoopGetCurrent(),
                                     kCFRunLoopDefaultMode);
        Result result;
        start(disk, &result);
        ok = awaitVerdict(result);
        DASessionUnscheduleFromRunLoop(session, CFRunLoopGetCurrent(),
                                       kCFRunLoopDefaultMode);
        CFRelease(disk);
    }
    if (url) CFRelease(url);
    CFRelease(session);
    return ok;
}

} // namespace da

bool unmountVolumeGracefully(const std::string& mountPoint)
{
    return da::request(mountPoint, [](DADiskRef disk, da::Result* r) {
        DADiskUnmount(disk, kDADiskUnmountOptionDefault, da::callback, r);
    });
}

/// Remount the volume with `nobrowse`, so Finder never learns it exists.
///
/// This is the half that actually prevents the "Disk Not Ejected Properly"
/// notification. The unmount-after-copy above cannot: the unmount's own
/// writeback is what delivers the final UF2 block, and the bootrom reboots
/// the instant it has it -- mid-unmount, volume still mounted, notification
/// posted. MEASURED, on the first board flashed from a Mac: the graceful
/// unmount was in place and the notification appeared anyway. No ordering of
/// flush and unmount wins that race, because the flush IS the trigger.
///
/// What does win it: make the volume one Finder never tracks. diskarbitrationd
/// posts the notification for browsable volumes; a `nobrowse` mount vanishing
/// is nobody's business. So: cleanly unmount the auto-mounted volume BEFORE
/// any write -- nothing is dirty yet, so this is instant and genuinely clean
/// -- and remount it nobrowse. The copy then proceeds against the remounted
/// volume and the reboot takes down a volume macOS was never showing anyone.
///
/// The disk is keyed by BSD name, captured from statfs BEFORE the unmount: a
/// volume-path DADiskRef goes stale the moment the volume unmounts, while the
/// BSD device persists until the USB device itself detaches.
///
/// Returns the (possibly identical) mount point of the nobrowse mount, or
/// nullopt for "leave things as they are". On a failed remount it tries to
/// put the ordinary mount back rather than leave the board's volume mounted
/// nowhere -- the flash this call serves still needs SOMETHING to copy into,
/// and so does the user's next attempt.
std::optional<std::string> remountNoBrowse(const std::string& mountPoint)
{
    struct statfs sfs = {};
    if (::statfs(mountPoint.c_str(), &sfs) != 0) return std::nullopt;
    constexpr std::string_view kDev = "/dev/";
    std::string bsd = sfs.f_mntfromname;
    if (bsd.rfind(kDev, 0) == 0) bsd.erase(0, kDev.size());
    if (bsd.empty()) return std::nullopt;

    DASessionRef session = DASessionCreate(kCFAllocatorDefault);
    if (!session) return std::nullopt;
    DADiskRef disk = DADiskCreateFromBSDName(kCFAllocatorDefault, session, bsd.c_str());
    std::optional<std::string> out;
    if (disk) {
        DASessionScheduleWithRunLoop(session, CFRunLoopGetCurrent(),
                                     kCFRunLoopDefaultMode);
        da::Result unmounted;
        DADiskUnmount(disk, kDADiskUnmountOptionDefault, da::callback, &unmounted);
        if (da::awaitVerdict(unmounted)) {
            // NULL path: diskarbitrationd picks the mount point, exactly as it
            // did for the browsable mount. The argv form is the only way to
            // pass a mount OPTION (nobrowse is not a DADiskMountOptions bit).
            CFStringRef args[] = { CFSTR("nobrowse"), nullptr };
            da::Result mounted;
            DADiskMountWithArguments(disk, nullptr, kDADiskMountOptionDefault,
                                     da::callback, &mounted, args);
            if (da::awaitVerdict(mounted)) {
                // Where did it land? Asked of the disk itself rather than
                // assumed unchanged, so a diskarbitrationd that uniquifies the
                // path cannot silently break the copy that follows.
                if (CFDictionaryRef desc = DADiskCopyDescription(disk)) {
                    auto vol = static_cast<CFURLRef>(CFDictionaryGetValue(
                        desc, kDADiskDescriptionVolumePathKey));
                    char buf[MAXPATHLEN] = {};
                    if (vol && CFURLGetFileSystemRepresentation(
                                   vol, true, reinterpret_cast<UInt8*>(buf),
                                   sizeof(buf)))
                        out = std::string(buf);
                    CFRelease(desc);
                }
            } else {
                // Do not strand the volume unmounted: put the ordinary mount
                // back, best-effort. If this fails too the board re-presents
                // its drive on the next BOOTSEL entry anyway.
                da::Result remounted;
                DADiskMount(disk, nullptr, kDADiskMountOptionDefault, da::callback,
                            &remounted);
                da::awaitVerdict(remounted);
            }
        }
        DASessionUnscheduleFromRunLoop(session, CFRunLoopGetCurrent(),
                                       kCFRunLoopDefaultMode);
        CFRelease(disk);
    }
    CFRelease(session);
    return out;
}
#endif

#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
/// fsync `file`, then the directory holding it, so both the data and the
/// directory entry are on the device before the caller is told anything.
///
/// A vanished destination is SUCCESS, for the same reason the size check in
/// copyToVolume() treats it that way: the bootrom volume disappears the instant
/// it accepts the image, and on a small image that can happen before this runs.
/// Only a real error -- a device that is still there and still refusing -- is
/// reported, because failing here makes the app say a flash did not happen.
///
/// WHAT THAT RULE ASSUMES, stated because it is an assumption and not a proof:
/// that the bootrom is the ONLY thing that removes this device. It is the only
/// thing that removes it in the normal course of events, and it does so only
/// after accepting every block -- which is what makes "gone" mean "finished".
/// A pulled cable, a power loss or a yanked hub mid-writeback produces the
/// identical errno with a partial image on the CPU, and copyToVolume() would
/// then report a completed flash. Nothing here can tell the two apart: the
/// device is gone in both, and the kernel does not say why.
///
/// This is not a regression introduced by adding the fsync -- the pre-existing
/// size check in copyToVolume() has exactly the same hole and has always had it
/// (a missing destination is read as success there too), so the flush neither
/// widens nor narrows it. It is recorded here rather than left implicit because
/// the honest bound on this whole function is "the write reached the device, OR
/// the device left while we were writing", and only the first of those is what
/// the caller goes on to report.
std::expected<void, std::string> flushToDevice(const std::filesystem::path& file)
{
    const auto vanished = [](int e) {
        // ENOENT: the bootrom took the file and the volume went with it.
        // ENODEV/ENXIO: the device itself is already gone underneath us.
        return e == ENOENT || e == ENODEV || e == ENXIO;
    };

    // O_RDONLY is enough: Linux permits fsync on any descriptor, and asking for
    // write access to a file the bootrom may be in the middle of consuming
    // gains nothing.
    int fd = ::open(file.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (vanished(errno)) return {};
        return std::unexpected(std::string("cannot reopen the written file: ") +
                               std::strerror(errno));
    }
#if defined(__APPLE__)
    // fsync() on macOS is documented NOT to force the write through the drive's
    // own cache; F_FULLFSYNC is the call that does, and this function exists
    // precisely to close the "reported success while bytes were still in
    // flight" gap. A filesystem that does not support F_FULLFSYNC (msdos is not
    // guaranteed to) falls back to the plain fsync rather than failing a flash
    // over a durability nicety the platform declined to provide.
    int rc = ::fcntl(fd, F_FULLFSYNC);
    if (rc != 0) rc = ::fsync(fd);
#else
    const int rc = ::fsync(fd);
#endif
    const int fsyncErrno = errno;
    ::close(fd);
    if (rc != 0 && !vanished(fsyncErrno)) {
        // vanished()'s errno set was measured on Linux; which errno macOS's
        // msdos surfaces when the bootrom detaches mid-F_FULLFSYNC was not,
        // and EIO is at least as likely there. EIO from a device that is
        // still present is a genuine failure though, so rather than widen the
        // set to an ambiguous errno, ask the question the set approximates:
        // if the destination itself is gone, the device left, and gone means
        // finished for the same reason as everywhere else in this function.
        std::error_code gone;
        if (!std::filesystem::exists(file, gone) && !gone) return {};
        return std::unexpected(std::strerror(fsyncErrno));
    }

    // The directory entry, best-effort. A FAT directory whose entry has not
    // been written yet leaves a file the bootrom cannot see, but a failure to
    // sync it is not evidence the DATA did not land, and this function's
    // errors become "the flash failed" in the UI. Report nothing.
    int dirFd = ::open(file.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dirFd >= 0) {
        ::fsync(dirFd);
        ::close(dirFd);
    }
    return {};
}
#endif
} // namespace

std::vector<std::string> findRpiRp2Volumes()
{
    std::vector<std::string> out;
#if defined(_WIN32)
    // An empty card reader or CD drive must never be allowed to raise the
    // Windows "There is no disk in the drive" modal -- that would block the
    // worker thread indefinitely behind a dialog the user may not even
    // connect to this app. Suppress it for the duration of the enumeration.
    ErrorModeGuard errorModeGuard(SEM_FAILCRITICALERRORS);

    const DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1u << i))) continue;
        char root[] = { char('A' + i), ':', '\\', '\0' };

        // Network and CD drives can make GetVolumeInformationA block for
        // seconds (e.g. a disconnected network share), which would stall the
        // flash worker inside this poll loop. The RP2040 bootrom volume
        // always enumerates as removable, so skip anything else without
        // touching it.
        if (GetDriveTypeA(root) != DRIVE_REMOVABLE) continue;

        char label[MAX_PATH + 1] = {};
        if (!GetVolumeInformationA(root, label, MAX_PATH, nullptr, nullptr,
                                   nullptr, nullptr, 0))
            continue;
        if (std::string(label) == kLabel) out.emplace_back(root);
    }
#elif defined(__EMSCRIPTEN__)
    // No mass storage in a browser.
#else
    // The decision itself is in detail::selectRpiRp2Volumes(), which is where
    // the two filters and the reasoning behind them live. All that is left here
    // is the machine: the real /proc/mounts and the real INFO_UF2.TXT. Keeping
    // them apart is what makes the filters testable -- with the file opened
    // inline, either could be deleted and nothing failed.
    detail::VolumeIo io;

#if defined(__APPLE__)
    // No /proc/mounts here; getmntinfo() is the same table from the kernel's
    // own hand. It is rendered into /proc/mounts's line format so that
    // detail::selectRpiRp2Volumes() -- the filters, the tests that pin them,
    // and the octal unescaping -- stays one shared implementation.
    // detail::escapeMount() is the exact inverse of detail::unescapeMount():
    // macOS mounts a SECOND volume with the same label at "/Volumes/RPI-RP2 1",
    // and a space fed unescaped into a whitespace-split parser would truncate
    // the mount point at "/Volumes/RPI-RP2" -- a path that names the OTHER
    // board's volume. The fstype macOS gives a FAT volume is "msdos", which the
    // shared filter already accepts for the manually-mounted Linux case.
    io.readMounts = [] {
        // MNT_NOWAIT: the cached table, no per-filesystem statfs round trip.
        // This runs in the flash worker's ~250ms poll loop, and a stalled
        // network mount must not be allowed to hold that loop up just to
        // refresh size fields nothing here reads.
        //
        // getmntinfo_r_np, NOT getmntinfo: the plain call hands back a pointer
        // into one process-wide static allocation that every call from any
        // thread reallocs and overwrites -- and this lambda IS called from two
        // threads at once on every flash (the Recovery tab's poll on the UI
        // thread at 500ms, the flash/probe worker at 250ms). Torn strings or a
        // read of freed memory mid-flash is the price of the convenient
        // spelling; the _r_np variant allocates a fresh array the caller
        // frees, which is the same getpwuid_r-over-getpwuid reasoning
        // fwPaths.cpp already recorded.
        struct statfs* mounts = nullptr;
        const int n = ::getmntinfo_r_np(&mounts, MNT_NOWAIT);
        std::string out;
        for (int i = 0; i < n; ++i) {
            out += detail::escapeMount(mounts[i].f_mntfromname);
            out += ' ';
            out += detail::escapeMount(mounts[i].f_mntonname);
            out += ' ';
            out += mounts[i].f_fstypename;   // kernel identifier, never escaped
            out += " - 0 0\n";
        }
        ::free(mounts);                      // _r_np's contract: caller frees
        return out;
    };
#else
    io.readMounts = [] {
        std::ifstream f("/proc/mounts", std::ios::binary);
        std::ostringstream ss;
        ss << f.rdbuf();
        return ss.str();
    };
#endif

    io.readInfoUf2 = [](const std::string& mountPoint) -> std::optional<std::string> {
        std::ifstream info(std::filesystem::path(mountPoint) / "INFO_UF2.TXT",
                           std::ios::binary);
        if (!info) return std::nullopt;
        // Bounded read: this is a file on a device the user plugged in, and
        // nothing about it should be trusted to be small. The real one is 62
        // bytes. The bound lives with the real reader rather than in the filter,
        // so a fake cannot accidentally be exempted from it -- and so a fake is
        // never obliged to simulate it.
        std::array<char, 512> buf{};
        info.read(buf.data(), buf.size());
        return std::string(buf.data(), static_cast<size_t>(info.gcount()));
    };

    out = detail::selectRpiRp2Volumes(io);
#endif
    return out;
}

int countBootselDevices()
{
#if defined(_WIN32) || defined(__EMSCRIPTEN__)
    // Not asked here. On Windows the drive-letter enumeration above is already
    // the whole answer -- there is no equivalent "device present but nothing
    // mounted it" state to distinguish, because Windows mounts it itself -- and
    // a browser has no USB tree to walk. Returning 0 makes
    // unmountedBootselNotice() produce nothing, so no caller has to branch on
    // the platform to decide whether to ask.
    return 0;
#elif defined(__APPLE__)
    // The same question the Linux branch below asks of /sys/bus/usb/devices,
    // asked of the IOKit registry: one IOUSBHostDevice node per physical
    // device, so two boards in BOOTSEL count as two -- the by-label collapse
    // described below cannot happen here either. Any failure returns 0, which
    // silences unmountedBootselNotice() rather than inventing a device.
    //
    // (In practice macOS auto-mounts the bootrom volume like Windows does, so
    // this notice should rarely fire -- but "device present, nothing mounted"
    // IS reachable here, e.g. after `diskutil unmount`, so the honest count is
    // computed rather than hard-coded to 0 on the Windows argument.)
    CFMutableDictionaryRef match = IOServiceMatching("IOUSBHostDevice");
    if (!match) return 0;
    const int32_t vid = 0x2e8a, pid = 0x0003;   // "RP2 Boot", same as below
    CFNumberRef v = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &vid);
    CFNumberRef p = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &pid);
    CFDictionarySetValue(match, CFSTR("idVendor"), v);
    CFDictionarySetValue(match, CFSTR("idProduct"), p);
    CFRelease(v);
    CFRelease(p);
    io_iterator_t it = IO_OBJECT_NULL;
    // IOServiceGetMatchingServices consumes `match` whether it succeeds or not.
    if (IOServiceGetMatchingServices(kIOMainPortDefault, match, &it) != KERN_SUCCESS)
        return 0;
    int n = 0;
    for (io_object_t dev; (dev = IOIteratorNext(it)) != IO_OBJECT_NULL;
         IOObjectRelease(dev))
        ++n;
    IOObjectRelease(it);
    return n;
#else
    // Counted from the USB device tree rather than from /dev/disk/by-label,
    // deliberately. udev publishes ONE by-label symlink per label, so two
    // boards in BOOTSEL produce one symlink and would be counted as one device
    // -- which is the same undercount that made the mount-point match unsafe.
    // The USB tree has a node per device and cannot collapse them.
    int n = 0;
    std::error_code ec;
    std::filesystem::directory_iterator it("/sys/bus/usb/devices", ec);
    if (ec) return 0;
    for (const auto& dev : it) {
        const auto readId = [&](const char* what) {
            std::ifstream f(dev.path() / what);
            std::string v;
            f >> v;
            return v;
        };
        // 2e8a:0003 is "RP2 Boot": an RP2040 sitting in the bootrom with its
        // mass-storage interface up. 2e8a:000a, the prober's CDC, is a running
        // application and is not this.
        if (readId("idVendor") == "2e8a" && readId("idProduct") == "0003") ++n;
    }
    return n;
#endif
}

std::expected<void, std::string> copyToVolume(const std::filesystem::path& src,
                                              const std::string& volume)
{
#if defined(__EMSCRIPTEN__)
    // Unreachable in practice -- findRpiRp2Volumes() returns nothing here, so
    // no plan can ever reach a copy, and every Flash button is disabled with
    // platformLimitationNotice() before that. It is still a hard refusal
    // rather than a fall-through to the std::filesystem path below: MEMFS
    // would happily "succeed" at copying a UF2 into a directory inside the
    // browser's own sandbox, and this function reporting success is exactly
    // the "told the user a board was written when it was not" failure the
    // comment further down calls the worst thing it can do.
    (void)src;
    (void)volume;
    return std::unexpected(std::string(platformLimitationNotice()));
#else
    std::error_code ec;
    const auto srcSize = std::filesystem::file_size(src, ec);
    if (ec) return std::unexpected("cannot read " + src.string() + ": " + ec.message());

#if defined(__APPLE__)
    // BEFORE the first byte is written: take the volume off Finder's books.
    // See remountNoBrowse() for why this ordering is the only one that
    // prevents the "Disk Not Ejected Properly" notification -- once any block
    // of the image is in flight, no unmount can beat the bootrom's reboot to
    // the punch. On any failure the copy proceeds against the original,
    // browsable mount; the cost is the notification, never the flash.
    std::string targetVolume = volume;
    if (auto nb = remountNoBrowse(volume)) targetVolume = *nb;

    // remountNoBrowse() unmounts before anything is written, which revokes
    // this function's own precondition by its own hand: if both its remounts
    // then fail AND a non-DA-owned /Volumes/RPI-RP2 directory survives the
    // unmount (diskarbitrationd removes only mount-point directories it
    // created), the path above is now a plain directory on the boot volume --
    // and the copy below would put the image on the internal disk, pass the
    // size check, and report a flash that never reached a board: the exact
    // failure this file's comments call the worst one. So ask the filesystem
    // directly: the target must still BE a mount point, of a FAT volume.
    {
        struct statfs sfs{};
        if (::statfs(targetVolume.c_str(), &sfs) != 0 ||
            targetVolume != sfs.f_mntonname ||
            std::string_view(sfs.f_fstypename) != "msdos")
            return std::unexpected(targetVolume +
                                   " is no longer a mounted RPI-RP2 volume -- "
                                   "refusing to write the image anywhere else");
    }
#else
    const std::string& targetVolume = volume;
#endif

    const auto dst = std::filesystem::path(targetVolume) / src.filename();
    std::filesystem::copy_file(src, dst,
                               std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) return std::unexpected("copy to " + targetVolume + " failed: " + ec.message());

#if defined(__APPLE__)
    // Verify the copy while it is still readable from the page cache, then
    // unmount, which flushes every dirty page to the device. A clean unmount
    // subsumes the fsync below (unmount semantics: all dirty data reaches the
    // device or the unmount errors), so success here is the same durability
    // guarantee by a different call. The bootrom usually wins this unmount --
    // the flush delivers its last block and it reboots mid-detach -- which is
    // exactly why the nobrowse remount above, not this call, is what keeps
    // the notification away. Any failure other than a size mismatch falls
    // through to the fsync path, whose rules understand a vanished
    // destination.
    {
        std::error_code copiedEc;
        const auto copiedSize = std::filesystem::file_size(dst, copiedEc);
        if (!copiedEc && copiedSize != srcSize)
            return std::unexpected(
                "the copied image is the wrong size; the write did not complete");
        if (!copiedEc && unmountVolumeGracefully(targetVolume)) return {};
    }
#endif

#if !defined(_WIN32)
    // copy_file() returning does NOT mean the image is on the board. MEASURED,
    // writing firmware/FreeWiliMainV92.uf2 (4,917,760 B = 9,605 sectors) to the
    // real board and reading /sys/block/sda/stat the instant copy_file()
    // returned: 8,931 sectors had reached the device. 674 sectors -- 345,088
    // bytes, 7% of the image -- were still in the page cache while this
    // function was about to report success.
    //
    // It is not visible with a small image: the same measurement for
    // probe/probe.uf2 (49,664 B = 97 sectors) showed 100 sectors written,
    // the whole file plus FAT metadata, because udisks2 mounts this volume
    // with the vfat `flush` option and that keeps up at 49 kB. Testing only
    // with the prober would have concluded, wrongly, that nothing was needed.
    //
    // What the gap costs: runFlashPlan() treats this function returning success
    // as "the bytes were written to the mass-storage volume" and reports the
    // step complete on the strength of it. Between that report and the kernel
    // finishing writeback, a pulled cable, a closed lid or a killed process
    // truncates the image, and the user has been told the board was flashed.
    // That is a silent partial write, and it is worth the wait to close it.
    //
    // fsync on the file, then on its directory: the file's data and the FAT
    // directory entry that makes it findable are separate inodes, and only
    // fsync'ing the former is the classic half of this recipe that people get
    // wrong.
    if (auto flushed = flushToDevice(dst); !flushed)
        return std::unexpected("could not flush the image to " + targetVolume + ": " +
                               flushed.error());
#endif

    // The bootrom volume disappears the instant it accepts the image, so a
    // destination that is now missing means success. Any OTHER error from the
    // size check is a genuine failure and must not be reported as a completed
    // flash -- telling the user a board was written when it was not is the
    // worst thing this function can do.
    const auto dstSize = std::filesystem::file_size(dst, ec);
    if (!ec) {
        if (dstSize != srcSize)
            return std::unexpected("the copied image is the wrong size; the write did not complete");
    } else if (ec != std::errc::no_such_file_or_directory) {
        return std::unexpected("could not verify the written image: " + ec.message());
    }

    return {};
#endif
}

} // namespace fwog
