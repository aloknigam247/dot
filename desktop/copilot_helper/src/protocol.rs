use std::path::PathBuf;

use serde::{Deserialize, Serialize};
use tokio::io::{AsyncWrite, AsyncWriteExt};

use crate::cli::CliResolution;

pub const PROTOCOL_VERSION: u32 = 1;

#[derive(Debug, Deserialize)]
pub struct ControlRequest {
    #[serde(flatten)]
    pub kind: RequestKind,
    pub request_id: String,
}

#[derive(Debug, Deserialize)]
#[serde(rename_all = "snake_case", tag = "type")]
pub enum RequestKind {
    Cancel { turn_id: String },
    Send { prompt: String },
    Shutdown,
}

#[derive(Clone, Copy, Debug, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum CancelRejectKind {
    AbortFailed,
    NoActiveTurn,
    TurnMismatch,
    TurnStarting,
}

#[derive(Clone, Copy, Debug, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum CandidateOutcome {
    ProtocolMismatch,
    ProtocolUnverified,
    StartupFailed,
    TimedOut,
}

#[derive(Debug, Serialize)]
pub struct CandidateReport {
    #[serde(skip_serializing_if = "Option::is_none")]
    pub cli_protocol: Option<u32>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub cli_version: Option<String>,
    pub message: String,
    pub outcome: CandidateOutcome,
    pub path: PathBuf,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub required_protocol: Option<u32>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum ErrorKind {
    CliNotFound,
    CliUnusable,
    EventStreamClosed,
    EventStreamLagged,
    InvalidRequest,
    SessionShutdown,
    SessionStartFailed,
}

#[derive(Clone, Copy, Debug, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum ItemSource {
    Lifecycle,
    Session,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum SendRejectKind {
    CancelPending,
    EmptyTurnId,
    ReuseRejected,
    SendFailed,
    SessionFailed,
    TurnActive,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum TurnOutcome {
    Aborted,
    Completed,
    Failed,
}

#[derive(Debug, Serialize)]
#[serde(rename_all = "snake_case", tag = "type")]
pub enum HelperMessage {
    CancelAcknowledged {
        request_id: String,
        turn_id: String,
    },
    CancelRejected {
        kind: CancelRejectKind,
        message: String,
        request_id: String,
    },
    Error {
        #[serde(skip_serializing_if = "Option::is_none")]
        candidates: Option<Vec<CandidateReport>>,
        kind: ErrorKind,
        message: String,
        #[serde(skip_serializing_if = "Option::is_none")]
        protocol: Option<u32>,
        #[serde(skip_serializing_if = "Option::is_none")]
        request_id: Option<String>,
    },
    Item {
        item: String,
        source: ItemSource,
    },
    Ready {
        cli: CliResolution,
        protocol: u32,
    },
    SendAccepted {
        request_id: String,
        turn_id: String,
    },
    SendRejected {
        kind: SendRejectKind,
        message: String,
        request_id: String,
    },
    TurnEnded {
        outcome: TurnOutcome,
        request_id: String,
        turn_id: String,
    },
}

impl HelperMessage {
    pub fn error(kind: ErrorKind, message: impl Into<String>) -> Self {
        Self::Error {
            candidates: None,
            kind,
            message: message.into(),
            protocol: None,
            request_id: None,
        }
    }

    pub fn startup_error(
        kind: ErrorKind,
        message: impl Into<String>,
        candidates: Option<Vec<CandidateReport>>,
    ) -> Self {
        Self::Error {
            candidates,
            kind,
            message: message.into(),
            protocol: Some(PROTOCOL_VERSION),
            request_id: None,
        }
    }
}

pub fn parse_request(line: &[u8]) -> Result<ControlRequest, HelperMessage> {
    let value: serde_json::Value = serde_json::from_slice(line)
        .map_err(|error| HelperMessage::error(ErrorKind::InvalidRequest, format!("invalid JSON: {error}")))?;
    let request_id = value
        .get("request_id")
        .and_then(serde_json::Value::as_str)
        .map(str::to_owned);
    serde_json::from_value(value).map_err(|error| HelperMessage::Error {
        candidates: None,
        kind: ErrorKind::InvalidRequest,
        message: error.to_string(),
        protocol: None,
        request_id,
    })
}

pub struct Output<W> {
    writer: W,
}

impl<W: AsyncWrite + Unpin> Output<W> {
    pub fn new(writer: W) -> Self {
        Self { writer }
    }

    pub async fn emit(&mut self, message: &HelperMessage) -> std::io::Result<()> {
        let mut line = serde_json::to_string(message)?;
        line.push('\n');
        self.writer.write_all(line.as_bytes()).await?;
        self.writer.flush().await
    }
}
