SELECT name,severity,source,value,description FROM stats
WHERE value>0 AND (severity IN ('data_loss','error') OR name GLOB '*overrun*' OR name GLOB '*discard*' OR name GLOB '*lost*'
 OR name GLOB '*overwrit*' OR name GLOB '*skipped*incremental*')
ORDER BY name;
