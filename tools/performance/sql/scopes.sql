SELECT module,name AS phase,owner,COUNT(*) AS calls,SUM(dur)/1e6 AS wallMs,
 SUM(self_ns)/1e6 AS selfWallMs,MAX(dur)/1e6 AS maxWallMs,
 PERCENTILE(dur,50)/1e6 AS p50WallMs,PERCENTILE(dur,95)/1e6 AS p95WallMs,
 PERCENTILE(dur,99)/1e6 AS p99WallMs,
 CASE WHEN COUNT(cpu_ns)=COUNT(*) THEN SUM(cpu_ns)/1e6 END AS inclusiveCpuMs,
 CASE WHEN COUNT(cpu_cycles)=COUNT(*) THEN SUM(cpu_cycles) END AS inclusiveCpuCycles,
 NULL AS selfCpuMs,COUNT(cpu_ns) AS cpuSamples
FROM snow_scopes GROUP BY module,name,owner ORDER BY selfWallMs DESC LIMIT 10000;
