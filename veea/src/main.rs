mod api;
mod capture;
mod config;
mod db;
mod error;
mod search;

use std::{
    sync::mpsc,
    thread,
    time::Duration,
};
use std::sync::Arc;
use std::sync::atomic::AtomicBool;

use capture::CaptureEngine;
use config::{CaptureConfig, DEFAULT_CONFIG_PATH};
use error::{AppError, AppResult};
use std::net::SocketAddr;
use xcap::Window;
use std::path::Path;

#[derive(Debug, Clone)]
enum WindowEvent {
    FocusChanged { window_title: String },
    TitleChanged { window_title: String },
    Periodic { window_title: String },
}

fn get_focused_window() -> Option<(u32, String)> {
    if let Ok(windows) = Window::all() {
        for window in windows {
            if let Ok(is_minimized) = window.is_minimized() {
                if !is_minimized {
                    if let Ok(title) = window.title() {
                        if !title.is_empty() {
                            if let Ok(window_id) = window.id() {
                                return Some((window_id, title));
                            }
                        }
                    }
                }
            }
        }
    }
    None
}

fn monitor_window_events(event_sender: mpsc::Sender<WindowEvent>) {
    let mut last_focused_window_id: Option<u32> = None;
    let mut last_window_title: Option<String> = None;

    loop {
        if let Some((window_id, window_title)) = get_focused_window() {
            if last_focused_window_id != Some(window_id) {
                let _ = event_sender.send(WindowEvent::FocusChanged {
                    window_title: window_title.clone(),
                });
                last_focused_window_id = Some(window_id);
            }

            if last_focused_window_id == Some(window_id) {
                if last_window_title.as_ref() != Some(&window_title) {
                    let _ = event_sender.send(WindowEvent::TitleChanged {
                        window_title: window_title.clone(),
                    });
                }
            }

            last_window_title = Some(window_title);
        } else {
            if last_focused_window_id.is_some() {
                last_focused_window_id = None;
                last_window_title = None;
            }
        }

        thread::sleep(Duration::from_millis(200));
    }
}

fn monitor_periodic(event_sender: mpsc::Sender<WindowEvent>, interval_ms: u64) {
    loop {
        if let Some((_id, title)) = get_focused_window() {
            let _ = event_sender.send(WindowEvent::Periodic { window_title: title });
        }
        thread::sleep(Duration::from_millis(interval_ms));
    }
}

fn run() -> AppResult<()> {
    println!("Starting capture daemon...");
    let config = CaptureConfig::load_or_init(Path::new(DEFAULT_CONFIG_PATH))?;
    let db = db::Db::new(&config.db_path)?;
    let pause_flag = Arc::new(AtomicBool::new(false));
    let mut engine = CaptureEngine::new(config.clone(), db, pause_flag.clone())?;
    let api_state = api::ApiState {
        db_path: engine.db_path(),
        config: config.clone(),
        search_index_path: config.search_index_path.clone(),
        pause_flag: pause_flag.clone(),
    };

    let (tx, rx) = mpsc::channel();

    let watcher_tx = tx.clone();
    thread::spawn(move || {
        monitor_window_events(watcher_tx);
    });

    // Start local API server
    let api_handle = api_state.clone();
    thread::spawn(move || {
        let rt = tokio::runtime::Runtime::new().expect("failed to create tokio runtime");
        let addr: SocketAddr = "127.0.0.1:8787"
            .parse()
            .expect("failed to parse default API address");
        if let Err(e) = rt.block_on(api::serve(addr, api_handle)) {
            eprintln!("API server failed: {e}");
        }
    });

    if config.capture_interval_ms > 0 {
        let periodic_tx = tx.clone();
        let interval = config.capture_interval_ms;
        thread::spawn(move || monitor_periodic(periodic_tx, interval));
    }

    println!(
        "Monitoring window events... captures stored under {:?}",
        config.capture_dir
    );

    for event in rx {
        match event {
            WindowEvent::FocusChanged { window_title }
                if config.capture_on_focus =>
            {
                println!("Focus changed to: {}", window_title);
                if let Err(e) = engine.capture_event(&window_title, "focus") {
                    eprintln!("Capture failed: {}", e);
                }
            }
            WindowEvent::TitleChanged { window_title }
                if config.capture_on_title_change =>
            {
                println!("Title changed to: {}", window_title);
                if let Err(e) = engine.capture_event(&window_title, "title") {
                    eprintln!("Capture failed: {}", e);
                }
            }
            WindowEvent::Periodic { window_title } => {
                if let Err(e) = engine.capture_event(&window_title, "interval") {
                    if !matches!(e, AppError::Capture(_)) {
                        eprintln!("Capture failed: {}", e);
                    }
                }
            }
            _ => {}
        }
    }

    Ok(())
}

fn test_capture() -> AppResult<()> {
    println!("=== Veea Capture Test Mode ===");
    let config = CaptureConfig::load_or_init(Path::new(DEFAULT_CONFIG_PATH))?;
    let db = db::Db::new(&config.db_path)?;
    let pause_flag = Arc::new(AtomicBool::new(false));
    let engine = CaptureEngine::new(config, db, pause_flag)?;
    engine.test_capture()
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    if args.len() > 1 && args[1] == "test" {
        if let Err(e) = test_capture() {
            eprintln!("Test failed: {e}");
            std::process::exit(1);
        }
    } else {
        if let Err(e) = run() {
            eprintln!("Fatal error: {e}");
        }
    }
}
