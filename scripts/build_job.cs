// A private job contains only the build/test process tree. Closing its last
// handle, including when the coordinator crashes, terminates that tree.
using System;
using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Diagnostics;
using System.Globalization;
using System.Collections.Generic;

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
        [StructLayout(LayoutKind.Sequential)]
        struct CpuLimits { public uint flags, rate; }
        [StructLayout(LayoutKind.Sequential)]
        struct MemoryStatus
        {
            public uint size, load;
            public ulong physical, availablePhysical, pageFile, availablePageFile, virtualBytes, availableVirtual, extendedVirtual;
        }
        static uint resourceCpuSlots, resourceLogicalProcessors;
        static ulong resourceMemoryMiB;
        public static uint LogicalProcessors()
        {
            uint value = GetActiveProcessorCount(0xffff);
            Check(value > 0); return value;
        }
        static MemoryStatus ReadMemory()
        {
            MemoryStatus value = new MemoryStatus(); value.size = (uint)Marshal.SizeOf(typeof(MemoryStatus));
            Check(GlobalMemoryStatusEx(ref value)); return value;
        }
        public static ulong PhysicalMemoryBytes() { return ReadMemory().physical; }
        public static ulong AvailableMemoryBytes() { return ReadMemory().availablePhysical; }
        public static void ConfigureResourceBudget(uint cpuSlots, uint logicalProcessors, ulong memoryMiB)
        {
            if (cpuSlots == 0 && logicalProcessors == 0 && memoryMiB == 0)
            { resourceCpuSlots = 0; resourceLogicalProcessors = 0; resourceMemoryMiB = 0; return; }
            if (logicalProcessors != LogicalProcessors() || cpuSlots < 1 || cpuSlots > logicalProcessors ||
                memoryMiB < 256 || memoryMiB > PhysicalMemoryBytes() / 1048576UL)
                throw new ArgumentException("A resource budget needs valid CPU slots and committed-memory MiB on this machine");
            resourceCpuSlots = cpuSlots; resourceLogicalProcessors = logicalProcessors; resourceMemoryMiB = memoryMiB;
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
        [DllImport("kernel32.dll", SetLastError = true)] static extern bool SetInformationJobObject(IntPtr job, int kind, ref CpuLimits limits, int size);
        [DllImport("kernel32.dll", SetLastError = true)] static extern bool QueryInformationJobObject(IntPtr job, int kind, ref CpuLimits limits, int size, IntPtr returned);
        [DllImport("kernel32.dll", SetLastError = true)] static extern bool GlobalMemoryStatusEx(ref MemoryStatus status);
        [DllImport("kernel32.dll", SetLastError = true)] static extern uint GetActiveProcessorCount(ushort group);
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
            CpuLimits cpu = new CpuLimits();
            bool cpuAvailable = QueryInformationJobObject(job, 15, ref cpu, Marshal.SizeOf(typeof(CpuLimits)), IntPtr.Zero);
            string json = "{\"schemaVersion\":1,\"sampledUtc\":\"" + DateTime.UtcNow.ToString("o", CultureInfo.InvariantCulture) +
                "\",\"pid\":" + pid + ",\"elapsedSeconds\":" + elapsed.Elapsed.TotalSeconds.ToString("F3", CultureInfo.InvariantCulture) +
                ",\"finished\":" + (finished ? "true" : "false") + ",\"cancelled\":" + (cancelled ? "true" : "false") +
                ",\"accountingAvailable\":" + (available ? "true" : "false") + ",\"accountingError\":" + accountingError +
                ",\"cpuSeconds\":" + (available ? ((accounting.userTime + accounting.kernelTime) / 10000000.0).ToString("F6", CultureInfo.InvariantCulture) : "null") +
                ",\"activeProcesses\":" + (available ? accounting.activeProcesses.ToString(CultureInfo.InvariantCulture) : "null") +
                ",\"totalProcesses\":" + (available ? accounting.totalProcesses.ToString(CultureInfo.InvariantCulture) : "null") +
                ",\"ioReadBytes\":" + (available ? accounting.io.readBytes.ToString(CultureInfo.InvariantCulture) : "null") +
                ",\"ioWriteBytes\":" + (available ? accounting.io.writeBytes.ToString(CultureInfo.InvariantCulture) : "null") +
                ",\"peakJobMemoryBytes\":" + (memoryAvailable ? limits.peakJobMemory.ToUInt64().ToString(CultureInfo.InvariantCulture) : "null") +
                ",\"requestedCpuSlots\":" + (resourceCpuSlots > 0 ? resourceCpuSlots.ToString(CultureInfo.InvariantCulture) : "null") +
                ",\"requestedMemoryMiB\":" + (resourceMemoryMiB > 0 ? resourceMemoryMiB.ToString(CultureInfo.InvariantCulture) : "null") +
                ",\"cpuLimitBasisPoints\":" + (cpuAvailable && (cpu.flags & 5) == 5 ? cpu.rate.ToString(CultureInfo.InvariantCulture) : "null") +
                ",\"committedMemoryLimitBytes\":" + (memoryAvailable && (limits.basic.flags & 0x200) != 0 ? limits.jobMemory.ToUInt64().ToString(CultureInfo.InvariantCulture) : "null") + "}";
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
                if (resourceMemoryMiB > 0)
                {
                    limits.basic.flags |= 0x200; // JOB_OBJECT_LIMIT_JOB_MEMORY: committed memory, not working set.
                    limits.jobMemory = new UIntPtr(checked(resourceMemoryMiB * 1048576UL));
                }
                Check(SetInformationJobObject(job, 9, ref limits, Marshal.SizeOf(typeof(Limits))));
                if (resourceCpuSlots > 0)
                {
                    // Microsoft documents CPU_RATE_ENABLE|HARD_CAP and rate/10000.
                    // A stricter ancestor Job can further reduce this ceiling.
                    CpuLimits cpu = new CpuLimits { flags = 5,
                        rate = (uint)Math.Max(1UL, 10000UL * resourceCpuSlots / resourceLogicalProcessors) };
                    Check(SetInformationJobObject(job, 15, ref cpu, Marshal.SizeOf(typeof(CpuLimits))));
                }
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

    public sealed class ResourceQueueTurn
    {
        public int Position, Waiting;
        public long Sequence;
        public string HeadRunId;
    }
    // A ticket is alive only while its private, non-inherited handle excludes
    // a probe. Clock age, PID metadata and observer lifetime grant no turn.
    public sealed class ResourceWaiter : IDisposable
    {
        readonly string directory, path;
        FileStream lease;
        public readonly string RunId;
        public readonly long Sequence;
        ResourceWaiter(string folder, string file, string run, long order, FileStream held)
        { directory = folder; path = file; RunId = run; Sequence = order; lease = held; }
        static bool Sharing(IOException error)
        { int code = error.HResult & 0xffff; return code == 32 || code == 33; }
        static List<string> Active(string folder, string own)
        {
            List<string> alive = new List<string>(); int inspected = 0;
            foreach (string file in Directory.EnumerateFiles(folder, "*.ticket"))
            {
                if (++inspected > 1024) throw new InvalidOperationException("Resource queue exceeds its ticket budget; preserve it");
                string name = Path.GetFileName(file); long order; Guid id;
                if (name.Length != 60 || name[20] != '-' || !name.EndsWith(".ticket", StringComparison.Ordinal) ||
                    !Int64.TryParse(name.Substring(0, 20), NumberStyles.None, CultureInfo.InvariantCulture, out order) || order < 1 ||
                    !Guid.TryParseExact(name.Substring(21, 32), "N", out id))
                    throw new InvalidOperationException("Invalid resource queue ticket; preserve it");
                try { if ((File.GetAttributes(file) & FileAttributes.ReparsePoint) != 0) throw new InvalidOperationException("Linked resource queue ticket; preserve it"); }
                catch (FileNotFoundException) { continue; }
                if (file == own) { alive.Add(file); continue; }
                try
                {
                    // Holding this exclusive read/write probe proves there is
                    // no owning waiter. Delete sharing keeps the proof alive
                    // while removing only that abandoned scheduling ticket.
                    using (FileStream probe = new FileStream(file, FileMode.Open, FileAccess.ReadWrite, FileShare.Delete))
                        File.Delete(file);
                }
                catch (FileNotFoundException) { }
                catch (IOException error) { if (!Sharing(error)) throw; alive.Add(file); }
            }
            alive.Sort(StringComparer.Ordinal); return alive;
        }
        public static ResourceWaiter Join(string resourceDirectory, string runId)
        {
            Guid run;
            if (!Guid.TryParseExact(runId, "N", out run)) throw new ArgumentException("Invalid resource queue Run identity");
            string folder = Path.Combine(resourceDirectory, "admission"); Directory.CreateDirectory(folder);
            if ((File.GetAttributes(folder) & FileAttributes.ReparsePoint) != 0)
                throw new InvalidOperationException("Linked resource queue directory; preserve it");
            using (FileStream transaction = new FileStream(Path.Combine(folder, "queue.lock"), FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None))
            {
                List<string> active = Active(folder, null);
                if (active.Count >= 1024) throw new InvalidOperationException("Resource queue is full");
                string counter = Path.Combine(folder, "sequence"); long previous = 0;
                if (File.Exists(counter))
                {
                    if (!Int64.TryParse(File.ReadAllText(counter).Trim(), NumberStyles.None, CultureInfo.InvariantCulture, out previous) || previous < 0 || previous == Int64.MaxValue)
                        throw new InvalidOperationException("Invalid resource queue sequence; preserve it");
                }
                else if (active.Count != 0) throw new InvalidOperationException("Resource queue sequence is missing; preserve it");
                long next = previous + 1;
                if (active.Count != 0 && StringComparer.Ordinal.Compare(Path.GetFileName(active[active.Count - 1]).Substring(0, 20), next.ToString("D20", CultureInfo.InvariantCulture)) >= 0)
                    throw new InvalidOperationException("Resource queue sequence moved backwards; preserve it");
                string temporary = counter + "." + Guid.NewGuid().ToString("N") + ".tmp";
                try
                {
                    byte[] bytes = Encoding.UTF8.GetBytes(next.ToString(CultureInfo.InvariantCulture));
                    using (FileStream output = new FileStream(temporary, FileMode.CreateNew, FileAccess.Write, FileShare.None))
                    { output.Write(bytes, 0, bytes.Length); output.Flush(true); }
                    if (File.Exists(counter)) File.Replace(temporary, counter, null); else File.Move(temporary, counter);
                }
                finally { if (File.Exists(temporary)) File.Delete(temporary); }
                string ticket = Path.Combine(folder, next.ToString("D20", CultureInfo.InvariantCulture) + "-" + runId + ".ticket");
                FileStream held = new FileStream(ticket, FileMode.CreateNew, FileAccess.ReadWrite, FileShare.None);
                return new ResourceWaiter(folder, ticket, runId, next, held);
            }
        }
        public ResourceQueueTurn Inspect()
        {
            if (lease == null) throw new InvalidOperationException("Resource queue ticket is no longer owned");
            using (FileStream transaction = new FileStream(Path.Combine(directory, "queue.lock"), FileMode.Open, FileAccess.ReadWrite, FileShare.None))
            {
                List<string> active = Active(directory, path); int index = active.IndexOf(path);
                if (index < 0) throw new InvalidOperationException("Owned resource queue ticket disappeared");
                return new ResourceQueueTurn { Position = index + 1, Waiting = active.Count, Sequence = Sequence,
                    HeadRunId = Path.GetFileName(active[0]).Substring(21, 32) };
            }
        }
        public void Dispose()
        {
            FileStream held = lease; lease = null;
            if (held == null) return;
            held.Dispose();
            try { File.Delete(path); }
            catch (IOException error) { if (!Sharing(error)) throw; }
        }
    }
    public sealed class ResourceReservation : IDisposable
    {
        List<FileStream> handles;
        internal ResourceReservation(List<FileStream> held) { handles = held; }
        public void AttachCompatibility(FileStream lease)
        {
            if (handles == null || lease == null) throw new InvalidOperationException("Reservation has no live compatibility lease");
            handles.Add(lease); // Release CPU/memory tokens before the v1 compatibility reader.
        }
        public void Dispose()
        {
            List<FileStream> held = handles; handles = null;
            if (held != null) foreach (FileStream value in held) value.Dispose();
        }
    }
    public sealed class ResourceAttempt
    {
        public ResourceReservation Lease;
        public string BusyKind;
        public int OccupiedCpuSlots, OccupiedMemorySlots;
        public ulong AvailablePhysicalBytes;
    }
    public static class ResourceBroker
    {
        static List<FileStream> Scan(string directory, string kind, int capacity, int requested, out int occupied)
        {
            List<FileStream> held = new List<FileStream>(); occupied = 0;
            try
            {
                for (int i = 0; i < capacity; i++)
                {
                    FileStream stream;
                    try { stream = new FileStream(Path.Combine(directory, kind + "-" + i.ToString("D4", CultureInfo.InvariantCulture) + ".lock"), FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None); }
                    catch (IOException error)
                    { if ((error.HResult & 0xffff) != 32 && (error.HResult & 0xffff) != 33) throw; occupied++; continue; }
                    if (held.Count < requested) held.Add(stream); else stream.Dispose();
                }
                return held;
            }
            catch { foreach (FileStream stream in held) stream.Dispose(); throw; }
        }
        // The caller holds budget.lock and a shared v1 compatibility handle.
        // Locks are the authority; no owner heartbeat or stale JSON reclaims them.
        public static ResourceAttempt TryReserve(string directory, int cpuCapacity, int memoryCapacitySlots,
            int cpuSlots, int memorySlots, ulong availablePhysicalBytes, ulong headroomBytes)
        {
            if (cpuCapacity < 1 || cpuCapacity > 4096 || memoryCapacitySlots < 1 || memoryCapacitySlots > 4096 ||
                cpuSlots < 1 || cpuSlots > cpuCapacity || memorySlots < 1 || memorySlots > memoryCapacitySlots)
                throw new ArgumentException("Resource reservation exceeds validated policy capacity");
            List<FileStream> cpu = null, memory = null;
            try
            {
                int busyCpu, busyMemory;
                cpu = Scan(directory, "cpu", cpuCapacity, cpuSlots, out busyCpu);
                memory = Scan(directory, "memory", memoryCapacitySlots, memorySlots, out busyMemory);
                ResourceAttempt result = new ResourceAttempt { OccupiedCpuSlots = busyCpu,
                    OccupiedMemorySlots = busyMemory, AvailablePhysicalBytes = availablePhysicalBytes };
                if (cpu.Count < cpuSlots) result.BusyKind = "cpu-capacity";
                else if (memory.Count < memorySlots) result.BusyKind = "memory-capacity";
                // Count all other reservations again, conservatively, even when
                // their actual allocations already reduce available memory.
                else if (availablePhysicalBytes < checked(headroomBytes + (ulong)(busyMemory + memorySlots) * 268435456UL))
                    result.BusyKind = "memory-headroom";
                else
                {
                    cpu.AddRange(memory); memory = null;
                    result.Lease = new ResourceReservation(cpu); cpu = null;
                }
                return result;
            }
            finally
            {
                if (cpu != null) foreach (FileStream stream in cpu) stream.Dispose();
                if (memory != null) foreach (FileStream stream in memory) stream.Dispose();
            }
        }
    }
}
