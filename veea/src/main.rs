use fs_extra::dir;
use xcap::{Monitor, Window};
use std::sync::mpsc;
use std::thread;
use std::time::{Duration, SystemTime, UNIX_EPOCH};

#[derive(Debug, Clone)]
enum WindowEvent {
    FocusChanged { window_title: String },
    TitleChanged { window_title: String },
}

fn normalized(filename: String) -> String {
    filename.replace(['|', '\\', ':', '/', '<', '>', '"', '?', '*'], "_")
}

fn get_focused_window() -> Option<(u32, String)> {
    if let Ok(windows) = Window::all() {
        // Try to find the frontmost visible window
        // On macOS, windows are typically returned in z-order, so the first visible one is likely focused
        for window in windows {
            if let Ok(is_minimized) = window.is_minimized() {
                if !is_minimized {
                    if let Ok(title) = window.title() {
                        // Skip empty titles (they're usually background windows)
                        if !title.is_empty() {
                            if let Ok(window_id) = window.id() {
                                // Return the first candidate as it's likely the frontmost
                                // On macOS, Window::all() typically returns windows in z-order
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

fn take_screenshot(window_title: &str, event_type: &str) -> Result<(), Box<dyn std::error::Error>> {
    let timestamp = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap()
        .as_nanos();
    let safe_title = normalized(window_title.to_string());
    let filename = format!("target/screenshots/{}_{}_{}.png", event_type, safe_title, timestamp);
    
    // Ensure directory exists
    dir::create_all("target/screenshots", true)?;

    // Try to capture the focused window first
    if let Ok(windows) = Window::all() {
        for window in windows {
            if let Ok(title) = window.title() {
                if title == window_title {
                    if let Ok(image) = window.capture_image() {
                        image.save(&filename)?;
                        println!("Screenshot saved: {} (Window: {})", filename, window_title);
                        return Ok(());
                    }
                }
            }
        }
    }

    // Fallback to monitor capture
    let monitors = Monitor::all()?;
    if let Some(monitor) = monitors.first() {
        let image = monitor.capture_image()?;
        image.save(&filename)?;
        println!("Screenshot saved: {} (Monitor fallback for: {})", filename, window_title);
    }

    Ok(())
}

fn monitor_window_events(event_sender: mpsc::Sender<WindowEvent>) {
    let mut last_focused_window_id: Option<u32> = None;
    let mut last_window_title: Option<String> = None;

    loop {
        if let Some((window_id, window_title)) = get_focused_window() {
            // Check for focus change
            if last_focused_window_id != Some(window_id) {
                let _ = event_sender.send(WindowEvent::FocusChanged {
                    window_title: window_title.clone(),
                });
                last_focused_window_id = Some(window_id);
            }

            // Check for title change (same window, different title)
            if last_focused_window_id == Some(window_id) {
                if last_window_title.as_ref() != Some(&window_title) {
                    let _ = event_sender.send(WindowEvent::TitleChanged {
                        window_title: window_title.clone(),
                    });
                }
            }

            last_window_title = Some(window_title);
        } else {
            // No focused window, reset tracking
            if last_focused_window_id.is_some() {
                last_focused_window_id = None;
                last_window_title = None;
            }
        }

        thread::sleep(Duration::from_millis(200));
    }
}

fn main() {
    println!("Starting window event monitor...");
    println!("Screenshots will be saved to target/screenshots/");
    
    // Create directory for screenshots
    dir::create_all("target/screenshots", true).unwrap();

    let (tx, rx) = mpsc::channel();
    
    // Start monitoring in a separate thread
    thread::spawn(move || {
        monitor_window_events(tx);
    });

    println!("Monitoring window events... (Press Ctrl+C to stop)");

    // Process events
    loop {
        match rx.recv() {
            Ok(WindowEvent::FocusChanged { window_title }) => {
                println!("Focus changed to: {}", window_title);
                if let Err(e) = take_screenshot(&window_title, "focus") {
                    eprintln!("Error taking screenshot: {}", e);
                }
            }
            Ok(WindowEvent::TitleChanged { window_title }) => {
                println!("Title changed to: {}", window_title);
                if let Err(e) = take_screenshot(&window_title, "title") {
                    eprintln!("Error taking screenshot: {}", e);
                }
            }
            Err(e) => {
                eprintln!("Error receiving event: {}", e);
                break;
            }
        }
    }
}
