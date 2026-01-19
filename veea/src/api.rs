use std::{
    net::SocketAddr,
    path::PathBuf,
    sync::{
        atomic::{AtomicBool, Ordering},
        Arc,
    },
};

use axum::{
    extract::{Path, Query, State},
    http::StatusCode,
    response::{Html, IntoResponse, Response},
    routing::get,
    Json, Router,
};
use serde::Deserialize;
use tokio::fs;

use crate::{
    config::CaptureConfig,
    db::{CaptureRecord, Db},
    error::AppResult,
};

#[derive(Clone)]
pub struct ApiState {
    pub db_path: PathBuf,
    pub config: CaptureConfig,
    pub search_index_path: PathBuf,
    pub pause_flag: Arc<AtomicBool>,
}

#[derive(Debug, Deserialize)]
pub struct ListParams {
    pub limit: Option<usize>,
}

#[derive(Debug, Deserialize)]
pub struct SearchParams {
    pub q: String,
    pub limit: Option<usize>,
}

pub async fn serve(addr: SocketAddr, state: ApiState) -> AppResult<()> {
    let app = Router::new()
        .route("/captures", get(list_captures))
        .route("/captures/:id", get(get_capture))
        .route("/captures/:id/image", get(get_image))
        .route("/config", get(get_config))
        .route("/search", get(search_captures))
        .route("/control/pause", axum::routing::post(pause))
        .route("/control/resume", axum::routing::post(resume))
        .route("/control/erase", axum::routing::post(erase_recent))
        .route("/", get(index_page))
        .with_state(state);

    let listener = tokio::net::TcpListener::bind(addr)
        .await
        .map_err(|e| crate::error::AppError::Capture(e.to_string()))?;

    axum::serve(listener, app)
        .await
        .map_err(|e| crate::error::AppError::Capture(e.to_string()))?;

    Ok(())
}

async fn list_captures(
    State(state): State<ApiState>,
    Query(params): Query<ListParams>,
) -> Response {
    let limit = params.limit.unwrap_or(50).clamp(1, 500);
    match Db::new(&state.db_path)
        .and_then(|db| db.list_recent(limit))
        .map(|rows| rows.into_iter().map(CaptureSummary::from).collect::<Vec<_>>())
    {
        Ok(list) => Json(list).into_response(),
        Err(e) => (
            StatusCode::INTERNAL_SERVER_ERROR,
            format!("error listing captures: {e}"),
        )
            .into_response(),
    }
}

async fn get_capture(State(state): State<ApiState>, Path(id): Path<String>) -> Response {
    match Db::new(&state.db_path).and_then(|db| db.get_capture(&id)) {
        Ok(Some(record)) => Json(CaptureSummary::from(record)).into_response(),
        Ok(None) => (StatusCode::NOT_FOUND, "not found").into_response(),
        Err(e) => (
            StatusCode::INTERNAL_SERVER_ERROR,
            format!("error fetching capture: {e}"),
        )
            .into_response(),
    }
}

async fn get_config(State(state): State<ApiState>) -> Response {
    Json(state.config).into_response()
}

async fn search_captures(
    State(state): State<ApiState>,
    Query(params): Query<SearchParams>,
) -> Response {
    let limit = params.limit.unwrap_or(20).clamp(1, 200);
    match crate::search::SearchIndex::new(&state.search_index_path)
        .and_then(|index| index.search(&params.q, limit))
    {
        Ok(results) => Json(results).into_response(),
        Err(e) => (
            StatusCode::INTERNAL_SERVER_ERROR,
            format!("search error: {e}"),
        )
            .into_response(),
    }
}

async fn get_image(State(state): State<ApiState>, Path(id): Path<String>) -> Response {
    match Db::new(&state.db_path).and_then(|db| db.get_capture(&id)) {
        Ok(Some(record)) => match fs::read(record.path).await {
            Ok(bytes) => (
                StatusCode::OK,
                [("content-type", "image/png")],
                bytes,
            )
                .into_response(),
            Err(e) => (
                StatusCode::INTERNAL_SERVER_ERROR,
                format!("read image failed: {e}"),
            )
                .into_response(),
        },
        Ok(None) => (StatusCode::NOT_FOUND, "not found").into_response(),
        Err(e) => (
            StatusCode::INTERNAL_SERVER_ERROR,
            format!("error fetching capture: {e}"),
        )
            .into_response(),
    }
}

async fn pause(State(state): State<ApiState>) -> Response {
    state.pause_flag.store(true, Ordering::Relaxed);
    (StatusCode::OK, "paused").into_response()
}

async fn resume(State(state): State<ApiState>) -> Response {
    state.pause_flag.store(false, Ordering::Relaxed);
    (StatusCode::OK, "resumed").into_response()
}

#[derive(Debug, Deserialize)]
pub struct EraseParams {
    pub minutes: Option<i64>,
}

async fn erase_recent(
    State(state): State<ApiState>,
    Query(params): Query<EraseParams>,
) -> Response {
    let minutes = params.minutes.unwrap_or(5).clamp(1, 240);
    match Db::new(&state.db_path).and_then(|db| db.delete_recent(minutes)) {
        Ok(count) => Json(serde_json::json!({ "deleted": count })).into_response(),
        Err(e) => (
            StatusCode::INTERNAL_SERVER_ERROR,
            format!("erase failed: {e}"),
        )
            .into_response(),
    }
}

async fn index_page() -> Html<&'static str> {
    const HTML: &str = r#"<!doctype html>
<html>
  <head>
    <meta charset="utf-8" />
    <title>Veea Timeline</title>
    <style>
      body { font-family: sans-serif; margin: 16px; }
      .grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(220px, 1fr)); gap: 12px; }
      .card { border: 1px solid #ccc; padding: 8px; border-radius: 6px; }
      img { max-width: 100%; }
      .controls { margin-bottom: 12px; display: flex; gap: 8px; }
    </style>
  </head>
  <body>
    <h1>Veea Timeline</h1>
    <div class="controls">
      <input id="searchBox" placeholder="Search title/app" />
      <button onclick="doSearch()">Search</button>
      <button onclick="loadCaptures()">Refresh</button>
      <button onclick="togglePause()" id="pauseBtn">Pause</button>
    </div>
    <div id="status"></div>
    <div class="grid" id="grid"></div>
    <script>
      let paused = false;
      async function loadCaptures() {
        const res = await fetch('/captures?limit=40');
        const data = await res.json();
        render(data);
      }
      async function doSearch() {
        const q = document.getElementById('searchBox').value;
        if (!q) return loadCaptures();
        const res = await fetch('/search?q=' + encodeURIComponent(q));
        const data = await res.json();
        render(data);
      }
      async function togglePause() {
        paused = !paused;
        const endpoint = paused ? '/control/pause' : '/control/resume';
        await fetch(endpoint, { method: 'POST' });
        document.getElementById('pauseBtn').innerText = paused ? 'Resume' : 'Pause';
      }
      function render(list) {
        const grid = document.getElementById('grid');
        grid.innerHTML = '';
        for (const item of list) {
          const div = document.createElement('div');
          div.className = 'card';
          div.innerHTML = `
            <div>${new Date(item.ts).toLocaleString()}</div>
            <div><strong>${item.event_type}</strong></div>
            <div>${item.window_title || ''}</div>
            <img src="/captures/${item.id}/image" />
          `;
          grid.appendChild(div);
        }
        document.getElementById('status').innerText = list.length + ' items';
      }
      loadCaptures();
    </script>
  </body>
</html>
"#;
    Html(HTML)
}

#[derive(serde::Serialize)]
struct CaptureSummary {
    id: String,
    ts: i64,
    window_title: Option<String>,
    app_name: Option<String>,
    event_type: String,
    path: String,
    width: Option<u32>,
    height: Option<u32>,
    monitor: Option<String>,
}

impl From<CaptureRecord> for CaptureSummary {
    fn from(record: CaptureRecord) -> Self {
        Self {
            id: record.id,
            ts: record.ts.timestamp_millis(),
            window_title: record.window_title,
            app_name: record.app_name,
            event_type: record.event_type,
            path: record.path,
            width: record.width,
            height: record.height,
            monitor: record.monitor,
        }
    }
}
