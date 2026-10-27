if (-not ('SnowDesktop.Performance.BoundedQuery' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Diagnostics;
using System.IO;
using System.Text;
using System.Threading.Tasks;
namespace SnowDesktop.Performance {
    public static class BoundedQuery {
        static void Copy(StreamReader input, string path, int maximum) {
            using (var output = new FileStream(path, FileMode.CreateNew, FileAccess.Write, FileShare.Read)) {
                var bytes = new byte[8192];
                int count, total = 0;
                while ((count = input.BaseStream.Read(bytes, 0, bytes.Length)) != 0) {
                    if (count > maximum - total) throw new InvalidDataException("analysisStreamLimit");
                    output.Write(bytes, 0, count); total += count;
                }
            }
        }
        public static void Run(string executable, string arguments, string csv, string log) {
            var start = new ProcessStartInfo(executable, arguments) {
                UseShellExecute = false, CreateNoWindow = true,
                RedirectStandardOutput = true, RedirectStandardError = true,
                StandardOutputEncoding = new UTF8Encoding(false, true),
                StandardErrorEncoding = new UTF8Encoding(false, true)
            };
            using (var process = Process.Start(start)) {
                var output = Task.Run(() => Copy(process.StandardOutput, csv + ".partial", 4 * 1024 * 1024));
                var errors = Task.Run(() => Copy(process.StandardError, log + ".partial", 256 * 1024));
                var watch = Stopwatch.StartNew();
                try {
                    while (!process.WaitForExit(100)) {
                        if (output.IsFaulted || errors.IsFaulted) throw new InvalidDataException("analysisStreamLimit");
                        if (watch.ElapsedMilliseconds > 60000) throw new TimeoutException("analysisTimeout");
                    }
                    if (!Task.WaitAll(new Task[] {output, errors}, 5000)) throw new TimeoutException("analysisStreamTimeout");
                    if (process.ExitCode != 0) throw new InvalidDataException("analysisQueryFailed");
                    File.Move(csv + ".partial", csv); File.Move(log + ".partial", log);
                }
                catch {
                    if (!process.HasExited) { process.Kill(); process.WaitForExit(5000); }
                    try { Task.WaitAll(new Task[] {output, errors}, 5000); } catch {}
                    throw;
                }
            }
        }
    }
}
'@
}
