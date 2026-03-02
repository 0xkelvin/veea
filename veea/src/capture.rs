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

    /// Capture a single snapshot and store as PNG.
    pub fn snapshot_png(&mut self, label: &str) -> AppResult<PathBuf> {
        if self.paused.load(Ordering::Relaxed) {
            return Err(AppError::Capture("capture paused".to_string()));
        }

        let now = Utc::now();
        let id = Uuid::new_v4().to_string();
        let safe_label = normalized(label);
        let date_dir = self.date_dir(now);
        fs::create_dir_all(&date_dir)?;
        let filename = date_dir.join(format!("snapshot_{}_{}.png", safe_label, id));

        let (image, monitor_label) = self.capture_monitor_fallback()?;
        let width = image.width();
        let height = image.height();

        if width == 0 || height == 0 {
            return Err(AppError::Capture(format!(
                "captured image has invalid dimensions: {}x{}",
                width, height
            )));
        }

        image
            .save(&filename)
            .map_err(|e| AppError::Capture(e.to_string()))?;

        let record = CaptureRecord {
            id: id.clone(),
            ts: now,
            window_title: Some(label.to_string()),
            app_name: None,
            event_type: "snapshot".to_string(),
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

        Ok(filename)
    }

    /// Test function to verify capture is working
    pub fn test_capture(&self) -> AppResult<()> {
        println!("=== Testing capture functionality ===");
        
        // Test 1: List windows
        println!("Test 1: Listing windows...");
        match Window::all() {
            Ok(windows) => {
                let mut count = 0;
                for window in windows {
                    count += 1;
                    if let Ok(title) = window.title() {
                        if !title.is_empty() {
                            let minimized = window.is_minimized().unwrap_or(false);
                            println!("  Window {}: '{}' (minimized: {})", count, title, minimized);
                        }
                    }
                }
                println!("Found {} total windows", count);
            }
            Err(e) => {
                eprintln!("ERROR: Failed to list windows: {:?}", e);
                return Err(AppError::Capture(format!("Cannot list windows: {:?}", e)));
            }
        }
        
        // Test 2: Try to capture focused window
        println!("Test 2: Attempting to capture focused window...");
        if let Some(image) = self.capture_focused_window() {
            println!("SUCCESS: Captured focused window: {}x{}", image.width(), image.height());
        } else {
            eprintln!("FAILED: Could not capture focused window");
        }
        
        // Test 3: Try monitor capture
        println!("Test 3: Attempting monitor capture...");
        match self.capture_monitor_fallback() {
            Ok((image, name)) => {
                println!("SUCCESS: Captured monitor '{}': {}x{}", 
                    name.as_deref().unwrap_or("unknown"), image.width(), image.height());
            }
            Err(e) => {
                eprintln!("FAILED: Monitor capture error: {}", e);
            }
        }
        
        println!("=== Test complete ===");
        Ok(())
    }

    pub fn capture_event(&mut self, window_title: &str, event_type: &str) -> AppResult<()> {
        if self.paused.load(Ordering::Relaxed) {
            println!("Capture paused, skipping event for '{}'", window_title);
            return Ok(());
        }

        if self.should_skip(window_title) {
            println!("Window '{}' is in exclude list, skipping", window_title);
            return Ok(());
        }

        if !self.consume_rate_limit() {
            return Err(AppError::Capture(format!(
                "capture rate exceeded ({} per minute)",
                self.config.max_captures_per_minute
            )));
        }
        
        println!("Attempting to capture window '{}' (event: {})", window_title, event_type);

        let now = Utc::now();
        let id = Uuid::new_v4().to_string();
        let safe_title = normalized(window_title);
        let date_dir = self.date_dir(now);
        fs::create_dir_all(&date_dir)?;
        let filename = date_dir.join(format!("{event_type}_{safe_title}_{id}.png"));

        // Try to capture focused window first (more reliable)
        let (image, monitor_label) = match self.capture_focused_window() {
            Some(img) => {
                let w = img.width();
                let h = img.height();
                if w == 0 || h == 0 {
                    eprintln!("Warning: captured image has zero dimensions ({}x{})", w, h);
                } else {
                    println!("Captured focused window: {}x{}", w, h);
                }
                (img, None)
            }
            None => {
                // Fallback to searching by title
                match self.capture_window_image(window_title) {
                    Some(img) => {
                        let w = img.width();
                        let h = img.height();
                        if w == 0 || h == 0 {
                            eprintln!("Warning: captured image has zero dimensions ({}x{})", w, h);
                        } else {
                            println!("Captured window '{}': {}x{}", window_title, w, h);
                        }
                        (img, None)
                    }
                    None if self.config.allow_monitor_fallback => {
                        println!("Window capture failed for '{}', using monitor fallback", window_title);
                        self.capture_monitor_fallback()?
                    }
                    None => {
                        return Err(AppError::Capture(format!(
                            "no window matched title '{window_title}' and monitor fallback disabled"
                        )))
                    }
                }
            }
        };

        let width = image.width();
        let height = image.height();
        
        if width == 0 || height == 0 {
            return Err(AppError::Capture(format!(
                "captured image has invalid dimensions: {}x{}",
                width, height
            )));
        }
        
        image.save(&filename).map_err(|e| AppError::Capture(e.to_string()))?;
        println!("Saved screenshot: {} ({}x{})", filename.display(), width, height);

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

    fn capture_focused_window(&self) -> Option<xcap::image::RgbaImage> {
        // On macOS, Window::all() typically returns windows in z-order,
        // so the first visible, non-minimized window should be the focused one
        let windows = match Window::all() {
            Ok(w) => w,
            Err(e) => {
                eprintln!("ERROR: Failed to get window list: {:?}", e);
                return None;
            }
        };
        
        let mut tried = 0;
        for window in windows {
            tried += 1;
            
            let minimized = match window.is_minimized() {
                Ok(m) => m,
                Err(e) => {
                    eprintln!("WARNING: Failed to check if window minimized: {:?}", e);
                    continue;
                }
            };
            if minimized {
                continue;
            }
            
            let title = match window.title() {
                Ok(t) => t,
                Err(e) => {
                    eprintln!("WARNING: Failed to get window title: {:?}", e);
                    continue;
                }
            };
            
            // Skip empty titles (usually background/system windows)
            if title.is_empty() {
                continue;
            }
            
            // Try to capture this window
            match window.capture_image() {
                Ok(image) => {
                    let w = image.width();
                    let h = image.height();
                    if w > 0 && h > 0 {
                        println!("Successfully captured window '{}': {}x{} (tried {} windows)", title, w, h, tried);
                        return Some(image);
                    } else {
                        eprintln!("WARNING: Window '{}' captured but has zero dimensions: {}x{}", title, w, h);
                    }
                }
                Err(e) => {
                    eprintln!("ERROR: Failed to capture window '{}': {:?}", title, e);
                    // On macOS, this often means Screen Recording permission is missing
                    if e.to_string().contains("permission") || e.to_string().contains("denied") {
                        eprintln!("HINT: Check System Settings > Privacy & Security > Screen Recording");
                    }
                }
            }
        }
        
        eprintln!("ERROR: Tried {} windows but none could be captured", tried);
        None
    }

    fn capture_window_image(&self, window_title: &str) -> Option<xcap::image::RgbaImage> {
        if let Ok(windows) = Window::all() {
            // First, try to find the focused window by title
            for window in windows {
                if let Ok(title) = window.title() {
                    if title == window_title {
                        // Check if window is visible and not minimized
                        if let Ok(minimized) = window.is_minimized() {
                            if minimized {
                                eprintln!("Window '{}' is minimized, skipping", window_title);
                                continue;
                            }
                        }
                        if let Ok(image) = window.capture_image() {
                            // Validate image has content
                            let w = image.width();
                            let h = image.height();
                            if w > 0 && h > 0 {
                                return Some(image);
                            } else {
                                eprintln!("Window '{}' captured but has zero dimensions: {}x{}", window_title, w, h);
                            }
                        } else {
                            eprintln!("Failed to capture image for window '{}'", window_title);
                        }
                    }
                }
            }
        } else {
            eprintln!("Failed to get window list");
        }
        None
    }

    fn capture_monitor_fallback(&self) -> AppResult<(xcap::image::RgbaImage, Option<String>)> {
        let monitors = match Monitor::all() {
            Ok(m) => m,
            Err(e) => {
                let err_msg = format!("Failed to get monitors: {:?}", e);
                eprintln!("ERROR: {}", err_msg);
                if e.to_string().contains("permission") || e.to_string().contains("denied") {
                    eprintln!("HINT: Check System Settings > Privacy & Security > Screen Recording");
                }
                return Err(AppError::Capture(err_msg));
            }
        };
        
        if monitors.is_empty() {
            return Err(AppError::Capture("no monitors available".to_string()));
        }
        
        let monitor = &monitors[0];
        let monitor_name = monitor.name().ok();
        
        let image = match monitor.capture_image() {
            Ok(img) => img,
            Err(e) => {
                let err_msg = format!("Failed to capture monitor '{}': {:?}", 
                    monitor_name.as_deref().unwrap_or("unknown"), e);
                eprintln!("ERROR: {}", err_msg);
                if e.to_string().contains("permission") || e.to_string().contains("denied") {
                    eprintln!("HINT: Check System Settings > Privacy & Security > Screen Recording");
                }
                return Err(AppError::Capture(err_msg));
            }
        };
        
        let w = image.width();
        let h = image.height();
        if w == 0 || h == 0 {
            return Err(AppError::Capture(format!(
                "monitor capture returned zero dimensions: {}x{}",
                w, h
            )));
        }
        println!("Monitor fallback captured: {}x{} from '{}'", w, h, 
            monitor_name.as_deref().unwrap_or("unknown"));
        Ok((image, monitor_name))
    }
}
