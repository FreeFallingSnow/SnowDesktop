// A private job contains only the build/test process tree. Closing its last
// handle, including when the coordinator crashes, terminates that tree.
using System;
using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;

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
        [DllImport("kernel32.dll", SetLastError = true)] static extern IntPtr CreateJobObject(IntPtr security, string name);
        [DllImport("kernel32.dll", SetLastError = true)] static extern bool SetInformationJobObject(IntPtr job, int kind, ref Limits limits, int size);
        [DllImport("kernel32.dll", SetLastError = true)] static extern uint WaitForSingleObject(IntPtr handle, uint timeout);
        [DllImport("kernel32.dll", SetLastError = true)] static extern bool GetExitCodeProcess(IntPtr process, out uint code);
        [DllImport("kernel32.dll", SetLastError = true)] static extern bool TerminateProcess(IntPtr process, uint code);
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

        public static int Run(string directory, string logPath)
        { return RunCommand(directory, logPath, "call scripts\\build.bat && call scripts\\test.bat"); }

        public static int RunBatch(string directory, string logPath, string batch, bool build)
        {
            if (batch == null || batch.Length != 32 || !System.Text.RegularExpressions.Regex.IsMatch(batch, "^[a-f0-9]{32}$"))
                throw new ArgumentException("Invalid batch identity");
            string command = (build ? "call scripts\\build.bat && " : "") +
                PowerShell() + " -NoProfile -ExecutionPolicy Bypass -File scripts\\build_batch_tests.ps1 -Batch " + batch;
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
                PowerShell() + " -NoProfile -ExecutionPolicy Bypass -File scripts\\build_batch_tests.ps1 -Batch " + batch;
            return RunCommand(directory, logPath, command);
        }

        public static int RunLeasedCommand(string directory, string logPath, string pipeline)
        { return RunCommand(directory, logPath, pipeline); }

        static string PowerShell()
        { return "\"" + Path.Combine(Environment.SystemDirectory, "WindowsPowerShell", "v1.0", "powershell.exe") + "\""; }

        static int RunCommand(string directory, string logPath, string pipeline)
        {
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
                Check(WaitForSingleObject(process.process, uint.MaxValue) == 0);
                uint code;
                Check(GetExitCodeProcess(process.process, out code));
                completed = true;
                return unchecked((int)code);
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
