-- Self wall time is additive within the recorded synchronous hierarchy.
-- CPU remains inclusive; hidden/filtered children forbid precise self CPU.
WITH roots AS (
 SELECT s.* FROM snow_scopes s WHERE NOT EXISTS (
  SELECT 1 FROM ancestor_slice(s.id) a JOIN snow_scopes parent ON parent.id=a.id WHERE parent.module=s.module
 )
), cpu AS (
 SELECT module,COUNT(*) AS rootScopes,COUNT(cpu_ns) AS rootCpuSamples,
 CASE WHEN COUNT(cpu_ns)=COUNT(*) THEN SUM(cpu_ns)/1e6 END AS rootInclusiveCpuMs,
 CASE WHEN COUNT(cpu_cycles)=COUNT(*) THEN SUM(cpu_cycles) END AS rootCpuCycles
 FROM roots GROUP BY module
)
SELECT s.module,COUNT(*) AS calls,SUM(s.self_ns)/1e6 AS selfWallMs,
 NULL AS selfCpuMs,c.rootInclusiveCpuMs,c.rootCpuCycles,c.rootScopes,c.rootCpuSamples
FROM snow_scopes s LEFT JOIN cpu c USING(module) GROUP BY s.module ORDER BY selfWallMs DESC;
