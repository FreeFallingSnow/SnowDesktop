param([string]$Root)
$ErrorActionPreference='Stop'
# C++ drives the tests. One real PowerShell host per fixture keeps engine setup
# out of each assertion; each script still receives a fresh script scope.
Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Text;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Globalization;
using System.Management.Automation;
using System.Management.Automation.Host;
using System.Management.Automation.Runspaces;
public sealed class NativeTestUI : PSHostUserInterface {
 public override PSHostRawUserInterface RawUI { get { return null; } }
 public override string ReadLine() { throw new InvalidOperationException("Unexpected interactive test prompt"); }
 public override System.Security.SecureString ReadLineAsSecureString() { throw new InvalidOperationException("Unexpected credential prompt"); }
 public override void Write(string value) { Console.Out.Write(value); }
 public override void Write(ConsoleColor foreground,ConsoleColor background,string value) { Write(value); }
 public override void WriteLine(string value) { Console.Out.WriteLine(value); }
 public override void WriteErrorLine(string value) { Console.Error.WriteLine(value); }
 public override void WriteDebugLine(string value) { Console.Error.WriteLine(value); }
 public override void WriteProgress(long source,ProgressRecord record) {}
 public override void WriteVerboseLine(string value) { Console.Error.WriteLine(value); }
 public override void WriteWarningLine(string value) { Console.Error.WriteLine(value); }
 public override Dictionary<string,PSObject> Prompt(string caption,string message,Collection<FieldDescription> descriptions) { throw new InvalidOperationException("Unexpected prompt"); }
 public override PSCredential PromptForCredential(string caption,string message,string user,string target) { throw new InvalidOperationException("Unexpected credential prompt"); }
 public override PSCredential PromptForCredential(string caption,string message,string user,string target,PSCredentialTypes types,PSCredentialUIOptions options) { throw new InvalidOperationException("Unexpected credential prompt"); }
 public override int PromptForChoice(string caption,string message,Collection<ChoiceDescription> choices,int choice) { throw new InvalidOperationException("Unexpected confirmation"); }
}
public sealed class NativeTestHost : PSHost {
 readonly Guid id=Guid.NewGuid(); readonly NativeTestUI ui=new NativeTestUI();
 public int? ExitCode;
 public override Guid InstanceId {get{return id;}}
 public override string Name {get{return "SnowDesktop native test fixture";}}
 public override Version Version {get{return new Version(1,0);}}
 public override PSHostUserInterface UI {get{return ui;}}
 public override CultureInfo CurrentCulture {get{return CultureInfo.CurrentCulture;}}
 public override CultureInfo CurrentUICulture {get{return CultureInfo.CurrentUICulture;}}
 public override void SetShouldExit(int code) {ExitCode=code;}
 public override void EnterNestedPrompt() {throw new InvalidOperationException("Unexpected nested prompt");}
 public override void ExitNestedPrompt() {}
 public override void NotifyBeginApplication() {}
 public override void NotifyEndApplication() {}
}
public static class NativeTestBridge {
 static string Decode(string value) {return Encoding.UTF8.GetString(Convert.FromBase64String(value));}
 static string Encode(string value) {return Convert.ToBase64String(Encoding.UTF8.GetBytes(value));}
 static string Literal(string value) {return "'"+value.Replace("'","''")+"'";}
 public static void Run(string root) {
  TextWriter originalOut=Console.Out,originalErr=Console.Error;
  var host=new NativeTestHost();
  using(Runspace space=RunspaceFactory.CreateRunspace(host)) {
   space.Open();
   File.WriteAllText(Path.Combine(root,"ready"),"ready");
   string line;
   while((line=Console.ReadLine())!=null&&line!="quit") {
    string[] fields=line.Split('\t');string id=fields[0];int code=0;
    var output=new StringWriter();var error=new StringWriter();
    var previous=new Dictionary<string,string>();
    string originalDirectory=Environment.CurrentDirectory;
    try {
     host.ExitCode=null;space.SessionStateProxy.SetVariable("LASTEXITCODE",null);
     Environment.CurrentDirectory=Decode(fields[1]);
     if(fields.Length>4)foreach(string pair in fields[4].Split(','))if(pair.Length>0) {int separator=pair.IndexOf(':');string key=Decode(pair.Substring(0,separator));previous[key]=Environment.GetEnvironmentVariable(key);Environment.SetEnvironmentVariable(key,Decode(pair.Substring(separator+1)));}
     Console.SetOut(output);Console.SetError(error);
     using(PowerShell command=PowerShell.Create()) {
      command.Runspace=space;
      command.AddScript("Set-Location -LiteralPath "+Literal(Environment.CurrentDirectory),true).Invoke();command.Commands.Clear();command.Streams.ClearStreams();
      string expression="& "+Literal(Decode(fields[2]));
      foreach(string argument in fields[3].Split(','))if(argument.Length>0) {string value=Decode(argument);expression+=" "+(System.Text.RegularExpressions.Regex.IsMatch(value,@"^-[A-Za-z][A-Za-z0-9]*(?::\$(?:true|false))?$")?value:Literal(value));}
      foreach(PSObject value in command.AddScript(expression,true).Invoke())if(value!=null)output.WriteLine(value.ToString());
      foreach(ErrorRecord value in command.Streams.Error)error.WriteLine(value.ToString());
      object nativeExit=space.SessionStateProxy.GetVariable("LASTEXITCODE");
      code=host.ExitCode.HasValue?host.ExitCode.Value:nativeExit!=null?Convert.ToInt32(nativeExit,CultureInfo.InvariantCulture):command.HadErrors?1:0;
     }
    } catch(Exception exception) {code=host.ExitCode.HasValue?host.ExitCode.Value:1;error.WriteLine(exception.ToString());}
    finally {Console.SetOut(originalOut);Console.SetError(originalErr);Environment.CurrentDirectory=originalDirectory;foreach(var pair in previous)Environment.SetEnvironmentVariable(pair.Key,pair.Value);}
    string response=code.ToString(CultureInfo.InvariantCulture)+"\n"+Encode(output.ToString())+"\n"+Encode(error.ToString())+"\n";
    string temporary=Path.Combine(root,id+".pending");File.WriteAllText(temporary,response,new UTF8Encoding(false));File.Move(temporary,Path.Combine(root,id+".response"));
   }
  }
 }
}
'@
[NativeTestBridge]::Run($Root)
