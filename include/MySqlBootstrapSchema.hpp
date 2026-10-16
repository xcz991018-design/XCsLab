// Base schema for new databases; MySqlTemplateSchema adds migration 004.
#pragma once
inline constexpr const char* MySqlBootstrapSchema = R"SQL(CREATE TABLE IF NOT EXISTS camera_archives (
    camera_id VARCHAR(128) COLLATE utf8mb4_bin PRIMARY KEY,
    archive_json JSON NULL,
    display_name VARCHAR(255) NOT NULL,
    protocol VARCHAR(32) NOT NULL,
    endpoint VARCHAR(1024) NOT NULL COMMENT 'Credential-free address',
    credential_ref VARCHAR(255) NULL,
    substation VARCHAR(255) NULL,
    area VARCHAR(255) NULL,
    installation_location VARCHAR(512) NULL,
    device_tag VARCHAR(255) NULL,
    gauge_type VARCHAR(128) NULL,
    manufacturer VARCHAR(255) NULL,
    accuracy_class VARCHAR(64) NULL,
    enabled BOOLEAN NOT NULL DEFAULT TRUE,
    deleted_at DATETIME(3) NULL,
    created_at DATETIME(3) NOT NULL,
    updated_at DATETIME(3) NOT NULL,
    INDEX idx_camera_location (substation, area)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS camera_config_versions (
    config_id BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    camera_id VARCHAR(128) COLLATE utf8mb4_bin NOT NULL,
    snapshot_sha256 CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NULL,
    UNIQUE KEY uq_camera_snapshot (camera_id, snapshot_sha256),
    version_number BIGINT UNSIGNED NOT NULL,
    calibration JSON NOT NULL,
    recognition_profile JSON NOT NULL,
    archive_snapshot JSON NOT NULL COMMENT 'Exclude passwords and URL credentials',
    created_at DATETIME(3) NOT NULL,
    UNIQUE KEY uq_camera_version (camera_id, version_number),
    UNIQUE KEY uq_config_camera (config_id, camera_id),
    FOREIGN KEY (camera_id) REFERENCES camera_archives(camera_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS inspection_history (
    task_id CHAR(36) CHARACTER SET ascii COLLATE ascii_bin PRIMARY KEY,
    camera_id VARCHAR(128) COLLATE utf8mb4_bin NOT NULL,
    config_id BIGINT UNSIGNED NOT NULL,
    parent_task_id CHAR(36) CHARACTER SET ascii COLLATE ascii_bin NULL,
    input_kind ENUM('live_snapshot', 'local_image', 'historical_image') NOT NULL,
    run_mode ENUM('production', 'evidence', 'debug') NOT NULL,
    captured_at DATETIME(3) NULL COMMENT 'NULL when acquisition fails',
    processed_at DATETIME(3) NOT NULL,
    recognition_performed BOOLEAN NOT NULL,
    recognition_success BOOLEAN NOT NULL,
    reading_value DOUBLE NULL,
    unit VARCHAR(64) NULL,
    confidence DOUBLE NULL,
    quality_status VARCHAR(32) NOT NULL,
    process_status VARCHAR(32) NOT NULL,
    error_code VARCHAR(64) NULL,
    error_message TEXT NULL,
    result_snapshot JSON NULL,
    UNIQUE KEY uq_task_camera (task_id, camera_id),
    INDEX idx_camera_history (camera_id, processed_at, task_id),
    INDEX idx_history_time (processed_at, task_id),
    FOREIGN KEY (camera_id) REFERENCES camera_archives(camera_id),
    FOREIGN KEY (config_id, camera_id) REFERENCES camera_config_versions(config_id, camera_id),
    FOREIGN KEY (parent_task_id, camera_id) REFERENCES inspection_history(task_id, camera_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS image_blobs (
    image_id BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    mime_type VARCHAR(64) NOT NULL,
    pixel_width INT UNSIGNED NOT NULL,
    pixel_height INT UNSIGNED NOT NULL,
    sha256 CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    byte_size BIGINT UNSIGNED NOT NULL,
    image_bytes LONGBLOB NOT NULL,
    created_at DATETIME(3) NOT NULL,
    UNIQUE KEY uq_image_sha256 (sha256),
    CONSTRAINT ck_image_size CHECK (byte_size = OCTET_LENGTH(image_bytes)),
    CONSTRAINT ck_image_dimensions CHECK (pixel_width > 0 AND pixel_height > 0)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;


CREATE TABLE IF NOT EXISTS inspection_evidence (
    task_id CHAR(36) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    evidence_kind ENUM('original', 'rectified', 'recognition', 'debug') NOT NULL,
    sequence_number INT UNSIGNED NOT NULL DEFAULT 0,
    storage_key VARCHAR(1024) NULL,
    storage_backend ENUM('file', 'mysql') NOT NULL DEFAULT 'file',
    image_id BIGINT UNSIGNED NULL,
    CONSTRAINT fk_evidence_image FOREIGN KEY (image_id) REFERENCES image_blobs(image_id),
    CONSTRAINT ck_evidence_storage CHECK ((storage_backend = 'file' AND storage_key IS NOT NULL AND image_id IS NULL) OR (storage_backend = 'mysql' AND storage_key IS NULL AND image_id IS NOT NULL)),
    sha256 CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    byte_size BIGINT UNSIGNED NOT NULL,
    availability ENUM('available', 'missing', 'expired') NOT NULL DEFAULT 'available',
    created_at DATETIME(3) NOT NULL,
    PRIMARY KEY (task_id, evidence_kind, sequence_number),
    FOREIGN KEY (task_id) REFERENCES inspection_history(task_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS reading_reviews (
    review_id BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    task_id CHAR(36) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    reviewer_id VARCHAR(128) NOT NULL,
    decision ENUM('confirmed', 'corrected', 'unreadable') NOT NULL,
    corrected_value DOUBLE NULL,
    comment TEXT NULL,
    reviewed_at DATETIME(3) NOT NULL,
    INDEX idx_task_reviews (task_id, reviewed_at, review_id),
    FOREIGN KEY (task_id) REFERENCES inspection_history(task_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS runtime_settings (setting_key VARCHAR(64) PRIMARY KEY, setting_json JSON NOT NULL) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;)SQL";
