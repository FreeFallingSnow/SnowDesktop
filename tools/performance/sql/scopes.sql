SELECT module,name AS phase,owner,COUNT(*) AS calls,SUM(dur)/1e6 AS wallMs,
 SUM(self_ns)/1e6 AS selfWallMs,MAX(dur)/1e6 AS maxWallMs,
 PERCENTILE(dur,50)/1e6 AS p50WallMs,PERCENTILE(dur,95)/1e6 AS p95WallMs,
 PERCENTILE(dur,99)/1e6 AS p99WallMs,
 CASE WHEN COUNT(thread_dur)=COUNT(*) THEN SUM(thread_dur)/1e6 END AS inclusiveCpuMs,
 NULL AS selfCpuMs,COUNT(thread_dur) AS cpuSamples
FROM snow_scopes GROUP BY module,name,owner ORDER BY selfWallMs DESC LIMIT 10000;
