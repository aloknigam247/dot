use std::ffi::OsString;
use std::fs::OpenOptions;
use std::io::{BufRead, BufReader, Read, Write};
use std::os::windows::ffi::OsStringExt;
use std::os::windows::io::{AsRawHandle, FromRawHandle, OwnedHandle};
use std::path::{Path, PathBuf};
use std::process::{Child, ChildStdin, Command, Stdio};
use std::ptr;
use std::sync::OnceLock;
use std::sync::mpsc::{Receiver, RecvTimeoutError, sync_channel};
use std::thread;
use std::time::{Duration, Instant};

use copilot_sdk::{SessionEvent, SessionLifecycleEvent};
use serde_json::{Value, json};
use tempfile::TempDir;
use windows_sys::Win32::Foundation::INVALID_HANDLE_VALUE;
use windows_sys::Win32::Storage::FileSystem::PIPE_ACCESS_INBOUND;
use windows_sys::Win32::System::Pipes::{CreateNamedPipeW, PIPE_READMODE_MESSAGE, PIPE_TYPE_MESSAGE, PIPE_WAIT};
use windows_sys::Win32::System::Threading::{
    OpenProcess, PROCESS_QUERY_LIMITED_INFORMATION, PROCESS_TERMINATE, QueryFullProcessImageNameW, TerminateProcess,
};

#[path = "fixtures/common.rs"]
mod fake;

const FAKE_LOG_VARIABLE: &str = "DOT_FAKE_CLI_LOG";
const READ_TIMEOUT: Duration = Duration::from_secs(20);
const STARTUP_TIMEOUT_VARIABLE: &str = "DOT_COPILOT_HELPER_STARTUP_TIMEOUT_MS";
// SDK event forwarding

#[test]
fn items_match_sdk_serialization() {
    let fixture = Fixture::new();
    let mut helper = fixture.helper();
    helper.expect_ready();
    helper.send("t1", "complete");
    let turn = helper.until_turn_ended("t1");
    let (code, records) = helper.shutdown();
    assert_eq!(code, 0);
    assert_eq!(items(&records, "session"), expected_items(&fixture.log(), "session"));
    assert_eq!(
        items(&records, "lifecycle"),
        expected_items(&fixture.log(), "lifecycle")
    );
    let types = item_types(&turn);
    let last_delta = types
        .iter()
        .rposition(|kind| kind == "assistant.message_delta")
        .unwrap();
    let idle = types.iter().position(|kind| kind == "session.idle").unwrap();
    assert!(last_delta < idle, "deltas must precede terminal evidence: {types:?}");
    assert_eq!(turn.last().unwrap()["outcome"], "completed");
}

// Startup and terminal ordering

#[test]
fn startup_items_follow_ready() {
    let fixture = Fixture::new();
    let mut helper = fixture.helper();
    let ready = helper.next();
    assert_eq!(ready["type"], "ready");
    assert_eq!(ready["protocol"], 1);
    assert_eq!(
        ready["cli"],
        json!({
            "auth": "signed_in",
            "path": fixture.bin.join("copilot.exe"),
            "protocol": 4,
            "version": fake::VERSION,
        })
    );
    // Shutdown competes with buffered events, so it is sent only once the startup items have been forwarded.
    for _ in 0..3 {
        assert_eq!(helper.next()["type"], "item");
    }
    let (code, records) = helper.shutdown();
    assert_eq!(code, 0);
    assert_eq!(records.iter().filter(|record| record["type"] == "ready").count(), 1);
    assert!(records[1..].iter().all(|record| record["type"] == "item"));
    let session = items(&records, "session");
    assert_eq!(session, expected_items(&fixture.log(), "session"));
    assert_eq!(session.len(), 2);
    assert_eq!(
        items(&records, "lifecycle"),
        expected_items(&fixture.log(), "lifecycle")
    );
    assert_eq!(items(&records, "lifecycle").len(), 1);
}

#[test]
fn terminal_before_send_ack_holds_turn_ended() {
    let fixture = Fixture::new();
    let mut helper = fixture.helper();
    helper.expect_ready();
    helper.send("t1", "buffered_idle");
    helper.until(|record| item_type(record).as_deref() == Some("session.idle"));
    assert!(
        !helper
            .records
            .iter()
            .any(|record| matches!(record["type"].as_str(), Some("send_accepted" | "turn_ended")))
    );
    fixture.release();
    helper.until_turn_ended("t1");
    let (code, records) = helper.shutdown();
    assert_eq!(code, 0);
    assert_eq!(items(&records, "session"), expected_items(&fixture.log(), "session"));
    let accepted = position(&records, |record| record["type"] == "send_accepted");
    let idle = position(&records, |record| item_type(record).as_deref() == Some("session.idle"));
    let ended = position(&records, |record| record["type"] == "turn_ended");
    assert!(idle < accepted && accepted < ended, "{records:?}");
    assert_eq!(
        turn_ended(&records),
        vec![json!({"type": "turn_ended", "outcome": "completed", "request_id": "t1", "turn_id": "msg-1"})]
    );
}

#[test]
fn rejected_send_emits_no_turn_ended() {
    let fixture = Fixture::new();
    let mut helper = fixture.helper();
    helper.expect_ready();
    helper.send("t1", "send_error");
    let rejected = helper.until(|record| record["type"] == "send_rejected");
    assert_eq!(rejected.last().unwrap()["kind"], "send_failed");
    assert_eq!(rejected.last().unwrap()["request_id"], "t1");
    helper.send("t2", "complete");
    let rejected = helper.until(|record| record["type"] == "send_rejected");
    assert_eq!(rejected.last().unwrap()["kind"], "reuse_rejected");
    assert_eq!(rejected.last().unwrap()["request_id"], "t2");
    let (code, records) = helper.shutdown();
    assert_eq!(code, 0);
    assert!(turn_ended(&records).is_empty(), "{records:?}");
    assert_eq!(rpc_count(&fixture.log(), "session.send"), 1);
}

// Send and cancel correlation

#[test]
fn cancel_correlation() {
    let fixture = Fixture::new();
    let mut helper = fixture.helper();
    helper.expect_ready();
    helper.cancel("c0", "msg-1");
    assert_rejection(&helper.until_type("cancel_rejected"), "c0", "no_active_turn");
    helper.send("t1", "hold");
    let accepted = helper.until_type("send_accepted");
    assert_eq!(accepted["turn_id"], "msg-1");
    helper.cancel("c1", "msg-other");
    assert_rejection(&helper.until_type("cancel_rejected"), "c1", "turn_mismatch");
    helper.send("t2", "complete");
    assert_rejection(&helper.until_type("send_rejected"), "t2", "turn_active");
    assert_eq!(rpc_count(&fixture.log(), "session.abort"), 0);
    helper.cancel("c2", "msg-1");
    assert_eq!(
        helper.until_type("cancel_acknowledged"),
        json!({"type": "cancel_acknowledged", "request_id": "c2", "turn_id": "msg-1"})
    );
    // The fake holds the abort's terminal events, so the acknowledgement alone must leave the turn active.
    helper.send("t3", "complete");
    let rejected = helper.until(|record| record["type"] == "send_rejected");
    assert!(turn_ended(&rejected).is_empty(), "{rejected:?}");
    assert_rejection(rejected.last().unwrap(), "t3", "turn_active");
    fixture.release();
    let records = helper.until_turn_ended("t1");
    assert_eq!(records.last().unwrap()["outcome"], "aborted");
    helper.cancel("c3", "msg-1");
    assert_rejection(&helper.until_type("cancel_rejected"), "c3", "no_active_turn");
    let (code, records) = helper.shutdown();
    assert_eq!(code, 0);
    let acknowledged: Vec<&Value> = records
        .iter()
        .filter(|record| record["type"] == "cancel_acknowledged")
        .collect();
    assert_eq!(
        acknowledged,
        vec![&json!({"type": "cancel_acknowledged", "request_id": "c2", "turn_id": "msg-1"})]
    );
    assert_eq!(
        turn_ended(&records),
        vec![json!({"type": "turn_ended", "outcome": "aborted", "request_id": "t1", "turn_id": "msg-1"})]
    );
    assert_eq!(rpc_count(&fixture.log(), "session.abort"), 1);
    assert_eq!(rpc_count(&fixture.log(), "session.send"), 1);
}

#[test]
fn cancel_during_send_startup_is_rejected() {
    let fixture = Fixture::new();
    let mut helper = fixture.helper();
    helper.expect_ready();
    helper.send("t1", "slow_ack");
    fixture.wait_for_log(|entry| entry["event"] == "rpc" && entry["method"] == "session.send");
    helper.cancel("c1", "msg-1");
    assert_rejection(&helper.until_type("cancel_rejected"), "c1", "turn_starting");
    fixture.release();
    let records = helper.until_turn_ended("t1");
    assert_eq!(records.last().unwrap()["outcome"], "completed");
    let (code, _) = helper.shutdown();
    assert_eq!(code, 0);
    assert_eq!(rpc_count(&fixture.log(), "session.abort"), 0);
}

#[test]
fn cancel_pending_rejects_send() {
    let fixture = Fixture::with_scenario("idle_before_abort");
    let mut helper = fixture.helper();
    helper.expect_ready();
    helper.send("t1", "hold");
    helper.until_type("send_accepted");
    helper.cancel("c1", "msg-1");
    // The fake ends the turn on its own and then holds the abort reply.
    helper.until_turn_ended("t1");
    helper.send("t2", "complete");
    helper.until_type("send_rejected");
    fixture.release();
    helper.until_type("cancel_acknowledged");
    helper.send("t3", "complete");
    helper.until_turn_ended("t3");
    let (code, records) = helper.shutdown();
    assert_eq!(code, 0);
    assert_eq!(
        replies(&records),
        vec![
            json!({"type": "send_accepted", "request_id": "t1", "turn_id": "msg-1"}),
            json!({"type": "turn_ended", "outcome": "completed", "request_id": "t1", "turn_id": "msg-1"}),
            json!({
                "type": "send_rejected",
                "kind": "cancel_pending",
                "message": "a cancellation is still in progress",
                "request_id": "t2",
            }),
            json!({"type": "cancel_acknowledged", "request_id": "c1", "turn_id": "msg-1"}),
            json!({"type": "send_accepted", "request_id": "t3", "turn_id": "msg-2"}),
            json!({"type": "turn_ended", "outcome": "completed", "request_id": "t3", "turn_id": "msg-2"}),
        ]
    );
    assert_eq!(rpc_count(&fixture.log(), "session.send"), 2);
}

#[test]
fn all_cancels_must_finish_before_reuse() {
    let fixture = Fixture::with_scenario("two_aborts");
    let mut helper = fixture.helper();
    helper.expect_ready();
    helper.send("t1", "hold");
    helper.until_type("send_accepted");
    helper.cancel("c1", "msg-1");
    fixture.wait_for_log(|entry| entry["method"] == "session.abort");
    helper.cancel("c2", "msg-1");
    while !helper.records.iter().any(|r| r["type"] == "turn_ended")
        || !helper.records.iter().any(|r| r["type"] == "cancel_acknowledged")
    {
        helper.next();
    }
    helper.send("blocked", "complete");
    assert_rejection(&helper.until_type("send_rejected"), "blocked", "cancel_pending");
    fixture.release();
    assert_eq!(helper.until_type("cancel_acknowledged")["request_id"], "c2");
    helper.send("t2", "complete");
    assert_eq!(helper.until_turn_ended("t2").last().unwrap()["outcome"], "completed");
    assert_eq!(helper.shutdown().0, 0);
    assert_eq!(rpc_count(&fixture.log(), "session.send"), 2);
}

#[test]
fn queued_control_is_served_before_sdk_backlog_drains() {
    let fixture = Fixture::new();
    let mut helper = fixture.helper();
    helper.expect_ready();
    helper.send("t1", "control_backlog");
    helper.until_type("send_accepted");
    fixture.release();
    fixture.wait_for_log(|entry| entry["event"] == "backlog_ready");
    helper.cancel("c1", "wrong-id");
    assert_rejection(&helper.until_type("cancel_rejected"), "c1", "turn_mismatch");
    helper.until(|r| item(r).is_some_and(|e| e["data"]["deltaContent"] == "last"));
    assert_eq!(helper.shutdown().0, 0);
}

#[test]
fn unattributed_early_failure_does_not_allow_reuse() {
    let fixture = Fixture::new();
    let mut helper = fixture.helper();
    helper.expect_ready();
    helper.send("t1", "session_error_uncorrelated");
    assert_eq!(helper.until_turn_ended("t1").last().unwrap()["outcome"], "failed");
    helper.send("t2", "hold");
    assert_rejection(&helper.until_type("send_rejected"), "t2", "reuse_rejected");
    assert_eq!(helper.shutdown().0, 0);
    assert_eq!(rpc_count(&fixture.log(), "session.send"), 1);
}

#[test]
fn empty_turn_id_is_rejected() {
    let fixture = Fixture::new();
    let mut helper = fixture.helper();
    helper.expect_ready();
    helper.send("t1", "empty_id");
    assert_rejection(&helper.until_type("send_rejected"), "t1", "empty_turn_id");
    fixture.release();
    helper.until(|record| item_type(record).as_deref() == Some("session.idle"));
    helper.send("t2", "complete");
    assert_rejection(&helper.until_type("send_rejected"), "t2", "reuse_rejected");
    let (code, records) = helper.shutdown();
    assert_eq!(code, 0);
    assert!(turn_ended(&records).is_empty(), "{records:?}");
    assert_eq!(items(&records, "session"), expected_items(&fixture.log(), "session"));
}

// Turn attribution and errors

#[test]
fn trailing_idle_never_ends_next_turn() {
    let fixture = Fixture::new();
    let mut helper = fixture.helper();
    helper.expect_ready();
    helper.send("a", "complete_stray");
    helper.until_turn_ended("a");
    helper.send("b", "trailing");
    let turn_b = helper.until_turn_ended("b");
    let (code, records) = helper.shutdown();
    assert_eq!(code, 0);
    let user_b = position(&turn_b, |record| {
        item(record).is_some_and(|event| event["type"] == "user.message" && event["data"]["messageId"] == "msg-2")
    });
    let last_idle = turn_b
        .iter()
        .rposition(|record| item_type(record).as_deref() == Some("session.idle"))
        .unwrap();
    assert!(user_b < last_idle && last_idle < turn_b.len() - 1, "{turn_b:?}");
    assert_eq!(
        turn_ended(&records),
        vec![
            json!({"type": "turn_ended", "outcome": "completed", "request_id": "a", "turn_id": "msg-1"}),
            json!({"type": "turn_ended", "outcome": "completed", "request_id": "b", "turn_id": "msg-2"}),
        ]
    );
    let idles = records
        .iter()
        .filter(|record| item_type(record).as_deref() == Some("session.idle"))
        .count();
    assert_eq!(idles, 5);
}

#[test]
fn uncorrelated_start_waits_for_shutdown() {
    let fixture = Fixture::new();
    let mut helper = fixture.helper();
    helper.expect_ready();
    helper.send("t1", "uncorrelated_start");
    let turn = helper.until(|record| item_type(record).as_deref() == Some("session.idle"));
    let types = item_types(&turn);
    assert_eq!(types[types.len() - 2..], ["user.message", "session.idle"]);
    assert!(turn_ended(&turn).is_empty(), "{turn:?}");
    let (code, records) = helper.shutdown();
    assert_eq!(code, 0);
    assert_eq!(
        replies(&records),
        vec![
            json!({"type": "send_accepted", "request_id": "t1", "turn_id": "msg-1"}),
            json!({"type": "turn_ended", "outcome": "failed", "request_id": "t1", "turn_id": "msg-1"}),
        ]
    );
    assert_eq!(records.last().unwrap()["type"], "turn_ended");
    fixture.assert_children_exited();
}

#[test]
fn session_error_fails_turn_at_idle() {
    let fixture = Fixture::new();
    let mut helper = fixture.helper();
    helper.expect_ready();
    helper.send("t1", "session_error");
    let turn = helper.until_turn_ended("t1");
    helper.send("t2", "complete");
    helper.until_turn_ended("t2");
    let (code, records) = helper.shutdown();
    assert_eq!(code, 0);
    let error = position(&turn, |record| item_type(record).as_deref() == Some("session.error"));
    let delta = turn
        .iter()
        .rposition(|record| item_type(record).as_deref() == Some("assistant.message_delta"));
    assert!(delta.is_some_and(|delta| error < delta), "{turn:?}");
    assert_eq!(item_types(&turn).last().map(String::as_str), Some("session.idle"));
    assert_eq!(
        turn_ended(&records),
        vec![
            json!({"type": "turn_ended", "outcome": "failed", "request_id": "t1", "turn_id": "msg-1"}),
            json!({"type": "turn_ended", "outcome": "completed", "request_id": "t2", "turn_id": "msg-2"}),
        ]
    );
    assert!(records.iter().all(|record| record["type"] != "error"));
}

#[test]
fn session_error_before_send_ack_fails_turn() {
    let fixture = Fixture::new();
    let mut helper = fixture.helper();
    helper.expect_ready();
    helper.send("t1", "session_error_before_ack");
    helper.until(|record| item_type(record).as_deref() == Some("session.idle"));
    fixture.release();
    helper.until_turn_ended("t1");
    helper.send("t2", "complete");
    helper.until_turn_ended("t2");
    let (code, records) = helper.shutdown();
    assert_eq!(code, 0);
    assert_eq!(
        replies(&records),
        vec![
            json!({"type": "send_accepted", "request_id": "t1", "turn_id": "msg-1"}),
            json!({"type": "turn_ended", "outcome": "failed", "request_id": "t1", "turn_id": "msg-1"}),
            json!({"type": "send_accepted", "request_id": "t2", "turn_id": "msg-2"}),
            json!({"type": "turn_ended", "outcome": "completed", "request_id": "t2", "turn_id": "msg-2"}),
        ]
    );
}

#[test]
fn lagged_stream_fails_turn() {
    let fixture = Fixture::new();
    let mut helper = fixture.helper();
    helper.expect_ready();
    helper.send("t1", "lag");
    // The helper blocks on its full stdout pipe while the burst overflows the SDK's bounded event buffer.
    fixture.wait_for_log(|entry| entry["event"] == "burst_done");
    let records = helper.until(|record| record["type"] == "turn_ended");
    assert_eq!(records.last().unwrap()["outcome"], "failed");
    let errors: Vec<&Value> = records.iter().filter(|record| record["type"] == "error").collect();
    assert_eq!(errors.len(), 1);
    assert_eq!(errors[0]["kind"], "event_stream_lagged");
    // A lag drops only the oldest buffered events, so newer ones are still forwarded.
    helper.until(|record| item_type(record).as_deref() == Some("session.idle"));
    helper.send("t2", "complete");
    assert_rejection(&helper.until_type("send_rejected"), "t2", "session_failed");
    let (code, records) = helper.shutdown();
    assert_eq!(code, 0);
    assert_eq!(turn_ended(&records).len(), 1);
}

#[test]
fn lagged_lifecycle_stream_fails_turn() {
    let fixture = Fixture::new();
    let mut helper = fixture.helper();
    helper.expect_ready();
    helper.send("t1", "lifecycle_lag");
    fixture.wait_for_log(|entry| entry["event"] == "lifecycle_burst_done");
    let records = helper.until(|record| record["type"] == "turn_ended");
    assert_eq!(records.last().unwrap()["outcome"], "failed");
    let errors: Vec<&Value> = records.iter().filter(|record| record["type"] == "error").collect();
    assert_eq!(errors.len(), 1);
    assert_eq!(errors[0]["kind"], "event_stream_lagged");
    assert!(
        errors[0]["message"].as_str().unwrap().contains("lifecycle"),
        "{errors:?}"
    );
    helper.send("t2", "complete");
    assert_rejection(&helper.until_type("send_rejected"), "t2", "session_failed");
    assert_eq!(helper.shutdown().0, 0);
}

#[test]
fn closed_stream_fails_turn() {
    let fixture = Fixture::new();
    let mut helper = fixture.helper();
    helper.expect_ready();
    helper.send("t1", "close");
    let records = helper.until(|record| record["type"] == "turn_ended");
    assert_eq!(records.last().unwrap()["outcome"], "failed");
    let (code, records) = helper.shutdown();
    assert_eq!(code, 0);
    let errors: Vec<&Value> = records.iter().filter(|record| record["type"] == "error").collect();
    assert_eq!(errors.len(), 1);
    assert_eq!(errors[0]["kind"], "event_stream_closed");
    assert_eq!(turn_ended(&records).len(), 1);
}

#[test]
fn fatal_shutdown_fails_turn() {
    let fixture = Fixture::new();
    let mut helper = fixture.helper();
    helper.expect_ready();
    helper.send("t1", "fatal_shutdown");
    let records = helper.until(|record| record["type"] == "turn_ended");
    assert_eq!(records.last().unwrap()["outcome"], "failed");
    let errors: Vec<&Value> = records.iter().filter(|record| record["type"] == "error").collect();
    assert_eq!(errors.len(), 1);
    assert_eq!(errors[0]["kind"], "session_shutdown");
    helper.send("t2", "complete");
    assert_rejection(&helper.until_type("send_rejected"), "t2", "session_failed");
    let (code, records) = helper.shutdown();
    assert_eq!(code, 0);
    assert_eq!(turn_ended(&records).len(), 1);
}

#[test]
fn shutdown_before_send_ack_fails_turn() {
    let fixture = Fixture::new();
    let mut helper = fixture.helper();
    helper.expect_ready();
    helper.send("t1", "shutdown_before_ack");
    helper.until_type("error");
    fixture.release();
    helper.until_turn_ended("t1");
    helper.send("t2", "complete");
    helper.until_type("send_rejected");
    let (code, records) = helper.shutdown();
    assert_eq!(code, 0);
    assert_eq!(
        replies(&records),
        vec![
            json!({"type": "error", "kind": "session_shutdown", "message": "the session shut down (error)"}),
            json!({"type": "send_accepted", "request_id": "t1", "turn_id": "msg-1"}),
            json!({"type": "turn_ended", "outcome": "failed", "request_id": "t1", "turn_id": "msg-1"}),
            json!({
                "type": "send_rejected",
                "kind": "session_failed",
                "message": "the session failed; restart the helper",
                "request_id": "t2",
            }),
        ]
    );
    assert_eq!(rpc_count(&fixture.log(), "session.send"), 1);
}

#[test]
fn unproven_boundary_rejects_next_send() {
    let fixture = Fixture::new();
    let mut helper = fixture.helper();
    helper.expect_ready();
    helper.send("t1", "autopilot");
    let turn = helper.until_turn_ended("t1");
    assert_eq!(turn.last().unwrap()["outcome"], "completed");
    helper.send("t2", "complete");
    assert_rejection(&helper.until_type("send_rejected"), "t2", "reuse_rejected");
    let (code, records) = helper.shutdown();
    assert_eq!(code, 0);
    assert_eq!(turn_ended(&records).len(), 1);
    assert_eq!(rpc_count(&fixture.log(), "session.send"), 1);
}

#[test]
fn failed_abort_is_rejected() {
    let fixture = Fixture::with_scenario("abort_error");
    let mut helper = fixture.helper();
    helper.expect_ready();
    helper.send("t1", "hold");
    helper.until_type("send_accepted");
    helper.cancel("c1", "msg-1");
    let before_shutdown = helper.until(|record| record["type"] == "cancel_rejected");
    assert_rejection(before_shutdown.last().unwrap(), "c1", "abort_failed");
    helper.send("t2", "complete");
    assert_rejection(&helper.until_type("send_rejected"), "t2", "turn_active");
    let (code, records) = helper.shutdown();
    assert_eq!(code, 0);
    assert!(records.iter().all(|record| record["type"] != "cancel_acknowledged"));
    assert_eq!(
        records
            .iter()
            .filter(|record| record["type"] == "cancel_rejected")
            .count(),
        1
    );
    // The only turn_ended comes from shutdown, so the failed abort did not end the turn.
    assert_eq!(
        turn_ended(&records),
        vec![json!({"type": "turn_ended", "outcome": "failed", "request_id": "t1", "turn_id": "msg-1"})]
    );
    assert_eq!(records.last().unwrap()["type"], "turn_ended");
    assert_eq!(rpc_count(&fixture.log(), "session.abort"), 1);
}

#[test]
fn sub_agent_events_do_not_end_turn() {
    let fixture = Fixture::new();
    let mut helper = fixture.helper();
    helper.expect_ready();
    helper.send("t1", "sub_agent");
    let turn = helper.until_turn_ended("t1");
    let (code, records) = helper.shutdown();
    assert_eq!(code, 0);
    let events: Vec<(Value, Value)> = turn
        .iter()
        .filter_map(item)
        .map(|event| (event["type"].clone(), event["agentId"].clone()))
        .collect();
    assert_eq!(
        events[events.len() - 4..],
        [
            (json!("assistant.message"), json!("agent-1")),
            (json!("session.idle"), json!("agent-1")),
            (json!("assistant.message_delta"), Value::Null),
            (json!("session.idle"), Value::Null),
        ]
    );
    assert_eq!(
        turn_ended(&records),
        vec![json!({"type": "turn_ended", "outcome": "completed", "request_id": "t1", "turn_id": "msg-1"})]
    );
}

// Permission, auth, and shutdown behavior

#[test]
fn permission_request_is_denied() {
    let fixture = Fixture::new();
    let mut helper = fixture.helper();
    helper.expect_ready();
    helper.send("t1", "permission");
    let turn = helper.until_turn_ended("t1");
    assert_eq!(turn.last().unwrap()["outcome"], "completed");
    let (code, _) = helper.shutdown();
    assert_eq!(code, 0);
    let log = fixture.log();
    let create = rpc_params(&log, "session.create");
    assert_eq!(create[0]["permissions"]["request"], true);
    assert_eq!(create[0]["events"]["streaming"], true);
    assert_eq!(
        Path::new(create[0]["workingDirectory"].as_str().unwrap()),
        fixture.root.path()
    );
    let answers = rpc_params(&log, "session.permissions.handlePendingPermissionRequest");
    assert_eq!(answers.len(), 1);
    assert_eq!(answers[0]["requestId"], "perm-1");
    assert_eq!(answers[0]["result"]["kind"], "reject");
    let starts: Vec<&Value> = log.iter().filter(|entry| entry["event"] == "start").collect();
    assert_eq!(starts.len(), 1);
    assert_eq!(starts[0]["telemetry_disabled"], "true");
}

#[test]
fn auth_status_is_reported() {
    let cases = [
        ("auth_error", "unknown", Some("auth unavailable")),
        ("signed_out", "signed_out", None),
    ];
    for (scenario, auth, auth_error) in cases {
        let fixture = Fixture::with_scenario(scenario);
        let mut helper = fixture.helper();
        let ready = helper.next();
        assert_eq!(ready["type"], "ready", "{scenario}");
        assert_eq!(ready["cli"]["auth"], auth, "{scenario}");
        match auth_error {
            Some(expected) => assert!(
                ready["cli"]["auth_error"]
                    .as_str()
                    .is_some_and(|error| error.contains(expected)),
                "{ready}"
            ),
            None => assert!(ready["cli"].get("auth_error").is_none(), "{ready}"),
        }
        let (code, _) = helper.shutdown();
        assert_eq!(code, 0, "{scenario}");
    }
}

#[test]
fn shutdown_request_stops_child() {
    let fixture = Fixture::new();
    let mut helper = fixture.helper();
    helper.expect_ready();
    let (code, _) = helper.shutdown();
    assert_eq!(code, 0);
    fixture.assert_children_exited();
    assert_eq!(rpc_count(&fixture.log(), "runtime.shutdown"), 1);
}

#[test]
fn closed_stdin_stops_child() {
    let fixture = Fixture::new();
    let mut helper = fixture.helper();
    helper.expect_ready();
    let (code, _) = helper.finish();
    assert_eq!(code, 0);
    fixture.assert_children_exited();
}

#[test]
fn stdout_failure_at_ready_stops_child() {
    let fixture = Fixture::new();
    let stdout = fixture.root.path().join("stdout.txt");
    std::fs::write(&stdout, "").unwrap();
    // A read-only handle makes writing the ready record fail after the CLI session has started.
    let output = fixture
        .command(&[&fixture.bin], Some(".EXE"), None)
        .stderr(Stdio::piped())
        .stdin(Stdio::null())
        .stdout(std::fs::File::open(&stdout).unwrap())
        .output()
        .unwrap();
    assert_eq!(output.status.code(), Some(1));
    let stderr = String::from_utf8_lossy(&output.stderr);
    assert!(stderr.starts_with("dot_copilot_helper: "), "{stderr}");
    assert_eq!(rpc_count(&fixture.log(), "session.create"), 1);
    fixture.assert_children_exited();
}

#[test]
fn stdout_failure_during_turn_stops_child() {
    let fixture = Fixture::new();
    let mut command = fixture.command(&[&fixture.bin], Some(".EXE"), None);
    command.stderr(Stdio::piped()).stdin(Stdio::piped());
    let mut child = command.spawn().unwrap();
    let mut stdin = child.stdin.take().unwrap();
    let mut stderr = child.stderr.take().unwrap();
    let stdout = child.stdout.take().unwrap();
    let (closed_tx, closed) = sync_channel(1);
    // Closing stdout once the held turn is quiet makes the helper's next record fail to write.
    thread::spawn(move || {
        let mut accepted = false;
        let mut started = false;
        for line in BufReader::new(stdout).lines() {
            let record: Value = serde_json::from_str(&line.unwrap()).unwrap();
            accepted |= record["type"] == "send_accepted";
            started |= item_type(&record).as_deref() == Some("assistant.message_delta");
            if accepted && started {
                break;
            }
        }
        closed_tx.send(accepted && started).unwrap();
    });
    writeln!(
        stdin,
        "{}",
        json!({"type": "send", "request_id": "t1", "prompt": "hold"})
    )
    .unwrap();
    assert_eq!(closed.recv_timeout(READ_TIMEOUT), Ok(true));
    writeln!(stdin, "not json").unwrap();
    let deadline = Instant::now() + READ_TIMEOUT;
    let status = loop {
        if let Some(status) = child.try_wait().unwrap() {
            break status;
        }
        assert!(Instant::now() < deadline, "helper did not exit");
        thread::sleep(Duration::from_millis(20));
    };
    assert_eq!(status.code(), Some(1));
    let mut message = String::new();
    stderr.read_to_string(&mut message).unwrap();
    assert!(message.starts_with("dot_copilot_helper: "), "{message}");
    assert_eq!(rpc_count(&fixture.log(), "runtime.shutdown"), 1);
    fixture.assert_children_exited();
}

#[test]
fn stdin_read_error_fails_turn() {
    let fixture = Fixture::new();
    let (mut client, server) = fixture.message_pipe();
    let mut command = fixture.command(&[&fixture.bin], Some(".EXE"), None);
    command.stderr(Stdio::piped()).stdin(server);
    let mut helper = Helper::spawn(command);
    let mut stderr = helper.child.stderr.take().unwrap();
    helper.expect_ready();
    let send = json!({"type": "send", "request_id": "t1", "prompt": "hold"});
    client.write_all(format!("{send}\n").as_bytes()).unwrap();
    helper.until_type("send_accepted");
    // One message larger than the helper's stdin buffer fails its read with ERROR_MORE_DATA instead of reaching EOF.
    client.write_all(&[b' '; 16 * 1024]).unwrap();
    let (code, records) = helper.finish();
    assert_eq!(code, 1);
    assert_eq!(
        replies(&records),
        vec![
            json!({"type": "send_accepted", "request_id": "t1", "turn_id": "msg-1"}),
            json!({"type": "turn_ended", "outcome": "failed", "request_id": "t1", "turn_id": "msg-1"}),
        ]
    );
    assert_eq!(records.last().unwrap()["type"], "turn_ended");
    fixture.assert_children_exited();
    let mut text = String::new();
    stderr.read_to_string(&mut text).unwrap();
    assert!(text.starts_with("dot_copilot_helper: "), "{text}");
}

// Discovery behavior

#[test]
fn discovery_selects_first_successful_handshake() {
    let fixture = Fixture::empty();
    let first = fixture.dir("first");
    let second = fixture.dir("second");
    let third = fixture.dir("third");
    fixture.install(&first, "copilot.com", "protocol3");
    fixture.install(&first, "copilot.txt", "ok");
    fixture.install(&second, "copilot.exe", "ok");
    fixture.install(&third, "copilot.exe", "ok");
    let mut helper = fixture.helper_with(&[&first, &second, &third], Some(".COM;.EXE"), None);
    let ready = helper.next();
    assert_eq!(ready["type"], "ready");
    assert_eq!(ready["cli"]["path"], json!(second.join("copilot.exe")));
    let (code, _) = helper.shutdown();
    assert_eq!(code, 0);
    assert_eq!(
        started_exes(&fixture.log()),
        vec![first.join("copilot.com"), second.join("copilot.exe")]
    );
}

#[test]
fn protocol_mismatch_reports_version() {
    let fixture = Fixture::with_scenario("protocol3");
    let helper = fixture.helper();
    let (code, records) = helper.finish();
    assert_eq!(code, 1);
    assert_eq!(records.len(), 1);
    let error = &records[0];
    assert_eq!(error["type"], "error");
    assert_eq!(error["kind"], "cli_unusable");
    assert_eq!(error["protocol"], 1);
    let candidates = error["candidates"].as_array().unwrap();
    assert_eq!(candidates.len(), 1);
    assert_eq!(candidates[0]["path"], json!(fixture.bin.join("copilot.exe")));
    assert_eq!(candidates[0]["outcome"], "protocol_mismatch");
    assert_eq!(candidates[0]["cli_protocol"], 3);
    assert_eq!(candidates[0]["cli_version"], fake::LEGACY_VERSION);
    assert_eq!(candidates[0]["required_protocol"], 4);
    let log = fixture.log();
    assert_eq!(rpc_count(&log, "session.create"), 0);
    assert_eq!(rpc_count(&log, "session.send"), 0);
    fixture.assert_children_exited();
}

#[test]
fn protocol_mismatch_omits_unanswered_version() {
    let fixture = Fixture::with_scenario("protocol3_silent");
    let helper = fixture.helper_with(&[&fixture.bin], Some(".EXE"), Some(2000));
    let (code, records) = helper.finish();
    assert_eq!(code, 1);
    assert_eq!(records.len(), 1);
    let candidates = records[0]["candidates"].as_array().unwrap();
    assert_eq!(candidates.len(), 1);
    let candidate = &candidates[0];
    assert_eq!(candidate["outcome"], "protocol_mismatch");
    assert_eq!(candidate["cli_protocol"], 3);
    assert_eq!(candidate["required_protocol"], 4);
    assert!(candidate.get("cli_version").is_none(), "{candidate}");
    assert_eq!(rpc_count(&fixture.log(), "status.get"), 1);
    fixture.assert_children_exited();
}

#[test]
fn cmd_wrapper_mismatch_stops_lingering_cli() {
    let fixture = Fixture::empty();
    fixture.install_cmd(&fixture.bin, "protocol3_linger");
    let mut helper = fixture.helper_with(&[&fixture.bin], Some(".CMD"), None);
    let error = helper.next();
    assert_eq!(error["kind"], "cli_unusable");
    let candidates = error["candidates"].as_array().unwrap();
    assert_eq!(candidates.len(), 1);
    assert_eq!(candidates[0]["path"], json!(fixture.bin.join("copilot.cmd")));
    assert_eq!(candidates[0]["outcome"], "protocol_mismatch");
    assert_eq!(candidates[0]["cli_version"], fake::LEGACY_VERSION);
    let log = fixture.log();
    let probe = log
        .iter()
        .find(|entry| {
            entry["event"] == "start"
                && !log
                    .iter()
                    .any(|rpc| rpc["pid"] == entry["pid"] && rpc["method"] == "connect")
        })
        .expect("raw version probe did not start a CLI process");
    assert_eq!(probe["args"], json!(["--server", "--stdio", "--no-auto-update"]));
    assert_eq!(probe["telemetry_disabled"], "true");
    // Both waits together stay below LINGER, so a probe CLI left running fails here rather than exiting on its own.
    assert_eq!(helper.wait(), 1);
    fixture.assert_children_exit_within(fake::LINGER / 3);
    assert_eq!(rpc_count(&fixture.log(), "status.get"), 1);
    let (_, records) = helper.finish();
    assert_eq!(records.len(), 1);
}

#[test]
fn cmd_wrapper_runs_turn() {
    let fixture = Fixture::empty();
    fixture.install_cmd(&fixture.bin, "ok");
    let mut helper = fixture.helper_with(&[&fixture.bin], Some(".CMD"), None);
    let ready = helper.next();
    assert_eq!(ready["type"], "ready");
    assert_eq!(ready["cli"]["path"], json!(fixture.bin.join("copilot.cmd")));
    helper.send("t1", "complete");
    assert_eq!(helper.until_turn_ended("t1").last().unwrap()["outcome"], "completed");
    let (code, _) = helper.shutdown();
    assert_eq!(code, 0);
    assert_eq!(started_exes(&fixture.log()), vec![fixture.bin.join("fake_cli.exe")]);
    fixture.assert_children_exited();
}

#[test]
fn missing_cli_reports_not_found() {
    let fixture = Fixture::empty();
    let first = fixture.dir("first");
    let second = fixture.dir("second");
    fixture.install(&first, "copilot.txt", "ok");
    let helper = fixture.helper_with(&[&first, &second], Some(".COM;.EXE"), None);
    let (code, records) = helper.finish();
    assert_eq!(code, 1);
    assert_eq!(records.len(), 1);
    assert_eq!(records[0]["type"], "error");
    assert_eq!(records[0]["kind"], "cli_not_found");
    assert_eq!(records[0]["protocol"], 1);
    assert_eq!(records[0]["candidates"], json!([]));
    assert!(fixture.log().is_empty());
}

#[test]
fn startup_failures_report_candidates() {
    let fixture = Fixture::empty();
    let first = fixture.dir("first");
    let second = fixture.dir("second");
    let third = fixture.dir("third");
    fixture.install(&first, "copilot.exe", "exit");
    fixture.install(&second, "copilot.exe", "no_protocol");
    fixture.install(&third, "copilot.exe", "hang");
    let helper = fixture.helper_with(&[&first, &first, &second, &third], Some(".EXE"), Some(2000));
    let (code, records) = helper.finish();
    assert_eq!(code, 1);
    assert_eq!(records.len(), 1);
    assert_eq!(records[0]["kind"], "cli_unusable");
    assert_eq!(records[0]["protocol"], 1);
    let candidates = records[0]["candidates"].as_array().unwrap();
    let outcomes: Vec<(&Value, &Value)> = candidates
        .iter()
        .map(|candidate| (&candidate["path"], &candidate["outcome"]))
        .collect();
    assert_eq!(
        outcomes,
        vec![
            (&json!(first.join("copilot.exe")), &json!("startup_failed")),
            (&json!(second.join("copilot.exe")), &json!("protocol_unverified")),
            (&json!(third.join("copilot.exe")), &json!("timed_out")),
        ],
        "{candidates:#?}"
    );
    assert_eq!(candidates[2]["message"], "no protocol handshake within 2000 ms");
    let exes = [&first, &second, &third].map(|dir| dir.join("copilot.exe"));
    assert_eq!(started_exes(&fixture.log()), exes);
    fixture.assert_children_exited();
}

#[test]
fn cli_status_timeout_reports_unknown() {
    let fixture = Fixture::with_scenario("hang:auth.getStatus,status.get");
    let mut helper = fixture.helper_with(&[&fixture.bin], Some(".EXE"), Some(2000));
    let ready = helper.next();
    assert_eq!(ready["type"], "ready", "{ready}");
    assert_eq!(ready["cli"]["auth"], "unknown");
    assert_eq!(ready["cli"]["auth_error"], "no auth status within 2000 ms");
    assert_eq!(ready["cli"].get("version"), Some(&Value::Null));
    let (code, _) = helper.shutdown();
    assert_eq!(code, 0);
    let log = fixture.log();
    assert_eq!(rpc_count(&log, "auth.getStatus"), 1);
    assert_eq!(rpc_count(&log, "status.get"), 1);
    fixture.assert_children_exited();
}

#[test]
fn discovery_skips_relative_path_entries() {
    let fixture = Fixture::empty();
    let relative = fixture.dir("relative");
    let absolute = fixture.dir("absolute");
    fixture.install(&relative, "copilot.exe", "ok");
    fixture.install(&absolute, "copilot.exe", "ok");
    // Without PATHEXT, only the default executable extensions can find copilot.exe.
    let mut helper = fixture.helper_with(&[Path::new("relative"), &absolute], None, None);
    assert_eq!(helper.next()["cli"]["path"], json!(absolute.join("copilot.exe")));
    let (code, _) = helper.shutdown();
    assert_eq!(code, 0);
    let started: Vec<Value> = fixture
        .log()
        .iter()
        .filter(|entry| entry["event"] == "start")
        .map(|entry| entry["exe"].clone())
        .collect();
    assert_eq!(started, vec![json!(absolute.join("copilot.exe"))]);
}

#[test]
fn discovery_ignores_malformed_pathext_entries() {
    let fixture = Fixture::empty();
    let first = fixture.dir("first");
    let second = fixture.dir("second");
    fixture.install(&first, "copilot", "ok");
    fixture.install(&first, "copilotexe", "ok");
    fixture.install(&second, "copilot.exe", "ok");
    let mut helper = fixture.helper_with(&[&first, &second], Some(";.EXE;;EXE;."), None);
    let ready = helper.next();
    assert_eq!(ready["cli"]["path"], json!(second.join("copilot.exe")), "{ready}");
    let (code, _) = helper.shutdown();
    assert_eq!(code, 0);
    assert_eq!(started_exes(&fixture.log()), vec![second.join("copilot.exe")]);
}

#[test]
fn session_create_failure_reports_start_failed() {
    let cases = [
        ("create_error", "create failed"),
        ("hang:session.create", "session creation did not finish within 2000 ms"),
    ];
    for (scenario, message) in cases {
        let fixture = Fixture::with_scenario(scenario);
        let helper = fixture.helper_with(&[&fixture.bin], Some(".EXE"), Some(2000));
        let (code, records) = helper.finish();
        assert_eq!(code, 1, "{scenario}");
        assert_eq!(records.len(), 1, "{records:?}");
        assert_eq!(records[0]["type"], "error");
        assert_eq!(records[0]["kind"], "session_start_failed");
        assert!(records[0]["message"].as_str().unwrap().contains(message), "{records:?}");
        assert_eq!(records[0]["protocol"], 1);
        fixture.assert_children_exited();
    }
}

// Request parsing and exit during a turn

#[test]
fn invalid_requests_are_reported() {
    let fixture = Fixture::new();
    let mut helper = fixture.helper();
    helper.expect_ready();
    helper.write_raw(b"\r\n");
    helper.write_raw(b"{not json\n");
    helper.write_raw(b"{\"type\": \"pause\", \"request_id\": \"p1\"}\n");
    helper.write_raw(b"\xff\xfe\n");
    helper.send("t1", "complete");
    let turn = helper.until_turn_ended("t1");
    let (code, records) = helper.shutdown();
    assert_eq!(code, 0);
    let errors: Vec<(&Value, Option<&Value>)> = records
        .iter()
        .filter(|record| record["type"] == "error")
        .map(|record| (&record["kind"], record.get("request_id")))
        .collect();
    let invalid = json!("invalid_request");
    assert_eq!(
        errors,
        vec![(&invalid, None), (&invalid, Some(&json!("p1"))), (&invalid, None)]
    );
    assert_eq!(turn.last().unwrap()["outcome"], "completed");
}

#[test]
fn shutdown_finishes_active_turn() {
    let fixture = Fixture::new();
    let mut helper = fixture.helper();
    helper.expect_ready();
    helper.send("t1", "hold");
    helper.until_type("send_accepted");
    let (code, records) = helper.shutdown();
    assert_eq!(code, 0);
    assert_eq!(
        replies(&records),
        vec![
            json!({"type": "send_accepted", "request_id": "t1", "turn_id": "msg-1"}),
            json!({"type": "turn_ended", "outcome": "failed", "request_id": "t1", "turn_id": "msg-1"}),
        ]
    );
    assert_eq!(records.last().unwrap()["type"], "turn_ended");
    fixture.assert_children_exited();
}

#[test]
fn shutdown_rejects_unanswered_cancel() {
    let fixture = Fixture::with_scenario("hang:session.abort");
    let mut helper = fixture.helper();
    helper.expect_ready();
    helper.send("t1", "hold");
    helper.until_type("send_accepted");
    helper.cancel("c1", "msg-1");
    fixture.wait_for_log(|entry| entry["event"] == "rpc" && entry["method"] == "session.abort");
    let (code, records) = helper.shutdown();
    assert_eq!(code, 0);
    assert_eq!(
        replies(&records),
        vec![
            json!({"type": "send_accepted", "request_id": "t1", "turn_id": "msg-1"}),
            json!({
                "type": "cancel_rejected",
                "kind": "abort_failed",
                "message": "the helper stopped before the abort finished",
                "request_id": "c1",
            }),
            json!({"type": "turn_ended", "outcome": "failed", "request_id": "t1", "turn_id": "msg-1"}),
        ]
    );
    fixture.assert_children_exited();
}

#[test]
fn closed_stdin_finishes_starting_turn() {
    let fixture = Fixture::new();
    let mut helper = fixture.helper();
    helper.expect_ready();
    helper.send("t1", "slow_hold");
    fixture.wait_for_log(|entry| entry["event"] == "rpc" && entry["method"] == "session.send");
    helper.close_stdin();
    fixture.release();
    let (code, records) = helper.finish();
    assert_eq!(code, 0);
    assert_eq!(
        replies(&records),
        vec![
            json!({"type": "send_accepted", "request_id": "t1", "turn_id": "msg-1"}),
            json!({"type": "turn_ended", "outcome": "failed", "request_id": "t1", "turn_id": "msg-1"}),
        ]
    );
    assert_eq!(records.last().unwrap()["type"], "turn_ended");
    fixture.assert_children_exited();
}

#[test]
fn closed_stdin_rejects_unacknowledged_send() {
    let fixture = Fixture::new();
    let mut helper = fixture.helper();
    helper.expect_ready();
    helper.send("t1", "slow_hold");
    fixture.wait_for_log(|entry| entry["event"] == "rpc" && entry["method"] == "session.send");
    helper.close_stdin();
    assert_rejection(&helper.until_type("send_rejected"), "t1", "send_failed");
    // The fake acknowledges only after the helper gave up, so the late result cannot produce a turn.
    fixture.release();
    let (code, records) = helper.finish();
    assert_eq!(code, 0);
    assert_eq!(replies(&records).len(), 1, "{records:?}");
    assert!(turn_ended(&records).is_empty());
    fixture.assert_children_exited();
}

// Harness

fn fake_executable() -> &'static Path {
    static EXE: OnceLock<PathBuf> = OnceLock::new();
    EXE.get_or_init(|| {
        // A targeted `cargo test --test` does not build examples.
        let output = Command::new(env!("CARGO"))
            .args(["build", "--locked", "--example", "fake_cli", "--message-format=json"])
            .current_dir(env!("CARGO_MANIFEST_DIR"))
            .output()
            .unwrap();
        assert!(output.status.success(), "{}", String::from_utf8_lossy(&output.stderr));
        String::from_utf8(output.stdout)
            .unwrap()
            .lines()
            .map(|line| serde_json::from_str::<Value>(line).unwrap())
            .find_map(|msg| {
                (msg["target"]["name"] == "fake_cli")
                    .then(|| msg["executable"].as_str().map(PathBuf::from))
                    .flatten()
            })
            .expect("cargo did not report the fake CLI executable")
    })
}

struct Fixture {
    bin: PathBuf,
    root: TempDir,
}

impl Fixture {
    fn new() -> Self {
        Self::with_scenario("ok")
    }

    fn with_scenario(scenario: &str) -> Self {
        let fixture = Self::empty();
        fixture.install(&fixture.bin, "copilot.exe", scenario);
        fixture
    }

    fn empty() -> Self {
        let root = tempfile::Builder::new()
            .prefix("helper")
            .tempdir_in(env!("CARGO_TARGET_TMPDIR"))
            .unwrap();
        let bin = root.path().join("bin");
        std::fs::create_dir(&bin).unwrap();
        Self { bin, root }
    }

    fn dir(&self, name: &str) -> PathBuf {
        let dir = self.root.path().join(name);
        std::fs::create_dir(&dir).unwrap();
        dir
    }

    fn install(&self, dir: &Path, name: &str, scenario: &str) {
        let exe = fake_executable();
        let target = dir.join(name);
        if std::fs::hard_link(exe, &target).is_err() {
            std::fs::copy(exe, &target).unwrap();
        }
        std::fs::write(fake::scenario_path(&target), scenario).unwrap();
    }

    fn install_cmd(&self, dir: &Path, scenario: &str) {
        self.install(dir, "fake_cli.exe", scenario);
        std::fs::write(dir.join("copilot.cmd"), "@\"%~dp0fake_cli.exe\" %*\r\n").unwrap();
    }

    fn log_path(&self) -> PathBuf {
        self.root.path().join("fake.jsonl")
    }

    fn log(&self) -> Vec<Value> {
        read_log(&self.log_path())
    }

    fn release(&self) {
        std::fs::write(fake::release_path(&self.log_path()), "").unwrap();
    }

    fn wait_for_log(&self, predicate: impl Fn(&Value) -> bool) {
        let deadline = Instant::now() + READ_TIMEOUT;
        while !self.log().iter().any(&predicate) {
            assert!(Instant::now() < deadline, "fake CLI log never matched");
            thread::sleep(Duration::from_millis(20));
        }
    }

    fn assert_children_exited(&self) {
        self.assert_children_exit_within(READ_TIMEOUT);
    }

    fn assert_children_exit_within(&self, limit: Duration) {
        let deadline = Instant::now() + limit;
        for entry in self.log().iter().filter(|entry| entry["event"] == "start") {
            let pid = entry["pid"].as_u64().unwrap();
            while process_alive(pid) {
                assert!(Instant::now() < deadline, "fake CLI {pid} is still running");
                thread::sleep(Duration::from_millis(50));
            }
        }
    }

    fn helper(&self) -> Helper {
        self.helper_with(&[&self.bin], Some(".EXE"), None)
    }

    fn helper_with(&self, dirs: &[&Path], pathext: Option<&str>, timeout_ms: Option<u64>) -> Helper {
        let mut command = self.command(dirs, pathext, timeout_ms);
        command.stdin(Stdio::piped());
        Helper::spawn(command)
    }

    fn command(&self, dirs: &[&Path], pathext: Option<&str>, timeout_ms: Option<u64>) -> Command {
        let mut command = Command::new(env!("CARGO_BIN_EXE_dot_copilot_helper"));
        command
            .current_dir(self.root.path())
            .env("PATH", std::env::join_paths(dirs).unwrap())
            .env(FAKE_LOG_VARIABLE, self.log_path())
            .env_remove("COPILOT_CLI_PATH")
            .env_remove("COPILOT_TELEMETRY_DISABLED")
            .env_remove(STARTUP_TIMEOUT_VARIABLE)
            .stderr(Stdio::null())
            .stdout(Stdio::piped());
        match pathext {
            Some(pathext) => command.env("PATHEXT", pathext),
            None => command.env_remove("PATHEXT"),
        };
        if let Some(timeout_ms) = timeout_ms {
            command.env(STARTUP_TIMEOUT_VARIABLE, timeout_ms.to_string());
        }
        command
    }

    fn message_pipe(&self) -> (std::fs::File, OwnedHandle) {
        let unique = self.root.path().file_name().unwrap().to_string_lossy().into_owned();
        let name = format!(r"\\.\pipe\dot_helper_{}_{unique}", std::process::id());
        let wide: Vec<u16> = name.encode_utf16().chain([0]).collect();
        let mode = PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT;
        // SAFETY: wide is a live, nul-terminated name, and null security attributes are allowed.
        let handle = unsafe { CreateNamedPipeW(wide.as_ptr(), PIPE_ACCESS_INBOUND, mode, 1, 0, 65536, 0, ptr::null()) };
        assert_ne!(handle, INVALID_HANDLE_VALUE, "{}", std::io::Error::last_os_error());
        // SAFETY: CreateNamedPipeW returned a new handle that nothing else owns.
        let server = unsafe { OwnedHandle::from_raw_handle(handle) };
        // Reading a server end that no client has connected to fails, so the client connects before the helper starts.
        let client = OpenOptions::new().write(true).open(&name).unwrap();
        (client, server)
    }
}

impl Drop for Fixture {
    fn drop(&mut self) {
        if !thread::panicking() {
            return;
        }
        // A failed test can leave a fake CLI running. Matching the full image path on the opened handle keeps a reused
        // PID from terminating a process outside this fixture.
        for entry in self.log().iter().filter(|entry| entry["event"] == "start") {
            let Some(expected) = entry["exe"].as_str().and_then(|exe| Path::new(exe).canonicalize().ok()) else {
                continue;
            };
            let Some(pid) = entry["pid"].as_u64().and_then(|pid| u32::try_from(pid).ok()) else {
                continue;
            };
            // SAFETY: OpenProcess takes no pointers and returns null or a new handle.
            let handle = unsafe { OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE, 0, pid) };
            if handle.is_null() {
                continue;
            }
            // SAFETY: OpenProcess returned a new handle that nothing else owns.
            let process = unsafe { OwnedHandle::from_raw_handle(handle) };
            let mut image = vec![0u16; 32768];
            let mut size = image.len() as u32;
            // SAFETY: image holds size UTF-16 units, and both outlive the call on the live process handle.
            let queried =
                unsafe { QueryFullProcessImageNameW(process.as_raw_handle(), 0, image.as_mut_ptr(), &mut size) } != 0;
            let actual = queried.then(|| PathBuf::from(OsString::from_wide(&image[..size as usize])));
            if actual.and_then(|path| path.canonicalize().ok()) == Some(expected) {
                // SAFETY: the handle is live and was opened with terminate access.
                unsafe { TerminateProcess(process.as_raw_handle(), 1) };
            }
        }
    }
}

struct Helper {
    child: Child,
    lines: Receiver<String>,
    records: Vec<Value>,
    stdin: Option<ChildStdin>,
}

impl Helper {
    fn spawn(mut command: Command) -> Self {
        let mut child = command.spawn().unwrap();
        let stdin = child.stdin.take();
        let stdout = child.stdout.take().unwrap();
        // A rendezvous channel keeps the reader from draining stdout ahead of the test.
        let (tx, lines) = sync_channel(0);
        thread::spawn(move || {
            for line in BufReader::new(stdout).lines() {
                let Ok(line) = line else {
                    break;
                };
                if tx.send(line).is_err() {
                    break;
                }
            }
        });
        Self {
            child,
            lines,
            records: Vec::new(),
            stdin,
        }
    }

    fn next(&mut self) -> Value {
        let line = self.lines.recv_timeout(READ_TIMEOUT).expect("helper output timed out");
        let record: Value = serde_json::from_str(&line).unwrap();
        self.records.push(record.clone());
        record
    }

    fn until(&mut self, predicate: impl Fn(&Value) -> bool) -> Vec<Value> {
        let mut records = Vec::new();
        loop {
            let record = self.next();
            let done = predicate(&record);
            records.push(record);
            if done {
                return records;
            }
        }
    }

    fn until_type(&mut self, kind: &str) -> Value {
        self.until(|record| record["type"] == kind).pop().unwrap()
    }

    fn until_turn_ended(&mut self, request_id: &str) -> Vec<Value> {
        self.until(|record| record["type"] == "turn_ended" && record["request_id"] == request_id)
    }

    fn expect_ready(&mut self) {
        assert_eq!(self.next()["type"], "ready");
    }

    fn request(&mut self, request: Value) {
        let stdin = self.stdin.as_mut().unwrap();
        writeln!(stdin, "{request}").unwrap();
        stdin.flush().unwrap();
    }

    fn send(&mut self, request_id: &str, prompt: &str) {
        self.request(json!({"type": "send", "request_id": request_id, "prompt": prompt}));
    }

    fn write_raw(&mut self, bytes: &[u8]) {
        let stdin = self.stdin.as_mut().unwrap();
        stdin.write_all(bytes).unwrap();
        stdin.flush().unwrap();
    }

    fn close_stdin(&mut self) {
        self.stdin = None;
    }

    fn cancel(&mut self, request_id: &str, turn_id: &str) {
        self.request(json!({"type": "cancel", "request_id": request_id, "turn_id": turn_id}));
    }

    fn shutdown(mut self) -> (i32, Vec<Value>) {
        self.request(json!({"type": "shutdown", "request_id": "stop"}));
        // Stdin stays open until the helper exits, so end of input cannot stand in for the shutdown request.
        self.drain()
    }

    fn finish(mut self) -> (i32, Vec<Value>) {
        self.stdin = None;
        self.drain()
    }

    fn drain(mut self) -> (i32, Vec<Value>) {
        loop {
            match self.lines.recv_timeout(READ_TIMEOUT) {
                Ok(line) => self.records.push(serde_json::from_str(&line).unwrap()),
                Err(RecvTimeoutError::Disconnected) => break,
                Err(RecvTimeoutError::Timeout) => panic!("helper did not close stdout"),
            }
        }
        let code = self.wait();
        (code, std::mem::take(&mut self.records))
    }

    fn wait(&mut self) -> i32 {
        let deadline = Instant::now() + READ_TIMEOUT;
        loop {
            if let Some(status) = self.child.try_wait().unwrap() {
                return status.code().unwrap();
            }
            assert!(Instant::now() < deadline, "helper did not exit");
            thread::sleep(Duration::from_millis(20));
        }
    }
}

impl Drop for Helper {
    fn drop(&mut self) {
        let _ = self.child.kill();
    }
}

fn assert_rejection(record: &Value, request_id: &str, kind: &str) {
    assert_eq!(record["request_id"], request_id, "{record}");
    assert_eq!(record["kind"], kind, "{record}");
    assert!(
        record["message"].as_str().is_some_and(|message| !message.is_empty()),
        "{record}"
    );
}

fn item(record: &Value) -> Option<Value> {
    (record["type"] == "item").then(|| serde_json::from_str(record["item"].as_str().unwrap()).unwrap())
}

fn item_type(record: &Value) -> Option<String> {
    item(record).and_then(|event| event["type"].as_str().map(str::to_owned))
}

fn item_types(records: &[Value]) -> Vec<String> {
    records.iter().filter_map(item_type).collect()
}

fn items(records: &[Value], source: &str) -> Vec<String> {
    records
        .iter()
        .filter(|record| record["type"] == "item" && record["source"] == source)
        .map(|record| record["item"].as_str().unwrap().to_owned())
        .collect()
}

// The SDK's own serialization of what the fake sent is the byte-for-byte reference for each item payload.
fn expected_items(log: &[Value], source: &str) -> Vec<String> {
    log.iter()
        .filter(|entry| entry["event"] == "sent" && entry["source"] == source)
        .map(|entry| match source {
            "session" => serde_json::to_string(&serde_json::from_value::<SessionEvent>(entry["item"].clone()).unwrap()),
            _ => {
                serde_json::to_string(&serde_json::from_value::<SessionLifecycleEvent>(entry["item"].clone()).unwrap())
            }
        })
        .map(Result::unwrap)
        .collect()
}

fn position(records: &[Value], predicate: impl Fn(&Value) -> bool) -> usize {
    records
        .iter()
        .position(predicate)
        .unwrap_or_else(|| panic!("no matching record in {records:?}"))
}

fn replies(records: &[Value]) -> Vec<Value> {
    records
        .iter()
        .filter(|record| record["type"] != "item" && record["type"] != "ready")
        .cloned()
        .collect()
}

fn turn_ended(records: &[Value]) -> Vec<Value> {
    records
        .iter()
        .filter(|record| record["type"] == "turn_ended")
        .cloned()
        .collect()
}

fn read_log(path: &Path) -> Vec<Value> {
    std::fs::read_to_string(path)
        .unwrap_or_default()
        .lines()
        .map(|line| serde_json::from_str(line).unwrap())
        .collect()
}

fn rpc_params(log: &[Value], method: &str) -> Vec<Value> {
    log.iter()
        .filter(|entry| entry["event"] == "rpc" && entry["method"] == method)
        .map(|entry| entry["params"].clone())
        .collect()
}

fn rpc_count(log: &[Value], method: &str) -> usize {
    rpc_params(log, method).len()
}

fn started_exes(log: &[Value]) -> Vec<PathBuf> {
    log.iter()
        .filter(|entry| entry["event"] == "start")
        .map(|entry| PathBuf::from(entry["exe"].as_str().unwrap()))
        .collect()
}

fn process_alive(pid: u64) -> bool {
    let filter = format!("PID eq {pid}");
    let output = Command::new("tasklist")
        .args(["/FI", &filter, "/FO", "CSV", "/NH"])
        .output()
        .unwrap();
    assert!(
        output.status.success() && output.stderr.is_empty(),
        "tasklist exited {}: {}",
        output.status,
        String::from_utf8_lossy(&output.stderr)
    );
    String::from_utf8_lossy(&output.stdout).contains(&format!("\"{pid}\""))
}
