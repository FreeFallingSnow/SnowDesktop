WITH points AS (
 SELECT EXTRACT_ARG(arg_set_id,'debug.target') AS target,
 EXTRACT_ARG(arg_set_id,'debug.present_qpc_ms') AS present_ms,
 EXTRACT_ARG(arg_set_id,'debug.present_count') AS presents,
 EXTRACT_ARG(arg_set_id,'debug.vblank_ms') AS vblank_ms
 FROM slice,snow_window w WHERE name='compositor.present' AND ts BETWEEN w.start_ns AND w.end_ns
), intervals AS (
 SELECT *,present_ms-LAG(present_ms) OVER(PARTITION BY target ORDER BY present_ms) AS gap_ms,
 presents-LAG(presents) OVER(PARTITION BY target ORDER BY present_ms) AS present_delta FROM points
)
SELECT printf('%016x',target) AS target,COUNT(*) AS samples,MAX(present_ms)-MIN(present_ms) AS presentWindowMs,
 CASE WHEN COUNT(*)>=2 AND MAX(present_ms)>MIN(present_ms) AND MIN(present_delta)>=0
 THEN SUM(present_delta)*1000.0/(MAX(present_ms)-MIN(present_ms)) END AS composedDisplayFps,
 PERCENTILE(gap_ms,95) AS p95PresentGapMs,PERCENTILE(gap_ms,99) AS p99PresentGapMs,MAX(gap_ms) AS maxPresentGapMs,
 MIN(vblank_ms) AS minimumVblankMs,MAX(vblank_ms) AS maximumVblankMs,
 NULL AS animationPresentedFps,NULL AS droppedFrames,'display_composition_only' AS coverage
FROM intervals GROUP BY target ORDER BY target LIMIT 16;
