#include "device/fwDeviceRecords.h"

#include <array>
#include <fstream>
#include <string_view>

#if defined(__APPLE__)
  #include <CoreFoundation/CoreFoundation.h>
  #include <IOKit/IOKitLib.h>
#endif

namespace fwog {

std::optional<CpuPortRecord> usbDeviceToRecord(UsbKind kind,
                                               const std::string& port,
                                               const std::string& product,
                                               uint32_t location,
                                               const std::string& volume,
                                               const std::string& serial)
{
    if (kind == UsbKind::MassStorage) {
        // A drive letter is what makes a mass-storage record actionable -- it
        // is what gets copied to. An RP2040 in its bootrom with no mount yet
        // (still enumerating, or no letter assigned) tells us nothing usable.
        if (volume.empty()) return std::nullopt;

        // fwfinder's macOS backend answers "/dev/diskN" -- the raw disk, not a
        // mount point -- while diskutil has not yet reported where (or
        // whether) the volume mounted. That is the same "tells us nothing
        // usable" state as an empty volume, but spelled so that it used to
        // pass the emptiness check and flow into classifyVolumes(), where it
        // string-matches no real mount and turns a hub-resolved drive into a
        // ForeignMounted refusal -- or, under fwogcli --yes, into a write that
        // skipped the very guard the classification exists to apply. A device
        // node is never something to copy a file into, on any platform this
        // builds for, so refuse it here the same way an unknown hub port is
        // refused: say nothing rather than guess.
        if (volume.rfind("/dev/", 0) == 0) return std::nullopt;

        // An unrecognised hub port is left unattributed rather than guessed.
        // This is the whole safety property of the structural pass: it says
        // where a CPU is or it says nothing, and a drive on some port that is
        // neither MAIN nor DISPLAY -- an SD card reader, a FREE-WILi2 layout,
        // a device on a plain external hub -- must not be mistaken for a CPU.
        if (location != kHubPortMain && location != kHubPortDisplay)
            return std::nullopt;

        CpuPortRecord r;
        r.volume  = volume;
        r.product = product;
        r.isMassStorageMain    = (location == kHubPortMain);
        r.isMassStorageDisplay = (location == kHubPortDisplay);
        return r;
    }

    switch (kind) {
    case UsbKind::Serial:
    case UsbKind::SerialMain:
    case UsbKind::SerialDisplay:
        break;
    default:
        return std::nullopt;   // hubs, FTDI, ESP32: not a CPU we can locate
    }

    // A port is the only thing that makes a serial record actionable -- it is
    // what gets opened at 1200 baud.
    if (port.empty()) return std::nullopt;

    CpuPortRecord r;
    r.port    = port;
    r.product = product;
    r.serial  = serial;
    r.isSerialMain    = (kind == UsbKind::SerialMain);
    r.isSerialDisplay = (kind == UsbKind::SerialDisplay);
    return r;
}

} // namespace fwog

#ifndef __EMSCRIPTEN__
namespace fwog {

UsbKind fromFwfinder(Fw::USBDeviceType t)
{
    switch (t) {
    case Fw::USBDeviceType::Hub:           return UsbKind::Hub;
    case Fw::USBDeviceType::Serial:        return UsbKind::Serial;
    case Fw::USBDeviceType::SerialMain:    return UsbKind::SerialMain;
    case Fw::USBDeviceType::SerialDisplay: return UsbKind::SerialDisplay;
    case Fw::USBDeviceType::MassStorage:   return UsbKind::MassStorage;
    case Fw::USBDeviceType::ESP32:         return UsbKind::ESP32;
    case Fw::USBDeviceType::FTDI:          return UsbKind::FTDI;
    default:                               return UsbKind::Other;
    }
}

namespace detail {

std::string productFromSysfs(const std::string& rawSyspath,
                             const std::string& finderName,
                             const ReadFileFn& readFile)
{
    constexpr std::string_view kSysPrefix = "/sys/";
    if (std::string_view(rawSyspath).substr(0, kSysPrefix.size()) != kSysPrefix)
        return finderName;
    if (!readFile) return finderName;

    const auto raw = readFile(rawSyspath + "/product");
    if (!raw) return finderName;

    // sysfs string attributes come back newline-terminated. Trailing CR is
    // trimmed with it so the comparison cannot depend on which the kernel
    // happened to emit -- the prefix tests downstream are exact.
    std::string value = *raw;
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r'))
        value.pop_back();

    // A device that publishes an EMPTY iProduct has told us nothing, which is
    // not an improvement on the display name.
    return value.empty() ? finderName : value;
}

} // namespace detail

#if defined(__APPLE__)
namespace {

/// The device's own iProduct string from the IO registry, or "" for "could
/// not tell". fwfinder_mac.cpp sets `_raw` to the empty string, so the sysfs
/// route productStringOf() takes on Linux has nothing to key from here; what a
/// USBDevice does carry is vid/pid/serial, and that triple is how the device
/// is found again in the registry. MEASURED against the attached FreeWili
/// 1-OG: fwfinder's display name for its DISPLAY CPU is "FreeWili OG FWOG
/// display ..." -- manufacturer prefixed, exactly the Linux defect -- and the
/// registry's "USB Product Name" is the bare "FWOG display ..." the prefix
/// tests downstream are anchored on.
///
/// The serial requirement is strict when a serial exists: with two identical
/// boards attached, vid/pid alone names both, and answering with whichever
/// enumerated first would attribute one board's product string to the other.
/// A device that publishes no serial is matched by vid/pid only if it is the
/// ONLY match, same reasoning in a different key.
///
/// UNTESTED, knowingly: the choose-among-candidates rule is pure logic, but
/// every value it decides on comes straight out of the IO registry, and a
/// seam injected here would mock the only thing the function does. The rule
/// is not unpinned -- the Linux sibling, detail::productFromSysfs with its
/// injected reader, carries the tested version -- so this stays fused to
/// IOKit and says so rather than staying silent. Board-verified on macOS
/// 2026-08-14, pre-v2 rebase; build+tests re-verified on the rebased branch,
/// the flash has not been re-run.
std::string ioKitUsbProductString(uint16_t vid, uint16_t pid, const std::string& serial)
{
    CFMutableDictionaryRef match = IOServiceMatching("IOUSBHostDevice");
    if (!match) return {};
    const int32_t v = vid, p = pid;
    CFNumberRef vn = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &v);
    CFNumberRef pn = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &p);
    CFDictionarySetValue(match, CFSTR("idVendor"), vn);
    CFDictionarySetValue(match, CFSTR("idProduct"), pn);
    CFRelease(vn);
    CFRelease(pn);

    io_iterator_t it = IO_OBJECT_NULL;
    // The call consumes `match`, success or not.
    if (IOServiceGetMatchingServices(kIOMainPortDefault, match, &it) != KERN_SUCCESS)
        return {};

    const auto strProp = [](io_object_t svc, CFStringRef key) -> std::string {
        CFTypeRef ref = IORegistryEntryCreateCFProperty(svc, key, kCFAllocatorDefault, 0);
        if (!ref) return {};
        std::string out;
        if (CFGetTypeID(ref) == CFStringGetTypeID()) {
            // A USB string descriptor caps at 126 UTF-16 units; 512 bytes holds
            // any honest answer in UTF-8.
            char buf[512] = {};
            if (CFStringGetCString(static_cast<CFStringRef>(ref), buf, sizeof(buf),
                                   kCFStringEncodingUTF8))
                out = buf;
        }
        CFRelease(ref);
        return out;
    };

    std::string found;
    int vidPidMatches = 0;
    for (io_object_t dev; (dev = IOIteratorNext(it)) != IO_OBJECT_NULL;
         IOObjectRelease(dev)) {
        ++vidPidMatches;
        if (!serial.empty() && strProp(dev, CFSTR("USB Serial Number")) != serial)
            continue;
        found = strProp(dev, CFSTR("USB Product Name"));
        if (!serial.empty()) break;   // the one device this triple names
    }
    IOObjectRelease(it);

    if (serial.empty() && vidPidMatches != 1) return {};   // ambiguous: no answer
    return found;
}

} // namespace
#endif

std::string productStringOf(const Fw::USBDevice& usb)
{
#if defined(__APPLE__)
    // Falls back rather than blanking, same rule as every step of the sysfs
    // path below: `usb.name` is what this field has always contained, so a
    // registry miss returns to the status quo instead of newly making
    // ogBootloaderState() answer Unknown.
    if (auto s = ioKitUsbProductString(usb.vid, usb.pid, usb.serial); !s.empty())
        return s;
    return usb.name;
#else
    return detail::productFromSysfs(usb._raw, usb.name,
        [](const std::string& path) -> std::optional<std::string> {
            std::ifstream f(path, std::ios::binary);
            if (!f) return std::nullopt;
            // Bounded, like fwVolume.cpp's INFO_UF2.TXT read and for the same
            // reason: this is a file whose size is decided by a device the user
            // plugged in. A USB string descriptor cannot exceed 126 characters,
            // so 256 bytes is the whole of any honest answer.
            std::array<char, 256> buf{};
            f.read(buf.data(), buf.size());
            return std::string(buf.data(), static_cast<std::size_t>(f.gcount()));
        });
#endif
}

std::vector<CpuPortRecord> toCpuPortRecords(const Fw::FreeWiliDevice& device)
{
    std::vector<CpuPortRecord> out;
    for (const auto& usb : device.getUSBDevices()) {
        const std::string port = usb.port.value_or(std::string{});
        // fwfinder reports mass-storage mounts as a LIST (a device can expose
        // several). Only a single unambiguous mount is taken: two mounts on one
        // bootrom device is not something an RP2040 does, and picking one of
        // them would be exactly the guess this whole path exists to avoid.
        std::string volume;
        if (usb.paths.has_value() && usb.paths->size() == 1)
            volume = usb.paths->front();

        // productStringOf(usb), NOT usb.name: the display name fwfinder builds
        // is not the USB product string on every platform, and the prefix tests
        // downstream are anchored at position 0. See productStringOf().
        if (auto r = usbDeviceToRecord(fromFwfinder(usb.kind), port, productStringOf(usb),
                                       usb.location, volume, usb.serial))
            out.push_back(std::move(*r));
    }
    return out;
}

} // namespace fwog
#endif
