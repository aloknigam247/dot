# Android companion and console client

[settings.gradle.kts](settings.gradle.kts) defines the modules; [README.md](README.md) owns setup and product usage.

## Modules

- [app](app/build.gradle.kts): Android application, Compose UI, and lifecycle adapter.
- [chat_cli](chat_cli/build.gradle.kts): Windows console client using the shared JVM client.
- [chat_client](chat_client/build.gradle.kts): Android-free AHP client and transcript projection.

## Entry points

Android package sources are under `app\src\main\java\com\dot\companion`:

- `ChatViewModel.kt` owns a `ChatClient` in `viewModelScope` and closes it from `onCleared`.
- `MainActivity.kt` restores endpoint preferences and launches `ui\ChatScreen.kt`.
- `ui\ChatScreen.kt` renders the Compose interface; use the [design index](../docs/AGENTS.md) for its approved target.

Shared client sources are under `chat_client\src\main\java\com\dot\companion`:

- `AhpTranscript.kt` projects folded AHP chat state into `model\UiMessage.kt` rows.
- `ChatClient.kt` owns host connections, merged session state, and the selected transcript.
- `net\AhpConnection.kt` owns WebSocket/JSON-RPC exchange, AHP handshake, subscriptions, and reducer application.
- `net\HostCatalog.kt` discovers cptower hosts; `net\HostConnection.kt` is the injectable connection boundary.
- `net\HostProgress.kt` defines progress and redacted diagnostics.

Console startup is `chat_cli\src\main\kotlin\com\dot\companion\cli\Main.kt`;
`ChatCli.kt` owns commands and terminal output.
Keep transport/transcript behavior in `chat_client`, Android lifecycle/UI in `app`, and terminal behavior in `chat_cli`.
Respect `ChatClient`'s serial-dispatcher contract and caller-owned scope during cancellation and shutdown.

## Build and validation

Run Gradle from `android\` using `.\gradlew.bat`; see [README.md](README.md#build--test) for setup and build commands.
The [Android CI workflow](../.github/workflows/android-ci.yml) defines lint, JVM-test, coverage, and APK gates;
module `build.gradle.kts` files own task configuration and coverage exclusions.
Tests live under each module's `src\test`; desktop CTest results do not validate these modules.
For changes that affect on-device behavior, use the [local emulator](README.md#local-android-emulator-windows-x64)
to reproduce, debug, and validate the affected path. Keep emulator runs out of CI.

For the native console executable, use [build_chat_cli.ps1](scripts/build_chat_cli.ps1); it configures MSVC and the
GraalVM pinned by [graalvm.properties](graalvm.properties) without replacing Gradle's JDK.
See [console instructions](README.md#windows-console-client) for native build and smoke-test requirements.

## Boundaries

- **Assets:** [app/build.gradle.kts](app/build.gradle.kts) includes the shared Android icon resource directory;
  edit artwork through the [common guide](../common/AGENTS.md).
- **Formatting:** use [.editorconfig](.editorconfig) and [.gitattributes](.gitattributes); preserve Gradle wrapper EOLs.
- **Networking:** [src/debug](app/src/debug) owns cleartext allowances. Do not move debug networking policy into the
  release manifest; follow [release signing](README.md#release-signing) and its TLS requirements.
- **Protocol:** use the official AHP types/reducers from `chat_client` dependencies, not duplicated wire models.
- **Releases:** use [common release tooling](../common/AGENTS.md#release-tooling) and the existing signing procedure;
  never store signing material in the checkout.
