-- Existing databases: applied automatically by the application when ALTER is permitted.
ALTER TABLE inspection_history ADD COLUMN result_snapshot JSON NULL;
