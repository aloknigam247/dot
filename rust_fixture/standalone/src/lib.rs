#[cfg(test)]
mod external_checks;

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

pub fn uncalled() -> u32 {
    19
}

pub fn answer() -> u32 {
    42
}
