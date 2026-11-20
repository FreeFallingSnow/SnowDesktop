WITH windows(scenario,repetition,edge,surface,start_ns,ack_ns,end_ns,confirmed) AS (
    VALUES {{WINDOWS}}
), capture AS (
    SELECT MIN(CASE name WHEN 'capture.start' THEN ts END) AS first_ns,
           MAX(CASE name WHEN 'capture.stop' THEN ts END) AS last_ns
    FROM slice WHERE category='snow.profiler'
), tracks AS (
    SELECT w.scenario,w.repetition,w.edge,s.ts AS opened_ns,s.ts+s.dur AS closed_ns,
           EXTRACT_ARG(s.arg_set_id,'debug.animation') AS animation
    FROM windows w JOIN slice s ON s.ts BETWEEN w.start_ns AND w.end_ns
    WHERE s.name='animation.lifetime' AND s.dur>=0 AND s.ts+s.dur<=w.end_ns
      AND EXTRACT_ARG(s.arg_set_id,'debug.observed')=0
      AND EXTRACT_ARG(s.arg_set_id,'debug.surface')=w.surface
), updates AS (
    SELECT t.scenario,t.repetition,t.edge,
           EXTRACT_ARG(s.arg_set_id,'debug.effective_ms') AS effective_ms,
           EXTRACT_ARG(s.arg_set_id,'debug.missed_deadlines') AS missed,
           (s.ts-LAG(s.ts) OVER (PARTITION BY t.animation ORDER BY s.ts))/1e6 AS gap_ms
    FROM tracks t JOIN slice s ON s.ts BETWEEN t.opened_ns AND t.closed_ns
    WHERE s.name='animation.update' AND EXTRACT_ARG(s.arg_set_id,'debug.animation')=t.animation
), timing AS (
    SELECT scenario,repetition,edge,COUNT(*) AS updates,COUNT(gap_ms) AS gaps,
           ROUND(AVG(effective_ms),3) AS requestedPeriodMs,
           ROUND(PERCENTILE(gap_ms,95),3) AS p95GapMs,
           ROUND(MAX(gap_ms),3) AS maxGapMs,SUM(missed) AS missedDeadlines,
           SUM(CASE WHEN gap_ms > 1.5*effective_ms THEN 1 ELSE 0 END) AS longGaps
    FROM updates GROUP BY scenario,repetition,edge
), lifetimes AS (
    SELECT scenario,repetition,edge,COUNT(*) AS tracks,
           ROUND(MAX(closed_ns-opened_ns)/1e6,3) AS longestTrackMs
    FROM tracks GROUP BY scenario,repetition,edge
), commits AS (
    SELECT w.scenario,w.repetition,w.edge,COUNT(s.id) AS sharedCommits,
           ROUND(PERCENTILE(EXTRACT_ARG(s.arg_set_id,'debug.duration_ms'),95),3) AS p95CommitMs
    FROM windows w LEFT JOIN slice s ON s.name='animation.commit' AND s.ts BETWEEN w.start_ns AND w.end_ns
    GROUP BY w.scenario,w.repetition,w.edge
)
SELECT w.scenario,w.repetition,w.edge,w.surface,w.confirmed,
       CASE WHEN w.start_ns>=c.first_ns AND w.end_ns<=c.last_ns THEN 1 ELSE 0 END AS windowCovered,
       ROUND((w.ack_ns-w.start_ns)/1e6,3) AS triggerMs,
       COALESCE(l.tracks,0) AS uiTracks,l.longestTrackMs,
       COALESCE(t.updates,0) AS uiUpdates,COALESCE(t.gaps,0) AS intervalSamples,
       t.requestedPeriodMs,t.p95GapMs,t.maxGapMs,t.longGaps,t.missedDeadlines,
       k.sharedCommits,k.p95CommitMs
FROM windows w CROSS JOIN capture c
LEFT JOIN timing t USING(scenario,repetition,edge)
LEFT JOIN lifetimes l USING(scenario,repetition,edge)
LEFT JOIN commits k USING(scenario,repetition,edge)
ORDER BY w.start_ns;
