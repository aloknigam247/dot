use std::ffi::OsStr;
use std::path::{Path, PathBuf};
use std::process::Stdio;
use std::time::Duration;

use copilot_sdk::{
    CliProgram, Client, ClientMode, ClientOptions, ErrorKind, ProtocolErrorKind, RuntimeConnection,
    SpawnedProcessOptions,
};
use serde::Serialize;
use tokio::time::timeout;

use crate::protocol::{CandidateOutcome, CandidateReport};

pub const REQUIRED_PROTOCOL: u32 = 4;

const CLI_NAME: &str = "copilot";
const DEFAULT_PATHEXT: &str = ".COM;.EXE;.BAT;.CMD";
const SERVER_ARGS: [&str; 3] = ["--server", "--stdio", "--no-auto-update"];

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum AuthStatus {
    SignedIn,
    SignedOut,
    Unknown,
}

#[derive(Debug, Serialize)]
pub struct CliResolution {
    pub auth: AuthStatus,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub auth_error: Option<String>,
    pub path: PathBuf,
    pub protocol: u32,
    pub version: Option<String>,
}

pub enum ResolveError {
    NotFound,
    Unusable(Vec<CandidateReport>),
}

pub async fn resolve_cli(cwd: &Path, limit: Duration) -> Result<(Client, CliResolution), ResolveError> {
    let mut reports = Vec::new();
    let path = std::env::var_os("PATH");
    let pathext = std::env::var_os("PATHEXT");
    let candidates = path_candidates(path.as_deref(), pathext.as_deref());
    if candidates.is_empty() {
        return Err(ResolveError::NotFound);
    }
    for candidate in candidates {
        match handshake_candidate(&candidate, cwd, limit).await {
            Ok(resolved) => return Ok(resolved),
            Err(report) => reports.push(report),
        }
    }
    Err(ResolveError::Unusable(reports))
}

fn path_candidates(path: Option<&OsStr>, pathext: Option<&OsStr>) -> Vec<PathBuf> {
    let mut candidates: Vec<PathBuf> = Vec::new();
    let extensions = executable_extensions(pathext);
    // Relative PATH entries would make discovery depend on the working directory.
    for dir in std::env::split_paths(path.unwrap_or_default()).filter(|dir| dir.is_absolute()) {
        for extension in &extensions {
            let candidate = dir.join(format!("{CLI_NAME}{extension}"));
            if candidate.is_file() && !candidates.contains(&candidate) {
                candidates.push(candidate);
            }
        }
    }
    candidates
}

fn executable_extensions(pathext: Option<&OsStr>) -> Vec<String> {
    pathext
        .map_or(Some(DEFAULT_PATHEXT), OsStr::to_str)
        .unwrap_or_default()
        .split(';')
        .filter(|extension| extension.len() > 1 && extension.starts_with('.'))
        .map(str::to_ascii_lowercase)
        .collect()
}

async fn handshake_candidate(
    path: &Path,
    cwd: &Path,
    limit: Duration,
) -> Result<(Client, CliResolution), CandidateReport> {
    let options = ClientOptions::new(ClientMode::CodingAgent)
        .with_observability(|observability| observability.with_disable_client_telemetry(true))
        .with_runtime(RuntimeConnection::for_spawned_stdio(
            SpawnedProcessOptions::new()
                .with_program(CliProgram::Path(path.to_path_buf()))
                .with_working_directory(cwd),
        ));
    let client = match timeout(limit, Client::start(options)).await {
        Err(_) => return Err(timed_out(path, limit)),
        Ok(Err(error)) => {
            return Err(match error.kind() {
                ErrorKind::Protocol(ProtocolErrorKind::VersionMismatch { server, .. }) => CandidateReport {
                    cli_protocol: Some(*server),
                    cli_version: probe_version(path, cwd, limit).await,
                    message: error.to_string(),
                    outcome: CandidateOutcome::ProtocolMismatch,
                    path: path.to_path_buf(),
                    required_protocol: Some(REQUIRED_PROTOCOL),
                },
                _ => report(path, CandidateOutcome::StartupFailed, error.to_string()),
            });
        }
        Ok(Ok(client)) => client,
    };
    // The SDK accepts a handshake without a protocol version, which cannot prove compatibility.
    if client.protocol_version() != Some(REQUIRED_PROTOCOL) {
        client.force_stop();
        return Err(report(
            path,
            CandidateOutcome::ProtocolUnverified,
            "handshake completed without reporting a protocol version".to_owned(),
        ));
    }
    let resolution = describe(&client, path, limit).await;
    Ok((client, resolution))
}

// Version and auth are reported only when the CLI answers in time; neither decides whether the CLI is usable.
async fn describe(client: &Client, path: &Path, limit: Duration) -> CliResolution {
    let (status, auth_status) = tokio::join!(
        timeout(limit, client.get_status()),
        timeout(limit, client.get_auth_status())
    );
    let version = status.ok().and_then(Result::ok).map(|status| status.version);
    let (auth, auth_error) = match auth_status {
        Ok(Ok(status)) if status.is_authenticated => (AuthStatus::SignedIn, None),
        Ok(Ok(_)) => (AuthStatus::SignedOut, None),
        Ok(Err(error)) => (AuthStatus::Unknown, Some(error.to_string())),
        Err(_) => (
            AuthStatus::Unknown,
            Some(format!("no auth status within {} ms", limit.as_millis())),
        ),
    };
    CliResolution {
        auth,
        auth_error,
        path: path.to_path_buf(),
        protocol: REQUIRED_PROTOCOL,
        version,
    }
}

// The SDK rejects a mismatched CLI before exposing its version, so ask the CLI directly over a raw connection.
async fn probe_version(path: &Path, cwd: &Path, limit: Duration) -> Option<String> {
    let mut child = tokio::process::Command::new(path)
        .args(SERVER_ARGS)
        .current_dir(cwd)
        .env("COPILOT_TELEMETRY_DISABLED", "true")
        .kill_on_drop(true)
        .stderr(Stdio::null())
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .spawn()
        .ok()?;
    let reader = child.stdout.take()?;
    let writer = child.stdin.take()?;
    let client = Client::from_streams(ClientMode::CodingAgent, reader, writer, cwd.to_path_buf()).ok()?;
    let status = timeout(limit, client.get_status()).await;
    client.force_stop();
    let _ = child.kill().await;
    status.ok()?.ok().map(|status| status.version)
}

fn report(path: &Path, outcome: CandidateOutcome, message: String) -> CandidateReport {
    CandidateReport {
        cli_protocol: None,
        cli_version: None,
        message,
        outcome,
        path: path.to_path_buf(),
        required_protocol: None,
    }
}

fn timed_out(path: &Path, limit: Duration) -> CandidateReport {
    report(
        path,
        CandidateOutcome::TimedOut,
        format!("no protocol handshake within {} ms", limit.as_millis()),
    )
}
