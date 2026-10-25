WITH observations AS (
 SELECT s.name,s.ts,EXTRACT_ARG(s.arg_set_id,'debug.operation') AS operation,
 printf('%016x',EXTRACT_ARG(s.arg_set_id,'debug.owner')) AS owner,
 EXTRACT_ARG(s.arg_set_id,'debug.success') AS success
 FROM slice s,snow_window w WHERE s.name IN ('task.queued','task.execute.begin','task.execute.end','task.completed')
 AND s.ts BETWEEN w.start_ns AND w.end_ns
), lifetimes AS (
 SELECT operation,owner,MIN(CASE WHEN name='task.queued' THEN ts END) AS queued,
 MIN(CASE WHEN name='task.execute.begin' THEN ts END) AS began,
 MAX(CASE WHEN name='task.execute.end' THEN ts END) AS ended,
 MAX(CASE WHEN name='task.completed' THEN ts END) AS completed,
 MAX(CASE WHEN name='task.completed' THEN success END) AS success
 FROM observations GROUP BY operation,owner
)
SELECT printf('%016x',operation) AS operation,owner,
 CASE WHEN began>=queued THEN (began-queued)/1e6 END AS queueWaitMs,
 CASE WHEN ended>=began THEN (ended-began)/1e6 END AS executionWallMs,
 CASE WHEN completed>=queued THEN (completed-queued)/1e6 END AS totalWallMs,
 success,CASE WHEN queued IS NULL OR completed IS NULL THEN 'partial' ELSE 'complete' END AS coverage
FROM lifetimes ORDER BY queueWaitMs DESC LIMIT 10000;
