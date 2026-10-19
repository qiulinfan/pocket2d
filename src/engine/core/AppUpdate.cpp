#include "core/AppUpdate.h"

#include "core/Log.h"
#include "shared/persistence/Sha256.h"
#include "rapidjson/document.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace AppUpdate {
namespace {

namespace fs = std::filesystem;

// ---- state ----

struct State {
    std::mutex mutex;
    Status status;
    std::string repo, url, digest; // the release API URL; the asset's URL and sha256
    fs::path download;             // the downloaded zip
    bool busy = false;
};

// Never destroyed: a worker may still run while the process exits.
State &S() {
    static State *state = new State();
    return *state;
}

#if defined(__APPLE__)
const char *const kPlatform[] = {"macos", "mac", "osx"};
#elif defined(_WIN32)
const char *const kPlatform[] = {"windows", "win64", "win"};
#else
const char *const kPlatform[] = {"linux"};
#endif

// Paths as UTF-8 (SDL's process arguments; path::string() is the ANSI code page on Windows).
std::string U8(const fs::path &path) {
    const auto text = path.u8string();
    return std::string(text.begin(), text.end());
}

std::string Lower(std::string s) {
    for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Runs a program to its end; stdout is returned when captured.
bool Run(const std::vector<std::string> &args, std::string *output, std::string &error) {
    std::vector<const char *> argv;
    for (const auto &a : args) argv.push_back(a.c_str());
    argv.push_back(nullptr);
    SDL_Process *process = SDL_CreateProcess(argv.data(), output != nullptr);
    if (!process) {
        error = args[0] + ": " + SDL_GetError();
        return false;
    }
    int code = -1;
    if (output) {
        std::size_t size = 0;
        void *data = SDL_ReadProcess(process, &size, &code);
        if (data) output->assign(static_cast<const char *>(data), size), SDL_free(data);
    } else {
        SDL_WaitProcess(process, true, &code);
    }
    SDL_DestroyProcess(process);
    if (code != 0) error = args[0] + " failed (exit " + std::to_string(code) + ")";
    return code == 0;
}

std::string Curl() {
#ifdef _WIN32
    return "curl.exe";
#else
    return "curl";
#endif
}

void Finish(const std::string &state, const std::string &error = {}) {
    std::lock_guard lock(S().mutex);
    S().status.state = state;
    S().status.error = error;
    S().busy = false;
    if (!error.empty()) Log::Warning("update", error);
}

void CheckWorker(std::string url) {
    std::string body, error;
    if (!Run({Curl(), "-sSfL", "--max-time", "20", "-H", "Accept: application/vnd.github+json", "-H",
              "User-Agent: Pocket2D-update", url},
             &body, error))
        return Finish("error", "update check: " + error);
    rapidjson::Document doc;
    doc.Parse(body.data(), body.size());
    if (doc.HasParseError() || !doc.IsObject() || !doc.HasMember("tag_name") || !doc["tag_name"].IsString())
        return Finish("error", "update check: no release found");
    std::string tag = doc["tag_name"].GetString();
    if (!tag.empty() && (tag[0] == 'v' || tag[0] == 'V')) tag.erase(0, 1);
    std::string notes = doc.HasMember("body") && doc["body"].IsString() ? doc["body"].GetString() : "";
    if (notes.size() > 600) notes.resize(600);
    std::string name, asset_url, digest;
    std::uint64_t size = 0;
    if (doc.HasMember("assets") && doc["assets"].IsArray()) {
        for (const char *token : kPlatform) {
            for (const auto &a : doc["assets"].GetArray()) {
                if (!a.IsObject() || !a.HasMember("name") || !a["name"].IsString() || !a.HasMember("browser_download_url") ||
                    !a["browser_download_url"].IsString())
                    continue;
                const std::string n = Lower(a["name"].GetString());
                if (n.size() < 4 || n.substr(n.size() - 4) != ".zip" || n.find(token) == std::string::npos) continue;
                name = a["name"].GetString();
                asset_url = a["browser_download_url"].GetString();
                if (a.HasMember("size") && a["size"].IsUint64()) size = a["size"].GetUint64();
                if (a.HasMember("digest") && a["digest"].IsString()) digest = a["digest"].GetString();
                break;
            }
            if (!name.empty()) break;
        }
    }
    std::lock_guard lock(S().mutex);
    Status &st = S().status;
    st.latest = tag, st.notes = notes, st.asset = name, st.size = size, st.progress = 0.0, st.error.clear();
    S().url = asset_url;
    S().digest = digest.rfind("sha256:", 0) == 0 ? Lower(digest.substr(7)) : "";
    S().busy = false;
    if (CompareVersions(tag, st.current) <= 0) st.state = "current";
    else if (name.empty()) st.state = "error", st.error = "release " + tag + " has no package for this platform";
    else if (S().digest.empty()) st.state = "error", st.error = "release " + tag + " publishes no SHA-256 digest for " + name;
    else st.state = "available";
    Log::Info("update", "latest release " + tag + " (this build " + st.current + "): " + st.state);
}

fs::path WorkDir() {
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / "pocket2d-update";
    fs::create_directories(dir, ec);
    return dir;
}

void DownloadWorker(std::string url, std::string digest, fs::path target) {
    std::error_code ec;
    fs::remove(target, ec);
    std::string error;
    if (!Run({Curl(), "-sSfL", "--max-time", "1800", "-H", "User-Agent: Pocket2D-update", "-o", U8(target), url},
             nullptr, error))
        return Finish("error", "update download: " + error);
    std::ifstream in(target, std::ios::binary);
    Sha256 hash;
    std::vector<char> chunk(1 << 20);
    while (in) {
        in.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        if (in.gcount() > 0) hash.Add(chunk.data(), static_cast<std::size_t>(in.gcount()));
    }
    if (hash.Hex() != digest) {
        fs::remove(target, ec);
        return Finish("error", "update download: the package's SHA-256 does not match the release");
    }
    {
        std::lock_guard lock(S().mutex);
        S().download = target;
        S().status.progress = 1.0;
    }
    Finish("ready");
}

// The package this process runs from: the .app on macOS, the folder of the
// executable elsewhere. Empty unless it holds a packaged project.
fs::path PackageRoot() {
    const char *base = SDL_GetBasePath();
    if (!base) return {};
    fs::path dir = fs::path(base);
    if (dir.filename().empty()) dir = dir.parent_path(); // SDL ends the base path with a separator
    std::error_code ec;
    if (!fs::is_regular_file(dir / "project" / "game.config", ec)) return {};
#ifdef __APPLE__
    const fs::path app = dir.parent_path().parent_path(); // Name.app/Contents/Resources
    return app.extension() == ".app" ? app : fs::path();
#else
    return dir;
#endif
}

// The unpacked package: its .app (macOS) or its single top folder.
fs::path FindPackage(const fs::path &dir) {
    std::error_code ec;
    std::vector<fs::path> entries;
    for (const auto &e : fs::directory_iterator(dir, ec))
        if (e.is_directory(ec) && e.path().filename().string().rfind("__MACOSX", 0) != 0) entries.push_back(e.path());
#ifdef __APPLE__
    for (const auto &e : entries)
        if (e.extension() == ".app") return e;
    return {};
#else
    if (fs::is_regular_file(dir / "project" / "game.config", ec)) return dir;
    return entries.size() == 1 ? entries[0] : fs::path();
#endif
}

std::string ProjectVersion(const fs::path &game_config) {
    std::ifstream in(game_config, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    rapidjson::Document doc;
    doc.Parse(text.data(), text.size());
    if (doc.HasParseError() || !doc.IsObject() || !doc.HasMember("version") || !doc["version"].IsString()) return "";
    return doc["version"].GetString();
}

bool StartBackground(const std::vector<std::string> &args, std::string &error) {
    std::vector<const char *> argv;
    for (const auto &a : args) argv.push_back(a.c_str());
    argv.push_back(nullptr);
    const SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, argv.data());
    SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_BACKGROUND_BOOLEAN, true);
    SDL_Process *process = SDL_CreateProcessWithProperties(props);
    SDL_DestroyProperties(props);
    if (!process) {
        error = "cannot start the update helper: " + std::string(SDL_GetError());
        return false;
    }
    SDL_DestroyProcess(process);
    return true;
}

void InstallWorker(fs::path zip, std::string latest, fs::path root) {
    auto fail = [](const std::string &message) { Finish("error", message); };
    std::error_code ec;
    // Unpacked beside the package, so that swapping the two is a rename on one volume
    // (the temporary folder when that one cannot be written).
    fs::path staging = root;
    staging += ".update-new";
    fs::remove_all(staging, ec);
    ec.clear();
    if (!fs::create_directories(staging, ec) || ec) {
        staging = WorkDir() / ("unpacked-" + latest);
        fs::remove_all(staging, ec);
        fs::create_directories(staging, ec);
    }
    std::string error;
#if defined(__APPLE__)
    const bool unpacked = Run({"ditto", "-x", "-k", U8(zip), U8(staging)}, nullptr, error);
#elif defined(_WIN32)
    const bool unpacked = Run({"tar.exe", "-xf", U8(zip), "-C", U8(staging)}, nullptr, error);
#else
    const bool unpacked = Run({"unzip", "-q", U8(zip), "-d", U8(staging)}, nullptr, error);
#endif
    if (!unpacked) return fail("cannot unpack the update: " + error);
    const fs::path fresh = FindPackage(staging);
#ifdef __APPLE__
    const fs::path fresh_config = fresh / "Contents" / "Resources" / "project" / "game.config";
#else
    const fs::path fresh_config = fresh / "project" / "game.config";
#endif
    if (fresh.empty() || CompareVersions(ProjectVersion(fresh_config), latest) != 0)
        return fail("the update package does not hold version " + latest);
#ifdef _WIN32
    const std::string pid = std::to_string(GetCurrentProcessId());
    wchar_t module[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, module, MAX_PATH);
    const std::string exe = U8(fs::path(std::wstring(module, length)).filename());
    // Wait for this process, swap the folders (renames; the old one goes back if the new
    // one cannot move in; retried while something still holds a file), start the game.
    // Only when no rename works (another volume) are the files copied over the old ones.
    auto quote = [](std::string s) {
        std::string out = "'";
        for (char c : s) out += c == '\'' ? std::string("''") : std::string(1, c);
        return out + "'";
    };
    const fs::path script = WorkDir() / "install.ps1";
    {
        std::ofstream out(script, std::ios::binary);
        out << "\xEF\xBB\xBF"
            << "$old = " << quote(U8(root)) << "\r\n$new = " << quote(U8(fresh)) << "\r\n$stage = " << quote(U8(staging))
            << "\r\n$bak = $old + '.update-old'\r\n"
            << "try { Wait-Process -Id " << pid << " -ErrorAction SilentlyContinue } catch {}\r\n"
            << "Start-Sleep -Milliseconds 300\r\n"
            << "$swapped = $false\r\n"
            << "for ($i = 0; $i -lt 20 -and -not $swapped; $i++) {\r\n"
            << "  try {\r\n"
            << "    if (Test-Path -LiteralPath $bak) { Remove-Item -LiteralPath $bak -Recurse -Force -ErrorAction Stop }\r\n"
            << "    Rename-Item -LiteralPath $old -NewName (Split-Path $bak -Leaf) -ErrorAction Stop\r\n"
            << "    try { Move-Item -LiteralPath $new -Destination $old -ErrorAction Stop; $swapped = $true }\r\n"
            << "    catch { Rename-Item -LiteralPath $bak -NewName (Split-Path $old -Leaf); throw }\r\n"
            << "  } catch { Start-Sleep -Milliseconds 500 }\r\n"
            << "}\r\n"
            << "if ($swapped) { Remove-Item -LiteralPath $bak -Recurse -Force -ErrorAction SilentlyContinue }\r\n"
            << "else { Copy-Item -Path (Join-Path $new '*') -Destination $old -Recurse -Force }\r\n"
            << "Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue\r\n"
            << "Start-Process -FilePath (Join-Path $old " << quote(exe) << ")\r\n";
    }
    if (!StartBackground({"powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-WindowStyle", "Hidden", "-File",
                          U8(script)},
                         error))
        return fail(error);
#else
    const std::string pid = std::to_string(getpid());
    fs::path backup = root;
    backup += ".update-old";
#ifdef __APPLE__
    const std::string relaunch = "open \"$2\"";
#else
    std::string exe = fs::read_symlink("/proc/self/exe", ec).filename().string();
    const std::string relaunch = "\"$2/" + exe + "\" >/dev/null 2>&1 &";
#endif
    // Wait for this process, swap the packages (putting the old one back if the
    // new one cannot move in), start the game.
    const std::string script = "while kill -0 \"$1\" 2>/dev/null; do sleep 0.2; done; rm -rf \"$4\"; "
                               "if mv \"$2\" \"$4\"; then if mv \"$3\" \"$2\"; then rm -rf \"$4\"; else mv \"$4\" \"$2\"; fi; fi; "
                               "rm -rf \"$5\"; " +
                               relaunch;
    // $5, the unpacking folder, is gone once the package moved out of it (or is the package itself).
    if (!StartBackground({"/bin/sh", "-c", script, "sh", pid, U8(root), U8(fresh), U8(backup), U8(staging)}, error))
        return fail(error);
#endif
    Log::Info("update", "installing " + latest + " into " + U8(root) + " once the game has quit");
    Finish("restart");
}

} // namespace

int CompareVersions(const std::string &a, const std::string &b) {
    auto parts = [](std::string s) {
        if (!s.empty() && (s[0] == 'v' || s[0] == 'V')) s.erase(0, 1);
        std::vector<long> out;
        std::size_t i = 0;
        while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
            long v = 0;
            while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) v = v * 10 + (s[i++] - '0');
            out.push_back(v);
            if (i < s.size() && s[i] == '.') ++i;
            else break;
        }
        return out;
    };
    const auto x = parts(a), y = parts(b);
    for (std::size_t i = 0; i < std::max(x.size(), y.size()); ++i) {
        const long p = i < x.size() ? x[i] : 0, q = i < y.size() ? y[i] : 0;
        if (p != q) return p < q ? -1 : 1;
    }
    return 0;
}

void Configure(const std::string &version, const std::string &github_repo) {
    std::lock_guard lock(S().mutex);
    State &s = S();
    s.status = Status{};
    s.status.current = version;
    s.status.installable = !PackageRoot().empty();
    s.repo = github_repo;
    const char *api = SDL_getenv("POCKET_UPDATE_API");
    s.url.clear();
    if (api && *api) s.repo = api; // a full URL of the latest release's JSON
    s.status.state = s.repo.empty() || version.empty() ? "off" : "idle";
}

void Check() {
    std::string url;
    {
        std::lock_guard lock(S().mutex);
        State &s = S();
        if (s.status.state == "off" || s.busy) return;
        url = s.repo.find("://") != std::string::npos ? s.repo : "https://api.github.com/repos/" + s.repo + "/releases/latest";
        s.busy = true;
        s.status.state = "checking";
        s.status.error.clear();
    }
    std::thread(CheckWorker, url).detach();
}

void Download() {
    std::string url, digest;
    fs::path target;
    {
        std::lock_guard lock(S().mutex);
        State &s = S();
        if (s.busy || s.status.state != "available") return;
        url = s.url, digest = s.digest;
        target = WorkDir() / ("download-" + s.status.latest + ".zip");
        s.download = target;
        s.busy = true;
        s.status.state = "downloading";
        s.status.progress = 0.0;
    }
    std::thread(DownloadWorker, url, digest, target).detach();
}

bool Install() {
    fs::path zip;
    std::string latest;
    const fs::path root = PackageRoot();
    {
        std::lock_guard lock(S().mutex);
        if (S().busy || S().status.state != "ready") return false;
        if (root.empty()) {
            S().status.error = "this build is not a packaged game, so it cannot replace itself";
            return false;
        }
        zip = S().download, latest = S().status.latest;
        S().busy = true;
        S().status.state = "installing";
        S().status.error.clear();
    }
    std::thread(InstallWorker, zip, latest, root).detach();
    return true;
}

Status GetStatus() {
    std::lock_guard lock(S().mutex);
    Status st = S().status;
    if (st.state == "downloading" && st.size > 0) {
        std::error_code ec;
        const auto have = fs::file_size(S().download, ec);
        if (!ec) st.progress = std::min(0.99, static_cast<double>(have) / static_cast<double>(st.size));
    }
    return st;
}

} // namespace AppUpdate
