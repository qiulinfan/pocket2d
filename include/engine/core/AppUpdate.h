#ifndef POCKET2D_APP_UPDATE_H
#define POCKET2D_APP_UPDATE_H

#include <cstdint>
#include <string>

// Updates of a packaged game from its GitHub Releases (game.config:
// "version": "1.2.0", "update": {"github": "owner/repo"}). Check asks the
// latest release in the background; a newer version with a .zip asset for
// this platform (its name says mac, windows or linux) is "available".
// Download fetches it and checks the SHA-256 digest GitHub publishes for it;
// Install unpacks it and starts a helper that swaps the package in once this
// process has ended and starts the game again: at "restart" the game quits.
// Saves live outside the package and are kept. The system curl does the
// transfers (macOS, Windows 10+ and Linux ship it); POCKET_UPDATE_API
// replaces the release URL (tests, mirrors).
namespace AppUpdate {

struct Status {
    // off, idle, checking, current, available, downloading, ready, installing, restart, error
    std::string state = "off";
    std::string current;       // this build's version
    std::string latest;        // the latest release's version
    std::string notes;         // its release notes (the start)
    std::string asset;         // the chosen asset's name
    std::uint64_t size = 0;    // its size in bytes
    double progress = 0.0;     // download progress 0..1
    std::string error;
    bool installable = false;  // a packaged build that Install can replace
};

void Configure(const std::string &version, const std::string &github_repo);
void Check();
void Download();
// After a download, in the background: unpacks the package and starts the
// helper (state "restart": quit now). False when it cannot start (not a
// packaged build: Status().error says so).
bool Install();
Status GetStatus();

// -1, 0 or 1 by numeric components ("v1.10" > "1.9"; missing components are 0).
int CompareVersions(const std::string &a, const std::string &b);

} // namespace AppUpdate

#endif
