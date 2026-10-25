SELECT name,severity,source,value,description,
 CASE WHEN name IN ('traced_chunks_discarded','traced_patches_discarded')
 THEN 'service_lifetime_unattributed' ELSE 'current_trace' END AS scope
FROM stats
WHERE value>0 AND (severity IN ('data_loss','error')
 OR (name GLOB 'traced_buf_*' AND (name GLOB '*discard*' OR name GLOB '*overwrit*'))
 OR name GLOB '*skipped*incremental*' OR name IN ('traced_chunks_discarded','traced_patches_discarded'))
ORDER BY name;
