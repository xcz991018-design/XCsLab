-- Apply once after 001_initial.sql and 002_database_images.sql.
-- Full connection/asset/calibration descriptor, with credentials removed.
ALTER TABLE camera_archives ADD COLUMN archive_json JSON NULL;
CREATE TABLE runtime_settings (
    setting_key VARCHAR(64) PRIMARY KEY,
    setting_json JSON NOT NULL
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
-- Content hash prevents creating a new config version for each reading.
ALTER TABLE camera_config_versions
    ADD COLUMN snapshot_sha256 CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NULL,
    ADD UNIQUE KEY uq_camera_snapshot (camera_id, snapshot_sha256);
