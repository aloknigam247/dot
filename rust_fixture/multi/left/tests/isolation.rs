#[cfg(not(feature = "sibling"))]
#[test]
fn isolated_left() {
    assert_eq!(dot_left::covered_elsewhere(), 31);
}
