#include <doctest/doctest.h>
#include "platform/fwSerialPorts.h"
#include "flash/fwCpuProbe.h"

#include <filesystem>
#include <fstream>
#include <string>

using namespace fwog;

// ---------------------------------------------------------------------------
// The Linux usbId. Everything in this file runs on EVERY platform, deliberately
// -- see SerialPortInfo::usbId for the argument. The functions under test touch
// no device and no /sys; the sysfs-shaped ones are pointed at a tree this file
// builds in the temp directory. The one thing a test cannot do is attach a
// board, and the one thing this file therefore does NOT claim to prove is that
// the strings below are what a real machine produces. That was checked
// separately, by hand, against a FreeWili 1-OG and an FTDI adapter, and the
// values captured that day are the fixtures used here.
// ---------------------------------------------------------------------------

TEST_CASE("makeLinuxUsbId builds the documented form from real sysfs values") {
    // Captured verbatim from the attached board: /dev/ttyACM0 is 093c:2054
    // "MainCPU v87" on interface 0, behind sysfs bus id 3-4.1.1:1.0. sysfs
    // attribute files end in a newline, and the fixtures keep it so that the
    // trimming is exercised rather than assumed away.
    CHECK(makeLinuxUsbId({ "093c\n", "2054\n", "00\n", "3-4.1.1:1.0" }) ==
          "usb:v093Cp2054in00:3-4.1.1:1.0");
    // The board's other CPU, on its own USB device.
    CHECK(makeLinuxUsbId({ "093c\n", "2055\n", "00\n", "3-4.1.2:1.0" }) ==
          "usb:v093Cp2055in00:3-4.1.2:1.0");
    // The unrelated FTDI adapter that was plugged into the same hub.
    CHECK(makeLinuxUsbId({ "0403\n", "6014\n", "00\n", "3-4.1.3:1.0" }) ==
          "usb:v0403p6014in00:3-4.1.3:1.0");

    // Hex is upper-cased to match the kernel's own MODALIAS spelling, and the
    // letters around it are not. This is asserted rather than left to chance
    // because looksLikeProberUsbId() upper-cases before it parses, so a change
    // here would be invisible to the matching tests below.
    CHECK(makeLinuxUsbId({ "2E8A", "000a", "00", "1-1:1.0" }) ==
          "usb:v2E8Ap000Ain00:1-1:1.0");
}

TEST_CASE("makeLinuxUsbId says nothing rather than half of something") {
    // No vendor, or no product, means the identity was not read. "" is the
    // documented value for "the platform could not say", and the whole flow
    // depends on that being distinguishable from "not a match".
    CHECK(makeLinuxUsbId({ "", "2054", "00", "3-4.1.1:1.0" }).empty());
    CHECK(makeLinuxUsbId({ "093c", "", "00", "3-4.1.1:1.0" }).empty());
    CHECK(makeLinuxUsbId({}).empty());
    // Not four hex digits: whatever was read, it is not a USB vendor id.
    CHECK(makeLinuxUsbId({ "93c", "2054", "00", "x" }).empty());
    CHECK(makeLinuxUsbId({ "093ca", "2054", "00", "x" }).empty());
    CHECK(makeLinuxUsbId({ "09zz", "2054", "00", "x" }).empty());
    CHECK(makeLinuxUsbId({ "093c", "20 4", "00", "x" }).empty());
}

TEST_CASE("makeLinuxUsbId omits the parts that are genuinely absent") {
    // A non-composite device has no bInterfaceNumber. Omitting the field is
    // not the same as writing "in00": looksLikeProberUsbId() ACCEPTS an id
    // that names no interface and REFUSES one that names the wrong one, so
    // inventing an interface number would turn "unknown" into a claim.
    CHECK(makeLinuxUsbId({ "2e8a", "000a", "", "1-1:1.0" }) ==
          "usb:v2E8Ap000A:1-1:1.0");
    // Malformed reads the same way as absent, for the same reason.
    CHECK(makeLinuxUsbId({ "2e8a", "000a", "zz", "1-1:1.0" }) ==
          "usb:v2E8Ap000A:1-1:1.0");
    CHECK(makeLinuxUsbId({ "2e8a", "000a", "0", "1-1:1.0" }) ==
          "usb:v2E8Ap000A:1-1:1.0");
    // No bus id: the identity is still knowable, it just names a class of
    // device rather than one physical port.
    CHECK(makeLinuxUsbId({ "2e8a", "000a", "00", "" }) == "usb:v2E8Ap000Ain00");
    CHECK(makeLinuxUsbId({ "2e8a", "000a", "", "" }) == "usb:v2E8Ap000A");
}

TEST_CASE("isUsbSerialPortName accepts port names and nothing else") {
    CHECK(isUsbSerialPortName("ttyACM0"));
    CHECK(isUsbSerialPortName("ttyACM1"));
    // Not a collision with ttyACM1 -- a different port, and both must survive.
    CHECK(isUsbSerialPortName("ttyACM10"));
    CHECK(isUsbSerialPortName("ttyUSB0"));
    CHECK(isUsbSerialPortName("ttyUSB123"));

    // The 100-odd other entries /sys/class/tty holds on the machine this was
    // written on: 64 virtual consoles and 32 legacy 8250 ports, none of which
    // is a device anyone plugged in. A prefix test let the first group through
    // only by luck of naming; this one refuses them by rule.
    CHECK_FALSE(isUsbSerialPortName("ttyS0"));
    CHECK_FALSE(isUsbSerialPortName("tty0"));
    CHECK_FALSE(isUsbSerialPortName("tty"));
    CHECK_FALSE(isUsbSerialPortName("console"));
    CHECK_FALSE(isUsbSerialPortName("ptmx"));
    CHECK_FALSE(isUsbSerialPortName(""));

    // What the old prefix test would have accepted. These decide what gets
    // OPENED, which is why the predicate is exact.
    CHECK_FALSE(isUsbSerialPortName("ttyACM"));
    CHECK_FALSE(isUsbSerialPortName("ttyUSB"));
    CHECK_FALSE(isUsbSerialPortName("ttyACMfoo"));
    CHECK_FALSE(isUsbSerialPortName("ttyACM0x"));
    CHECK_FALSE(isUsbSerialPortName("ttyACM 0"));
    CHECK_FALSE(isUsbSerialPortName("xttyACM0"));

    // The macOS spellings. The suffix is driver-chosen, so only its PRESENCE
    // is checked -- but it must be present: a bare prefix is not a name macOS
    // produces, and accepting one would be the ttyACM-prefix mistake above
    // with a cu. in front of it. Bluetooth's cu.* nodes are not USB serial
    // ports and must not spend a bounded port question.
    CHECK(isUsbSerialPortName("cu.usbmodem14201"));
    CHECK(isUsbSerialPortName("cu.usbserial-0001"));
    CHECK_FALSE(isUsbSerialPortName("cu.usbmodem"));
    CHECK_FALSE(isUsbSerialPortName("cu.usbserial"));
    CHECK_FALSE(isUsbSerialPortName("cu.Bluetooth-Incoming-Port"));
}

// ---------------------------------------------------------------------------
// The sysfs walk, against a FABRICATED tree. This is the part with a real
// chance of being wrong -- the two drivers put the tty at different depths --
// and it is the part a board-free test can still nail down completely.
// ---------------------------------------------------------------------------

namespace {

/// A disposable directory tree standing in for a slice of /sys.
class FakeSysfs {
public:
    explicit FakeSysfs(const char* tag) {
        std::error_code ec;
        m_root = std::filesystem::temp_directory_path(ec) /
                 (std::string("fwog-sysfs-test-") + tag);
        std::filesystem::remove_all(m_root, ec);
        std::filesystem::create_directories(m_root, ec);
    }
    ~FakeSysfs() { std::error_code ec; std::filesystem::remove_all(m_root, ec); }
    FakeSysfs(const FakeSysfs&) = delete;
    FakeSysfs& operator=(const FakeSysfs&) = delete;

    const std::filesystem::path& root() const { return m_root; }

    void dir(const std::string& rel) const {
        std::error_code ec;
        std::filesystem::create_directories(m_root / rel, ec);
    }
    /// sysfs attributes are one line with a trailing newline, and the fixtures
    /// say so, because that newline is exactly what caught the untrimmed
    /// version of makeLinuxUsbId().
    void attr(const std::string& rel, const std::string& value) const {
        std::ofstream out(m_root / rel);
        out << value << "\n";
    }
    /// The `device` link every /sys/class/tty/<name> has. A real symlink, so
    /// that the canonical() the walk depends on is genuinely exercised.
    void link(const std::string& from, const std::string& toRel) const {
        std::error_code ec;
        std::filesystem::create_directory_symlink(m_root / toRel, m_root / from, ec);
    }

private:
    std::filesystem::path m_root;
};

} // namespace

TEST_CASE("usbIdForSysfsTtyDir handles the cdc_acm layout: tty on the interface") {
    // /sys/class/tty/ttyACM0/device -> .../3-4.1.1/3-4.1.1:1.0
    const FakeSysfs fs("acm");
    fs.dir("dev/3-4.1.1/3-4.1.1:1.0");
    fs.attr("dev/3-4.1.1/idVendor", "093c");
    fs.attr("dev/3-4.1.1/idProduct", "2054");
    fs.attr("dev/3-4.1.1/3-4.1.1:1.0/bInterfaceNumber", "00");
    fs.dir("class/ttyACM0");
    fs.link("class/ttyACM0/device", "dev/3-4.1.1/3-4.1.1:1.0");

    CHECK(usbIdForSysfsTtyDir(fs.root() / "class/ttyACM0") ==
          "usb:v093Cp2054in00:3-4.1.1:1.0");
}

TEST_CASE("usbIdForSysfsTtyDir handles the usb-serial layout: tty one level deeper") {
    // /sys/class/tty/ttyUSB0/device -> .../3-4.1.3/3-4.1.3:1.0/ttyUSB0
    // The extra level is the whole reason the walk is a search and not a "..".
    const FakeSysfs fs("usbserial");
    fs.dir("dev/3-4.1.3/3-4.1.3:1.0/ttyUSB0");
    fs.attr("dev/3-4.1.3/idVendor", "0403");
    fs.attr("dev/3-4.1.3/idProduct", "6014");
    fs.attr("dev/3-4.1.3/3-4.1.3:1.0/bInterfaceNumber", "00");
    fs.dir("class/ttyUSB0");
    fs.link("class/ttyUSB0/device", "dev/3-4.1.3/3-4.1.3:1.0/ttyUSB0");

    CHECK(usbIdForSysfsTtyDir(fs.root() / "class/ttyUSB0") ==
          "usb:v0403p6014in00:3-4.1.3:1.0");
}

TEST_CASE("usbIdForSysfsTtyDir says nothing when there is nothing to say") {
    const FakeSysfs fs("nothing");

    // No `device` link at all -- a pty, or a port unplugged between the
    // listing and the read.
    fs.dir("class/ttyACM9");
    CHECK(usbIdForSysfsTtyDir(fs.root() / "class/ttyACM9").empty());

    // A directory that does not exist.
    CHECK(usbIdForSysfsTtyDir(fs.root() / "class/nope").empty());

    // A device chain with no USB interface anywhere in it: this is what an
    // 8250 port's chain looks like, and the walk must give up rather than
    // climb to the root of the filesystem looking.
    fs.dir("dev/platform/serial8250/serial8250:0/serial8250:0.1");
    fs.dir("class/ttyS1");
    fs.link("class/ttyS1/device", "dev/platform/serial8250/serial8250:0/serial8250:0.1");
    CHECK(usbIdForSysfsTtyDir(fs.root() / "class/ttyS1").empty());

    // An interface whose parent has no vendor/product. Found the interface,
    // could not name the device: "unknown", not a partial claim.
    fs.dir("dev/orphan/orphan:1.0");
    fs.attr("dev/orphan/orphan:1.0/bInterfaceNumber", "00");
    fs.dir("class/ttyACM8");
    fs.link("class/ttyACM8/device", "dev/orphan/orphan:1.0");
    CHECK(usbIdForSysfsTtyDir(fs.root() / "class/ttyACM8").empty());
}

TEST_CASE("usbIdForSysfsTtyDir gives up before it walks out of the device tree") {
    // The interface is FIVE levels above the tty, one past the bound. The
    // point of the bound is that an unbounded search would keep climbing
    // through /sys/devices, /sys and / -- and on the way it would happily
    // pick up any directory that happened to contain a bInterfaceNumber file.
    const FakeSysfs fs("deep");
    fs.dir("dev/1-1/1-1:1.0/a/b/c/d/e");
    fs.attr("dev/1-1/idVendor", "2e8a");
    fs.attr("dev/1-1/idProduct", "000a");
    fs.attr("dev/1-1/1-1:1.0/bInterfaceNumber", "00");
    fs.dir("class/ttyACM7");
    fs.link("class/ttyACM7/device", "dev/1-1/1-1:1.0/a/b/c/d/e");
    CHECK(usbIdForSysfsTtyDir(fs.root() / "class/ttyACM7").empty());

    // Four hops is reached, though -- the bound is not off by one against the
    // layouts that matter, which need zero hops and one.
    fs.dir("class/ttyACM6");
    fs.link("class/ttyACM6/device", "dev/1-1/1-1:1.0/a/b/c");
    CHECK(usbIdForSysfsTtyDir(fs.root() / "class/ttyACM6") ==
          "usb:v2E8Ap000Ain00:1-1:1.0");
}

// ---------------------------------------------------------------------------
// The join: what this file BUILDS must be what fwCpuProbe's predicate reads.
// Those two live in different translation units with no compiler-enforced link
// between them, and the format is only correct in relation to the predicate --
// so the round trip is asserted here rather than each half being checked
// against a hand-written string that could drift from the other.
// ---------------------------------------------------------------------------

TEST_CASE("the ids this platform builds are judged correctly by the prober predicate") {
    // The prober's CDC as this file would report it: RP2 vendor, pico_stdio_usb
    // product, interface 0.
    const auto proberCdc = makeLinuxUsbId({ "2e8a\n", "000a\n", "00\n", "3-4.1.1:1.0" });
    CHECK(proberCdc == "usb:v2E8Ap000Ain00:3-4.1.1:1.0");
    CHECK(looksLikeProberUsbId(proberCdc));

    // Interface 2 is pico_stdio_usb's Reset interface -- the 1200-baud path,
    // not a serial port. Offering it as a candidate to READ would queue
    // something that cannot answer.
    const auto proberReset = makeLinuxUsbId({ "2e8a\n", "000a\n", "02\n", "3-4.1.1:1.2" });
    CHECK(proberReset == "usb:v2E8Ap000Ain02:3-4.1.1:1.2");
    CHECK_FALSE(looksLikeProberUsbId(proberReset));

    // No interface named: accepted, exactly as the Windows shape is when it
    // carries no MI_ field. The vendor/product pair is what identifies it.
    CHECK(looksLikeProberUsbId(makeLinuxUsbId({ "2e8a", "000a", "", "" })));

    // THE THREE DEVICES THAT WERE ACTUALLY ATTACHED while this was written.
    // None of them is the prober, and the predicate must say so about each --
    // this is the "and nothing else" half of the claim, in the values the
    // machine really produced.
    CHECK_FALSE(looksLikeProberUsbId(
        makeLinuxUsbId({ "093c", "2054", "00", "3-4.1.1:1.0" })));   // MainCPU v87
    CHECK_FALSE(looksLikeProberUsbId(
        makeLinuxUsbId({ "093c", "2055", "00", "3-4.1.2:1.0" })));   // DisplayCPU v67
    CHECK_FALSE(looksLikeProberUsbId(
        makeLinuxUsbId({ "0403", "6014", "00", "3-4.1.3:1.0" })));   // the FTDI

    // An RP2040 in BOOTSEL: same vendor, different product, and not a CDC.
    CHECK_FALSE(looksLikeProberUsbId(makeLinuxUsbId({ "2e8a", "0003", "", "3-4.1.1:1.0" })));
    // Right product, wrong vendor.
    CHECK_FALSE(looksLikeProberUsbId(makeLinuxUsbId({ "0403", "000a", "00", "x" })));

    // "The platform could not say" is never a match, and makeLinuxUsbId()
    // returning "" is the only way it says that.
    CHECK(makeLinuxUsbId({ "", "", "", "" }).empty());
    CHECK_FALSE(looksLikeProberUsbId(makeLinuxUsbId({ "", "", "", "" })));
}

TEST_CASE("looksLikeProberUsbId parses the Linux shape rather than searching it") {
    // Case does not matter, on either side of the join.
    CHECK(looksLikeProberUsbId("USB:V2E8AP000AIN00:3-4.1.1:1.0"));
    CHECK(looksLikeProberUsbId("usb:v2e8ap000ain00:3-4.1.1:1.0"));

    // The bus id is deliberately not examined -- it names WHICH port, which is
    // not a question this predicate asks. Including one that spells the
    // prober's numbers changes nothing either way.
    CHECK(looksLikeProberUsbId("usb:v2E8Ap000Ain00:anything-at-all"));
    CHECK_FALSE(looksLikeProberUsbId("usb:v093Cp2054in00:2E8A-000A"));

    // A "contains 2E8A" test would match all of these. Parsing does not.
    CHECK_FALSE(looksLikeProberUsbId("usb:v093Cp2054in00:usb-2E8A-000A"));
    CHECK_FALSE(looksLikeProberUsbId("2E8A000A"));
    CHECK_FALSE(looksLikeProberUsbId("usb:v2E8A"));
    CHECK_FALSE(looksLikeProberUsbId("usb:v2E8Ap000"));
    // Trailing junk that is not the bus-id separator: refused rather than
    // matched on the strength of its first thirteen characters.
    CHECK_FALSE(looksLikeProberUsbId("usb:v2E8Ap000AGARBAGE"));
    CHECK_FALSE(looksLikeProberUsbId("usb:v2E8Ap000Ain0"));
    CHECK_FALSE(looksLikeProberUsbId("usb:v2E8Ap000Ain00x"));

    // Not the Linux shape at all: falls through to the Windows rules, which
    // must be unchanged by any of this.
    CHECK(looksLikeProberUsbId("USB\\VID_2E8A&PID_000A&MI_00\\7&29198214&0&0000"));
    CHECK_FALSE(looksLikeProberUsbId("USB\\VID_2E8A&PID_000A&MI_02\\7&29198214&0&0002"));
}

TEST_CASE("the two spellings of the prober's USB numbers are the same numbers") {
    // kProbeUsbVid/kProbeUsbPid are what a Windows instance id spells;
    // kProbeUsbVidHex/kProbeUsbPidHex are what sysfs and lsusb print. Two
    // spellings of one fact are two things that can drift, and a drift would
    // make looksLikeProberUsbId() answer differently on the two platforms for
    // the same physical device -- which is precisely the failure this whole
    // component exists to avoid.
    CHECK(std::string(kProbeUsbVid) == "VID_" + std::string(kProbeUsbVidHex));
    CHECK(std::string(kProbeUsbPid) == "PID_" + std::string(kProbeUsbPidHex));
}

// ---------------------------------------------------------------------------
// listSerialPortInfo() itself. It reports whatever is plugged into the machine
// running the suite, so a test cannot assert WHAT it finds -- but it can assert
// that everything it finds is well formed, and those invariants are what a
// broken walk or a broken filter would violate. On a machine with no serial
// ports this passes without checking anything, which is stated here rather than
// hidden: the real evidence for this function is the by-hand run against the
// board, recorded in the branch's comments.
// ---------------------------------------------------------------------------

TEST_CASE("listSerialPortInfo reports only well-formed entries") {
    for (const auto& info : listSerialPortInfo()) {
        CHECK_FALSE(info.port.empty());
#if !defined(_WIN32)
        // Gated because the Windows branch reports "COM69" and a Windows
        // device instance id, neither of which these invariants describe. The
        // Windows branch is the verified reference implementation and is not
        // what this component changed.
        const std::filesystem::path path(info.port);
        CHECK(isUsbSerialPortName(path.filename().string()));
        CHECK(path.parent_path() == "/dev");
        // Either the identity was readable, or it was not. What it must never
        // be is a string that only half parses -- looksLikeProberUsbId() would
        // then be judging something no branch of it understands.
        if (!info.usbId.empty()) {
            CHECK(info.usbId.rfind("usb:v", 0) == 0);
            CHECK(info.usbId.size() >= 14);   // "usb:v" + 4 + "p" + 4
        }
#endif
    }
    // Names are unique per call; the deduplication in listSerialPorts() must
    // not be load-bearing for correctness, only for tidiness.
    const auto names = listSerialPorts();
    CHECK(names.size() == dedupePortNames(names).size());
}
