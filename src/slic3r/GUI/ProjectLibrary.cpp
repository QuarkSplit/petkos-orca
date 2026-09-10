#include "ProjectLibrary.hpp"

#include "libslic3r/Config.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/miniz_extension.hpp"
#include <algorithm>
#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/nowide/convert.hpp>
#include <boost/property_tree/xml_parser.hpp>
#include <chrono>
#include <cmath>
#include <ctime>
#include <cwchar>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iterator>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <sstream>
#include <thread>
#include <png.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#ifdef __linux__
#include <sched.h>
#endif
#endif

namespace Slic3r::GUI::ProjectLibrary {
namespace fs = boost::filesystem;
using json = nlohmann::json;
namespace pt = boost::property_tree;

std::string path_key(const std::string &path)
{
    boost::system::error_code ec;
    auto resolved = fs::weakly_canonical(fs::path(path), ec);
    std::string key = (ec ? fs::path(path).lexically_normal() : resolved).generic_string();
#ifdef _WIN32
    boost::algorithm::to_lower(key);
#endif
    return key;
}

namespace {
constexpr int renderer_version = 1;
constexpr size_t max_image_bytes = 8 * 1024 * 1024;
constexpr auto render_deadline = std::chrono::seconds(45);

struct SourceStamp {
    uint64_t size = 0;
    std::time_t mtime = 0;
    std::string revision;
};

bool source_stamp(const fs::path &file, SourceStamp &stamp)
{
    boost::system::error_code ec;
    if (!fs::is_regular_file(file, ec) || ec) return false;
    stamp.size = fs::file_size(file, ec);
    if (ec) return false;
    stamp.mtime = fs::last_write_time(file, ec);
    if (ec) return false;
    std::error_code error;
    const auto time = std::filesystem::last_write_time(std::filesystem::u8path(file.string()), error);
    if (error) return false;
    stamp.revision = std::to_string(int64_t(time.time_since_epoch().count()));
    return true;
}

bool unchanged(const fs::path &file, const SourceStamp &stamp)
{
    SourceStamp current;
    return source_stamp(file, current) && current.size == stamp.size && current.revision == stamp.revision;
}

fs::path cache_file(const std::string &directory, const fs::path &file,
                    const SourceStamp &stamp, const std::string &view)
{
    // Defined hashing keeps cache names stable between runs and compiler versions.
    const auto identity = path_key(file.string()) + '\n' + stamp.revision + '\n' +
        std::to_string(stamp.size) + '\n' + std::to_string(renderer_version) + '\n' + view;
    uint64_t hash = UINT64_C(14695981039346656037);
    for (unsigned char c : identity) { hash ^= c; hash *= UINT64_C(1099511628211); }
    std::ostringstream name;
    name << "library-" << renderer_version << '-' << std::hex << std::setw(16) << std::setfill('0') << hash << ".png";
    return fs::path(directory) / name.str();
}

bool valid_png(const std::string &bytes, bool generated = false)
{
    if (bytes.empty() || bytes.size() > max_image_bytes) return false;
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    const auto release = [&image](png_image *) { png_image_free(&image); };
    std::unique_ptr<png_image, decltype(release)> guard(&image, release);
    if (!png_image_begin_read_from_memory(&image, bytes.data(), bytes.size())) return false;
    if (image.width == 0 || image.height == 0 || image.width > 4096 || image.height > 4096 ||
        (generated && (image.width != 512 || image.height != 512))) return false;
    image.format = PNG_FORMAT_RGBA;
    std::vector<png_byte> pixels(PNG_IMAGE_SIZE(image));
    if (!png_image_finish_read(&image, nullptr, pixels.data(), 0, nullptr)) return false;
    for (size_t i = 3; i < pixels.size(); i += 4)
        if (pixels[i] != 0) return true;
    return false;
}

std::string read_file(const fs::path &file, size_t limit = max_image_bytes)
{
    boost::system::error_code ec;
    const auto size = fs::file_size(file, ec);
    if (ec || size == 0 || size > limit) return {};
    std::string bytes(size_t(size), '\0');
    boost::nowide::ifstream in(file.string(), std::ios::binary);
    in.read(bytes.data(), std::streamsize(bytes.size()));
    return in ? bytes : std::string();
}

bool cached_png(const fs::path &file, bool generated = false)
{
    boost::system::error_code ec;
    const auto size = fs::file_size(file, ec);
    if (ec || size < 45 || size > max_image_bytes) return false;
    unsigned char header[24]{}, trailer[12]{};
    boost::nowide::ifstream in(file.string(), std::ios::binary);
    in.read(reinterpret_cast<char *>(header), sizeof(header));
    in.seekg(-std::streamoff(sizeof(trailer)), std::ios::end);
    in.read(reinterpret_cast<char *>(trailer), sizeof(trailer));
    const unsigned char signature[] = {137, 80, 78, 71, 13, 10, 26, 10};
    const unsigned char end[] = {0, 0, 0, 0, 73, 69, 78, 68, 174, 66, 96, 130};
    const auto dimension = [](const unsigned char *p) {
        return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
    };
    const uint32_t width = dimension(header + 16), height = dimension(header + 20);
    return in && std::equal(std::begin(signature), std::end(signature), header) &&
        std::equal(std::begin(end), std::end(end), trailer) &&
        header[8] == 0 && header[9] == 0 && header[10] == 0 && header[11] == 13 &&
        header[12] == 'I' && header[13] == 'H' && header[14] == 'D' && header[15] == 'R' &&
        width > 0 && height > 0 && width <= 4096 && height <= 4096 &&
        (!generated || (width == 512 && height == 512));
}

bool publish(const fs::path &staged, const fs::path &target)
{
#ifdef _WIN32
    return ::MoveFileExW(boost::nowide::widen(staged.string()).c_str(),
                         boost::nowide::widen(target.string()).c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    return ::rename(staged.string().c_str(), target.string().c_str()) == 0;
#endif
}

struct Scratch {
    fs::path path;
    explicit Scratch(const fs::path &directory) : path(directory / fs::unique_path("render-%%%%-%%%%-%%%%")) {
        fs::create_directories(path);
    }
    ~Scratch() { boost::system::error_code ec; fs::remove_all(path, ec); }
};

#ifdef _WIN32
struct Handle {
    HANDLE value = nullptr;
    explicit Handle(HANDLE handle = nullptr) : value(handle) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) ::CloseHandle(value); }
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
    explicit operator bool() const { return value && value != INVALID_HANDLE_VALUE; }
};

std::wstring quote_argument(const std::wstring &argument)
{
    std::wstring quoted = L"\"";
    size_t slashes = 0;
    for (wchar_t c : argument) {
        if (c == L'\\') { ++slashes; continue; }
        quoted.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        quoted.push_back(c);
        slashes = 0;
    }
    quoted.append(slashes * 2, L'\\');
    quoted.push_back(L'"');
    return quoted;
}
#else
struct ChildProcess {
    pid_t pid;
    ~ChildProcess() {
        if (pid <= 0) return;
        ::kill(-pid, SIGKILL);
        ::kill(pid, SIGKILL);
        while (::waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {}
    }
};
#endif

std::string run_renderer(const std::string &executable, const fs::path &input,
                         const fs::path &output, const fs::path &scratch,
                         const std::atomic<bool> &cancel)
{
    if (cancel) return "Preview cancelled.";
    const std::vector<std::string> arguments = {
        executable, "--library-thumbnail", fs::absolute(input).string(), fs::absolute(output).string(), fs::absolute(scratch).string()};
    const auto deadline = std::chrono::steady_clock::now() + render_deadline;
    int exit_code = -1;
#ifdef _WIN32
    Handle job(::CreateJobObjectW(nullptr, nullptr));
    if (!job) return "Could not create the preview process limit.";
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE |
        JOB_OBJECT_LIMIT_PROCESS_MEMORY | JOB_OBJECT_LIMIT_JOB_MEMORY |
        JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION | JOB_OBJECT_LIMIT_AFFINITY;
    limits.ProcessMemoryLimit = SIZE_T(1024) * 1024 * 1024;
    limits.JobMemoryLimit = limits.ProcessMemoryLimit;
    DWORD_PTR allowed = 0, system = 0;
    if (!::GetProcessAffinityMask(::GetCurrentProcess(), &allowed, &system))
        return "Could not limit preview processor usage.";
    unsigned processors = 0;
    for (DWORD_PTR bit = 1; bit && processors < 2; bit <<= 1)
        if ((allowed & bit) != 0) { limits.BasicLimitInformation.Affinity |= bit; ++processors; }
    if (!::SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
        return "Could not apply the preview process limit.";

    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    Handle null_io(::CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    const auto stderr_path = boost::nowide::widen((scratch / "renderer.stderr").string());
    Handle error_io(::CreateFileW(stderr_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!null_io || !error_io) return "Could not open preview process output.";
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    startup.StartupInfo.wShowWindow = SW_HIDE;
    startup.StartupInfo.hStdInput = startup.StartupInfo.hStdOutput = null_io.value;
    startup.StartupInfo.hStdError = error_io.value;
    SIZE_T attribute_bytes = 0;
    ::InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_bytes);
    std::vector<unsigned char> attributes(attribute_bytes);
    startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    if (!::InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &attribute_bytes))
        return "Could not isolate preview process handles.";
    const auto release_attributes = [](LPPROC_THREAD_ATTRIBUTE_LIST list) { ::DeleteProcThreadAttributeList(list); };
    std::unique_ptr<_PROC_THREAD_ATTRIBUTE_LIST, decltype(release_attributes)> attribute_guard(startup.lpAttributeList, release_attributes);
    HANDLE inherited[] = {null_io.value, error_io.value};
    if (!::UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                     inherited, sizeof(inherited), nullptr, nullptr))
        return "Could not isolate preview process handles.";
    std::wstring command;
    for (const auto &argument : arguments) {
        if (!command.empty()) command.push_back(L' ');
        command += quote_argument(boost::nowide::widen(argument));
    }
    std::wstring environment;
    wchar_t *inherited_environment = ::GetEnvironmentStringsW();
    if (!inherited_environment) return "Could not prepare the preview environment.";
    for (const wchar_t *cursor = inherited_environment; *cursor != L'\0'; cursor += std::wcslen(cursor) + 1) {
        const std::wstring entry(cursor);
        auto key = entry.substr(0, entry.find(L'='));
        boost::algorithm::to_upper(key);
        if (boost::starts_with(key, L"PETKOS_PERF") || key == L"PETKOS_ACCEPT" || key == L"PETKOS_TEST_ASSIGN") continue;
        environment.append(entry);
        environment.push_back(L'\0');
    }
    ::FreeEnvironmentStringsW(inherited_environment);
    if (environment.empty()) environment.push_back(L'\0');
    environment.push_back(L'\0');
    PROCESS_INFORMATION process{};
    if (!::CreateProcessW(boost::nowide::widen(executable).c_str(), command.data(), nullptr, nullptr, TRUE,
                           CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT |
                               BELOW_NORMAL_PRIORITY_CLASS | CREATE_UNICODE_ENVIRONMENT,
                           environment.data(), boost::nowide::widen(scratch.string()).c_str(), &startup.StartupInfo, &process))
        return "Could not start the preview renderer (Windows " + std::to_string(::GetLastError()) + ").";
    Handle process_handle(process.hProcess), thread_handle(process.hThread);
    if (!::AssignProcessToJobObject(job.value, process.hProcess) || ::ResumeThread(process.hThread) == DWORD(-1)) {
        ::TerminateProcess(process.hProcess, 1);
        ::WaitForSingleObject(process.hProcess, INFINITE);
        return "Could not contain the preview renderer.";
    }
    while (true) {
        const DWORD wait = ::WaitForSingleObject(process.hProcess, 100);
        if (wait == WAIT_OBJECT_0) break;
        if (cancel || std::chrono::steady_clock::now() >= deadline || wait == WAIT_FAILED) {
            ::TerminateJobObject(job.value, 1);
            ::WaitForSingleObject(process.hProcess, INFINITE);
            return cancel ? "Preview cancelled." : wait == WAIT_FAILED ? "Preview process could not be checked." : "Preview took too long.";
        }
    }
    DWORD code = 1;
    if (::GetExitCodeProcess(process.hProcess, &code)) exit_code = int(code);
    // Closing the job also ends any descendants left behind by a failed loader.
#else
    std::vector<char *> argv;
    for (const auto &argument : arguments) argv.push_back(const_cast<char *>(argument.c_str()));
    argv.push_back(nullptr);
    std::vector<std::string> environment;
    for (char **cursor = environ; cursor && *cursor; ++cursor) {
        const std::string entry(*cursor);
        auto key = entry.substr(0, entry.find('='));
        boost::algorithm::to_upper(key);
        if (boost::starts_with(key, "PETKOS_PERF") || key == "PETKOS_ACCEPT" || key == "PETKOS_TEST_ASSIGN") continue;
        environment.push_back(entry);
    }
    std::vector<char *> envp;
    for (const auto &entry : environment) envp.push_back(const_cast<char *>(entry.c_str()));
    envp.push_back(nullptr);
    const auto error_path = (scratch / "renderer.stderr").string();
    const auto working_directory = scratch.string();
#ifdef __linux__
    cpu_set_t allowed, selected;
    CPU_ZERO(&selected);
    if (::sched_getaffinity(0, sizeof(allowed), &allowed) != 0) return "Could not limit preview processor usage.";
    unsigned processors = 0;
    for (int cpu = 0; cpu < CPU_SETSIZE && processors < 2; ++cpu)
        if (CPU_ISSET(cpu, &allowed)) { CPU_SET(cpu, &selected); ++processors; }
#endif
    const pid_t child = ::fork();
    if (child < 0) return "Could not start the preview renderer.";
    if (child == 0) {
        if (::setpgid(0, 0) != 0) ::_exit(125);
        ::setpriority(PRIO_PROCESS, 0, 10);
#ifdef __linux__
        if (::sched_setaffinity(0, sizeof(selected), &selected) != 0) ::_exit(125);
#endif
        const rlimit memory{rlim_t(1024) * 1024 * 1024, rlim_t(1024) * 1024 * 1024};
        const rlimit cpu{45, 46};
        if (::setrlimit(RLIMIT_AS, &memory) != 0 || ::setrlimit(RLIMIT_CPU, &cpu) != 0) ::_exit(125);
        const int null_fd = ::open("/dev/null", O_RDWR);
        const int error_fd = ::open(error_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (null_fd < 0 || error_fd < 0 || ::chdir(working_directory.c_str()) != 0) ::_exit(125);
        ::dup2(null_fd, STDIN_FILENO); ::dup2(null_fd, STDOUT_FILENO); ::dup2(error_fd, STDERR_FILENO);
        if (null_fd > STDERR_FILENO) ::close(null_fd);
        if (error_fd > STDERR_FILENO) ::close(error_fd);
        ::execve(argv[0], argv.data(), envp.data());
        ::_exit(127);
    }
    ChildProcess child_guard{child};
    ::setpgid(child, child);
    std::string failure;
    while (true) {
        siginfo_t info{};
        const int waited = ::waitid(P_PID, id_t(child), &info, WEXITED | WNOHANG | WNOWAIT);
        if (waited == 0 && info.si_pid == child) break;
        if (cancel || std::chrono::steady_clock::now() >= deadline || (waited != 0 && errno != EINTR)) {
            failure = cancel ? "Preview cancelled." : "Preview took too long.";
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    // Keep the child unreaped until the group is killed, so its id cannot be reused.
    ::kill(-child, SIGKILL);
    ::kill(child, SIGKILL);
    int status = 0;
    while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    child_guard.pid = 0;
    if (!failure.empty()) return failure;
    if (WIFEXITED(status)) exit_code = WEXITSTATUS(status);
#endif
    if (exit_code == 0) return {};
    auto detail = read_file(scratch / "renderer.stderr", 4096);
    boost::algorithm::trim(detail);
    if (detail.size() > 240) detail.resize(240);
    return detail.empty() ? "Could not render this file (exit " + std::to_string(exit_code) + ")." : detail;
}

struct Archive {
    mz_zip_archive zip{};
    bool open = false;
    explicit Archive(const std::string &path) : open(open_zip_reader(&zip, path)) {}
    ~Archive() { if (open) close_zip_reader(&zip); }

    std::string read(const char *name, size_t limit = 8 * 1024 * 1024) {
        const int index = mz_zip_reader_locate_file(&zip, name, nullptr, 0);
        if (index < 0) return {};
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&zip, index, &stat) || stat.m_uncomp_size > limit)
            throw std::runtime_error("Project metadata is too large to preview");
        size_t size = 0;
        std::unique_ptr<void, decltype(&mz_free)> data(
            mz_zip_reader_extract_to_heap(&zip, index, &size, 0), &mz_free);
        if (!data) throw std::runtime_error("Could not read project metadata");
        return std::string(static_cast<const char *>(data.get()), size);
    }

    std::string header(const char *name) {
        const int index = mz_zip_reader_locate_file(&zip, name, nullptr, 0);
        if (index < 0) return {};
        std::string prefix;
        mz_zip_reader_extract_to_callback(&zip, index,
            [](void *opaque, mz_uint64, const void *data, size_t size) -> size_t {
                auto &text = *static_cast<std::string *>(opaque);
                const size_t take = std::min(size, size_t(64 * 1024) - text.size());
                text.append(static_cast<const char *>(data), take);
                return take;
            }, &prefix, 0);
        return prefix;
    }
};

std::string string_at(const json &value, const char *key, size_t index = 0)
{
    auto field = value.find(key);
    if (field == value.end()) return {};
    if (field->is_string()) return index == 0 ? field->get<std::string>() : std::string();
    if (field->is_array() && index < field->size() && (*field)[index].is_string())
        return (*field)[index].get<std::string>();
    return {};
}

pt::ptree xml(const std::string &text)
{
    pt::ptree tree;
    if (!text.empty()) {
        std::istringstream stream(text);
        pt::read_xml(stream, tree, pt::xml_parser::no_comments);
    }
    return tree;
}

std::map<std::string, std::string> metadata(const pt::ptree &node)
{
    std::map<std::string, std::string> out;
    for (const auto &entry : node)
        if (entry.first == "metadata")
            out[entry.second.get<std::string>("<xmlattr>.key", "")] =
                entry.second.get<std::string>("<xmlattr>.value", "");
    return out;
}

std::string file_url(const fs::path &path)
{
    const std::string source = path.generic_string();
    std::ostringstream out;
    out << (boost::starts_with(source, "//") ? "file:" : "file:///");
    for (unsigned char c : source) {
        if (std::isalnum(c) || c == '/' || c == ':' || c == '-' || c == '_' || c == '.') out << c;
        else out << '%' << std::uppercase << std::hex << std::setw(2) << std::setfill('0') << int(c);
    }
    return out.str();
}

std::string cache_url(const fs::path &path)
{
    SourceStamp stamp;
    return file_url(path) + (source_stamp(path, stamp) ? "?v=" + stamp.revision : std::string());
}

void pending_thumbnail(json &record)
{
    record["thumb"] = "";
    record["views"] = json::array();
    record["thumbState"] = "pending";
    record["thumbError"] = "";
    record.erase("thumbSource");
}

void ready_thumbnail(json &record, const fs::path &file, const std::string &label, const char *source)
{
    const auto url = cache_url(file);
    record["thumb"] = url;
    record["views"] = json::array({{{"src", url}, {"label", label}, {"cacheFile", file.string()}}});
    record["thumbCacheFile"] = file.string();
    record["thumbState"] = "ready";
    record["thumbError"] = "";
    record["thumbSource"] = source;
}

bool hydrate_thumbnail(json &record, const std::string &directory, const fs::path &file, const SourceStamp &stamp)
{
    if (directory.empty()) return false;
    json views = json::array();
    if (const auto old = record.find("views"); old != record.end() && old->is_array()) {
        for (const auto &view : *old) {
            if (!view.is_object()) continue;
            const fs::path cached(view.value("cacheFile", ""));
            if (!cached.empty() && path_key(cached.parent_path().string()) == path_key(directory) &&
                cached_png(cached, record.value("thumbSource", "") == "geometry")) {
                auto refreshed = view;
                refreshed["src"] = cache_url(cached);
                views.push_back(std::move(refreshed));
            }
        }
    }
    if (!views.empty()) {
        record["views"] = views;
        record["thumb"] = views.front()["src"];
        record["thumbCacheFile"] = views.front()["cacheFile"];
        record["thumbState"] = "ready";
        record["thumbError"] = "";
        return true;
    }
    const fs::path generated = cache_file(directory, file, stamp, "geometry");
    if (cached_png(generated, true)) {
        ready_thumbnail(record, generated, "Model", "geometry");
        return true;
    }
    return false;
}

std::string relationship_target(const std::string &target)
{
    if (target.empty() || target.find(':') != std::string::npos || boost::starts_with(target, "//")) return {};
    std::string decoded;
    const auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    };
    for (size_t i = 0; i < target.size(); ++i) {
        if (target[i] == '%') {
            if (i + 2 >= target.size() || hex(target[i + 1]) < 0 || hex(target[i + 2]) < 0) return {};
            const char c = char(hex(target[i + 1]) * 16 + hex(target[i + 2]));
            if (c == '\0' || c == ':' || c == '\\') return {};
            decoded.push_back(c);
            i += 2;
        } else {
            if (target[i] == '\\' || target[i] == '?' || target[i] == '#') return {};
            decoded.push_back(target[i]);
        }
    }
    while (!decoded.empty() && decoded.front() == '/') decoded.erase(decoded.begin());
    std::vector<std::string> components;
    boost::split(components, decoded, boost::is_any_of("/"));
    std::vector<std::string> normalized;
    for (const auto &component : components) {
        if (component.empty() || component == ".") continue;
        if (component == "..") {
            if (normalized.empty()) return {};
            normalized.pop_back();
        } else normalized.push_back(component);
    }
    return boost::join(normalized, "/");
}

void extract_thumbnails(json &record, Archive &archive, const fs::path &file,
                         const std::string &directory, const SourceStamp &stamp, bool force = false)
{
    if (directory.empty()) return;
    std::vector<std::pair<std::string, std::string>> candidates = {
        {"Auxiliaries/.thumbnails/thumbnail_middle.png", "Model"}};
    try {
        const auto relationships = xml(archive.read("_rels/.rels", 1024 * 1024));
        for (const auto &root : relationships)
            for (const auto &relation : root.second) {
                const auto type = relation.second.get<std::string>("<xmlattr>.Type", "");
                if (!boost::ends_with(type, "/metadata/thumbnail") ||
                    boost::iequals(relation.second.get<std::string>("<xmlattr>.TargetMode", ""), "External")) continue;
                const auto target = relationship_target(relation.second.get<std::string>("<xmlattr>.Target", ""));
                if (!target.empty()) candidates.emplace_back(target, "Model");
            }
    } catch (const std::exception &) { /* A broken optional relationship cannot hide readable geometry. */ }
    for (const char *path : {"Metadata/thumbnail.png", "Metadata/thumbnail_3mf.png", "Metadata/thumbnail_middle.png",
                             "Auxiliaries/.thumbnails/thumbnail_3mf.png", "Metadata/thumbnail_small.png", "thumbnail.png"})
        candidates.emplace_back(path, "Model");
    for (int plate = 1; plate <= std::max(1, record.value("plateCount", 0)) && plate <= 64; ++plate)
        candidates.emplace_back("Metadata/plate_" + std::to_string(plate) + ".png", "Plate " + std::to_string(plate));
    candidates.emplace_back("Metadata/plate_no_light_1.png", "Plate 1");
    candidates.emplace_back("Metadata/plate_1_small.png", "Plate 1");
    json views = json::array();
    std::set<std::string> labels;
    for (const auto &[candidate, label] : candidates) {
        if (labels.count(label)) continue;
        try {
            const fs::path target = cache_file(directory, file, stamp, candidate);
            if (force || !cached_png(target)) {
                const auto bytes = archive.read(candidate.c_str(), max_image_bytes);
                if (!valid_png(bytes)) continue;
                fs::create_directories(fs::path(directory));
                Scratch staging{fs::path(directory)};
                const fs::path staged = staging.path / "embedded.png";
                {
                    boost::nowide::ofstream out(staged.string(), std::ios::binary);
                    out.write(bytes.data(), std::streamsize(bytes.size()));
                    out.close();
                    if (!out) continue;
                }
                if (!unchanged(file, stamp) || !publish(staged, target)) continue;
            }
            views.push_back({{"src", cache_url(target)}, {"label", label}, {"cacheFile", target.string()}});
            labels.insert(label);
        } catch (const std::exception &) { /* Try another image or request a geometry preview. */ }
    }
    if (!views.empty()) {
        record["views"] = views;
        record["thumb"] = views.front()["src"];
        record["thumbCacheFile"] = views.front()["cacheFile"];
        record["thumbState"] = "ready";
        record["thumbError"] = "";
        record["thumbSource"] = "embedded";
    }
}

void add_filament(json &record, const json &config, size_t slot, const std::string &colour,
                  const std::string &profile, int plate)
{
    if (colour == "#00000000" || (colour.empty() && profile.empty())) return;
    std::string material;
    const auto undecorated_profile = Preset::strip_project_decoration(profile);
    const auto profiles = config.find("filament_settings_id");
    if (profiles != config.end() && profiles->is_array()) {
        for (size_t i = 0; i < profiles->size(); ++i) {
            const auto stored_profile = string_at(config, "filament_settings_id", i);
            if (stored_profile == profile) {
                material = string_at(config, "filament_type", i);
                break;
            }
            if (material.empty() && Preset::strip_project_decoration(stored_profile) == undecorated_profile)
                material = string_at(config, "filament_type", i);
        }
    }
    record["filaments"].push_back({{"slot", slot + 1}, {"plate", plate}, {"colour", colour},
        {"profile", profile}, {"type", material}, {"used", true}});
    if (!colour.empty()) record["colours"].push_back(colour);
    if (!material.empty()) record["materials"].push_back(material);
}

void inspect(json &record, const fs::path &file, const std::string &thumbnail_dir, const SourceStamp &stamp)
{
    Archive archive(file.string());
    if (!archive.open) throw std::runtime_error("Could not open this 3MF archive");
    const auto config_text = archive.read("Metadata/project_settings.config");
    const json config = config_text.empty() ? json::object() : json::parse(config_text);
    if (!config.is_object()) throw std::runtime_error("Project settings are not a JSON object");
    const std::string header = archive.header("3D/3dmodel.model");
    const std::regex metadata_tag(R"(<metadata\s[^>]*>[\s\S]*?</metadata>)");
    for (auto it = std::sregex_iterator(header.begin(), header.end(), metadata_tag); it != std::sregex_iterator(); ++it) {
        const auto node = xml(it->str());
        const auto name = node.get<std::string>("metadata.<xmlattr>.name", "");
        if (name == "Title") record["title"] = node.get<std::string>("metadata", "");
        if (name == "Designer") record["designer"] = node.get<std::string>("metadata", "");
        if (name == "DesignModelId") record["makerworld"] = true;
    }
    record["printer"] = string_at(config, "printer_settings_id");
    record["process"] = string_at(config, "print_settings_id");
    record["colours"] = json::array();
    record["materials"] = json::array();
    record["machines"] = json::array();
    record["filaments"] = json::array();
    record["plateCount"] = 0;
    int gcodes = 0;
    for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&archive.zip); ++i) {
        mz_zip_archive_file_stat stat{};
        if (mz_zip_reader_file_stat(&archive.zip, i, &stat) &&
            boost::starts_with(stat.m_filename, "Metadata/plate_") && boost::ends_with(stat.m_filename, ".gcode"))
            ++gcodes;
    }
    record["gcode"] = gcodes;
    const auto model_settings = xml(archive.read("Metadata/model_settings.config"));
    const auto plates = model_settings.get_child_optional("config");
    bool owns_filament_slots = false;
    if (plates) {
        for (const auto &entry : *plates) {
            if (entry.first != "plate") continue;
            const int plate = record["plateCount"].get<int>() + 1;
            record["plateCount"] = plate;
            auto meta = metadata(entry.second);
            if (!meta["plater_printer_preset"].empty()) record["machines"].push_back(meta["plater_printer_preset"]);
            std::vector<std::string> colours, profiles;
            unescape_strings_cstyle(meta["plater_filament_colours"], colours);
            unescape_strings_cstyle(meta["plater_filament_presets"], profiles);
            owns_filament_slots |= !profiles.empty() || !colours.empty();
            // Legacy files may lack the entire palette. Only the same slot with the same
            // declared material can inherit a project colour; an explicit empty cell cannot.
            if (colours.empty() && !profiles.empty()) {
                colours.resize(profiles.size());
                for (size_t i = 0; i < profiles.size(); ++i)
                    if (Preset::strip_project_decoration(profiles[i]) ==
                        Preset::strip_project_decoration(string_at(config, "filament_settings_id", i)))
                        colours[i] = string_at(config, "filament_colour", i);
            }
            for (size_t i = 0; i < std::max(colours.size(), profiles.size()); ++i)
                add_filament(record, config, i, i < colours.size() ? colours[i] : "",
                             i < profiles.size() ? profiles[i] : "", plate);
        }
    }
    if (!owns_filament_slots) {
        size_t slots = 0;
        for (const char *key : {"filament_colour", "filament_settings_id"})
            if (const auto values = config.find(key); values != config.end())
                slots = std::max(slots, values->is_array() ? values->size() : size_t(values->is_string()));
        for (size_t i = 0; i < slots; ++i)
            add_filament(record, config, i, string_at(config, "filament_colour", i),
                         string_at(config, "filament_settings_id", i), 0);
    }
    // Measurements belong to the saved slice and its palette. Never copy a shared folder's
    // sidecar weights onto every project in that folder, or weigh an edited colour as its old one.
    try {
        const auto slices = xml(archive.read("Metadata/slice_info.config"));
        const auto slice_plates = slices.get_child_optional("config");
        double seconds = 0, grams = 0;
        if (slice_plates && gcodes > 0) {
            for (const auto &entry : *slice_plates) {
                if (entry.first != "plate") continue;
                const auto meta = metadata(entry.second);
                const auto index = meta.find("index");
                if (index == meta.end()) continue;
                const int plate = std::stoi(index->second);
                const auto gcode = "Metadata/plate_" + std::to_string(plate) + ".gcode";
                if (mz_zip_reader_locate_file(&archive.zip, gcode.c_str(), nullptr, 0) < 0) continue;
                if (const auto prediction = meta.find("prediction"); prediction != meta.end()) {
                    const double value = std::stod(prediction->second);
                    if (std::isfinite(value) && value > 0) seconds += value;
                }
                for (const auto &filament : entry.second) {
                    if (filament.first != "filament") continue;
                    const auto weight = filament.second.get_optional<double>("<xmlattr>.used_g");
                    if (!weight || !std::isfinite(*weight) || *weight < 0) continue;
                    const int slot = filament.second.get<int>("<xmlattr>.id", 0);
                    const auto colour = filament.second.get<std::string>("<xmlattr>.color", "");
                    for (auto &declared : record["filaments"]) {
                        if (declared["slot"] != slot || (declared["plate"] != 0 && declared["plate"] != plate)) continue;
                        if (!boost::iequals(declared["colour"].get<std::string>(), colour)) continue;
                        declared["grams"] = declared.value("grams", 0.0) + *weight;
                        grams += *weight;
                    }
                }
            }
        }
        if (seconds > 0) record["hours"] = seconds / 3600;
        if (grams > 0) record["grams"] = grams;
    } catch (const std::exception &) {
        record["note"] = "Some saved slice measurements could not be read.";
    }
    if (record["machines"].empty() && !string_at(config, "printer_settings_id").empty())
        record["machines"].push_back(string_at(config, "printer_settings_id"));
    for (const char *field : {"colours", "materials", "machines"}) {
        auto &values = record[field];
        std::sort(values.begin(), values.end());
        values.erase(std::unique(values.begin(), values.end()), values.end());
    }
    bool bambu = false;
    for (const auto &machine : record["machines"]) {
        const auto name = boost::algorithm::to_lower_copy(machine.get<std::string>());
        bambu |= name.find("bambu") != std::string::npos || boost::starts_with(name, "bbl ");
    }
    record["state"] = gcodes ? "sliced" : record["machines"].empty() ? "mesh" : bambu ? "foreign" : "prepped";

    extract_thumbnails(record, archive, file, thumbnail_dir, stamp);
}
} // namespace

json thumbnail(const json &item, const std::string &thumbnail_dir, const std::string &executable,
                 const std::atomic<bool> &cancel, bool force)
{
    const fs::path file(item.value("path", ""));
    json result{{"path", file.string()}, {"revision", ""}, {"size", uint64_t(0)}};
    pending_thumbnail(result);
    const auto unavailable = [&result](const std::string &error) {
        result["thumb"] = "";
        result["views"] = json::array();
        result["thumbState"] = "unavailable";
        result["thumbError"] = error;
        return result;
    };
    try {
        SourceStamp stamp;
        if (!source_stamp(file, stamp)) return unavailable("File is missing or cannot be read.");
        result["revision"] = stamp.revision;
        result["size"] = stamp.size;
        if (cancel) return unavailable("Preview cancelled.");
        if (thumbnail_dir.empty()) return unavailable("Preview cache is unavailable.");
        const fs::path target = cache_file(thumbnail_dir, file, stamp, "geometry");
        result["thumbCacheFile"] = target.string();
        if (!force && cached_png(target, true)) {
            ready_thumbnail(result, target, "Model", "geometry");
            if (unchanged(file, stamp)) return result;
            return unavailable("File changed while its preview was being read. Refresh the library.");
        }
        if (boost::iequals(file.extension().string(), ".3mf")) {
            Archive archive(file.string());
            if (archive.open) {
                result["plateCount"] = item.value("plateCount", 0);
                extract_thumbnails(result, archive, file, thumbnail_dir, stamp, force);
                result.erase("plateCount");
                if (result["thumbState"] == "ready") {
                    if (unchanged(file, stamp)) return result;
                    return unavailable("File changed while its preview was being read. Refresh the library.");
                }
            }
        }
        fs::create_directories(fs::path(thumbnail_dir));
        Scratch scratch{fs::path(thumbnail_dir)};
        if (force && fs::exists(target) && !valid_png(read_file(target), true)) {
            boost::system::error_code error;
            fs::rename(target, scratch.path / "invalid-cache.png", error);
        }
        const fs::path staged = scratch.path / "preview.png";
        const auto error = run_renderer(executable, file, staged, scratch.path, cancel);
        if (!error.empty()) return unavailable(error);
        if (cancel) return unavailable("Preview cancelled.");
        if (!valid_png(read_file(staged), true)) return unavailable("Renderer did not produce a valid preview.");
        if (!unchanged(file, stamp)) return unavailable("File changed while its preview was being rendered. Refresh the library.");
        if (!publish(staged, target)) return unavailable("Could not save the generated preview.");
        ready_thumbnail(result, target, "Model", "geometry");
        return result;
    } catch (const std::exception &error) {
        return unavailable(std::string("Preview unavailable: ") + error.what());
    }
}

json scan(const std::vector<std::string> &roots, const std::string &thumbnail_dir,
          const json &previous, const std::atomic<bool> &cancel)
{
    constexpr int schema_version = 2;
    json result{{"items", json::array()}, {"roots", roots}, {"rootStatus", json::array()}, {"native", true},
        {"schemaVersion", schema_version},
        {"generated", std::chrono::system_clock::to_time_t(std::chrono::system_clock::now())}};
    std::map<std::string, json> cached;
    if (previous.is_object() && previous.value("schemaVersion", 0) == schema_version &&
        previous.contains("items") && previous["items"].is_array())
        for (const auto &item : previous["items"])
            cached[path_key(item.value("path", ""))] = item;
    std::set<std::string> seen;
    for (const auto &root_string : roots) {
        if (cancel) break;
        const fs::path root(root_string);
        boost::system::error_code ec;
        json status{{"path", root_string}, {"count", 0}, {"error", ""}};
        if (!fs::is_directory(root, ec)) {
            status["error"] = "Folder unavailable. Reconnect the drive or add its new location.";
            result["rootStatus"].push_back(status);
            continue;
        }
        fs::recursive_directory_iterator it(root, fs::directory_options::none, ec), end;
        for (; !ec && it != end && !cancel; it.increment(ec)) {
            const auto path = it->path();
            const auto filename = path.filename().string();
            if (filename == "__MACOSX" || boost::starts_with(filename, "._")) {
                if (fs::is_directory(path, ec)) it.disable_recursion_pending();
                ec.clear();
                continue;
            }
            const std::string extension = boost::algorithm::to_lower_copy(path.extension().string());
            if (extension != ".3mf" && extension != ".stl" && extension != ".obj" && extension != ".step" && extension != ".stp") continue;
            if (!fs::is_regular_file(path, ec)) { ec.clear(); continue; }
            const auto key = path_key(path.string());
            if (!seen.insert(key).second) continue;
            SourceStamp stamp;
            if (!source_stamp(path, stamp)) continue;
            const auto size = stamp.size;
            const auto mtime = stamp.mtime;
            const auto &revision = stamp.revision;
            json record;
            auto old = cached.find(key);
            if (old != cached.end() && old->second.value("revision", std::string()) == revision &&
                old->second.value("size", uint64_t(0)) == size && old->second.value("state", "") != "unreadable")
            {
                record = old->second;
                if (!hydrate_thumbnail(record, thumbnail_dir, path, stamp)) {
                    const bool had_embedded = record.value("thumbSource", "") == "embedded";
                    pending_thumbnail(record);
                    if (had_embedded && extension == ".3mf") {
                        try {
                            Archive archive(path.string());
                            if (archive.open) extract_thumbnails(record, archive, path, thumbnail_dir, stamp);
                        } catch (const std::exception &) {}
                    }
                }
            }
            else {
                record = {{"path", path.string()}, {"name", path.stem().string()}, {"mtime", mtime}, {"size", size}, {"revision", revision},
                    {"sizeMB", std::round(size / 10000.0) / 100.0}, {"kind", extension.substr(1)},
                    {"state", "mesh"}, {"superseded", false}, {"thumb", ""}};
                pending_thumbnail(record);
                if (extension == ".3mf") {
                    try { inspect(record, path, thumbnail_dir, stamp); }
                    catch (const std::exception &error) { record["state"] = "unreadable"; record["note"] = error.what(); }
                }
                if (record["thumbState"] != "ready") hydrate_thumbnail(record, thumbnail_dir, path, stamp);
            }
            record["root"] = root_string;
            record["folder"] = path.parent_path().string();
            fs::path group;
            for (const auto &part : path.parent_path().lexically_relative(root)) {
                const auto name = part.string();
                if (name == ".") continue;
                if (name == "02-needs-colour" && record["state"] != "unreadable") record["state"] = "needs-colour";
                if (name != "01-source" && name != "02-needs-colour" && name != "03-prepped" && name != "04-sliced" && name != "05-sent") group /= part;
            }
            record["group"] = group.empty() ? root.filename().string() : group.generic_string();
            result["items"].push_back(std::move(record));
            status["count"] = status["count"].get<int>() + 1;
        }
        if (ec) status["error"] = "Some folders could not be read: " + ec.message();
        result["rootStatus"].push_back(status);
    }
    return result;
}
} // namespace Slic3r::GUI::ProjectLibrary
