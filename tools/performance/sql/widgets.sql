WITH owned AS (
 SELECT * FROM snow_scopes WHERE owner!='0000000000000000'
 AND (module GLOB 'widget.*' OR module IN ('lua','widget','task.worker'))
), roots AS (
 SELECT s.* FROM owned s WHERE NOT EXISTS (
  SELECT 1 FROM ancestor_slice(s.id) a JOIN owned parent ON parent.id=a.id WHERE parent.owner=s.owner
 )
), cpu AS (
 SELECT owner,COUNT(*) AS rootScopes,SUM(dur)/1e6 AS rootInclusiveWallMs,COUNT(thread_dur) AS rootCpuSamples,
 CASE WHEN COUNT(thread_dur)=COUNT(*) THEN SUM(thread_dur)/1e6 END AS rootInclusiveCpuMs
 FROM roots GROUP BY owner
)
SELECT s.owner,COUNT(*) AS recordedCalls,SUM(s.self_ns)/1e6 AS recordedSelfWallMs,
 c.rootInclusiveWallMs,c.rootInclusiveCpuMs,c.rootScopes,c.rootCpuSamples,NULL AS selfCpuMs
FROM owned s LEFT JOIN cpu c USING(owner) GROUP BY s.owner ORDER BY recordedSelfWallMs DESC LIMIT 4096;
