mod common;

use std::fs::OpenOptions;
use std::io::{BufRead, BufReader, Write};
use std::path::PathBuf;
use std::thread;
use std::time::Duration;

use serde_json::{Value, json};

use common::*;

const BURST_EVENTS: usize = 3000;

fn main() {
    let log = PathBuf::from(std::env::var_os("DOT_FAKE_CLI_LOG").expect("fake CLI log path"));
    let exe = std::env::current_exe().unwrap();
    let scenario = std::fs::read_to_string(scenario_path(&exe)).unwrap_or_default();
    let mut fake = Fake {
        connected: false,
        event_count: 0,
        log,
        message_count: 0,
        pending_abort: None,
        scenario: scenario.trim().to_owned(),
        session_id: String::new(),
        stdout: std::io::stdout(),
        waiting_for_abort: false,
        waiting_for_permission: false,
    };
    fake.record(json!({
        "args": std::env::args().skip(1).collect::<Vec<_>>(),
        "event": "start",
        "exe": exe,
        "pid": std::process::id(),
        "telemetry_disabled": std::env::var("COPILOT_TELEMETRY_DISABLED").ok(),
    }));
    if fake.scenario == "exit" {
        fake.exit();
    }
    let mut stdin = BufReader::new(std::io::stdin());
    while let Some(message) = read_frame(&mut stdin) {
        fake.handle(&message);
    }
    // Only the version probe skips connect, so this outlives the probe unless the helper stops its process tree.
    if fake.scenario == "protocol3_linger" && !fake.connected {
        thread::sleep(LINGER);
    }
    fake.exit();
}

fn read_frame(reader: &mut impl BufRead) -> Option<Value> {
    let mut length = None;
    loop {
        let mut header = String::new();
        if reader.read_line(&mut header).ok()? == 0 {
            return None;
        }
        let header = header.trim_end();
        if header.is_empty() {
            break;
        }
        if let Some(value) = header.strip_prefix("Content-Length:") {
            length = value.trim().parse::<usize>().ok();
        }
    }
    let mut body = vec![0; length?];
    reader.read_exact(&mut body).ok()?;
    serde_json::from_slice(&body).ok()
}

struct Fake {
    connected: bool,
    event_count: usize,
    log: PathBuf,
    message_count: usize,
    pending_abort: Option<Value>,
    scenario: String,
    session_id: String,
    stdout: std::io::Stdout,
    waiting_for_abort: bool,
    waiting_for_permission: bool,
}

impl Fake {
    fn record(&self, entry: Value) {
        let mut file = OpenOptions::new().append(true).create(true).open(&self.log).unwrap();
        file.write_all(format!("{entry}\n").as_bytes()).unwrap();
    }

    fn exit(&self) -> ! {
        self.record(json!({"event": "exit", "pid": std::process::id()}));
        std::process::exit(0);
    }

    fn write(&mut self, message: &Value) {
        let body = message.to_string();
        let mut stdout = self.stdout.lock();
        write!(stdout, "Content-Length: {}\r\n\r\n{body}", body.len()).unwrap();
        stdout.flush().unwrap();
    }

    fn reply(&mut self, id: &Value, result: Value) {
        self.write(&json!({"jsonrpc": "2.0", "id": id, "result": result}));
    }

    fn reply_error(&mut self, id: &Value, code: i64, message: &str) {
        self.write(&json!({"jsonrpc": "2.0", "id": id, "error": {"code": code, "message": message}}));
    }

    fn event(&mut self, kind: &str, data: Value) {
        self.agent_event(None, kind, data);
    }

    fn agent_event(&mut self, agent_id: Option<&str>, kind: &str, data: Value) {
        self.event_count += 1;
        let mut event = json!({
            "data": data,
            "id": format!("evt-{}", self.event_count),
            "parentId": null,
            "timestamp": "2026-01-01T00:00:00.000Z",
            "type": kind,
        });
        if let Some(agent_id) = agent_id {
            event["agentId"] = json!(agent_id);
        }
        self.record(json!({"event": "sent", "item": event, "source": "session"}));
        let params = json!({"event": event, "sessionId": self.session_id});
        self.write(&json!({"jsonrpc": "2.0", "method": "session.event", "params": params}));
    }

    fn lifecycle(&mut self, kind: &str) {
        let params = json!({"sessionId": self.session_id, "type": kind});
        self.record(json!({"event": "sent", "item": params, "source": "lifecycle"}));
        self.write(&json!({"jsonrpc": "2.0", "method": "session.lifecycle", "params": params}));
    }

    fn delta(&mut self, content: &str) {
        self.event(
            "assistant.message_delta",
            json!({"deltaContent": content, "messageId": "reply"}),
        );
    }

    fn idle(&mut self) {
        self.event("session.idle", json!({}));
    }

    fn session_error(&mut self, originating_message_id: Option<&str>) {
        let mut data = json!({"errorType": "query", "message": "request failed"});
        if let Some(originating_message_id) = originating_message_id {
            data["originatingMessageId"] = json!(originating_message_id);
        }
        self.event("session.error", data);
    }

    fn wait_for_release(&self) {
        let release = release_path(&self.log);
        while !release.exists() {
            thread::sleep(Duration::from_millis(10));
        }
    }

    fn user(&mut self, message_id: &str) {
        self.event("user.message", json!({"content": "prompt", "messageId": message_id}));
    }

    fn complete(&mut self, message_id: &str) {
        self.user(message_id);
        self.delta("Hello");
        self.delta(", world");
        self.event(
            "assistant.message",
            json!({"content": "Hello, world", "messageId": "reply", "originatingMessageId": message_id}),
        );
        self.idle();
    }

    fn handle(&mut self, message: &Value) {
        let Some(method) = message["method"].as_str() else {
            return;
        };
        let id = &message["id"];
        let params = &message["params"];
        self.record(json!({"event": "rpc", "method": method, "params": params, "pid": std::process::id()}));
        self.connected |= method == "connect";
        let hung = self
            .scenario
            .strip_prefix("hang:")
            .is_some_and(|methods| methods.split(',').any(|hung| hung == method));
        if id.is_null() || self.scenario == "hang" || hung {
            return;
        }
        match method {
            "auth.getStatus" if self.scenario == "auth_error" => self.reply_error(id, -32603, "auth unavailable"),
            "auth.getStatus" if self.scenario == "signed_out" => self.reply(id, json!({"isAuthenticated": false})),
            "auth.getStatus" => self.reply(id, json!({"isAuthenticated": true})),
            "connect" if self.scenario == "no_protocol" => self.reply_error(id, -32601, "method not found"),
            "connect" if self.scenario.starts_with("protocol3") => {
                self.reply(id, json!({"ok": true, "protocolVersion": 3, "version": LEGACY_VERSION}))
            }
            "connect" => self.reply(id, json!({"ok": true, "protocolVersion": 4, "version": VERSION})),
            "ping" => self.reply(id, json!({"message": "pong", "timestamp": "1970-01-01T00:00:00Z"})),
            "session.abort" if self.scenario == "abort_error" => self.reply_error(id, -32000, "abort failed"),
            "session.abort" if self.scenario == "two_aborts" => {
                if let Some(first) = self.pending_abort.take() {
                    self.idle();
                    self.reply(&first, json!({}));
                    self.wait_for_release();
                    self.reply(id, json!({}));
                } else {
                    self.pending_abort = Some(id.clone());
                }
            }
            "session.abort" if self.scenario == "idle_before_abort" => {
                if std::mem::take(&mut self.waiting_for_abort) {
                    self.idle();
                }
                self.wait_for_release();
                self.reply(id, json!({}));
            }
            "session.abort" => {
                self.reply(id, json!({}));
                if std::mem::take(&mut self.waiting_for_abort) {
                    self.wait_for_release();
                    self.event("abort", json!({"reason": "user initiated"}));
                    self.event("session.idle", json!({"aborted": true}));
                }
            }
            "session.create" if self.scenario == "create_error" => self.reply_error(id, -32000, "create failed"),
            "session.create" => {
                self.session_id = params["sessionId"].as_str().unwrap_or("fake-session").to_owned();
                self.lifecycle("session.created");
                self.event("session.start", json!({"sessionId": self.session_id}));
                self.event(
                    "session.info",
                    json!({"infoType": "notification", "message": "fake ready"}),
                );
                let session_id = self.session_id.clone();
                self.reply(id, json!({"sessionId": session_id, "workspacePath": "workspace"}));
            }
            "session.permissions.handlePendingPermissionRequest" => {
                self.reply(id, json!({}));
                if std::mem::take(&mut self.waiting_for_permission) {
                    self.event("permission.completed", json!({"requestId": "perm-1"}));
                    self.idle();
                }
            }
            "session.send" => self.send(id, params["prompt"].as_str().unwrap_or_default()),
            "status.get" if self.scenario == "protocol3_silent" => {}
            "status.get" if self.scenario.starts_with("protocol3") => {
                self.reply(id, json!({"protocolVersion": 3, "version": LEGACY_VERSION}))
            }
            "status.get" => self.reply(id, json!({"protocolVersion": 4, "version": VERSION})),
            _ => self.reply(id, json!({})),
        }
    }

    fn send(&mut self, id: &Value, prompt: &str) {
        self.message_count += 1;
        let message_id = format!("msg-{}", self.message_count);
        let accepted = json!({"messageId": message_id});
        match prompt {
            "autopilot" => {
                self.reply(id, accepted);
                self.user(&message_id);
                self.delta("working");
                self.event(
                    "session.autopilot_stopped",
                    json!({"reason": "task_complete", "requestId": message_id}),
                );
            }
            "close" => {
                self.reply(id, accepted);
                self.user(&message_id);
                self.delta("partial");
                self.exit();
            }
            "complete_stray" => {
                self.reply(id, accepted);
                self.complete(&message_id);
                self.idle();
            }
            "early_terminal" => {
                self.complete(&message_id);
                self.reply(id, accepted);
            }
            "empty_id" => {
                self.reply(id, json!({"messageId": ""}));
                self.wait_for_release();
                self.complete(&message_id);
            }
            "fatal_shutdown" => {
                self.reply(id, accepted);
                self.user(&message_id);
                self.event(
                    "session.shutdown",
                    json!({"errorReason": "crash", "shutdownType": "error"}),
                );
            }
            "hold" => {
                self.reply(id, accepted);
                self.user(&message_id);
                self.delta("thinking");
                self.waiting_for_abort = true;
            }
            "control_backlog" => {
                self.reply(id, accepted);
                self.user(&message_id);
                self.wait_for_release();
                for _ in 0..192 {
                    self.delta(&"x".repeat(1024));
                }
                self.delta("last");
                self.record(json!({"event": "backlog_ready"}));
            }
            "lifecycle_lag" => {
                self.reply(id, accepted);
                self.user(&message_id);
                for _ in 0..BURST_EVENTS {
                    self.lifecycle("session.updated");
                }
                self.record(json!({"event": "lifecycle_burst_done"}));
            }
            "buffered_idle" => {
                self.complete(&message_id);
                self.wait_for_release();
                self.reply(id, accepted);
            }
            "lag" => {
                self.reply(id, accepted);
                self.user(&message_id);
                let content = "x".repeat(1000);
                for _ in 0..BURST_EVENTS {
                    self.delta(&content);
                }
                self.idle();
                self.record(json!({"event": "burst_done"}));
            }
            "permission" => {
                self.reply(id, accepted);
                self.user(&message_id);
                self.event(
                    "permission.requested",
                    json!({"permissionRequest": {"kind": "shell"}, "requestId": "perm-1"}),
                );
                self.waiting_for_permission = true;
            }
            "send_error" => self.reply_error(id, -32000, "send failed"),
            "session_error" => {
                self.reply(id, accepted);
                self.user(&message_id);
                self.session_error(None);
                self.delta("recovered");
                self.idle();
            }
            "session_error_before_ack" => {
                self.user(&message_id);
                self.session_error(Some(&message_id));
                self.idle();
                self.wait_for_release();
                self.reply(id, accepted);
            }
            "session_error_uncorrelated" => {
                self.reply(id, accepted);
                self.session_error(None);
                self.idle();
            }
            "shutdown_before_ack" => {
                self.user(&message_id);
                self.event(
                    "session.shutdown",
                    json!({"errorReason": "crash", "shutdownType": "error"}),
                );
                self.wait_for_release();
                self.reply(id, accepted);
            }
            "slow_ack" => {
                self.wait_for_release();
                self.reply(id, accepted);
                self.complete(&message_id);
            }
            "slow_hold" => {
                self.wait_for_release();
                self.reply(id, accepted);
            }
            "sub_agent" => {
                self.reply(id, accepted);
                self.user(&message_id);
                self.agent_event(
                    Some("agent-1"),
                    "assistant.message",
                    json!({"content": "sub", "messageId": "sub-reply", "originatingMessageId": message_id}),
                );
                self.agent_event(Some("agent-1"), "session.idle", json!({}));
                self.delta("root");
                self.idle();
            }
            "trailing" => {
                self.idle();
                self.reply(id, accepted);
                self.idle();
                self.complete(&message_id);
            }
            "uncorrelated_start" => {
                self.reply(id, accepted);
                self.event("user.message", json!({"content": "prompt"}));
                self.idle();
            }
            _ => {
                self.reply(id, accepted);
                self.complete(&message_id);
            }
        }
    }
}
