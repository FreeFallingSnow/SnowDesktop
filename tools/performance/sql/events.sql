SELECT EXTRACT_ARG(s.arg_set_id,'debug.module') AS module,
 EXTRACT_ARG(s.arg_set_id,'debug.metric') AS metric,
 printf('%016x',EXTRACT_ARG(s.arg_set_id,'debug.owner')) AS owner,
 COUNT(*) AS events,SUM(EXTRACT_ARG(s.arg_set_id,'debug.value')) AS amount,
 MIN(EXTRACT_ARG(s.arg_set_id,'debug.value')) AS minimum,
 MAX(EXTRACT_ARG(s.arg_set_id,'debug.value')) AS maximum
FROM slice s,snow_window w WHERE s.name='metric.event'
 AND s.ts BETWEEN w.start_ns AND w.end_ns
GROUP BY module,metric,owner ORDER BY events DESC LIMIT 10000;
