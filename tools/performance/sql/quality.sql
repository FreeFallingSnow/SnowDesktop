SELECT w.start_ns, w.end_ns, (w.end_ns-w.start_ns)/1e6 AS durationMs,
 (SELECT COUNT(*) FROM slice WHERE name='capture.start' AND category='snow.profiler') AS starts,
 (SELECT COUNT(*) FROM slice WHERE name='capture.stop' AND category='snow.profiler') AS stops,
 (SELECT COUNT(*) FROM slice WHERE dur<0 AND category GLOB 'snow.*') AS incompleteSlices,
 (SELECT COUNT(*) FROM slice WHERE dur>=0 AND category GLOB 'snow.*' AND (ts<w.start_ns OR ts+dur>w.end_ns)) AS boundarySlices,
 (SELECT COALESCE(SUM(value),0) FROM stats WHERE value>0 AND
   (severity IN ('data_loss','error') OR name GLOB '*overrun*' OR name GLOB '*discard*' OR name GLOB '*lost*'
    OR name GLOB '*overwrit*' OR name GLOB '*skipped*incremental*')) AS lossOrErrors,
 (SELECT COUNT(*) FROM snow_scopes WHERE thread_dur IS NULL) AS missingCpuSlices
FROM snow_window w;
