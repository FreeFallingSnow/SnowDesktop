-- Self wall time is additive within the recorded synchronous hierarchy.
-- CPU remains inclusive; hidden/filtered children forbid precise self CPU.
SELECT module,COUNT(*) AS calls,SUM(self_ns)/1e6 AS selfWallMs,
 NULL AS selfCpuMs FROM snow_scopes GROUP BY module ORDER BY selfWallMs DESC;
