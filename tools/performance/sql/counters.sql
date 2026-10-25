WITH values_in_window AS (
 SELECT t.name,c.ts,c.value,LAG(c.value) OVER(PARTITION BY c.track_id ORDER BY c.ts,c.id) AS previous
 FROM counter c JOIN track t ON t.id=c.track_id,snow_window w
 WHERE INSTR(t.name,'|')>0 AND c.ts BETWEEN w.start_ns AND w.end_ns
), ranked AS (
 SELECT *,ROW_NUMBER() OVER(PARTITION BY name ORDER BY ts) AS first_rank,
 ROW_NUMBER() OVER(PARTITION BY name ORDER BY ts DESC) AS last_rank FROM values_in_window
)
SELECT name,COUNT(*) AS samples,MIN(ts) AS firstNs,MAX(ts) AS lastNs,
 (MAX(ts)-MIN(ts))/1e6 AS sampleWindowMs,MIN(value) AS minValue,MAX(value) AS peakValue,
 MAX(CASE WHEN first_rank=1 THEN value END) AS firstValue,
 MAX(CASE WHEN last_rank=1 THEN value END) AS lastValue,
 SUM(CASE WHEN previous IS NOT NULL AND value<previous THEN 1 ELSE 0 END) AS decreases
FROM ranked GROUP BY name ORDER BY name;
