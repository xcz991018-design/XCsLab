#pragma once
inline constexpr const char* MySqlTemplateSchema = R"SQL(
CREATE TABLE IF NOT EXISTS gauge_templates (
    template_id VARCHAR(128) COLLATE utf8mb4_bin PRIMARY KEY,
    definition JSON NOT NULL,
    revision BIGINT UNSIGNED NOT NULL DEFAULT 1,
    created_at DATETIME(3) NOT NULL,
    updated_at DATETIME(3) NOT NULL,
    deleted_at DATETIME(3) NULL,
    CONSTRAINT ck_template_revision CHECK (revision > 0),
    CONSTRAINT ck_template_identity CHECK (JSON_UNQUOTE(JSON_EXTRACT(definition, '$.gauge_id')) = template_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
)SQL";
