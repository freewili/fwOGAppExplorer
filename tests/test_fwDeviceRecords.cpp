#include <doctest/doctest.h>
#include <system_error>
#include <fstream>
#include <filesystem>
#include "device/fwDeviceRecords.h"
#include "device/fwCpuIdentify.h"

#include <map>

using namespace fwog;

TEST_CASE("a SerialMain USB device becomes a main record") {
    auto r = usbDeviceToRecord(UsbKind::SerialMain, "COM60", "Pico");
    REQUIRE(r.has_value());
    CHECK(r->port == "COM60");
    CHECK(r->isSerialMain == true);
    CHECK(r->isSerialDisplay == false);
}

TEST_CASE("a SerialDisplay USB device becomes a display record") {
    auto r = usbDeviceToRecord(UsbKind::SerialDisplay, "COM65", "");
    REQUIRE(r.has_value());
    CHECK(r->isSerialDisplay == true);
    CHECK(r->isSerialMain == false);
}

TEST_CASE("a plain Serial device becomes a record with neither flag") {
    // Still useful: the product-string fallback in identifyCpus reads these.
    auto r = usbDeviceToRecord(UsbKind::Serial, "COM3", "FWOG main template");
    REQUIRE(r.has_value());
    CHECK(r->isSerialMain == false);
    CHECK(r->isSerialDisplay == false);
    CHECK(r->product == "FWOG main template");
}

TEST_CASE("non-serial USB devices produce no record") {
    CHECK_FALSE(usbDeviceToRecord(UsbKind::Hub, "", "").has_value());
    CHECK_FALSE(usbDeviceToRecord(UsbKind::MassStorage, "", "").has_value());
    CHECK_FALSE(usbDeviceToRecord(UsbKind::FTDI, "", "").has_value());
    CHECK_FALSE(usbDeviceToRecord(UsbKind::ESP32, "", "").has_value());
}

TEST_CASE("a mounted bootrom drive on the hub's MAIN port becomes a main volume record") {
    // The fact that used to be thrown away. A CPU sitting in the RP2040 bootrom
    // publishes no serial port, so it can only ever be located this way.
    auto r = usbDeviceToRecord(UsbKind::MassStorage, "", "RP2 Boot", kHubPortMain, "G:/");
    REQUIRE(r.has_value());
    CHECK(r->volume == "G:/");
    CHECK(r->isMassStorageMain == true);
    CHECK(r->isMassStorageDisplay == false);
    // A volume record is not a port record: nothing here may be touched at
    // 1200 baud, and a port field filled in by accident would invite exactly
    // that.
    CHECK(r->port.empty());
    CHECK(r->isSerialMain == false);
}

TEST_CASE("a mounted bootrom drive on the hub's DISPLAY port becomes a display volume record") {
    auto r = usbDeviceToRecord(UsbKind::MassStorage, "", "", kHubPortDisplay, "H:/");
    REQUIRE(r.has_value());
    CHECK(r->volume == "H:/");
    CHECK(r->isMassStorageDisplay == true);
    CHECK(r->isMassStorageMain == false);
}

TEST_CASE("a bootrom drive with no mount point yet produces no record") {
    // Enumerated but not yet assigned a drive letter. There is nothing to copy
    // to, so there is nothing to say.
    CHECK_FALSE(usbDeviceToRecord(UsbKind::MassStorage, "", "", kHubPortMain, "").has_value());
}

TEST_CASE("a raw device node is not a volume, and produces no record") {
    // fwfinder's macOS backend reports "/dev/diskN" -- the disk itself, not a
    // mount point -- while diskutil has not yet said where the volume mounted.
    // That is the not-yet-mounted state above wearing a non-empty spelling: a
    // device node is never something to copy a file into, so it must be
    // refused the same way, not passed downstream to string-match against
    // real mount paths and turn a hub-resolved drive into a refusal.
    CHECK_FALSE(usbDeviceToRecord(UsbKind::MassStorage, "", "RP2 Boot",
                                  kHubPortMain, "/dev/disk4").has_value());
    CHECK_FALSE(usbDeviceToRecord(UsbKind::MassStorage, "", "",
                                  kHubPortDisplay, "/dev/disk5s1").has_value());
    // The real mount-point spellings stay accepted on every platform.
    CHECK(usbDeviceToRecord(UsbKind::MassStorage, "", "",
                            kHubPortMain, "/Volumes/RPI-RP2").has_value());
    CHECK(usbDeviceToRecord(UsbKind::MassStorage, "", "",
                            kHubPortMain, "/run/media/you/RPI-RP2").has_value());
}

TEST_CASE("mass storage on a hub port that is neither CPU is never attributed") {
    // An SD card reader, a FREE-WILi2 layout, a drive on a plain external hub.
    // The structural pass says where a CPU is or says nothing; it must never
    // promote an unrelated drive into being one of the two CPUs.
    CHECK_FALSE(usbDeviceToRecord(UsbKind::MassStorage, "", "", 0, "G:/").has_value());
    CHECK_FALSE(usbDeviceToRecord(UsbKind::MassStorage, "", "", 3, "G:/").has_value());
    CHECK_FALSE(usbDeviceToRecord(UsbKind::MassStorage, "", "", 6, "G:/").has_value());
}

TEST_CASE("a serial device with no port produces no record") {
    // A port is the only thing that makes a record actionable -- it is what
    // gets touched at 1200 baud.
    CHECK_FALSE(usbDeviceToRecord(UsbKind::SerialMain, "", "Pico").has_value());
}

#ifndef __EMSCRIPTEN__
// fromFwfinder is the most safety-critical piece of new logic in this task:
// a transposed entry here (e.g. SerialMain mapped to UsbKind::SerialDisplay)
// would route a main-CPU image at a display CPU, and nothing else in the
// diff would catch it. One assertion per enumerator, naming both sides, so a
// swap is visible directly in the failing assertion text.
TEST_CASE("fromFwfinder maps every Fw::USBDeviceType to its matching UsbKind") {
    CHECK(fromFwfinder(Fw::USBDeviceType::Hub)           == UsbKind::Hub);
    CHECK(fromFwfinder(Fw::USBDeviceType::Serial)        == UsbKind::Serial);
    CHECK(fromFwfinder(Fw::USBDeviceType::SerialMain)    == UsbKind::SerialMain);
    CHECK(fromFwfinder(Fw::USBDeviceType::SerialDisplay) == UsbKind::SerialDisplay);
    CHECK(fromFwfinder(Fw::USBDeviceType::MassStorage)   == UsbKind::MassStorage);
    CHECK(fromFwfinder(Fw::USBDeviceType::ESP32)         == UsbKind::ESP32);
    CHECK(fromFwfinder(Fw::USBDeviceType::FTDI)          == UsbKind::FTDI);
    CHECK(fromFwfinder(Fw::USBDeviceType::Other)         == UsbKind::Other);
}

// --------------------------------------------------------------------------
// productFromSysfs: CpuPortRecord::product has to be the USB PRODUCT STRING.
//
// fwfinder hands out a display name, not a product string, and the two differ
// on Linux and macOS (manufacturer prepended) but not on Windows. Every case
// below is about one of those two shapes; the real strings are the ones the
// attached board and firmware/bl_display.uf2 actually publish.
// --------------------------------------------------------------------------

namespace {
// A reader over a fixed table, so these stay pure -- no /sys, no board.
detail::ReadFileFn readerFor(std::map<std::string, std::string> files)
{
    return [files = std::move(files)](const std::string& path) -> std::optional<std::string> {
        const auto it = files.find(path);
        if (it == files.end()) return std::nullopt;
        return it->second;
    };
}
} // namespace

TEST_CASE("productFromSysfs prefers the kernel's product string over fwfinder's display name") {
    // The exact strings measured on the attached board: fwfinder's Linux branch
    // reports manufacturer + " " + product, and /sys/.../product is the
    // descriptor verbatim.
    const auto read = readerFor({ { "/sys/devices/x/3-4.1.1/product", "MainCPU v92\n" } });
    CHECK(detail::productFromSysfs("/sys/devices/x/3-4.1.1", "FreeWili MainCPU v92", read)
          == "MainCPU v92");
}

TEST_CASE("productFromSysfs strips the newline sysfs attributes carry") {
    // The prefix tests downstream are exact, so a stray "\n" would only matter
    // for a SUFFIX -- but the value is also shown to the user, and the trailing
    // CR case exists because nothing here should depend on which the kernel
    // emitted.
    CHECK(detail::productFromSysfs("/sys/a", "fallback",
                                   readerFor({ { "/sys/a/product", "X\r\n" } })) == "X");
}

TEST_CASE("productFromSysfs leaves a Windows device instance id alone") {
    // The guard, and the reason this is a value test rather than an #ifdef:
    // `_raw` is an instance id on Windows, where `name` already begins with the
    // product string. Following it would be nonsense, and the reader must never
    // even be consulted.
    bool consulted = false;
    detail::ReadFileFn spy = [&consulted](const std::string&) -> std::optional<std::string> {
        consulted = true;
        return "should not be read";
    };
    CHECK(detail::productFromSysfs("USB\\VID_093C&PID_2055\\E463A857",
                                   "FWOG display bl 001 (USB Serial Device)", spy)
          == "FWOG display bl 001 (USB Serial Device)");
    CHECK_FALSE(consulted);
}

TEST_CASE("productFromSysfs falls back rather than blanking the field") {
    const std::string name = "FreeWili MainCPU v92";
    // No reader at all.
    CHECK(detail::productFromSysfs("/sys/a", name, nullptr) == name);
    // A path that is not there (device unplugged between enumeration and here).
    CHECK(detail::productFromSysfs("/sys/a", name, readerFor({})) == name);
    // A device that publishes an empty iProduct has told us nothing, and
    // nothing is not an improvement on the display name.
    CHECK(detail::productFromSysfs("/sys/a", name,
                                   readerFor({ { "/sys/a/product", "\n" } })) == name);
    // An empty `_raw` -- substr on a shorter string must not throw.
    CHECK(detail::productFromSysfs("", name, readerFor({})) == name);
}

TEST_CASE("the display bootloader's own product string reaches both prefix tests") {
    // THE REGRESSION THIS FIXES, end to end through the pure layer, with the
    // strings firmware/bl_display.uf2 really declares: manufacturer
    // "FreeWili OG", product "FWOG display bl 001", 093C:2055.
    const std::string finderName = "FreeWili OG FWOG display bl 001";
    const auto read = readerFor({ { "/sys/d/product", "FWOG display bl 001\n" } });
    const std::string product = detail::productFromSysfs("/sys/d", finderName, read);

    // What Linux used to put in the field, and what it does now.
    CpuIdentity before;
    before.displayPort = "/dev/ttyACM1";
    before.displayProduct = finderName;
    CHECK(ogBootloaderState(before) == OgBootloaderState::Missing);   // wrong, and expensive

    CpuIdentity after;
    after.displayPort = "/dev/ttyACM1";
    after.displayProduct = product;
    CHECK(ogBootloaderState(after) == OgBootloaderState::Present);

    // Signal 2 likewise: a record carrying no structural flag is what pass 2
    // exists for, and the prefix has to be at position 0 to be seen.
    auto rec = usbDeviceToRecord(UsbKind::Serial, "/dev/ttyACM1", product);
    REQUIRE(rec.has_value());
    const std::vector<CpuPortRecord> records{ *rec };
    const auto id = identifyCpus(records);
    REQUIRE(id.displayPort.has_value());
    CHECK(*id.displayPort == "/dev/ttyACM1");
    CHECK(id.displaySource == IdentitySource::ProductString);

    // And with the un-stripped name it resolves nothing at all -- which is the
    // state Linux was in.
    auto stale = usbDeviceToRecord(UsbKind::Serial, "/dev/ttyACM1", finderName);
    REQUIRE(stale.has_value());
    const std::vector<CpuPortRecord> staleRecords{ *stale };
    CHECK_FALSE(identifyCpus(staleRecords).displayPort.has_value());
}

// WHAT IS NOT TESTED HERE, AND WHY IT CANNOT BE.
//
// Everything above drives productFromSysfs(), the pure seam. None of it touches
// the one line that makes the fix reach production: toCpuPortRecords() calling
// productStringOf(usb) rather than usb.name. Reverting exactly that line leaves
// this whole suite green while the shipped app goes back to reading the wrong
// string -- found by mutation, not by reasoning.
//
// I tried to close it and could not, and the reason is the guard itself.
// productStringOf() only consults sysfs when `_raw` starts with "/sys/" -- a
// value test rather than an #ifdef, so the Windows instance-id shape provably
// falls through. That same guard rejects any synthetic path a test could
// create: a temp directory holding a `product` file is not under /sys/, so the
// real function correctly ignores it and returns usb.name, and the test fails
// against CORRECT code. Pointing `_raw` at a real /sys device instead would
// make the assertion depend on which board is plugged in, which this suite
// deliberately never does.
//
// So the honest options were: leave the gap, or weaken the guard to make it
// testable. Weakening a guard so a test can reach it is the wrong trade when
// the guard is what keeps a Linux-only code path off the Windows build.
//
// The line is not unverified, though -- it is verified more strongly than a
// unit test would manage, just not here. It was measured on the real board in
// both directions: with the line present the device bar shows no bootloader
// banner, and with only that line reverted the same board -- which
// demonstrably carries the bootloader -- renders "No OG bootloader on this
// board". That is recorded in docs/hardware-verification.md.
#endif
