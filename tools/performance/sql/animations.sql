WITH updates AS (
 SELECT ts,EXTRACT_ARG(arg_set_id,'debug.animation') AS animation,
 EXTRACT_ARG(arg_set_id,'debug.effective_ms') AS effective_ms,
 EXTRACT_ARG(arg_set_id,'debug.late_ms') AS late_ms,
 EXTRACT_ARG(arg_set_id,'debug.missed_deadlines') AS missed,
 (ts-LAG(ts) OVER(PARTITION BY EXTRACT_ARG(arg_set_id,'debug.animation') ORDER BY ts))/1e6 AS gap_ms
 FROM slice,snow_window w WHERE name='animation.update' AND ts BETWEEN w.start_ns AND w.end_ns
), activity AS (
 SELECT animation,COUNT(*) AS updates,MIN(ts) AS first_ns,MAX(ts) AS last_ns,
 PERCENTILE(gap_ms,95) AS p95GapMs,PERCENTILE(gap_ms,99) AS p99GapMs,MAX(gap_ms) AS maxPauseMs,
 MAX(late_ms) AS maxLateMs,SUM(missed) AS missedDeadlines,MIN(effective_ms) AS minimumPeriodMs,
 MAX(effective_ms) AS maximumPeriodMs FROM updates GROUP BY animation
)
SELECT printf('%016x',EXTRACT_ARG(l.arg_set_id,'debug.animation')) AS animation,
 EXTRACT_ARG(l.arg_set_id,'debug.type') AS type,EXTRACT_ARG(l.arg_set_id,'debug.surface') AS surface,
 EXTRACT_ARG(l.arg_set_id,'debug.target_ms') AS schedulerTargetPeriodMs,
 CASE WHEN l.dur>=0 THEN l.dur/1e6 END AS lifetimeMs,
 EXTRACT_ARG(l.arg_set_id,'debug.observed') AS observedAfterStart,
 COALESCE(a.updates,0) AS updates,(a.last_ns-a.first_ns)/1e6 AS updateWindowMs,
 CASE WHEN a.updates>=2 AND a.last_ns>a.first_ns THEN (a.updates-1)*1e9/(a.last_ns-a.first_ns) END AS updateFps,
 a.p95GapMs,a.p99GapMs,a.maxPauseMs,a.maxLateMs,a.missedDeadlines,a.minimumPeriodMs,a.maximumPeriodMs,
 NULL AS presentedFps,NULL AS droppedFrames,
 CASE WHEN l.dur<0 OR EXTRACT_ARG(l.arg_set_id,'debug.observed')=1 THEN 'partial' ELSE 'complete' END AS coverage
FROM slice l LEFT JOIN activity a ON EXTRACT_ARG(l.arg_set_id,'debug.animation')=a.animation,
 snow_window w WHERE l.name='animation.lifetime' AND l.ts BETWEEN w.start_ns AND w.end_ns
ORDER BY maxPauseMs DESC LIMIT 10000;
