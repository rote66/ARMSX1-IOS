#include <SDL.h>
#include <SDL_gamecontroller.h>
#include <SDL_render.h>
#include <SDL_system.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <csignal>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if defined(__ANDROID__)
#include <unistd.h>
#endif

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#if !defined(UWP_TARGET)
#include <dbghelp.h>
#endif
#elif !defined(__EMSCRIPTEN__) && !defined(__ANDROID__) && !defined(PSVITA_TARGET)
#if defined(__has_include)
#if __has_include(<execinfo.h>)
#include <dlfcn.h>
#include <execinfo.h>
#define PSXE_HAS_EXECINFO 1
#endif
#endif
#endif

#if defined(__EMSCRIPTEN__)
#include <emscripten/html5.h>
#endif

#if defined(__ANDROID__)
#include <jni.h>
#endif

extern "C" {
#include "../psx/psx.h"
#include "../psx/fastboot.h"
#include "../psx/dev/cdrom/cdrom.h"
#include "../psx/dev/gpu.h"
#include "../psx/dev/input.h"
#include "../psx/dev/pad.h"
#include "../psx/dev/timer.h"
#include "../psx/input/sda.h"
#include "common.h"
#include "config.h"
#include "toml.h"
}

#include "archive.h"
#include "IconsFontAwesome5.h"
#include "platform_file.h"
#ifdef USE_HARDWARE
#include "gpu_hw.h"
#endif
#include "fsui/backend_sdl.hpp"
#include "fsui/fsui.hpp"
#include "fsui/imgui_fullscreen.hpp"
#include "imgui.h"

#undef main

namespace {

constexpr Uint32 kPauseChordGraceMs = 120;
constexpr std::string_view kCustomFsuiAppIconPath = "icons/FsuiAppIcon.png";

constexpr bool SupportsManagedWindowSizing() {
#if defined(__ANDROID__) || defined(IOS_TARGET) || defined(__EMSCRIPTEN__) || defined(UWP_TARGET) || defined(PSVITA_TARGET)
    return false;
#else
    return true;
#endif
}

constexpr bool DefaultVsyncEnabled() {
#if defined(UWP_TARGET)
    return false;
#else
    return true;
#endif
}

constexpr bool DefaultMobileControlsEnabled() {
#if defined(__ANDROID__) || defined(IOS_TARGET)
    return true;
#else
    return false;
#endif
}

#ifdef USE_HARDWARE
enum class GpuBackend {
    Software = 0,
    SDLAccelerated = 1,
};
#endif

class ArmsxApp;

ArmsxApp* g_active_app = nullptr;
std::atomic_bool g_crash_reporting{false};
std::mutex g_pending_launch_lock;
std::vector<std::string> g_pending_launch_arguments;
std::vector<std::string> g_pending_web_errors;

struct PlatformLibraryRoot {
    std::string path;
    std::string label;
    bool recursive = true;
};

struct PlatformLibraryGame {
    std::string path;
    std::string label;
    std::uint64_t size = 0;
    std::time_t modified = 0;
};

using PlatformDirectoryPickerCallback = void (*)(int recursive, void* userdata);
using PlatformDirectoryRemovedCallback = void (*)(const char* path, void* userdata);

std::mutex g_platform_library_lock;
std::vector<PlatformLibraryRoot> g_platform_library_roots;
std::vector<PlatformLibraryGame> g_platform_library_games;
std::uint64_t g_platform_library_version = 0;
PlatformDirectoryPickerCallback g_platform_directory_picker_callback = nullptr;
void* g_platform_directory_picker_userdata = nullptr;
PlatformDirectoryRemovedCallback g_platform_directory_removed_callback = nullptr;
void* g_platform_directory_removed_userdata = nullptr;

constexpr double kUiFrameRate = 60.0;
constexpr Uint32 kMobileControlsAutoHideMs = 3000;

const char* LogLevelTitle(int level) {
    switch (level) {
        case LOG_TRACE: return "trace";
        case LOG_DEBUG: return "debug";
        case LOG_INFO: return "info";
        case LOG_WARN: return "warn";
        case LOG_ERROR: return "error";
        case LOG_FATAL: return "fatal";
        default: return "unknown";
    }
}

void StructuredLogCallback(log_Event* ev) {
    if (!ev) {
        return;
    }

    char message[4096] = {};
    vsnprintf(message, sizeof(message), ev->fmt, ev->ap);
    psxe_diag_logf("psx", "%s %s:%d %s", LogLevelTitle(ev->level), ev->file, ev->line, message);
}

void SdlLogOutput(void*, int category, SDL_LogPriority priority, const char* message) {
    psxe_diag_logf("sdl", "category=%d priority=%d %s", category, static_cast<int>(priority), message ? message : "");
}

[[noreturn]] void ReportNativeCrash(const char* reason);

#if defined(_WIN32) && !defined(UWP_TARGET)
LONG WINAPI WindowsUnhandledExceptionFilter(EXCEPTION_POINTERS* exception_info) {
    const DWORD code = exception_info && exception_info->ExceptionRecord
        ? exception_info->ExceptionRecord->ExceptionCode
        : 0u;
    char reason[64] = {};
    SDL_snprintf(reason, sizeof(reason), "SEH 0x%08lx", static_cast<unsigned long>(code));
    ReportNativeCrash(reason);
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

double CounterTicksToMilliseconds(uint64_t ticks) {
    const uint64_t frequency = SDL_GetPerformanceFrequency();
    if (frequency == 0) {
        return 0.0;
    }

    return (static_cast<double>(ticks) * 1000.0) / static_cast<double>(frequency);
}

std::string RendererFlagsTitle(Uint32 flags) {
    std::string title;

    auto append = [&](const char* value) {
        if (!title.empty()) {
            title.append("|");
        }
        title.append(value);
    };

    if (flags & SDL_RENDERER_SOFTWARE) {
        append("software");
    }
    if (flags & SDL_RENDERER_ACCELERATED) {
        append("accelerated");
    }
    if (flags & SDL_RENDERER_PRESENTVSYNC) {
        append("present-vsync");
    }
    if (flags & SDL_RENDERER_TARGETTEXTURE) {
        append("target-texture");
    }

    if (title.empty()) {
        title = "none";
    }

    return title;
}

[[noreturn]] void ReportNativeCrash(const char* reason);
void WriteNativeStackTraceImpl();

enum class LaunchKind {
    None,
    Bios,
    Disc,
    Exe,
};

const char* CpuEngineTitle(psx_cpu_execution_mode_t mode) {
    return mode == PSX_CPU_INTERPRETER ? "参考解释器" : "缓存解释器";
}

const char* CpuEngineSettingToken(psx_cpu_execution_mode_t mode) {
    return mode == PSX_CPU_INTERPRETER ? "interpreter" : "cached";
}

std::optional<psx_cpu_execution_mode_t> ParseCpuEngine(const char* value) {
    if (!value || !value[0]) {
        return std::nullopt;
    }

    std::string lowered(value);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    if (lowered == "interpreter" || lowered == "reference") {
        return PSX_CPU_INTERPRETER;
    }
    if (lowered == "cached" || lowered == "cached-interpreter") {
        return PSX_CPU_CACHED_INTERPRETER;
    }
    return std::nullopt;
}

const char* LaunchKindTitle(LaunchKind kind) {
    switch (kind) {
        case LaunchKind::Bios: return "bios";
        case LaunchKind::Disc: return "disc";
        case LaunchKind::Exe: return "exe";
        case LaunchKind::None:
        default:
            return "none";
    }
}

struct CliFlags {
    bool bios = false;
    bool bios_folder = false;
    bool model = false;
    bool region = false;
    bool scale = false;
    bool settings_file = false;
    bool quiet = false;
    bool log_level = false;
    bool exp_rom = false;
    bool exe = false;
    bool cdrom = false;
    bool cpu_engine = false;
    bool has_boot_request = false;
};

struct LaunchRequest {
    LaunchKind kind = LaunchKind::None;
    std::filesystem::path path;
    std::string label;
};

void EnqueuePendingLaunchArgument(std::string argument) {
    if (argument.empty()) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_pending_launch_lock);
    g_pending_launch_arguments.push_back(std::move(argument));
}

std::vector<std::string> DrainPendingLaunchArguments() {
    std::lock_guard<std::mutex> lock(g_pending_launch_lock);
    std::vector<std::string> pending;
    pending.swap(g_pending_launch_arguments);
    return pending;
}

void EnqueueWebError(std::string message) {
    if (message.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_pending_launch_lock);
    g_pending_web_errors.push_back(std::move(message));
}

std::vector<std::string> DrainWebErrors() {
    std::lock_guard<std::mutex> lock(g_pending_launch_lock);
    std::vector<std::string> pending;
    pending.swap(g_pending_web_errors);
    return pending;
}

bool IsAndroidVirtualPath(std::string_view path) {
    return path.rfind("armsx-android:///", 0) == 0;
}

bool PlatformPathBelongsToRoot(std::string_view path, std::string_view root, bool recursive) {
    if (path.size() <= root.size() || path.substr(0, root.size()) != root || path[root.size()] != '/') {
        return false;
    }

    const std::string_view relative = path.substr(root.size() + 1);
    return recursive || relative.find('/') == std::string_view::npos;
}

void SetPlatformLibrarySnapshot(
    std::vector<PlatformLibraryRoot> roots,
    std::vector<PlatformLibraryGame> games
) {
    std::lock_guard<std::mutex> lock(g_platform_library_lock);
    g_platform_library_roots = std::move(roots);
    g_platform_library_games = std::move(games);
    g_platform_library_version++;
}

void RegisterPlatformLibraryRoot(std::string path, std::string label, bool recursive) {
    if (path.empty()) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_platform_library_lock);
    const auto found = std::find_if(g_platform_library_roots.begin(), g_platform_library_roots.end(), [&](const auto& root) {
        return root.path == path;
    });
    if (found == g_platform_library_roots.end()) {
        g_platform_library_roots.push_back(PlatformLibraryRoot{
            .path = std::move(path),
            .label = std::move(label),
            .recursive = recursive,
        });
    } else {
        found->label = std::move(label);
        found->recursive = recursive;
    }
    g_platform_library_version++;
}

std::uint64_t CopyPlatformLibrarySnapshot(
    std::vector<PlatformLibraryRoot>& roots,
    std::vector<PlatformLibraryGame>& games
) {
    std::lock_guard<std::mutex> lock(g_platform_library_lock);
    roots = g_platform_library_roots;
    games = g_platform_library_games;
    return g_platform_library_version;
}

#if defined(__ANDROID__)
bool CallAndroidActivityVoidMethod(const char* name, const char* signature, jvalue* args = nullptr) {
    JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    jobject activity = static_cast<jobject>(SDL_AndroidGetActivity());
    if (!env || !activity) {
        return false;
    }

    jclass cls = env->GetObjectClass(activity);
    if (!cls) {
        env->DeleteLocalRef(activity);
        return false;
    }

    jmethodID method = env->GetMethodID(cls, name, signature);
    if (!method) {
        env->ExceptionClear();
        env->DeleteLocalRef(cls);
        env->DeleteLocalRef(activity);
        return false;
    }

    env->CallVoidMethodA(activity, method, args);
    const bool succeeded = !env->ExceptionCheck();
    if (!succeeded) {
        env->ExceptionDescribe();
        env->ExceptionClear();
    }
    env->DeleteLocalRef(cls);
    env->DeleteLocalRef(activity);
    return succeeded;
}

bool RequestAndroidLibraryDirectory(bool recursive) {
    jvalue args[1]{};
    args[0].z = recursive ? JNI_TRUE : JNI_FALSE;
    return CallAndroidActivityVoidMethod("openLibraryDirectoryPicker", "(Z)V", args);
}

void RemoveAndroidLibraryDirectory(std::string_view path) {
    JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    jobject activity = static_cast<jobject>(SDL_AndroidGetActivity());
    if (!env || !activity) {
        return;
    }

    jclass cls = env->GetObjectClass(activity);
    jmethodID method = cls ? env->GetMethodID(cls, "removeLibraryDirectory", "(Ljava/lang/String;)V") : nullptr;
    if (method) {
        jstring value = env->NewStringUTF(std::string(path).c_str());
        env->CallVoidMethod(activity, method, value);
        env->DeleteLocalRef(value);
    } else {
        env->ExceptionClear();
    }
    if (cls) {
        env->DeleteLocalRef(cls);
    }
    env->DeleteLocalRef(activity);
}

FILE* AndroidPlatformFopen(const char* path, const char* mode, void*) {
    if (!path || !mode || !IsAndroidVirtualPath(path)) {
        return fopen(path, mode);
    }
    if (mode[0] != 'r') {
        return nullptr;
    }

    JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    jobject activity = static_cast<jobject>(SDL_AndroidGetActivity());
    if (!env || !activity) {
        return nullptr;
    }

    int fd = -1;
    jclass cls = env->GetObjectClass(activity);
    jmethodID method = cls ? env->GetMethodID(cls, "openVirtualFileDescriptor", "(Ljava/lang/String;)I") : nullptr;
    if (method) {
        jstring value = env->NewStringUTF(path);
        fd = env->CallIntMethod(activity, method, value);
        env->DeleteLocalRef(value);
    } else {
        env->ExceptionClear();
    }
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
        fd = -1;
    }
    if (cls) {
        env->DeleteLocalRef(cls);
    }
    env->DeleteLocalRef(activity);

    if (fd < 0) {
        return nullptr;
    }

    FILE* file = fdopen(fd, mode);
    if (!file) {
        close(fd);
    }
    return file;
}
#endif

bool RequestPlatformLibraryDirectory(bool recursive) {
#if defined(__ANDROID__)
    if (RequestAndroidLibraryDirectory(recursive)) {
        return true;
    }
#endif
    if (g_platform_directory_picker_callback) {
        g_platform_directory_picker_callback(recursive ? 1 : 0, g_platform_directory_picker_userdata);
        return true;
    }
    return false;
}

void NotifyPlatformLibraryDirectoryRemoved(std::string_view path) {
#if defined(__ANDROID__)
    if (IsAndroidVirtualPath(path)) {
        RemoveAndroidLibraryDirectory(path);
    }
#endif
    if (g_platform_directory_removed_callback) {
        g_platform_directory_removed_callback(std::string(path).c_str(), g_platform_directory_removed_userdata);
    }
}

struct FrontendSettings {
    std::string settings_path;
    std::string bios_override;
    std::string bios_search = "bios";
    std::string model = "scph1001";
    std::string region = "auto";
    std::string exp_path;
    std::string default_exe_path;
    int scale = 3;
    int log_level = LOG_INFO;
    bool quiet = true;
    bool logging_enabled = false;
    bool vsync_enabled = DefaultVsyncEnabled();
    psx_cpu_execution_mode_t cpu_engine = PSX_CPU_CACHED_INTERPRETER;
#ifdef USE_HARDWARE
    GpuBackend gpu_backend = GpuBackend::Software;
#endif
    bool texture_scale_mode = false;
    bool debug_panel = false;
    bool show_fps = false;
    bool fast_boot = false;
    bool stretch_mode = false;
    int display_aspect = 0;
    int upscale_height = 480;
    int presentation_upscale = 1;
    bool mobile_controls = DefaultMobileControlsEnabled();
    fsui::UiState ui_state{
        .theme = "Dark",
        .prompt_icon_pack = fsui::PromptIconPack::Auto,
        .background_image_path = {},
        .default_game_view = 0,
        .game_sort = 2,
        .game_sort_reverse = false,
        .game_list_paths = {},
        .game_list_recursive_paths = {},
        .covers_path = {},
        .show_inputs_overlay = false,
        .show_settings_overlay = false,
        .show_performance_overlay = false,
    };
};

struct PendingChordButton {
    bool physical_down = false;
    bool pending = false;
    bool forwarded = false;
    Uint32 pending_since = 0;
};

enum class FsuiWindowState {
    None,
    Landing,
    StartGame,
    Exit,
    GameList,
    Settings,
    PauseMenu,
};

std::string ToLower(std::string_view value) {
    std::string out(value);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return out;
}

std::string Trim(std::string_view value) {
    size_t begin = 0;
    size_t end = value.size();

    while ((begin < end) && std::isspace(static_cast<unsigned char>(value[begin]))) {
        begin++;
    }

    while ((end > begin) && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        end--;
    }

    return std::string(value.substr(begin, end - begin));
}

std::string NormalizeModel(std::string_view value) {
    std::string out;

    for (unsigned char ch : value) {
        if (std::isalnum(ch)) {
            out.push_back(static_cast<char>(std::tolower(ch)));
        }
    }

    return out;
}

std::string DefaultSettingsPath() {
    const char* pref = psxe_cfg_get_pref_path();

    if (pref && pref[0]) {
        return std::string(pref) + "settings.toml";
    }

    return "settings.toml";
}

std::filesystem::path DefaultBrowseDirectory() {
#if defined(IOS_TARGET)
    const char* documents = std::getenv("ARMSX_DOCUMENTS_DIR");

    if (documents && documents[0]) {
        return std::filesystem::path(documents);
    }
#endif

    const char* pref = psxe_cfg_get_pref_path();

    if (pref && pref[0]) {
        return std::filesystem::path(pref);
    }

    return std::filesystem::current_path();
}

std::filesystem::path DefaultDiagnosticsLogPath() {
#if defined(IOS_TARGET)
    return DefaultBrowseDirectory() / "armsx_log.txt";
#else
    std::filesystem::path path = DefaultBrowseDirectory() / "logs";
#if defined(UWP_TARGET)
    path /= "armsx-uwp.log";
#else
    path /= "armsx.log";
#endif
    return path;
#endif
}

std::string AppendBrowseRootHint(std::string summary) {
#if defined(_WIN32)
    if (!summary.empty() && summary.back() != ' ') {
        summary.push_back(' ');
    }
    summary += "使用“文件系统根目录”或“上级目录”切换位置。";
#endif
    return summary;
}

bool PathsMatch(const std::filesystem::path& left, const std::filesystem::path& right) {
#if defined(_WIN32)
    return ToLower(left.lexically_normal().generic_string()) == ToLower(right.lexically_normal().generic_string());
#else
    return left.lexically_normal() == right.lexically_normal();
#endif
}

bool PathListContains(const std::vector<std::filesystem::path>& paths, const std::filesystem::path& candidate) {
    return std::any_of(paths.begin(), paths.end(), [&](const std::filesystem::path& path) {
        return PathsMatch(path, candidate);
    });
}

bool FileContainsCaseInsensitive(const std::filesystem::path& path, const std::string& needle_lower) {
    std::ifstream input(path, std::ios::binary);
    if (!input.good()) {
        return false;
    }

    std::string content((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    return ToLower(content).find(needle_lower) != std::string::npos;
}

bool CueDirectoryReferencesImage(const std::filesystem::path& image_path) {
    if (!image_path.has_parent_path()) {
        return false;
    }

    const std::string needle = ToLower(image_path.filename().string());
    std::error_code ec;

    for (const auto& item : std::filesystem::directory_iterator(image_path.parent_path(), ec)) {
        if (ec) {
            break;
        }
        if (!item.is_regular_file()) {
            continue;
        }
        if (ToLower(item.path().extension().string()) != ".cue") {
            continue;
        }
        if (FileContainsCaseInsensitive(item.path(), needle)) {
            return true;
        }
    }

    return false;
}

bool IsLikelyBiosImagePath(const std::filesystem::path& path, const FrontendSettings& settings) {
    const std::string ext = ToLower(path.extension().string());
    if (ext != ".bin" && ext != ".rom") {
        return false;
    }

    if (!settings.bios_override.empty() && PathsMatch(path, std::filesystem::path(settings.bios_override))) {
        return true;
    }

    const std::string stem = NormalizeModel(path.stem().string());
    if (stem == "bios" || stem == "biosbin") {
        return true;
    }
    if (stem.starts_with("scph")) {
        return true;
    }

    return false;
}

std::string NormalizePathString(std::string value) {
#if defined(_WIN32)
    std::replace(value.begin(), value.end(), '\\', '/');
    if (value.size() == 2 && std::isalpha(static_cast<unsigned char>(value[0])) && value[1] == ':') {
        value.push_back('/');
    }
#endif
    return value;
}

bool StartsWithCaseInsensitive(std::string_view value, std::string_view prefix) {
    if (value.size() < prefix.size()) {
        return false;
    }

    for (size_t index = 0; index < prefix.size(); index++) {
        const unsigned char lhs = static_cast<unsigned char>(value[index]);
        const unsigned char rhs = static_cast<unsigned char>(prefix[index]);
        if (std::tolower(lhs) != std::tolower(rhs)) {
            return false;
        }
    }

    return true;
}

int HexValue(unsigned char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return 10 + (ch - 'a');
    }
    if (ch >= 'A' && ch <= 'F') {
        return 10 + (ch - 'A');
    }
    return -1;
}

std::string PercentDecode(std::string_view value) {
    std::string decoded;
    decoded.reserve(value.size());

    for (size_t index = 0; index < value.size(); index++) {
        const unsigned char ch = static_cast<unsigned char>(value[index]);
        if ((ch == '%') && ((index + 2) < value.size())) {
            const int hi = HexValue(static_cast<unsigned char>(value[index + 1]));
            const int lo = HexValue(static_cast<unsigned char>(value[index + 2]));
            if (hi >= 0 && lo >= 0) {
                decoded.push_back(static_cast<char>((hi << 4) | lo));
                index += 2;
                continue;
            }
        }

        decoded.push_back(ch == '+' ? ' ' : static_cast<char>(ch));
    }

    return decoded;
}

std::optional<std::string> NormalizedPickerSelection(const std::string& path) {
    const std::string trimmed = Trim(path);
    if (trimmed.empty()) {
        return std::nullopt;
    }

    return NormalizePathString(trimmed);
}

std::string WithHiddenId(std::string_view label, std::string_view id) {
    std::string out(label);
    out += "##";
    out += id;
    return out;
}

bool ReadSdlFileBytes(std::string_view path, std::vector<unsigned char>& bytes) {
    SDL_RWops* handle = SDL_RWFromFile(std::string(path).c_str(), "rb");
    if (!handle) {
        return false;
    }

    bytes.clear();
    std::array<unsigned char, 16 * 1024> buffer{};
    for (;;) {
        const size_t read = SDL_RWread(handle, buffer.data(), 1, buffer.size());
        if (read == 0) {
            break;
        }
        bytes.insert(bytes.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(read));
    }

    SDL_RWclose(handle);
    return !bytes.empty();
}

std::string MaterializeBundledFsuiAppIcon() {
    const std::filesystem::path destination = DefaultBrowseDirectory() / kCustomFsuiAppIconPath;
    std::error_code ec;

    if (std::filesystem::exists(destination, ec) && std::filesystem::is_regular_file(destination, ec)) {
        return destination.string();
    }

    std::vector<unsigned char> bytes;
    if (!ReadSdlFileBytes(kCustomFsuiAppIconPath, bytes)) {
        return {};
    }

    std::filesystem::create_directories(destination.parent_path(), ec);
    if (ec) {
        return {};
    }

    std::ofstream output(destination, std::ios::binary | std::ios::trunc);
    if (!output.good()) {
        return {};
    }

    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!output.good()) {
        return {};
    }

    return destination.string();
}

std::string ResolveFsuiAppIconPath() {
    const std::filesystem::path relative(kCustomFsuiAppIconPath);
    std::error_code ec;

    auto try_path = [&](const std::filesystem::path& candidate) -> std::string {
        if (candidate.empty()) {
            return {};
        }
        ec.clear();
        if (std::filesystem::exists(candidate, ec) && std::filesystem::is_regular_file(candidate, ec)) {
            return candidate.lexically_normal().string();
        }
        return {};
    };

#ifdef PSVITA_TARGET
    const char* vita_base = "app0:/";
    struct VitaBaseDeleter { void operator()(const char*) const {} };
    std::unique_ptr<const char, VitaBaseDeleter> base_path(vita_base);
#else
    std::unique_ptr<char, decltype(&SDL_free)> base_path(SDL_GetBasePath(), &SDL_free);
#endif
    if (base_path) {
        const std::filesystem::path base(base_path.get());

        if (std::string resolved = try_path(base / relative); !resolved.empty()) {
            return resolved;
        }
        if (std::string resolved = try_path((base / ".." / "Resources" / relative).lexically_normal()); !resolved.empty()) {
            return resolved;
        }
    }

    if (std::string resolved = try_path(relative); !resolved.empty()) {
        return resolved;
    }
    if (std::string resolved = try_path(std::filesystem::current_path(ec) / relative); !resolved.empty()) {
        return resolved;
    }
    if (std::string resolved = MaterializeBundledFsuiAppIcon(); !resolved.empty()) {
        return resolved;
    }

    return std::string(kCustomFsuiAppIconPath);
}

bool IsDiscPath(const std::filesystem::path& path) {
    const std::string ext = ToLower(path.extension().string());
    return ext == ".cue" || ext == ".bin" || ext == ".iso" || ext == ".img"
#ifdef USE_CHD
        || ext == ".chd"
#endif
        ;
}

bool IsExePath(const std::filesystem::path& path) {
    const std::string ext = ToLower(path.extension().string());
    return ext == ".exe" || ext == ".ps-exe" || ext == ".psexe";
}

bool IsZipPath(const std::filesystem::path& path) {
    return armsx::IsZipPath(path);
}

std::string StemToTitle(const std::filesystem::path& path) {
    std::string value = path.stem().string();

    if (value.empty()) {
        value = path.filename().string();
    }

    std::replace(value.begin(), value.end(), '_', ' ');

    return Trim(value);
}

std::string EscapeTomlString(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 8);

    for (char ch : value) {
        switch (ch) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(ch); break;
        }
    }

    return out;
}

const char* AspectToString(int aspect) {
    switch (aspect) {
        case 1: return "square";
        case 2: return "wide16x9";
        default: return "classic";
    }
}

const char* AspectTitle(int aspect) {
    switch (aspect) {
        case 1: return "方形 1:1";
        case 2: return "宽屏 16:9";
        default: return "经典 4:3";
    }
}

const char* UpscaleToString(int height) {
    switch (height) {
        case 720: return "720p";
        case 1080: return "1080p";
        case 1440: return "1440p";
        case 2160: return "2160p";
        default: return "480p";
    }
}

std::string BoolTitle(bool value) {
    return value ? "开" : "关";
}

std::string RendererValueTitle(const FrontendSettings& settings) {
    std::string value = settings.texture_scale_mode ? "线性" : "最近邻";

    if (settings.stretch_mode) {
        value += "，拉伸";
    } else {
        value += "，保持比例";
    }

    return value;
}

#ifdef USE_HARDWARE
constexpr bool SupportsHardwareGpuBackend() {
    return true;
}

const char* GpuBackendTitle(GpuBackend backend) {
    switch (backend) {
        case GpuBackend::SDLAccelerated:
            return "SDL 加速";
        case GpuBackend::Software:
        default:
            return "SDL 软件";
    }
}

const char* GpuBackendSettingToken(GpuBackend backend) {
    switch (backend) {
        case GpuBackend::SDLAccelerated:
            return "sdl-accelerated";
        case GpuBackend::Software:
        default:
            return "software";
    }
}

std::optional<GpuBackend> ParseGpuBackendOverride(const char* value) {
    if (!value || !value[0]) {
        return std::nullopt;
    }

    const std::string lowered = ToLower(value);
    if (lowered == "sdl-accelerated" || lowered == "hardware" || lowered == "hardware (experimental)" || lowered == "hw" || lowered == "hw-renderer") {
        return GpuBackend::SDLAccelerated;
    }

    if (lowered == "software" || lowered == "sw" || lowered == "sw-renderer") {
        return GpuBackend::Software;
    }

    return std::nullopt;
}
#endif

int RegionFromSettings(const FrontendSettings& settings) {
    const std::string region = ToLower(settings.region);

    if (region == "pal") {
        return CDR_REGION_EUROPE;
    }

    if (region == "ntsc") {
        const std::string model = NormalizeModel(settings.model);
        static const std::array<const char*, 8> japan_models = {
            "scph1000",
            "scph3000",
            "scph3500",
            "scph5000",
            "scph5500",
            "scph7000",
            "scph7003",
            "scph100",
        };

        if (std::find(japan_models.begin(), japan_models.end(), model) != japan_models.end()) {
            return CDR_REGION_JAPAN;
        }

        return CDR_REGION_AMERICA;
    }

    const std::string model = NormalizeModel(settings.model);
    static const std::array<const char*, 8> europe_models = {
        "scph1002",
        "scph5502",
        "scph5552",
        "scph7002",
        "scph7502",
        "scph9002",
        "scph102a",
        "scph102b",
    };
    static const std::array<const char*, 8> japan_models = {
        "scph1000",
        "scph3000",
        "scph3500",
        "scph5000",
        "scph5500",
        "scph7000",
        "scph7003",
        "scph100",
    };

    if (std::find(europe_models.begin(), europe_models.end(), model) != europe_models.end()) {
        return CDR_REGION_EUROPE;
    }

    if (std::find(japan_models.begin(), japan_models.end(), model) != japan_models.end()) {
        return CDR_REGION_JAPAN;
    }

    return CDR_REGION_AMERICA;
}

LaunchRequest LaunchForPath(const std::filesystem::path& path) {
    LaunchRequest request;
    if (path.empty()) {
        return request;
    }

    request.path = std::filesystem::path(NormalizePathString(path.string()));
    request.label = StemToTitle(request.path);

    if (IsExePath(request.path)) {
        request.kind = LaunchKind::Exe;
    } else if (IsDiscPath(request.path)) {
        request.kind = LaunchKind::Disc;
    }

    return request;
}

std::optional<LaunchKind> ParseUriLaunchKind(std::string_view value) {
    const std::string lower = ToLower(Trim(value));

    if (lower == "bios") {
        return LaunchKind::Bios;
    }
    if (lower == "disc" || lower == "cdrom") {
        return LaunchKind::Disc;
    }
    if (lower == "exe") {
        return LaunchKind::Exe;
    }

    return std::nullopt;
}

std::optional<std::string> QueryValue(std::string_view query, std::string_view key) {
    size_t begin = 0;

    while (begin <= query.size()) {
        const size_t end = query.find('&', begin);
        const std::string_view pair = query.substr(begin, end == std::string_view::npos ? query.size() - begin : end - begin);
        const size_t sep = pair.find('=');
        const std::string_view pair_key = pair.substr(0, sep);

        if (ToLower(PercentDecode(pair_key)) == ToLower(key)) {
            if (sep == std::string_view::npos) {
                return std::string();
            }
            return PercentDecode(pair.substr(sep + 1));
        }

        if (end == std::string_view::npos) {
            break;
        }
        begin = end + 1;
    }

    return std::nullopt;
}

std::string DecodeLaunchUriPathPayload(std::string_view payload) {
    std::string decoded = PercentDecode(payload);

    if (decoded.rfind("///", 0) == 0) {
        decoded.erase(0, 2);
    } else if (decoded.rfind("//", 0) == 0) {
        decoded.erase(0, 2);
    }

#if defined(_WIN32)
    if (decoded.size() >= 3 &&
        decoded[0] == '/' &&
        std::isalpha(static_cast<unsigned char>(decoded[1])) &&
        decoded[2] == ':') {
        decoded.erase(decoded.begin());
    }
#endif

    return NormalizePathString(decoded);
}

LaunchRequest LaunchForUri(std::string_view uri) {
    LaunchRequest request;

    std::string_view remainder = uri;
    if (StartsWithCaseInsensitive(uri, "armsx:")) {
        remainder = uri.substr(6);
    } else if (StartsWithCaseInsensitive(uri, "web+armsx:")) {
        remainder = uri.substr(10);
    } else {
        return request;
    }
    const size_t fragment_pos = remainder.find('#');
    if (fragment_pos != std::string_view::npos) {
        remainder = remainder.substr(0, fragment_pos);
    }

    std::string_view query;
    const size_t query_pos = remainder.find('?');
    if (query_pos != std::string_view::npos) {
        query = remainder.substr(query_pos + 1);
        remainder = remainder.substr(0, query_pos);
    }

    std::optional<LaunchKind> kind = std::nullopt;
    if (const std::optional<std::string> kind_value = QueryValue(query, "kind")) {
        kind = ParseUriLaunchKind(*kind_value);
    }

    if (kind == LaunchKind::Bios) {
        request.kind = LaunchKind::Bios;
        request.label = "PlayStation BIOS";
        return request;
    }

    std::string launch_path;
    if (const std::optional<std::string> query_path = QueryValue(query, "path")) {
        launch_path = NormalizePathString(*query_path);
    } else {
        launch_path = DecodeLaunchUriPathPayload(remainder);
    }

    const std::string launch_path_lower = ToLower(Trim(launch_path));
    if (launch_path_lower == "bios" || launch_path_lower == "/bios") {
        request.kind = LaunchKind::Bios;
        request.label = "PlayStation BIOS";
        return request;
    }

    request = LaunchForPath(std::filesystem::path(launch_path));
    if (request.kind == LaunchKind::None) {
        return request;
    }

    if (kind == LaunchKind::Disc || kind == LaunchKind::Exe) {
        request.kind = *kind;
    }

    return request;
}

LaunchRequest LaunchForArgument(std::string_view argument, std::optional<LaunchKind> forced_kind = std::nullopt) {
    LaunchRequest request;
    const std::string trimmed = Trim(argument);
    if (trimmed.empty()) {
        return request;
    }

    if (StartsWithCaseInsensitive(trimmed, "armsx:")) {
        request = LaunchForUri(trimmed);
    } else {
        const std::filesystem::path path(trimmed);
#ifdef USE_CHD
        if (IsZipPath(path)) {
            std::string archive_error;
            const auto extracted = armsx::ExtractZipLaunchCandidate(path, archive_error);
            if (!extracted.has_value()) {
                psxe_diag_logf(
                    "launch",
                    "ZIP launch failed path=%s error=%s",
                    path.string().c_str(),
                    archive_error.c_str()
                );
                return request;
            }
            request = LaunchForPath(*extracted);
        } else
#endif
        {
            request = LaunchForPath(path);
        }
    }

    if (request.kind == LaunchKind::None) {
        return request;
    }

    if (forced_kind == LaunchKind::Bios) {
        return LaunchRequest{.kind = LaunchKind::Bios, .path = {}, .label = "PlayStation BIOS"};
    }

    if ((forced_kind == LaunchKind::Disc || forced_kind == LaunchKind::Exe) && !request.path.empty()) {
        request.kind = *forced_kind;
    }

    return request;
}

std::vector<fsui::SettingsChoiceOption> BuildChoices(const std::vector<std::string>& titles, int selected_index) {
    std::vector<fsui::SettingsChoiceOption> options;
    options.reserve(titles.size());

    for (size_t index = 0; index < titles.size(); index++) {
        fsui::SettingsChoiceOption option;
        option.title = titles[index];
        option.selected = (static_cast<int>(index) == selected_index);
        options.push_back(std::move(option));
    }

    return options;
}

std::vector<fsui::SettingsChoiceOption> BuildCpuEngineChoices(psx_cpu_execution_mode_t mode) {
    return BuildChoices(
        {"缓存解释器", "参考解释器"},
        mode == PSX_CPU_INTERPRETER ? 1 : 0
    );
}

#ifdef USE_HARDWARE
std::vector<fsui::SettingsChoiceOption> BuildGpuBackendChoices(GpuBackend current_backend) {
    return BuildChoices(
        {"SDL 软件", "SDL 加速"},
        static_cast<int>(current_backend == GpuBackend::SDLAccelerated ? 1 : 0)
    );
}
#endif

std::vector<fsui::SettingsChoiceOption> BuildAspectChoices(int current) {
    return BuildChoices({"经典 4:3", "方形 1:1", "宽屏 16:9"}, current);
}

std::vector<fsui::SettingsChoiceOption> BuildUpscaleChoices(int current_height) {
    const std::vector<int> heights = {480, 720, 1080, 1440, 2160};
    const std::vector<std::string> labels = {"480p", "720p", "1080p", "1440p", "2160p"};
    int selected = 0;

    for (size_t index = 0; index < heights.size(); index++) {
        if (heights[index] == current_height) {
            selected = static_cast<int>(index);
            break;
        }
    }

    return BuildChoices(labels, selected);
}

std::vector<fsui::SettingsChoiceOption> BuildScaleChoices(int current_scale) {
    const std::vector<int> scales = {1, 2, 3, 4, 5, 6};
    std::vector<std::string> labels;
    labels.reserve(scales.size());

    int selected = 0;
    for (size_t index = 0; index < scales.size(); index++) {
        labels.push_back(std::to_string(scales[index]) + "x");
        if (scales[index] == current_scale) {
            selected = static_cast<int>(index);
        }
    }

    return BuildChoices(labels, selected);
}

std::vector<fsui::SettingsChoiceOption> BuildPresentationUpscaleChoices(int current_scale) {
    std::vector<std::string> labels;
    labels.reserve(8);
    for (int scale = 1; scale <= 8; scale++) {
        labels.push_back(std::to_string(scale) + "x");
    }
    return BuildChoices(labels, std::clamp(current_scale, 1, 8) - 1);
}

std::vector<fsui::SettingsChoiceOption> BuildRegionChoices(const std::string& current_region) {
    const std::vector<std::string> values = {"auto", "ntsc", "pal"};
    const std::vector<std::string> labels = {"自动", "NTSC", "PAL"};
    const std::string current = ToLower(current_region);
    int selected = 0;

    for (size_t index = 0; index < values.size(); index++) {
        if (values[index] == current) {
            selected = static_cast<int>(index);
            break;
        }
    }

    return BuildChoices(labels, selected);
}

std::vector<std::string> ModelChoices() {
    return {
        "scph1000", "scph1001", "scph1002", "scph3000", "scph3500", "scph5000", "scph5500",
        "scph5501", "scph5502", "scph5552", "scph7000", "scph7001", "scph7002", "scph7003",
        "scph7501", "scph7502", "scph9002", "scph100", "scph101", "scph102a", "scph102b", "scph102c",
    };
}

std::vector<fsui::SettingsChoiceOption> BuildModelChoices(const std::string& current_model) {
    const std::vector<std::string> models = ModelChoices();
    const std::string current = NormalizeModel(current_model);
    int selected = 0;

    for (size_t index = 0; index < models.size(); index++) {
        if (NormalizeModel(models[index]) == current) {
            selected = static_cast<int>(index);
            break;
        }
    }

    return BuildChoices(models, selected);
}

std::vector<fsui::SettingsChoiceOption> BuildLogLevelChoices(int current_level) {
    const std::vector<std::string> labels = {"跟踪", "调试", "信息", "警告", "错误", "严重"};
    const int clamped = std::clamp(current_level, static_cast<int>(LOG_TRACE), static_cast<int>(LOG_FATAL));
    return BuildChoices(labels, clamped);
}

std::time_t FileTimeToTimeT(const std::filesystem::file_time_type& value) {
    using namespace std::chrono;
    const auto adjusted = time_point_cast<system_clock::duration>(
        value - std::filesystem::file_time_type::clock::now() + system_clock::now()
    );
    return system_clock::to_time_t(adjusted);
}

void CopyTomlString(const toml_datum_t& datum, std::string& out) {
    if (!datum.ok || !datum.u.s) {
        return;
    }

    out = datum.u.s;
    free(datum.u.s);
}

void CopyTomlPathString(const toml_datum_t& datum, std::string& out) {
    if (!datum.ok || !datum.u.s) {
        return;
    }

    out = NormalizePathString(datum.u.s);
    free(datum.u.s);
}

void CopyTomlArrayStrings(const toml_array_t* array, std::vector<std::filesystem::path>& out) {
    out.clear();

    if (!array) {
        return;
    }

    const int count = toml_array_nelem(array);
    out.reserve(static_cast<size_t>(count));

    for (int index = 0; index < count; index++) {
        toml_datum_t value = toml_string_at(array, index);

        if (!value.ok || !value.u.s) {
            continue;
        }

        out.emplace_back(NormalizePathString(value.u.s));
        free(value.u.s);
    }
}

CliFlags ScanCliFlags(int argc, const char* argv[]) {
    CliFlags flags;

    for (int index = 1; index < argc; index++) {
        const std::string_view arg(argv[index] ? argv[index] : "");

        auto mark_value_opt = [&](const char* short_name, const char* long_name, bool* target) {
            const std::string_view short_opt(short_name);
            const std::string_view long_opt(long_name);

            if (arg == short_opt || arg == long_opt) {
                *target = true;
                index++;
                return true;
            }

            if (arg.starts_with(long_opt) && arg.size() > (long_opt.size() + 1) && arg[long_opt.size()] == '=') {
                *target = true;
                return true;
            }

            return false;
        };

        if (mark_value_opt("-b", "--bios", &flags.bios) ||
            mark_value_opt("-B", "--bios-folder", &flags.bios_folder) ||
            mark_value_opt("-M", "--model", &flags.model) ||
            mark_value_opt("-r", "--region", &flags.region) ||
            mark_value_opt("-s", "--scale", &flags.scale) ||
            mark_value_opt("-S", "--settings-file", &flags.settings_file) ||
            mark_value_opt("-L", "--log-level", &flags.log_level) ||
            mark_value_opt("-e", "--exp-rom", &flags.exp_rom) ||
            mark_value_opt("-x", "--exe", &flags.exe) ||
            mark_value_opt("", "--cpu-engine", &flags.cpu_engine) ||
            mark_value_opt("", "--cdrom", &flags.cdrom)) {
            continue;
        }

        if (arg == "-q" || arg == "--quiet") {
            flags.quiet = true;
            continue;
        }

        if (!arg.empty() && arg[0] != '-') {
            flags.cdrom = true;
        }
    }

    flags.has_boot_request = flags.cdrom || flags.exe;

    return flags;
}

void LoadExtraSettings(FrontendSettings& settings, const CliFlags& cli) {
    if (settings.settings_path.empty()) {
        return;
    }

    FILE* file = fopen(settings.settings_path.c_str(), "rb");

    if (!file) {
        return;
    }

    char error[256] = {};
    toml_table_t* root = toml_parse_file(file, error, sizeof(error));
    fclose(file);

    if (!root) {
        return;
    }

    if (toml_table_t* bios = toml_table_in(root, "bios")) {
        if (!cli.bios_folder) {
            CopyTomlPathString(toml_string_in(bios, "search_path"), settings.bios_search);
        }

        if (!cli.model) {
            CopyTomlString(toml_string_in(bios, "preferred_model"), settings.model);
        }

        if (!cli.bios) {
            CopyTomlPathString(toml_string_in(bios, "override_file"), settings.bios_override);
        }

        toml_datum_t fast_boot = toml_bool_in(bios, "fast_boot");
        if (fast_boot.ok) {
            settings.fast_boot = fast_boot.u.b != 0;
        }
    }

    if (toml_table_t* console = toml_table_in(root, "console")) {
        if (!cli.region) {
            CopyTomlString(toml_string_in(console, "region"), settings.region);
        }
    }

    if (toml_table_t* runtime = toml_table_in(root, "runtime")) {
        if (!cli.scale) {
            toml_datum_t value = toml_int_in(runtime, "display_scale");
            if (value.ok) {
                settings.scale = static_cast<int>(value.u.i);
            }
        }

        if (!cli.log_level) {
            toml_datum_t value = toml_int_in(runtime, "log_level");
            if (value.ok) {
                settings.log_level = static_cast<int>(value.u.i);
            }
        }

        if (!cli.quiet) {
            bool resolved_logging_enabled = false;
            toml_datum_t logging_enabled = toml_bool_in(runtime, "logging_enabled");
            if (logging_enabled.ok) {
                settings.logging_enabled = logging_enabled.u.b != 0;
                settings.quiet = !settings.logging_enabled;
                resolved_logging_enabled = true;
            }

            toml_datum_t value = toml_bool_in(runtime, "quiet");
            if (!resolved_logging_enabled && value.ok) {
                settings.quiet = value.u.b != 0;
                settings.logging_enabled = !settings.quiet;
            }
        }
    }

    if (toml_table_t* paths = toml_table_in(root, "paths")) {
        if (!cli.exp_rom) {
            CopyTomlPathString(toml_string_in(paths, "expansion_rom"), settings.exp_path);
        }

        if (!cli.exe) {
            CopyTomlPathString(toml_string_in(paths, "default_psx_exe"), settings.default_exe_path);
        }
    }

    if (toml_table_t* video = toml_table_in(root, "video")) {
        toml_datum_t vsync = toml_bool_in(video, "vsync");
        if (vsync.ok) {
            settings.vsync_enabled = vsync.u.b != 0;
        }

#ifdef USE_HARDWARE
        toml_datum_t gpu_backend = toml_string_in(video, "gpu_backend");
        if (gpu_backend.ok && gpu_backend.u.s) {
            if (std::optional<GpuBackend> backend = ParseGpuBackendOverride(gpu_backend.u.s)) {
                settings.gpu_backend = *backend;
            } else {
                settings.gpu_backend = GpuBackend::Software;
            }
            free(gpu_backend.u.s);
        }
#endif

        toml_datum_t texture_scale_mode = toml_bool_in(video, "texture_scale_mode");
        if (texture_scale_mode.ok) {
            settings.texture_scale_mode = texture_scale_mode.u.b != 0;
        }

        toml_datum_t presentation_upscale = toml_int_in(video, "presentation_upscale");
        if (presentation_upscale.ok) {
            settings.presentation_upscale = std::clamp(static_cast<int>(presentation_upscale.u.i), 1, 8);
        }

        toml_datum_t debug_panel = toml_bool_in(video, "debug_panel");
        if (debug_panel.ok) {
            settings.debug_panel = debug_panel.u.b != 0;
        }

        toml_datum_t show_fps = toml_bool_in(video, "show_fps");
        if (show_fps.ok) {
            settings.show_fps = show_fps.u.b != 0;
        }

        toml_datum_t stretch_mode = toml_bool_in(video, "stretch_mode");
        if (stretch_mode.ok) {
            settings.stretch_mode = stretch_mode.u.b != 0;
        }

        toml_datum_t display_aspect = toml_string_in(video, "display_aspect");
        if (display_aspect.ok && display_aspect.u.s) {
            const std::string value = ToLower(display_aspect.u.s);
            if (value == "square") {
                settings.display_aspect = 1;
            } else if (value == "wide16x9") {
                settings.display_aspect = 2;
            } else {
                settings.display_aspect = 0;
            }
            free(display_aspect.u.s);
        }

        toml_datum_t wide_upscale = toml_string_in(video, "wide_upscale");
        if (wide_upscale.ok && wide_upscale.u.s) {
            const std::string value = ToLower(wide_upscale.u.s);
            if (value == "720p") settings.upscale_height = 720;
            else if (value == "1080p") settings.upscale_height = 1080;
            else if (value == "1440p") settings.upscale_height = 1440;
            else if (value == "2160p") settings.upscale_height = 2160;
            else settings.upscale_height = 480;
            free(wide_upscale.u.s);
        }
    }

    if (toml_table_t* input = toml_table_in(root, "input")) {
        toml_datum_t mobile_controls = toml_bool_in(input, "mobile_controls");
        if (mobile_controls.ok) {
            settings.mobile_controls = mobile_controls.u.b != 0;
        }
    }

    if (toml_table_t* library = toml_table_in(root, "library")) {
        CopyTomlArrayStrings(toml_array_in(library, "folders"), settings.ui_state.game_list_paths);
        CopyTomlArrayStrings(toml_array_in(library, "recursive_folders"), settings.ui_state.game_list_recursive_paths);
    }

    if (toml_table_t* fsui_state = toml_table_in(root, "fsui")) {
        toml_datum_t default_view = toml_int_in(fsui_state, "default_game_view");
        toml_datum_t sort = toml_int_in(fsui_state, "game_sort");
        toml_datum_t reverse = toml_bool_in(fsui_state, "game_sort_reverse");

        if (default_view.ok) {
            settings.ui_state.default_game_view = static_cast<int>(default_view.u.i);
        }
        if (sort.ok) {
            settings.ui_state.game_sort = static_cast<int>(sort.u.i);
        }
        if (reverse.ok) {
            settings.ui_state.game_sort_reverse = reverse.u.b != 0;
        }
    }

    toml_free(root);
}

FrontendSettings BuildSettings(const psxe_config_t* cfg, const CliFlags& cli) {
    FrontendSettings settings;
#ifdef USE_HARDWARE
    std::optional<GpuBackend> env_gpu_backend;
#endif

    settings.settings_path = (cfg && cfg->settings_path && cfg->settings_path[0]) ? cfg->settings_path : DefaultSettingsPath();
    settings.bios_override = (cfg && cfg->bios && cfg->bios[0]) ? cfg->bios : "";
    settings.bios_search = (cfg && cfg->bios_search && cfg->bios_search[0]) ? cfg->bios_search : "bios";
    settings.model = (cfg && cfg->model && cfg->model[0]) ? cfg->model : "scph1001";
    settings.region = (cfg && cfg->region && cfg->region[0]) ? cfg->region : "auto";
    settings.exp_path = (cfg && cfg->exp_path && cfg->exp_path[0]) ? cfg->exp_path : "";
    settings.default_exe_path = (cfg && cfg->exe && cfg->exe[0]) ? cfg->exe : "";
    settings.scale = cfg ? cfg->scale : 3;
    settings.log_level = cfg ? cfg->log_level : LOG_INFO;
    settings.quiet = cfg ? (cfg->quiet != 0) : true;
    settings.logging_enabled = !settings.quiet;
    settings.vsync_enabled = cfg ? (cfg->vsync_enabled != 0) : DefaultVsyncEnabled();
    settings.cpu_engine = cfg && !cfg->cpu_engine ? PSX_CPU_INTERPRETER : PSX_CPU_CACHED_INTERPRETER;
    if (const char* env_cpu_engine = std::getenv("ARMSX_CPU_ENGINE")) {
        if (std::optional<psx_cpu_execution_mode_t> parsed = ParseCpuEngine(env_cpu_engine)) {
            settings.cpu_engine = *parsed;
        }
    }
#if defined(USE_HARDWARE)
    settings.gpu_backend = cfg && cfg->gpu_backend ? GpuBackend::SDLAccelerated : GpuBackend::Software;
    if (const char* env_backend = std::getenv("ARMSX_GPU_BACKEND")) {
        env_gpu_backend = ParseGpuBackendOverride(env_backend);
    }
#endif
    settings.texture_scale_mode = cfg ? (cfg->texture_scale_mode != 0) : false;
    settings.debug_panel = cfg ? (cfg->debug_panel != 0) : false;
    settings.stretch_mode = cfg ? (cfg->stretch_mode != 0) : false;
    settings.display_aspect = cfg ? cfg->display_aspect : 0;
    settings.upscale_height = cfg ? cfg->upscale_height : 480;
    settings.presentation_upscale = 1;
    settings.mobile_controls = DefaultMobileControlsEnabled();
    settings.ui_state.show_settings_overlay = settings.debug_panel;
    settings.ui_state.show_performance_overlay = settings.show_fps;

    if (!cli.bios && !settings.bios_override.empty()) {
        const std::filesystem::path bios_path(settings.bios_override);
        if (!std::filesystem::exists(bios_path) && (settings.bios_override == "bios.bin")) {
            settings.bios_override.clear();
        }
    }

    LoadExtraSettings(settings, cli);

#if defined(USE_HARDWARE)
    if (env_gpu_backend) {
        settings.gpu_backend = *env_gpu_backend;
    }
#endif
    if (const char* env_upscale = std::getenv("ARMSX_PRESENTATION_UPSCALE")) {
        char* end = nullptr;
        const long scale = std::strtol(env_upscale, &end, 10);
        if (end && *end == '\0') {
            settings.presentation_upscale = std::clamp(static_cast<int>(scale), 1, 8);
        }
    }
    if (const char* env_mobile_controls = std::getenv("ARMSX_MOBILE_CONTROLS")) {
        const std::string value = ToLower(env_mobile_controls);
        settings.mobile_controls = value == "1" || value == "true" || value == "on" || value == "yes";
    }

    settings.scale = std::max(1, settings.scale);
    settings.presentation_upscale = std::clamp(settings.presentation_upscale, 1, 8);
    settings.log_level = std::clamp(settings.log_level, static_cast<int>(LOG_TRACE), static_cast<int>(LOG_FATAL));
    settings.logging_enabled = !settings.quiet;
    settings.ui_state.show_settings_overlay = settings.debug_panel;
    settings.ui_state.show_performance_overlay = settings.show_fps;

    if (settings.ui_state.covers_path.empty()) {
        settings.ui_state.covers_path = DefaultBrowseDirectory() / "covers";
    }

    return settings;
}

bool SaveSettings(const FrontendSettings& settings) {
    const std::filesystem::path target = settings.settings_path.empty() ? std::filesystem::path(DefaultSettingsPath()) : std::filesystem::path(settings.settings_path);
    std::filesystem::path temp = target;
    temp += ".tmp";

    try {
        if (target.has_parent_path()) {
            std::filesystem::create_directories(target.parent_path());
        }
    } catch (...) {
        return false;
    }

    std::ofstream out(temp, std::ios::binary | std::ios::trunc);

    if (!out.is_open()) {
        return false;
    }

    auto write_path_array = [&](const std::vector<std::filesystem::path>& paths) {
        out << "[";
        for (size_t index = 0; index < paths.size(); index++) {
            if (index) {
                out << ", ";
            }
            out << "\"" << EscapeTomlString(paths[index].string()) << "\"";
        }
        out << "]";
    };

    out
        << "# Settings file generated by ARMSX\n\n"
        << "psxe_version = \"" << STR(REP_VERSION) << "\"\n\n"
        << "[bios]\n"
        << "    search_path     = \"" << EscapeTomlString(settings.bios_search) << "\"\n"
        << "    preferred_model = \"" << EscapeTomlString(settings.model) << "\"\n"
        << "    override_file   = \"" << EscapeTomlString(settings.bios_override) << "\"\n"
        << "    fast_boot       = " << (settings.fast_boot ? "true" : "false") << "\n\n"
        << "[console]\n"
        << "    region          = \"" << EscapeTomlString(settings.region) << "\"\n\n"
        << "[cpu]\n"
        << "    execution_mode  = \"" << CpuEngineSettingToken(settings.cpu_engine) << "\"\n\n"
        << "[runtime]\n"
        << "    display_scale = " << settings.scale << "\n"
        << "    logging_enabled = " << (settings.logging_enabled ? "true" : "false") << "\n"
        << "    log_level = " << settings.log_level << "\n"
        << "    quiet = " << (settings.logging_enabled ? "false" : "true") << "\n\n"
        << "[paths]\n"
        << "    expansion_rom = \"" << EscapeTomlString(settings.exp_path) << "\"\n"
        << "    default_psx_exe = \"" << EscapeTomlString(settings.default_exe_path) << "\"\n\n"
        << "[video]\n"
        << "    vsync = " << (settings.vsync_enabled ? "true" : "false") << "\n"
#ifdef USE_HARDWARE
        << "    gpu_backend = \"" << GpuBackendSettingToken(settings.gpu_backend) << "\"\n"
#endif
        << "    texture_scale_mode = " << (settings.texture_scale_mode ? "true" : "false") << "\n"
        << "    presentation_upscale = " << settings.presentation_upscale << "\n"
        << "    debug_panel = " << (settings.debug_panel ? "true" : "false") << "\n"
        << "    show_fps = " << (settings.show_fps ? "true" : "false") << "\n"
        << "    stretch_mode = " << (settings.stretch_mode ? "true" : "false") << "\n"
        << "    display_aspect = \"" << AspectToString(settings.display_aspect) << "\"\n"
        << "    wide_upscale = \"" << UpscaleToString(settings.upscale_height) << "\"\n\n"
        << "[input]\n"
        << "    mobile_controls = " << (settings.mobile_controls ? "true" : "false") << "\n\n"
        << "[library]\n"
        << "    folders = ";
    write_path_array(settings.ui_state.game_list_paths);
    out << "\n    recursive_folders = ";
    write_path_array(settings.ui_state.game_list_recursive_paths);
    out
        << "\n\n[fsui]\n"
        << "    default_game_view = " << settings.ui_state.default_game_view << "\n"
        << "    game_sort = " << settings.ui_state.game_sort << "\n"
        << "    game_sort_reverse = " << (settings.ui_state.game_sort_reverse ? "true" : "false") << "\n";

    out.flush();
    if (!out.good()) {
        std::error_code ec;
        std::filesystem::remove(temp, ec);
        return false;
    }

    out.close();
    if (!out.good()) {
        std::error_code ec;
        std::filesystem::remove(temp, ec);
        return false;
    }

#if defined(_WIN32)
    if (MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
        std::error_code ec;
        std::filesystem::remove(temp, ec);
        return false;
    }
#else
    std::error_code ec;
    std::filesystem::rename(temp, target, ec);
    if (ec) {
        std::filesystem::remove(temp, ec);
        return false;
    }
#endif

    return true;
}

void MixPsxAudio(psx_t* psx, uint8_t* buffer, int size) {
    if (!psx || !buffer || size <= 0) {
        return;
    }

    psx_cdrom_t* cdrom = psx->cdrom;
    psx_spu_t* spu = psx->spu;

    std::memset(buffer, 0, static_cast<size_t>(size));

    psx_cdrom_get_audio_samples(cdrom, buffer, size);
    psx_spu_update_cdda_buffer(spu, cdrom->cdda_buf);

    for (int sample = 0; sample < (size >> 2); sample++) {
        const uint32_t value = psx_spu_get_sample(spu);
        const int16_t left = static_cast<int16_t>(value & 0xffff);
        const int16_t right = static_cast<int16_t>(value >> 16);

        *reinterpret_cast<int16_t*>(&buffer[(sample << 2) + 0]) += left;
        *reinterpret_cast<int16_t*>(&buffer[(sample << 2) + 2]) += right;
    }
}

class ArmsxSession {
  public:
    ~ArmsxSession() {
        destroy();
    }

    bool create(SDL_Renderer* renderer, const FrontendSettings& settings, const LaunchRequest& request, std::string& error) {
        destroy();

        if (!renderer) {
            error = "显示系统尚未就绪。";
            return false;
        }

        renderer_ = renderer;
        vblank_counter_ = 0;

        psxe_config_t cfg{};
        cfg.bios = settings.bios_override.empty() ? nullptr : settings.bios_override.c_str();
        cfg.bios_search = settings.bios_search.c_str();
        cfg.model = settings.model.c_str();

        char* bios_path = psxe_cfg_get_bios_path(&cfg);
        std::unique_ptr<char, decltype(&free)> bios_guard(bios_path, &free);

        if (!bios_path || !bios_path[0]) {
            error = "未找到可用的 BIOS。请指定 BIOS 文件，或选择包含所选型号 BIOS 的文件夹。";
            return false;
        }

        {
            std::error_code ec;
            if (!std::filesystem::is_regular_file(std::filesystem::path(bios_path), ec)) {
                error = std::string("BIOS 文件不存在或无法访问：") + bios_path;
                psxe_diag_logf("launch", "Resolved BIOS is unavailable: %s", bios_path);
                return false;
            }
        }

        const char* expansion = settings.exp_path.empty() ? nullptr : settings.exp_path.c_str();
        if (expansion) {
            std::error_code ec;
            if (!std::filesystem::is_regular_file(std::filesystem::path(expansion), ec)) {
                error = std::string("扩展 ROM 文件不存在或无法访问：") + expansion;
                psxe_diag_logf("launch", "Expansion ROM is unavailable: %s", expansion);
                return false;
            }
        }

        psx_ = psx_create();
        if (!psx_) {
            error = "无法创建 PSX 会话。";
            return false;
        }

        const int init_result = psx_init(psx_, bios_path, expansion);

        if (init_result != 0) {
            error = "模拟器核心初始化失败。请检查 BIOS 和扩展 ROM 路径。";
            destroy();
            return false;
        }

        psx_cpu_set_execution_mode(psx_get_cpu(psx_), settings.cpu_engine);
        psx_fastboot_set_enabled(request.kind == LaunchKind::Disc && settings.fast_boot);
        if (request.kind == LaunchKind::Disc && settings.fast_boot) {
            const bool patched = psx_fastboot_patch_bios(psx_get_bios(psx_)) != 0;
            psxe_diag_logf("fastboot", "BIOS shell patch result=%s", patched ? "applied" : "unsupported-fallback");
        }
        psxe_diag_logf("cpu", "execution engine=%s", CpuEngineSettingToken(settings.cpu_engine));

        psx_gpu_t* gpu = psx_get_gpu(psx_);
        psx_gpu_set_event_callback(gpu, GPU_EVENT_DMODE, nullptr);
        psx_gpu_set_event_callback(gpu, GPU_EVENT_VBLANK, SessionVblankEvent);
        psx_gpu_set_event_callback(gpu, GPU_EVENT_HBLANK, psxe_gpu_hblank_event_cb);
        psx_gpu_set_event_callback(gpu, GPU_EVENT_VBLANK_END, psxe_gpu_vblank_end_event_cb);
        psx_gpu_set_event_callback(gpu, GPU_EVENT_HBLANK_END, psxe_gpu_hblank_end_event_cb);
        psx_gpu_set_udata(gpu, 0, this);
        psx_gpu_set_udata(gpu, 1, psx_->timer);
        psx_gpu_set_udata(gpu, 2, nullptr);

#ifdef USE_HARDWARE
        hardware_backend_active_ = settings.gpu_backend == GpuBackend::SDLAccelerated;
        if (hardware_backend_active_) {
            SDL_RendererInfo renderer_info{};
            if (SDL_GetRendererInfo(renderer_, &renderer_info) != 0 ||
                !(renderer_info.flags & SDL_RENDERER_ACCELERATED)) {
                psxe_diag_logf("renderer", "SDL acceleration requested but unavailable; using SDL software presentation.");
                hardware_backend_active_ = false;
            } else {
                psxe_diag_logf(
                    "renderer",
                    "SDL accelerated presentation enabled driver=%s; PlayStation VRAM remains software-authoritative.",
                    renderer_info.name ? renderer_info.name : "(unknown)"
                );
            }
        }
#endif

        psx_cdrom_set_region(psx_get_cdrom(psx_), RegionFromSettings(settings));

        input_ = psx_input_create();
        if (input_) {
            psx_input_init(input_);
        }
        pad_device_ = psxi_sda_create();

        if (!input_ || !pad_device_) {
            error = "无法创建控制器输入桥接。";
            destroy();
            return false;
        }

        psxi_sda_init(pad_device_, SDA_MODEL_DIGITAL);
        psxi_sda_init_input(pad_device_, input_);
        psx_pad_attach_joy(psx_->pad, 0, input_);
        pad_device_ = nullptr;

        const std::string slot1 = std::string(psxe_cfg_get_pref_path() ? psxe_cfg_get_pref_path() : "") + "slot1.mcd";
        const std::string slot2 = std::string(psxe_cfg_get_pref_path() ? psxe_cfg_get_pref_path() : "") + "slot2.mcd";
        psx_pad_attach_mcd(psx_->pad, 0, slot1.c_str());
        psx_pad_attach_mcd(psx_->pad, 1, slot2.c_str());

        switch (request.kind) {
            case LaunchKind::Disc:
                if (psx_cdrom_open(psx_get_cdrom(psx_), request.path.string().c_str()) == 0) {
                    error = "无法打开所选光盘镜像。";
                    destroy();
                    return false;
                }
                disc_path_ = request.path;
                exe_path_.clear();
                title_ = request.label.empty() ? StemToTitle(request.path) : request.label;

                break;

            case LaunchKind::Exe:
                while (psx_->cpu->pc != 0x80030000) {
                    psx_update(psx_);
                }
                psx_load_exe(psx_, request.path.string().c_str());
                exe_path_ = request.path;
                disc_path_.clear();
                title_ = request.label.empty() ? StemToTitle(request.path) : request.label;
                break;

            case LaunchKind::Bios:
                disc_path_.clear();
                exe_path_.clear();
                title_ = "PlayStation BIOS";
                break;

            case LaunchKind::None:
            default:
                error = "启动请求无效。";
                destroy();
                return false;
        }

        launch_kind_ = request.kind;
#if defined(HW_DEBUG)
#ifdef USE_HARDWARE
        if (hardware_backend_active_ && psx_ && psx_->gpu) {
            int output_width = 0;
            int output_height = 0;
            SDL_GetRendererOutputSize(renderer_, &output_width, &output_height);
            psxe_diag_logf(
                "hw",
                "session-create kind=%s title=%s renderer=%p output=%dx%d texture=%dx%d timing=%s target_fps=%.3f display_mode=0x%08x gpustat=0x%08x",
                LaunchKindTitle(request.kind),
                title_.c_str(),
                (void*)renderer_,
                output_width,
                output_height,
                texture_width_,
                texture_height_,
                timingModeTitle(),
                targetFrameRate(),
                psx_->gpu->display_mode,
                psx_->gpu->gpustat
            );
        }
#endif
#endif
        psxe_diag_breadcrumbf(
            "Session created kind=%s title=%s path=%s bios=%s model=%s region=%s",
            LaunchKindTitle(request.kind),
            title_.c_str(),
            request.path.empty() ? "(none)" : request.path.string().c_str(),
            bios_path,
            settings.model.c_str(),
            settings.region.c_str()
        );

        SDL_AudioSpec desired{};
        SDL_AudioSpec obtained{};
        desired.freq = 44100;
        desired.format = AUDIO_S16SYS;
        desired.channels = 2;
        desired.samples = 1024;
        desired.callback = nullptr;
        desired.userdata = nullptr;

        audio_dev_ = SDL_OpenAudioDevice(nullptr, 0, &desired, &obtained, 0);
        paused_ = false;
        fast_forward_enabled_ = false;
        debug_view_ = false;
        texture_width_ = 0;
        texture_height_ = 0;
        texture_format_ = SDL_PIXELFORMAT_UNKNOWN;
        if (audio_dev_) {
            audio_sample_accumulator_ = 0.0;
            audio_playback_started_ = false;
            psxe_diag_logf(
                "audio",
                "SDL queued audio opened frequency=%d channels=%u samples=%u prime_bytes=%zu max_bytes=%zu",
                obtained.freq,
                static_cast<unsigned>(obtained.channels),
                static_cast<unsigned>(obtained.samples),
                kAudioPrimeBytes,
                kMaxQueuedAudioBytes
            );
            updateAudioPlaybackState();
        }
        updateTexture(settings);

        return true;
    }

    void destroy() {
        if (psx_) {
            psxe_diag_breadcrumbf(
                "Session destroyed kind=%s title=%s path=%s",
                LaunchKindTitle(launch_kind_),
                title_.empty() ? "(none)" : title_.c_str(),
                disc_path_.empty() ? (exe_path_.empty() ? "(none)" : exe_path_.string().c_str()) : disc_path_.string().c_str()
            );
        }

        if (audio_dev_) {
            SDL_PauseAudioDevice(audio_dev_, 1);
            SDL_ClearQueuedAudio(audio_dev_);
            SDL_CloseAudioDevice(audio_dev_);
            audio_dev_ = 0;
        }

        if (texture_) {
            SDL_DestroyTexture(texture_);
            texture_ = nullptr;
        }
        destroyPresentationTexture();
        texture_snapshot_.clear();

#ifdef USE_HARDWARE
        if (hw_renderer_) {
            armsx_hw_renderer_destroy(hw_renderer_);
            hw_renderer_ = nullptr;
        }
#endif

        const bool input_attached = psx_ && psx_->pad && (psx_->pad->joy_slot[0] == input_);

        if (psx_) {
            psx_destroy(psx_);
            psx_ = nullptr;
        }

        if (input_ && !input_attached) {
            psx_input_destroy(input_);
        }

        if (pad_device_) {
            psxi_sda_destroy(pad_device_);
            pad_device_ = nullptr;
        }

        input_ = nullptr;
        renderer_ = nullptr;
        disc_path_.clear();
        exe_path_.clear();
        title_.clear();
        launch_kind_ = LaunchKind::None;
        paused_ = false;
        fast_forward_enabled_ = false;
        debug_view_ = false;
        vblank_counter_ = 0;
#ifdef USE_HARDWARE
        hardware_backend_active_ = false;
#endif
    }

    bool valid() const {
        return psx_ != nullptr;
    }

    psx_t* psx() const {
        return psx_;
    }

    psx_pad_t* pad() const {
        return psx_ ? psx_->pad : nullptr;
    }

    void rebindRenderer(SDL_Renderer* renderer, const FrontendSettings& settings) {
        renderer_ = renderer;
#ifdef USE_HARDWARE
        if (hw_renderer_) {
            armsx_hw_renderer_set_renderer(hw_renderer_, renderer_);
            if (texture_width_ > 0 && texture_height_ > 0) {
                armsx_hw_renderer_set_output_size(hw_renderer_, texture_width_, texture_height_);
            }
        }
#endif
        if (texture_) {
            SDL_DestroyTexture(texture_);
            texture_ = nullptr;
        }
        destroyPresentationTexture();
        texture_snapshot_.clear();
        texture_width_ = 0;
        texture_height_ = 0;
        texture_format_ = SDL_PIXELFORMAT_UNKNOWN;
        updateTexture(settings);
    }

    void setPaused(bool paused) {
        paused_ = paused;
        if (audio_dev_) {
            if (paused || fast_forward_enabled_) {
                clearQueuedAudio();
            }
            updateAudioPlaybackState();
        }
    }

    bool paused() const {
        return paused_;
    }

    void setFastForwardEnabled(bool enabled) {
        if (fast_forward_enabled_ == enabled) {
            return;
        }

        fast_forward_enabled_ = enabled;
        clearQueuedAudio();

        if (audio_dev_) {
            updateAudioPlaybackState();
        }
    }

    bool fastForwardEnabled() const {
        return fast_forward_enabled_;
    }

    std::uint32_t runFrame() {
        if (!psx_ || paused_) {
            return 0;
        }

#if defined(HW_DEBUG)
        traceHardwareFrameState("frame-start", 0);
#endif

#ifdef USE_HARDWARE
        if (hardware_backend_active_ && hw_renderer_) {
            armsx_hw_renderer_begin_frame(hw_renderer_);
        }
#endif

        const std::uint64_t start_vblank = vblank_counter_;
        std::uint32_t steps = 0;

        while (vblank_counter_ == start_vblank) {
            psx_update(psx_);
            steps++;

            if (steps >= kMaxFrameSteps) {
                psxe_diag_logf("timing", "Session frame advance exceeded vblank step budget title=%s", title_.empty() ? "(none)" : title_.c_str());
#if defined(HW_DEBUG)
                traceHardwareFrameState("frame-budget-exceeded", steps);
#endif
                return steps;
            }
        }

        queueAudioForFrame();

#if defined(HW_DEBUG)
        traceHardwareFrameState("frame-end", steps);
#endif
        return steps;
    }

    void setDebugView(bool enabled) {
        debug_view_ = enabled;
    }

    bool debugView() const {
        return debug_view_;
    }

    LaunchRequest currentLaunchRequest() const {
        LaunchRequest request;
        request.kind = launch_kind_;
        request.label = title_;

        if (launch_kind_ == LaunchKind::Disc) {
            request.path = disc_path_;
        } else if (launch_kind_ == LaunchKind::Exe) {
            request.path = exe_path_;
        }

        return request;
    }

    bool swapDisc(const std::filesystem::path& path) {
        if (!psx_) {
            return false;
        }

        if (psx_swap_disc(psx_, path.string().c_str()) == 0) {
            return false;
        }

        setFastForwardEnabled(false);
        disc_path_ = path;
        launch_kind_ = LaunchKind::Disc;
        title_ = StemToTitle(path);
        psxe_diag_breadcrumbf("Disc swapped path=%s", path.string().c_str());
        return true;
    }

    fsui::CurrentGameInfo currentGameInfo() const {
        fsui::CurrentGameInfo info;

        if (!psx_) {
            return info;
        }

        info.has_game = true;
        info.title = title_.empty() ? "ARMSX" : title_;
        info.subtitle = launch_kind_ == LaunchKind::Bios ? "未插入光盘" : (disc_path_.empty() ? exe_path_.filename().string() : disc_path_.filename().string());
        info.title_id = NormalizeModel(info.title);
        info.path = launch_kind_ == LaunchKind::Bios ? std::filesystem::path("bios://boot") : (disc_path_.empty() ? exe_path_ : disc_path_);
        return info;
    }

    std::vector<fsui::OverlayTextLine> settingsOverlayLines(const FrontendSettings& settings) const {
        std::vector<fsui::OverlayTextLine> lines;
        lines.push_back(fsui::OverlayTextLine{.text = std::string("宽高比：") + AspectTitle(settings.display_aspect)});
        lines.push_back(fsui::OverlayTextLine{.text = std::string("画面：") + RendererValueTitle(settings)});
        lines.push_back(fsui::OverlayTextLine{.text = std::string("VSync: ") + BoolTitle(settings.vsync_enabled)});
        lines.push_back(fsui::OverlayTextLine{.text = std::string("调试视图：") + BoolTitle(debug_view_)});
        lines.push_back(fsui::OverlayTextLine{.text = std::string("快进：") + (fast_forward_enabled_ ? "2x" : "关")});
        return lines;
    }

    std::vector<fsui::OverlayTextLine> performanceOverlayLines(const FrontendSettings& settings) const {
        (void)settings;
        std::vector<fsui::OverlayTextLine> lines;
        char fps[64] = {};
        std::snprintf(fps, sizeof(fps), "FPS: %.1f", ImGui::GetIO().Framerate);
        lines.push_back(fsui::OverlayTextLine{.text = fps});
        return lines;
    }

    void updateTexture(const FrontendSettings& settings) {
        if (!psx_ || !renderer_) {
            return;
        }

        int next_width = debug_view_ ? PSX_GPU_FB_WIDTH : static_cast<int>(psx_get_display_width(psx_));
        int next_height = debug_view_ ? PSX_GPU_FB_HEIGHT : static_cast<int>(psx_get_display_height(psx_));
        if (next_width <= 0) {
            next_width = texture_width_ > 0 ? texture_width_ : 320;
        }
        if (next_height <= 0) {
            next_height = texture_height_ > 0 ? texture_height_ : 240;
        }
        const Uint32 next_format = debug_view_ || !psx_get_display_format(psx_) ? SDL_PIXELFORMAT_BGR555 : SDL_PIXELFORMAT_RGB24;
        const bool use_vram_source = debug_view_ || !psx_ || !psx_->gpu ? false : ((psx_->gpu->disp_y + next_height) > PSX_GPU_FB_HEIGHT);

#if defined(USE_HARDWARE) && defined(HW_DEBUG)
        if (hardware_backend_active_) {
            int output_width = 0;
            int output_height = 0;
            SDL_GetRendererOutputSize(renderer_, &output_width, &output_height);
            psxe_diag_logf(
                "hw",
                "texture-select frame=%llu display_mode=0x%08x gpustat=0x%08x display_enable=%d dmode=%ux%u display=%ux%u aspect=%.3f disp_window=(%u,%u)-(%u,%u) disp_y=%u offset=(%d,%d) output=%dx%d next=%dx%d format=%s source=%s stretch=%s debug=%s timing=%s target_fps=%.3f",
                static_cast<unsigned long long>(vblank_counter_),
                psx_ && psx_->gpu ? psx_->gpu->display_mode : 0u,
                psx_ && psx_->gpu ? psx_->gpu->gpustat : 0u,
                psx_ && psx_->gpu ? psx_->gpu->display_enable : 0,
                psx_ ? psx_get_dmode_width(psx_) : 0u,
                psx_ ? psx_get_dmode_height(psx_) : 0u,
                psx_ ? psx_get_display_width(psx_) : 0u,
                psx_ ? psx_get_display_height(psx_) : 0u,
                psx_ ? psx_get_display_aspect(psx_) : 0.0,
                psx_ && psx_->gpu ? psx_->gpu->disp_x1 : 0u,
                psx_ && psx_->gpu ? psx_->gpu->disp_y1 : 0u,
                psx_ && psx_->gpu ? psx_->gpu->disp_x2 : 0u,
                psx_ && psx_->gpu ? psx_->gpu->disp_y2 : 0u,
                psx_ && psx_->gpu ? psx_->gpu->disp_y : 0u,
                psx_ && psx_->gpu ? psx_->gpu->off_x : 0,
                psx_ && psx_->gpu ? psx_->gpu->off_y : 0,
                output_width,
                output_height,
                next_width,
                next_height,
                SDL_GetPixelFormatName(next_format),
                use_vram_source ? "vram" : "display",
                settings.stretch_mode ? "true" : "false",
                debug_view_ ? "true" : "false",
                timingModeTitle(),
                targetFrameRate()
            );
        }
#endif

        if ((next_width != texture_width_) || (next_height != texture_height_) || (next_format != texture_format_) || !texture_) {
            if (texture_) {
                SDL_DestroyTexture(texture_);
                texture_ = nullptr;
            }

            texture_ = SDL_CreateTexture(renderer_, next_format, SDL_TEXTUREACCESS_STREAMING, next_width, next_height);
            texture_width_ = next_width;
            texture_height_ = next_height;
            texture_format_ = next_format;
            texture_snapshot_.clear();

#ifdef USE_HARDWARE
            if (hw_renderer_) {
                armsx_hw_renderer_set_output_size(hw_renderer_, texture_width_, texture_height_);
            }
#endif
        }

        if (!texture_) {
            return;
        }

        if (settings.texture_scale_mode) {
#if SDL_VERSION_ATLEAST(2, 0, 12)
            SDL_SetTextureScaleMode(texture_, SDL_ScaleModeLinear);
#endif
        } else {
#if SDL_VERSION_ATLEAST(2, 0, 12)
            SDL_SetTextureScaleMode(texture_, SDL_ScaleModeNearest);
#endif
        }

        void* display_buffer = use_vram_source ? psx_get_vram(psx_) : psx_get_display_buffer(psx_);
        const int bytes_per_pixel = SDL_BYTESPERPIXEL(texture_format_);
        const size_t row_bytes = static_cast<size_t>(texture_width_) * static_cast<size_t>(bytes_per_pixel);
        const size_t snapshot_size = static_cast<size_t>(PSX_GPU_FB_STRIDE) * static_cast<size_t>(texture_height_);
        const auto* source = static_cast<const uint8_t*>(display_buffer);

        int first_dirty_row = 0;
        int last_dirty_row = texture_height_ - 1;
        if (texture_snapshot_.size() == snapshot_size) {
            first_dirty_row = texture_height_;
            last_dirty_row = -1;
            for (int row = 0; row < texture_height_; ++row) {
                const size_t offset = static_cast<size_t>(row) * static_cast<size_t>(PSX_GPU_FB_STRIDE);
                if (std::memcmp(source + offset, texture_snapshot_.data() + offset, row_bytes) != 0) {
                    first_dirty_row = std::min(first_dirty_row, row);
                    last_dirty_row = row;
                }
            }
        } else {
            texture_snapshot_.assign(snapshot_size, 0);
        }

        if (last_dirty_row >= first_dirty_row) {
            SDL_Rect dirty_rect{
                0,
                first_dirty_row,
                texture_width_,
                last_dirty_row - first_dirty_row + 1,
            };
            const size_t first_offset = static_cast<size_t>(first_dirty_row) * static_cast<size_t>(PSX_GPU_FB_STRIDE);
            SDL_UpdateTexture(texture_, &dirty_rect, source + first_offset, PSX_GPU_FB_STRIDE);
            for (int row = first_dirty_row; row <= last_dirty_row; ++row) {
                const size_t offset = static_cast<size_t>(row) * static_cast<size_t>(PSX_GPU_FB_STRIDE);
                std::memcpy(texture_snapshot_.data() + offset, source + offset, row_bytes);
            }
        }

        updatePresentationTexture(settings);
    }

    void draw(const FrontendSettings& settings) {
        if (!texture_) {
            return;
        }

        const ImVec2 display = ImGui::GetIO().DisplaySize;
        const ImVec2 framebuffer_scale = ImGui::GetIO().DisplayFramebufferScale;
        const float display_width = std::max(display.x, 1.0f);
        const float display_height = std::max(display.y, 1.0f);

        float aspect = 4.0f / 3.0f;
        if (debug_view_) {
            aspect = static_cast<float>(texture_width_) / static_cast<float>(std::max(texture_height_, 1));
        } else if (settings.display_aspect == 2) {
            aspect = 16.0f / 9.0f;
        } else if (settings.display_aspect == 1) {
            aspect = 1.0f;
        } else {
            aspect = static_cast<float>(psx_get_display_aspect(psx_));
        }

        float target_width = display_width;
        float target_height = display_height;
        float offset_x = 0.0f;
        float offset_y = 0.0f;

        if (!settings.stretch_mode) {
            target_width = display_width;
            target_height = target_width / aspect;

            if (target_height > display_height) {
                target_height = display_height;
                target_width = target_height * aspect;
            }

            offset_x = (display_width - target_width) * 0.5f;
            offset_y = (display_height - target_height) * 0.5f;
        }

        ImDrawList* draw_list = ImGui::GetBackgroundDrawList();
#if defined(USE_HARDWARE) && defined(HW_DEBUG)
        psxe_diag_logf(
            "ui",
            "session-draw frame=%llu display=(%.1f,%.1f) fb_scale=(%.2f,%.2f) texture=%dx%d aspect=%.3f stretch=%s debug=%s target=(%.1f,%.1f) offset=(%.1f,%.1f) hardware=%s timing=%s",
            static_cast<unsigned long long>(vblank_counter_),
            display.x,
            display.y,
            framebuffer_scale.x,
            framebuffer_scale.y,
            texture_width_,
            texture_height_,
            aspect,
            settings.stretch_mode ? "true" : "false",
            debug_view_ ? "true" : "false",
            target_width,
            target_height,
            offset_x,
            offset_y,
#ifdef USE_HARDWARE
            hardware_backend_active_ ? "true" : "false",
#else
            "false",
#endif
            timingModeTitle()
        );
#endif
        draw_list->AddRectFilled(ImVec2(0.0f, 0.0f), display, IM_COL32(0, 0, 0, 255));
        SDL_Texture* presentation = presentation_texture_ ? presentation_texture_ : texture_;
        draw_list->AddImage(
            reinterpret_cast<ImTextureID>(presentation),
            ImVec2(offset_x, offset_y),
            ImVec2(offset_x + target_width, offset_y + target_height)
        );

#ifdef USE_HARDWARE
        if (!debug_view_ && hw_renderer_) {
            if (SDL_Texture* overlay = armsx_hw_renderer_overlay_texture(hw_renderer_)) {
                draw_list->AddImage(
                    reinterpret_cast<ImTextureID>(overlay),
                    ImVec2(offset_x, offset_y),
                    ImVec2(offset_x + target_width, offset_y + target_height)
                );
            }
        }
#endif
    }

    void finishHardwareFrame() {
#ifdef USE_HARDWARE
        if (hardware_backend_active_ && hw_renderer_) {
            armsx_hw_renderer_end_frame(hw_renderer_);
        }
#endif
    }

#if defined(HW_DEBUG)
    void traceHardwareFrameState(const char* stage, std::uint32_t steps) const {
#ifdef USE_HARDWARE
        if (!hardware_backend_active_ || !psx_ || !psx_->gpu) {
            return;
        }

        const psx_gpu_t* gpu = psx_->gpu;
        psxe_diag_logf(
            "hw",
            "%s frame=%llu steps=%u timing=%s target_fps=%.3f display_mode=0x%08x gpustat=0x%08x display_enable=%d dmode=%ux%u display=%ux%u draw=(%u,%u)-(%u,%u) disp=(%u,%u)-(%u,%u) offset=(%d,%d) texture=%dx%d renderer=%p",
            stage ? stage : "frame-state",
            static_cast<unsigned long long>(vblank_counter_),
            steps,
            timingModeTitle(),
            targetFrameRate(),
            gpu->display_mode,
            gpu->gpustat,
            (gpu->gpustat & 0x00800000) != 0,
            psx_get_dmode_width(psx_),
            psx_get_dmode_height(psx_),
            psx_get_display_width(psx_),
            psx_get_display_height(psx_),
            gpu->draw_x1,
            gpu->draw_y1,
            gpu->draw_x2,
            gpu->draw_y2,
            gpu->disp_x1,
            gpu->disp_y1,
            gpu->disp_x2,
            gpu->disp_y2,
            gpu->off_x,
            gpu->off_y,
            texture_width_,
            texture_height_,
            (void*)renderer_
        );
#else
        (void)stage;
        (void)steps;
#endif
    }
#endif

    bool saveScreenshot(const std::filesystem::path& path) {
        if (!psx_) {
            return false;
        }

        const int width = texture_width_;
        const int height = texture_height_;
        const Uint32 format = texture_format_;
        void* source = debug_view_ ? psx_get_vram(psx_) : psx_get_display_buffer(psx_);

        if (!debug_view_ && ((psx_->gpu->disp_y + texture_height_) > PSX_GPU_FB_HEIGHT)) {
            source = psx_get_vram(psx_);
        }

        SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(0, width, height, SDL_BITSPERPIXEL(format), format);

        if (!surface) {
            return false;
        }

        const size_t row_size = static_cast<size_t>(surface->pitch);
        const uint8_t* src = static_cast<const uint8_t*>(source);
        uint8_t* dst = static_cast<uint8_t*>(surface->pixels);

        for (int row = 0; row < height; row++) {
            std::memcpy(dst + (static_cast<size_t>(row) * static_cast<size_t>(surface->pitch)), src + (static_cast<size_t>(row) * static_cast<size_t>(PSX_GPU_FB_STRIDE)), row_size);
        }

        const int result = SDL_SaveBMP(surface, path.string().c_str());
        SDL_FreeSurface(surface);
        return result == 0;
    }

    double frameRate() const {
        if (!psx_ || !psx_->gpu) {
            return 59.29;
        }

        return static_cast<double>(psx_gpu_frame_rate(psx_->gpu));
    }

    double targetFrameRate() const {
        return frameRate() * (fast_forward_enabled_ ? 2.0 : 1.0);
    }

    std::uint64_t vblankCounter() const {
        return vblank_counter_;
    }

#ifdef USE_HARDWARE
    bool hardwareBackendActive() const {
        return hardware_backend_active_;
    }

    int textureWidth() const {
        return texture_width_;
    }

    int textureHeight() const {
        return texture_height_;
    }

    Uint32 textureFormat() const {
        return texture_format_;
    }
#endif

    const char* timingModeTitle() const {
        if (!psx_ || !psx_->gpu) {
            return "NTSC-like";
        }

        return psx_gpu_is_pal_mode(psx_->gpu) ? "PAL-like" : "NTSC-like";
    }

  private:
    void destroyPresentationTexture() {
        if (presentation_texture_) {
            SDL_DestroyTexture(presentation_texture_);
            presentation_texture_ = nullptr;
        }
        presentation_width_ = 0;
        presentation_height_ = 0;
        presentation_scale_ = 1;
    }

    void updatePresentationTexture(const FrontendSettings& settings) {
        const int requested_scale = std::clamp(settings.presentation_upscale, 1, 8);
        if (!renderer_ || !texture_ || requested_scale == 1) {
            destroyPresentationTexture();
            return;
        }

        SDL_RendererInfo info{};
        if (SDL_GetRendererInfo(renderer_, &info) != 0 || (info.flags & SDL_RENDERER_TARGETTEXTURE) == 0) {
            destroyPresentationTexture();
            return;
        }

        int actual_scale = requested_scale;
        if (info.max_texture_width > 0) {
            actual_scale = std::min(actual_scale, info.max_texture_width / std::max(texture_width_, 1));
        }
        if (info.max_texture_height > 0) {
            actual_scale = std::min(actual_scale, info.max_texture_height / std::max(texture_height_, 1));
        }
        actual_scale = std::max(actual_scale, 1);
        if (actual_scale == 1) {
            destroyPresentationTexture();
            return;
        }

        const int target_width = texture_width_ * actual_scale;
        const int target_height = texture_height_ * actual_scale;
        if (!presentation_texture_ || presentation_width_ != target_width ||
            presentation_height_ != target_height || presentation_scale_ != actual_scale) {
            destroyPresentationTexture();
            presentation_texture_ = SDL_CreateTexture(
                renderer_,
                SDL_PIXELFORMAT_ARGB8888,
                SDL_TEXTUREACCESS_TARGET,
                target_width,
                target_height
            );
            if (!presentation_texture_) {
                psxe_diag_logf(
                    "renderer",
                    "SDL presentation upscale unavailable requested=%dx target=%dx%d error=%s",
                    requested_scale,
                    target_width,
                    target_height,
                    SDL_GetError()
                );
                return;
            }
            presentation_width_ = target_width;
            presentation_height_ = target_height;
            presentation_scale_ = actual_scale;
            psxe_diag_logf(
                "renderer",
                "SDL presentation upscale created requested=%dx actual=%dx target=%dx%d",
                requested_scale,
                actual_scale,
                target_width,
                target_height
            );
            SDL_SetTextureBlendMode(presentation_texture_, SDL_BLENDMODE_NONE);
#if SDL_VERSION_ATLEAST(2, 0, 12)
            SDL_SetTextureScaleMode(presentation_texture_, SDL_ScaleModeBest);
#endif
        }

#if SDL_VERSION_ATLEAST(2, 0, 12)
        if (SDL_SetTextureScaleMode(texture_, SDL_ScaleModeBest) != 0) {
            SDL_SetTextureScaleMode(texture_, SDL_ScaleModeLinear);
        }
#endif

        SDL_Texture* previous_target = SDL_GetRenderTarget(renderer_);
        SDL_Rect previous_viewport{};
        SDL_Rect previous_clip{};
        float previous_scale_x = 1.0f;
        float previous_scale_y = 1.0f;
        Uint8 previous_r = 0;
        Uint8 previous_g = 0;
        Uint8 previous_b = 0;
        Uint8 previous_a = 0;
        SDL_BlendMode previous_blend = SDL_BLENDMODE_NONE;
        const SDL_bool clip_enabled = SDL_RenderIsClipEnabled(renderer_);
        SDL_RenderGetViewport(renderer_, &previous_viewport);
        SDL_RenderGetClipRect(renderer_, &previous_clip);
        SDL_RenderGetScale(renderer_, &previous_scale_x, &previous_scale_y);
        SDL_GetRenderDrawColor(renderer_, &previous_r, &previous_g, &previous_b, &previous_a);
        SDL_GetRenderDrawBlendMode(renderer_, &previous_blend);

        bool rendered = false;
        if (SDL_SetRenderTarget(renderer_, presentation_texture_) == 0) {
            SDL_RenderSetViewport(renderer_, nullptr);
            SDL_RenderSetClipRect(renderer_, nullptr);
            SDL_RenderSetScale(renderer_, 1.0f, 1.0f);
            SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);
            SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
            rendered = SDL_RenderClear(renderer_) == 0 &&
                SDL_RenderCopy(renderer_, texture_, nullptr, nullptr) == 0;
        }

        SDL_SetRenderTarget(renderer_, previous_target);
        SDL_RenderSetViewport(renderer_, &previous_viewport);
        SDL_RenderSetScale(renderer_, previous_scale_x, previous_scale_y);
        SDL_RenderSetClipRect(renderer_, clip_enabled ? &previous_clip : nullptr);
        SDL_SetRenderDrawBlendMode(renderer_, previous_blend);
        SDL_SetRenderDrawColor(renderer_, previous_r, previous_g, previous_b, previous_a);

        if (!rendered) {
            psxe_diag_logf(
                "renderer",
                "SDL presentation upscale render failed requested=%dx actual=%dx error=%s",
                requested_scale,
                actual_scale,
                SDL_GetError()
            );
            destroyPresentationTexture();
        }
    }

    static void SessionVblankEvent(psx_gpu_t* gpu) {
        if (gpu) {
            if (auto* session = static_cast<ArmsxSession*>(gpu->udata[0])) {
                session->vblank_counter_++;
            }

            psxe_gpu_vblank_timer_event_cb(gpu);
        }
    }

    void updateAudioPlaybackState() {
        if (!audio_dev_) {
            return;
        }

        const bool should_pause = paused_ || fast_forward_enabled_ || !audio_playback_started_;
        SDL_PauseAudioDevice(audio_dev_, should_pause ? 1 : 0);
    }

    void queueAudioForFrame() {
        if (!psx_ || !audio_dev_ || fast_forward_enabled_) {
            return;
        }

        audio_sample_accumulator_ += static_cast<double>(kAudioMixRate) / frameRate();
        const int sample_count = static_cast<int>(audio_sample_accumulator_);
        if (sample_count <= 0) {
            return;
        }

        audio_sample_accumulator_ -= static_cast<double>(sample_count);

        const size_t byte_count = static_cast<size_t>(sample_count) * sizeof(int16_t) * 2;
        if (byte_count == 0) {
            return;
        }

        std::vector<uint8_t> frame_audio(byte_count);
        MixPsxAudio(psx_, frame_audio.data(), static_cast<int>(frame_audio.size()));

        size_t queue_before = static_cast<size_t>(SDL_GetQueuedAudioSize(audio_dev_));
        if (queue_before > kMaxQueuedAudioBytes) {
            SDL_PauseAudioDevice(audio_dev_, 1);
            SDL_ClearQueuedAudio(audio_dev_);
            audio_playback_started_ = false;
            queue_before = 0;
            audio_sample_accumulator_ = 0.0;
        }

        if (SDL_QueueAudio(audio_dev_, frame_audio.data(), static_cast<Uint32>(frame_audio.size())) != 0) {
            psxe_diag_logf("audio", "SDL_QueueAudio failed: %s", SDL_GetError());
            return;
        }

        const size_t queue_after = static_cast<size_t>(SDL_GetQueuedAudioSize(audio_dev_));
        if (!audio_playback_started_ && queue_after >= kAudioPrimeBytes) {
            audio_playback_started_ = true;
            psxe_diag_logf(
                "audio",
                "SDL queued audio primed bytes=%zu target=%zu frame=%llu",
                queue_after,
                kAudioPrimeBytes,
                static_cast<unsigned long long>(vblank_counter_)
            );
            updateAudioPlaybackState();
        }

#if defined(USE_HARDWARE) && defined(HW_DEBUG)
        if (hardware_backend_active_) {
            psxe_diag_logf(
                "audio",
                "frame-audio frame=%llu samples=%d bytes=%zu queue_before=%zu queue_after=%zu accumulator=%.3f frame_rate=%.3f fast_forward=%s paused=%s",
                static_cast<unsigned long long>(vblank_counter_),
                sample_count,
                byte_count,
                queue_before,
                queue_after,
                audio_sample_accumulator_,
                frameRate(),
                fast_forward_enabled_ ? "true" : "false",
                paused_ ? "true" : "false"
            );
        }
#endif
    }

    void clearQueuedAudio() {
        audio_sample_accumulator_ = 0.0;
        audio_playback_started_ = false;
        if (!audio_dev_) {
            return;
        }

        SDL_PauseAudioDevice(audio_dev_, 1);
        SDL_ClearQueuedAudio(audio_dev_);
    }

    static constexpr int kAudioMixRate = 44100;
    static constexpr size_t kAudioBytesPerSampleFrame = sizeof(int16_t) * 2;
    static constexpr size_t kAudioPrimeBytes =
        static_cast<size_t>(kAudioMixRate * kAudioBytesPerSampleFrame * 50 / 1000);
    static constexpr size_t kMaxQueuedAudioBytes =
        static_cast<size_t>(kAudioMixRate * kAudioBytesPerSampleFrame * 200 / 1000);
    static constexpr std::uint32_t kMaxFrameSteps = PSX_CPU_CPS / 8u;

    SDL_Renderer* renderer_ = nullptr;
    psx_t* psx_ = nullptr;
    psx_input_t* input_ = nullptr;
    psxi_sda_t* pad_device_ = nullptr;
    SDL_Texture* texture_ = nullptr;
    SDL_Texture* presentation_texture_ = nullptr;
    std::vector<uint8_t> texture_snapshot_;
#ifdef USE_HARDWARE
    armsx_hw_renderer_t* hw_renderer_ = nullptr;
#endif
    SDL_AudioDeviceID audio_dev_ = 0;
    double audio_sample_accumulator_ = 0.0;
    bool audio_playback_started_ = false;
    std::filesystem::path disc_path_;
    std::filesystem::path exe_path_;
    std::string title_;
    LaunchKind launch_kind_ = LaunchKind::None;
    bool paused_ = false;
    bool fast_forward_enabled_ = false;
    bool debug_view_ = false;
#ifdef USE_HARDWARE
    bool hardware_backend_active_ = false;
#endif
    std::uint64_t vblank_counter_ = 0;
    int texture_width_ = 0;
    int texture_height_ = 0;
    int presentation_width_ = 0;
    int presentation_height_ = 0;
    int presentation_scale_ = 1;
    Uint32 texture_format_ = SDL_PIXELFORMAT_UNKNOWN;
};

class GameplayInputRouter {
  public:
    ~GameplayInputRouter() {
        if (controller_) {
            SDL_GameControllerClose(controller_);
            controller_ = nullptr;
        }
    }

    void attach(psx_pad_t* pad) {
        clearAll();
        pad_ = pad;
    }

    void detach() {
        clearAll();
        pad_ = nullptr;
    }

    void onFsuiOpened() {
        fsui_owns_input_ = true;
        clearAll();
    }

    void onFsuiClosed() {
        fsui_owns_input_ = false;
        clearAll();
    }

    bool takePauseRequest() {
        const bool requested = pause_requested_;
        pause_requested_ = false;
        return requested;
    }

    bool takeControllerActivity() {
        const bool active = controller_activity_;
        controller_activity_ = false;
        return active;
    }

    bool takeControllerDisconnected() {
        const bool disconnected = controller_disconnected_;
        controller_disconnected_ = false;
        return disconnected;
    }

    void setVirtualDigitalMask(uint32_t mask) {
        if (virtual_digital_mask_ == mask) {
            return;
        }
        virtual_digital_mask_ = mask;
        syncDigitalMask();
    }

    uint32_t activeDigitalMask() const {
        return active_digital_mask_;
    }

    void tick(bool fsui_active) {
        if (!fsui_active) {
            flushChordButtons(SDL_GetTicks());
        }
    }

    std::vector<fsui::InputOverlayDeviceState> buildInputOverlay() const {
        fsui::InputOverlayDeviceState device;
        device.title = "P1";
        device.bindings = {
            fsui::InputOverlayBinding{.label = "Up", .glyph = "^", .kind = fsui::InputOverlayBindingKind::Button, .value = (active_digital_mask_ & PSXI_SW_SDA_PAD_UP) ? 1.0f : 0.0f},
            fsui::InputOverlayBinding{.label = "Down", .glyph = "v", .kind = fsui::InputOverlayBindingKind::Button, .value = (active_digital_mask_ & PSXI_SW_SDA_PAD_DOWN) ? 1.0f : 0.0f},
            fsui::InputOverlayBinding{.label = "Left", .glyph = "<", .kind = fsui::InputOverlayBindingKind::Button, .value = (active_digital_mask_ & PSXI_SW_SDA_PAD_LEFT) ? 1.0f : 0.0f},
            fsui::InputOverlayBinding{.label = "Right", .glyph = ">", .kind = fsui::InputOverlayBindingKind::Button, .value = (active_digital_mask_ & PSXI_SW_SDA_PAD_RIGHT) ? 1.0f : 0.0f},
            fsui::InputOverlayBinding{.label = "Start", .glyph = "S", .kind = fsui::InputOverlayBindingKind::Button, .value = (active_digital_mask_ & PSXI_SW_SDA_START) ? 1.0f : 0.0f},
            fsui::InputOverlayBinding{.label = "Select", .glyph = "=", .kind = fsui::InputOverlayBindingKind::Button, .value = (active_digital_mask_ & PSXI_SW_SDA_SELECT) ? 1.0f : 0.0f},
        };
        return {device};
    }

    void processEvent(const SDL_Event& event, ArmsxSession* session, bool fsui_active) {
        flushChordButtons(SDL_GetTicks());
        handleControllerLifecycle(event);

        if (!session || !session->valid()) {
            return;
        }

        if (event.type == SDL_KEYDOWN) {
            if (event.key.repeat == 0 && event.key.keysym.sym == SDLK_ESCAPE && !fsui_active) {
                pause_requested_ = true;
                return;
            }

            if (fsui_active || fsui_owns_input_ || event.key.repeat != 0) {
                return;
            }

            const uint32_t mask = buttonForKey(event.key.keysym.sym);
            if (mask) {
                press(mask);
            }
            return;
        }

        if (event.type == SDL_KEYUP) {
            if (event.key.keysym.sym == SDLK_ESCAPE) {
                return;
            }

            if (fsui_active || fsui_owns_input_) {
                return;
            }

            const uint32_t mask = buttonForKey(event.key.keysym.sym);
            if (mask) {
                release(mask);
            }
            return;
        }

        if (!controller_) {
            return;
        }

        SDL_Joystick* joystick = SDL_GameControllerGetJoystick(controller_);
        if (!joystick) {
            return;
        }

        const SDL_JoystickID controller_id = SDL_JoystickInstanceID(joystick);

        if (event.type == SDL_CONTROLLERBUTTONDOWN || event.type == SDL_CONTROLLERBUTTONUP) {
            if (event.cbutton.which != controller_id) {
                return;
            }

            const bool pressed = event.type == SDL_CONTROLLERBUTTONDOWN;
            const SDL_GameControllerButton button = static_cast<SDL_GameControllerButton>(event.cbutton.button);
            if (pressed && !fsui_active && !fsui_owns_input_) {
                controller_activity_ = true;
            }

            if (button == SDL_CONTROLLER_BUTTON_START || button == SDL_CONTROLLER_BUTTON_BACK ||
                button == SDL_CONTROLLER_BUTTON_GUIDE || button == SDL_CONTROLLER_BUTTON_MISC1) {
                handlePauseChordButton(button, pressed, fsui_active);
                return;
            }

            if (fsui_active || fsui_owns_input_) {
                return;
            }

            const uint32_t mask = buttonForController(button);
            if (mask) {
                if (pressed) {
                    press(mask);
                } else {
                    release(mask);
                }
            }
            return;
        }

        if (event.type == SDL_CONTROLLERAXISMOTION) {
            if (event.caxis.which != controller_id || fsui_active || fsui_owns_input_ || !pad_) {
                return;
            }

            if (std::abs(static_cast<int>(event.caxis.value)) > 8000) {
                controller_activity_ = true;
            }
            const uint16_t mapped = static_cast<uint16_t>((static_cast<int>(event.caxis.value) + INT16_MAX + 1) / 0x100);
            switch (event.caxis.axis) {
                case SDL_CONTROLLER_AXIS_RIGHTX:
                    psx_pad_analog_change(pad_, 0, PSXI_AX_SDA_RIGHT_HORZ, mapped);
                    break;
                case SDL_CONTROLLER_AXIS_RIGHTY:
                    psx_pad_analog_change(pad_, 0, PSXI_AX_SDA_RIGHT_VERT, mapped);
                    break;
                case SDL_CONTROLLER_AXIS_LEFTX:
                    psx_pad_analog_change(pad_, 0, PSXI_AX_SDA_LEFT_HORZ, mapped);
                    break;
                case SDL_CONTROLLER_AXIS_LEFTY:
                    psx_pad_analog_change(pad_, 0, PSXI_AX_SDA_LEFT_VERT, mapped);
                    break;
#ifdef CONTROLLER_GENERIC
                case SDL_CONTROLLER_AXIS_TRIGGERLEFT:
                    handleTrigger(PSXI_SW_SDA_L2, trigger_left_down_, event.caxis.value);
                    break;
                case SDL_CONTROLLER_AXIS_TRIGGERRIGHT:
                    handleTrigger(PSXI_SW_SDA_R2, trigger_right_down_, event.caxis.value);
                    break;
#endif
                default:
                    break;
            }
        }
    }

  private:
    void handleControllerLifecycle(const SDL_Event& event) {
        if (event.type == SDL_CONTROLLERDEVICEADDED) {
            if (!controller_ && SDL_IsGameController(event.cdevice.which)) {
                controller_ = SDL_GameControllerOpen(event.cdevice.which);
            }
            return;
        }

        if (event.type == SDL_CONTROLLERDEVICEREMOVED && controller_) {
            SDL_Joystick* joystick = SDL_GameControllerGetJoystick(controller_);
            if (joystick && SDL_JoystickInstanceID(joystick) == event.cdevice.which) {
                SDL_GameControllerClose(controller_);
                controller_ = nullptr;
                controller_disconnected_ = true;
                start_button_ = {};
                select_button_ = {};
                trigger_left_down_ = false;
                trigger_right_down_ = false;
            }
        }
    }

    void handlePauseChordButton(SDL_GameControllerButton button, bool pressed, bool fsui_active) {
        PendingChordButton* state = nullptr;
        PendingChordButton* other = nullptr;

        if (button == SDL_CONTROLLER_BUTTON_START) {
            state = &start_button_;
            other = &select_button_;
        } else {
            state = &select_button_;
            other = &start_button_;
        }

        if (pressed) {
            state->physical_down = true;
            if (!fsui_active && !fsui_owns_input_ && !pause_latched_) {
                state->pending = true;
                state->pending_since = SDL_GetTicks();
                if (other->physical_down && other->pending) {
                    pause_latched_ = true;
                    pause_requested_ = true;
                    state->pending = false;
                    other->pending = false;
                    clearAll();
                }
            }
            return;
        }

        state->physical_down = false;

        if (pause_latched_) {
            if (!start_button_.physical_down && !select_button_.physical_down) {
                pause_latched_ = false;
                start_button_ = {};
                select_button_ = {};
            }
            return;
        }

        if (state->pending) {
            const uint32_t mask = (state == &start_button_) ? PSXI_SW_SDA_START : PSXI_SW_SDA_SELECT;
            press(mask);
            release(mask);
            state->pending = false;
            return;
        }

        if (state->forwarded) {
            const uint32_t mask = (state == &start_button_) ? PSXI_SW_SDA_START : PSXI_SW_SDA_SELECT;
            release(mask);
            state->forwarded = false;
        }
    }

    void flushChordButtons(Uint32 now) {
        if (pause_latched_ || fsui_owns_input_) {
            return;
        }

        flushOneChordButton(start_button_, select_button_, PSXI_SW_SDA_START, now);
        flushOneChordButton(select_button_, start_button_, PSXI_SW_SDA_SELECT, now);
    }

    void flushOneChordButton(PendingChordButton& state, PendingChordButton& other, uint32_t mask, Uint32 now) {
        if (!state.pending || other.physical_down || !pad_) {
            return;
        }

        if ((now - state.pending_since) < kPauseChordGraceMs) {
            return;
        }

        press(mask);
        state.pending = false;
        state.forwarded = true;
    }

    void press(uint32_t mask) {
        if (!mask) {
            return;
        }
        physical_digital_mask_ |= mask;
        syncDigitalMask();
    }

    void release(uint32_t mask) {
        if (!mask) {
            return;
        }
        physical_digital_mask_ &= ~mask;
        syncDigitalMask();
    }

    void syncDigitalMask() {
        const uint32_t desired_mask = physical_digital_mask_ | virtual_digital_mask_;
        if (!pad_) {
            active_digital_mask_ = desired_mask;
            return;
        }

        const uint32_t pressed = desired_mask & ~active_digital_mask_;
        const uint32_t released = active_digital_mask_ & ~desired_mask;
        if (pressed) {
            psx_pad_button_press(pad_, 0, pressed);
        }
        if (released) {
            psx_pad_button_release(pad_, 0, released);
        }
        active_digital_mask_ = desired_mask;
    }

    void clearAll() {
        if (!pad_) {
            physical_digital_mask_ = 0;
            virtual_digital_mask_ = 0;
            active_digital_mask_ = 0;
            return;
        }

        static const std::array<uint32_t, 17> masks = {
            PSXI_SW_SDA_SELECT, PSXI_SW_SDA_L3, PSXI_SW_SDA_R3, PSXI_SW_SDA_START,
            PSXI_SW_SDA_PAD_UP, PSXI_SW_SDA_PAD_RIGHT, PSXI_SW_SDA_PAD_DOWN, PSXI_SW_SDA_PAD_LEFT,
            PSXI_SW_SDA_L2, PSXI_SW_SDA_R2, PSXI_SW_SDA_L1, PSXI_SW_SDA_R1,
            PSXI_SW_SDA_TRIANGLE, PSXI_SW_SDA_CIRCLE, PSXI_SW_SDA_CROSS, PSXI_SW_SDA_SQUARE,
            PSXI_SW_SDA_ANALOG,
        };

        for (uint32_t mask : masks) {
            if ((active_digital_mask_ & mask) != 0) {
                psx_pad_button_release(pad_, 0, mask);
            }
        }

        physical_digital_mask_ = 0;
        virtual_digital_mask_ = 0;
        active_digital_mask_ = 0;
        psx_pad_analog_change(pad_, 0, PSXI_AX_SDA_RIGHT_HORZ, 0x80);
        psx_pad_analog_change(pad_, 0, PSXI_AX_SDA_RIGHT_VERT, 0x80);
        psx_pad_analog_change(pad_, 0, PSXI_AX_SDA_LEFT_HORZ, 0x80);
        psx_pad_analog_change(pad_, 0, PSXI_AX_SDA_LEFT_VERT, 0x80);
    }

#ifdef CONTROLLER_GENERIC
    void handleTrigger(uint32_t mask, bool& down_flag, Sint16 value) {
        const bool pressed = value > 8000;
        if (pressed == down_flag) {
            return;
        }

        down_flag = pressed;
        if (pressed) {
            press(mask);
        } else {
            release(mask);
        }
    }
#endif

    static uint32_t buttonForKey(SDL_Keycode key) {
        switch (key) {
            case SDLK_x: return PSXI_SW_SDA_CROSS;
            case SDLK_a: return PSXI_SW_SDA_SQUARE;
            case SDLK_w: return PSXI_SW_SDA_TRIANGLE;
            case SDLK_d: return PSXI_SW_SDA_CIRCLE;
            case SDLK_RETURN: return PSXI_SW_SDA_START;
            case SDLK_s: return PSXI_SW_SDA_SELECT;
            case SDLK_UP: return PSXI_SW_SDA_PAD_UP;
            case SDLK_DOWN: return PSXI_SW_SDA_PAD_DOWN;
            case SDLK_LEFT: return PSXI_SW_SDA_PAD_LEFT;
            case SDLK_RIGHT: return PSXI_SW_SDA_PAD_RIGHT;
            case SDLK_q: return PSXI_SW_SDA_L1;
            case SDLK_e: return PSXI_SW_SDA_R1;
            case SDLK_1: return PSXI_SW_SDA_L2;
            case SDLK_3: return PSXI_SW_SDA_R2;
            case SDLK_z: return PSXI_SW_SDA_L3;
            case SDLK_c: return PSXI_SW_SDA_R3;
            case SDLK_2: return PSXI_SW_SDA_ANALOG;
            default: return 0;
        }
    }

    static uint32_t buttonForController(SDL_GameControllerButton button) {
        switch (button) {
            case SDL_CONTROLLER_BUTTON_A: return PSXI_SW_SDA_CROSS;
            case SDL_CONTROLLER_BUTTON_X: return PSXI_SW_SDA_SQUARE;
            case SDL_CONTROLLER_BUTTON_Y: return PSXI_SW_SDA_TRIANGLE;
            case SDL_CONTROLLER_BUTTON_B: return PSXI_SW_SDA_CIRCLE;
            case SDL_CONTROLLER_BUTTON_DPAD_UP: return PSXI_SW_SDA_PAD_UP;
            case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return PSXI_SW_SDA_PAD_DOWN;
            case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return PSXI_SW_SDA_PAD_LEFT;
            case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return PSXI_SW_SDA_PAD_RIGHT;
            case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return PSXI_SW_SDA_L1;
            case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return PSXI_SW_SDA_R1;
            case SDL_CONTROLLER_BUTTON_LEFTSTICK: return PSXI_SW_SDA_L3;
            case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return PSXI_SW_SDA_R3;
            default: return 0;
        }
    }

    psx_pad_t* pad_ = nullptr;
    SDL_GameController* controller_ = nullptr;
    uint32_t physical_digital_mask_ = 0;
    uint32_t virtual_digital_mask_ = 0;
    uint32_t active_digital_mask_ = 0;
    PendingChordButton start_button_{};
    PendingChordButton select_button_{};
    bool pause_requested_ = false;
    bool pause_latched_ = false;
    bool fsui_owns_input_ = false;
    bool trigger_left_down_ = false;
    bool trigger_right_down_ = false;
    bool controller_activity_ = false;
    bool controller_disconnected_ = false;
};

class ImGuiMobileControls {
  public:
    void processEvent(const SDL_Event& event) {
        if (event.type == SDL_MOUSEBUTTONDOWN && event.button.button == SDL_BUTTON_LEFT) {
            if (!visible_) {
                showNow();
                suppress_mouse_until_up_ = true;
            } else {
                last_activity_ms_ = SDL_GetTicks();
                const ImVec2 display = ImGui::GetIO().DisplaySize;
                if (display.x > 0.0f && display.y > 0.0f) {
                    const Layout layout = makeLayout(display);
                    if (pointInRect(ImVec2(static_cast<float>(event.button.x), static_cast<float>(event.button.y)), layout.pause)) {
                        pause_requested_ = true;
                        suppress_mouse_until_up_ = true;
                    }
                }
            }
            return;
        }
        if (event.type == SDL_MOUSEBUTTONUP && event.button.button == SDL_BUTTON_LEFT) {
            suppress_mouse_until_up_ = false;
            return;
        }
        if (event.type != SDL_FINGERDOWN && event.type != SDL_FINGERMOTION && event.type != SDL_FINGERUP) {
            return;
        }

        const auto suppressed = std::find(suppressed_touches_.begin(), suppressed_touches_.end(), event.tfinger.fingerId);
        if (suppressed != suppressed_touches_.end()) {
            if (event.type == SDL_FINGERUP) {
                suppressed_touches_.erase(suppressed);
            }
            return;
        }

        if (event.type == SDL_FINGERDOWN && !visible_) {
            showNow();
            suppressed_touches_.push_back(event.tfinger.fingerId);
            return;
        }

        last_activity_ms_ = SDL_GetTicks();
        const auto found = std::find_if(touches_.begin(), touches_.end(), [&](const Touch& touch) {
            return touch.id == event.tfinger.fingerId;
        });

        if (event.type == SDL_FINGERUP) {
            if (found != touches_.end()) {
                touches_.erase(found);
            }
            return;
        }

        const float x = std::clamp(event.tfinger.x, 0.0f, 1.0f);
        const float y = std::clamp(event.tfinger.y, 0.0f, 1.0f);
        if (found != touches_.end()) {
            found->x = x;
            found->y = y;
            return;
        }

        if (event.type == SDL_FINGERDOWN) {
            const ImVec2 display = ImGui::GetIO().DisplaySize;
            if (display.x <= 0.0f || display.y <= 0.0f) {
                return;
            }

            Touch next{
                .id = event.tfinger.fingerId,
                .x = x,
                .y = y,
            };
            captureTouch(next, makeLayout(display), ImVec2(x * display.x, y * display.y));
            if (next.kind == TouchKind::Pause) {
                pause_requested_ = true;
            }
            touches_.push_back(next);
        }
    }

    void reset(GameplayInputRouter& input) {
        touches_.clear();
        suppressed_touches_.clear();
        routed_mask_ = 0;
        pause_requested_ = false;
        input.setVirtualDigitalMask(0);
    }

    bool takePauseRequest() {
        const bool requested = pause_requested_;
        pause_requested_ = false;
        return requested;
    }

    void showNow() {
        visible_ = true;
        controller_in_use_ = false;
        last_activity_ms_ = SDL_GetTicks();
    }

    void onControllerActivity(GameplayInputRouter& input) {
        controller_in_use_ = true;
        visible_ = false;
        reset(input);
    }

    void onControllerDisconnected() {
        showNow();
    }

    void drawAndRoute(GameplayInputRouter& input) {
        const Uint32 now = SDL_GetTicks();
        if (last_activity_ms_ == 0) {
            last_activity_ms_ = now;
        }
        if (visible_ && touches_.empty() && !suppress_mouse_until_up_ &&
            (now - last_activity_ms_) >= kMobileControlsAutoHideMs) {
            visible_ = false;
            reset(input);
        }
        if (!visible_) {
            input.setVirtualDigitalMask(0);
            return;
        }

        const ImVec2 display = ImGui::GetIO().DisplaySize;
        if (display.x <= 0.0f || display.y <= 0.0f) {
            input.setVirtualDigitalMask(0);
            return;
        }

        const Layout layout = makeLayout(display);
        uint32_t mask = 0;
        for (const Touch& touch : touches_) {
            if (touch.kind == TouchKind::Stick) {
                mask |= stickMaskForPoint(layout, ImVec2(touch.x * display.x, touch.y * display.y));
            } else if (touch.kind == TouchKind::DigitalButton) {
                mask |= touch.mask;
            }
        }

        ImGuiIO& io = ImGui::GetIO();
        if (touches_.empty() && io.MouseDown[0] && !io.WantCaptureMouse && !suppress_mouse_until_up_) {
            mask |= maskForPoint(layout, io.MousePos);
        }

        routed_mask_ = mask;
        input.setVirtualDigitalMask(mask);
        draw(layout, input.activeDigitalMask());
    }

  private:
    enum class TouchKind {
        None,
        Stick,
        DigitalButton,
        Pause,
    };

    struct Touch {
        SDL_FingerID id = 0;
        float x = 0.0f;
        float y = 0.0f;
        TouchKind kind = TouchKind::None;
        uint32_t mask = 0;
    };

    struct Button {
        ImVec2 center{};
        float radius = 0.0f;
        uint32_t mask = 0;
    };

    struct RectButton {
        ImVec2 min{};
        ImVec2 max{};
        uint32_t mask = 0;
        const char* label = "";
    };

    struct Layout {
        ImVec2 display{};
        ImVec2 stick_center{};
        float stick_radius = 0.0f;
        float stick_deadzone = 0.0f;
        std::array<Button, 4> face{};
        std::array<RectButton, 6> utility{};
        RectButton pause{};
    };

    static Layout makeLayout(const ImVec2& display) {
        const float unit = std::max(std::min(display.x, display.y), 1.0f);
        const float bottom_margin = unit * 0.075f;
        const float stick_radius = unit * 0.135f;
        const float face_radius = unit * 0.066f;
        const float face_offset = unit * 0.125f;
        const ImVec2 stick_center(unit * 0.21f, display.y - bottom_margin - stick_radius);
        const ImVec2 face_center(display.x - unit * 0.21f, display.y - bottom_margin - stick_radius);

        Layout layout;
        layout.display = display;
        layout.stick_center = stick_center;
        layout.stick_radius = stick_radius;
        layout.stick_deadzone = stick_radius * 0.27f;
        layout.face = {
            Button{ImVec2(face_center.x, face_center.y - face_offset), face_radius, PSXI_SW_SDA_TRIANGLE},
            Button{ImVec2(face_center.x + face_offset, face_center.y), face_radius, PSXI_SW_SDA_CIRCLE},
            Button{ImVec2(face_center.x, face_center.y + face_offset), face_radius, PSXI_SW_SDA_CROSS},
            Button{ImVec2(face_center.x - face_offset, face_center.y), face_radius, PSXI_SW_SDA_SQUARE},
        };

        const float shoulder_width = unit * 0.23f;
        const float shoulder_height = unit * 0.075f;
        const float edge = unit * 0.035f;
        const float shoulder_gap = unit * 0.025f;
        const float pill_width = unit * 0.17f;
        const float pill_height = unit * 0.065f;
        const float pill_y = display.y - bottom_margin - pill_height;
        layout.utility = {
            RectButton{ImVec2(edge, edge), ImVec2(edge + shoulder_width, edge + shoulder_height), PSXI_SW_SDA_L2, "L2"},
            RectButton{ImVec2(edge, edge + shoulder_height + shoulder_gap), ImVec2(edge + shoulder_width, edge + shoulder_height * 2.0f + shoulder_gap), PSXI_SW_SDA_L1, "L1"},
            RectButton{ImVec2(display.x - edge - shoulder_width, edge), ImVec2(display.x - edge, edge + shoulder_height), PSXI_SW_SDA_R2, "R2"},
            RectButton{ImVec2(display.x - edge - shoulder_width, edge + shoulder_height + shoulder_gap), ImVec2(display.x - edge, edge + shoulder_height * 2.0f + shoulder_gap), PSXI_SW_SDA_R1, "R1"},
            RectButton{ImVec2(display.x * 0.5f - pill_width - shoulder_gap * 0.5f, pill_y), ImVec2(display.x * 0.5f - shoulder_gap * 0.5f, pill_y + pill_height), PSXI_SW_SDA_SELECT, "SELECT"},
            RectButton{ImVec2(display.x * 0.5f + shoulder_gap * 0.5f, pill_y), ImVec2(display.x * 0.5f + pill_width + shoulder_gap * 0.5f, pill_y + pill_height), PSXI_SW_SDA_START, "START"},
        };
        const float pause_width = unit * 0.15f;
        const float pause_height = unit * 0.075f;
        layout.pause = RectButton{
            ImVec2(display.x * 0.5f - pause_width * 0.5f, edge),
            ImVec2(display.x * 0.5f + pause_width * 0.5f, edge + pause_height),
            0,
            ICON_FA_PAUSE,
        };
        return layout;
    }

    static float distanceSquared(const ImVec2& lhs, const ImVec2& rhs) {
        const float dx = lhs.x - rhs.x;
        const float dy = lhs.y - rhs.y;
        return dx * dx + dy * dy;
    }

    static bool pointInRect(const ImVec2& point, const RectButton& button) {
        return point.x >= button.min.x && point.x <= button.max.x &&
            point.y >= button.min.y && point.y <= button.max.y;
    }

    static uint32_t stickMaskForPoint(const Layout& layout, const ImVec2& point) {
        uint32_t mask = 0;
        const float dx = point.x - layout.stick_center.x;
        const float dy = point.y - layout.stick_center.y;
        if (dx <= -layout.stick_deadzone) mask |= PSXI_SW_SDA_PAD_LEFT;
        if (dx >= layout.stick_deadzone) mask |= PSXI_SW_SDA_PAD_RIGHT;
        if (dy <= -layout.stick_deadzone) mask |= PSXI_SW_SDA_PAD_UP;
        if (dy >= layout.stick_deadzone) mask |= PSXI_SW_SDA_PAD_DOWN;
        return mask;
    }

    static void captureTouch(Touch& touch, const Layout& layout, const ImVec2& point) {
        if (pointInRect(point, layout.pause)) {
            touch.kind = TouchKind::Pause;
            return;
        }

        const float stick_hit_radius = layout.stick_radius * 1.35f;
        if (distanceSquared(point, layout.stick_center) <= stick_hit_radius * stick_hit_radius) {
            touch.kind = TouchKind::Stick;
            return;
        }

        for (const Button& button : layout.face) {
            const float hit_radius = button.radius * 1.35f;
            if (distanceSquared(point, button.center) <= hit_radius * hit_radius) {
                touch.kind = TouchKind::DigitalButton;
                touch.mask = button.mask;
                return;
            }
        }
        for (const RectButton& button : layout.utility) {
            if (pointInRect(point, button)) {
                touch.kind = TouchKind::DigitalButton;
                touch.mask = button.mask;
                return;
            }
        }
    }

    static uint32_t maskForPoint(const Layout& layout, const ImVec2& point) {
        uint32_t mask = 0;
        const float stick_hit_radius = layout.stick_radius * 1.35f;
        if (distanceSquared(point, layout.stick_center) <= stick_hit_radius * stick_hit_radius) {
            mask |= stickMaskForPoint(layout, point);
        }

        for (const Button& button : layout.face) {
            const float hit_radius = button.radius * 1.35f;
            if (distanceSquared(point, button.center) <= hit_radius * hit_radius) {
                mask |= button.mask;
            }
        }
        for (const RectButton& button : layout.utility) {
            if (pointInRect(point, button)) {
                mask |= button.mask;
            }
        }
        return mask;
    }

    static void drawCenteredText(ImDrawList* draw_list, const ImVec2& center, ImU32 color, const char* text) {
        const ImVec2 size = ImGui::CalcTextSize(text);
        draw_list->AddText(ImVec2(center.x - size.x * 0.5f, center.y - size.y * 0.5f), color, text);
    }

    static void drawFaceSymbol(ImDrawList* draw_list, const Button& button, uint32_t active_mask) {
        const bool active = (active_mask & button.mask) != 0;
        ImU32 accent = IM_COL32(225, 225, 235, 220);
        if (button.mask == PSXI_SW_SDA_TRIANGLE) accent = IM_COL32(70, 220, 150, 235);
        if (button.mask == PSXI_SW_SDA_CIRCLE) accent = IM_COL32(245, 90, 105, 235);
        if (button.mask == PSXI_SW_SDA_CROSS) accent = IM_COL32(95, 150, 255, 235);
        if (button.mask == PSXI_SW_SDA_SQUARE) accent = IM_COL32(225, 105, 220, 235);
        draw_list->AddCircleFilled(
            button.center,
            button.radius,
            active ? IM_COL32(255, 255, 255, 105) : IM_COL32(18, 20, 28, 112),
            32
        );
        draw_list->AddCircle(button.center, button.radius, accent, 32, active ? 4.0f : 2.5f);

        const float symbol = button.radius * 0.48f;
        if (button.mask == PSXI_SW_SDA_TRIANGLE) {
            draw_list->AddTriangle(
                ImVec2(button.center.x, button.center.y - symbol),
                ImVec2(button.center.x + symbol, button.center.y + symbol),
                ImVec2(button.center.x - symbol, button.center.y + symbol),
                accent,
                3.0f
            );
        } else if (button.mask == PSXI_SW_SDA_CIRCLE) {
            draw_list->AddCircle(button.center, symbol, accent, 24, 3.0f);
        } else if (button.mask == PSXI_SW_SDA_CROSS) {
            draw_list->AddLine(
                ImVec2(button.center.x - symbol, button.center.y - symbol),
                ImVec2(button.center.x + symbol, button.center.y + symbol),
                accent,
                3.0f
            );
            draw_list->AddLine(
                ImVec2(button.center.x + symbol, button.center.y - symbol),
                ImVec2(button.center.x - symbol, button.center.y + symbol),
                accent,
                3.0f
            );
        } else {
            draw_list->AddRect(
                ImVec2(button.center.x - symbol, button.center.y - symbol),
                ImVec2(button.center.x + symbol, button.center.y + symbol),
                accent,
                1.0f,
                0,
                3.0f
            );
        }
    }

    static void draw(const Layout& layout, uint32_t active_mask) {
        ImDrawList* draw_list = ImGui::GetForegroundDrawList();
        const ImU32 panel = IM_COL32(18, 20, 28, 112);
        const ImU32 outline = IM_COL32(235, 238, 246, 185);
        const ImU32 active = IM_COL32(255, 255, 255, 105);

        draw_list->AddCircleFilled(layout.stick_center, layout.stick_radius, panel, 40);
        draw_list->AddCircle(layout.stick_center, layout.stick_radius, outline, 40, 2.5f);
        ImVec2 knob = layout.stick_center;
        const float knob_offset = layout.stick_radius * 0.43f;
        if (active_mask & PSXI_SW_SDA_PAD_LEFT) knob.x -= knob_offset;
        if (active_mask & PSXI_SW_SDA_PAD_RIGHT) knob.x += knob_offset;
        if (active_mask & PSXI_SW_SDA_PAD_UP) knob.y -= knob_offset;
        if (active_mask & PSXI_SW_SDA_PAD_DOWN) knob.y += knob_offset;
        const bool stick_active = (active_mask &
            (PSXI_SW_SDA_PAD_LEFT | PSXI_SW_SDA_PAD_RIGHT | PSXI_SW_SDA_PAD_UP | PSXI_SW_SDA_PAD_DOWN)) != 0;
        draw_list->AddCircleFilled(
            knob,
            layout.stick_radius * 0.42f,
            stick_active ? IM_COL32(165, 185, 255, 180) : IM_COL32(105, 115, 140, 160),
            32
        );
        draw_list->AddCircle(knob, layout.stick_radius * 0.42f, outline, 32, 2.0f);

        for (const Button& button : layout.face) {
            drawFaceSymbol(draw_list, button, active_mask);
        }

        for (const RectButton& button : layout.utility) {
            const bool is_active = (active_mask & button.mask) != 0;
            draw_list->AddRectFilled(button.min, button.max, is_active ? active : panel, 10.0f);
            draw_list->AddRect(button.min, button.max, outline, 10.0f, 0, is_active ? 3.5f : 2.0f);
            drawCenteredText(
                draw_list,
                ImVec2((button.min.x + button.max.x) * 0.5f, (button.min.y + button.max.y) * 0.5f),
                IM_COL32(245, 247, 252, 225),
                button.label
            );
        }

        draw_list->AddRectFilled(layout.pause.min, layout.pause.max, panel, 10.0f);
        draw_list->AddRect(layout.pause.min, layout.pause.max, outline, 10.0f, 0, 2.0f);
        drawCenteredText(
            draw_list,
            ImVec2((layout.pause.min.x + layout.pause.max.x) * 0.5f, (layout.pause.min.y + layout.pause.max.y) * 0.5f),
            IM_COL32(245, 247, 252, 235),
            layout.pause.label
        );
    }

    std::vector<Touch> touches_;
    std::vector<SDL_FingerID> suppressed_touches_;
    uint32_t routed_mask_ = 0;
    Uint32 last_activity_ms_ = 0;
    bool visible_ = true;
    bool controller_in_use_ = false;
    bool suppress_mouse_until_up_ = false;
    bool pause_requested_ = false;
};

class ArmsxApp {
  public:
    ArmsxApp(int argc, const char* argv[], void* external_window, void* external_renderer)
        : argc_(argc), argv_(argv), external_window_(static_cast<SDL_Window*>(external_window)),
          external_renderer_(static_cast<SDL_Renderer*>(external_renderer)), cli_(ScanCliFlags(argc, argv)) {}

    int run() {
#if defined(IOS_TARGET)
        psxe_diag_set_enabled(1);
        log_set_level(LOG_INFO);
        log_set_quiet(0);
#endif
        psxe_diag_initialize(psxe_cfg_get_pref_path());
        psxe_diag_breadcrumbf("ARMSX startup argc=%d", argc_);
        psxe_diag_logf("diag", "Pref path: %s", psxe_cfg_get_pref_path() ? psxe_cfg_get_pref_path() : "(none)");

        static bool log_callback_installed = false;
        if (!log_callback_installed) {
            log_add_callback(StructuredLogCallback, nullptr, LOG_TRACE);
            SDL_LogSetOutputFunction(SdlLogOutput, nullptr);
            log_callback_installed = true;
        }

        psxe_config_t* cfg = psxe_cfg_create();
        if (!cfg) {
            return 1;
        }

        psxe_cfg_init(cfg);
        psxe_cfg_load_defaults(cfg);
        psxe_cfg_load(cfg, argc_, const_cast<const char**>(argv_));
        settings_ = BuildSettings(cfg, cli_);
        psxe_cfg_destroy(cfg);
        syncPlatformLibraries(false, false);

        applyLoggingSettings("startup");
#if defined(USE_HARDWARE)
        if (const char* env_backend = std::getenv("ARMSX_GPU_BACKEND")) {
            psxe_diag_breadcrumbf(
                "GPU backend env override=%s resolved=%s",
                env_backend,
                GpuBackendTitle(settings_.gpu_backend)
            );
            psxe_diag_logf(
                "diag",
                "GPU backend env override=%s resolved=%s",
                env_backend,
                GpuBackendTitle(settings_.gpu_backend)
            );
        }
#endif
        psxe_diag_breadcrumbf("Settings loaded model=%s region=%s logging_enabled=%s log_level=%d",
            settings_.model.c_str(),
            settings_.region.c_str(),
            settings_.logging_enabled ? "true" : "false",
            settings_.log_level);

        installCrashHandlers();

        if (!initializeSdl() || !initializeWindowAndRenderer() || !initializeFsui()) {
            shutdown();
            return 1;
        }

        logRendererBootstrap("frontend-init", kUiFrameRate);
        refreshGameList(true);

        g_active_app = this;
        initializeWebLaunchSupport();
        consumePendingLaunchArguments();

        if (cli_.has_boot_request) {
            if (pending_cli_launch_.has_value()) {
                launchSession(*pending_cli_launch_, false);
            } else if (!pending_cli_argument_.empty()) {
                pending_error_dialog_ = "不支持此启动路径或 URI。";
            }
        }

        consumePendingLaunchArguments();

        if (!session_.valid()) {
            showLandingWindow();
        }

        fsui::SdlMainLoopCallbacks loop;
        loop.handle_event = [this](const SDL_Event& event) { handleEvent(event); };
        loop.run_frame = [this]() { runFrame(); };
        loop.should_continue = [this]() { return running_; };
        loop.on_exit = [this]() { shutdown(); };
        fsui::RunSdlMainLoop(loop);
        g_active_app = nullptr;

        return 0;
    }

    void writeCrashContext(const char* reason) const {
        const fsui::CurrentGameInfo info = session_.currentGameInfo();
        psxe_diag_logf("crash", "Crash reason: %s", reason ? reason : "(unknown)");
        psxe_diag_logf("crash", "Running game: %s", info.has_game ? info.title.c_str() : "(none)");
        psxe_diag_logf("crash", "Game path: %s", info.path.empty() ? "(none)" : info.path.string().c_str());
        psxe_diag_logf("crash", "BIOS override: %s", settings_.bios_override.empty() ? "(none)" : settings_.bios_override.c_str());
        psxe_diag_logf("crash", "BIOS folder: %s", settings_.bios_search.c_str());
        psxe_diag_logf("crash", "Model=%s Region=%s Expansion=%s",
            settings_.model.c_str(),
            settings_.region.c_str(),
            settings_.exp_path.empty() ? "(none)" : settings_.exp_path.c_str());
        logRendererBootstrap("crash", currentTargetFrameRate());
        logCpuState();
        psxe_diag_dump_breadcrumbs();
    }

  private:
    void initializeWebLaunchSupport() {
#if defined(__EMSCRIPTEN__)
        static bool initialized = false;
        if (initialized) {
            return;
        }

        initialized = true;
        EM_ASM({
            try {
                const searchParams = new URLSearchParams(window.location.search);
                let launchUri = searchParams.get('uri') || '';
                if (!launchUri && window.location.hash) {
                    const hash = window.location.hash.startsWith('#') ? window.location.hash.substring(1) : window.location.hash;
                    launchUri = new URLSearchParams(hash).get('uri') || '';
                }

                if (launchUri.startsWith('web+armsx:')) {
                    launchUri = 'armsx:' + launchUri.substring('web+armsx:'.length);
                }

                if (window.location.protocol === 'https:' && typeof navigator !== 'undefined' &&
                    typeof navigator.registerProtocolHandler === 'function') {
                    try {
                        navigator.registerProtocolHandler(
                            'web+armsx',
                            window.location.origin + window.location.pathname + '?uri=%s',
                            'ARMSX'
                        );
                    } catch (error) {
                        console.warn('ARMSX protocol handler registration skipped', error);
                    }
                }

                if (launchUri && typeof Module !== 'undefined' && typeof Module.ccall === 'function') {
                    Module.ccall('psxe_enqueue_launch_argument', null, ['string'], [launchUri]);
                }
            } catch (error) {
                console.warn('ARMSX web launch bootstrap failed', error);
            }
        });
#endif
    }

    void requestWebFiles(bool directory) {
#if defined(__EMSCRIPTEN__)
        if (directory) {
            EM_ASM({ window.ARMSXWebFiles.openDirectory(); });
        } else {
            EM_ASM({ window.ARMSXWebFiles.openFiles(); });
        }
#else
        (void)directory;
#endif
    }

    void queueLaunchArgument(std::string_view argument, bool close_ui, const char* error_message = "不支持此启动路径或 URI。") {
        const LaunchRequest request = LaunchForArgument(argument);
        psxe_diag_logf("launch", "Launch argument=%s kind=%s", std::string(argument).c_str(), LaunchKindTitle(request.kind));
        if (request.kind == LaunchKind::None) {
            pending_error_dialog_ = error_message;
            if (!session_.valid()) {
                showLandingWindow();
            }
            return;
        }

        queueLaunchRequest(request, close_ui);
    }

    void consumePendingLaunchArguments() {
        for (const std::string& message : DrainWebErrors()) {
            pending_error_dialog_ = message;
            if (!session_.valid()) {
                showLandingWindow();
            }
        }
        for (const std::string& argument : DrainPendingLaunchArguments()) {
            queueLaunchArgument(argument, true);
        }
    }

    void queueLaunchRequest(const LaunchRequest& request, bool close_ui) {
        if (request.kind == LaunchKind::None) {
            return;
        }

        deferred_launch_ = request;
        close_ui_after_launch_ = close_ui;
    }

    void queueLaunchSelection(const std::string& path, std::optional<LaunchKind> forced_kind = std::nullopt) {
        const std::optional<std::string> selection = NormalizedPickerSelection(path);
        if (!selection.has_value()) {
            return;
        }

        const LaunchRequest request = LaunchForArgument(*selection, forced_kind);
        if (request.kind != LaunchKind::None) {
            queueLaunchRequest(request, true);
        }
    }

    void queueDiscSwapSelection(const std::string& path) {
        const std::optional<std::string> selection = NormalizedPickerSelection(path);
        if (!selection.has_value()) {
            return;
        }

        deferred_change_disc_ = std::filesystem::path(*selection);
    }

    Uint32 managedRendererFlags(bool vsync_enabled) const {
        Uint32 renderer_flags = settings_.gpu_backend == GpuBackend::SDLAccelerated
            ? SDL_RENDERER_ACCELERATED
            : SDL_RENDERER_SOFTWARE;
#if !defined(__EMSCRIPTEN__)
        if (vsync_enabled) {
            renderer_flags |= SDL_RENDERER_PRESENTVSYNC;
        }
#endif
        return renderer_flags;
    }

#ifdef USE_HARDWARE
    bool hardwareRendererAvailable() const {
        if (!owns_renderer_ || external_renderer_ || !renderer_) {
            return false;
        }
        return SupportsHardwareGpuBackend();
    }
#endif

    bool createManagedRenderer(bool vsync_enabled) {
        renderer_ = SDL_CreateRenderer(window_, -1, managedRendererFlags(vsync_enabled));
        owns_renderer_ = renderer_ != nullptr;

        if (!renderer_ && settings_.gpu_backend == GpuBackend::SDLAccelerated) {
            psxe_diag_logf(
                "renderer",
                "SDL accelerated renderer unavailable error=%s; retrying software renderer.",
                SDL_GetError()
            );
            Uint32 fallback_flags = SDL_RENDERER_SOFTWARE;
#if !defined(__EMSCRIPTEN__)
            if (vsync_enabled) {
                fallback_flags |= SDL_RENDERER_PRESENTVSYNC;
            }
#endif
            renderer_ = SDL_CreateRenderer(window_, -1, fallback_flags);
            owns_renderer_ = renderer_ != nullptr;
        }

        if (!renderer_) {
            psxe_diag_logf(
                "renderer",
                "Renderer initialization failed requested_vsync=%s error=%s",
                vsync_enabled ? "true" : "false",
                SDL_GetError()
            );
            return false;
        }

        SDL_RendererInfo info{};
        if (SDL_GetRendererInfo(renderer_, &info) == 0) {
            psxe_diag_logf(
                "renderer",
                "Selected SDL renderer driver=%s flags=%s",
                info.name ? info.name : "(unknown)",
                RendererFlagsTitle(info.flags).c_str()
            );
        }

        managed_renderer_vsync_ = vsync_enabled;
        return true;
    }

    void shutdownFsuiFrontend() {
        if (fsui_initialized_) {
            fsui::Shutdown(true);
            fsui_initialized_ = false;
        }

        ImGuiFullscreen::ClearFileSelectorExtraRoots();

        imgui_backend_.shutdown();

        if (ImGui::GetCurrentContext()) {
            ImGui::DestroyContext();
        }
    }

    void refreshFileSelectorRoots() {
        std::vector<ImGuiFullscreen::FileSelectorRootEntry> roots{
            ImGuiFullscreen::FileSelectorRootEntry{
                .title = "ARMSX 文件夹",
                .path = DefaultBrowseDirectory().string(),
            },
        };

        ImGuiFullscreen::SetFileSelectorExtraRoots(std::move(roots));
    }

    void showLandingWindow() {
        ui_window_state_ = FsuiWindowState::Landing;
        fsui::ShowLandingWindow();
    }

    void showStartGameWindow() {
        ui_window_state_ = FsuiWindowState::StartGame;
        fsui::ShowStartGameWindow();
    }

    void showExitWindow() {
        ui_window_state_ = FsuiWindowState::Exit;
        fsui::ShowExitWindow();
    }

    void showGameListWindow() {
        ui_window_state_ = FsuiWindowState::GameList;
        fsui::ShowGameListWindow();
    }

    void switchToSettingsWindow() {
        ui_window_state_ = FsuiWindowState::Settings;
        fsui::SwitchToSettings();
    }

    void returnToMainWindow() {
        ui_window_state_ = session_.valid() ? FsuiWindowState::None : FsuiWindowState::Landing;
        fsui::ReturnToMainWindow();
    }

    void showPauseMenuWindow() {
        ui_window_state_ = FsuiWindowState::PauseMenu;
        fsui::OpenPauseMenu();
    }

    void restoreUiWindow() {
        switch (ui_window_state_) {
            case FsuiWindowState::Landing:
                showLandingWindow();
                break;
            case FsuiWindowState::StartGame:
                showStartGameWindow();
                break;
            case FsuiWindowState::Exit:
                showExitWindow();
                break;
            case FsuiWindowState::GameList:
                showGameListWindow();
                break;
            case FsuiWindowState::Settings:
                switchToSettingsWindow();
                break;
            case FsuiWindowState::PauseMenu:
                showPauseMenuWindow();
                break;
            case FsuiWindowState::None:
            default:
                break;
        }
    }

    bool recreateManagedRenderer(bool desired_vsync) {
        if (!owns_renderer_ || external_renderer_ || !window_) {
            return false;
        }

        const bool had_session = session_.valid();
        const bool was_paused = had_session ? session_.paused() : false;
        const FsuiWindowState restore_window = ui_window_state_;
        const bool previous_vsync = managed_renderer_vsync_;
        const bool should_be_paused_after_restore = had_session && (was_paused || restore_window != FsuiWindowState::None);

        auto restore_previous_renderer = [&](const char* phase, const char* message) {
            settings_.vsync_enabled = previous_vsync;
            SaveSettings(settings_);

            if (renderer_) {
                SDL_DestroyRenderer(renderer_);
                renderer_ = nullptr;
            }
            owns_renderer_ = false;

            if (!createManagedRenderer(previous_vsync) || !initializeFsui()) {
                psxe_diag_logf("renderer", "Renderer recovery failed phase=%s error=%s", phase ? phase : "(none)", SDL_GetError());
                running_ = false;
                return false;
            }

            if (had_session) {
                session_.rebindRenderer(renderer_, settings_);
                session_.setPaused(should_be_paused_after_restore);
            }

            ui_window_state_ = restore_window;
            restoreUiWindow();
            resetFramePacing("renderer-recovery");
            logRendererBootstrap(phase, currentTargetFrameRate());
            pending_error_dialog_ = message;
            return false;
        };

        if (had_session) {
            session_.setPaused(true);
        }

        shutdownFsuiFrontend();

        if (renderer_) {
            SDL_DestroyRenderer(renderer_);
            renderer_ = nullptr;
        }
        owns_renderer_ = false;

        if (!createManagedRenderer(desired_vsync)) {
            return restore_previous_renderer("renderer-recreate-recover", "无法应用所选 VSync 模式，已恢复之前的渲染器。");
        }

        if (!initializeFsui()) {
            return restore_previous_renderer("renderer-recreate-recover", "更改 VSync 后无法重建界面，已恢复之前的渲染器。");
        }

        if (had_session) {
            session_.rebindRenderer(renderer_, settings_);
            session_.setPaused(should_be_paused_after_restore);
        }

        ui_window_state_ = restore_window;
        restoreUiWindow();
        resetFramePacing("renderer-recreate");
        logRendererBootstrap("renderer-recreate", currentTargetFrameRate());
        return true;
    }

    void installCrashHandlers() {
        static bool installed = false;
        if (installed) {
            return;
        }

        installed = true;

        std::set_terminate([]() {
            ReportNativeCrash("std::terminate");
        });

        auto signal_handler = [](int signal_value) {
            switch (signal_value) {
                case SIGABRT:
                    ReportNativeCrash("SIGABRT");
                    break;
                case SIGSEGV:
                    ReportNativeCrash("SIGSEGV");
                    break;
                case SIGILL:
                    ReportNativeCrash("SIGILL");
                    break;
                case SIGFPE:
                    ReportNativeCrash("SIGFPE");
                    break;
                default:
                    ReportNativeCrash("signal");
                    break;
            }
        };

        std::signal(SIGABRT, signal_handler);
        std::signal(SIGSEGV, signal_handler);
        std::signal(SIGILL, signal_handler);
        std::signal(SIGFPE, signal_handler);

#if defined(_WIN32) && !defined(UWP_TARGET)
        SetUnhandledExceptionFilter(&WindowsUnhandledExceptionFilter);
#endif
    }

    double currentTargetFrameRate() const {
        if (session_.valid() && !session_.paused()) {
            return session_.targetFrameRate();
        }

        return kUiFrameRate;
    }

    void updateFramePeriod(double frame_rate) {
#if !defined(__EMSCRIPTEN__)
        const double safe_rate = std::max(frame_rate, 1.0);
        const uint64_t frequency = SDL_GetPerformanceFrequency();
        const uint64_t ticks = std::max<uint64_t>(
            1,
            static_cast<uint64_t>(std::llround(static_cast<double>(frequency) / safe_rate))
        );

        if (frame_period_ticks_ != ticks) {
            frame_period_ticks_ = ticks;
            next_frame_deadline_ = 0;
            psxe_diag_breadcrumbf(
                "Frame pacing target updated fps=%.2f period_ms=%.3f",
                safe_rate,
                CounterTicksToMilliseconds(frame_period_ticks_)
            );
        }
#else
        (void)frame_rate;
#endif
    }

    void waitForFrameDeadline(double frame_rate) {
#if !defined(__EMSCRIPTEN__)
        updateFramePeriod(frame_rate);

        const uint64_t frequency = SDL_GetPerformanceFrequency();
        const uint64_t slack_ticks = std::max<uint64_t>(1, frequency / 2000u);
        uint64_t now = SDL_GetPerformanceCounter();

        if (next_frame_deadline_ == 0) {
            next_frame_deadline_ = now;
            return;
        }

        if (frame_period_ticks_ != 0 && now > (next_frame_deadline_ + (frame_period_ticks_ * 2u))) {
            psxe_diag_breadcrumbf(
                "Frame pacing resync lateness_ms=%.3f",
                CounterTicksToMilliseconds(now - next_frame_deadline_)
            );
            next_frame_deadline_ = now;
            return;
        }

        while (now + slack_ticks < next_frame_deadline_) {
            const double wait_ms = CounterTicksToMilliseconds(next_frame_deadline_ - now);
            if (wait_ms > 1.5) {
                SDL_Delay(static_cast<Uint32>(wait_ms - 1.0));
            } else {
                std::this_thread::yield();
            }
            now = SDL_GetPerformanceCounter();
        }

        while (now < next_frame_deadline_) {
            std::this_thread::yield();
            now = SDL_GetPerformanceCounter();
        }
#else
        (void)frame_rate;
#endif
    }

    void advanceFrameDeadline() {
#if !defined(__EMSCRIPTEN__)
        if (frame_period_ticks_ == 0) {
            return;
        }

        if (next_frame_deadline_ == 0) {
            next_frame_deadline_ = SDL_GetPerformanceCounter();
        }

        next_frame_deadline_ += frame_period_ticks_;
#endif
    }

    void resetFramePacing(const char* reason) {
#if !defined(__EMSCRIPTEN__)
        next_frame_deadline_ = 0;
        frame_period_ticks_ = 0;
        if (reason && reason[0]) {
            psxe_diag_breadcrumbf("Frame pacing reset reason=%s", reason);
        }
#else
        (void)reason;
#endif
    }

    std::filesystem::path diagnosticsLogPath() const {
        const char* live_path = psxe_diag_log_path();
        if (live_path && live_path[0]) {
            return std::filesystem::path(live_path);
        }

        return DefaultDiagnosticsLogPath();
    }

    void applyLoggingSettings(const char* reason) {
#if defined(IOS_TARGET)
        settings_.logging_enabled = true;
        const bool enable_logs = true;
#else
        const bool enable_logs = settings_.logging_enabled;
#endif
        settings_.quiet = !enable_logs;

        if (!enable_logs && psxe_diag_is_enabled()) {
            psxe_diag_logf("diag", "Debug logging disabled reason=%s", reason ? reason : "(none)");
        }

        psxe_diag_set_enabled(enable_logs ? 1 : 0);
        log_set_level(settings_.log_level);
        log_set_quiet(settings_.quiet ? 1 : 0);

        if (enable_logs) {
            psxe_diag_logf(
                "diag",
                "Debug logging enabled reason=%s level=%s path=%s",
                reason ? reason : "(none)",
                log_level_string(settings_.log_level),
                diagnosticsLogPath().string().c_str()
            );
        }
    }

    void logRendererBootstrap(const char* phase, double frame_rate) const {
        if (!renderer_) {
            return;
        }

        SDL_RendererInfo info{};
        SDL_GetRendererInfo(renderer_, &info);

        int output_width = 0;
        int output_height = 0;
        SDL_GetRendererOutputSize(renderer_, &output_width, &output_height);

        int refresh_rate = 0;
        SDL_DisplayMode display_mode{};
        if (window_) {
            const int display_index = SDL_GetWindowDisplayIndex(window_);
            if (display_index >= 0 && SDL_GetCurrentDisplayMode(display_index, &display_mode) == 0) {
                refresh_rate = display_mode.refresh_rate;
            }
        }

        const char* timing_title = session_.valid() ? session_.timingModeTitle() : "UI";
        psxe_diag_logf(
            "renderer",
            "phase=%s source=%s driver=%s flags=%s output=%dx%d refresh_hz=%d timing=%s target_fps=%.2f frame_period_ms=%.3f requested_vsync=%s",
            phase ? phase : "(none)",
            external_renderer_ ? "external" : "internal",
            info.name ? info.name : "(unknown)",
            RendererFlagsTitle(info.flags).c_str(),
            output_width,
            output_height,
            refresh_rate,
            timing_title,
            frame_rate,
            frame_rate > 0.0 ? (1000.0 / frame_rate) : 0.0,
            (owns_renderer_ && !external_renderer_) ? (managed_renderer_vsync_ ? "true" : "false") : "host-controlled"
        );
    }

    void logUiRendererState(const char* stage) const {
        if (!renderer_) {
            return;
        }

        int window_width = 0;
        int window_height = 0;
        if (window_) {
            SDL_GetWindowSize(window_, &window_width, &window_height);
        }

        int output_width = 0;
        int output_height = 0;
        SDL_GetRendererOutputSize(renderer_, &output_width, &output_height);

        SDL_Rect viewport = {0, 0, 0, 0};
        SDL_Rect clip_rect = {0, 0, 0, 0};
        int logical_width = 0;
        int logical_height = 0;
        float scale_x = 0.0f;
        float scale_y = 0.0f;

        SDL_RenderGetViewport(renderer_, &viewport);
        SDL_RenderGetClipRect(renderer_, &clip_rect);
        SDL_RenderGetLogicalSize(renderer_, &logical_width, &logical_height);
        SDL_RenderGetScale(renderer_, &scale_x, &scale_y);

        const SDL_Texture* target = SDL_GetRenderTarget(renderer_);
        const Uint32 window_flags = window_ ? SDL_GetWindowFlags(window_) : 0u;
        const bool clip_enabled = SDL_RenderIsClipEnabled(renderer_) == SDL_TRUE;
        const bool integer_scale = SDL_RenderGetIntegerScale(renderer_) == SDL_TRUE;

        float imgui_display_x = 0.0f;
        float imgui_display_y = 0.0f;
        float imgui_scale_x = 0.0f;
        float imgui_scale_y = 0.0f;
        float imgui_font_scale = 0.0f;
        int imgui_backend_flags = 0;
        int imgui_config_flags = 0;
        if (ImGui::GetCurrentContext()) {
            const ImGuiIO& io = ImGui::GetIO();
            imgui_display_x = io.DisplaySize.x;
            imgui_display_y = io.DisplaySize.y;
            imgui_scale_x = io.DisplayFramebufferScale.x;
            imgui_scale_y = io.DisplayFramebufferScale.y;
            imgui_font_scale = io.FontGlobalScale;
            imgui_backend_flags = static_cast<int>(io.BackendFlags);
            imgui_config_flags = static_cast<int>(io.ConfigFlags);
        }

        psxe_diag_logf(
            "ui",
            "%s window=%dx%d window_flags=0x%x output=%dx%d target=%p viewport=%d,%d %dx%d clip=%d,%d %dx%d scale=(%f,%f) logical=%dx%d clip_enabled=%s integer_scale=%s imgui_display=(%.1f,%.1f) imgui_fb_scale=(%.2f,%.2f) imgui_font_scale=%.3f backend_flags=0x%x config_flags=0x%x session_hw=%s texture=%dx%d format=%s",
            stage ? stage : "(state)",
            window_width,
            window_height,
            window_flags,
            output_width,
            output_height,
            (const void*)target,
            viewport.x,
            viewport.y,
            viewport.w,
            viewport.h,
            clip_rect.x,
            clip_rect.y,
            clip_rect.w,
            clip_rect.h,
            scale_x,
            scale_y,
            logical_width,
            logical_height,
            clip_enabled ? "true" : "false",
            integer_scale ? "true" : "false",
            imgui_display_x,
            imgui_display_y,
            imgui_scale_x,
            imgui_scale_y,
            imgui_font_scale,
            imgui_backend_flags,
            imgui_config_flags,
#ifdef USE_HARDWARE
            session_.valid() ? (session_.hardwareBackendActive() ? "true" : "false") : "false",
            session_.valid() ? session_.textureWidth() : 0,
            session_.valid() ? session_.textureHeight() : 0,
            session_.valid() ? SDL_GetPixelFormatName(session_.textureFormat()) : "(none)"
#else
            "false",
            0,
            0,
            "(none)"
#endif
        );
    }

    void logCpuState() const {
        if (!session_.valid() || !session_.psx() || !session_.psx()->cpu) {
            psxe_diag_logf("crash", "CPU state unavailable.");
            return;
        }

        const psx_cpu_t* cpu = session_.psx()->cpu;
        psxe_diag_logf("crash", "r0=%08x at=%08x v0=%08x v1=%08x", cpu->r[0], cpu->r[1], cpu->r[2], cpu->r[3]);
        psxe_diag_logf("crash", "a0=%08x a1=%08x a2=%08x a3=%08x", cpu->r[4], cpu->r[5], cpu->r[6], cpu->r[7]);
        psxe_diag_logf("crash", "t0=%08x t1=%08x t2=%08x t3=%08x", cpu->r[8], cpu->r[9], cpu->r[10], cpu->r[11]);
        psxe_diag_logf("crash", "t4=%08x t5=%08x t6=%08x t7=%08x", cpu->r[12], cpu->r[13], cpu->r[14], cpu->r[15]);
        psxe_diag_logf("crash", "s0=%08x s1=%08x s2=%08x s3=%08x", cpu->r[16], cpu->r[17], cpu->r[18], cpu->r[19]);
        psxe_diag_logf("crash", "s4=%08x s5=%08x s6=%08x s7=%08x", cpu->r[20], cpu->r[21], cpu->r[22], cpu->r[23]);
        psxe_diag_logf("crash", "t8=%08x t9=%08x k0=%08x k1=%08x", cpu->r[24], cpu->r[25], cpu->r[26], cpu->r[27]);
        psxe_diag_logf("crash", "gp=%08x sp=%08x fp=%08x ra=%08x", cpu->r[28], cpu->r[29], cpu->r[30], cpu->r[31]);
        psxe_diag_logf(
            "crash",
            "pc=%08x next=%08x saved=%08x hi=%08x lo=%08x epc=%08x opcode=%08x",
            cpu->pc,
            cpu->next_pc,
            cpu->saved_pc,
            cpu->hi,
            cpu->lo,
            cpu->cop0_r[COP0_EPC],
            cpu->opcode
        );
    }

    bool initializeSdl() {
        const Uint32 required = SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER;
        psxe_diag_breadcrumbf("Initializing SDL subsystems flags=0x%x", required);

        // Keep SDL's touch-to-mouse bridge enabled on iOS.  Some iOS SDL
        // backends expose primary touches as mouse events only; disabling
        // this makes the frontend appear completely non-interactive on
        // affected devices.  Multi-touch game input is still handled through
        // SDL_FINGER* events when available.
        SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "1");
        SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "1");

        if (SDL_WasInit(0) == 0) {
            owns_sdl_ = true;
            if (SDL_Init(required) != 0) {
                psxe_diag_logf("sdl", "SDL_Init failed: %s", SDL_GetError());
                return false;
            }
        } else if (SDL_InitSubSystem(required) != 0) {
            psxe_diag_logf("sdl", "SDL_InitSubSystem failed: %s", SDL_GetError());
            return false;
        }

        if (argc_ > 1) {
            for (int index = 1; index < argc_; index++) {
                const std::string_view arg(argv_[index] ? argv_[index] : "");

                auto set_boot_argument = [&](std::string_view value, std::optional<LaunchKind> forced_kind = std::nullopt) {
                    pending_cli_argument_ = std::string(value);
                    pending_cli_launch_ = LaunchForArgument(value, forced_kind);
                };

                if (arg == "--cdrom") {
                    if ((index + 1) < argc_) {
                        set_boot_argument(argv_[index + 1] ? argv_[index + 1] : "", LaunchKind::Disc);
                        index++;
                    }
                } else if (arg == "-x" || arg == "--exe") {
                    if ((index + 1) < argc_) {
                        set_boot_argument(argv_[index + 1] ? argv_[index + 1] : "", LaunchKind::Exe);
                        index++;
                    }
                } else if (arg.starts_with("--cdrom=")) {
                    set_boot_argument(arg.substr(8), LaunchKind::Disc);
                } else if (arg.starts_with("--exe=")) {
                    set_boot_argument(arg.substr(6), LaunchKind::Exe);
                } else if (!arg.empty() && arg[0] != '-') {
                    set_boot_argument(arg);
                }
            }
        }

        return true;
    }

    bool initializeWindowAndRenderer() {
        if (external_window_) {
            window_ = external_window_;
        } else {
            int window_width = 1280;
            int window_height = 720;
#if defined(__EMSCRIPTEN__)
            double css_width = 0.0;
            double css_height = 0.0;
            if (emscripten_get_element_css_size("#canvas", &css_width, &css_height) == EMSCRIPTEN_RESULT_SUCCESS &&
                css_width > 0.0 && css_height > 0.0) {
                window_width = std::max(1, static_cast<int>(std::lround(css_width)));
                window_height = std::max(1, static_cast<int>(std::lround(css_height)));
            }
#endif
            Uint32 flags = 0;
            if (SupportsManagedWindowSizing()) {
                flags |= SDL_WINDOW_RESIZABLE;
            }
#if defined(__ANDROID__) || defined(IOS_TARGET)
            flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
#endif
            window_ = SDL_CreateWindow(
                "ARMSX",
                SDL_WINDOWPOS_CENTERED,
                SDL_WINDOWPOS_CENTERED,
                window_width,
                window_height,
                flags
            );
            owns_window_ = window_ != nullptr;
        }

        if (!window_) {
            psxe_diag_logf("renderer", "Window initialization failed: %s", SDL_GetError());
            return false;
        }

        if (external_renderer_) {
            renderer_ = external_renderer_;
            owns_renderer_ = false;
        } else {
            renderer_ = nullptr;
            if (!createManagedRenderer(settings_.vsync_enabled)) {
                return false;
            }
        }

        if (!renderer_) {
            psxe_diag_logf("renderer", "Renderer initialization failed: %s", SDL_GetError());
        }

        return renderer_ != nullptr;
    }

    bool initializeFsui() {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
        io.IniFilename = nullptr;

        fsui::SdlImGuiBackendConfig backend_config;
        backend_config.window = window_;
        backend_config.sdl_renderer = renderer_;
        backend_config.renderer_backend = fsui::RendererBackend::SDLRenderer;
#if defined(__EMSCRIPTEN__)
        // SDL_Renderer on Emscripten renders correctly when the canvas backing store
        // matches the CSS size instead of a DPR-scaled framebuffer.
        backend_config.web_canvas.max_device_pixel_ratio = 1.0;
#endif

        if (!imgui_backend_.initialize(backend_config)) {
            return false;
        }

#if defined(__EMSCRIPTEN__)
        fsui::SyncSdl2WebCanvasLayout(window_, backend_config.web_canvas, true);
#endif

        fsui::FontStack fonts;
        if (!fsui::BuildDefaultFontStack(fsui::DescribeSdl2FontBootstrap(window_), fonts)) {
            return false;
        }

        fsui::UiContext context;
        context.window = window_;
        context.fonts = fonts;
        context.app_title = "ARMSX";
        context.app_icon_path = ResolveFsuiAppIconPath();
        context.platform_backend = fsui::PlatformBackendKind::SDL2;
        context.renderer_backend = fsui::RendererBackend::SDLRenderer;
#if defined(__ANDROID__) || defined(IOS_TARGET)
        context.default_prompt_icon_pack = fsui::PromptIconPack::PlayStation;
#endif

        fsui::Host host;
        host.ui_state = &settings_.ui_state;
        host.has_running_game = [this]() { return session_.valid(); };
        host.get_current_game = [this]() { return session_.currentGameInfo(); };
        host.get_game_list = [this]() { return game_list_; };
        host.refresh_game_list = [this](bool full_rescan) { refreshGameList(full_rescan); };
        host.persist_ui_state = [this](bool reload) {
            if (reload) {
                refreshGameList(true);
            }
            SaveSettings(settings_);
        };
        host.set_paused = [this](bool paused) {
            if (!session_.valid()) {
                return;
            }
            session_.setPaused(paused);
            resetFramePacing(paused ? "fsui-pause" : "fsui-unpause");
            if (paused) {
                input_router_.onFsuiOpened();
            }
        };
        host.resume_game = [this]() {
            if (session_.valid()) {
                session_.setPaused(false);
                resetFramePacing("fsui-resume");
            }
        };
        host.reset_system = [this]() {
            if (session_.valid()) {
                restartCurrentSession("fsui-host-reset");
            }
        };
        host.exit_to_library = [this]() { exitToLibrary(); };
        host.request_quit = [this]() { running_ = false; };
        host.request_classic_ui = []() {};
        host.close_selector = []() {};
        host.detect_prompt_icon_pack = []() { return fsui::DetectPromptIconPackFromSDL(); };
        host.detect_swap_north_west_gamepad_buttons = []() { return false; };
        host.runtime_overlay_options = fsui::RuntimeOverlayOptions{
            .show_inputs = true,
            .show_settings = true,
            .show_performance = true,
        };
        host.get_input_overlay_devices = [this]() { return input_router_.buildInputOverlay(); };
        host.get_settings_overlay_lines = [this]() { return session_.settingsOverlayLines(settings_); };
        host.get_performance_overlay_lines = [this]() { return session_.performanceOverlayLines(settings_); };
        host.get_landing_items = [this]() { return buildLandingItems(); };
        host.get_start_items = [this]() { return buildStartItems(); };
        host.get_exit_items = [this]() { return buildExitItems(); };
        host.get_pause_items = [this]() { return buildPauseItems(); };
        host.get_game_launch_options = [this](const fsui::GameEntry& entry) { return buildGameLaunchOptions(entry); };
        host.get_settings_pages = [this](fsui::SettingsScope scope) { return buildSettingsPages(scope); };

        context.host = std::move(host);
        refreshFileSelectorRoots();

        if (!fsui::Initialize(context)) {
            return false;
        }

        fsui_initialized_ = true;
        return true;
    }

    void shutdown() {
        psxe_diag_breadcrumbf("Frontend shutdown");
        input_router_.detach();
        session_.destroy();

        shutdownFsuiFrontend();

        if (owns_renderer_ && renderer_) {
            SDL_DestroyRenderer(renderer_);
        }
        if (owns_window_ && window_) {
            SDL_DestroyWindow(window_);
        }

        renderer_ = nullptr;
        window_ = nullptr;

        if (owns_sdl_) {
            SDL_Quit();
        }

        psxe_diag_shutdown();
    }

    void handleEvent(const SDL_Event& event) {
        imgui_backend_.processEvent(event);
        mobile_controls_.processEvent(event);

        if (event.type == SDL_QUIT) {
            running_ = false;
            return;
        }

        if (event.type == SDL_DROPFILE) {
            if (event.drop.file) {
                queueLaunchArgument(event.drop.file, true);
                SDL_free(event.drop.file);
            }
            return;
        }

        if (event.type == SDL_DROPCOMPLETE) {
            return;
        }

        const bool fsui_active = fsui::HasActiveWindow();
        input_router_.processEvent(event, session_.valid() ? &session_ : nullptr, fsui_active);
        if (input_router_.takeControllerDisconnected()) {
            mobile_controls_.onControllerDisconnected();
        }
        if (input_router_.takeControllerActivity()) {
            mobile_controls_.onControllerActivity(input_router_);
        }

        if (event.type == SDL_KEYDOWN && !fsui_active && session_.valid() && event.key.repeat == 0 && event.key.keysym.sym == SDLK_RETURN) {
            psx_exp2_atcons_put(session_.psx()->exp2, 13);
        }

        const bool mobile_pause = settings_.mobile_controls && mobile_controls_.takePauseRequest();
        if (session_.valid() && !fsui_active && (input_router_.takePauseRequest() || mobile_pause)) {
            openPauseMenu();
        }
    }

    void runFrame() {
        const bool fsui_was_active = fsui::HasActiveWindow();
        input_router_.tick(fsui_was_active);
        syncPlatformLibraries(true, true);

        if (deferred_vsync_.has_value()) {
            const bool desired_vsync = *deferred_vsync_;
            deferred_vsync_.reset();

            if (owns_renderer_ && !external_renderer_ && desired_vsync != managed_renderer_vsync_) {
                recreateManagedRenderer(desired_vsync);
                if (!running_) {
                    return;
                }
            }
        }

        consumePendingLaunchArguments();

        waitForFrameDeadline(currentTargetFrameRate());

        std::uint32_t session_steps = 0;
        if (session_.valid() && !session_.paused()) {
            session_steps = session_.runFrame();
            session_.updateTexture(settings_);
            session_.finishHardwareFrame();
        }

#if defined(USE_HARDWARE) && defined(HW_DEBUG)
        if (session_.valid() && session_.hardwareBackendActive()) {
            logUiRendererState("ui-pre-imgui");
            psxe_diag_logf(
                "hw",
                "frame-present frame=%llu steps=%u paused=%s fast_forward=%s texture=%dx%d format=%s target_fps=%.3f",
                static_cast<unsigned long long>(session_.vblankCounter()),
                session_steps,
                session_.paused() ? "true" : "false",
                session_.fastForwardEnabled() ? "true" : "false",
                session_.textureWidth(),
                session_.textureHeight(),
                SDL_GetPixelFormatName(session_.textureFormat()),
                session_.targetFrameRate()
            );
        }
#endif
        imgui_backend_.newFrame();
#if defined(USE_HARDWARE) && defined(HW_DEBUG)
        if (session_.valid() && session_.hardwareBackendActive()) {
            logUiRendererState("ui-post-imgui-backend");
        }
#endif
        ImGui::NewFrame();

        if (session_.valid()) {
            session_.draw(settings_);
        }

        if (session_.valid() && settings_.mobile_controls && !fsui::HasActiveWindow()) {
            mobile_controls_.drawAndRoute(input_router_);
        } else {
            mobile_controls_.reset(input_router_);
        }

        if (pending_error_dialog_.has_value()) {
            ImGuiFullscreen::OpenInfoMessageDialog("ARMSX", *pending_error_dialog_);
            pending_error_dialog_.reset();
        }

        fsui::Render();

        for (const fsui::Command& command : fsui::ConsumeCommands()) {
            applyCommand(command);
        }

        applyDeferredActions();

        ImGui::Render();
        imgui_backend_.renderDrawData(ImVec4(0.0f, 0.0f, 0.0f, 1.0f));

#if defined(USE_HARDWARE) && defined(HW_DEBUG)
        if (session_.valid() && session_.hardwareBackendActive()) {
            logUiRendererState("ui-post-imgui-render");
        }
#endif

        const bool fsui_is_active = fsui::HasActiveWindow();

        if (fsui_was_active && !fsui_is_active) {
            ui_window_state_ = FsuiWindowState::None;
            if (session_.valid()) {
                input_router_.onFsuiClosed();
            }
        } else if (!fsui_was_active && fsui_is_active && session_.valid()) {
            input_router_.onFsuiOpened();
        }

        advanceFrameDeadline();
    }

    void applyCommand(const fsui::Command& command) {
        psxe_diag_breadcrumbf("Frontend command type=%d", static_cast<int>(command.type));

        switch (command.type) {
            case fsui::CommandType::LaunchPath:
                queueLaunchArgument(command.path.string(), true);
                break;

            case fsui::CommandType::RequestQuit:
                if (session_.valid()) {
                    exitToLibrary();
                } else {
                    running_ = false;
                }
                break;

            case fsui::CommandType::Resume:
                if (session_.valid()) {
                    fsui::ReturnToMainWindow();
                    ui_window_state_ = FsuiWindowState::None;
                    session_.setPaused(false);
                    resetFramePacing("command-resume");
                }
                break;

            case fsui::CommandType::Reset:
                if (session_.valid()) {
                    restartCurrentSession("fsui-command-reset");
                }
                break;

            case fsui::CommandType::ExitToLibrary:
                exitToLibrary();
                break;

            case fsui::CommandType::CloseSelector:
            case fsui::CommandType::ExitFullscreenUI:
            case fsui::CommandType::SwitchShell:
            case fsui::CommandType::Custom:
            default:
                break;
        }
    }

    void applyDeferredActions() {
        if (deferred_launch_.has_value()) {
            const LaunchRequest request = *deferred_launch_;
            deferred_launch_.reset();
            psxe_diag_breadcrumbf("Deferred launch kind=%s path=%s",
                LaunchKindTitle(request.kind),
                request.path.empty() ? "(none)" : request.path.string().c_str());
            if (request.kind != LaunchKind::None) {
                launchSession(request, close_ui_after_launch_);
            }
            close_ui_after_launch_ = false;
        }

        if (deferred_exit_to_library_) {
            deferred_exit_to_library_ = false;
            exitToLibrary();
        }

        if (deferred_reset_) {
            deferred_reset_ = false;
            if (session_.valid()) {
                restartCurrentSession("settings-reset");
            }
        }

        if (deferred_change_disc_.has_value()) {
            const std::filesystem::path path = *deferred_change_disc_;
            deferred_change_disc_.reset();

            if (path.empty()) {
                returnToMainWindow();
                return;
            }

            if (!session_.swapDisc(path)) {
                pending_error_dialog_ = "无法换入所选光盘镜像。";
            } else {
                session_.setPaused(false);
                resetFramePacing("change-disc");
                refreshGameList(false);
                returnToMainWindow();
            }
        }

        if (deferred_screenshot_) {
            deferred_screenshot_ = false;

            const std::filesystem::path snap_dir = DefaultBrowseDirectory() / "snap";
            try {
                std::filesystem::create_directories(snap_dir);
                const std::time_t now = std::time(nullptr);
                char name[64] = {};
                std::strftime(name, sizeof(name), "armsx-%Y%m%d-%H%M%S.bmp", std::localtime(&now));
                if (!session_.saveScreenshot(snap_dir / name)) {
                    pending_error_dialog_ = "无法写入截图文件。";
                }
            } catch (...) {
                pending_error_dialog_ = "无法创建截图文件夹。";
            }
        }

        if (deferred_fast_forward_toggle_) {
            deferred_fast_forward_toggle_ = false;
            if (session_.valid()) {
                session_.setFastForwardEnabled(!session_.fastForwardEnabled());
                session_.setPaused(false);
                resetFramePacing("toggle-fast-forward");
                returnToMainWindow();
            }
        }
    }

    bool launchSession(const LaunchRequest& request, bool close_ui) {
        std::string error;
        psxe_diag_breadcrumbf("Launching session kind=%s path=%s close_ui=%s",
            LaunchKindTitle(request.kind),
            request.path.empty() ? "(none)" : request.path.string().c_str(),
            close_ui ? "true" : "false");

        if (!session_.create(renderer_, settings_, request, error)) {
            pending_error_dialog_ = error;
            psxe_diag_logf("launch", "Session launch failed kind=%s error=%s", LaunchKindTitle(request.kind), error.c_str());
            if (request.kind == LaunchKind::Bios) {
                showLandingWindow();
            } else {
                showGameListWindow();
            }
            return false;
        }

        input_router_.attach(session_.pad());
        mobile_controls_.showNow();
        resetFramePacing("launch-session");
        applyWindowMetrics();
        logRendererBootstrap("session-launch", session_.frameRate());
        refreshGameList(false);

        if (close_ui) {
            returnToMainWindow();
        }

        return true;
    }

    bool restartCurrentSession(const char* reason) {
        if (!session_.valid()) {
            return false;
        }

        const LaunchRequest request = session_.currentLaunchRequest();
        if (request.kind == LaunchKind::None ||
            ((request.kind == LaunchKind::Disc || request.kind == LaunchKind::Exe) && request.path.empty())) {
            psxe_diag_logf("ui", "Full reset rejected: incomplete launch request reason=%s",
                reason ? reason : "(unknown)");
            return false;
        }

        psxe_diag_breadcrumbf("Full session reset requested reason=%s kind=%s path=%s",
            reason ? reason : "(unknown)",
            LaunchKindTitle(request.kind),
            request.path.empty() ? "(none)" : request.path.string().c_str());

        input_router_.detach();
        if (!launchSession(request, true)) {
            psxe_diag_logf("ui", "Full session reset failed reason=%s kind=%s path=%s",
                reason ? reason : "(unknown)",
                LaunchKindTitle(request.kind),
                request.path.empty() ? "(none)" : request.path.string().c_str());
            return false;
        }

        psxe_diag_logf("ui", "Full session reset completed reason=%s kind=%s path=%s",
            reason ? reason : "(unknown)",
            LaunchKindTitle(request.kind),
            request.path.empty() ? "(none)" : request.path.string().c_str());
        return true;
    }

    void exitToLibrary() {
        psxe_diag_breadcrumbf("Exit to library requested");
        input_router_.detach();
        session_.destroy();
        resetFramePacing("exit-to-library");
        showLandingWindow();
    }

    void openPauseMenu() {
        if (!session_.valid()) {
            return;
        }

        session_.setPaused(true);
        resetFramePacing("open-pause-menu");
        input_router_.onFsuiOpened();
        showPauseMenuWindow();
    }

    bool syncPlatformLibraries(bool persist, bool refresh) {
        std::vector<PlatformLibraryRoot> roots;
        std::vector<PlatformLibraryGame> games;
        const std::uint64_t version = CopyPlatformLibrarySnapshot(roots, games);
        if (version == 0 || version == platform_library_version_) {
            return false;
        }

#if defined(__ANDROID__)
        auto remove_virtual_roots = [](std::vector<std::filesystem::path>& paths) {
            paths.erase(
                std::remove_if(paths.begin(), paths.end(), [](const std::filesystem::path& path) {
                    return IsAndroidVirtualPath(path.string());
                }),
                paths.end()
            );
        };
        remove_virtual_roots(settings_.ui_state.game_list_paths);
        remove_virtual_roots(settings_.ui_state.game_list_recursive_paths);
#endif

        platform_roots_ = std::move(roots);
        platform_games_ = std::move(games);
        for (const PlatformLibraryRoot& root : platform_roots_) {
            auto& paths = root.recursive
                ? settings_.ui_state.game_list_recursive_paths
                : settings_.ui_state.game_list_paths;
            const std::filesystem::path path(root.path);
            if (!PathListContains(paths, path)) {
                paths.push_back(path);
            }
        }
        platform_library_version_ = version;

        if (refresh) {
            refreshGameList(true);
        }
        if (persist) {
            SaveSettings(settings_);
        }
        return true;
    }

    void refreshGameList(bool full_rescan) {
        (void)full_rescan;
        game_list_.clear();
        std::vector<std::filesystem::path> seen_game_paths;
        std::vector<std::pair<std::filesystem::path, bool>> scan_roots;

        auto add_scan_root = [&](const std::filesystem::path& root, bool recursive) {
            if (root.empty() || IsAndroidVirtualPath(root.string())) {
                return;
            }

            for (auto& entry : scan_roots) {
                if (PathsMatch(entry.first, root)) {
                    entry.second = entry.second || recursive;
                    return;
                }
            }

            scan_roots.emplace_back(root, recursive);
        };

        auto scan_root = [&](const std::filesystem::path& root, bool recursive) {
            try {
                if (!std::filesystem::exists(root)) {
                    return;
                }

                auto append_entry = [&](const std::filesystem::directory_entry& item) {
                    if (!item.is_regular_file()) {
                        return;
                    }

                    const std::filesystem::path path = item.path();
                    if (!IsDiscPath(path) && !IsExePath(path)) {
                        return;
                    }
                    if (IsLikelyBiosImagePath(path, settings_)) {
                        return;
                    }
                    if (ToLower(path.extension().string()) == ".bin" && CueDirectoryReferencesImage(path)) {
                        return;
                    }
                    if (PathListContains(seen_game_paths, path)) {
                        return;
                    }
                    seen_game_paths.push_back(path);

                    fsui::GameEntry entry;
                    entry.path = path;
                    entry.type = IsExePath(path) ? fsui::GameEntryType::Homebrew : fsui::GameEntryType::Application;
                    entry.title = StemToTitle(path);
                    entry.title_sort = ToLower(entry.title);
                    entry.english_title = entry.title;
                    entry.title_id = NormalizeModel(entry.title);
                    entry.file_size = item.file_size();
                    entry.modified_time = FileTimeToTimeT(item.last_write_time());

                    const int region = RegionFromSettings(settings_);
                    if (region == CDR_REGION_EUROPE) {
                        entry.region = "欧洲";
                        entry.region_flag_path = "icons/flags/eu.png";
                    } else if (region == CDR_REGION_JAPAN) {
                        entry.region = "日本";
                        entry.region_flag_path = "icons/flags/jp.png";
                    } else {
                        entry.region = "北美";
                        entry.region_flag_path = "icons/flags/us.png";
                    }

                    game_list_.push_back(std::move(entry));
                };

                if (recursive) {
                    for (const auto& item : std::filesystem::recursive_directory_iterator(root)) {
                        append_entry(item);
                    }
                } else {
                    for (const auto& item : std::filesystem::directory_iterator(root)) {
                        append_entry(item);
                    }
                }
            } catch (...) {
            }
        };

        for (const auto& root : settings_.ui_state.game_list_paths) {
            add_scan_root(root, false);
        }

        for (const auto& root : settings_.ui_state.game_list_recursive_paths) {
            add_scan_root(root, true);
        }

#if defined(UWP_TARGET) || defined(IOS_TARGET)
        add_scan_root(DefaultBrowseDirectory(), true);
#endif

        for (const auto& [root, recursive] : scan_roots) {
            scan_root(root, recursive);
        }

        for (const PlatformLibraryGame& game : platform_games_) {
            const auto root = std::find_if(platform_roots_.begin(), platform_roots_.end(), [&](const PlatformLibraryRoot& candidate) {
                return PlatformPathBelongsToRoot(game.path, candidate.path, candidate.recursive);
            });
            if (root == platform_roots_.end()) {
                continue;
            }

            const std::filesystem::path path(game.path);
            if ((!IsDiscPath(path) && !IsExePath(path) && !IsZipPath(path)) ||
                IsLikelyBiosImagePath(path, settings_) ||
                PathListContains(seen_game_paths, path)) {
                continue;
            }
            seen_game_paths.push_back(path);

            fsui::GameEntry entry;
            entry.path = path;
            entry.type = IsExePath(path) ? fsui::GameEntryType::Homebrew : fsui::GameEntryType::Application;
            entry.title = game.label.empty() ? StemToTitle(path) : game.label;
            entry.title_sort = ToLower(entry.title);
            entry.english_title = entry.title;
            entry.title_id = NormalizeModel(entry.title);
            entry.file_size = game.size;
            entry.modified_time = game.modified;

            const int region = RegionFromSettings(settings_);
            if (region == CDR_REGION_EUROPE) {
                entry.region = "欧洲";
                entry.region_flag_path = "icons/flags/eu.png";
            } else if (region == CDR_REGION_JAPAN) {
                entry.region = "日本";
                entry.region_flag_path = "icons/flags/jp.png";
            } else {
                entry.region = "北美";
                entry.region_flag_path = "icons/flags/us.png";
            }
            game_list_.push_back(std::move(entry));
        }

        std::sort(game_list_.begin(), game_list_.end(), [](const fsui::GameEntry& left, const fsui::GameEntry& right) {
            if (left.title_sort == right.title_sort) {
                return left.path < right.path;
            }
            return left.title_sort < right.title_sort;
        });
    }

    std::vector<fsui::MenuItemDescriptor> buildLandingItems() {
        std::vector<fsui::MenuItemDescriptor> items;

        fsui::MenuItemDescriptor game_list;
        game_list.id = "game-list";
        game_list.icon_path = fsui::GetBuiltInStartupIconPath(fsui::BuiltInStartupIcon::GameList);
        game_list.title = "游戏列表";
        game_list.summary = "浏览游戏目录并启动游戏。";
        game_list.on_activate = [this]() { showGameListWindow(); };
        items.push_back(std::move(game_list));

        fsui::MenuItemDescriptor start;
        start.id = "start-game";
        start.icon_path = fsui::GetBuiltInStartupIconPath(fsui::BuiltInStartupIcon::StartGame);
        start.title = "启动游戏";
        start.summary = "直接启动光盘镜像、PS-X EXE 或 BIOS。";
        start.on_activate = [this]() { showStartGameWindow(); };
        items.push_back(std::move(start));

        fsui::MenuItemDescriptor settings;
        settings.id = "settings";
        settings.icon_path = fsui::GetBuiltInStartupIconPath(fsui::BuiltInStartupIcon::Settings);
        settings.title = "设置";
        settings.summary = "调整 ARMSX、BIOS、画面和游戏库选项。";
        settings.on_activate = [this]() { switchToSettingsWindow(); };
        items.push_back(std::move(settings));

        fsui::MenuItemDescriptor exit;
        exit.id = "exit";
        exit.icon_path = fsui::GetBuiltInStartupIconPath(fsui::BuiltInStartupIcon::Exit);
        exit.title = "退出";
        exit.summary = "关闭 ARMSX。";
        exit.on_activate = [this]() { showExitWindow(); };
        items.push_back(std::move(exit));

        return items;
    }

    std::vector<fsui::MenuItemDescriptor> buildStartItems() {
        std::vector<fsui::MenuItemDescriptor> items;

        auto default_dir = initialBrowseDirectory();

        fsui::MenuItemDescriptor start_file;
        start_file.id = "start-file";
        start_file.icon_path = fsui::GetBuiltInStartupIconPath(fsui::BuiltInStartupIcon::StartFile);
        start_file.title = "选择文件启动";
        start_file.summary = "从文件中选择光盘镜像或 PS-X EXE。";
        start_file.on_activate = [this, default_dir]() {
#if defined(__EMSCRIPTEN__)
            requestWebFiles(false);
#else
            ImGuiFullscreen::OpenFileSelector(
                "选择启动文件",
                false,
                [this](const std::string& path) {
                    queueLaunchSelection(path);
                },
                {".cue", ".bin", ".iso", ".img",
#ifdef USE_CHD
                 ".chd",
#endif
                 ".exe", ".ps-exe", ".psexe"},
                default_dir.string()
            );
#endif
        };
        items.push_back(std::move(start_file));

        if (!settings_.default_exe_path.empty()) {
            fsui::MenuItemDescriptor start_saved_exe;
            start_saved_exe.id = "start-default-exe";
            start_saved_exe.icon_path = fsui::GetBuiltInStartupIconPath(fsui::BuiltInStartupIcon::StartFile);
            start_saved_exe.title = "启动已保存的 PS-X EXE";
            start_saved_exe.summary = settings_.default_exe_path;
            start_saved_exe.on_activate = [this]() {
                deferred_launch_ = LaunchRequest{
                    .kind = LaunchKind::Exe,
                    .path = std::filesystem::path(settings_.default_exe_path),
                    .label = StemToTitle(settings_.default_exe_path),
                };
                close_ui_after_launch_ = true;
            };
            items.push_back(std::move(start_saved_exe));
        }

        fsui::MenuItemDescriptor start_disc;
        start_disc.id = "start-disc";
        start_disc.icon_path = fsui::GetBuiltInStartupIconPath(fsui::BuiltInStartupIcon::StartDisc);
        start_disc.title = "启动光盘";
        start_disc.summary = "直接选择一个光盘镜像。";
        start_disc.on_activate = [this, default_dir]() {
#if defined(__EMSCRIPTEN__)
            requestWebFiles(false);
#else
            ImGuiFullscreen::OpenFileSelector(
                "选择光盘镜像",
                false,
                [this](const std::string& path) {
                    queueLaunchSelection(path, LaunchKind::Disc);
                },
                {".cue", ".bin", ".iso", ".img",
#ifdef USE_CHD
                 ".chd",
#endif
                },
                default_dir.string()
            );
#endif
        };
        items.push_back(std::move(start_disc));

#if defined(__EMSCRIPTEN__)
        fsui::MenuItemDescriptor start_folder;
        start_folder.id = "start-web-folder";
        start_folder.icon_path = fsui::GetBuiltInStartupIconPath(fsui::BuiltInStartupIcon::StartFile);
        start_folder.title = "打开游戏文件夹";
        start_folder.summary = "临时授权访问文件夹并启动其中发现的游戏。";
        start_folder.on_activate = [this]() { requestWebFiles(true); };
        items.push_back(std::move(start_folder));
#endif

        fsui::MenuItemDescriptor start_bios;
        start_bios.id = "start-bios";
        start_bios.icon_path = fsui::GetBuiltInStartupIconPath(fsui::BuiltInStartupIcon::StartBios);
        start_bios.title = "启动 BIOS";
        start_bios.summary = "不插入光盘，直接进入 BIOS。";
        start_bios.on_activate = [this]() {
            deferred_launch_ = LaunchRequest{.kind = LaunchKind::Bios, .path = {}, .label = "PlayStation BIOS"};
            close_ui_after_launch_ = true;
        };
        items.push_back(std::move(start_bios));

        fsui::MenuItemDescriptor back;
        back.id = "back";
        back.icon_path = fsui::GetBuiltInStartupIconPath(fsui::BuiltInStartupIcon::Back);
        back.title = "返回";
        back.summary = "返回主菜单。";
        back.on_activate = [this]() { showLandingWindow(); };
        items.push_back(std::move(back));

        return items;
    }

    std::vector<fsui::MenuItemDescriptor> buildExitItems() {
        std::vector<fsui::MenuItemDescriptor> items;

        fsui::MenuItemDescriptor quit;
        quit.id = "quit";
        quit.icon_path = fsui::GetBuiltInStartupIconPath(fsui::BuiltInStartupIcon::Exit);
        quit.title = "退出";
        quit.summary = "关闭 ARMSX。";
        quit.command = fsui::MakeRequestQuitCommand();
        items.push_back(std::move(quit));

        fsui::MenuItemDescriptor back;
        back.id = "back";
        back.icon_path = fsui::GetBuiltInStartupIconPath(fsui::BuiltInStartupIcon::Back);
        back.title = "返回";
        back.summary = "返回主菜单。";
        back.on_activate = [this]() { showLandingWindow(); };
        items.push_back(std::move(back));

        return items;
    }

    std::vector<fsui::MenuItemDescriptor> buildPauseItems() {
        std::vector<fsui::MenuItemDescriptor> items;

        fsui::MenuItemDescriptor resume;
        resume.id = "resume";
        resume.title = ICON_FA_PLAY " 继续游戏";
        resume.summary = "返回游戏。";
        resume.command = fsui::MakeResumeCommand();
        items.push_back(std::move(resume));

        fsui::MenuItemDescriptor reset;
        reset.id = "reset";
        reset.title = ICON_FA_SYNC " 重置";
        reset.summary = "软重置当前游戏。";
        reset.command = fsui::MakeResetCommand();
        items.push_back(std::move(reset));

        fsui::MenuItemDescriptor change_disc;
        change_disc.id = "change-disc";
        change_disc.title = ICON_FA_COMPACT_DISC " 更换光盘";
        change_disc.summary = "换入其他光盘镜像并继续游戏。";
        change_disc.on_activate = [this]() {
            ImGuiFullscreen::OpenFileSelector(
                "更换光盘",
                false,
                [this](const std::string& path) { queueDiscSwapSelection(path); },
                {".cue", ".bin", ".iso", ".img",
#ifdef USE_CHD
                 ".chd",
#endif
                },
                initialBrowseDirectory().string()
            );
        };
        items.push_back(std::move(change_disc));

        fsui::MenuItemDescriptor screenshot;
        screenshot.id = "screenshot";
        screenshot.title = ICON_FA_CAMERA " 保存截图";
        screenshot.summary = "将 BMP 截图保存到截图文件夹。";
        screenshot.on_activate = [this]() { deferred_screenshot_ = true; };
        items.push_back(std::move(screenshot));

        fsui::MenuItemDescriptor settings;
        settings.id = "settings";
        settings.title = ICON_FA_SLIDERS_H " 设置";
        settings.summary = "打开游戏内设置。";
        settings.on_activate = [this]() { switchToSettingsWindow(); };
        items.push_back(std::move(settings));

        fsui::MenuItemDescriptor fast_forward;
        fast_forward.id = "fast-forward";
        fast_forward.title = ICON_FA_FAST_FORWARD " 快进";
        fast_forward.summary = session_.fastForwardEnabled()
            ? "当前速度为 2 倍。点击恢复正常速度并继续游戏。"
            : "当前为正常速度。点击以 2 倍速度继续游戏。";
        fast_forward.on_activate = [this]() { deferred_fast_forward_toggle_ = true; };
        items.push_back(std::move(fast_forward));

        fsui::MenuItemDescriptor quit;
        quit.id = "quit-game";
        quit.title = ICON_FA_POWER_OFF " 退出游戏";
        quit.summary = "关闭当前游戏并返回主菜单。";
        quit.command = fsui::MakeExitToLibraryCommand();
        items.push_back(std::move(quit));

        return items;
    }

    std::vector<fsui::MenuItemDescriptor> buildGameLaunchOptions(const fsui::GameEntry&) {
        return {};
    }

    std::vector<fsui::SettingsPageDescriptor> buildSettingsPages(fsui::SettingsScope scope) {
        std::vector<fsui::SettingsPageDescriptor> pages;

        if (scope == fsui::SettingsScope::PerGame) {
            pages = buildPerGameSettingsPages();
        } else {
            pages = buildGlobalSettingsPages();
        }

        if (pending_settings_page_restore_.has_value()) {
            const std::string restore_id = *pending_settings_page_restore_;
            const auto match = std::find_if(pages.begin(), pages.end(), [&](const fsui::SettingsPageDescriptor& page) {
                return page.id == restore_id;
            });

            if (match != pages.end()) {
                std::rotate(pages.begin(), match, match + 1);
            }

            pending_settings_page_restore_.reset();
        }

        return pages;
    }

    std::vector<fsui::SettingsPageDescriptor> buildGlobalSettingsPages() {
        std::vector<fsui::SettingsPageDescriptor> pages;

        fsui::SettingsPageDescriptor bios_page;
        bios_page.id = "BIOS";
        bios_page.scope = fsui::SettingsScope::Global;
        bios_page.built_in_page = fsui::BuiltInSettingsPage::BIOS;
        bios_page.build_rows = [this]() {
            std::vector<fsui::SettingsRowDescriptor> rows;

            fsui::SettingsRowDescriptor model;
            model.kind = fsui::SettingsRowKind::Choice;
            model.id = "model";
            model.title = ICON_FA_MICROCHIP " BIOS 型号";
            model.summary = "当 BIOS 文件夹中有多个文件时，选择优先使用的型号。";
            model.value = settings_.model;
            model.dialog_title = model.title;
            model.choices = BuildModelChoices(settings_.model);
            model.on_choice = [this](int index) {
                const auto models = ModelChoices();
                if (index >= 0 && index < static_cast<int>(models.size())) {
                    settings_.model = models[static_cast<size_t>(index)];
                    SaveSettings(settings_);
                }
            };
            rows.push_back(std::move(model));

            fsui::SettingsRowDescriptor folder;
            folder.kind = fsui::SettingsRowKind::Action;
            folder.id = "bios-folder";
            folder.title = ICON_FA_FOLDER " BIOS 文件夹";
            if (settings_.bios_search.empty()) {
                folder.summary = AppendBrowseRootHint("选择包含 BIOS 文件的文件夹。");
            } else {
                folder.summary = settings_.bios_search;
            }
            folder.on_activate = [this]() {
                ImGuiFullscreen::OpenFileSelector(
                    "选择 BIOS 文件夹",
                    true,
                    [this](const std::string& path) {
                        if (const std::optional<std::string> selection = NormalizedPickerSelection(path)) {
                            settings_.bios_search = *selection;
                            SaveSettings(settings_);
                        }
                    },
                    {},
                    browseDirectoryForPath(settings_.bios_search, true).string()
                );
            };
            rows.push_back(std::move(folder));

            fsui::SettingsRowDescriptor override;
            override.kind = fsui::SettingsRowKind::Action;
            override.id = "bios-override";
            override.title = ICON_FA_FILE " 指定 BIOS 文件";
            override.summary = settings_.bios_override.empty() ? "从 BIOS 文件夹中自动选择。" : settings_.bios_override;
            override.on_activate = [this]() {
                const std::string browse_seed = settings_.bios_override.empty() ? settings_.bios_search : settings_.bios_override;
                ImGuiFullscreen::OpenFileSelector(
                    "指定 BIOS 文件",
                    false,
                    [this](const std::string& path) {
                        if (const std::optional<std::string> selection = NormalizedPickerSelection(path)) {
                            settings_.bios_override = *selection;
                            SaveSettings(settings_);
                        }
                    },
                    {".bin", ".rom"},
                    browseDirectoryForPath(browse_seed, false).string()
                );
            };
            rows.push_back(std::move(override));

            fsui::SettingsRowDescriptor clear_override;
            clear_override.kind = fsui::SettingsRowKind::Action;
            clear_override.id = "clear-bios-override";
            clear_override.title = ICON_FA_TIMES " 清除指定 BIOS";
            clear_override.summary = "恢复按 BIOS 文件夹和首选型号自动选择。";
            clear_override.enabled = !settings_.bios_override.empty();
            clear_override.on_activate = [this]() {
                settings_.bios_override.clear();
                SaveSettings(settings_);
            };
            rows.push_back(std::move(clear_override));

            return rows;
        };
        pages.push_back(std::move(bios_page));

        fsui::SettingsPageDescriptor emulation_page;
        emulation_page.id = "Emulation";
        emulation_page.scope = fsui::SettingsScope::Global;
        emulation_page.built_in_page = fsui::BuiltInSettingsPage::Emulation;
        emulation_page.build_rows = [this]() {
            std::vector<fsui::SettingsRowDescriptor> rows;

            fsui::SettingsRowDescriptor cpu_engine;
            cpu_engine.kind = fsui::SettingsRowKind::Choice;
            cpu_engine.id = "cpu-engine";
            cpu_engine.title = ICON_FA_MICROCHIP " CPU 引擎";
            cpu_engine.summary = "正常游玩建议使用缓存解释器；兼容性排查可使用参考解释器。下次启动游戏时生效。";
            cpu_engine.value = CpuEngineTitle(settings_.cpu_engine);
            cpu_engine.dialog_title = cpu_engine.title;
            cpu_engine.choices = BuildCpuEngineChoices(settings_.cpu_engine);
            cpu_engine.on_choice = [this](int index) {
                settings_.cpu_engine = index == 1 ? PSX_CPU_INTERPRETER : PSX_CPU_CACHED_INTERPRETER;
                SaveSettings(settings_);
            };
            rows.push_back(std::move(cpu_engine));

            fsui::SettingsRowDescriptor fast_boot;
            fast_boot.kind = fsui::SettingsRowKind::Toggle;
            fast_boot.id = "fast-boot";
            fast_boot.title = ICON_FA_BOLT " Fast Boot（快速启动）";
            fast_boot.summary = "BIOS 完成系统初始化后跳过 PlayStation 开机动画。如游戏必须完整运行 BIOS 启动流程，请关闭此项。";
            fast_boot.toggle_value = settings_.fast_boot;
            fast_boot.on_toggle = [this](bool value) {
                settings_.fast_boot = value;
                SaveSettings(settings_);
            };
            rows.push_back(std::move(fast_boot));

            fsui::SettingsRowDescriptor region;
            region.kind = fsui::SettingsRowKind::Choice;
            region.id = "region";
            region.title = ICON_FA_GLOBE " 主机区域";
            region.summary = "选择启动光盘时使用的主机区域。";
            region.value = ToLower(settings_.region);
            region.dialog_title = region.title;
            region.choices = BuildRegionChoices(settings_.region);
            region.on_choice = [this](int index) {
                static const std::array<const char*, 3> values = {"auto", "ntsc", "pal"};
                if (index >= 0 && index < static_cast<int>(values.size())) {
                    settings_.region = values[static_cast<size_t>(index)];
                    SaveSettings(settings_);
                }
            };
            rows.push_back(std::move(region));

            fsui::SettingsRowDescriptor expansion;
            expansion.kind = fsui::SettingsRowKind::Action;
            expansion.id = "expansion-rom";
            expansion.title = ICON_FA_MEMORY " 扩展 ROM";
            expansion.summary = settings_.exp_path.empty() ? "尚未选择扩展 ROM。" : settings_.exp_path;
            expansion.on_activate = [this]() {
                ImGuiFullscreen::OpenFileSelector(
                    "选择扩展 ROM",
                    false,
                    [this](const std::string& path) {
                        if (const std::optional<std::string> selection = NormalizedPickerSelection(path)) {
                            settings_.exp_path = *selection;
                            SaveSettings(settings_);
                        }
                    },
                    {".bin", ".rom"},
                    initialBrowseDirectory().string()
                );
            };
            rows.push_back(std::move(expansion));

            fsui::SettingsRowDescriptor default_exe;
            default_exe.kind = fsui::SettingsRowKind::Action;
            default_exe.id = "default-exe";
            default_exe.title = ICON_FA_FILE_CODE " 默认 PS-X EXE";
            default_exe.summary = settings_.default_exe_path.empty() ? "尚未选择默认可执行文件。" : settings_.default_exe_path;
            default_exe.on_activate = [this]() {
                ImGuiFullscreen::OpenFileSelector(
                    "选择默认 PS-X EXE",
                    false,
                    [this](const std::string& path) {
                        if (const std::optional<std::string> selection = NormalizedPickerSelection(path)) {
                            settings_.default_exe_path = *selection;
                            SaveSettings(settings_);
                        }
                    },
                    {".exe", ".ps-exe", ".psexe"},
                    initialBrowseDirectory().string()
                );
            };
            rows.push_back(std::move(default_exe));

            return rows;
        };
        pages.push_back(std::move(emulation_page));

        fsui::SettingsPageDescriptor graphics_page;
        graphics_page.id = "Graphics";
        graphics_page.scope = fsui::SettingsScope::Global;
        graphics_page.built_in_page = fsui::BuiltInSettingsPage::Graphics;
        graphics_page.build_rows = [this]() { return buildGraphicsRows(SupportsManagedWindowSizing()); };
        pages.push_back(std::move(graphics_page));

        fsui::SettingsPageDescriptor folders_page;
        folders_page.id = "Folders";
        folders_page.scope = fsui::SettingsScope::Global;
        folders_page.built_in_page = fsui::BuiltInSettingsPage::Folders;
        folders_page.build_rows = [this]() {
            std::vector<fsui::SettingsRowDescriptor> rows;

#if defined(UWP_TARGET)
            fsui::SettingsRowDescriptor browse_notice;
            browse_notice.kind = fsui::SettingsRowKind::Notice;
            browse_notice.id = "uwp-filesystem-roots";
            browse_notice.title = ICON_FA_FOLDER_OPEN " 已挂载位置";
            browse_notice.summary = "文件选择器中可访问各存储根目录；使用“文件系统根目录”或“上级目录”切换位置。";
            rows.push_back(std::move(browse_notice));
#endif

            fsui::SettingsRowDescriptor add_folder;
            add_folder.kind = fsui::SettingsRowKind::Action;
            add_folder.id = "add-folder";
            add_folder.title = ICON_FA_FOLDER_PLUS " 添加游戏文件夹";
#if defined(__ANDROID__) || defined(IOS_TARGET)
            add_folder.summary = "通过系统文件夹选择器永久授权访问一个游戏目录。";
#else
            add_folder.summary = AppendBrowseRootHint("只扫描所选目录。");
#endif
            add_folder.on_activate = [this]() {
                if (RequestPlatformLibraryDirectory(false)) {
                    return;
                }
                ImGuiFullscreen::OpenFileSelector(
                    "添加游戏文件夹",
                    true,
                    [this](const std::string& path) {
                        if (const std::optional<std::string> selection = NormalizedPickerSelection(path)) {
                            settings_.ui_state.game_list_paths.emplace_back(*selection);
                            refreshGameList(true);
                            SaveSettings(settings_);
                        }
                    },
                    {},
                    initialBrowseDirectory().string()
                );
            };
            rows.push_back(std::move(add_folder));

            fsui::SettingsRowDescriptor add_recursive;
            add_recursive.kind = fsui::SettingsRowKind::Action;
            add_recursive.id = "add-recursive-folder";
            add_recursive.title = ICON_FA_FOLDER_PLUS " 添加递归扫描文件夹";
#if defined(__ANDROID__) || defined(IOS_TARGET)
            add_recursive.summary = "永久授权访问游戏目录，并扫描其中所有子文件夹。";
#else
            add_recursive.summary = AppendBrowseRootHint("扫描所选目录及其所有子目录。");
#endif
            add_recursive.on_activate = [this]() {
                if (RequestPlatformLibraryDirectory(true)) {
                    return;
                }
                ImGuiFullscreen::OpenFileSelector(
                    "添加递归扫描文件夹",
                    true,
                    [this](const std::string& path) {
                        if (const std::optional<std::string> selection = NormalizedPickerSelection(path)) {
                            settings_.ui_state.game_list_recursive_paths.emplace_back(*selection);
                            refreshGameList(true);
                            SaveSettings(settings_);
                        }
                    },
                    {},
                    initialBrowseDirectory().string()
                );
            };
            rows.push_back(std::move(add_recursive));

            fsui::SettingsRowDescriptor refresh;
            refresh.kind = fsui::SettingsRowKind::Action;
            refresh.id = "refresh-library";
            refresh.title = ICON_FA_SYNC " 刷新游戏库";
            refresh.summary = "重新扫描所有已配置的游戏文件夹。";
            refresh.on_activate = [this]() { refreshGameList(true); };
            rows.push_back(std::move(refresh));

            auto append_folder_rows = [&](const std::vector<std::filesystem::path>& folders, bool recursive) {
                for (size_t index = 0; index < folders.size(); index++) {
                    fsui::SettingsRowDescriptor row;
                    row.kind = fsui::SettingsRowKind::Action;
                    row.id = std::string(recursive ? "recursive-" : "folder-") + std::to_string(index);
                    row.title = WithHiddenId(
                        recursive ? ICON_FA_FOLDER_OPEN " 递归扫描文件夹" : ICON_FA_FOLDER " 游戏文件夹",
                        row.id
                    );
                    const std::string configured_path = folders[index].string();
                    const auto platform_root = std::find_if(platform_roots_.begin(), platform_roots_.end(), [&](const PlatformLibraryRoot& root) {
                        return root.path == configured_path;
                    });
                    row.summary = platform_root != platform_roots_.end() && !platform_root->label.empty()
                        ? platform_root->label + "\n" + configured_path
                        : configured_path;
                    row.on_activate = [this, index, recursive]() {
                        auto& list = recursive ? settings_.ui_state.game_list_recursive_paths : settings_.ui_state.game_list_paths;
                        if (index < list.size()) {
                            const std::string removed_path = list[index].string();
                            list.erase(list.begin() + static_cast<std::ptrdiff_t>(index));
                            NotifyPlatformLibraryDirectoryRemoved(removed_path);
                            refreshGameList(true);
                            SaveSettings(settings_);
                        }
                    };
                    rows.push_back(std::move(row));
                }
            };

            append_folder_rows(settings_.ui_state.game_list_paths, false);
            append_folder_rows(settings_.ui_state.game_list_recursive_paths, true);

            return rows;
        };
        pages.push_back(std::move(folders_page));

        fsui::SettingsPageDescriptor advanced_page;
        advanced_page.id = "Advanced";
        advanced_page.scope = fsui::SettingsScope::Global;
        advanced_page.built_in_page = fsui::BuiltInSettingsPage::Advanced;
        advanced_page.build_rows = [this]() {
            std::vector<fsui::SettingsRowDescriptor> rows;

#if defined(IOS_TARGET)
            fsui::SettingsRowDescriptor logging_status;
            logging_status.kind = fsui::SettingsRowKind::Notice;
            logging_status.id = "logging-status";
            logging_status.title = ICON_FA_BUG " 模拟器日志";
            logging_status.summary = "启动、PSX 核心、SDL 和游戏控制台输出始终写入日志文件。";
            logging_status.value = "已开启";
            rows.push_back(std::move(logging_status));
#else
            fsui::SettingsRowDescriptor logging_enabled;
            logging_enabled.kind = fsui::SettingsRowKind::Toggle;
            logging_enabled.id = "logging-enabled";
            logging_enabled.title = ICON_FA_BUG " 调试日志";
            logging_enabled.summary =
                "写入用于故障排查的诊断日志。正常游玩时建议关闭，以免影响性能。";
            logging_enabled.toggle_value = settings_.logging_enabled;
            logging_enabled.on_toggle = [this](bool value) {
                settings_.logging_enabled = value;
                applyLoggingSettings("settings-toggle");
                SaveSettings(settings_);
            };
            rows.push_back(std::move(logging_enabled));
#endif

            fsui::SettingsRowDescriptor mobile_controls;
            mobile_controls.kind = fsui::SettingsRowKind::Toggle;
            mobile_controls.id = "mobile-controls";
            mobile_controls.title = ICON_FA_GAMEPAD " 屏幕触控按键";
            mobile_controls.summary =
                "游戏中显示支持多点触控的 PlayStation 按键。闲置三秒或使用实体手柄后自动隐藏，触摸屏幕时重新显示。";
            mobile_controls.toggle_value = settings_.mobile_controls;
            mobile_controls.on_toggle = [this](bool value) {
                settings_.mobile_controls = value;
                if (!value) {
                    mobile_controls_.reset(input_router_);
                } else {
                    mobile_controls_.showNow();
                }
                SaveSettings(settings_);
            };
            rows.push_back(std::move(mobile_controls));

            if (settings_.logging_enabled) {
                fsui::SettingsRowDescriptor log_level;
                log_level.kind = fsui::SettingsRowKind::Choice;
                log_level.id = "log-level";
                log_level.title = ICON_FA_TERMINAL " 日志级别";
                log_level.summary = "选择 ARMSX 写入调试日志的详细程度。";
                log_level.value = log_level_string(settings_.log_level);
                log_level.dialog_title = log_level.title;
                log_level.choices = BuildLogLevelChoices(settings_.log_level);
                log_level.on_choice = [this](int index) {
                    settings_.log_level = std::clamp(index, static_cast<int>(LOG_TRACE), static_cast<int>(LOG_FATAL));
                    applyLoggingSettings("settings-log-level");
                    SaveSettings(settings_);
                };
                rows.push_back(std::move(log_level));

                fsui::SettingsRowDescriptor log_path;
                log_path.kind = fsui::SettingsRowKind::Notice;
                log_path.id = "log-path";
                log_path.title = ICON_FA_FILE_ALT " 日志文件";
                log_path.summary = diagnosticsLogPath().string();
                rows.push_back(std::move(log_path));
            }

            fsui::SettingsRowDescriptor settings_path;
            settings_path.kind = fsui::SettingsRowKind::Notice;
            settings_path.id = "settings-path";
            settings_path.title = ICON_FA_FILE_ALT " 设置文件";
            settings_path.summary = settings_.settings_path;
            rows.push_back(std::move(settings_path));

            return rows;
        };
        pages.push_back(std::move(advanced_page));

        return pages;
    }

    std::vector<fsui::SettingsPageDescriptor> buildPerGameSettingsPages() {
        std::vector<fsui::SettingsPageDescriptor> pages;

        fsui::SettingsPageDescriptor summary;
        summary.id = "Summary";
        summary.scope = fsui::SettingsScope::PerGame;
        summary.built_in_page = fsui::BuiltInSettingsPage::Summary;
        summary.build_rows = [this]() {
            std::vector<fsui::SettingsRowDescriptor> rows;
            const fsui::CurrentGameInfo info = session_.currentGameInfo();

            fsui::SettingsRowDescriptor title;
            title.kind = fsui::SettingsRowKind::Notice;
            title.id = "current-game";
            title.title = info.title;
            title.summary = info.path.empty() ? info.subtitle : info.path.string();
            rows.push_back(std::move(title));

            return rows;
        };
        pages.push_back(std::move(summary));

        fsui::SettingsPageDescriptor graphics;
        graphics.id = "Graphics";
        graphics.scope = fsui::SettingsScope::PerGame;
        graphics.built_in_page = fsui::BuiltInSettingsPage::Graphics;
        graphics.build_rows = [this]() { return buildGraphicsRows(false); };
        pages.push_back(std::move(graphics));

        fsui::SettingsPageDescriptor emulation;
        emulation.id = "Emulation";
        emulation.scope = fsui::SettingsScope::PerGame;
        emulation.built_in_page = fsui::BuiltInSettingsPage::Emulation;
        emulation.build_rows = [this]() {
            std::vector<fsui::SettingsRowDescriptor> rows;

            fsui::SettingsRowDescriptor reset;
            reset.kind = fsui::SettingsRowKind::Action;
            reset.id = "reset-now";
            reset.title = ICON_FA_SYNC " 重置";
            reset.summary = "软重置当前游戏。";
            reset.on_activate = [this]() { deferred_reset_ = true; };
            rows.push_back(std::move(reset));

            fsui::SettingsRowDescriptor disc;
            disc.kind = fsui::SettingsRowKind::Action;
            disc.id = "change-disc-now";
            disc.title = ICON_FA_COMPACT_DISC " 更换光盘";
            disc.summary = "换入其他光盘镜像。";
            disc.on_activate = [this]() {
                ImGuiFullscreen::OpenFileSelector(
                    "更换光盘",
                    false,
                [this](const std::string& path) { queueDiscSwapSelection(path); },
                {".cue", ".bin", ".iso", ".img",
#ifdef USE_CHD
                 ".chd",
#endif
                },
                initialBrowseDirectory().string()
            );
        };
            rows.push_back(std::move(disc));

            fsui::SettingsRowDescriptor screenshot;
            screenshot.kind = fsui::SettingsRowKind::Action;
            screenshot.id = "save-screenshot-now";
            screenshot.title = ICON_FA_CAMERA " 保存截图";
            screenshot.summary = "将 BMP 截图写入截图文件夹。";
            screenshot.on_activate = [this]() { deferred_screenshot_ = true; };
            rows.push_back(std::move(screenshot));

            fsui::SettingsRowDescriptor exit_library;
            exit_library.kind = fsui::SettingsRowKind::Action;
            exit_library.id = "exit-library-now";
            exit_library.title = ICON_FA_FOLDER_OPEN " 返回游戏库";
            exit_library.summary = "关闭当前游戏并返回主菜单。";
            exit_library.on_activate = [this]() { deferred_exit_to_library_ = true; };
            rows.push_back(std::move(exit_library));

            return rows;
        };
        pages.push_back(std::move(emulation));

        return pages;
    }

    std::vector<fsui::SettingsRowDescriptor> buildGraphicsRows(bool include_scale) {
        std::vector<fsui::SettingsRowDescriptor> rows;
#if defined(__EMSCRIPTEN__)
        const bool can_control_vsync = false;
#else
        const bool can_control_vsync = owns_renderer_ && !external_renderer_;
#endif

#ifdef USE_HARDWARE
        {
            fsui::SettingsRowDescriptor gpu_backend;
            gpu_backend.kind = fsui::SettingsRowKind::Choice;
            gpu_backend.id = "gpu-backend";
            gpu_backend.title = ICON_FA_TACHOMETER_ALT " GPU 后端";
            gpu_backend.summary = hardwareRendererAvailable()
                ? "选择 SDL 软件或加速显示。PlayStation GPU 光栅化仍采用精度优先的软件实现。下次启动游戏时生效。"
                : "当前显示模式下，SDL 渲染器由宿主程序控制。";
            gpu_backend.value = GpuBackendTitle(settings_.gpu_backend);
            gpu_backend.dialog_title = gpu_backend.title;
            gpu_backend.choices = BuildGpuBackendChoices(settings_.gpu_backend);
            gpu_backend.enabled = hardwareRendererAvailable();
            if (!gpu_backend.enabled) {
                gpu_backend.availability = fsui::AvailabilityMode::Disabled;
                gpu_backend.unavailable_title = gpu_backend.title;
                gpu_backend.unavailable_message = "SDL 渲染器由当前宿主程序管理。";
            }
            gpu_backend.on_choice = [this](int index) {
                settings_.gpu_backend = (index == 1) ? GpuBackend::SDLAccelerated : GpuBackend::Software;
                SaveSettings(settings_);
            };
            rows.push_back(std::move(gpu_backend));
        }
#endif

        if (include_scale) {
            fsui::SettingsRowDescriptor scale;
            scale.kind = fsui::SettingsRowKind::Choice;
            scale.id = "display-scale";
            scale.title = ICON_FA_EXPAND " 显示缩放";
            scale.summary = "调整应用窗口大小。";
            scale.value = std::to_string(settings_.scale) + "x";
            scale.dialog_title = scale.title;
            scale.choices = BuildScaleChoices(settings_.scale);
            scale.on_choice = [this](int index) {
                static const std::array<int, 6> values = {1, 2, 3, 4, 5, 6};
                if (index >= 0 && index < static_cast<int>(values.size())) {
                    settings_.scale = values[static_cast<size_t>(index)];
                    applyWindowMetrics();
                    SaveSettings(settings_);
                }
            };
            rows.push_back(std::move(scale));
        }

        fsui::SettingsRowDescriptor vsync;
        vsync.kind = fsui::SettingsRowKind::Toggle;
        vsync.id = "vsync";
        vsync.title = ICON_FA_SYNC " VSync";
#if defined(__EMSCRIPTEN__)
        vsync.summary = "将画面显示与屏幕刷新率同步；网页版由浏览器控制。";
#else
        vsync.summary = can_control_vsync
            ? "将画面显示与屏幕刷新率同步，以减少撕裂。立即生效。"
            : "将画面显示与屏幕刷新率同步；当前渲染器由宿主程序控制。";
#endif
        vsync.toggle_value = settings_.vsync_enabled;
        vsync.enabled = can_control_vsync;
        vsync.on_toggle = [this](bool value) {
            settings_.vsync_enabled = value;
            pending_settings_page_restore_ = "Graphics";
            SaveSettings(settings_);
            deferred_vsync_ = value;
        };
        rows.push_back(std::move(vsync));

        fsui::SettingsRowDescriptor filter;
        filter.kind = fsui::SettingsRowKind::Toggle;
        filter.id = "texture-filter";
        filter.title = ICON_FA_ADJUST " 画面缩放 / 双线性过滤";
        filter.summary = "对最终显示画面使用更平滑的缩放过滤。";
        filter.toggle_value = settings_.texture_scale_mode;
        filter.on_toggle = [this](bool value) {
            settings_.texture_scale_mode = value;
            SaveSettings(settings_);
        };
        rows.push_back(std::move(filter));

        fsui::SettingsRowDescriptor presentation_upscale;
        presentation_upscale.kind = fsui::SettingsRowKind::Choice;
        presentation_upscale.id = "presentation-upscale";
        presentation_upscale.title = ICON_FA_MAGIC " SDL 高质量放大";
        presentation_upscale.summary =
            "在不改变 PlayStation 光栅化精度的情况下，以更高分辨率生成 SDL 显示纹理。";
        presentation_upscale.value = std::to_string(settings_.presentation_upscale) + "x";
        presentation_upscale.dialog_title = presentation_upscale.title;
        presentation_upscale.choices = BuildPresentationUpscaleChoices(settings_.presentation_upscale);
        presentation_upscale.on_choice = [this](int index) {
            if (index >= 0 && index < 8) {
                settings_.presentation_upscale = index + 1;
                SaveSettings(settings_);
            }
        };
        rows.push_back(std::move(presentation_upscale));

        fsui::SettingsRowDescriptor stretch;
        stretch.kind = fsui::SettingsRowKind::Toggle;
        stretch.id = "stretch";
        stretch.title = ICON_FA_EXPAND_ARROWS_ALT " 拉伸填满屏幕";
        stretch.summary = "填满整个屏幕，不保留原始宽高比。";
        stretch.toggle_value = settings_.stretch_mode;
        stretch.on_toggle = [this](bool value) {
            settings_.stretch_mode = value;
            SaveSettings(settings_);
        };
        rows.push_back(std::move(stretch));

        fsui::SettingsRowDescriptor aspect;
        aspect.kind = fsui::SettingsRowKind::Choice;
        aspect.id = "aspect";
        aspect.title = ICON_FA_TV " 显示宽高比";
        aspect.summary = "选择画面的显示宽高比。";
        aspect.value = AspectTitle(settings_.display_aspect);
        aspect.dialog_title = aspect.title;
        aspect.choices = BuildAspectChoices(settings_.display_aspect);
        aspect.on_choice = [this](int index) {
            settings_.display_aspect = std::clamp(index, 0, 2);
            applyWindowMetrics();
            SaveSettings(settings_);
        };
        rows.push_back(std::move(aspect));

        fsui::SettingsRowDescriptor upscale;
        upscale.kind = fsui::SettingsRowKind::Choice;
        upscale.id = "wide-upscale";
        upscale.title = ICON_FA_EXPAND " 宽屏输出分辨率";
        upscale.summary = "选择宽屏模式的输出高度。";
        upscale.value = UpscaleToString(settings_.upscale_height);
        upscale.dialog_title = upscale.title;
        upscale.choices = BuildUpscaleChoices(settings_.upscale_height);
        upscale.on_choice = [this](int index) {
            static const std::array<int, 5> heights = {480, 720, 1080, 1440, 2160};
            if (index >= 0 && index < static_cast<int>(heights.size())) {
                settings_.upscale_height = heights[static_cast<size_t>(index)];
                applyWindowMetrics();
                SaveSettings(settings_);
            }
        };
        rows.push_back(std::move(upscale));

        fsui::SettingsRowDescriptor show_fps;
        show_fps.kind = fsui::SettingsRowKind::Toggle;
        show_fps.id = "show-fps";
        show_fps.title = ICON_FA_TACHOMETER_ALT " 显示 FPS";
        show_fps.summary = "游戏中在右上角显示当前画面帧率。";
        show_fps.toggle_value = settings_.show_fps;
        show_fps.on_toggle = [this](bool value) {
            settings_.show_fps = value;
            settings_.ui_state.show_performance_overlay = value;
            SaveSettings(settings_);
        };
        rows.push_back(std::move(show_fps));

        fsui::SettingsRowDescriptor debug_panel;
        debug_panel.kind = fsui::SettingsRowKind::Toggle;
        debug_panel.id = "debug-panel";
        debug_panel.title = ICON_FA_TACHOMETER_ALT " 调试面板";
        debug_panel.summary = "显示当前模拟器设置，供故障排查使用。";
        debug_panel.toggle_value = settings_.debug_panel;
        debug_panel.on_toggle = [this](bool value) {
            settings_.debug_panel = value;
            settings_.ui_state.show_settings_overlay = value;
            SaveSettings(settings_);
        };
        rows.push_back(std::move(debug_panel));

        if (session_.valid()) {
            fsui::SettingsRowDescriptor debug_view;
            debug_view.kind = fsui::SettingsRowKind::Toggle;
            debug_view.id = "debug-view";
            debug_view.title = ICON_FA_BUG " VRAM 调试视图";
            debug_view.summary = "显示原始 VRAM，而不是游戏画面。";
            debug_view.toggle_value = session_.debugView();
            debug_view.on_toggle = [this](bool value) {
                session_.setDebugView(value);
                applyWindowMetrics();
            };
            rows.push_back(std::move(debug_view));
        }

        return rows;
    }

    void applyWindowMetrics() {
        if (!SupportsManagedWindowSizing() || !owns_window_ || !window_) {
            return;
        }

        int width = 1280;
        int height = 720;

        if (session_.valid()) {
            if (session_.debugView()) {
                width = PSX_GPU_FB_WIDTH;
                height = PSX_GPU_FB_HEIGHT;
            } else if (settings_.display_aspect == 2) {
                height = std::max(240, settings_.upscale_height);
                width = static_cast<int>((16.0f / 9.0f) * static_cast<float>(height));
            } else if (settings_.display_aspect == 1) {
                width = 320 * std::max(1, settings_.scale);
                height = width;
            } else {
                int base_width = 320;
                if (session_.psx()) {
                    const int display_width = static_cast<int>(psx_get_dmode_width(session_.psx()));
                    if (display_width == 256 || display_width == 320) {
                        base_width = display_width;
                    } else if (display_width == 368) {
                        base_width = 384;
                    }
                }

                width = base_width * std::max(1, settings_.scale);
                height = 240 * std::max(1, settings_.scale);
            }
        }
        SDL_SetWindowSize(window_, width, height);
    }

    std::filesystem::path initialBrowseDirectory() const {
        if (session_.valid()) {
            const fsui::CurrentGameInfo info = session_.currentGameInfo();
            if (!info.path.empty() && info.path.has_parent_path()) {
                return info.path.parent_path();
            }
        }

        if (!settings_.ui_state.game_list_paths.empty()) {
            return settings_.ui_state.game_list_paths.front();
        }

        if (!settings_.ui_state.game_list_recursive_paths.empty()) {
            return settings_.ui_state.game_list_recursive_paths.front();
        }

        return DefaultBrowseDirectory();
    }

    std::filesystem::path browseDirectoryForPath(const std::string& configured_path, bool expect_directory) const {
        std::error_code ec;

        if (!configured_path.empty()) {
            const std::filesystem::path candidate(configured_path);

            if (std::filesystem::exists(candidate, ec)) {
                if (expect_directory && std::filesystem::is_directory(candidate, ec)) {
                    return candidate;
                }

                if (!expect_directory) {
                    if (std::filesystem::is_directory(candidate, ec)) {
                        return candidate;
                    }

                    if (candidate.has_parent_path() && std::filesystem::exists(candidate.parent_path(), ec)) {
                        return candidate.parent_path();
                    }
                }
            }

            if (candidate.has_parent_path() && std::filesystem::exists(candidate.parent_path(), ec)) {
                return candidate.parent_path();
            }
        }

        return initialBrowseDirectory();
    }

    int argc_ = 0;
    const char* const* argv_ = nullptr;
    SDL_Window* external_window_ = nullptr;
    SDL_Renderer* external_renderer_ = nullptr;
    CliFlags cli_{};
    FrontendSettings settings_{};
    bool running_ = true;
    bool owns_sdl_ = false;
    bool owns_window_ = false;
    bool owns_renderer_ = false;
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    fsui::SdlImGuiBackend imgui_backend_{};
    ArmsxSession session_{};
    GameplayInputRouter input_router_{};
    ImGuiMobileControls mobile_controls_{};
    std::vector<fsui::GameEntry> game_list_{};
    std::vector<PlatformLibraryRoot> platform_roots_{};
    std::vector<PlatformLibraryGame> platform_games_{};
    std::uint64_t platform_library_version_ = 0;
    std::optional<std::string> pending_error_dialog_{};
    std::optional<LaunchRequest> deferred_launch_{};
    std::optional<std::filesystem::path> deferred_change_disc_{};
    std::optional<bool> deferred_vsync_{};
    std::optional<std::string> pending_settings_page_restore_{};
    std::optional<LaunchRequest> pending_cli_launch_{};
    std::string pending_cli_argument_;
    bool close_ui_after_launch_ = false;
    bool deferred_exit_to_library_ = false;
    bool deferred_reset_ = false;
    bool deferred_screenshot_ = false;
    bool deferred_fast_forward_toggle_ = false;
    bool fsui_initialized_ = false;
    FsuiWindowState ui_window_state_ = FsuiWindowState::None;
    bool managed_renderer_vsync_ = DefaultVsyncEnabled();
    uint64_t next_frame_deadline_ = 0;
    uint64_t frame_period_ticks_ = 0;
};

std::string ModuleNameFromPath(const char* path) {
    if (!path || !path[0]) {
        return "(unknown)";
    }

    return std::filesystem::path(path).filename().string();
}

void WriteNativeStackTraceImpl() {
#if defined(_WIN32)
    void* frames[64] = {};
    const USHORT frame_count = CaptureStackBackTrace(0, static_cast<DWORD>(std::size(frames)), frames, nullptr);
    psxe_diag_logf("crash", "Native stack trace (%u frames):", static_cast<unsigned int>(frame_count));

#if !defined(UWP_TARGET)
    HANDLE process = GetCurrentProcess();
    SymInitialize(process, nullptr, TRUE);

    char symbol_buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME] = {};
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(symbol_buffer);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = MAX_SYM_NAME;
#endif

    for (USHORT index = 0; index < frame_count; index++) {
        const DWORD64 address = static_cast<DWORD64>(reinterpret_cast<uintptr_t>(frames[index]));
        HMODULE module = nullptr;
        char module_path[MAX_PATH] = {};
        DWORD64 module_offset = 0;

        if (GetModuleHandleExA(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCSTR>(frames[index]),
                &module) != 0) {
            GetModuleFileNameA(module, module_path, static_cast<DWORD>(std::size(module_path)));
            module_offset = address - static_cast<DWORD64>(reinterpret_cast<uintptr_t>(module));
        }

#if !defined(UWP_TARGET)
        DWORD64 displacement = 0;
        if (SymFromAddr(process, address, &displacement, symbol) != 0) {
            psxe_diag_logf(
                "crash",
                "  #%u %p %s!%s+0x%llx module+0x%llx",
                static_cast<unsigned int>(index),
                frames[index],
                module_path[0] ? ModuleNameFromPath(module_path).c_str() : "(unknown)",
                symbol->Name,
                static_cast<unsigned long long>(displacement),
                static_cast<unsigned long long>(module_offset)
            );
            continue;
        }
#endif

        psxe_diag_logf(
            "crash",
            "  #%u %p %s+0x%llx",
            static_cast<unsigned int>(index),
            frames[index],
            module_path[0] ? ModuleNameFromPath(module_path).c_str() : "(unknown)",
            static_cast<unsigned long long>(module_offset)
        );
    }

#if !defined(UWP_TARGET)
    SymCleanup(process);
#endif
#elif defined(PSXE_HAS_EXECINFO)
    void* frames[64] = {};
    const int frame_count = backtrace(frames, static_cast<int>(std::size(frames)));
    psxe_diag_logf("crash", "Native stack trace (%d frames):", frame_count);

    for (int index = 0; index < frame_count; index++) {
        Dl_info info{};
        if (dladdr(frames[index], &info) != 0 && info.dli_fname) {
            const uintptr_t symbol_offset =
                info.dli_saddr
                    ? (reinterpret_cast<uintptr_t>(frames[index]) - reinterpret_cast<uintptr_t>(info.dli_saddr))
                    : 0u;
            psxe_diag_logf(
                "crash",
                "  #%d %p %s %s+0x%zx",
                index,
                frames[index],
                ModuleNameFromPath(info.dli_fname).c_str(),
                info.dli_sname ? info.dli_sname : "(unknown)",
                symbol_offset
            );
        } else {
            psxe_diag_logf("crash", "  #%d %p", index, frames[index]);
        }
    }
#else
    psxe_diag_logf("crash", "Native stack trace unavailable on this platform build.");
#endif
}

[[noreturn]] void ReportNativeCrash(const char* reason) {
    if (g_crash_reporting.exchange(true)) {
        std::_Exit(1);
    }

    psxe_diag_logf("crash", "Native crash captured: %s", reason ? reason : "(unknown)");

    if (g_active_app) {
        g_active_app->writeCrashContext(reason);
    } else {
        psxe_diag_logf("crash", "No active app context available.");
        psxe_diag_dump_breadcrumbs();
    }

    WriteNativeStackTraceImpl();
    psxe_diag_shutdown();
    std::_Exit(1);
}

} // namespace

extern "C" void psxe_diag_write_native_stacktrace(void) {
    WriteNativeStackTraceImpl();
}

extern "C" int psxe_run(int argc, const char* argv[], void* external_window, void* external_renderer) {
    ArmsxApp app(argc, argv, external_window, external_renderer);
    return app.run();
}

extern "C" PSXE_API void psxe_enqueue_launch_argument(const char* argument) {
    if (!argument || !*argument) {
        return;
    }

    EnqueuePendingLaunchArgument(argument);
}

extern "C" PSXE_API void psxe_wasm_on_file(const char* path) {
    if (!path || !*path) {
        return;
    }

    psxe_enqueue_launch_argument(path);
}

extern "C" PSXE_API void psxe_wasm_on_error(const char* message) {
    const char* text = message && *message ? message : "Unknown browser file error.";
    psxe_diag_logf("web", "Browser file access failed: %s", text);
    EnqueueWebError(text);
}

extern "C" PSXE_API void psxe_set_library_directory_picker_callback(
    PlatformDirectoryPickerCallback callback,
    void* userdata
) {
    g_platform_directory_picker_callback = callback;
    g_platform_directory_picker_userdata = userdata;
}

extern "C" PSXE_API void psxe_set_library_directory_removed_callback(
    PlatformDirectoryRemovedCallback callback,
    void* userdata
) {
    g_platform_directory_removed_callback = callback;
    g_platform_directory_removed_userdata = userdata;
}

extern "C" PSXE_API void psxe_register_platform_library_directory(
    const char* path,
    const char* label,
    int recursive
) {
    RegisterPlatformLibraryRoot(
        path ? path : "",
        label ? label : "",
        recursive != 0
    );
}

#if defined(__ANDROID__)
extern "C" JNIEXPORT void JNICALL Java_com_nanodata_armsx_EmulatorActivity_nativeEnqueueLaunchArgument(
    JNIEnv* env,
    jclass,
    jstring argument
) {
    if (!env || !argument) {
        return;
    }

    const char* utf = env->GetStringUTFChars(argument, nullptr);
    if (!utf) {
        return;
    }

    psxe_enqueue_launch_argument(utf);
    env->ReleaseStringUTFChars(argument, utf);
}

extern "C" JNIEXPORT void JNICALL Java_com_nanodata_armsx_EmulatorActivity_nativeSetPlatformLibrarySnapshot(
    JNIEnv* env,
    jclass,
    jobjectArray root_paths,
    jobjectArray root_labels,
    jbooleanArray root_recursive,
    jobjectArray game_paths,
    jobjectArray game_labels,
    jlongArray game_sizes,
    jlongArray game_modified
) {
    if (!env) {
        return;
    }

    auto array_string = [&](jobjectArray values, jsize index) -> std::string {
        if (!values || index < 0 || index >= env->GetArrayLength(values)) {
            return {};
        }
        auto value = static_cast<jstring>(env->GetObjectArrayElement(values, index));
        if (!value) {
            return {};
        }
        const char* utf = env->GetStringUTFChars(value, nullptr);
        std::string result = utf ? utf : "";
        if (utf) {
            env->ReleaseStringUTFChars(value, utf);
        }
        env->DeleteLocalRef(value);
        return result;
    };

    std::vector<PlatformLibraryRoot> roots;
    const jsize root_count = root_paths ? env->GetArrayLength(root_paths) : 0;
    std::vector<jboolean> recursive(static_cast<size_t>(root_count), JNI_TRUE);
    if (root_recursive && root_count > 0) {
        const jsize count = std::min(root_count, env->GetArrayLength(root_recursive));
        env->GetBooleanArrayRegion(root_recursive, 0, count, recursive.data());
    }
    roots.reserve(static_cast<size_t>(root_count));
    for (jsize index = 0; index < root_count; index++) {
        std::string path = array_string(root_paths, index);
        if (!path.empty()) {
            roots.push_back(PlatformLibraryRoot{
                .path = std::move(path),
                .label = array_string(root_labels, index),
                .recursive = recursive[static_cast<size_t>(index)] == JNI_TRUE,
            });
        }
    }

    std::vector<PlatformLibraryGame> games;
    const jsize game_count = game_paths ? env->GetArrayLength(game_paths) : 0;
    std::vector<jlong> sizes(static_cast<size_t>(game_count), 0);
    std::vector<jlong> modified(static_cast<size_t>(game_count), 0);
    if (game_sizes && game_count > 0) {
        env->GetLongArrayRegion(game_sizes, 0, std::min(game_count, env->GetArrayLength(game_sizes)), sizes.data());
    }
    if (game_modified && game_count > 0) {
        env->GetLongArrayRegion(game_modified, 0, std::min(game_count, env->GetArrayLength(game_modified)), modified.data());
    }
    games.reserve(static_cast<size_t>(game_count));
    for (jsize index = 0; index < game_count; index++) {
        std::string path = array_string(game_paths, index);
        if (!path.empty()) {
            games.push_back(PlatformLibraryGame{
                .path = std::move(path),
                .label = array_string(game_labels, index),
                .size = sizes[static_cast<size_t>(index)] > 0
                    ? static_cast<std::uint64_t>(sizes[static_cast<size_t>(index)])
                    : 0,
                .modified = modified[static_cast<size_t>(index)] > 0
                    ? static_cast<std::time_t>(modified[static_cast<size_t>(index)] / 1000)
                    : 0,
            });
        }
    }
    SetPlatformLibrarySnapshot(std::move(roots), std::move(games));
}
#endif

extern "C" PSXE_API int external_main(int argc, const char* argv[], void* external_window, void* external_renderer) {
    return psxe_run(argc, argv, external_window, external_renderer);
}

#if defined(__ANDROID__)
extern "C" PSXE_API int armsx_android_main(int argc, char* argv[]) {
    psxe_platform_set_fopen_callback(AndroidPlatformFopen, nullptr);
    return psxe_run(argc, const_cast<const char**>(argv), nullptr, nullptr);
}
#endif

#ifndef __DLL_BUILD
int main(int argc, const char* argv[]) {
    return external_main(argc, argv, nullptr, nullptr);
}
#endif
