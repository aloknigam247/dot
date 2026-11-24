mod cli;
mod protocol;
mod session;

use std::io::BufRead;
use std::process::ExitCode;
use std::time::Duration;

use copilot_sdk::Client;
use tokio::sync::mpsc;

use crate::cli::ResolveError;
use crate::protocol::{ErrorKind, HelperMessage, Output, PROTOCOL_VERSION};
use crate::session::{SdkSession, SessionController};

const DEFAULT_STARTUP_TIMEOUT: Duration = Duration::from_secs(30);
const STARTUP_TIMEOUT_VARIABLE: &str = "DOT_COPILOT_HELPER_STARTUP_TIMEOUT_MS";

#[tokio::main]
async fn main() -> ExitCode {
    run_stdio().await.unwrap_or(ExitCode::FAILURE)
}

async fn run_stdio() -> std::io::Result<ExitCode> {
    let cwd = std::env::current_dir()?;
    let limit = startup_timeout();
    let mut out = Output::new(tokio::io::stdout());
    let requests = spawn_stdin_reader();
    let (client, resolution) = match cli::resolve_cli(&cwd, limit).await {
        Ok(resolved) => resolved,
        Err(error) => {
            out.emit(&resolve_error_message(error)).await?;
            return Ok(ExitCode::FAILURE);
        }
    };
    let sdk = match SdkSession::start(&client, &cwd, limit).await {
        Ok(sdk) => sdk,
        Err(message) => {
            let emitted = out
                .emit(&HelperMessage::startup_error(
                    ErrorKind::SessionStartFailed,
                    message,
                    None,
                ))
                .await;
            stop_client(&client).await;
            return emitted.map(|()| ExitCode::FAILURE);
        }
    };
    out.emit(&HelperMessage::Ready {
        cli: resolution,
        protocol: PROTOCOL_VERSION,
    })
    .await?;
    let result = SessionController::new(sdk, out).run_session(requests).await;
    stop_client(&client).await;
    result.map(|()| ExitCode::SUCCESS)
}

fn resolve_error_message(error: ResolveError) -> HelperMessage {
    match error {
        ResolveError::NotFound => HelperMessage::startup_error(
            ErrorKind::CliNotFound,
            "no copilot executable was found on PATH",
            Some(Vec::new()),
        ),
        ResolveError::Unusable(candidates) => HelperMessage::startup_error(
            ErrorKind::CliUnusable,
            format!(
                "no copilot candidate completed a protocol {} handshake",
                cli::REQUIRED_PROTOCOL
            ),
            Some(candidates),
        ),
    }
}

// A blocked tokio stdin read would keep the runtime alive after shutdown, so lines are read on a plain thread.
// Raw bytes let the controller reject a non-UTF-8 line as an invalid request instead of ending input.
fn spawn_stdin_reader() -> mpsc::Receiver<std::io::Result<Vec<u8>>> {
    let (tx, rx) = mpsc::channel(64);
    std::thread::spawn(move || {
        let mut stdin = std::io::stdin().lock();
        loop {
            let mut line = Vec::new();
            let read = match stdin.read_until(b'\n', &mut line) {
                Ok(0) => break,
                result => result.map(|_| line),
            };
            let failed = read.is_err();
            if tx.blocking_send(read).is_err() || failed {
                break;
            }
        }
    });
    rx
}

fn startup_timeout() -> Duration {
    std::env::var(STARTUP_TIMEOUT_VARIABLE)
        .ok()
        .and_then(|value| value.parse().ok())
        .map_or(DEFAULT_STARTUP_TIMEOUT, Duration::from_millis)
}

async fn stop_client(client: &Client) {
    if client.stop().await.is_err() {
        client.force_stop();
    }
}
