use std::{
    fs,
    path::{Path, PathBuf},
};

use serde::{Deserialize, Serialize};

use crate::error::AppResult;

pub const DEFAULT_CONFIG_PATH: &str = "data/config.toml";

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(default)]
pub struct CaptureConfig {
    pub capture_dir: PathBuf,
    pub db_path: PathBuf,
    pub capture_on_focus: bool,
    pub capture_on_title_change: bool,
    pub capture_interval_ms: u64,
    pub max_captures_per_minute: u32,
    pub allow_monitor_fallback: bool,
    pub exclude_titles: Vec<String>,
    pub exclude_apps: Vec<String>,
    pub search_index_path: PathBuf,
    pub enable_search_index: bool,
}

impl Default for CaptureConfig {
    fn default() -> Self {
        Self {
            capture_dir: PathBuf::from("data/captures"),
            db_path: PathBuf::from("data/index.db"),
            capture_on_focus: true,
            capture_on_title_change: true,
            capture_interval_ms: 0,
            max_captures_per_minute: 20,
            allow_monitor_fallback: true,
            exclude_titles: vec![],
            exclude_apps: vec![],
            search_index_path: PathBuf::from("data/index.db"),
            enable_search_index: true,
        }
    }
}

impl CaptureConfig {
    pub fn load_or_init(path: &Path) -> AppResult<Self> {
        if path.exists() {
            let raw = fs::read_to_string(path)?;
            let parsed: CaptureConfig = toml::from_str(&raw)?;
            return Ok(parsed);
        }

        let default = CaptureConfig::default();
        if let Some(parent) = path.parent() {
            fs::create_dir_all(parent)?;
        }
        let encoded = toml::to_string_pretty(&default)?;
        fs::write(path, encoded)?;
        Ok(default)
    }
}
