# dot companion (Android)

A minimal Android client that connects to Microsoft [Agent Host Protocol](https://microsoft.github.io/agent-host-protocol/)
(AHP) servers over a WebSocket and mirrors their chat sessions: it subscribes to every session and
chat on each host, renders the transcript, and streams assistant replies live as they arrive.

The application ID is `com.dot.companion`. Preferences from other application IDs are not imported.
Its launcher uses the [at-rest owl](../common/icons/owl_idle.svg).

It points at a **cptower** multiplexer rather than a single AHP host: cptower
discovers every live Copilot AHP host on a machine and exposes them behind one port, and the app
merges all of their sessions into one picker.

## What it does

- Connects to a cptower endpoint (`host:port`, or a full `ws://` / `wss://` URL) you type into the
  top bar.
- Fetches cptower's `GET /hosts` catalog and opens one connection per advertised host at
  `/ws/<port>`. If the endpoint is not a cptower instance (no `/hosts`), it falls back to a single
  direct connection to the endpoint itself.
- Runs the AHP handshake per host — `initialize` → `listSessions` → `subscribe` to each session, then
  to each of its chats — and folds the `chat` action stream through the canonical AHP reducers.
- Merges every host's sessions into one picker, each entry tagged with its host label; selecting one
  scopes the transcript to that host's session.
- Renders the transcript for the selected chat: the user prompt that started each turn, followed by
  the assistant reply. Markdown response parts render as text; every other AHP part kind (reasoning,
  tool calls, input requests, errors, notifications, resources) is currently dumped as a labeled JSON
  block so its raw content is visible pending dedicated UI.
- Sends what you type as a new turn (`chat/turnStarted` dispatched to the default chat).

The wire types, reducers, and JSON-RPC helpers come from the official Kotlin client
(`com.microsoft.agenthostprotocol:agent-host-protocol`); this app supplies only the OkHttp WebSocket
transport, the reactive handshake/subscription driver, and the transcript projection.

## Layout

```
android/
  settings.gradle.kts, build.gradle.kts, gradle.properties
  app/
    build.gradle.kts
    src/main/java/com/dot/companion/
      model/SessionInfo.kt            # a selectable session, tagged with its cptower host
      model/UiMessage.kt              # one rendered transcript row
      net/  ConnectionState.kt        # Disconnected / Connecting / Connected
            AhpConnection.kt          # OkHttp WebSocket + JSON-RPC driver + AHP state mirror
            HostCatalog.kt            # fetches cptower's GET /hosts catalog
      AhpTranscript.kt                # pure ChatState -> transcript projection (JVM-unit-testable)
      ChatViewModel.kt                # owns one connection per host, merges sessions + transcript
      ui/ChatScreen.kt                # Compose transcript + input + endpoint bar + session picker
      MainActivity.kt
    src/test/java/com/dot/companion/AhpTranscriptTest.kt   # reducer fold + projection (pure JVM)
    src/debug/                        # debug-only cleartext network-security config
```

## Prerequisites (headless — no Android Studio)

- **JDK 17** — set `JAVA_HOME` to it.
- **Android SDK** (`cmdline-tools;latest` + `platforms;android-34` + `build-tools;35.0.0` + `platform-tools`)
  — set `ANDROID_HOME`, or write `android/local.properties` with `sdk.dir=<path>`.
- **Gradle** — not needed globally; use the committed `./gradlew` wrapper.

Example (PowerShell, matching this repo's dev setup):

```ps1
$env:JAVA_HOME    = "$env:LOCALAPPDATA\Java\jdk-17"
$env:ANDROID_HOME = "$env:LOCALAPPDATA\Android\Sdk"
```

`local.properties` is git-ignored (it holds a machine-specific absolute path). Create it once:

```ps1
"sdk.dir=$($env:ANDROID_HOME.Replace("\", "\\").Replace(":", "\:"))" | Set-Content android\local.properties
```

## Build & test

From `android/`:

```ps1
.\gradlew.bat test           # JVM unit tests (reducer fold + transcript projection)
.\gradlew.bat assembleDebug  # -> app/build/outputs/apk/debug/app-debug.apk
.\gradlew.bat assembleRelease
```

Without signing inputs, `assembleRelease` produces `app\build\outputs\apk\release\app-release-unsigned.apk`.
Release builds use R8 and resource shrinking.

The app tracks the AHP client's Kotlin toolchain (Kotlin 2.3.21, Compose compiler applied via the
`org.jetbrains.kotlin.plugin.compose` plugin), so the newer-Kotlin AHP artifact is consumed directly
with no metadata-version workarounds.

## Versioning

See the repository's [independent component versioning](../README.md#versioning).

## Release signing

### Provision the certificate

Generate the release key on a trusted offline machine, outside the checkout. Choose your certificate subject when
`keytool` prompts. Use a strong password and retain an encrypted offline backup of the PKCS12 file and its credentials.
Keep this signing identity for future updates; a different key cannot update an installed APK.

```ps1
$keystore = "D:\private\dot-android-release.p12"
keytool -genkeypair -keystore $keystore -storetype PKCS12 -alias dot-android-release `
  -keyalg RSA -keysize 4096 -sigalg SHA256withRSA -validity 10000
```

Do not put keystores, passwords, or their base64 encoding in the repository, command-line arguments, logs, or artifacts.

Create the GitHub Environment **`android-release`** with required reviewers, block administrator bypass, and allow
deployments only from tags matching `android-v*`. The tag pusher (the `RELEASE_TOKEN` identity) triggers the job, so
enable prevent self-review only when another reviewer can approve. Restrict creation/update/deletion of those tags to
the release identity with a repository ruleset. Reviewers must verify the tag commit belongs to `main` and the workflow
is trusted.

| Environment setting | Value |
| --- | --- |
| Secret `ANDROID_KEY_ALIAS` | Keystore alias |
| Secret `ANDROID_KEY_PASSWORD` | Key password (same as the store password for this PKCS12 file) |
| Secret `ANDROID_KEYSTORE_BASE64` | Base64 encoding of the entire PKCS12 file |
| Secret `ANDROID_KEYSTORE_PASSWORD` | Keystore password |

Transfer the key through a trusted channel to an authenticated provisioning machine. To populate the file secret
without printing it or writing an encoded copy:

```ps1
[Convert]::ToBase64String([IO.File]::ReadAllBytes($keystore)) |
  gh secret set ANDROID_KEYSTORE_BASE64 --env android-release --repo aloknigam247/dot
```

Use GitHub's masked secret entry for the remaining credentials. Keep the original offline; remove any provisioning copy.

### Protected tag builds

The [Android release workflow](../.github/workflows/android-release.yml) signs only stable tags matching `versionName`
on commits reachable from `main`. Tests run in ordinary Android CI, not on release tags.

After approval, the job builds and signs the optimized APK, removes the temporary keystore, and retains only
`app-release.apk` as a workflow artifact for **one day**. It does not publish a GitHub Release asset or App Bundle.

### Local signing

On a trusted machine, set these environment variables from your secret manager before running Gradle. Never use the
production key for routine development; use a disposable test key to exercise the signing configuration.

| Variable | Value |
| --- | --- |
| `DOT_ANDROID_KEY_ALIAS` | Keystore alias |
| `DOT_ANDROID_KEY_PASSWORD` | Key password |
| `DOT_ANDROID_KEYSTORE` | Absolute PKCS12 path outside the checkout |
| `DOT_ANDROID_KEYSTORE_PASSWORD` | Keystore password |

```ps1
.\gradlew.bat --no-daemon --no-configuration-cache assembleRelease -Pdot.signing.required=true
```

The signed output is `app\build\outputs\apk\release\app-release.apk`. Missing or partial signing inputs fail when
signing is required; partial inputs also fail ordinary builds. Clear the signing variables to return to unsigned builds.
Do not enable Gradle configuration caching or verbose/debug logging for builds using a private signing key.

### Install on a device

Download and extract the protected-tag artifact before it expires. Set `$apk` to the APK's absolute path:

```ps1
& "$env:ANDROID_HOME\platform-tools\adb.exe" install -r $apk
& "$env:ANDROID_HOME\platform-tools\adb.exe" shell am start -n com.dot.companion/.MainActivity
```

An existing debug-signed installation must be removed before installing this APK; uninstalling erases its local data.
Use a TLS-enabled `wss://` cptower/AHP endpoint with a certificate trusted by the device (release builds reject
cleartext).

## Run against cptower

Start the cptower multiplexer on the host machine (a separate host-side tool); it listens on
`0.0.0.0:8770` by default and discovers every live Copilot AHP host. Install and launch the app on a
USB-debugging device on the same network:

```ps1
$adb = "$env:ANDROID_HOME\platform-tools\adb.exe"
& $adb install -r app\build\outputs\apk\debug\app-debug.apk
& $adb shell am start -n com.dot.companion/.MainActivity
```

In the app, set the endpoint to `<HOST-LAN-IP>:8770` and Connect. The session picker lists every
session across all discovered hosts; pick one and its transcript appears and updates live. Typing a
message and Send starts a new turn. Pointing the endpoint straight at a single AHP host (no cptower)
also works via the direct-connection fallback.

## Cleartext note

The app talks plaintext `ws://` for LAN use. Cleartext is enabled **only** in the debug manifest
(`src/debug`) via a network-security config; the release manifest has no such allowance. Use `wss://`
against a TLS-terminating host for encrypted transport.
