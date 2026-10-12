-- Apply after 001_initial.sql, once, in the same database.
-- Images are separate from history metadata so list queries never load BLOBs.
CREATE TABLE image_blobs (
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

ALTER TABLE inspection_evidence
    MODIFY storage_key VARCHAR(1024) NULL,
    ADD COLUMN storage_backend ENUM('file', 'mysql') NOT NULL DEFAULT 'file',
    ADD COLUMN image_id BIGINT UNSIGNED NULL,
    ADD CONSTRAINT fk_evidence_image FOREIGN KEY (image_id) REFERENCES image_blobs(image_id),
    ADD CONSTRAINT ck_evidence_storage CHECK (
        (storage_backend = 'file' AND storage_key IS NOT NULL AND image_id IS NULL)
        OR (storage_backend = 'mysql' AND storage_key IS NULL AND image_id IS NOT NULL)
    );

-- Retrieval contract: bind task_id as a parameter; never fetch arbitrary BLOBs
-- in the paginated history query. Verify SHA-256 and dimensions before decode.
-- SELECT b.image_id, b.mime_type, b.pixel_width, b.pixel_height,
--        b.sha256, b.byte_size, b.image_bytes
-- FROM inspection_evidence AS e
-- JOIN image_blobs AS b ON b.image_id = e.image_id
-- WHERE e.task_id = ? AND e.evidence_kind = 'original'
--   AND e.sequence_number = 0 AND e.availability = 'available'
--   AND e.storage_backend = 'mysql';
-- Reanalysis references the existing image_id; it must not re-encode the original.
-- Evidence byte_size / sha256 must match the referenced BLOB when inserted.
-- Expiry removes evidence references first; garbage collection deletes only
-- unreferenced image_blobs, including references from historical reanalysis.
