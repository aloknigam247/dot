use std::path::Path;
use std::sync::Arc;
use std::time::Duration;

use copilot_sdk::session::Session;
use copilot_sdk::session_events::{SessionIdleData, SessionMode};
use copilot_sdk::subscription::{EventSubscription, LifecycleSubscription, RecvError, RecvErrorKind};
use copilot_sdk::{Client, CreateSessionOptions, SessionEvent};
use serde::Serialize;
use serde_json::Value;
use tokio::io::AsyncWrite;
use tokio::sync::mpsc;
use tokio::time::{Instant, timeout, timeout_at};

use crate::protocol::{
    BridgeMessage, CancelRejectKind, ControlRequest, ErrorKind, ItemSource, Output, RequestKind, SendRejectKind,
    TurnOutcome, parse_request,
};

const SEND_RESULT_WAIT: Duration = Duration::from_secs(5);

pub struct SdkSession {
    events: EventSubscription,
    lifecycle: LifecycleSubscription,
    session: Arc<Session>,
}

impl SdkSession {
    pub async fn start(client: &Client, cwd: &Path, limit: Duration) -> Result<Self, String> {
        let lifecycle = client.subscribe_lifecycle();
        let options = CreateSessionOptions::default()
            .with_events(|events| events.with_streaming(true))
            .with_permissions(|permissions| permissions.deny_all_permissions())
            .with_working_directory(cwd);
        let prepared = client.prepare_session(options).map_err(|error| error.to_string())?;
        // Subscribing before creation buffers the events the CLI emits while it creates the session.
        let events = prepared.subscribe();
        match timeout(limit, prepared.start()).await {
            Ok(Ok(session)) => Ok(Self {
                events,
                lifecycle,
                session: Arc::new(session),
            }),
            Ok(Err(error)) => Err(error.to_string()),
            Err(_) => Err(format!(
                "session creation did not finish within {} ms",
                limit.as_millis()
            )),
        }
    }
}

enum Evidence {
    Abort,
    AutopilotStopped { outcome: TurnOutcome, request_id: String },
    Error(Option<String>),
    Idle { aborted: bool },
    MalformedId,
    MalformedIdle,
    Started(String),
}

enum Internal {
    CancelDone {
        request_id: String,
        result: Result<(), String>,
        turn_id: String,
    },
    SendDone {
        request_id: String,
        result: Result<String, String>,
    },
}

enum TurnState {
    Ended {
        boundary_proven: bool,
        outcome: TurnOutcome,
    },
    Running,
}

struct ActiveTurn {
    abort_seen: bool,
    error_seen: bool,
    pending: Vec<Evidence>,
    request_id: String,
    sdk_id: Option<String>,
    started: bool,
    state: TurnState,
}

impl ActiveTurn {
    fn new(request_id: String) -> Self {
        Self {
            abort_seen: false,
            error_seen: false,
            pending: Vec::new(),
            request_id,
            sdk_id: None,
            started: false,
            state: TurnState::Running,
        }
    }

    fn fail(&mut self) {
        self.pending.clear();
        self.state = TurnState::Ended {
            boundary_proven: false,
            outcome: TurnOutcome::Failed,
        };
    }

    fn observe(&mut self, evidence: Evidence) {
        if matches!(self.state, TurnState::Ended { .. }) {
            return;
        }
        let Some(sdk_id) = self.sdk_id.as_deref() else {
            self.pending.push(evidence);
            return;
        };
        match evidence {
            Evidence::Abort => self.abort_seen |= self.started,
            Evidence::AutopilotStopped { outcome, request_id } if request_id == sdk_id => {
                // Autopilot stop is not session-wide idle, so trailing work may still follow it.
                self.state = TurnState::Ended {
                    boundary_proven: false,
                    outcome: if self.abort_seen {
                        TurnOutcome::Aborted
                    } else if self.error_seen && outcome == TurnOutcome::Completed {
                        TurnOutcome::Failed
                    } else {
                        outcome
                    },
                };
            }
            // The SDK stops waiting at the error; ending at the turn's own end keeps its boundary provable.
            Evidence::Error(id) if id.as_deref().is_none_or(|id| id == sdk_id) => {
                self.error_seen = true;
                self.started |= id.is_some();
            }
            Evidence::MalformedId => self.fail(),
            Evidence::Idle { aborted } if self.started || self.error_seen => {
                let outcome = if aborted || self.abort_seen {
                    TurnOutcome::Aborted
                } else if self.error_seen {
                    TurnOutcome::Failed
                } else {
                    TurnOutcome::Completed
                };
                self.state = TurnState::Ended {
                    boundary_proven: self.started,
                    outcome,
                };
            }
            Evidence::MalformedIdle if self.started || self.error_seen => self.fail(),
            Evidence::Started(id) if id == sdk_id => self.started = true,
            _ => {}
        }
    }
}

// The SDK handle is generic so turn bookkeeping can be driven without a CLI.
pub struct SessionController<W, S> {
    active_turn: Option<ActiveTurn>,
    internal_rx: mpsc::UnboundedReceiver<Internal>,
    internal_tx: mpsc::UnboundedSender<Internal>,
    out: Output<W>,
    pending_cancels: Vec<String>,
    reuse_safe: bool,
    sdk: S,
    session_failed: bool,
}

impl<W: AsyncWrite + Unpin> SessionController<W, SdkSession> {
    pub async fn run_session(mut self, mut requests: mpsc::Receiver<std::io::Result<Vec<u8>>>) -> std::io::Result<()> {
        let mut events_open = true;
        let mut lifecycle_open = true;
        loop {
            // Unbiased selection keeps a sustained event stream from starving a queued control.
            tokio::select! {
                Some(internal) = self.internal_rx.recv() => self.handle_internal(internal).await?,
                event = self.sdk.events.recv(), if events_open => match event {
                    Ok(event) => self.handle_event(event).await?,
                    Err(error) => events_open = self.handle_recv_error(&error, "session").await?,
                },
                event = self.sdk.lifecycle.recv(), if lifecycle_open => match event {
                    Ok(event) => self.emit_item(ItemSource::Lifecycle, &event).await?,
                    Err(error) => lifecycle_open = self.handle_recv_error(&error, "lifecycle").await?,
                },
                line = requests.recv() => {
                    if !self.handle_line(line).await? {
                        return Ok(());
                    }
                }
            }
        }
    }

    async fn handle_line(&mut self, line: Option<std::io::Result<Vec<u8>>>) -> std::io::Result<bool> {
        match line {
            Some(Ok(line)) if line.trim_ascii().is_empty() => return Ok(true),
            Some(Ok(line)) => match parse_request(&line) {
                Ok(request) => {
                    if self.handle_request(request).await? {
                        return Ok(true);
                    }
                }
                Err(message) => {
                    self.out.emit(&message).await?;
                    return Ok(true);
                }
            },
            Some(Err(error)) => {
                self.finish_active_turn().await?;
                return Err(error);
            }
            None => {}
        }
        self.finish_active_turn().await?;
        Ok(false)
    }

    async fn handle_request(&mut self, request: ControlRequest) -> std::io::Result<bool> {
        match request.kind {
            RequestKind::Cancel { turn_id } => self.handle_cancel(request.request_id, turn_id).await?,
            RequestKind::Send { prompt } => self.handle_send(request.request_id, prompt).await?,
            RequestKind::Shutdown => return Ok(false),
        }
        Ok(true)
    }

    async fn handle_send(&mut self, request_id: String, prompt: String) -> std::io::Result<()> {
        let rejection = if self.session_failed {
            Some((SendRejectKind::SessionFailed, "the session failed; restart the bridge"))
        } else if !self.pending_cancels.is_empty() {
            // A turn can end on its own before its abort runs, and that abort would then cancel the next turn.
            Some((SendRejectKind::CancelPending, "a cancellation is still in progress"))
        } else if self.active_turn.is_some() {
            Some((SendRejectKind::TurnActive, "another turn is still active"))
        } else if !self.reuse_safe {
            Some((
                SendRejectKind::ReuseRejected,
                "the previous turn boundary is unproven; restart the bridge",
            ))
        } else {
            None
        };
        if let Some((kind, message)) = rejection {
            return self
                .out
                .emit(&BridgeMessage::SendRejected {
                    kind,
                    message: message.to_owned(),
                    request_id,
                })
                .await;
        }
        self.active_turn = Some(ActiveTurn::new(request_id.clone()));
        let internal_tx = self.internal_tx.clone();
        let session = Arc::clone(&self.sdk.session);
        tokio::spawn(async move {
            let result = session.send(prompt).await.map_err(|error| error.to_string());
            let _ = internal_tx.send(Internal::SendDone { request_id, result });
        });
        Ok(())
    }

    async fn handle_cancel(&mut self, request_id: String, turn_id: String) -> std::io::Result<()> {
        let rejection = match &self.active_turn {
            None => Some((CancelRejectKind::NoActiveTurn, "no turn is active")),
            Some(turn) => match turn.sdk_id.as_deref() {
                None => Some((
                    CancelRejectKind::TurnStarting,
                    "the active turn has not been accepted yet",
                )),
                Some(sdk_id) if sdk_id != turn_id => Some((
                    CancelRejectKind::TurnMismatch,
                    "the turn ID does not match the active turn",
                )),
                Some(_) => None,
            },
        };
        if let Some((kind, message)) = rejection {
            return self
                .out
                .emit(&BridgeMessage::CancelRejected {
                    kind,
                    message: message.to_owned(),
                    request_id,
                })
                .await;
        }
        let internal_tx = self.internal_tx.clone();
        let session = Arc::clone(&self.sdk.session);
        self.pending_cancels.push(request_id.clone());
        tokio::spawn(async move {
            let result = session.abort().await.map_err(|error| error.to_string());
            let _ = internal_tx.send(Internal::CancelDone {
                request_id,
                result,
                turn_id,
            });
        });
        Ok(())
    }
}

impl<W: AsyncWrite + Unpin, S> SessionController<W, S> {
    pub fn new(sdk: S, out: Output<W>) -> Self {
        let (internal_tx, internal_rx) = mpsc::unbounded_channel();
        Self {
            active_turn: None,
            internal_rx,
            internal_tx,
            out,
            pending_cancels: Vec::new(),
            reuse_safe: true,
            sdk,
            session_failed: false,
        }
    }

    // Every send and cancel gets exactly one reply and every accepted turn exactly one turn_ended, even on exit.
    async fn finish_active_turn(&mut self) -> std::io::Result<()> {
        let deadline = Instant::now() + SEND_RESULT_WAIT;
        if let Some(turn) = self.active_turn.as_mut() {
            turn.fail();
        }
        while self.active_turn.as_ref().is_some_and(|turn| turn.sdk_id.is_none()) || !self.pending_cancels.is_empty() {
            match timeout_at(deadline, self.internal_rx.recv()).await {
                Ok(Some(internal)) => self.handle_internal(internal).await?,
                Ok(None) | Err(_) => break,
            }
        }
        if let Some(turn) = self.active_turn.take_if(|turn| turn.sdk_id.is_none()) {
            self.out
                .emit(&BridgeMessage::SendRejected {
                    kind: SendRejectKind::SendFailed,
                    message: "the bridge stopped before the send was acknowledged".to_owned(),
                    request_id: turn.request_id,
                })
                .await?;
        }
        for request_id in std::mem::take(&mut self.pending_cancels) {
            self.out
                .emit(&BridgeMessage::CancelRejected {
                    kind: CancelRejectKind::AbortFailed,
                    message: "the bridge stopped before the abort finished".to_owned(),
                    request_id,
                })
                .await?;
        }
        self.emit_turn_ended().await
    }

    async fn handle_internal(&mut self, internal: Internal) -> std::io::Result<()> {
        match internal {
            Internal::CancelDone {
                request_id,
                result,
                turn_id,
            } => {
                if let Some(index) = self.pending_cancels.iter().position(|pending| *pending == request_id) {
                    self.pending_cancels.remove(index);
                }
                let message = match result {
                    Ok(()) => BridgeMessage::CancelAcknowledged { request_id, turn_id },
                    Err(message) => BridgeMessage::CancelRejected {
                        kind: CancelRejectKind::AbortFailed,
                        message,
                        request_id,
                    },
                };
                self.out.emit(&message).await
            }
            Internal::SendDone { request_id, result } => self.handle_send_done(request_id, result).await,
        }
    }

    async fn handle_send_done(&mut self, request_id: String, result: Result<String, String>) -> std::io::Result<()> {
        let (kind, message) = match result {
            Ok(sdk_id) if sdk_id.is_empty() => (
                SendRejectKind::EmptyTurnId,
                "the SDK returned an empty turn ID".to_owned(),
            ),
            Ok(sdk_id) => {
                let Some(turn) = self.active_turn.as_mut() else {
                    return Ok(());
                };
                let pending = std::mem::take(&mut turn.pending);
                turn.sdk_id = Some(sdk_id.clone());
                for evidence in pending {
                    turn.observe(evidence);
                }
                self.out
                    .emit(&BridgeMessage::SendAccepted {
                        request_id,
                        turn_id: sdk_id,
                    })
                    .await?;
                return self.emit_turn_ended().await;
            }
            Err(message) => (SendRejectKind::SendFailed, message),
        };
        // Without a correlated turn ID, later terminal evidence cannot be attributed to this or a later turn.
        self.active_turn = None;
        self.reuse_safe = false;
        self.out
            .emit(&BridgeMessage::SendRejected {
                kind,
                message,
                request_id,
            })
            .await
    }

    async fn handle_event(&mut self, event: SessionEvent) -> std::io::Result<()> {
        self.emit_item(ItemSource::Session, &event).await?;
        if event.event_type == "session.shutdown" {
            let shutdown_type = event
                .data
                .get("shutdownType")
                .and_then(Value::as_str)
                .unwrap_or("unknown");
            return self
                .fail(
                    ErrorKind::SessionShutdown,
                    format!("the session shut down ({shutdown_type})"),
                )
                .await;
        }
        if let (Some(turn), Some(evidence)) = (self.active_turn.as_mut(), evidence(&event)) {
            turn.observe(evidence);
            return self.emit_turn_ended().await;
        }
        Ok(())
    }

    async fn handle_recv_error(&mut self, error: &RecvError, source: &str) -> std::io::Result<bool> {
        let (kind, open) = match error.kind() {
            RecvErrorKind::Lagged(_) => (ErrorKind::EventStreamLagged, true),
            _ => (ErrorKind::EventStreamClosed, false),
        };
        self.fail(kind, format!("{source} event stream: {error}")).await?;
        Ok(open)
    }

    // An incomplete or closed event stream can never prove success, so the active turn fails.
    async fn fail(&mut self, kind: ErrorKind, message: String) -> std::io::Result<()> {
        if self.session_failed {
            return Ok(());
        }
        self.session_failed = true;
        self.out.emit(&BridgeMessage::error(kind, message)).await?;
        if let Some(turn) = self.active_turn.as_mut() {
            turn.fail();
        }
        self.emit_turn_ended().await
    }

    // turn_ended is held until the send acknowledgement supplies the SDK turn ID.
    async fn emit_turn_ended(&mut self) -> std::io::Result<()> {
        let Some(turn) = self.active_turn.as_ref() else {
            return Ok(());
        };
        let Some(sdk_id) = turn.sdk_id.clone() else {
            return Ok(());
        };
        let outcome = match turn.state {
            TurnState::Ended {
                boundary_proven,
                outcome,
            } => {
                self.reuse_safe &= boundary_proven;
                outcome
            }
            TurnState::Running => return Ok(()),
        };
        let request_id = turn.request_id.clone();
        self.active_turn = None;
        self.out
            .emit(&BridgeMessage::TurnEnded {
                outcome,
                request_id,
                turn_id: sdk_id,
            })
            .await
    }

    async fn emit_item(&mut self, source: ItemSource, item: &impl Serialize) -> std::io::Result<()> {
        let item = serde_json::to_string(item)?;
        self.out.emit(&BridgeMessage::Item { item, source }).await
    }
}

// Mirrors the SDK's own send-and-wait correlation: only root-agent events mark a turn's start, failure, and end.
fn evidence(event: &SessionEvent) -> Option<Evidence> {
    if !event.agent_id.as_deref().is_none_or(str::is_empty) {
        return None;
    }
    let text = |key: &str| event.data.get(key).and_then(Value::as_str).map(str::to_owned);
    match event.event_type.as_str() {
        "abort" => Some(Evidence::Abort),
        "assistant.message" => text("originatingMessageId").map(Evidence::Started),
        "session.autopilot_stopped" => {
            let outcome = match text("reason").as_deref() {
                Some("aborted") => TurnOutcome::Aborted,
                Some("task_complete") => TurnOutcome::Completed,
                _ => TurnOutcome::Failed,
            };
            match event.data.get("requestId") {
                None => None,
                Some(Value::String(id)) if !id.is_empty() => Some(Evidence::AutopilotStopped {
                    outcome,
                    request_id: id.clone(),
                }),
                _ => Some(Evidence::MalformedId),
            }
        }
        "session.error" => match event.data.get("originatingMessageId") {
            None => Some(Evidence::Error(None)),
            Some(Value::String(id)) if !id.is_empty() => Some(Evidence::Error(Some(id.clone()))),
            _ => Some(Evidence::MalformedId),
        },
        "session.idle" => match event.typed_data::<SessionIdleData>() {
            Some(data) => (data.mode != Some(SessionMode::Autopilot)).then(|| Evidence::Idle {
                aborted: data.aborted == Some(true),
            }),
            None => Some(Evidence::MalformedIdle),
        },
        "session.warning" if text("warningType").as_deref() == Some("policy_blocked") => {
            text("originatingMessageId").map(Evidence::Started)
        }
        "user.message" => text("messageId").map(Evidence::Started),
        _ => None,
    }
}

#[cfg(test)]
mod tests;
