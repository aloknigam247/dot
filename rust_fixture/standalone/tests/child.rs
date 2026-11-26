#[test]
fn spawned_only() {
    let child = std::process::Command::new(env!("CARGO_BIN_EXE_dot_standalone"))
        .output()
        .unwrap();
    assert!(child.status.success());
    assert_eq!(String::from_utf8(child.stdout).unwrap().trim(), "42");
}
