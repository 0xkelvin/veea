use std::{collections::VecDeque, fs, path::PathBuf};
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};

use chrono::{DateTime, Datelike, Utc};
use uuid::Uuid;
use xcap::{Monitor, Window};

use crate::{
    config::CaptureConfig,
    db::{CaptureRecord, Db},
    error::{AppError, AppResult},
    search::SearchIndex,
};

fn normalized(filename: &str) -> String {
    filename.replace(['|', '\\', ':', '/', '<', '>', '"', '?', '*'], "_")
}

pub struct CaptureEngine {
    config: CaptureConfig,
    db: Db,
    recent_captures: VecDeque<DateTime<Utc>>,
    search: Option<SearchIndex>,
    paused: Arc<AtomicBool>,
}

impl CaptureEngine {
    pub fn new(
        config: CaptureConfig,
        db: Db,
        paused: Arc<AtomicBool>,
    ) -> AppResult<Self> {
        let search = if config.enable_search_index {
            Some(SearchIndex::new(&config.search_index_path)?)
        } else {
            None
        };

        Ok(Self {
            config,
            db,
            recent_captures: VecDeque::new(),
            search,
            paused,
        })
    }

    pub fn db_path(&self) -> PathBuf {
        self.db.connection_path()
    }

    pub fn capture_event(&mut self, window_title: &str, event_type: &str) -> AppResult<()> {
        if self.paused.load(Ordering::Relaxed) {
            return Ok(());
        }

        if self.should_skip(window_title) {
            return Ok(());
        }

        if !self.consume_rate_limit() {
            return Err(AppError::Capture(format!(
                "capture rate exceeded ({} per minute)",
                self.config.max_captures_per_minute
            )));
        }

        let now = Utc::now();
        let id = Uuid::new_v4().to_string();
        let safe_title = normalized(window_title);
        let date_dir = self.date_dir(now);
        fs::create_dir_all(&date_dir)?;
        let filename = date_dir.join(format!("{event_type}_{safe_title}_{id}.png"));

        let (image, monitor_label) = match self.capture_window_image(window_title) {
            Some(img) => (img, None),
            None if self.config.allow_monitor_fallback => self.capture_monitor_fallback()?,
            None => {
                return Err(AppError::Capture(format!(
                    "no window matched title '{window_title}' and monitor fallback disabled"
                )))
            }
        };

        let width = image.width();
        let height = image.height();
        image.save(&filename).map_err(|e| AppError::Capture(e.to_string()))?;

        let record = CaptureRecord {
            id: id.clone(),
            ts: now,
            window_title: Some(window_title.to_string()),
            app_name: None,
            event_type: event_type.to_string(),
            path: filename.to_string_lossy().to_string(),
            width: Some(width),
            height: Some(height),
            monitor: monitor_label,
            hash: None,
        };

        self.db.insert_capture(&record)?;
        if let Some(index) = &self.search {
            let _ = index.add_capture(&record, None);
        }
        Ok(())
    }

    fn date_dir(&self, ts: DateTime<Utc>) -> PathBuf {
        self.config
            .capture_dir
            .join(format!("{:04}", ts.year()))
            .join(format!("{:02}", ts.month()))
            .join(format!("{:02}", ts.day()))
    }

    fn should_skip(&self, window_title: &str) -> bool {
        let lower_title = window_title.to_lowercase();
        self.config
            .exclude_titles
            .iter()
            .any(|p| lower_title.contains(&p.to_lowercase()))
    }

    fn consume_rate_limit(&mut self) -> bool {
        let limit = self.config.max_captures_per_minute as usize;
        if limit == 0 {
            return true;
        }
        let now = Utc::now();
        while let Some(front) = self.recent_captures.front() {
            if (*front + chrono::Duration::seconds(60)) < now {
                self.recent_captures.pop_front();
            } else {
                break;
            }
        }
        if self.recent_captures.len() >= limit {
            return false;
        }
        self.recent_captures.push_back(now);
        true
    }

    fn capture_window_image(&self, window_title: &str) -> Option<xcap::image::RgbaImage> {
        if let Ok(windows) = Window::all() {
            for window in windows {
                if let Ok(title) = window.title() {
                    if title == window_title {
                        if let Ok(image) = window.capture_image() {
                            return Some(image);
                        }
                    }
                }
            }
        }
        None
    }

    fn capture_monitor_fallback(&self) -> AppResult<(xcap::image::RgbaImage, Option<String>)> {
        let monitors = Monitor::all().map_err(|e| AppError::Capture(e.to_string()))?;
        if let Some(monitor) = monitors.first() {
            let image = monitor
                .capture_image()
                .map_err(|e| AppError::Capture(e.to_string()))?;
            let monitor_name = monitor.name().ok();
            return Ok((image, monitor_name));
        }
        Err(AppError::Capture("no monitors available".to_string()))
    }
}
