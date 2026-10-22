-- Application roles and accounts. Password hashes are PBKDF2-HMAC-SHA256,
-- 120000 iterations; salt and hash are hexadecimal. No default password.
CREATE TABLE IF NOT EXISTS application_roles (
    role_code VARCHAR(32) PRIMARY KEY,
    display_name VARCHAR(64) NOT NULL
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
INSERT IGNORE INTO application_roles VALUES ('administrator','管理员'),('operator','操作员');
CREATE TABLE IF NOT EXISTS application_accounts (
    username VARCHAR(64) PRIMARY KEY,
    role_code VARCHAR(32) NOT NULL,
    salt VARCHAR(64) NOT NULL,
    password_hash VARCHAR(64) NOT NULL,
    FOREIGN KEY(role_code) REFERENCES application_roles(role_code)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
