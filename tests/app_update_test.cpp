// AppUpdate without the network: SHA-256 vectors, version order, and a check
// and download against a release JSON and a package served by file:// URLs.
#include "core/AppUpdate.h"
#include "shared/persistence/Sha256.h"

#include <SDL3/SDL.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace {
void Check(bool condition, const char *message) {
    if (condition) return;
    std::fprintf(stderr, "app_update_test: %s\n", message);
    std::exit(1);
}

std::string WaitWhile(const char *state) {
    for (int i = 0; i < 500 && AppUpdate::GetStatus().state == state; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    return AppUpdate::GetStatus().state;
}

std::string FileUrl(const std::filesystem::path &path) {
    const std::string p = std::filesystem::absolute(path).generic_string();
    return (p.rfind('/', 0) == 0 ? "file://" : "file:///") + p; // file:///C:/... on Windows
}

void Release(const std::filesystem::path &json, const std::string &tag, const std::filesystem::path &zip, const std::string &digest) {
    std::ofstream(json) << R"({"tag_name":")" << tag << R"(","body":"notes","assets":[)"
                        << R"({"name":"Game-source.zip","browser_download_url":"file:///nowhere","size":1},)"
                        << R"({"name":"Game-)"
#if defined(__APPLE__)
                        << "mac"
#elif defined(_WIN32)
                        << "windows"
#else
                        << "linux"
#endif
                        << R"(.zip","browser_download_url":")" << FileUrl(zip) << R"(","size":)"
                        << std::filesystem::file_size(zip) << R"(,"digest":"sha256:)" << digest << R"("}]})";
}
} // namespace

int main() {
    Check(Sha256::Of("", 0) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "sha256 of nothing");
    Check(Sha256::Of("abc", 3) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "sha256 of abc");
    const std::string million(1000000, 'a');
    Check(Sha256::Of(million.data(), million.size()) ==
              "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
          "sha256 of a million a");
    Check(AppUpdate::CompareVersions("v1.10", "1.9") == 1 && AppUpdate::CompareVersions("1.2", "1.2.0") == 0 &&
              AppUpdate::CompareVersions("1.2.0", "1.3") == -1,
          "versions compare by numeric components");

    AppUpdate::Configure("1.0.0", "");
    Check(AppUpdate::GetStatus().state == "off", "no repository: off");

    const auto dir = std::filesystem::temp_directory_path() / "pocket2d_app_update_test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const auto zip = dir / "package.zip";
    std::ofstream(zip, std::ios::binary) << "not really a zip, but some bytes";
    std::ifstream in(zip, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const auto json = dir / "latest.json";
    SDL_setenv_unsafe("POCKET_UPDATE_API", FileUrl(json).c_str(), 1);

    Release(json, "v1.0.0", zip, Sha256::Of(bytes.data(), bytes.size()));
    AppUpdate::Configure("1.0.0", "owner/repo");
    AppUpdate::Check();
    Check(WaitWhile("checking") == "current", "the same version is current");

    Release(json, "v1.1.0", zip, Sha256::Of(bytes.data(), bytes.size()));
    AppUpdate::Check();
    auto st = AppUpdate::GetStatus();
    Check(WaitWhile("checking") == "available", "a newer release is available");
    st = AppUpdate::GetStatus();
    Check(st.latest == "1.1.0" && st.notes == "notes" && st.asset.find("source") == std::string::npos, "the platform's asset");
    AppUpdate::Download();
    Check(WaitWhile("downloading") == "ready", "the download matches its digest");
    Check(!AppUpdate::GetStatus().installable && !AppUpdate::Install(), "a test binary is not a package to replace");

    Release(json, "v1.2.0", zip, std::string(64, '0'));
    AppUpdate::Configure("1.0.0", "owner/repo");
    AppUpdate::Check();
    Check(WaitWhile("checking") == "available", "a release with a wrong digest is still offered");
    AppUpdate::Download();
    Check(WaitWhile("downloading") == "error", "a download that does not match its digest is refused");

    std::filesystem::remove_all(dir);
    std::puts("app_update_test passed");
    return 0;
}
