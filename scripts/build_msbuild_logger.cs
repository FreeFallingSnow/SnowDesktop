using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Build.Framework;

namespace SnowDesktop.Build
{
    // Runs inside the leased MSBuild process. Never serialize command lines,
    // environment variables, task parameters or arbitrary MSBuild messages.
    public sealed class PhaseLogger : INodeLogger
    {
        sealed class Operation
        {
            public string Id = Guid.NewGuid().ToString("N"), Name, Task, Project;
            public long Tick;
            public DateTime Timestamp;
            public HashSet<string> Inputs = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            public HashSet<string> Invoked = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            public bool InputsObserved, CommandObserved, Unexpanded;
        }
        [DllImport("shell32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        static extern IntPtr CommandLineToArgvW(string command, out int count);
        [DllImport("kernel32.dll")] static extern IntPtr LocalFree(IntPtr memory);
        readonly object sync = new object();
        readonly Dictionary<string, Operation> active = new Dictionary<string, Operation>();
        readonly Dictionary<string, string> targets = new Dictionary<string, string>();
        StreamWriter output;
        string run, parent;
        bool capped;
        long written;
        public string Parameters { get; set; }
        public LoggerVerbosity Verbosity { get; set; }
        static string Context(BuildEventContext value)
        { return value == null ? "none" : value.NodeId+":"+value.ProjectContextId+":"+value.TargetId+":"+value.TaskId; }
        static string TargetContext(BuildEventContext value)
        { return value == null ? "none" : value.NodeId+":"+value.ProjectContextId+":"+value.TargetId; }
        static string Phase(string task, string target)
        {
            if (task == "CL") return "compile";
            // MultiToolTask also runs MIDL/FXC. Only its actual ClCompile target
            // denotes compiler work; do not label every parallel tool as CL.
            if (task == "MultiToolTask" && target == "ClCompile") return "compile";
            if (task == "Link" || task == "Lib") return "link";
            if (task.IndexOf("Xaml", StringComparison.OrdinalIgnoreCase) >= 0 || task.StartsWith("MarkupCompile", StringComparison.Ordinal)) return "codegen";
            if (task == "CustomBuild") return "custom-build";
            return null;
        }
        static string Json(string text)
        {
            var b = new StringBuilder("\"");
            foreach (char value in text ?? "")
            {
                if (value == '\\' || value == '"') { b.Append('\\'); b.Append(value); }
                else if (value < 32) b.Append("\\u"+((int)value).ToString("x4", CultureInfo.InvariantCulture));
                else b.Append(value);
            }
            return b.Append('"').ToString();
        }
        void Write(string kind, string fields)
        {
            if (capped) return;
            if (written > 4*1024*1024)
            { capped=true; output.WriteLine("{\"schemaVersion\":1,\"kind\":\"collector-limit\",\"runId\":"+Json(run)+"}");output.Flush();return; }
            string line="{\"schemaVersion\":1,\"source\":\"msbuild\",\"kind\":"+Json(kind)+",\"runId\":"+Json(run)+",\"utc\":"+Json(DateTime.UtcNow.ToString("o"))+fields+"}";
            output.WriteLine(line);output.Flush();written+=Encoding.UTF8.GetByteCount(line)+1;
        }
        public void Initialize(IEventSource events) { Initialize(events, 1); }
        public void Initialize(IEventSource events, int nodeCount)
        {
            string[] fields=Encoding.UTF8.GetString(Convert.FromBase64String(Parameters)).Split('\n');
            Guid parsed;
            if(fields.Length!=3 || !Guid.TryParseExact(fields[1],"N",out parsed)) throw new LoggerException("Invalid SnowDesktop observation parameters");
            run=fields[1];parent=fields[2];
            if(output!=null)Shutdown();
            active.Clear();targets.Clear();capped=false;written=0;
            // CMake can invoke/reuse MSBuild once per selected target with the
            // same logger parameters. Each initialization owns a fresh file.
            string file=Path.ChangeExtension(fields[0],Guid.NewGuid().ToString("N")+".jsonl");
            output=new StreamWriter(new FileStream(file,FileMode.CreateNew,FileAccess.Write,FileShare.Read|FileShare.Delete),new UTF8Encoding(false));
            var extended=events as IEventSource3;
            if(extended!=null)extended.IncludeTaskInputs();
            Write("collector",",\"nodeCount\":"+nodeCount+",\"taskInputsAvailable\":"+(extended!=null?"true":"false"));
            events.TaskStarted+=Start;
            events.TaskFinished+=Finish;
            events.MessageRaised+=Message;
            events.TargetStarted+=StartTarget;
            events.TargetFinished+=FinishTarget;
        }
        void StartTarget(object sender, TargetStartedEventArgs value)
        { lock(sync) { targets[TargetContext(value.BuildEventContext)]=value.TargetName; } }
        void FinishTarget(object sender, TargetFinishedEventArgs value)
        { lock(sync) { targets.Remove(TargetContext(value.BuildEventContext)); } }
        void Start(object sender, TaskStartedEventArgs value)
        {
            lock(sync)
            {
                string target;targets.TryGetValue(TargetContext(value.BuildEventContext),out target);
                string phase=Phase(value.TaskName,target);if(phase==null)return;
                var op=new Operation{Name=phase,Task=value.TaskName,Project=value.ProjectFile,Tick=Stopwatch.GetTimestamp(),Timestamp=value.Timestamp.ToUniversalTime()};
                active[Context(value.BuildEventContext)]=op;
                Write("start",",\"id\":"+Json(op.Id)+",\"parentId\":"+Json(parent)+",\"name\":"+Json(phase)+",\"task\":"+Json(op.Task)+",\"project\":"+Json(Path.GetFileNameWithoutExtension(op.Project))+",\"sourceUtc\":"+Json(op.Timestamp.ToString("o")));
            }
        }
        static string Absolute(string file, string project)
        {
            // A compiler argument can be a quoted macro or tool banner. It is
            // not necessarily a path; unknown arguments never become units.
            try { return Path.GetFullPath(Path.IsPathRooted(file)?file:Path.Combine(Path.GetDirectoryName(project),file)); }
            catch(ArgumentException) { return null; }
            catch(NotSupportedException) { return null; }
            catch(PathTooLongException) { return null; }
        }
        static readonly HashSet<string> ValueOptions=new HashSet<string>(StringComparer.Ordinal)
        {"/D","/U","/I","/FI","/Fo","/Fp","/Fd","/Fa","/Fe","/FR","/FU","/AI","/external:I","/sourceDependencies","/ifcOutput","/reference","/headerUnit","/scanDependencies"};
        static readonly HashSet<string> SourceExtensions=new HashSet<string>(StringComparer.OrdinalIgnoreCase)
        {".c",".cc",".cpp",".cxx",".c++",".ixx",".cppm",".mpp",".m",".mm"};
        static void Arguments(Operation op,string command,int depth)
        {
            int count;IntPtr argv=CommandLineToArgvW(command,out count);
            if(argv==IntPtr.Zero){op.Unexpanded=true;return;}
            bool skip=false;
            try{
                for(int i=0;i<count;i++){
                    string arg=Marshal.PtrToStringUni(Marshal.ReadIntPtr(argv,i*IntPtr.Size));
                    if(skip){skip=false;continue;}
                    if(String.IsNullOrEmpty(arg))continue;
                    if(ValueOptions.Contains(arg)){skip=true;continue;}
                    if(arg[0]=='@'){
                        string response=Absolute(arg.Substring(1),op.Project);
                        if(depth>=2||response==null||!File.Exists(response)||new FileInfo(response).Length>1024*1024){op.Unexpanded=true;continue;}
                        Arguments(op,"cl "+File.ReadAllText(response),depth+1);continue;
                    }
                    if(arg.Length>3&&(arg.StartsWith("/Tc",StringComparison.Ordinal)||arg.StartsWith("/Tp",StringComparison.Ordinal)))arg=arg.Substring(3);
                    else if(arg[0]=='/'||arg[0]=='-')continue;
                    string file=Absolute(arg,op.Project);
                    if(file!=null&&File.Exists(file)&&(SourceExtensions.Contains(Path.GetExtension(file))||op.Inputs.Contains(file)))op.Invoked.Add(file);
                }
            }finally{LocalFree(argv);}
        }
        void Message(object sender, BuildMessageEventArgs value)
        {
            lock(sync)
            {
                Operation op;if(!active.TryGetValue(Context(value.BuildEventContext),out op)||op.Name!="compile")return;
                var input=value as TaskParameterEventArgs;
                if(input!=null && input.Kind==TaskParameterMessageKind.TaskInput && (input.ParameterName=="Sources"||input.ItemType=="Sources"))
                {
                    op.InputsObserved=true;
                    if(input.Items!=null)foreach(object item in input.Items)
                    { var taskItem=item as ITaskItem;string file=Absolute(taskItem==null?item.ToString():taskItem.ItemSpec,op.Project);if(file!=null)op.Inputs.Add(file); }
                }
                var command=value as TaskCommandLineEventArgs;
                if(command==null)return;
                op.CommandObserved=true;
                Arguments(op,command.CommandLine,0);
            }
        }
        void Finish(object sender, TaskFinishedEventArgs value)
        {
            lock(sync)
            {
                Operation op;string key=Context(value.BuildEventContext);if(!active.TryGetValue(key,out op))return;
                // Native VC tasks need not emit structured Sources events.
                // Count actual compiler source arguments; a no-op CL task has
                // no command. Unexpanded response/module cases remain unknown.
                bool known=op.Name=="compile"&&(!op.CommandObserved||(!op.Unexpanded&&op.Invoked.Count>0));
                double elapsed=(Stopwatch.GetTimestamp()-op.Tick)/(double)Stopwatch.Frequency;
                double wall=(value.Timestamp.ToUniversalTime()-op.Timestamp).TotalSeconds;
                Write("end",",\"id\":"+Json(op.Id)+",\"name\":"+Json(op.Name)+",\"elapsedSeconds\":"+elapsed.ToString("R",CultureInfo.InvariantCulture)+",\"sourceElapsedSeconds\":"+(wall>=0?wall.ToString("R",CultureInfo.InvariantCulture):"null")+",\"exitCode\":"+(value.Succeeded?"0":"1")+",\"compilerInputs\":"+(op.InputsObserved?op.Inputs.Count.ToString(CultureInfo.InvariantCulture):"null")+",\"attemptedUnits\":"+(known?op.Invoked.Count.ToString(CultureInfo.InvariantCulture):"null")+",\"compilerCommandObserved\":"+(op.CommandObserved?"true":"false")+",\"unitCountKnown\":"+(known?"true":"false")+",\"measurement\":\"central logger monotonic event receipt interval\"");
                active.Remove(key);
            }
        }
        public void Shutdown()
        {
            lock(sync){if(output==null)return;Write("collector-end",",\"unfinishedTasks\":"+active.Count);output.Dispose();output=null;}
        }
    }
}
