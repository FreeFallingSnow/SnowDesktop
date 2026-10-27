SELECT w.start_ns, w.end_ns, (w.end_ns-w.start_ns)/1e6 AS durationMs,
 (SELECT COUNT(*) FROM slice WHERE name='capture.start' AND category='snow.profiler') AS starts,
 (SELECT COUNT(*) FROM slice WHERE name='capture.stop' AND category='snow.profiler') AS stops,
 (SELECT COUNT(*) FROM slice WHERE dur<0 AND category GLOB 'snow.*' AND category!='snow.profiler' AND EXTRACT_ARG(arg_set_id,'debug.scope')=1) AS incompleteSlices,
 (SELECT COUNT(*) FROM slice WHERE dur>=0 AND category GLOB 'snow.*' AND category!='snow.profiler'
  AND EXTRACT_ARG(arg_set_id,'debug.scope')=1 AND (ts<w.start_ns OR ts+dur>w.end_ns)) AS boundarySlices,
 (SELECT COALESCE(SUM(value),0) FROM stats WHERE value>0 AND
   (severity IN ('data_loss','error')
    OR (name GLOB 'traced_buf_*' AND (name GLOB '*discard*' OR name GLOB '*overwrit*'))
    OR name GLOB '*skipped*incremental*')) AS lossOrErrors,
 -- Service-wide counters include writes arriving after older sessions ended.
 -- They are not per-buffer loss; retain them as unattributed information.
 (SELECT COALESCE(SUM(value),0) FROM stats WHERE name='traced_chunks_discarded') AS serviceChunksDiscarded,
 (SELECT COALESCE(SUM(value),0) FROM stats WHERE name='traced_patches_discarded') AS servicePatchesDiscarded,
 (SELECT COUNT(*) FROM snow_scopes WHERE thread_dur IS NULL) AS missingCpuSlices,
 (SELECT COUNT(*) FROM (SELECT 1 FROM snow_scopes GROUP BY module,name,owner)) AS scopeGroups,
 (SELECT COUNT(DISTINCT name) FROM track WHERE INSTR(name,'|')>0) AS counterTracks,
 (SELECT COUNT(*) FROM slice WHERE name='animation.lifetime') AS animations,
 (SELECT COUNT(*) FROM slice WHERE name='task.queued') AS queuedTasks
 ,(SELECT COUNT(*) FROM (SELECT 1 FROM slice WHERE name='metric.event'
 GROUP BY EXTRACT_ARG(arg_set_id,'debug.module'),EXTRACT_ARG(arg_set_id,'debug.metric'),EXTRACT_ARG(arg_set_id,'debug.owner'))) AS eventGroups
FROM snow_window w;
