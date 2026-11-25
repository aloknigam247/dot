//! Hermetic protocol tests. The test binary doubles as the fake Copilot CLI: when the helper starts a copy of it
//! with DOT_FAKE_CLI_LOG set, `main` serves the SDK's JSON-RPC protocol instead of running tests.

use std::fs::OpenOptions;
use std::io::{BufRead, BufReader, Read, Write};
use std::os::windows::io::{FromRawHandle, OwnedHandle};
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::path::{Path, PathBuf};
use std::process::{Child, ChildStdin, Command, ExitCode, Stdio};
use std::ptr;
use std::sync::mpsc::{Receiver, RecvTimeoutError, sync_channel};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::{Duration, Instant};

use copilot_sdk::{SessionEvent, SessionLifecycleEvent};
use serde_json::{Value, json};
use tempfile::TempDir;
use windows_sys::Win32::Foundation::INVALID_HANDLE_VALUE;
use windows_sys::Win32::Storage::FileSystem::PIPE_ACCESS_INBOUND;
use windows_sys::Win32::System::Pipes::{CreateNamedPipeW, PIPE_READMODE_MESSAGE, PIPE_TYPE_MESSAGE, PIPE_WAIT};

const FAKE_LOG_VARIABLE: &str = "DOT_FAKE_CLI_LOG";
const READ_TIMEOUT: Duration = Duration::from_secs(20);
const STARTUP_TIMEOUT_VARIABLE: &str = "DOT_COPILOT_HELPER_STARTUP_TIMEOUT_MS";
const TEST_THREADS: usize = 4;

type Test = (&'static str, fn());

const TESTS: &[Test] = &[
    ("auth_status_is_reported", auth_status_is_reported),
    ("cancel_correlation", cancel_correlation),
    (
        "cancel_during_send_startup_is_rejected",
        cancel_during_send_startup_is_rejected,
    ),
    ("cancel_pending_rejects_send", cancel_pending_rejects_send),
    ("cli_status_timeout_reports_unknown", cli_status_timeout_reports_unknown),
    (
        "closed_stdin_finishes_starting_turn",
        closed_stdin_finishes_starting_turn,
    ),
    (
        "closed_stdin_rejects_unacknowledged_send",
        closed_stdin_rejects_unacknowledged_send,
    ),
    ("closed_stdin_stops_child", closed_stdin_stops_child),
    ("closed_stream_fails_turn", closed_stream_fails_turn),
    (
        "cmd_wrapper_mismatch_stops_lingering_cli",
        cmd_wrapper_mismatch_stops_lingering_cli,
    ),
    ("cmd_wrapper_runs_turn", cmd_wrapper_runs_turn),
    (
        "discovery_ignores_malformed_pathext_entries",
        discovery_ignores_malformed_pathext_entries,
    ),
    (
        "discovery_selects_first_successful_handshake",
        discovery_selects_first_successful_handshake,
    ),
    (
        "discovery_skips_relative_path_entries",
        discovery_skips_relative_path_entries,
    ),
    ("empty_turn_id_is_rejected", empty_turn_id_is_rejected),
    ("failed_abort_is_rejected", failed_abort_is_rejected),
    ("fatal_shutdown_fails_turn", fatal_shutdown_fails_turn),
    ("invalid_requests_are_reported", invalid_requests_are_reported),
    ("items_match_sdk_serialization", items_match_sdk_serialization),
    ("lagged_stream_fails_turn", lagged_stream_fails_turn),
    ("missing_cli_reports_not_found", missing_cli_reports_not_found),
    ("permission_request_is_denied", permission_request_is_denied),
    (
        "protocol_mismatch_omits_unanswered_version",
        protocol_mismatch_omits_unanswered_version,
    ),
    ("protocol_mismatch_reports_version", protocol_mismatch_reports_version),
    ("rejected_send_emits_no_turn_ended", rejected_send_emits_no_turn_ended),
    (
        "session_create_failure_reports_start_failed",
        session_create_failure_reports_start_failed,
    ),
    (
        "session_error_before_send_ack_fails_turn",
        session_error_before_send_ack_fails_turn,
    ),
    ("session_error_fails_turn_at_idle", session_error_fails_turn_at_idle),
    ("shutdown_answers_pending_cancel", shutdown_answers_pending_cancel),
    (
        "shutdown_before_send_ack_fails_turn",
        shutdown_before_send_ack_fails_turn,
    ),
    ("shutdown_finishes_active_turn", shutdown_finishes_active_turn),
    ("shutdown_rejects_unanswered_cancel", shutdown_rejects_unanswered_cancel),
    ("shutdown_request_stops_child", shutdown_request_stops_child),
    ("startup_failures_report_candidates", startup_failures_report_candidates),
    ("startup_items_follow_ready", startup_items_follow_ready),
    ("stdin_read_error_fails_turn", stdin_read_error_fails_turn),
    (
        "stdout_failure_is_reported_on_stderr",
        stdout_failure_is_reported_on_stderr,
    ),
    ("sub_agent_events_do_not_end_turn", sub_agent_events_do_not_end_turn),
    (
        "terminal_before_send_ack_holds_turn_ended",
        terminal_before_send_ack_holds_turn_ended,
    ),
    ("terminal_evidence_sets_outcome", terminal_evidence_sets_outcome),
    ("trailing_idle_never_ends_next_turn", trailing_idle_never_ends_next_turn),
    (
        "uncorrelated_start_waits_for_shutdown",
        uncorrelated_start_waits_for_shutdown,
    ),
    (
        "unproven_boundary_rejects_next_send",
        unproven_boundary_rejects_next_send,
    ),
];

fn main() -> ExitCode {
    if let Some(log) = std::env::var_os(FAKE_LOG_VARIABLE) {
        fake::run(PathBuf::from(log));
    }
    let filters: Vec<String> = std::env::args().skip(1).filter(|arg| !arg.starts_with('-')).collect();
    let selected: Vec<Test> = TESTS
        .iter()
        .copied()
        .filter(|(name, _)| filters.is_empty() || filters.iter().any(|filter| name.contains(filter.as_str())))
        .collect();
    println!("running {} tests", selected.len());
    let failures = Arc::new(Mutex::new(Vec::new()));
    let queue = Arc::new(Mutex::new(selected.clone().into_iter()));
    thread::scope(|scope| {
        for _ in 0..TEST_THREADS {
            let failures = Arc::clone(&failures);
            let queue = Arc::clone(&queue);
            scope.spawn(move || {
                loop {
                    let next = queue.lock().unwrap().next();
                    let Some((name, test)) = next else {
                        break;
                    };
                    let passed = catch_unwind(AssertUnwindSafe(test)).is_ok();
                    println!("test {name} ... {}", if passed { "ok" } else { "FAILED" });
                    if !passed {
                        failures.lock().unwrap().push(name);
                    }
                }
            });
        }
    });
    let failures = failures.lock().unwrap();
    let passed = selected.len() - failures.len();
    if failures.is_empty() {
        println!("\ntest result: ok. {passed} passed; 0 failed");
        ExitCode::SUCCESS
    } else {
        println!(
            "\nfailures: {failures:?}\ntest result: FAILED. {passed} passed; {} failed",
            failures.len()
        );
        ExitCode::FAILURE
    }
}

// SDK event forwarding

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

fn terminal_before_send_ack_holds_turn_ended() {
    let fixture = Fixture::new();
    let mut helper = fixture.helper();
    helper.expect_ready();
    helper.send("t1", "early_terminal");
    helper.until_turn_ended("t1");
    let (code, records) = helper.shutdown();
    assert_eq!(code, 0);
    assert_eq!(items(&records, "session"), expected_items(&fixture.log(), "session"));
    let accepted = position(&records, |record| record["type"] == "send_accepted");
    let idle = position(&records, |record| item_type(record).as_deref() == Some("session.idle"));
    let ended = position(&records, |record| record["type"] == "turn_ended");
    assert!(accepted < ended && idle < ended, "{records:?}");
    assert_eq!(
        turn_ended(&records),
        vec![json!({"type": "turn_ended", "outcome": "completed", "request_id": "t1", "turn_id": "msg-1"})]
    );
}

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

fn empty_turn_id_is_rejected() {
    let fixture = Fixture::new();
    let mut helper = fixture.helper();
    helper.expect_ready();
    helper.send("t1", "empty_id");
    assert_rejection(&helper.until_type("send_rejected"), "t1", "empty_turn_id");
    helper.until(|record| item_type(record).as_deref() == Some("session.idle"));
    helper.send("t2", "complete");
    assert_rejection(&helper.until_type("send_rejected"), "t2", "reuse_rejected");
    let (code, records) = helper.shutdown();
    assert_eq!(code, 0);
    assert!(turn_ended(&records).is_empty(), "{records:?}");
    assert_eq!(items(&records, "session"), expected_items(&fixture.log(), "session"));
}

// Turn attribution and errors

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
    helper.send("t2", "complete");
    assert_rejection(&helper.until_type("send_rejected"), "t2", "session_failed");
    let (code, records) = helper.shutdown();
    assert_eq!(code, 0);
    assert_eq!(turn_ended(&records).len(), 1);
}

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

fn failed_abort_is_rejected() {
    let fixture = Fixture::with_scenario("abort_error");
    let mut helper = fixture.helper();
    helper.expect_ready();
    helper.send("t1", "hold");
    helper.until_type("send_accepted");
    helper.cancel("c1", "msg-1");
    let before_shutdown = helper.until(|record| record["type"] == "cancel_rejected");
    assert_rejection(before_shutdown.last().unwrap(), "c1", "abort_failed");
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

fn terminal_evidence_sets_outcome() {
    let cases = [
        ("abort_plain_idle", "aborted", 1, true),
        ("aborted_idle", "aborted", 1, true),
        ("assistant_start", "completed", 1, true),
        ("autopilot:", "failed", 0, false),
        ("autopilot:aborted", "aborted", 0, false),
        ("autopilot:background_timeout", "failed", 0, false),
        ("autopilot:error", "failed", 0, false),
        ("autopilot_idle", "completed", 2, true),
        ("foreign_autopilot", "completed", 1, true),
        ("malformed_idle", "failed", 1, false),
        ("malformed_idle_unstarted", "completed", 2, true),
        ("policy_blocked", "completed", 1, true),
        ("session_error_abort", "aborted", 1, true),
        ("session_error_foreign", "completed", 1, true),
        ("session_error_uncorrelated", "failed", 1, true),
        ("session_error_unstarted", "failed", 1, true),
        ("stale_abort", "completed", 1, true),
    ];
    for (prompt, outcome, idles, reusable) in cases {
        let fixture = Fixture::new();
        let mut helper = fixture.helper();
        helper.expect_ready();
        helper.send("t1", prompt);
        let turn = helper.until_turn_ended("t1");
        assert_eq!(turn.last().unwrap()["outcome"], outcome, "{prompt}");
        let seen_idles = turn
            .iter()
            .filter(|record| item_type(record).as_deref() == Some("session.idle"))
            .count();
        assert_eq!(seen_idles, idles, "{prompt}");
        helper.send("t2", "complete");
        if reusable {
            assert_eq!(
                helper.until_turn_ended("t2").last().unwrap()["outcome"],
                "completed",
                "{prompt}"
            );
        } else {
            assert_rejection(&helper.until_type("send_rejected"), "t2", "reuse_rejected");
        }
        let (code, records) = helper.shutdown();
        assert_eq!(code, 0, "{prompt}");
        assert_eq!(turn_ended(&records).len(), if reusable { 2 } else { 1 }, "{prompt}");
    }
}

// Permission, auth, and shutdown behavior

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
    let answers = rpc_params(&log, "session.permissions.handlePendingPermissionRequest");
    assert_eq!(answers.len(), 1);
    assert_eq!(answers[0]["requestId"], "perm-1");
    assert_eq!(answers[0]["result"]["kind"], "reject");
    let starts: Vec<&Value> = log.iter().filter(|entry| entry["event"] == "start").collect();
    assert_eq!(starts.len(), 1);
    assert_eq!(starts[0]["telemetry_disabled"], "true");
}

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

fn shutdown_request_stops_child() {
    let fixture = Fixture::new();
    let mut helper = fixture.helper();
    helper.expect_ready();
    let (code, _) = helper.shutdown();
    assert_eq!(code, 0);
    fixture.assert_children_exited();
    assert_eq!(rpc_count(&fixture.log(), "runtime.shutdown"), 1);
}

fn closed_stdin_stops_child() {
    let fixture = Fixture::new();
    let mut helper = fixture.helper();
    helper.expect_ready();
    let (code, _) = helper.finish();
    assert_eq!(code, 0);
    fixture.assert_children_exited();
}

fn stdout_failure_is_reported_on_stderr() {
    let fixture = Fixture::empty();
    let stdout = fixture.root.path().join("stdout.txt");
    std::fs::write(&stdout, "").unwrap();
    // A read-only handle makes writing the cli_not_found record fail.
    let output = Command::new(env!("CARGO_BIN_EXE_dot_copilot_helper"))
        .current_dir(fixture.root.path())
        .env("PATH", "")
        .env_remove("COPILOT_CLI_PATH")
        .stdin(Stdio::null())
        .stdout(std::fs::File::open(&stdout).unwrap())
        .output()
        .unwrap();
    assert_eq!(output.status.code(), Some(1));
    let stderr = String::from_utf8_lossy(&output.stderr);
    assert!(stderr.starts_with("dot_copilot_helper: "), "{stderr}");
}

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

fn discovery_selects_first_successful_handshake() {
    let fixture = Fixture::empty();
    let first = fixture.dir("first");
    let second = fixture.dir("second");
    let third = fixture.dir("third");
    fixture.install(&first, "copilot.com", "exit");
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
    // Both waits together stay below LINGER, so a probe CLI left running fails here rather than exiting on its own.
    assert_eq!(helper.wait(), 1);
    fixture.assert_children_exit_within(fake::LINGER / 3);
    assert_eq!(rpc_count(&fixture.log(), "status.get"), 1);
    let (_, records) = helper.finish();
    assert_eq!(records.len(), 1);
}

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

fn shutdown_answers_pending_cancel() {
    let fixture = Fixture::with_scenario("idle_before_abort");
    let mut helper = fixture.helper();
    helper.expect_ready();
    helper.send("t1", "hold");
    helper.until_type("send_accepted");
    helper.cancel("c1", "msg-1");
    helper.until_turn_ended("t1");
    helper.request(json!({"type": "shutdown", "request_id": "stop"}));
    // Releasing well after the helper reads shutdown makes the held abort reply arrive while the helper is exiting.
    thread::sleep(Duration::from_millis(500));
    fixture.release();
    let (code, records) = helper.drain();
    assert_eq!(code, 0);
    assert_eq!(
        replies(&records),
        vec![
            json!({"type": "send_accepted", "request_id": "t1", "turn_id": "msg-1"}),
            json!({"type": "turn_ended", "outcome": "completed", "request_id": "t1", "turn_id": "msg-1"}),
            json!({"type": "cancel_acknowledged", "request_id": "c1", "turn_id": "msg-1"}),
        ]
    );
    fixture.assert_children_exited();
}

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
        let exe = std::env::current_exe().unwrap();
        let target = dir.join(name);
        if std::fs::hard_link(&exe, &target).is_err() {
            std::fs::copy(&exe, &target).unwrap();
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
        // A failed test can leave a fake CLI running; the image filter avoids killing a process that reused its pid.
        for entry in self.log().iter().filter(|entry| entry["event"] == "start") {
            let exe = PathBuf::from(entry["exe"].as_str().unwrap());
            let image = format!("IMAGENAME eq {}", exe.file_name().unwrap().to_string_lossy());
            let pid = entry["pid"].to_string();
            let _ = Command::new("taskkill")
                .args(["/F", "/PID", &pid, "/FI", &image])
                .stderr(Stdio::null())
                .stdout(Stdio::null())
                .status();
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
    String::from_utf8_lossy(&output.stdout).contains(&format!("\"{pid}\""))
}

mod fake {
    use super::*;

    pub const LEGACY_VERSION: &str = "3.0.0-fake";
    pub const LINGER: Duration = Duration::from_secs(60);
    pub const VERSION: &str = "9.9.9-fake";

    const BURST_EVENTS: usize = 3000;

    pub fn scenario_path(exe: &Path) -> PathBuf {
        let mut path = exe.as_os_str().to_owned();
        path.push(".scenario");
        PathBuf::from(path)
    }

    pub fn release_path(log: &Path) -> PathBuf {
        let mut path = log.as_os_str().to_owned();
        path.push(".release");
        PathBuf::from(path)
    }

    pub fn run(log: PathBuf) -> ! {
        let exe = std::env::current_exe().unwrap();
        let scenario = std::fs::read_to_string(scenario_path(&exe)).unwrap_or_default();
        let mut fake = Fake {
            connected: false,
            event_count: 0,
            log,
            message_count: 0,
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
            self.record(json!({"event": "rpc", "method": method, "params": params}));
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
                "abort_plain_idle" => {
                    self.reply(id, accepted);
                    self.user(&message_id);
                    self.event("abort", json!({"reason": "user initiated"}));
                    self.idle();
                }
                "aborted_idle" => {
                    self.reply(id, accepted);
                    self.user(&message_id);
                    self.event("session.idle", json!({"aborted": true}));
                }
                "assistant_start" => {
                    self.reply(id, accepted);
                    self.event(
                        "assistant.message",
                        json!({"content": "Hello", "messageId": "reply", "originatingMessageId": message_id}),
                    );
                    self.idle();
                }
                "autopilot" => {
                    self.reply(id, accepted);
                    self.user(&message_id);
                    self.delta("working");
                    self.event(
                        "session.autopilot_stopped",
                        json!({"reason": "task_complete", "requestId": message_id}),
                    );
                }
                "autopilot_idle" => {
                    self.reply(id, accepted);
                    self.user(&message_id);
                    self.event("session.idle", json!({"mode": "autopilot"}));
                    self.delta("continuing");
                    self.idle();
                }
                _ if prompt.starts_with("autopilot:") => {
                    let reason = &prompt["autopilot:".len()..];
                    let data = match reason {
                        "" => json!({"requestId": message_id}),
                        _ => json!({"reason": reason, "requestId": message_id}),
                    };
                    self.reply(id, accepted);
                    self.user(&message_id);
                    self.event("session.autopilot_stopped", data);
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
                "foreign_autopilot" => {
                    self.reply(id, accepted);
                    self.user(&message_id);
                    self.event(
                        "session.autopilot_stopped",
                        json!({"reason": "aborted", "requestId": "msg-other"}),
                    );
                    self.idle();
                }
                "hold" => {
                    self.reply(id, accepted);
                    self.user(&message_id);
                    self.delta("thinking");
                    self.waiting_for_abort = true;
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
                "malformed_idle" => {
                    self.reply(id, accepted);
                    self.user(&message_id);
                    self.event("session.idle", json!({"aborted": "yes"}));
                }
                "malformed_idle_unstarted" => {
                    self.reply(id, accepted);
                    self.event("session.idle", json!({"aborted": "yes"}));
                    self.complete(&message_id);
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
                "policy_blocked" => {
                    self.reply(id, accepted);
                    let warning = json!({
                        "message": "blocked",
                        "originatingMessageId": message_id,
                        "warningType": "policy_blocked",
                    });
                    self.event("session.warning", warning);
                    self.idle();
                }
                "send_error" => self.reply_error(id, -32000, "send failed"),
                "session_error" => {
                    self.reply(id, accepted);
                    self.user(&message_id);
                    self.session_error(None);
                    self.delta("recovered");
                    self.idle();
                }
                "session_error_abort" => {
                    self.reply(id, accepted);
                    self.user(&message_id);
                    self.session_error(Some(&message_id));
                    self.event("session.idle", json!({"aborted": true}));
                }
                "session_error_before_ack" => {
                    self.user(&message_id);
                    self.session_error(Some(&message_id));
                    self.idle();
                    self.wait_for_release();
                    self.reply(id, accepted);
                }
                "session_error_foreign" => {
                    self.reply(id, accepted);
                    self.user(&message_id);
                    self.session_error(Some("msg-other"));
                    self.idle();
                }
                "session_error_uncorrelated" => {
                    self.reply(id, accepted);
                    self.session_error(None);
                    self.idle();
                }
                "session_error_unstarted" => {
                    self.reply(id, accepted);
                    self.session_error(Some(&message_id));
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
                "stale_abort" => {
                    self.reply(id, accepted);
                    self.event("abort", json!({"reason": "user initiated"}));
                    self.user(&message_id);
                    self.idle();
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
}
