#include "platform/fwSerialPorts.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

#if defined(_WIN32)
  #include <windows.h>
  #include <devguid.h>
  #include <setupapi.h>
#elif !defined(__EMSCRIPTEN__)
  #include <cerrno>
  #include <fcntl.h>
  #include <sys/ioctl.h>
  #include <termios.h>
  #include <unistd.h>
  #include <chrono>
  #include <thread>
  #if defined(__APPLE__)
    // IOKit's serial registry: the macOS answer to /sys/class/tty, and the
    // only place a /dev/cu.* node's USB vendor/product identity can be read.
    #include <CoreFoundation/CoreFoundation.h>
    #include <IOKit/IOKitLib.h>
    #include <IOKit/serial/IOSerialKeys.h>
  #endif
#endif

namespace fwog {
namespace {

/// Strip the line terminator a caller must never see. Only CR and LF are
/// removed, and only from the END: leading or interior junk is left alone so
/// that parseProbeLine() can reject it, rather than being tidied into
/// something that parses.
std::string stripEol(std::string s)
{
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

/// Sysfs attribute files end in a newline; nothing else here wants it.
std::string trimmed(std::string_view s)
{
    std::size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return std::string(s.substr(b, e - b));
}

bool isHexRun(std::string_view s, std::size_t n)
{
    if (s.size() != n) return false;
    for (char c : s)
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    return true;
}

std::string uppered(std::string s)
{
    for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

/// Read the first line of a sysfs attribute file. Returns "" for anything that
/// is not a readable file, which is the same answer as "the attribute is not
/// there" -- and it has to be the same answer, because the difference between
/// "absent" and "unreadable" is not one any caller here could act on
/// differently: both mean the identity cannot be stated.
std::string readSysfsAttr(const std::filesystem::path& p)
{
    std::ifstream in(p);
    if (!in) return {};
    std::string s;
    std::getline(in, s);
    return s;
}

} // namespace

std::string makeLinuxUsbId(const LinuxUsbAttrs& attrs)
{
    const std::string vid = trimmed(attrs.idVendor);
    const std::string pid = trimmed(attrs.idProduct);
    // Four hex digits each, or this is not a USB vendor/product pair and
    // saying so would be worse than saying nothing -- see the header.
    if (!isHexRun(vid, 4) || !isHexRun(pid, 4)) return {};

    // Uppercase hex, matching the kernel's own MODALIAS spelling
    // ("usb:v2E8Ap000A..."), so that the borrowed notation is borrowed exactly.
    std::string id = "usb:v" + uppered(vid) + "p" + uppered(pid);

    // A non-composite device has no interface number, and that absence is
    // information rather than a failure: looksLikeProberUsbId() accepts an id
    // that names no interface, and refuses one that names the WRONG interface.
    // Emitting "in" with nothing after it, or a made-up "in00", would turn the
    // first case into the second.
    const std::string iface = trimmed(attrs.bInterfaceNumber);
    if (isHexRun(iface, 2)) id += "in" + uppered(iface);

    // The sysfs bus id is what makes this name one PHYSICAL port rather than a
    // class of device -- two identical boards plugged in at once share every
    // field before it and differ only here. It is never matched on: the
    // predicate stops at this colon.
    //
    // Nothing prints usbId today, so this suffix has no consumer, and that is a
    // deliberate choice rather than an oversight. Dropping it would make the id
    // of two identical boards identical, which is the one property that would
    // turn "which port is this" from answerable into unanswerable -- and it
    // would be dropped precisely when it is cheapest to keep and restored only
    // after someone had already been misled by an ambiguous id in a bug report.
    // It costs the length of a sysfs directory name.
    const std::string bus = trimmed(attrs.busId);
    if (!bus.empty()) id += ":" + bus;
    return id;
}

std::string usbIdForSysfsTtyDir(const std::filesystem::path& sysClassTtyEntry)
{
    std::error_code ec;
    auto dir = std::filesystem::canonical(sysClassTtyEntry / "device", ec);
    // No `device` symlink at all: a pty, or a port that vanished between the
    // directory listing and this call. Either way the honest answer is
    // "unknown", not a guess.
    if (ec) return {};

    for (int hop = 0; hop < 4; ++hop) {
        LinuxUsbAttrs attrs;
        attrs.bInterfaceNumber = readSysfsAttr(dir / "bInterfaceNumber");
        if (!attrs.bInterfaceNumber.empty()) {
            attrs.busId     = dir.filename().string();
            attrs.idVendor  = readSysfsAttr(dir.parent_path() / "idVendor");
            attrs.idProduct = readSysfsAttr(dir.parent_path() / "idProduct");
            // makeLinuxUsbId() returns "" if the vendor/product pair did not
            // read back as four hex digits each, and "" is the truth then.
            return makeLinuxUsbId(attrs);
        }
        if (!dir.has_parent_path() || dir.parent_path() == dir) break;
        dir = dir.parent_path();
    }
    return {};   // reached the bound without finding a USB interface
}

bool isUsbSerialPortName(std::string_view name)
{
    // The macOS spellings. A CDC-ACM device's callout node is
    // cu.usbmodem<suffix> and a vendor-driver serial's is cu.usbserial<suffix>
    // (the suffix conventionally starts with '-', but it is driver-chosen and
    // not worth matching on). The suffix must be present: a bare "cu.usbmodem"
    // is not a name macOS produces, and accepting it would let a lookalike
    // through on the strength of its prefix alone. cu.* rather than tty.* on
    // purpose -- the callout device opens without waiting for carrier, which
    // is what every open() in this project wants.
    if (name.rfind("cu.usbmodem", 0) == 0)  return name.size() > 11;
    if (name.rfind("cu.usbserial", 0) == 0) return name.size() > 12;

    std::string_view digits;
    if (name.rfind("ttyACM", 0) == 0)      digits = name.substr(6);
    else if (name.rfind("ttyUSB", 0) == 0) digits = name.substr(6);
    else return false;
    if (digits.empty()) return false;
    for (char c : digits)
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    return true;
}

std::vector<std::string> dedupePortNames(std::vector<std::string> ports)
{
    // First occurrence wins and the surviving order is preserved, rather than
    // sort-and-unique: callers document their order as "whatever the OS said",
    // and quietly re-sorting it would make that comment false.
    std::unordered_set<std::string> seen;
    std::vector<std::string> out;
    out.reserve(ports.size());
    for (auto& p : ports)
        if (seen.insert(p).second) out.push_back(std::move(p));
    return out;
}

std::vector<std::string> listSerialPorts()
{
    std::vector<std::string> names;
    for (auto& info : listSerialPortInfo()) names.push_back(std::move(info.port));
    return dedupePortNames(std::move(names));
}

#if defined(_WIN32)

std::vector<SerialPortInfo> listSerialPortInfo()
{
    // GUID_DEVCLASS_PORTS is Device Manager's "Ports (COM & LPT)" category, and
    // DIGCF_PRESENT restricts it to devnodes that are ATTACHED RIGHT NOW. That
    // is the whole difference from the SERIALCOMM key this used to read -- see
    // the header for the captured evidence of what that key actually contains.
    std::vector<SerialPortInfo> ports;
    const HDEVINFO set =
        SetupDiGetClassDevsA(&GUID_DEVCLASS_PORTS, nullptr, nullptr, DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) return ports;   // could not look: fail closed

    SP_DEVINFO_DATA dev = {};
    dev.cbSize = sizeof(dev);
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &dev); ++i) {
        // The COM name lives in the devnode's own "Device Parameters" subkey,
        // under PortName. This is the string Device Manager shows in brackets,
        // and it is written by the port's driver when the name is assigned.
        const HKEY key =
            SetupDiOpenDevRegKey(set, &dev, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
        if (key == INVALID_HANDLE_VALUE) continue;

        char  nameBuf[64] = {};
        DWORD nameLen = sizeof(nameBuf);
        DWORD type = 0;
        const LSTATUS rc = RegQueryValueExA(key, "PortName", nullptr, &type,
                                            reinterpret_cast<LPBYTE>(nameBuf), &nameLen);
        RegCloseKey(key);
        if (rc != ERROR_SUCCESS || type != REG_SZ) continue;
        // nameLen counts the NUL for values the drivers write, but the API does
        // not promise the data is NUL-terminated at all, so build from an
        // explicit length and trim.
        std::string name(nameBuf, nameLen ? nameLen - 1 : 0);
        while (!name.empty() && name.back() == '\0') name.pop_back();
        // This class also holds LPT ports, which are not serial and cannot be
        // read; nothing here has any use for them.
        if (name.rfind("COM", 0) != 0) continue;

        std::string id;
        char  idBuf[512] = {};
        DWORD idLen = 0;
        if (SetupDiGetDeviceInstanceIdA(set, &dev, idBuf, DWORD(sizeof(idBuf)), &idLen)) {
            id.assign(idBuf);
            // Upper-cased once, here, so that every comparison downstream can
            // be a plain one. Windows is inconsistent about the case of these
            // strings and a case-sensitive match against them is a latent bug.
            for (char& c : id) c = char(std::toupper(static_cast<unsigned char>(c)));
        }
        // An id we could not read is left EMPTY rather than guessed at: the
        // callers treat empty as "unknown", which is the truth.

        ports.push_back(SerialPortInfo{ std::move(name), std::move(id) });
    }
    SetupDiDestroyDeviceInfoList(set);
    return ports;
}

std::optional<std::string> readSerialLine(const std::string& port, int timeoutMs)
{
    // The "\\.\COMxx" form is required for port numbers above 9 -- same reason
    // touchPort1200() uses it.
    const std::string path = "\\\\.\\" + port;
    HANDLE h = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return std::nullopt;

    DCB dcb = {};
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(h, &dcb)) {
        CloseHandle(h);
        return std::nullopt;
    }
    // 115200, NOT 1200. See readSerialLine()'s header comment: 1200 would
    // reboot the CPU we are trying to read from.
    dcb.BaudRate    = CBR_115200;
    dcb.ByteSize    = 8;
    dcb.Parity      = NOPARITY;
    dcb.StopBits    = ONESTOPBIT;
    // DTR asserted, or the prober never writes anything -- see the header.
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fRtsControl = RTS_CONTROL_ENABLE;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDsrSensitivity = FALSE;
    dcb.fOutX = FALSE;
    dcb.fInX  = FALSE;
    if (!SetCommState(h, &dcb)) {
        CloseHandle(h);
        return std::nullopt;
    }
    EscapeCommFunction(h, SETDTR);

    // Short per-read timeouts, with the OVERALL budget enforced by the loop
    // below against a monotonic clock. A single long ReadFile timeout would be
    // simpler and wrong: it could not distinguish "nothing yet" from "a
    // partial line", and it would overshoot the caller's budget by up to one
    // whole read.
    COMMTIMEOUTS timeouts = {};
    timeouts.ReadIntervalTimeout        = 50;
    timeouts.ReadTotalTimeoutConstant   = 100;
    timeouts.ReadTotalTimeoutMultiplier = 0;
    SetCommTimeouts(h, &timeouts);

    const ULONGLONG start = GetTickCount64();
    std::string line;
    for (;;) {
        char  buf[128];
        DWORD read = 0;
        if (ReadFile(h, buf, sizeof(buf), &read, nullptr) && read > 0) {
            for (DWORD i = 0; i < read; ++i) {
                if (buf[i] == '\n') {
                    CloseHandle(h);
                    return stripEol(line);
                }
                // Bounded: a device that never sends a newline must not be
                // able to grow this without limit. 4 KB is far more than any
                // legitimate line and far less than a memory problem.
                if (line.size() < 4096) line.push_back(buf[i]);
            }
        }
        if (GetTickCount64() - start >= static_cast<ULONGLONG>(timeoutMs > 0 ? timeoutMs : 0))
            break;
    }
    CloseHandle(h);
    return std::nullopt;   // no COMPLETE line arrived: a partial one is not an answer
}

#elif defined(__EMSCRIPTEN__)

// The web build cannot reach serial ports at all (kDeviceSupportAvailable is
// false there and every device affordance is already disabled), so these are
// the honest answers rather than stubs that pretend.
std::vector<SerialPortInfo> listSerialPortInfo() { return {}; }

std::optional<std::string> readSerialLine(const std::string&, int) { return std::nullopt; }

#elif defined(__APPLE__)

// The IOKit registry, not a /dev glob. A directory listing of /dev could name
// the ports but could never say WHICH USB device a port belongs to, and that
// identity is this function's whole reason to exist -- an empty usbId reads
// downstream as "identity unknown", which the header calls out as the silent
// failure mode. IOSerialBSDClient nodes carry the callout path, and their
// ancestor chain in the service plane carries bInterfaceNumber (interface
// node), idVendor/idProduct and locationID (device node), which
// IORegistryEntrySearchCFProperty(kIORegistryIterateParents) reads without
// hand-walking the tree.
//
// The id is built by makeLinuxUsbId() ON PURPOSE: "usb:vXXXXpYYYY[inZZ]:bus"
// is the one non-Windows shape looksLikeProberUsbId() parses, and inventing a
// macOS-only spelling would mean teaching that predicate a third branch for
// zero information gained. locationID stands in for the sysfs bus id -- like
// the bus id it names one physical port, it is never matched on, and it keeps
// two identical boards' ids distinct.
//
// BOARD-VERIFIED ON MACOS 2026-08-14, against a real FreeWili 1-OG -- that
// run predates the v2 rebase; on the rebased branch only the build and test
// suite have been re-verified, the flash has not been re-run.

namespace {

std::optional<uint32_t> ioNumberProp(io_object_t svc, CFStringRef key)
{
    CFTypeRef ref = IORegistryEntrySearchCFProperty(
        svc, kIOServicePlane, key, kCFAllocatorDefault,
        kIORegistryIterateRecursively | kIORegistryIterateParents);
    if (!ref) return std::nullopt;
    std::optional<uint32_t> out;
    if (CFGetTypeID(ref) == CFNumberGetTypeID()) {
        int64_t v = 0;
        if (CFNumberGetValue(static_cast<CFNumberRef>(ref), kCFNumberSInt64Type, &v)
            && v >= 0)
            out = static_cast<uint32_t>(v);
    }
    CFRelease(ref);
    return out;
}

std::string ioStringProp(io_object_t svc, CFStringRef key)
{
    CFTypeRef ref = IORegistryEntryCreateCFProperty(svc, key, kCFAllocatorDefault, 0);
    if (!ref) return {};
    std::string out;
    if (CFGetTypeID(ref) == CFStringGetTypeID()) {
        char buf[512] = {};
        if (CFStringGetCString(static_cast<CFStringRef>(ref), buf, sizeof(buf),
                               kCFStringEncodingUTF8))
            out = buf;
    }
    CFRelease(ref);
    return out;
}

std::string hexField(uint32_t v, int digits)
{
    char buf[16] = {};
    std::snprintf(buf, sizeof(buf), "%0*x", digits, v);
    return buf;
}

} // namespace

std::vector<SerialPortInfo> listSerialPortInfo()
{
    std::vector<SerialPortInfo> ports;

    CFMutableDictionaryRef match = IOServiceMatching(kIOSerialBSDServiceValue);
    if (!match) return ports;   // could not ask: fail closed, same as Linux
    CFDictionarySetValue(match, CFSTR(kIOSerialBSDTypeKey),
                         CFSTR(kIOSerialBSDAllTypes));

    io_iterator_t it = IO_OBJECT_NULL;
    // The call consumes `match`, success or not.
    if (IOServiceGetMatchingServices(kIOMainPortDefault, match, &it) != KERN_SUCCESS)
        return ports;

    for (io_object_t svc; (svc = IOIteratorNext(it)) != IO_OBJECT_NULL;
         IOObjectRelease(svc)) {
        const std::string dev = ioStringProp(svc, CFSTR(kIOCalloutDeviceKey));
        if (dev.empty()) continue;

        // Same gate the Linux branch applies to /sys/class/tty entries: only
        // names shaped like a USB serial port. Bluetooth SPP ports
        // (cu.Bluetooth-*), debug consoles and the like are not candidates and
        // must not spend one of identifyCpus()'s bounded port questions.
        const std::string name = std::filesystem::path(dev).filename().string();
        if (!isUsbSerialPortName(name)) continue;

        // The registry can outrun devfs the same way sysfs outruns udev; a
        // path that cannot be opened is not worth returning. See the Linux
        // branch for the full argument.
        std::error_code existsEc;
        if (!std::filesystem::exists(dev, existsEc) || existsEc) continue;

        LinuxUsbAttrs attrs;
        if (const auto vid = ioNumberProp(svc, CFSTR("idVendor")))
            attrs.idVendor = hexField(*vid, 4);
        if (const auto pid = ioNumberProp(svc, CFSTR("idProduct")))
            attrs.idProduct = hexField(*pid, 4);
        // bInterfaceNumber is deliberately NOT reported, and this is the one
        // place this branch knowingly diverges from Linux. MEASURED against an
        // attached FreeWili 1-OG: the ancestor interface of an IOSerialBSDClient
        // here is the CDC DATA interface -- both 093C CDC ports read back
        // bInterfaceNumber=1 -- where Linux's ttyACM reports the COMM
        // interface, 0. Reporting the honest "01" would make
        // looksLikeProberUsbId() refuse the prober's own port (it accepts
        // interface 00 or none), and mapping 1 back to 0 would be inventing a
        // number the registry did not say. Omitting it loses nothing that
        // property exists to protect: the rule's target is pico_stdio_usb's
        // Reset interface (2), which never owns a serial node on macOS, so
        // there is no wrong-interface port here to refuse.
        if (const auto loc = ioNumberProp(svc, CFSTR("locationID")))
            attrs.busId = hexField(*loc, 8);
        // makeLinuxUsbId() returns "" unless vendor+product both read back as
        // clean hex, and "" is the honest answer then -- identical to Linux.
        ports.push_back(SerialPortInfo{ dev, makeLinuxUsbId(attrs) });
    }
    IOObjectRelease(it);
    return ports;
}

#else

// WAS UNVERIFIED, NOW MEASURED. When this branch was written no Linux
// toolchain existed on the developer's machine, so usbId was left EMPTY
// deliberately: deriving it means walking sysfs, and writing unverifiable code
// whose whole job is to identify which CPU may be written to was the wrong
// trade. That reason has expired. Everything below has been compiled with GCC
// and run against an attached FreeWili 1-OG (093c:2054 MainCPU and 093c:2055
// DisplayCPU, two cdc_acm ports) with an unrelated FTDI 0403:6014 on the same
// hub, and the sysfs layout described in the comments was read off that
// machine rather than assumed.
//
// WHAT WAS *NOT* VERIFIED, and must not be read as verified: no board running
// the PROBER image (2E8A:000A) was attached while this was written, because
// getting one into that state means the 1200-baud reset path and that belongs
// to a different change. So the prober's own usbId string has never been
// observed coming out of this function. What HAS been checked is that the
// three real devices present produce ids that looksLikeProberUsbId() refuses,
// and that a synthesised 2E8A:000A id of exactly the shape this function
// builds is accepted -- see tests/test_fwSerialPorts.cpp.

std::vector<SerialPortInfo> listSerialPortInfo()
{
    std::vector<SerialPortInfo> ports;
    std::error_code ec;

    // /sys/class/tty, not /dev -- see the header for why the difference is the
    // same one that made SERIALCOMM the wrong question on Windows.
    std::filesystem::directory_iterator it("/sys/class/tty", ec);
    if (ec) return ports;   // could not look: fail closed, as documented

    const std::filesystem::directory_iterator end;
    while (it != end) {
        const std::string name = it->path().filename().string();
        if (isUsbSerialPortName(name)) {
            // The caller is handed something it can open, and /dev/<name> is
            // what that is. Checking it exists is not paranoia: /sys/class/tty
            // is populated by the kernel and /dev by udev, so there is a real
            // (if brief) window in which the kernel knows about a port and no
            // node has been created for it, and handing back a path that
            // cannot be opened would spend one of identifyCpus()'s bounded
            // port questions on nothing.
            const std::string dev = "/dev/" + name;
            std::error_code existsEc;
            if (std::filesystem::exists(dev, existsEc) && !existsEc)
                ports.push_back(SerialPortInfo{ dev, usbIdForSysfsTtyDir(it->path()) });
        }
        // The error_code overload of increment(), because the throwing one is
        // what a range-for uses and this runs on a worker thread inside a
        // function documented to "return empty on any failure" -- an escaping
        // filesystem_error would be neither. A device unplugged mid-listing is
        // exactly how this fails in practice.
        it.increment(ec);
        if (ec) return {};   // could not finish looking: same answer as could not look
    }
    return ports;
}

#endif   // per-platform listSerialPortInfo

// One termios implementation for every POSIX platform: measured on Linux
// against an attached board (see the branch comment above), and since RUN on
// macOS too, not merely compiled -- the 1200-baud touch reset a real board
// from a Mac on 2026-08-14, on the /dev/cu.* callout node the mac branch
// lists. That run predates the v2 rebase; on the rebased branch only the
// build and test suite have been re-verified, the touch has not been re-run.
#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)

std::optional<std::string> readSerialLine(const std::string& port, int timeoutMs)
{
    int fd = ::open(port.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) return std::nullopt;

    termios tio = {};
    if (tcgetattr(fd, &tio) != 0) {
        ::close(fd);
        return std::nullopt;
    }
    cfmakeraw(&tio);
    // 115200, NOT 1200 -- see the header comment.
    cfsetispeed(&tio, B115200);
    cfsetospeed(&tio, B115200);
    tio.c_cflag |= (CLOCAL | CREAD);
    tio.c_cc[VMIN]  = 0;
    tio.c_cc[VTIME] = 1;
    if (tcsetattr(fd, TCSANOW, &tio) != 0) {
        ::close(fd);
        return std::nullopt;
    }

    // DTR asserted EXPLICITLY, which is what the Windows branch has always done
    // and this branch did not. The header says why it is load-bearing: without
    // it pico_stdio_usb never writes and a healthy board reads as silent.
    //
    // MEASURED on the machine this was written on (cdc_acm, an attached
    // FreeWili 1-OG), with TIOCMGET before and after each step:
    //   * open() came back with DTR and RTS already high, both with and without
    //     O_NONBLOCK, on a first open and on a second open taken while another
    //     fd held the same port with DTR pulled low;
    //   * the tcsetattr() above neither dropped DTR nor raised it -- a port
    //     whose DTR was low before it stayed low after;
    //   * TIOCMBIS raised it in every case.
    // So on that kernel this ioctl changes nothing observable. It is here
    // because the second measurement is the one that matters: nothing in the
    // sequence above would REPAIR a low DTR, so without this the contract holds
    // only for as long as open() keeps volunteering something this code never
    // asked for. Its return value is not checked for the same reason
    // EscapeCommFunction's is not on Windows -- a port that refuses the ioctl
    // may still be readable, and the read below is the real test.
    int modemBits = TIOCM_DTR | TIOCM_RTS;
    ::ioctl(fd, TIOCMBIS, &modemBits);

    const auto start = std::chrono::steady_clock::now();
    std::string line;
    for (;;) {
        char buf[128];
        const auto n = ::read(fd, buf, sizeof(buf));
        const bool interrupted = (n < 0 && errno == EINTR);
        for (ssize_t i = 0; i < n; ++i) {
            if (buf[i] == '\n') {
                ::close(fd);
                return stripEol(line);
            }
            if (line.size() < 4096) line.push_back(buf[i]);
        }
        // Checked every pass, including the passes where read() DID return
        // data: a device that streams bytes forever and never a newline must
        // hit the caller's budget like any other, and a loop that only checked
        // the clock on empty reads would never leave.
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        if (elapsed >= timeoutMs) break;
        // The fd is still O_NONBLOCK from the open, so read() returns EAGAIN
        // immediately rather than honouring VTIME, and this sleep is what
        // actually paces the loop -- 20 ms against the 2 s slices
        // identifyCpus() asks for (kProbeLineWaitMs / kProbeLineAttempts).
        //
        // EINTR is exempted because it means "ask again", not "no data": a
        // signal is not evidence about the device and should not cost a sleep.
        // The exemption is placed AFTER the timeout check on purpose -- a
        // `continue` above it would let a stream of signals keep this loop
        // alive past the caller's budget, on a worker thread, forever.
        if (n <= 0 && !interrupted)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    ::close(fd);
    return std::nullopt;
}

#endif

} // namespace fwog
