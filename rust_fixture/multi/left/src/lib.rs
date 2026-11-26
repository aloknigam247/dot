// Same line positions.
//

#[allow(clippy::items_after_test_module)]
#[cfg(test)]
mod inline_checks {
    #[test]
    fn raw_braces() {
        let text = r#"} // {"#;
        /* } */
        assert!(text.contains('}'));
    }
}

pub fn covered_elsewhere() -> u32 {
    31
}

pub fn answer() -> u32 {
    42
}
