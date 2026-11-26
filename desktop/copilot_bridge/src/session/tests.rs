use super::*;
use TurnOutcome::{Aborted, Completed, Failed};
use serde_json::json;

fn event(kind: &str, data: Value) -> SessionEvent {
    serde_json::from_value(json!({
        "data": data, "id": "evt", "parentId": null, "timestamp": "2026-01-01T00:00:00Z", "type": kind
    }))
    .unwrap()
}

#[test]
fn terminal_evidence_sets_outcome_and_boundary() {
    let abort = event("abort", json!({}));
    let assistant = event("assistant.message", json!({"originatingMessageId": "id"}));
    let error = |data| event("session.error", data);
    let idle = event("session.idle", json!({}));
    let invalid_idle = event("session.idle", json!({"aborted": "yes"}));
    let stop = |reason, id| event("session.autopilot_stopped", json!({"reason": reason, "requestId": id}));
    let user = event("user.message", json!({"messageId": "id"}));
    let cases = [
        (vec![user.clone(), abort.clone(), idle.clone()], Aborted, true),
        (
            vec![user.clone(), event("session.idle", json!({"aborted": true}))],
            Aborted,
            true,
        ),
        (vec![assistant, idle.clone()], Completed, true),
        (vec![stop("aborted", "id")], Aborted, false),
        (vec![stop("background_timeout", "id")], Failed, false),
        (vec![stop("error", "id")], Failed, false),
        (vec![stop("task_complete", "id")], Completed, false),
        (
            vec![event("session.autopilot_stopped", json!({"requestId": "id"}))],
            Failed,
            false,
        ),
        (
            vec![
                user.clone(),
                event("session.autopilot_stopped", json!({"reason": "error"})),
                idle.clone(),
            ],
            Completed,
            true,
        ),
        (
            vec![
                user.clone(),
                event("session.idle", json!({"mode": "autopilot"})),
                abort.clone(),
                idle.clone(),
            ],
            Aborted,
            true,
        ),
        (
            vec![user.clone(), stop("aborted", "other"), idle.clone()],
            Completed,
            true,
        ),
        (vec![user.clone(), invalid_idle.clone()], Failed, false),
        (vec![invalid_idle, user.clone(), idle.clone()], Completed, true),
        (
            vec![
                event(
                    "session.warning",
                    json!({"warningType":"policy_blocked","originatingMessageId":"id"}),
                ),
                idle.clone(),
            ],
            Completed,
            true,
        ),
        (
            vec![user.clone(), error(json!({})), abort.clone(), idle.clone()],
            Aborted,
            true,
        ),
        (
            vec![error(json!({"originatingMessageId":"id"})), idle.clone()],
            Failed,
            true,
        ),
        (
            vec![
                user.clone(),
                error(json!({"originatingMessageId":"other"})),
                idle.clone(),
            ],
            Completed,
            true,
        ),
        (vec![error(json!({})), idle.clone()], Failed, false),
        (vec![error(json!({})), user.clone(), idle.clone()], Failed, true),
        (vec![error(json!({})), stop("task_complete", "id")], Failed, false),
        (vec![abort, user, idle], Completed, true),
    ];
    for (events, expected, reusable) in cases {
        let mut turn = ActiveTurn::new("request".into());
        turn.sdk_id = Some("id".into());
        for event in &events {
            if let Some(evidence) = evidence(event) {
                turn.observe(evidence);
            }
        }
        assert!(
            matches!(turn.state, TurnState::Ended { outcome, boundary_proven }
                if outcome == expected && boundary_proven == reusable),
            "{events:?}"
        );
    }
}

#[test]
fn malformed_correlation_id_fails_active_turn_without_proving_boundary() {
    for id in [json!(""), json!(null), json!(false), json!(7), json!([])] {
        for malformed in [
            event("session.error", json!({"originatingMessageId": id})),
            event(
                "session.autopilot_stopped",
                json!({"reason": "task_complete", "requestId": id}),
            ),
        ] {
            let mut turn = ActiveTurn::new("request".into());
            turn.sdk_id = Some("id".into());
            turn.observe(Evidence::Started("id".into()));
            turn.observe(evidence(&malformed).unwrap());
            assert!(
                matches!(
                    turn.state,
                    TurnState::Ended {
                        outcome: Failed,
                        boundary_proven: false
                    }
                ),
                "{malformed:?}"
            );
        }
    }
}

#[test]
fn non_policy_warning_is_not_turn_evidence() {
    let warning = event(
        "session.warning",
        json!({"warningType": "truncation", "originatingMessageId": "id"}),
    );
    assert!(evidence(&warning).is_none());
}

#[test]
fn another_turns_start_does_not_end_active_turn() {
    let mut turn = ActiveTurn::new("request".into());
    turn.sdk_id = Some("id".into());
    for event in [
        event("user.message", json!({"messageId": "other"})),
        event("session.idle", json!({})),
    ] {
        turn.observe(evidence(&event).unwrap());
    }
    assert!(matches!(turn.state, TurnState::Running));
    for event in [
        event("user.message", json!({"messageId": "id"})),
        event("session.idle", json!({})),
    ] {
        turn.observe(evidence(&event).unwrap());
    }
    assert!(matches!(
        turn.state,
        TurnState::Ended {
            outcome: Completed,
            boundary_proven: true
        }
    ));
}

#[test]
fn failed_turn_drops_existing_and_subsequent_evidence_before_ack() {
    let mut turn = ActiveTurn::new("request".into());
    turn.observe(Evidence::Abort);
    turn.fail();
    turn.observe(Evidence::Started("id".into()));
    assert!(turn.pending.is_empty());
    assert!(matches!(
        turn.state,
        TurnState::Ended {
            outcome: Failed,
            boundary_proven: false
        }
    ));
}

fn controller(out: &mut Vec<u8>) -> SessionController<&mut Vec<u8>, ()> {
    SessionController::new((), Output::new(out))
}

fn records(out: &[u8]) -> Vec<Value> {
    out.split(|byte| *byte == b'\n')
        .filter(|line| !line.is_empty())
        .map(|line| serde_json::from_slice(line).unwrap())
        .collect()
}

#[tokio::test]
async fn shutdown_fails_turn_whose_ack_arrives_while_waiting() {
    let mut out = Vec::new();
    let mut controller = controller(&mut out);
    let mut turn = ActiveTurn::new("t1".into());
    turn.observe(Evidence::Started("msg-1".into()));
    turn.observe(Evidence::Idle { aborted: false });
    controller.active_turn = Some(turn);
    let internal_tx = controller.internal_tx.clone();
    // The current-thread runtime runs this only once shutdown is blocked waiting for the acknowledgement.
    tokio::spawn(async move {
        let result = Ok("msg-1".into());
        internal_tx
            .send(Internal::SendDone {
                request_id: "t1".into(),
                result,
            })
            .unwrap();
    });
    controller.finish_active_turn().await.unwrap();
    assert!(!controller.reuse_safe);
    drop(controller);
    assert_eq!(
        records(&out),
        vec![
            json!({"type": "send_accepted", "request_id": "t1", "turn_id": "msg-1"}),
            json!({"type": "turn_ended", "outcome": "failed", "request_id": "t1", "turn_id": "msg-1"}),
        ]
    );
}

#[tokio::test]
async fn shutdown_answers_cancel_that_finishes_while_waiting() {
    let mut out = Vec::new();
    let mut controller = controller(&mut out);
    controller.pending_cancels.push("c1".into());
    let internal_tx = controller.internal_tx.clone();
    tokio::spawn(async move {
        let done = Internal::CancelDone {
            request_id: "c1".into(),
            result: Ok(()),
            turn_id: "msg-1".into(),
        };
        internal_tx.send(done).unwrap();
    });
    controller.finish_active_turn().await.unwrap();
    drop(controller);
    assert_eq!(
        records(&out),
        vec![json!({"type": "cancel_acknowledged", "request_id": "c1", "turn_id": "msg-1"})]
    );
}

#[test]
fn abort_precedes_error_at_autopilot_stop() {
    let mut turn = ActiveTurn::new("request".into());
    turn.sdk_id = Some("id".into());
    for event in [
        event("user.message", json!({"messageId": "id"})),
        event("session.error", json!({"originatingMessageId": "id"})),
        event("abort", json!({})),
        event(
            "session.autopilot_stopped",
            json!({"reason": "task_complete", "requestId": "id"}),
        ),
    ] {
        turn.observe(evidence(&event).unwrap());
    }
    assert!(matches!(
        turn.state,
        TurnState::Ended {
            outcome: Aborted,
            boundary_proven: false
        }
    ));
}
