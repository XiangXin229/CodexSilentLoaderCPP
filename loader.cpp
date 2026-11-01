// CodexSilentLoader (native C++ / Win32)
// Launches the installed OpenAI.Codex MSIX app with a local DevTools port and
// injects a small renderer script that forces the Statsig i18n gate on.
//
// Build: see build.bat (MSVC, /MT, /SUBSYSTEM:WINDOWS -> one exe).

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#include <shobjidl.h>
#include <appmodel.h>
#include <tlhelp32.h>
#include <iphlpapi.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ws2_32.lib")

namespace {

constexpr const wchar_t* kPackageFamilyName = L"OpenAI.Codex_2p2nqsd0c76g0";
constexpr const wchar_t* kAppIdSuffix = L"!App";
constexpr int kTimeoutSeconds = 20;
constexpr int kGraceSeconds = 3;

// ---------------------------------------------------------------------------
// small helpers
// ---------------------------------------------------------------------------

std::string W2U(const std::wstring& value) {
    if (value.empty())
        return {};
    const int needed = WideCharToMultiByte(
        CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    if (needed <= 0)
        return {};
    std::string out(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()),
        out.data(), needed, nullptr, nullptr);
    return out;
}

std::wstring U2W(const std::string& value) {
    if (value.empty())
        return {};
    const int needed = MultiByteToWideChar(
        CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()),
        nullptr, 0);
    if (needed <= 0)
        return {};
    std::wstring out(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(
        CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()),
        out.data(), needed);
    return out;
}

std::string ToLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool StartsWith(const std::string& value, const std::string& prefix) {
    return value.size() >= prefix.size() &&
           value.compare(0, prefix.size(), prefix) == 0;
}

bool EndsWithNoCase(const std::string& value, const std::string& suffix) {
    if (value.size() < suffix.size())
        return false;
    return ToLower(value.substr(value.size() - suffix.size())) == ToLower(suffix);
}

bool ContainsNoCase(const std::string& value, const std::string& needle) {
    return ToLower(value).find(ToLower(needle)) != std::string::npos;
}

std::wstring TrimW(const std::wstring& value) {
    size_t begin = 0;
    size_t end = value.size();
    while (begin < end && iswspace(value[begin]))
        ++begin;
    while (end > begin && iswspace(value[end - 1]))
        --end;
    return value.substr(begin, end - begin);
}

std::string HexDword(DWORD value) {
    char buffer[16]{};
    std::snprintf(buffer, sizeof(buffer), "0x%08lX", static_cast<unsigned long>(value));
    return buffer;
}

std::filesystem::path LocalAppDataDir() {
    wchar_t buffer[MAX_PATH]{};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
        return std::filesystem::temp_directory_path();
    return std::filesystem::path(buffer);
}

// ---------------------------------------------------------------------------
// logging
// ---------------------------------------------------------------------------

class Logger {
public:
    Logger(bool enable_file, bool attach_console) : enabled_(enable_file) {
        if (attach_console)
            AttachConsoleForLogging();

        if (!enable_file)
            return;

        std::error_code ec;
        wchar_t current[MAX_PATH]{};
        std::filesystem::path directory;
        if (GetCurrentDirectoryW(MAX_PATH, current) > 0)
            directory = current;
        else
            directory = LocalAppDataDir();

        file_path_ = directory / L"CodexSilentLoader.log";
        file_.open(file_path_, std::ios::binary | std::ios::trunc);

        if (!file_.is_open()) {
            wchar_t exe[MAX_PATH]{};
            if (GetModuleFileNameW(nullptr, exe, MAX_PATH) > 0) {
                file_path_ = std::filesystem::path(exe).parent_path() /
                             L"CodexSilentLoader.log";
                file_.open(file_path_, std::ios::binary | std::ios::trunc);
            }
        }

        if (!file_.is_open()) {
            const auto fallback = LocalAppDataDir() / L"CodexSilentLoaderCPP";
            std::filesystem::create_directories(fallback, ec);
            file_path_ = fallback / L"CodexSilentLoader.log";
            file_.open(file_path_, std::ios::binary | std::ios::trunc);
        }
    }

    ~Logger() = default;

    const std::filesystem::path& file_path() const { return file_path_; }
    bool enabled() const {
        return enabled_ &&
               (file_.is_open() || console_handle_ != INVALID_HANDLE_VALUE);
    }

    void info(const std::string& message) { write("信息", message); }
    void warn(const std::string& message) { write("警告", message); }
    void error(const std::string& message) { write("错误", message); }

private:
    void AttachConsoleForLogging() {
        if (!AttachConsole(ATTACH_PARENT_PROCESS))
            return;
        SetConsoleOutputCP(CP_UTF8);
        console_handle_ = CreateFileW(
            L"CONOUT$", GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, 0, nullptr);
    }

    void write(const char* level, const std::string& message) {
        if (!enabled_)
            return;

        std::lock_guard<std::mutex> lock(mutex_);

        SYSTEMTIME now{};
        GetLocalTime(&now);
        char timestamp[48]{};
        std::snprintf(timestamp, sizeof(timestamp),
                      "%04u-%02u-%02u %02u:%02u:%02u.%03u",
                      now.wYear, now.wMonth, now.wDay,
                      now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);

        std::string line;
        line.reserve(message.size() + 64);
        line += '[';
        line += timestamp;
        line += "] [";
        line += level;
        line += "] ";
        line += message;
        line += "\r\n";

        if (file_.is_open()) {
            file_.write(line.data(), static_cast<std::streamsize>(line.size()));
            file_.flush();
        }

        if (console_handle_ != INVALID_HANDLE_VALUE && console_handle_ != nullptr) {
            DWORD written = 0;
            WriteFile(console_handle_, line.data(),
                      static_cast<DWORD>(line.size()), &written, nullptr);
        }
    }

    bool enabled_ = false;
    HANDLE console_handle_ = INVALID_HANDLE_VALUE;
    std::filesystem::path file_path_;
    std::ofstream file_;
    std::mutex mutex_;
};

// ---------------------------------------------------------------------------
// options
// ---------------------------------------------------------------------------

struct Options {
    bool log = false;
};

const wchar_t* kUsage =
    L"用法：CodexSilentLoader.exe [-log]\n\n"
    L"  -log   在当前目录写入 CodexSilentLoader.log，\n"
    L"         并输出到已附加的控制台。";

bool ParseOptions(int argc, wchar_t** argv, Options& options, std::wstring& error) {
    for (int i = 1; i < argc; ++i) {
        std::wstring lower = argv[i];
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });

        if (lower == L"-log" || lower == L"--log") {
            options.log = true;
        } else {
            error = L"未知参数：" + std::wstring(argv[i]);
            return false;
        }
    }
    return true;
}

void ShowError(const std::wstring& message) {
    MessageBoxW(nullptr, message.c_str(), L"CodexSilentLoader",
                MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
}

void ShowInfo(const std::wstring& message) {
    MessageBoxW(nullptr, message.c_str(), L"CodexSilentLoader",
                MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
}

// ---------------------------------------------------------------------------
// JSON parser (small, enough for ASAR headers and CDP responses)
// ---------------------------------------------------------------------------

struct JVal {
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string string;
    std::vector<JVal> array;
    std::map<std::string, JVal> object;

    const JVal* get(const std::string& key) const {
        if (type != Type::Object)
            return nullptr;
        const auto it = object.find(key);
        return it == object.end() ? nullptr : &it->second;
    }

    std::string as_string() const {
        return type == Type::String ? string : std::string();
    }

    int as_int(int fallback = 0) const {
        if (type == Type::Number)
            return static_cast<int>(number);
        if (type == Type::String) {
            try {
                return std::stoi(string);
            } catch (...) {
                return fallback;
            }
        }
        return fallback;
    }

    int64_t as_int64(int64_t fallback = -1) const {
        if (type == Type::Number)
            return static_cast<int64_t>(number);
        if (type == Type::String) {
            try {
                return std::stoll(string);
            } catch (...) {
                return fallback;
            }
        }
        return fallback;
    }
};

class JsonParser {
public:
    explicit JsonParser(const std::string& text) : text_(text) {}

    static JVal Parse(const std::string& text) {
        JsonParser parser(text);
        JVal value = parser.parse_value();
        parser.skip_ws();
        return value;
    }

private:
    const std::string& text_;
    size_t index_ = 0;

    void skip_ws() {
        while (index_ < text_.size() &&
               std::isspace(static_cast<unsigned char>(text_[index_])))
            ++index_;
    }

    char peek_raw() const {
        return index_ < text_.size() ? text_[index_] : '\0';
    }

    char take_raw() {
        if (index_ >= text_.size())
            throw std::runtime_error("unexpected end of JSON");
        return text_[index_++];
    }

    char take_token_char() {
        skip_ws();
        return take_raw();
    }

    void expect(char expected) {
        const char actual = take_token_char();
        if (actual != expected)
            throw std::runtime_error(std::string("expected '") + expected + "'");
    }

    static uint32_t parse_hex4(const std::string& text, size_t& index) {
        uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            if (index >= text.size())
                throw std::runtime_error("truncated \\u escape");
            const char c = text[index++];
            value <<= 4;
            if (c >= '0' && c <= '9')
                value |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f')
                value |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                value |= static_cast<uint32_t>(c - 'A' + 10);
            else
                throw std::runtime_error("bad \\u escape");
        }
        return value;
    }

    static void append_utf8(std::string& out, uint32_t code_point) {
        if (code_point <= 0x7F) {
            out.push_back(static_cast<char>(code_point));
        } else if (code_point <= 0x7FF) {
            out.push_back(static_cast<char>(0xC0 | (code_point >> 6)));
            out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
        } else if (code_point <= 0xFFFF) {
            out.push_back(static_cast<char>(0xE0 | (code_point >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (code_point >> 18)));
            out.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
        }
    }

    std::string parse_string() {
        skip_ws();
        if (peek_raw() != '"')
            throw std::runtime_error("expected string");
        take_raw();

        std::string out;
        while (true) {
            if (index_ >= text_.size())
                throw std::runtime_error("unterminated string");
            const char c = take_raw();
            if (c == '"')
                break;
            if (c != '\\') {
                out.push_back(c);
                continue;
            }

            const char escape = take_raw();
            switch (escape) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    uint32_t code_point = parse_hex4(text_, index_);
                    if (code_point >= 0xD800 && code_point <= 0xDBFF) {
                        if (index_ + 1 < text_.size() &&
                            text_[index_] == '\\' && text_[index_ + 1] == 'u') {
                            index_ += 2;
                            const uint32_t low = parse_hex4(text_, index_);
                            if (low >= 0xDC00 && low <= 0xDFFF) {
                                code_point = 0x10000 +
                                    ((code_point - 0xD800) << 10) +
                                    (low - 0xDC00);
                            }
                        }
                    }
                    append_utf8(out, code_point);
                    break;
                }
                default:
                    throw std::runtime_error("bad string escape");
            }
        }

        return out;
    }

    JVal parse_number() {
        skip_ws();
        const size_t start = index_;
        if (peek_raw() == '-')
            ++index_;
        while (index_ < text_.size()) {
            const char c = text_[index_];
            if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' ||
                c == '+' || c == '-') {
                ++index_;
            } else {
                break;
            }
        }
        if (start == index_)
            throw std::runtime_error("expected number");

        const std::string token = text_.substr(start, index_ - start);
        char* end = nullptr;
        const double value = std::strtod(token.c_str(), &end);
        if (end == token.c_str())
            throw std::runtime_error("bad number");

        JVal out;
        out.type = JVal::Type::Number;
        out.number = value;
        return out;
    }

    JVal parse_value() {
        skip_ws();
        const char c = peek_raw();
        if (c == '{')
            return parse_object();
        if (c == '[')
            return parse_array();
        if (c == '"') {
            JVal out;
            out.type = JVal::Type::String;
            out.string = parse_string();
            return out;
        }
        if (c == 't' || c == 'f') {
            const std::string literal = c == 't' ? "true" : "false";
            if (text_.compare(index_, literal.size(), literal) != 0)
                throw std::runtime_error("bad literal");
            index_ += literal.size();
            JVal out;
            out.type = JVal::Type::Bool;
            out.boolean = (c == 't');
            return out;
        }
        if (c == 'n') {
            if (text_.compare(index_, 4, "null") != 0)
                throw std::runtime_error("bad null");
            index_ += 4;
            return JVal{};
        }
        return parse_number();
    }

    JVal parse_array() {
        expect('[');
        JVal out;
        out.type = JVal::Type::Array;

        skip_ws();
        if (peek_raw() == ']') {
            take_raw();
            return out;
        }

        while (true) {
            out.array.push_back(parse_value());
            skip_ws();
            const char c = take_raw();
            if (c == ']')
                break;
            if (c != ',')
                throw std::runtime_error("expected ',' or ']'");
        }

        return out;
    }

    JVal parse_object() {
        expect('{');
        JVal out;
        out.type = JVal::Type::Object;

        skip_ws();
        if (peek_raw() == '}') {
            take_raw();
            return out;
        }

        while (true) {
            std::string key = parse_string();
            expect(':');
            out.object.emplace(std::move(key), parse_value());
            skip_ws();
            const char c = take_raw();
            if (c == '}')
                break;
            if (c != ',')
                throw std::runtime_error("expected ',' or '}'");
        }

        return out;
    }
};

std::string JsonEscape(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 32);
    for (const unsigned char c : value) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buffer[8]{};
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
                    out += buffer;
                } else {
                    out.push_back(static_cast<char>(c));
                }
                break;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// package discovery
// ---------------------------------------------------------------------------

struct PackageInfo {
    std::wstring family_name;
    std::wstring full_name;
    std::wstring version;
    std::wstring install_path;
    std::wstring aumid;
    std::wstring exe_path;
    uint64_t version_key = 0;
};

bool FileExists(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

uint64_t ParseVersionKey(const std::wstring& version) {
    unsigned major = 0, minor = 0, build = 0, revision = 0;
    swscanf_s(version.c_str(), L"%u.%u.%u.%u", &major, &minor, &build, &revision);
    return (static_cast<uint64_t>(major) << 48) |
           (static_cast<uint64_t>(minor) << 32) |
           (static_cast<uint64_t>(build) << 16) |
           static_cast<uint64_t>(revision);
}

std::optional<std::wstring> GetPackagePath(const std::wstring& full_name, Logger& log) {
    UINT32 length = 0;
    LONG result = GetPackagePathByFullName(full_name.c_str(), &length, nullptr);
    if (result != ERROR_INSUFFICIENT_BUFFER) {
        log.error("获取安装包路径所需缓冲区大小失败：" + W2U(full_name) +
                  ": " + HexDword(static_cast<DWORD>(result)));
        return std::nullopt;
    }

    std::vector<wchar_t> buffer(length + 1, L'\0');
    result = GetPackagePathByFullName(full_name.c_str(), &length, buffer.data());
    if (result != ERROR_SUCCESS) {
        log.error("获取安装包路径失败：" + W2U(full_name) +
                  ": " + HexDword(static_cast<DWORD>(result)));
        return std::nullopt;
    }

    return std::wstring(buffer.data());
}

std::optional<PackageInfo> FindLatestPackage(Logger& log) {
    UINT32 count = 0;
    UINT32 buffer_length = 0;
    LONG result = GetPackagesByPackageFamily(
        kPackageFamilyName, &count, nullptr, &buffer_length, nullptr);

    if (result == ERROR_SUCCESS && count == 0) {
        log.error("未找到匹配的安装包。");
        return std::nullopt;
    }

    if (result != ERROR_INSUFFICIENT_BUFFER) {
        log.error("获取安装包列表所需缓冲区大小失败：" +
                  HexDword(static_cast<DWORD>(result)));
        return std::nullopt;
    }

    std::vector<PWSTR> names(count, nullptr);
    std::vector<wchar_t> buffer(buffer_length + 1, L'\0');

    result = GetPackagesByPackageFamily(
        kPackageFamilyName, &count, names.data(), &buffer_length, buffer.data());
    if (result != ERROR_SUCCESS) {
        log.error("获取安装包列表失败：" +
                  HexDword(static_cast<DWORD>(result)));
        return std::nullopt;
    }

    PackageInfo best;
    bool found = false;

    for (UINT32 i = 0; i < count; ++i) {
        if (!names[i])
            continue;

        const std::wstring full_name = names[i];
        const size_t first = full_name.find(L'_');
        const size_t second = first == std::wstring::npos
            ? std::wstring::npos
            : full_name.find(L'_', first + 1);
        if (first == std::wstring::npos || second == std::wstring::npos)
            continue;

        const std::wstring version = full_name.substr(first + 1, second - first - 1);
        const auto install_path = GetPackagePath(full_name, log);
        if (!install_path)
            continue;

        const std::wstring exe_path = *install_path + L"\\app\\ChatGPT.exe";
        if (!FileExists(exe_path)) {
            log.warn("安装包中未找到 app\\ChatGPT.exe：" + W2U(full_name));
            continue;
        }

        PackageInfo candidate;
        candidate.family_name = kPackageFamilyName;
        candidate.full_name = full_name;
        candidate.version = version;
        candidate.version_key = ParseVersionKey(version);
        candidate.install_path = *install_path;
        candidate.aumid = std::wstring(kPackageFamilyName) + kAppIdSuffix;
        candidate.exe_path = exe_path;

        if (!found || candidate.version_key > best.version_key) {
            best = std::move(candidate);
            found = true;
        }
    }

    if (!found)
        return std::nullopt;

    return best;
}

// ---------------------------------------------------------------------------
// activation
// ---------------------------------------------------------------------------

HRESULT LaunchPackagedApp(
    const std::wstring& aumid,
    int debug_port,
    Logger& log)
{
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool should_uninitialize = init == S_OK || init == S_FALSE;
    if (FAILED(init) && init != RPC_E_CHANGED_MODE) {
        log.error("初始化 COM 失败：" + HexDword(static_cast<DWORD>(init)));
        return init;
    }

    IApplicationActivationManager* manager = nullptr;
    HRESULT result = CoCreateInstance(
        CLSID_ApplicationActivationManager,
        nullptr,
        CLSCTX_LOCAL_SERVER,
        IID_PPV_ARGS(&manager));

    if (SUCCEEDED(result)) {
        CoAllowSetForegroundWindow(manager, nullptr);

        wchar_t arguments[256]{};
        swprintf_s(arguments,
                   L"--remote-debugging-port=%d "
                   L"--remote-debugging-address=127.0.0.1 --lang=zh-CN",
                   debug_port);

        DWORD pid = 0;
        result = manager->ActivateApplication(
            aumid.c_str(), arguments, AO_NOSPLASHSCREEN, &pid);

        if (SUCCEEDED(result)) {
            log.info("启动应用成功：pid=" + std::to_string(pid) +
                     "，参数=" + W2U(arguments));
        } else {
            log.error("启动应用失败：" + HexDword(static_cast<DWORD>(result)));
        }

        manager->Release();
    } else {
        log.error("创建 ApplicationActivationManager 失败：" +
                  HexDword(static_cast<DWORD>(result)));
    }

    if (should_uninitialize)
        CoUninitialize();

    return result;
}

// ---------------------------------------------------------------------------
// Winsock helpers
// ---------------------------------------------------------------------------

int GetFreeLoopbackPort() {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
        return 0;

    const SOCKET socket_handle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket_handle == INVALID_SOCKET) {
        WSACleanup();
        return 0;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;

    int port = 0;
    if (bind(socket_handle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) {
        int length = sizeof(address);
        if (getsockname(socket_handle, reinterpret_cast<sockaddr*>(&address), &length) == 0)
            port = ntohs(address.sin_port);
    }

    closesocket(socket_handle);
    WSACleanup();
    return port;
}

// ---------------------------------------------------------------------------
// WinHTTP: /json/list and WebSocket
// ---------------------------------------------------------------------------

bool HttpGet(int port, const std::wstring& path, std::string& body, Logger& log) {
    body.clear();

    HINTERNET session = WinHttpOpen(
        L"CodexSilentLoader/1.0",
        WINHTTP_ACCESS_TYPE_NO_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0);
    if (!session) {
        log.warn("创建 WinHTTP 会话失败：" + HexDword(GetLastError()));
        return false;
    }

    WinHttpSetTimeouts(session, 1000, 1000, 3000, 3000);

    HINTERNET connection = WinHttpConnect(
        session, L"127.0.0.1", static_cast<INTERNET_PORT>(port), 0);
    if (!connection) {
        WinHttpCloseHandle(session);
        return false;
    }

    HINTERNET request = WinHttpOpenRequest(
        connection,
        L"GET",
        path.c_str(),
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        0);
    if (!request) {
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return false;
    }

    bool ok = false;
    if (WinHttpSendRequest(
            request,
            WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(request, nullptr)) {

        DWORD status = 0;
        DWORD status_size = sizeof(status);
        WinHttpQueryHeaders(
            request,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &status, &status_size, WINHTTP_NO_HEADER_INDEX);

        if (status == 200) {
            ok = true;
            while (true) {
                DWORD available = 0;
                if (!WinHttpQueryDataAvailable(request, &available) || available == 0)
                    break;

                if (body.size() > 16 * 1024 * 1024)
                    break;

                std::string chunk(available, '\0');
                DWORD read = 0;
                if (!WinHttpReadData(request, chunk.data(), available, &read) || read == 0)
                    break;
                body.append(chunk.data(), read);
            }
        }
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return ok;
}

class WebSocketClient {
public:
    WebSocketClient() = default;
    ~WebSocketClient() { Close(); }

    bool Connect(int port, const std::string& ws_url, Logger& log) {
        const std::wstring wide_url = U2W(ws_url);
        const size_t path_pos = wide_url.find(L"/devtools/page/");
        if (path_pos == std::wstring::npos) {
            log.error("无效的 WebSocket 地址：" + ws_url);
            return false;
        }
        const std::wstring path = wide_url.substr(path_pos);

        session_ = WinHttpOpen(
            L"CodexSilentLoader/1.0",
            WINHTTP_ACCESS_TYPE_NO_PROXY,
            WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS,
            0);
        if (!session_)
            return false;

        WinHttpSetTimeouts(session_, 2000, 2000, 5000, 5000);

        connection_ = WinHttpConnect(
            session_, L"127.0.0.1", static_cast<INTERNET_PORT>(port), 0);
        if (!connection_)
            return false;

        HINTERNET request = WinHttpOpenRequest(
            connection_,
            L"GET",
            path.c_str(),
            nullptr,
            WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES,
            0);
        if (!request)
            return false;

        WinHttpSetOption(request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0);

        bool upgraded = false;
        if (WinHttpSendRequest(
                request,
                WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(request, nullptr)) {

            socket_ = WinHttpWebSocketCompleteUpgrade(request, 0);
            upgraded = socket_ != nullptr;
        }

        WinHttpCloseHandle(request);

        if (!upgraded) {
            log.error("升级为 WebSocket 连接失败：" + HexDword(GetLastError()));
            Close();
        }

        return upgraded;
    }

    bool SendText(const std::string& text, Logger& log) {
        if (!socket_)
            return false;

        const DWORD result = WinHttpWebSocketSend(
            socket_,
            WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
            const_cast<char*>(text.data()),
            static_cast<DWORD>(text.size()));

        if (result != ERROR_SUCCESS) {
            log.warn("发送 WebSocket 消息失败：" + HexDword(result));
            return false;
        }
        return true;
    }

    bool ReceiveText(std::string& message, Logger& log) {
        message.clear();
        if (!socket_)
            return false;

        std::vector<char> buffer(64 * 1024);
        while (true) {
            DWORD read = 0;
            WINHTTP_WEB_SOCKET_BUFFER_TYPE type =
                WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE;

            const DWORD result = WinHttpWebSocketReceive(
                socket_,
                buffer.data(),
                static_cast<DWORD>(buffer.size()),
                &read,
                &type);

            if (result != ERROR_SUCCESS) {
                log.warn("接收 WebSocket 消息失败：" + HexDword(result));
                return false;
            }

            if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE)
                return false;

            message.append(buffer.data(), read);

            if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE)
                return true;

            if (message.size() > 32 * 1024 * 1024) {
                log.warn("WebSocket 消息过大。");
                return false;
            }
        }
    }

    JVal Call(const std::string& method, const std::string& params_json, Logger& log) {
        const int id = next_id_++;

        std::string request;
        request.reserve(128 + params_json.size());
        request += "{\"id\":";
        request += std::to_string(id);
        request += ",\"method\":\"";
        request += JsonEscape(method);
        request += "\"";
        if (!params_json.empty()) {
            request += ",\"params\":";
            request += params_json;
        }
        request += "}";

        if (!SendText(request, log))
                throw std::runtime_error("无法发送 CDP 请求");

        while (true) {
            std::string message;
            if (!ReceiveText(message, log))
                throw std::runtime_error("无法接收 CDP 响应");

            JVal root;
            try {
                root = JsonParser::Parse(message);
            } catch (const std::exception& error) {
                log.warn("无法解析 CDP 消息：" + std::string(error.what()));
                continue;
            }

            const JVal* response_id = root.get("id");
            if (!response_id || response_id->type != JVal::Type::Number)
                continue;
            if (static_cast<int>(response_id->number) != id)
                continue;

            if (const JVal* cdp_error = root.get("error")) {
                const JVal* cdp_message = cdp_error->get("message");
                throw std::runtime_error(
                    "CDP 返回错误：" +
                    (cdp_message ? cdp_message->as_string() : "未知错误"));
            }

            if (const JVal* result = root.get("result"))
                return *result;

            return JVal{};
        }
    }

    void Close() {
        if (socket_) {
            WinHttpWebSocketClose(
                socket_, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
            WinHttpCloseHandle(socket_);
            socket_ = nullptr;
        }
        if (connection_) {
            WinHttpCloseHandle(connection_);
            connection_ = nullptr;
        }
        if (session_) {
            WinHttpCloseHandle(session_);
            session_ = nullptr;
        }
    }

private:
    HINTERNET session_ = nullptr;
    HINTERNET connection_ = nullptr;
    HINTERNET socket_ = nullptr;
    int next_id_ = 1;
};

// ---------------------------------------------------------------------------
// ASAR discovery
// ---------------------------------------------------------------------------

struct AsarEntry {
    std::string path;
    int64_t size = 0;
    int64_t offset = 0;
};

bool EndsWithJs(const std::string& path) {
    return EndsWithNoCase(path, ".js");
}

bool IsPriorityFile(const std::string& path) {
    const std::string lower = ToLower(path);
    static const char* kNeedles[] = {
        "initialize-app-host-services",
        "general-settings",
        "settings",
        "app-initial",
        "i18n",
        "app-shared"
    };
    for (const char* needle : kNeedles) {
        if (lower.find(needle) != std::string::npos)
            return true;
    }
    return false;
}

void CollectAsarEntries(
    const JVal& node,
    const std::string& prefix,
    std::vector<AsarEntry>& entries)
{
    const JVal* files = node.get("files");
    if (!files || files->type != JVal::Type::Object)
        return;

    for (const auto& [name, value] : files->object) {
        const std::string path = prefix.empty() ? name : prefix + "/" + name;

        if (value.get("files")) {
            CollectAsarEntries(value, path, entries);
            continue;
        }

        const JVal* size = value.get("size");
        const JVal* offset = value.get("offset");
        if (!size || !offset)
            continue;

        AsarEntry entry;
        entry.path = path;
        entry.size = size->as_int64();
        entry.offset = offset->as_int64();
        if (entry.size >= 0 && entry.offset >= 0)
            entries.push_back(std::move(entry));
    }
}

bool ReadAsarHeader(
    const std::wstring& asar_path,
    std::vector<AsarEntry>& entries,
    int64_t& data_start,
    Logger& log)
{
    std::ifstream file(std::filesystem::path(asar_path), std::ios::binary);
    if (!file) {
        log.warn("无法打开 ASAR 文件：" + W2U(asar_path));
        return false;
    }

    unsigned char header[16]{};
    file.read(reinterpret_cast<char*>(header), sizeof(header));
    if (file.gcount() != sizeof(header)) {
        log.warn("ASAR 头部不完整。");
        return false;
    }

    const uint32_t header_size =
        static_cast<uint32_t>(header[4]) |
        (static_cast<uint32_t>(header[5]) << 8) |
        (static_cast<uint32_t>(header[6]) << 16) |
        (static_cast<uint32_t>(header[7]) << 24);

    const uint32_t json_size =
        static_cast<uint32_t>(header[12]) |
        (static_cast<uint32_t>(header[13]) << 8) |
        (static_cast<uint32_t>(header[14]) << 16) |
        (static_cast<uint32_t>(header[15]) << 24);

    if (json_size == 0 || json_size > 64 * 1024 * 1024) {
        log.warn("ASAR 头部大小异常：" + std::to_string(json_size));
        return false;
    }

    std::string json(json_size, '\0');
    file.read(json.data(), static_cast<std::streamsize>(json.size()));
    if (static_cast<uint32_t>(file.gcount()) != json_size) {
        log.warn("ASAR JSON 头部不完整。");
        return false;
    }

    data_start = 8 + static_cast<int64_t>(header_size);

    try {
        const JVal root = JsonParser::Parse(json);
        CollectAsarEntries(root, std::string(), entries);
    } catch (const std::exception& error) {
        log.warn("无法解析 ASAR JSON：" + std::string(error.what()));
        return false;
    }

    return true;
}

std::string ReadAsarEntry(
    std::ifstream& file,
    int64_t data_start,
    const AsarEntry& entry)
{
    if (entry.size <= 0 || entry.size > 64 * 1024 * 1024)
        return {};

    file.clear();
    file.seekg(data_start + entry.offset, std::ios::beg);
    if (!file)
        return {};

    std::string data(static_cast<size_t>(entry.size), '\0');
    file.read(data.data(), static_cast<std::streamsize>(data.size()));
    data.resize(static_cast<size_t>(file.gcount()));
    return data;
}

void ExtractLayerIds(const std::string& data, size_t marker, std::set<std::string>& ids) {
    const size_t start = marker > 350 ? marker - 350 : 0;
    const size_t end = std::min(data.size(), marker + 900);

    for (size_t i = start; i < end;) {
        if (data[i] >= '0' && data[i] <= '9') {
            size_t j = i;
            while (j < end && data[j] >= '0' && data[j] <= '9')
                ++j;
            const size_t length = j - i;
            if (length >= 5 && length <= 12)
                ids.insert(data.substr(i, length));
            i = j;
        } else {
            ++i;
        }
    }
}

std::vector<std::string> DiscoverLayerIds(
    const std::wstring& asar_path,
    Logger& log)
{
    std::vector<AsarEntry> entries;
    int64_t data_start = 0;
    if (!ReadAsarHeader(asar_path, entries, data_start, log))
        return {};

    log.info("ASAR 条目数：" + std::to_string(entries.size()));

    std::vector<AsarEntry> priority;
    std::vector<AsarEntry> rest;
    for (const AsarEntry& entry : entries) {
        if (!EndsWithJs(entry.path) || entry.size <= 0 || entry.size > 4 * 1024 * 1024)
            continue;
        if (IsPriorityFile(entry.path))
            priority.push_back(entry);
        else
            rest.push_back(entry);
    }

    log.info("可扫描的 JS 文件数：" + std::to_string(priority.size() + rest.size()) +
             "（优先扫描 " + std::to_string(priority.size()) + " 个）");

    std::ifstream file(std::filesystem::path(asar_path), std::ios::binary);
    if (!file) {
        log.warn("无法重新打开 ASAR 文件进行扫描。");
        return {};
    }

    std::set<std::string> ids;
    int scanned = 0;
    int hits = 0;

    auto scan = [&](const std::vector<AsarEntry>& list) {
        for (const AsarEntry& entry : list) {
            ++scanned;
            const std::string data = ReadAsarEntry(file, data_start, entry);
            if (data.empty())
                continue;

            size_t marker = data.find("enable_i18n");
            if (marker == std::string::npos)
                continue;

            ++hits;
            size_t count_before = ids.size();
            ExtractLayerIds(data, marker, ids);
            log.info("在 " + entry.path + " 中发现 i18n 标记"
                     "（候选 ID：" + std::to_string(ids.size() - count_before) + "）");
        }
    };

    scan(priority);
    if (ids.empty())
        scan(rest);

    log.info("ASAR 扫描完成：文件数=" + std::to_string(scanned) +
             "，命中数=" + std::to_string(hits));

    std::vector<std::string> result(ids.begin(), ids.end());
    return result;
}

// ---------------------------------------------------------------------------
// process / port probing
// ---------------------------------------------------------------------------

bool IsTargetProcess(DWORD process_id, const std::wstring& expected_exe_path) {
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,
                                       FALSE, process_id);
    if (!process)
        return false;

    std::vector<wchar_t> path(32768, L'\0');
    DWORD length = static_cast<DWORD>(path.size());
    const BOOL queried = QueryFullProcessImageNameW(
        process, 0, path.data(), &length);
    CloseHandle(process);

    return queried && _wcsicmp(
        std::wstring(path.data(), length).c_str(), expected_exe_path.c_str()) == 0;
}

std::vector<DWORD> FindChatGptProcesses(const std::wstring& expected_exe_path) {
    std::vector<DWORD> result;

    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return result;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);

    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, L"ChatGPT.exe") == 0 &&
                IsTargetProcess(entry.th32ProcessID, expected_exe_path)) {
                result.push_back(entry.th32ProcessID);
            }
        } while (Process32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return result;
}

// ---------------------------------------------------------------------------
// CDP target / injection
// ---------------------------------------------------------------------------

struct CdpTarget {
    std::string id;
    std::string type;
    std::string url;
    std::string websocket_url;
};

std::vector<CdpTarget> FindAppTargets(
    int port,
    int timeout_seconds,
    Logger& log)
{
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(timeout_seconds);

    while (std::chrono::steady_clock::now() < deadline) {
        std::string body;
        if (HttpGet(port, L"/json/list", body, log)) {
            try {
                const JVal root = JsonParser::Parse(body);
                if (root.type == JVal::Type::Array) {
                    std::vector<CdpTarget> targets;
                    for (const JVal& item : root.array) {
                        const JVal* type = item.get("type");
                        const JVal* url = item.get("url");
                        const JVal* ws = item.get("webSocketDebuggerUrl");
                        const JVal* id = item.get("id");
                        if (!type || !url || !ws)
                            continue;

                        const std::string type_text = type->as_string();
                        const std::string url_text = url->as_string();
                        const std::string ws_text = ws->as_string();

                        if (type_text != "page" ||
                            !StartsWith(url_text, "app://-/")) {
                            continue;
                        }

                        CdpTarget target;
                        target.id = id ? id->as_string() : std::string();
                        target.type = type_text;
                        target.url = url_text;
                        target.websocket_url = ws_text;
                        targets.push_back(std::move(target));
                    }

                    if (!targets.empty())
                        return targets;
                }
            } catch (const std::exception& error) {
                log.warn("无法解析 /json/list：" + std::string(error.what()));
            }
        }

        Sleep(250);
    }

    return {};
}

std::string BuildInjectionScript(const std::vector<std::string>& layer_ids) {
    std::string ids = "[";
    for (size_t i = 0; i < layer_ids.size(); ++i) {
        if (i)
            ids += ",";
        ids += "\"";
        ids += JsonEscape(layer_ids[i]);
        ids += "\"";
    }
    ids += "]";

    static const char* kTemplate = R"JS((() => {
  "use strict";
  const KEY = "__codexSilentLoaderV1__";
  if (globalThis[KEY]) {
    try { globalThis[KEY].tick(); } catch (_) {}
    return;
  }
  const known = new Set(__IDS__);
  const layerCache = new WeakMap();
  const originals = new Map();
  const sentinel = Symbol.for("react.memo_cache_sentinel");
  let patchedClients = 0;
  let cacheRowsCleared = 0;
  let queryClientRef = null;
  let localeForced = false;
  let localeFallbackSet = false;
  let lastError = "";
  let tickTimer = 0;
  let fastTickCount = 0;

  function wrapLayer(layer) {
    if (!layer || typeof layer.get !== "function") return layer;
    const cached = layerCache.get(layer);
    if (cached) return cached;

    const functions = new Map();
    const value = Object.assign({}, layer.value || {}, { enable_i18n: true });

    const proxy = new Proxy(layer, {
      get(target, prop, receiver) {
        if (prop === "get") {
          let fn = functions.get("get");
          if (!fn) {
            fn = (key, fallback) =>
              key === "enable_i18n" ? true : target.get(key, fallback);
            functions.set("get", fn);
          }
          return fn;
        }
        if (prop === "value") return value;

        const raw = Reflect.get(target, prop, receiver);
        if (typeof raw !== "function") return raw;

        let fn = functions.get(prop);
        if (!fn) {
          fn = raw.bind(target);
          functions.set(prop, fn);
        }
        return fn;
      }
    });

    layerCache.set(layer, proxy);
    return proxy;
  }

  function patchClient(client) {
    if (!client ||
        typeof client.getLayer !== "function" ||
        originals.has(client)) {
      return false;
    }

    const original = client.getLayer.bind(client);
    originals.set(client, original);
    client.getLayer = function (name, ...args) {
      return wrapLayer(original(name, ...args));
    };
    patchedClients++;
    return true;
  }

  function fibers(visit) {
    const stack = [globalThis.__codexRoot?._internalRoot?.current];
    const seen = new Set();
    while (stack.length && seen.size < 100000) {
      const node = stack.pop();
      if (!node || seen.has(node)) continue;
      seen.add(node);
      visit(node);
      if (node.child) stack.push(node.child);
      if (node.sibling) stack.push(node.sibling);
    }
  }

  function clearLanguageCache(force = false) {
    let count = 0;
    const seen = new Set();
    fibers(node => {
      for (const fiber of [node, node.alternate]) {
        const rows = fiber?.updateQueue?.memoCache?.data;
        if (!rows || typeof rows[Symbol.iterator] !== "function") continue;
        for (const row of rows) {
          if (!Array.isArray(row) || seen.has(row)) continue;
          seen.add(row);
          let stale = false;
          try {
            stale = row.some(value => {
              if (!value || typeof value !== "object" ||
                  typeof value.get !== "function" ||
                  typeof value.name !== "string") {
                return false;
              }
              if (known.size > 0 && !known.has(value.name)) {
                return false;
              }
              return force || value.get("enable_i18n", false) !== true;
            });
          } catch (_) {
            stale = false;
          }
          if (stale) {
            row.fill(sentinel);
            count++;
          }
        }
      }
    });
    cacheRowsCleared += count;
    return count;
  }

  function emit(client) {
    if (client && typeof client.$emt === "function") {
      client.$emt({ name: "values_updated", status: client.loadingStatus });
    }
  }


  function findQueryClient() {
    if (queryClientRef) return queryClientRef;
    const root = globalThis.__codexRoot?._internalRoot?.current;
    if (!root) return null;
    const stack = [root];
    const seen = new Set();
    while (stack.length && seen.size < 100000) {
      const node = stack.pop();
      if (!node || seen.has(node)) continue;
      seen.add(node);

      const props = node.memoizedProps;
      if (props && typeof props === "object") {
        for (const key of Object.keys(props)) {
          let value;
          try { value = props[key]; } catch (_) { continue; }
          if (value && typeof value === "object" &&
              typeof value.setQueryData === "function" &&
              typeof value.getQueryCache === "function") {
            queryClientRef = value;
            return value;
          }
        }
      }

      let hook = node.memoizedState;
      let hookIndex = 0;
      while (hook && hookIndex < 60) {
        const value = hook.memoizedState;
        if (value && typeof value === "object") {
          for (const candidate of [value, value.current]) {
            if (!candidate || typeof candidate !== "object") continue;
            if (typeof candidate.setQueryData === "function" &&
                typeof candidate.getQueryCache === "function") {
              queryClientRef = candidate;
              return candidate;
            }
            const candidateClient = candidate.queryClient;
            if (candidateClient &&
                typeof candidateClient.setQueryData === "function" &&
                typeof candidateClient.getQueryCache === "function") {
              queryClientRef = candidateClient;
              return candidateClient;
            }
          }
        }
        hook = hook.next;
        hookIndex++;
      }

      try {
        const context = node.type && node.type._context;
        const value = context && context._currentValue;
        if (value && typeof value === "object" &&
            typeof value.setQueryData === "function" &&
            typeof value.getQueryCache === "function") {
          queryClientRef = value;
          return value;
        }
      } catch (_) {
      }

      if (node.child) stack.push(node.child);
      if (node.sibling) stack.push(node.sibling);
    }
    return null;
  }

  function forceLocale() {
    const client = findQueryClient();
    if (!client) return false;

    let changed = false;
    let localeQueryFound = false;

    for (const query of client.getQueryCache().getAll()) {
      let keyText = "";
      try { keyText = JSON.stringify(query.queryKey); } catch (_) {}
      const data = query.state && query.state.data;
      const dataIsLocale = data && typeof data === "object" &&
        ("ideLocale" in data || "systemLocale" in data);
      const keyIsLocale = /locale-info|localeinfo|locale_info/i.test(keyText);
      if (!dataIsLocale && !keyIsLocale) continue;

      localeQueryFound = true;
      const updates = {};
      if (!dataIsLocale) {
        updates.ideLocale = "zh-CN";
        updates.systemLocale = "zh-CN";
      } else {
        if ("ideLocale" in data && data.ideLocale !== "zh-CN")
          updates.ideLocale = "zh-CN";
        if ("systemLocale" in data && data.systemLocale !== "zh-CN")
          updates.systemLocale = "zh-CN";
      }

      const override = data && typeof data === "object" ? data.localeOverride : undefined;
      if (typeof override === "string" && override !== "zh-CN") {
        updates.localeOverride = "zh-CN";
      } else if (override && typeof override === "object") {
        const next = { ...override };
        let changedOverride = false;
        if ("effective" in next && next.effective !== "zh-CN") {
          next.effective = "zh-CN";
          changedOverride = true;
        }
        if ("value" in next && next.value !== "zh-CN") {
          next.value = "zh-CN";
          changedOverride = true;
        }
        if (changedOverride)
          updates.localeOverride = next;
      }

      if (data && typeof data === "object") {
        for (const field of ["values", "configuredValues"]) {
          const values = data[field];
          if (values && typeof values === "object" &&
              "localeOverride" in values &&
              values.localeOverride !== "zh-CN") {
            updates[field] = { ...values, localeOverride: "zh-CN" };
          }
        }
      }

      if (Object.keys(updates).length === 0) continue;

      client.setQueryData(query.queryKey, old =>
        old && typeof old === "object" ? { ...old, ...updates } : updates);
      changed = true;
    }

    if (!localeQueryFound && !localeFallbackSet) {
      client.setQueryData(["vscode", "locale-info"], {
        ideLocale: "zh-CN",
        systemLocale: "zh-CN"
      });
      localeFallbackSet = true;
      changed = true;
    }

    if (changed) localeForced = true;
    return changed;
  }

  function tick() {
    try {
      const statsig = globalThis.__STATSIG__;
      const clients = new Set([
        statsig && statsig.firstInstance,
        ...Object.values((statsig && statsig.instances) || {})
      ]);
      let installed = false;
      for (const client of clients) {
        installed = patchClient(client) || installed;
      }

      let changed = 0;
      try {
        changed = clearLanguageCache();
      } catch (error) {
        lastError = "cache: " + String(error);
      }

      let localeChanged = false;
      try {
        localeChanged = forceLocale();
      } catch (error) {
        lastError = "locale: " + String(error);
      }

      if (installed || changed || localeChanged) {
        for (const client of originals.keys()) emit(client);
      }
    } catch (error) {
      lastError = String(error);
    }
  }

  try {
    Object.defineProperty(navigator, "language", {
      get: () => "zh-CN",
      configurable: true
    });
    Object.defineProperty(navigator, "languages", {
      get: () => ["zh-CN", "zh"],
      configurable: true
    });
  } catch (_) {
  }

  globalThis[KEY] = {
    tick,
    status() {
      const statsig = globalThis.__STATSIG__;
      const clients = [...new Set([
        statsig && statsig.firstInstance,
        ...Object.values((statsig && statsig.instances) || {})
      ])].filter(Boolean);
      return {
        patchedClients,
        totalClients: clients.length,
        knownLayers: [...known],
        cacheRowsCleared,
        localeForced,
        queryClient: !!queryClientRef,
        error: lastError,
        language: (document.documentElement && document.documentElement.lang) || "",
        href: location.href
      };
    },
    stop() {
      clearTimeout(tickTimer);
      for (const [client, original] of originals) {
        client.getLayer = original;
      }
      originals.clear();
      patchedClients = 0;
      clearLanguageCache(true);
      delete globalThis[KEY];
      return { stopped: true };
    }
  };

  function scheduleTick() {
    tick();
    const isStarting = patchedClients === 0 && fastTickCount++ < 80;
    tickTimer = setTimeout(scheduleTick, isStarting ? 250 : 5000);
  }

  scheduleTick();
})();)JS";

    std::string script = kTemplate;
    const std::string placeholder = "__IDS__";
    const size_t position = script.find(placeholder);
    if (position != std::string::npos)
        script.replace(position, placeholder.size(), ids);
    return script;
}

bool IsChineseStatus(const JVal& status) {
    const JVal* language = status.get("language");
    if (!language || language->type != JVal::Type::String)
        return false;
    return StartsWith(ToLower(language->string), "zh");
}

bool InjectOne(
    int port,
    const CdpTarget& target,
    const std::vector<std::string>& layer_ids,
    Logger& log)
{
    log.info("正在注入页面：" + target.url);

    WebSocketClient cdp;
    if (!cdp.Connect(port, target.websocket_url, log)) {
        log.error("无法连接到页面：" + target.websocket_url);
        return false;
    }

    try {
        cdp.Call("Page.enable", std::string(), log);

        const std::string script = BuildInjectionScript(layer_ids);
        const std::string add_script =
            "{\"source\":\"" + JsonEscape(script) + "\"}";
        cdp.Call("Page.addScriptToEvaluateOnNewDocument", add_script, log);

        const std::string evaluate_script =
            "{\"expression\":\"" + JsonEscape(script) +
            "\",\"returnByValue\":true,\"awaitPromise\":true}";
        cdp.Call("Runtime.evaluate", evaluate_script, log);

        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds(12);
        auto patched_at = std::chrono::steady_clock::time_point{};
        JVal last_status;
        bool have_status = false;

        const std::string evaluate_params =
            "{\"expression\":\"globalThis.__codexSilentLoaderV1__?.status()\","
            "\"returnByValue\":true,\"awaitPromise\":true}";

        while (std::chrono::steady_clock::now() < deadline) {
            try {
                const JVal result = cdp.Call("Runtime.evaluate", evaluate_params, log);
                const JVal* inner = result.get("result");
                const JVal* value = inner ? inner->get("value") : nullptr;

                if (value && value->type == JVal::Type::Object) {
                    const JVal* patched = value->get("patchedClients");
                    if (patched && patched->as_int(0) > 0) {
                        last_status = *value;
                        have_status = true;
                        if (patched_at.time_since_epoch().count() == 0)
                            patched_at = std::chrono::steady_clock::now();

                        const bool chinese = IsChineseStatus(*value);
                        const auto waited =
                            std::chrono::steady_clock::now() - patched_at;

                        if (chinese || waited > std::chrono::seconds(2)) {
                            std::string raw;
                            const JVal* language = value->get("language");
                            const JVal* count = value->get("patchedClients");
                            const JVal* forced = value->get("localeForced");
                            const JVal* query_client = value->get("queryClient");
                            const JVal* last_error = value->get("error");
                            const bool forced_value =
                                forced && forced->type == JVal::Type::Bool &&
                                forced->boolean;
                            const bool query_client_value =
                                query_client && query_client->type == JVal::Type::Bool &&
                                query_client->boolean;
                            raw = "{\"patchedClients\":" +
                                  std::to_string(count ? count->as_int(0) : 0) +
                                  ",\"localeForced\":" +
                                  (forced_value ? "true" : "false") +
                                  ",\"queryClient\":" +
                                  (query_client_value ? "true" : "false") +
                                  ",\"language\":\"" +
                                  (language ? language->as_string() : "") +
                                  "\",\"error\":\"" +
                                  (last_error ? JsonEscape(last_error->as_string()) : "") +
                                  "\"}";
                            log.info("注入状态：" + raw);
                            if (!chinese)
                                log.info("i18n 开关已开启，语言仍在初始化。");
                            cdp.Close();
                            return true;
                        }
                    }
                }
            } catch (const std::exception& error) {
                log.warn("状态检查失败：" + std::string(error.what()));
            }

            Sleep(300);
        }

        cdp.Close();

        if (have_status) {
            const JVal* language = last_status.get("language");
            log.warn("注入状态（语言尚未确认）：language=" +
                     (language ? language->as_string() : ""));
            return true;
        }

        log.error("补丁已安装，但 12 秒内未发现可处理的 Statsig 客户端。");
        return false;
    } catch (const std::exception& error) {
        log.error("注入失败：" + std::string(error.what()));
        cdp.Close();
        return false;
    }
}

int WaitAndInject(
    int port,
    const std::vector<std::string>& layer_ids,
    Logger& log)
{
    const std::vector<CdpTarget> initial =
        FindAppTargets(port, kTimeoutSeconds, log);
    if (initial.empty()) {
        log.error("等待 app://-/ DevTools 页面超时。");
        return 3;
    }

    {
        std::string joined;
        for (const CdpTarget& target : initial) {
            if (!joined.empty())
                joined += ", ";
            joined += target.url;
        }
        log.info("CDP 页面：" + joined);
    }

    std::set<std::string> injected;
    bool success = false;

    for (const CdpTarget& target : initial) {
        injected.insert(target.websocket_url);
        success = InjectOne(port, target, layer_ids, log) || success;
    }

    const auto grace_end = std::chrono::steady_clock::now() +
                           std::chrono::seconds(kGraceSeconds);
    while (std::chrono::steady_clock::now() < grace_end) {
        Sleep(500);

        const std::vector<CdpTarget> extras = FindAppTargets(port, 1, log);
        for (const CdpTarget& extra : extras) {
            if (injected.find(extra.websocket_url) != injected.end())
                continue;

            log.info("检测到新的应用页面：" + extra.url);
            injected.insert(extra.websocket_url);
            success = InjectOne(port, extra, layer_ids, log) || success;
        }
    }

    if (!success) {
        log.error("注入未报告已处理的 Statsig 客户端。");
        return 4;
    }

    log.info("加载器已完成，正在退出。");
    return 0;
}

// ---------------------------------------------------------------------------
// config probe
// ---------------------------------------------------------------------------

void CheckLocaleConfig(Logger& log) {
    wchar_t profile[MAX_PATH]{};
    const DWORD length = GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        log.warn("无法读取 USERPROFILE 环境变量。");
        return;
    }

    const std::filesystem::path config =
        std::filesystem::path(profile) / L".codex" / L"config.toml";
    std::ifstream file(config);
    if (!file) {
        log.warn("未找到配置文件：" + W2U(config.wstring()));
        return;
    }

    std::string line;
    while (std::getline(file, line)) {
        const size_t key = line.find("localeOverride");
        if (key == std::string::npos)
            continue;

        const size_t equals = line.find('=', key);
        if (equals == std::string::npos)
            continue;

        size_t begin = line.find('"', equals);
        size_t end = begin == std::string::npos ? std::string::npos
                                                : line.find('"', begin + 1);
        if (begin == std::string::npos || end == std::string::npos) {
            begin = line.find('\'', equals);
            end = begin == std::string::npos ? std::string::npos
                                             : line.find('\'', begin + 1);
        }

        if (begin != std::string::npos && end != std::string::npos) {
            const std::string locale = line.substr(begin + 1, end - begin - 1);
            log.info("localeOverride = " + locale);
            if (!StartsWith(ToLower(locale), "zh"))
                log.warn("localeOverride 不是中文，界面可能保持其他语言。");
            return;
        }
    }

    log.warn("~/.codex/config.toml 未设置 localeOverride。");
}

// ---------------------------------------------------------------------------
// entry
// ---------------------------------------------------------------------------

int Run(int argc, wchar_t** argv) {
    Options options;
    std::wstring option_error;
    if (!ParseOptions(argc, argv, options, option_error)) {
        ShowError(option_error + L"\n\n" + kUsage);
        return 7;
    }

    Logger log(options.log, options.log);
    log.info("CodexSilentLoader 原生版 0.1.0");
    if (log.enabled())
        log.info("日志文件：" + W2U(log.file_path().wstring()));

    try {
        CheckLocaleConfig(log);

        const auto package = FindLatestPackage(log);
        if (!package) {
            log.error("当前用户未安装 OpenAI.Codex。");
            ShowError(L"未找到已安装的 OpenAI Codex。\n\n"
                      L"请确认 ChatGPT/Codex 已安装，或稍后重试。");
            return 1;
        }

        log.info("安装包：" + W2U(package->full_name));
        log.info("版本：" + W2U(package->version));
        log.info("安装路径：" + W2U(package->install_path));
        log.info("AUMID：" + W2U(package->aumid));

        const std::wstring asar_path =
            package->install_path + L"\\app\\resources\\app.asar";
        log.info("ASAR：" + W2U(asar_path));

        const std::vector<std::string> layer_ids =
            DiscoverLayerIds(asar_path, log);
        if (layer_ids.empty()) {
            log.warn("未发现 i18n layer ID；将处理所有 Statsig layer。");
        } else {
            std::string joined;
            for (const std::string& id : layer_ids) {
                if (!joined.empty())
                    joined += ", ";
                joined += id;
            }
            log.info("发现的 i18n layer ID：" + joined);
        }

        if (!FindChatGptProcesses(package->exe_path).empty()) {
            log.warn("目标应用已在运行，请先退出。");
            ShowInfo(L"检测到 ChatGPT/Codex 正在运行。\n\n"
                     L"请彻底退出（包括托盘和后台进程）后再运行本程序。");
            return 5;
        }

        const int port = GetFreeLoopbackPort();
        if (port <= 0) {
            log.error("无法分配本地回环端口。");
            ShowError(L"无法分配本地端口。");
            return 2;
        }
        log.info("DevTools 端口：" + std::to_string(port));

        const HRESULT result = LaunchPackagedApp(package->aumid, port, log);
        if (FAILED(result)) {
            std::wstring message =
                L"启动 ChatGPT/Codex 失败。\n\n"
                L"请用 -log 运行并查看 CodexSilentLoader.log。";
            if (log.enabled())
                message += L"\n\n日志：" + log.file_path().wstring();
            ShowError(message);
            return 2;
        }

        const int inject_result = WaitAndInject(port, layer_ids, log);
        if (inject_result != 0) {
            std::wstring message =
                L"中文界面注入失败。\n\n"
                L"请用 -log 运行并查看 CodexSilentLoader.log。";
            if (log.enabled())
                message += L"\n\n日志：" + log.file_path().wstring();
            ShowError(message);
            return inject_result;
        }

        return 0;
    } catch (const std::exception& error) {
        log.error(std::string("发生致命错误：") + error.what());
        std::wstring message =
            L"加载器发生异常。\n\n"
            L"请用 -log 运行并查看 CodexSilentLoader.log。";
        if (log.enabled())
            message += L"\n\n日志：" + log.file_path().wstring();
        ShowError(message);
        return 99;
    }
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv)
        return 7;

    const int result = Run(argc, argv);
    LocalFree(argv);
    return result;
}
