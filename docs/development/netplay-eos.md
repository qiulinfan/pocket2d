# Netplay over Epic Online Services (planned)

Status: designed, not built. The EOS SDK is downloaded from the Epic Developer
Portal with the owner's account and is under Epic's license, so it cannot be
fetched by CMake or committed to this public repository. This page is the plan
for when the owner provides the SDK and the product's credentials.

## Why

Connection codes fail when both players sit behind strict NATs and have no
virtual LAN; the room server needs a VPS. EOS gives free lobbies (room codes),
NAT traversal and a relay through Epic's servers without either, and needs no
Epic account from players (the Connect interface's device ID login).

## What the owner provides

1. An Epic Games developer account (dev.epicgames.com) and an organization.
2. A product, with its **Product ID**, **Sandbox ID** and **Deployment ID**
   (Product Settings).
3. A client (Product Settings, Clients) with a policy that allows Connect,
   Lobby and P2P, and its **Client ID** and **Client Secret**.
4. The **EOS C SDK** for macOS and Windows (SDK download page), unpacked
   locally, for example to `~/sdks/EOS-SDK`. Its redistributable libraries
   (`libEOSSDK-Mac-Shipping.dylib`, `EOSSDK-Win64-Shipping.dll`) ship with the
   game.

Credentials are game configuration, not engine code: the game keeps them in a
file next to `game.config` (BleachVsNaruto: `netplay.json`, `"eos": {...}`),
which is ignored by Git.

## Engine design

- CMake option `POCKET_EOS` (default OFF) with `EOS_SDK_ROOT`; when ON,
  `src/engine/net/EosLink.cpp` builds and links the SDK, and the runtime kit
  installs its library beside the executable. Builds without the SDK are
  unchanged.
- `EosLink` is another `Link` (`src/engine/net/Link.h`): the session, rollback
  and lockstep do not change.
  - Startup: `EOS_Initialize`, `EOS_Platform_Create` with the credentials,
    `EOS_Connect_CreateDeviceId` then `EOS_Connect_Login` with
    `EOS_ECT_DEVICEID_ACCESS_TOKEN` (no Epic account), and `EOS_Platform_Tick`
    from `Link::Pump`.
  - Host: `EOS_Lobby_CreateLobby` (two members, presence off) with a
    four-character `ROOM` attribute; `Status().room` shows it.
  - Guest: `EOS_LobbySearch` on the `ROOM` attribute, `EOS_Lobby_JoinLobby`,
    then the host's product user ID from the lobby owner.
  - Datagrams: `EOS_P2P_SendPacket` / `EOS_P2P_ReceivePacket` on a socket
    name, unreliable unordered channel, `EOS_P2P_AcceptConnection` on the
    host; Epic's NAT traversal and relay fall back on their own
    (`EOS_P2P_SetRelayControl` allows relays). `relay` in the status reflects
    the connection type the SDK reports.
- Lua: `Net.EosHost(payload)` / `Net.EosJoin(room)`, with the credentials in
  `Net.Configure{eos = {product_id, sandbox_id, deployment_id, client_id,
  client_secret}}`; `Net.Status().link` is `"eos"`.
- Test: like `tests/netplay_test.py` with two runtimes logged in with two
  device IDs (`EOS_Connect_DeleteDeviceId` between runs), skipped when the
  SDK or credentials are absent.

## Game side (BleachVsNaruto)

A NET PLAY entry "EPIC ROOM" next to the room code entries, shown only when
the runtime was built with EOS and `netplay.json` has the credentials.
