use std::path::{Path, PathBuf};

use rusqlite::{params, Connection};

use crate::{
    db::CaptureRecord,
    error::AppResult,
};

#[derive(Clone)]
pub struct SearchIndex {
    db_path: PathBuf,
}

#[derive(serde::Serialize)]
pub struct SearchHit {
    pub id: String,
    pub ts: i64,
    pub window_title: Option<String>,
    pub app_name: Option<String>,
    pub event_type: String,
    pub path: String,
}

impl SearchIndex {
    pub fn new(db_path: &Path) -> AppResult<Self> {
        Ok(Self {
            db_path: db_path.to_path_buf(),
        })
    }

    pub fn add_capture(&self, _record: &CaptureRecord, _ocr_text: Option<&str>) -> AppResult<()> {
        // With a SQLite-backed search, the primary table already stores the fields
        // we search on. OCR text can be added later via an auxiliary table.
        Ok(())
    }

    pub fn search(&self, query: &str, limit: usize) -> AppResult<Vec<SearchHit>> {
        let conn = Connection::open(&self.db_path)?;
        let pattern = format!("%{}%", query);
        let mut stmt = conn.prepare(
            r#"
            SELECT id, ts, window_title, app_name, event_type, path
            FROM captures
            WHERE deleted = 0
              AND (window_title LIKE ?1 OR app_name LIKE ?1)
            ORDER BY ts DESC
            LIMIT ?2
            "#,
        )?;

        let rows = stmt.query_map(params![pattern, limit as i64], |row| {
            Ok(SearchHit {
                id: row.get(0)?,
                ts: row.get::<_, i64>(1)?,
                window_title: row.get(2)?,
                app_name: row.get(3)?,
                event_type: row.get(4)?,
                path: row.get(5)?,
            })
        })?;

        let mut out = Vec::new();
        for r in rows {
            out.push(r?);
        }
        Ok(out)
    }

    pub fn index_path(&self) -> PathBuf {
        self.db_path.clone()
    }
}
