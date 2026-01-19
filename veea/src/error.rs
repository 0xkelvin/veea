use thiserror::Error;

pub type AppResult<T> = Result<T, AppError>;

#[derive(Error, Debug)]
pub enum AppError {
    #[error("io error: {0}")]
    Io(#[from] std::io::Error),

    #[error("toml error: {0}")]
    TomlDeserialize(#[from] toml::de::Error),

    #[error("toml serialize error: {0}")]
    TomlSerialize(#[from] toml::ser::Error),

    #[error("database error: {0}")]
    Db(#[from] rusqlite::Error),

    #[error("capture error: {0}")]
    Capture(String),

    #[error("channel error: {0}")]
    Channel(String),
}
