// A private job contains only the build/test process tree. Closing its last
// handle, including when the coordinator crashes, terminates that tree.
using System;
using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Diagnostics;
using System.Globalization;

namespace SnowDesktop.Build
{
    public static class Job
    {
        [StructLayout(LayoutKind.Sequential)]
        struct Security { public int size; public IntPtr descriptor; public int inherit; }
        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        struct Startup
        {
            public int size; public string reserved, desktop, title;
            public int x, y, width, height, charsX, charsY, fill, flags;
            public short show, reservedSize; public IntPtr reservedData, input, output, error;
        }
        [StructLayout(LayoutKind.Sequential)]
        struct ExtendedStartup { public Startup startup; public IntPtr attributes; }
        [StructLayout(LayoutKind.Sequential)]
        struct ProcessInfo { public IntPtr process, thread; public int pid, tid; }
        [StructLayout(LayoutKind.Sequential)]
        struct BasicLimits
        {
            public long processTime, jobTime; public uint flags;
            public UIntPtr minWorkingSet, maxWorkingSet; public uint activeProcesses;
            public UIntPtr affinity; public uint priority, scheduling;
        }
        [StructLayout(LayoutKind.Sequential)]
        struct IoCounters { public ulong readOps, writeOps, otherOps, readBytes, writeBytes, otherBytes; }
        [StructLayout(LayoutKind.Sequential)]
        struct Limits
        {
            public BasicLimits basic; public IoCounters io;
            public UIntPtr processMemory, jobMemory, peakProcessMemory, peakJobMemory;
        }
        [StructLayout(LayoutKind.Sequential)]
        struct Accounting
        {
            public long userTime, kernelTime, periodUserTime, periodKernelTime;
            public uint pageFaults, totalProcesses, activeProcesses, terminatedProcesses;
            public IoCounters io;
        }
        static string cancelPath, cancelNonce, metricsPath;
        public static bool RunWasCancelled { get; private set; }
        public static void ConfigureRunControl(string cancellationPath, string nonce, string resourcePath)
        {
            if (!Path.IsPathRooted(cancellationPath) || !Path.IsPathRooted(resourcePath) ||
                nonce == null || !System.Text.RegularExpressions.Regex.IsMatch(nonce, "^[a-f0-9]{32}$"))
                throw new ArgumentException("Run control needs absolute paths and a fenced nonce");
            cancelPath = cancellationPath; cancelNonce = nonce; metricsPath = resourcePath;
        }
        [DllImport("kernel32.dll", SetLastError = true)] static extern IntPtr CreateJobObject(IntPtr security, string name);
        [DllImport("kernel32.dll", SetLastError = true)] static extern bool SetInformationJobObject(IntPtr job, int kind, ref Limits limits, int size);
        [DllImport("kernel32.dll", SetLastError = true)] static extern uint WaitForSingleObject(IntPtr handle, uint timeout);
        [DllImport("kernel32.dll", SetLastError = true)] static extern bool GetExitCodeProcess(IntPtr process, out uint code);
        [DllImport("kernel32.dll", SetLastError = true)] static extern bool TerminateProcess(IntPtr process, uint code);
        [DllImport("kernel32.dll", SetLastError = true)] static extern bool TerminateJobObject(IntPtr job, uint code);
        [DllImport("kernel32.dll", SetLastError = true)] static extern bool QueryInformationJobObject(IntPtr job, int kind, ref Accounting info, int size, IntPtr returned);
        [DllImport("kernel32.dll", SetLastError = true)] static extern bool QueryInformationJobObject(IntPtr job, int kind, ref Limits info, int size, IntPtr returned);
        [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        static extern IntPtr CreateFile(string path, uint access, uint share, ref Security security, uint creation, uint flags, IntPtr template);
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        static extern bool CreateProcess(string app, StringBuilder command, IntPtr processSecurity, IntPtr threadSecurity,
            bool inherit, uint flags, IntPtr environment, string directory, ref ExtendedStartup startup, out ProcessInfo info);
        [DllImport("kernel32.dll", SetLastError = true)]
        static extern bool InitializeProcThreadAttributeList(IntPtr list, int count, int flags, ref IntPtr size);
        [DllImport("kernel32.dll", SetLastError = true)]
        static extern bool UpdateProcThreadAttribute(IntPtr list, uint flags, IntPtr attribute, IntPtr value, IntPtr size, IntPtr previous, IntPtr returned);
        [DllImport("kernel32.dll")] static extern void DeleteProcThreadAttributeList(IntPtr list);

        static void Check(bool ok) { if (!ok) throw new Win32Exception(Marshal.GetLastWin32Error()); }
        static void Close(IntPtr handle)
        {
            if (handle != IntPtr.Zero && handle != new IntPtr(-1)) CloseHandle(handle);
        }
        static bool CancellationRequested()
        {
            if (cancelPath == null || !File.Exists(cancelPath)) return false;
            try { return File.ReadAllText(cancelPath, Encoding.UTF8).Trim() == cancelNonce; }
            catch (IOException) { return false; }
        }
        const int ResourcePublicationBudgetMilliseconds = 1000;
        static void PublishResourceFile(string temporary)
        {
            Stopwatch waiting = Stopwatch.StartNew();
            for (;;)
            {
                try
                {
                    if (File.Exists(metricsPath)) File.Replace(temporary, metricsPath, null);
                    else File.Move(temporary, metricsPath);
                    return;
                }
                catch (IOException error)
                {
                    int native = error.HResult & 0xffff;
                    // ReplaceFile error1175 retains both original names. The
                    // partial-move errors1176/1177 are deliberately not retried.
                    if ((native != 32 && native != 33 && native != 1175) ||
                        waiting.ElapsedMilliseconds >= ResourcePublicationBudgetMilliseconds) throw;
                    System.Threading.Thread.Sleep(20);
                }
            }
        }
        static void Sample(IntPtr job, int pid, Stopwatch elapsed, bool finished, bool cancelled)
        {
            if (metricsPath == null) return;
            Accounting accounting = new Accounting(); Limits limits = new Limits();
            bool available = QueryInformationJobObject(job, 8, ref accounting, Marshal.SizeOf(typeof(Accounting)), IntPtr.Zero);
            int accountingError = available ? 0 : Marshal.GetLastWin32Error();
            bool memoryAvailable = QueryInformationJobObject(job, 9, ref limits, Marshal.SizeOf(typeof(Limits)), IntPtr.Zero);
            string json = "{\"schemaVersion\":1,\"sampledUtc\":\"" + DateTime.UtcNow.ToString("o", CultureInfo.InvariantCulture) +
                "\",\"pid\":" + pid + ",\"elapsedSeconds\":" + elapsed.Elapsed.TotalSeconds.ToString("F3", CultureInfo.InvariantCulture) +
                ",\"finished\":" + (finished ? "true" : "false") + ",\"cancelled\":" + (cancelled ? "true" : "false") +
                ",\"accountingAvailable\":" + (available ? "true" : "false") + ",\"accountingError\":" + accountingError +
                ",\"cpuSeconds\":" + (available ? ((accounting.userTime + accounting.kernelTime) / 10000000.0).ToString("F6", CultureInfo.InvariantCulture) : "null") +
                ",\"activeProcesses\":" + (available ? accounting.activeProcesses.ToString(CultureInfo.InvariantCulture) : "null") +
                ",\"totalProcesses\":" + (available ? accounting.totalProcesses.ToString(CultureInfo.InvariantCulture) : "null") +
                ",\"ioReadBytes\":" + (available ? accounting.io.readBytes.ToString(CultureInfo.InvariantCulture) : "null") +
                ",\"ioWriteBytes\":" + (available ? accounting.io.writeBytes.ToString(CultureInfo.InvariantCulture) : "null") +
                ",\"peakJobMemoryBytes\":" + (memoryAvailable ? limits.peakJobMemory.ToUInt64().ToString(CultureInfo.InvariantCulture) : "null") + "}";
            string temporary = metricsPath + "." + Guid.NewGuid().ToString("N") + ".tmp";
            try
            {
                File.WriteAllText(temporary, json, new UTF8Encoding(false));
                PublishResourceFile(temporary);
            }
            finally { if (File.Exists(temporary)) File.Delete(temporary); }
        }

        public static int Run(string directory, string logPath)
        { return RunCommand(directory, logPath, "call scripts\\build.bat && call scripts\\test.bat"); }

        public static int RunBatch(string directory, string logPath, string batch, bool build)
        {
            if (batch == null || batch.Length != 32 || !System.Text.RegularExpressions.Regex.IsMatch(batch, "^[a-f0-9]{32}$"))
                throw new ArgumentException("Invalid batch identity");
            string command = (build ? "call scripts\\build.bat && " : "") +
                PowerShell() + " -NoProfile -File scripts\\build_batch_tests.ps1 -Batch " + batch;
            return RunCommand(directory, logPath, command);
        }

        public static int Run(string directory, string logPath, bool reloadShell)
        { return RunCommand(directory, logPath, "call scripts\\build.bat" + (reloadShell ? " --reload-shell" : "") + " && call scripts\\test.bat"); }

        public static int RunBatch(string directory, string logPath, string batch, bool build, bool reloadShell)
        { return RunBatch(directory, logPath, batch, build, reloadShell, false); }

        public static int RunBatch(string directory, string logPath, string batch, bool build, bool reloadShell, bool closeApplication)
        {
            if (reloadShell && closeApplication) throw new ArgumentException("Choose only one output-owner action");
            if (batch == null || !System.Text.RegularExpressions.Regex.IsMatch(batch, "^[a-f0-9]{32}$"))
                throw new ArgumentException("Invalid batch identity");
            string command = (build ? "call scripts\\build.bat" + (closeApplication ? " --close-application" : reloadShell ? " --reload-shell" : "") + " && " : "") +
                PowerShell() + " -NoProfile -File scripts\\build_batch_tests.ps1 -Batch " + batch;
            return RunCommand(directory, logPath, command);
        }

        public static int RunLeasedCommand(string directory, string logPath, string pipeline)
        { return RunCommand(directory, logPath, pipeline); }

        static string PowerShell()
        {
            string selected = Environment.GetEnvironmentVariable("SNOWDESKTOP_ENTRY_POWERSHELL");
            if (String.IsNullOrEmpty(selected) || !Path.IsPathRooted(selected) || !File.Exists(selected))
            {
                string core = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), "PowerShell", "7", "pwsh.exe");
                selected = File.Exists(core) ? core : Path.Combine(Environment.SystemDirectory, "WindowsPowerShell", "v1.0", "powershell.exe");
            }
            return "\"" + selected + "\"";
        }

        static int RunCommand(string directory, string logPath, string pipeline)
        {
            RunWasCancelled = false;
            IntPtr job = IntPtr.Zero, output = IntPtr.Zero, input = IntPtr.Zero;
            IntPtr attributes = IntPtr.Zero, handles = IntPtr.Zero, jobList = IntPtr.Zero;
            bool initialized = false, completed = false;
            ProcessInfo process = new ProcessInfo();
            try
            {
                job = CreateJobObject(IntPtr.Zero, null);
                Check(job != IntPtr.Zero);
                Limits limits = new Limits();
                limits.basic.flags = 0x2000; // JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
                Check(SetInformationJobObject(job, 9, ref limits, Marshal.SizeOf(typeof(Limits))));
                Security security = new Security { size = Marshal.SizeOf(typeof(Security)), inherit = 1 };
                output = CreateFile(logPath, 0x40000000, 3, ref security, 2, 0x80, IntPtr.Zero);
                Check(output != new IntPtr(-1));
                input = CreateFile("NUL", 0x80000000, 3, ref security, 3, 0x80, IntPtr.Zero);
                Check(input != new IntPtr(-1));

                // Whitelist handles: the child must not inherit either lease
                // or the job handle, otherwise a crashed parent cannot release it.
                IntPtr size = IntPtr.Zero;
                InitializeProcThreadAttributeList(IntPtr.Zero, 2, 0, ref size);
                attributes = Marshal.AllocHGlobal(size);
                Check(InitializeProcThreadAttributeList(attributes, 2, 0, ref size));
                initialized = true;
                handles = Marshal.AllocHGlobal(2 * IntPtr.Size);
                Marshal.WriteIntPtr(handles, 0, input);
                Marshal.WriteIntPtr(handles, IntPtr.Size, output);
                Check(UpdateProcThreadAttribute(attributes, 0, new IntPtr(0x20002), handles,
                    new IntPtr(2 * IntPtr.Size), IntPtr.Zero, IntPtr.Zero));
                // Windows 10+: assign the job atomically during creation. There
                // is no crash window leaving even a suspended child outside it.
                jobList = Marshal.AllocHGlobal(IntPtr.Size);
                Marshal.WriteIntPtr(jobList, job);
                Check(UpdateProcThreadAttribute(attributes, 0, new IntPtr(0x2000d), jobList,
                    new IntPtr(IntPtr.Size), IntPtr.Zero, IntPtr.Zero));
                ExtendedStartup startup = new ExtendedStartup();
                startup.startup.size = Marshal.SizeOf(typeof(ExtendedStartup));
                startup.startup.flags = 0x100; // STARTF_USESTDHANDLES
                startup.startup.input = input;
                startup.startup.output = startup.startup.error = output;
                startup.attributes = attributes;
                string cmd = Path.Combine(Environment.SystemDirectory, "cmd.exe");
                StringBuilder command = new StringBuilder("\"" + cmd + "\" /d /s /c \"" + pipeline + "\"");
                Check(CreateProcess(cmd, command, IntPtr.Zero, IntPtr.Zero, true,
                    0x80000 | 0x8000000, IntPtr.Zero, directory, ref startup, out process));
                Stopwatch elapsed = Stopwatch.StartNew(); double nextSample = 0, nextNotice = 30;
                bool cancelled = false, metricWarning = false;
                for (;;)
                {
                    uint wait = WaitForSingleObject(process.process, 200);
                    if (wait != 0 && wait != 258) throw new Win32Exception(Marshal.GetLastWin32Error());
                    if (wait == 258 && CancellationRequested())
                    {
                        Check(TerminateJobObject(job, 4));
                        Check(WaitForSingleObject(process.process, 5000) == 0);
                        cancelled = true; RunWasCancelled = true; wait = 0;
                    }
                    if (elapsed.Elapsed.TotalSeconds >= nextSample || wait == 0)
                    {
                        try { Sample(job, process.pid, elapsed, wait == 0, cancelled); }
                        catch (IOException error)
                        {
                            if (wait == 0 || !metricWarning)
                                Console.Error.WriteLine((wait == 0 ? "Terminal" : "Run") + " resource sample unavailable: " + error.Message);
                            metricWarning = true;
                        }
                        nextSample = elapsed.Elapsed.TotalSeconds + 1;
                    }
                    if (wait == 0) break;
                    if (elapsed.Elapsed.TotalSeconds >= nextNotice)
                    {
                        Console.Error.WriteLine("Build/test child " + process.pid + " remains active after " + (int)elapsed.Elapsed.TotalSeconds + "s; owned Job remains contained.");
                        nextNotice += 30;
                    }
                }
                uint code;
                Check(GetExitCodeProcess(process.process, out code));
                completed = true;
                return cancelled ? 4 : unchecked((int)code);
            }
            finally
            {
                if (!completed && process.process != IntPtr.Zero) TerminateProcess(process.process, 4);
                Close(job);
                Close(process.thread);
                Close(process.process);
                Close(input);
                Close(output);
                if (initialized) DeleteProcThreadAttributeList(attributes);
                if (attributes != IntPtr.Zero) Marshal.FreeHGlobal(attributes);
                if (handles != IntPtr.Zero) Marshal.FreeHGlobal(handles);
                if (jobList != IntPtr.Zero) Marshal.FreeHGlobal(jobList);
            }
        }
    }
}
