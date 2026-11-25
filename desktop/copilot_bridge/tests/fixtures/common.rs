use std::path::{Path, PathBuf};
use std::time::Duration;

pub const LEGACY_VERSION: &str = "3.0.0-fake";
pub const LINGER: Duration = Duration::from_secs(60);
pub const VERSION: &str = "9.9.9-fake";

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
