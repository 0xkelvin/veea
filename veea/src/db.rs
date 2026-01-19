use std::path::{Path, PathBuf};

use chrono::{DateTime, Utc};
use rusqlite::{params, Connection};
use chrono::Duration;

use crate::error::AppResult;

#[derive(Debug, Clone)]
pub struct CaptureRecord {
    pub id: String,
    pub ts: DateTime<Utc>,
    pub window_title: Option<String>,
    pub app_name: Option<String>,
    pub event_type: String,
    pub path: String,
    pub width: Option<u32>,
    pub height: Option<u32>,
    pub monitor: Option<String>,
    pub hash: Option<String>,
}

pub struct Db {
    path: PathBuf,
    conn: Connection,
}

impl Db {
    pub fn new(path: &Path) -> AppResult<Self> {
        if let Some(parent) = path.parent() {
            std::fs::create_dir_all(parent)?;
        }
        let conn = Connection::open(path)?;
        let db = Self {
            path: path.to_path_buf(),
            conn,
        };
        db.init()?;
        Ok(db)
    }

    fn init(&self) -> AppResult<()> {
        self.conn.execute_batch(
            r#"
            CREATE TABLE IF NOT EXISTS captures (
                id TEXT PRIMARY KEY,
                ts INTEGER NOT NULL,
                window_title TEXT,
                app_name TEXT,
                event_type TEXT NOT NULL,
                path TEXT NOT NULL,
                width INTEGER,
                height INTEGER,
                monitor TEXT,
                hash TEXT,
                deleted INTEGER DEFAULT 0
            );
            CREATE INDEX IF NOT EXISTS captures_ts_idx ON captures(ts);
        "#,
        )?;
        Ok(())
    }

    pub fn insert_capture(&self, record: &CaptureRecord) -> AppResult<()> {
        self.conn.execute(
            r#"
            INSERT INTO captures (
                id, ts, window_title, app_name, event_type, path,
                width, height, monitor, hash, deleted
            ) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, 0)
            "#,
            params![
                record.id,
                record.ts.timestamp_millis(),
                record.window_title,
                record.app_name,
                record.event_type,
                record.path,
                record.width.map(|w| w as i64),
                record.height.map(|h| h as i64),
                record.monitor,
                record.hash,
            ],
        )?;
        Ok(())
    }

    pub fn connection_path(&self) -> PathBuf {
        self.path.clone()
    }

    pub fn open_reader(&self) -> AppResult<Connection> {
        Connection::open(&self.path).map_err(Into::into)
    }

    pub fn list_recent(&self, limit: usize) -> AppResult<Vec<CaptureRecord>> {
        let conn = self.open_reader()?;
        let mut stmt = conn.prepare(
            "SELECT id, ts, window_title, app_name, event_type, path, width, height, monitor, hash
             FROM captures
             WHERE deleted = 0
             ORDER BY ts DESC
             LIMIT ?1",
        )?;

        let rows = stmt.query_map([limit as u32], |row| {
            Ok(CaptureRecord {
                id: row.get(0)?,
                ts: DateTime::<Utc>::from_timestamp_millis(row.get::<_, i64>(1)?)
                    .unwrap_or_else(Utc::now),
                window_title: row.get(2)?,
                app_name: row.get(3)?,
                event_type: row.get(4)?,
                path: row.get(5)?,
                width: row.get::<_, Option<i64>>(6)?.map(|v| v as u32),
                height: row.get::<_, Option<i64>>(7)?.map(|v| v as u32),
                monitor: row.get(8)?,
                hash: row.get(9)?,
            })
        })?;

        let mut results = Vec::new();
        for row in rows {
            results.push(row?);
        }
        Ok(results)
    }

    pub fn get_capture(&self, id: &str) -> AppResult<Option<CaptureRecord>> {
        let conn = self.open_reader()?;
        let mut stmt = conn.prepare(
            "SELECT id, ts, window_title, app_name, event_type, path, width, height, monitor, hash
             FROM captures
             WHERE id = ?1 AND deleted = 0
             LIMIT 1",
        )?;

        let mut rows = stmt.query([id])?;
        if let Some(row) = rows.next()? {
            let record = CaptureRecord {
                id: row.get(0)?,
                ts: DateTime::<Utc>::from_timestamp_millis(row.get::<_, i64>(1)?)
                    .unwrap_or_else(Utc::now),
                window_title: row.get(2)?,
                app_name: row.get(3)?,
                event_type: row.get(4)?,
                path: row.get(5)?,
                width: row.get::<_, Option<i64>>(6)?.map(|v| v as u32),
                height: row.get::<_, Option<i64>>(7)?.map(|v| v as u32),
                monitor: row.get(8)?,
                hash: row.get(9)?,
            };
            return Ok(Some(record));
        }

        Ok(None)
    }

    pub fn delete_recent(&self, minutes: i64) -> AppResult<usize> {
        let conn = Connection::open(&self.path)?;
        let threshold = (Utc::now() - Duration::minutes(minutes)).timestamp_millis();

        let mut stmt = conn.prepare(
            "SELECT id, path FROM captures WHERE ts >= ?1 AND deleted = 0",
        )?;

        let rows = stmt.query_map([threshold], |row| {
            Ok((row.get::<_, String>(0)?, row.get::<_, String>(1)?))
        })?;

        let mut deleted = 0;
        for row in rows {
            let (id, path) = row?;
            let _ = std::fs::remove_file(&path);
            conn.execute("UPDATE captures SET deleted = 1 WHERE id = ?1", [id])?;
            deleted += 1;
        }

        Ok(deleted)
    }
}
