# Updates

A packaged game can update itself from its GitHub Releases. game.config names
the build's version and the repository:

```json
"version": "1.2.0",
"update": {"github": "owner/repo"}
```

A release is tagged with the version (`v1.3.0`) and carries one `.zip` per
platform made by `pocket package`, its name saying `mac` (or `macos`),
`windows` or `linux`. GitHub publishes each asset's SHA-256; a download that
does not match it is refused. The transfers use the system `curl` (macOS,
Windows 10 and later and Linux ship it). Saves live outside the package and
are kept.

| Function | Description |
|----------|-------------|
| `Update.Check()` | Asks for the latest release in the background |
| `Update.Download()` | Fetches an `available` release's package and checks its digest |
| `Update.Install()` | After a download: unpacks it and starts a helper that swaps the package in once the game has quit, then starts it again; false when this build is not a package |
| `Update.Status()` | `{state, current, latest, notes, asset, size, progress, error, installable}` |

`state` is `off` (no version or repository), `idle`, `checking`, `current`
(up to date), `available`, `downloading` (`progress` 0..1), `ready`,
`installing`, `restart` (quit now: `Application.Quit()`) or `error`.
`installable` is true in packaged builds; a game usually checks at start only
then. `POCKET_UPDATE_API` replaces the release URL with any URL of a release's
JSON (tests, mirrors).
