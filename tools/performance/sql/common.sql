-- queryVersion 1; schema reviewed against Perfetto v58.2.
INCLUDE PERFETTO MODULE slices.self_dur;
CREATE PERFETTO TABLE snow_window AS
SELECT (SELECT MIN(ts) FROM slice WHERE name='capture.start' AND category='snow.profiler') AS start_ns,
       (SELECT MAX(ts) FROM slice WHERE name='capture.stop' AND category='snow.profiler') AS end_ns;
CREATE PERFETTO TABLE snow_scopes AS
SELECT s.*, COALESCE(EXTRACT_ARG(s.arg_set_id,'debug.module'),s.category) AS module,
       printf('%016x',EXTRACT_ARG(s.arg_set_id,'debug.owner')) AS owner,
       d.self_dur AS self_ns
FROM slice s JOIN slice_self_dur d USING(id), snow_window w
WHERE s.dur >= 0 AND s.ts >= w.start_ns AND s.ts+s.dur <= w.end_ns
  AND s.category GLOB 'snow.*' AND s.category != 'snow.profiler';
