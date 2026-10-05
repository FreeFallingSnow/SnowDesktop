#include "build_tool_test_support.h"
#include "json_value.h"
#include <algorithm>
#include <atomic>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace build_test {
namespace {
Json convert(const JsonValue& value) {
    Json out;
    out.kind=static_cast<Json::Kind>(value.type);
    out.boolean=value.boolean; out.number=value.number; out.text=value.string;
    for(const auto& child:value.array) out.items.push_back(convert(child));
    for(const auto& [key,child]:value.object) out.fields.emplace(key,convert(child));
    return out;
}
std::string escaped(const std::string& value) {
    std::string out="\"";
    const char* hex="0123456789abcdef";
    for(unsigned char c:value) {
        if(c=='"'||c=='\\') {out+='\\';out+=static_cast<char>(c);}
        else if(c<32) {out+="\\u00";out+=hex[c>>4];out+=hex[c&15];}
        else out+=static_cast<char>(c);
    }
    return out+'"';
}
struct CompareEnv { bool operator()(const std::wstring& a,const std::wstring& b) const {return _wcsicmp(a.c_str(),b.c_str())<0;} };
std::vector<wchar_t> child_environment(const Env& patch) {
    std::map<std::wstring,std::wstring,CompareEnv> values;
    wchar_t* block=GetEnvironmentStringsW();
    require(block!=nullptr,"GetEnvironmentStrings failed");
    for(const wchar_t* p=block;*p;p+=wcslen(p)+1) {
        std::wstring item(p);auto equal=item.find('=',item[0]=='='?1:0);
        if(equal!=std::wstring::npos) values[item.substr(0,equal)]=item.substr(equal+1);
    }
    FreeEnvironmentStringsW(block);
    values.erase(L"SNOWDESKTOP_EXECUTION_TOKEN");
    values[L"PSExecutionPolicyPreference"]=L"Bypass";
    for(const auto& [key,value]:patch) values[key]=value;
    std::vector<wchar_t> out;
    for(const auto& [key,value]:values) {const auto item=key+L"="+value;out.insert(out.end(),item.begin(),item.end());out.push_back(0);}
    out.push_back(0);return out;
}
std::uint64_t birth(HANDLE handle) {
    FILETIME created{},exit{},kernel{},user{};
    if(!GetProcessTimes(handle,&created,&exit,&kernel,&user)) return 0;
    return (static_cast<std::uint64_t>(created.dwHighDateTime)<<32)|created.dwLowDateTime;
}
std::string drain_pipe(HANDLE handle) {
    std::string out;auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(1);
    while(true) {
        DWORD available=0;
        if(!PeekNamedPipe(handle,nullptr,0,nullptr,&available,nullptr)) {
            require(GetLastError()==ERROR_BROKEN_PIPE,"Read child output failed");return out;
        }
        if(available) {
            char buffer[4096];DWORD count=0;
            require(ReadFile(handle,buffer,std::min<DWORD>(available,static_cast<DWORD>(sizeof(buffer))),&count,nullptr)!=0,"Read child output failed");out.append(buffer,count);
        } else {
            require(std::chrono::steady_clock::now()<deadline,"Completed parent output pipe is still held by a descendant");
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
}
}
Json& Json::operator[](const std::string& key) {if(kind==Kind::Null)kind=Kind::Object;require(kind==Kind::Object,"JSON is not an object");return fields[key];}
const Json& Json::at(const std::string& key) const {auto found=fields.find(key);require(found!=fields.end(),"Missing JSON field "+key+": "+dump());return found->second;}
bool Json::has(const std::string& key) const {return fields.find(key)!=fields.end();}
std::string Json::str() const {require(kind==Kind::String,"JSON is not a string: "+dump());return text;}
int Json::integer() const {require(kind==Kind::Number,"JSON is not a number");return static_cast<int>(number);}
std::string Json::dump() const {
    if(kind==Kind::Null)return "null";
    if(kind==Kind::Bool)return boolean?"true":"false";
    if(kind==Kind::Number){std::ostringstream stream;stream.precision(17);stream<<number;return stream.str();}
    if(kind==Kind::String)return escaped(text);
    std::string out=kind==Kind::Array?"[":"{";bool first=true;
    auto comma=[&] {if(!first)out+=',';first=false;};
    if(kind==Kind::Array) for(const auto& value:items){comma();out+=value.dump();}
    else for(const auto& [key,value]:fields){comma();out+=escaped(key)+":"+value.dump();}
    return out+(kind==Kind::Array?"]":"}");
}
Json Json::parse(const std::string& value) {JsonValue parsed;std::string error;std::string_view input(value);if(input.starts_with("\xef\xbb\xbf"))input.remove_prefix(3);require(ParseJson(input,parsed,&error),"Invalid JSON: "+error+"\n"+value);return convert(parsed);}
Json Json::array(std::initializer_list<Json> value) {Json out;out.kind=Kind::Array;out.items=value;return out;}
Json Json::object(std::initializer_list<std::pair<const std::string,Json>> value) {Json out;out.kind=Kind::Object;out.fields=value;return out;}
void require(bool condition,const std::string& message) {if(!condition)throw std::runtime_error(message);}
std::string read(const fs::path& path) {
    HANDLE handle=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
    require(handle!=INVALID_HANDLE_VALUE,"Cannot read "+utf8(path.wstring())+": "+std::to_string(GetLastError()));
    try {
        LARGE_INTEGER size{};require(GetFileSizeEx(handle,&size)!=0&&size.QuadPart>=0&&size.QuadPart<=64*1024*1024,"Invalid/oversized fixture file");
        std::string out(static_cast<size_t>(size.QuadPart),0);size_t offset=0;
        while(offset<out.size()){DWORD count=0;DWORD requested=static_cast<DWORD>(std::min<size_t>(out.size()-offset,65536));require(ReadFile(handle,out.data()+offset,requested,&count,nullptr)!=0,"Fixture read failed");if(!count)break;offset+=count;}
        out.resize(offset);CloseHandle(handle);return out;
    } catch(...){CloseHandle(handle);throw;}
}
void write(const fs::path& path,const std::string& text,bool bom) {fs::create_directories(path.parent_path());std::ofstream output(path,std::ios::binary);require(static_cast<bool>(output),"Cannot write fixture");if(bom)output<<"\xef\xbb\xbf";output<<text;require(static_cast<bool>(output),"Fixture write failed");}
void atomic(const fs::path& path,const std::string& text) {static std::atomic<unsigned> sequence=0;auto temporary=path.wstring()+L"."+std::to_wstring(GetCurrentProcessId())+L"."+std::to_wstring(sequence++)+L".pending";write(temporary,text);const bool moved=MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0;require(moved,"Fixture atomic replacement failed: "+std::to_string(GetLastError()));}
void copy(const fs::path& repo,const fs::path& root,const std::vector<std::string>& files) {for(const auto& file:files){fs::create_directories((root/file).parent_path());fs::copy_file(repo/file,root/file,fs::copy_options::overwrite_existing);}}
std::string utf8(const std::wstring& value) {if(value.empty())return {};int count=WideCharToMultiByte(CP_UTF8,0,value.data(),static_cast<int>(value.size()),nullptr,0,nullptr,nullptr);std::string out(static_cast<size_t>(count),0);WideCharToMultiByte(CP_UTF8,0,value.data(),static_cast<int>(value.size()),out.data(),count,nullptr,nullptr);return out;}
std::wstring wide(const std::string& value) {if(value.empty())return {};int count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),nullptr,0);require(count>0,"Invalid UTF-8");std::wstring out(static_cast<size_t>(count),0);MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),out.data(),count);return out;}
std::wstring environment(const wchar_t* name) {DWORD size=GetEnvironmentVariableW(name,nullptr,0);if(!size)return {};std::wstring out(size,0);DWORD count=GetEnvironmentVariableW(name,out.data(),size);out.resize(count);return out;}
std::wstring quote(const std::wstring& value) {std::wstring out=L"\"";size_t slashes=0;for(wchar_t c:value){if(c==L'\\'){++slashes;continue;}out.append(slashes*(c==L'"'?2:1)+(c==L'"'?1:0),L'\\');slashes=0;out+=c;}out.append(slashes*2,L'\\');return out+L'"';}
std::string ps_literal(const std::string& value) {std::string out="'";for(char c:value){out+=c;if(c=='\'')out+=c;}return out+"'";}
std::string ps_literal(const fs::path& value) {return ps_literal(utf8(value.wstring()));}
std::string base64(const std::string& value) {static const char* alphabet="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";std::string out;unsigned accumulator=0;int bits=0;for(unsigned char c:value){accumulator=(accumulator<<8)|c;bits+=8;while(bits>=6){bits-=6;out+=alphabet[(accumulator>>bits)&63];}}if(bits)out+=alphabet[(accumulator<<(6-bits))&63];while(out.size()%4)out+='=';return out;}
std::string unbase64(const std::string& value) {std::string out;unsigned accumulator=0;int bits=0;for(char c:value){if(c=='=')break;const std::string alphabet="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";auto index=alphabet.find(c);require(index!=std::string::npos,"Invalid base64 response");accumulator=(accumulator<<6)|static_cast<unsigned>(index);bits+=6;if(bits>=8){bits-=8;out+=static_cast<char>((accumulator>>bits)&255);}}return out;}
fs::path temporary(const std::string& label) {static std::atomic<unsigned> sequence=0;auto root=fs::temp_directory_path()/wide("SnowDesktop-native-"+label+"-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(sequence++));require(fs::create_directory(root),"Temporary fixture already exists");std::cout<<"Fixture: "<<utf8(root.wstring())<<std::endl;return root;}
fs::path executable() {std::wstring out(32768,0);DWORD size=GetModuleFileNameW(nullptr,out.data(),static_cast<DWORD>(out.size()));require(size>0,"Executable path unavailable");out.resize(size);return out;}
fs::path powershell(bool legacy) {if(!legacy){auto pinned=environment(L"SNOWDESKTOP_ENTRY_POWERSHELL");if(!pinned.empty()&&fs::is_regular_file(pinned))return pinned;auto core=fs::path(environment(L"ProgramFiles"))/L"PowerShell/7/pwsh.exe";if(fs::is_regular_file(core))return core;}auto fallback=fs::path(environment(L"SystemRoot"))/L"System32/WindowsPowerShell/v1.0/powershell.exe";require(fs::is_regular_file(fallback),"Windows PowerShell unavailable");return fallback;}
fs::path production_python() {std::wstring path(32768,0);DWORD count=SearchPathW(nullptr,L"python.exe",nullptr,static_cast<DWORD>(path.size()),path.data(),nullptr);require(count>0&&count<path.size(),"Python is unavailable for the existing external production tool (the test driver is native C++)");path.resize(count);return path;}
fs::path standin() {auto path=executable().parent_path()/L"SnowDesktopBuildToolTestStandIn.exe";require(fs::is_regular_file(path),"Native stand-in missing");return path;}
void replace(std::string& text,const std::string& before,const std::string& after) {auto position=text.find(before);require(position!=std::string::npos,"Fixture substitution missing: "+before);text.replace(position,before.size(),after);}
void until(const std::function<bool()>& predicate,int seconds) {auto end=std::chrono::steady_clock::now()+std::chrono::seconds(seconds);do {if(predicate())return;std::this_thread::sleep_for(std::chrono::milliseconds(40));}while(std::chrono::steady_clock::now()<end);throw std::runtime_error("Fixture condition timed out");}
Child::Child(const std::vector<std::wstring>& args,const fs::path& cwd,const Env& env,bool input,bool outputPipes):outputPipes_(outputPipes) {
    require(!args.empty(),"Missing command");static std::atomic<unsigned> sequence=0;
    auto stem="process-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(sequence++);
    out_=cwd/L".build/native-test-output"/wide(stem+".out");err_=cwd/L".build/native-test-output"/wide(stem+".err");fs::create_directories(out_.parent_path());
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES),nullptr,TRUE};
    HANDLE in=nullptr,out=nullptr,err=nullptr;LPPROC_THREAD_ATTRIBUTE_LIST attributes=nullptr;
    std::vector<unsigned char> storage;PROCESS_INFORMATION process{};bool initialized=false;
    try {
        if(input){require(CreatePipe(&in,&input_,&security,0)!=0,"CreatePipe failed");require(SetHandleInformation(input_,HANDLE_FLAG_INHERIT,0)!=0,"SetHandleInformation failed");}
        else in=CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&security,OPEN_EXISTING,0,nullptr);
        if(outputPipes) {
            require(CreatePipe(&outRead_,&out,&security,0)!=0&&CreatePipe(&errRead_,&err,&security,0)!=0,"Output pipe creation failed");
            require(SetHandleInformation(outRead_,HANDLE_FLAG_INHERIT,0)!=0&&SetHandleInformation(errRead_,HANDLE_FLAG_INHERIT,0)!=0,"Output read handle inheritance failed");
            write(out_,"");write(err_,"");
        } else {
            out=CreateFileW(out_.c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,&security,CREATE_ALWAYS,0,nullptr);
            err=CreateFileW(err_.c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,&security,CREATE_ALWAYS,0,nullptr);
        }
        require(in!=INVALID_HANDLE_VALUE&&out!=INVALID_HANDLE_VALUE&&err!=INVALID_HANDLE_VALUE,"Cannot open child streams");
        SIZE_T size=0;InitializeProcThreadAttributeList(nullptr,1,0,&size);storage.resize(size);attributes=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        require(InitializeProcThreadAttributeList(attributes,1,0,&size)!=0,"InitializeProcThreadAttributeList failed");initialized=true;
        HANDLE handles[]={in,out,err};require(UpdateProcThreadAttribute(attributes,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,handles,sizeof(handles),nullptr,nullptr)!=0,"Handle whitelist failed");
        STARTUPINFOEXW startup{};startup.StartupInfo.cb=sizeof(startup);startup.StartupInfo.dwFlags=STARTF_USESTDHANDLES;startup.StartupInfo.hStdInput=in;startup.StartupInfo.hStdOutput=out;startup.StartupInfo.hStdError=err;startup.lpAttributeList=attributes;
        std::wstring command;const bool cmd=_wcsicmp(fs::path(args[0]).filename().c_str(),L"cmd.exe")==0;
        for(const auto& arg:args){if(!command.empty())command+=L' ';command+=(cmd&&arg.starts_with(L"call "))?arg:quote(arg);}auto block=child_environment(env);
        const bool launched=CreateProcessW(nullptr,command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW|CREATE_UNICODE_ENVIRONMENT|EXTENDED_STARTUPINFO_PRESENT,block.data(),cwd.c_str(),&startup.StartupInfo,&process)!=0;
        require(launched,"CreateProcess failed: "+std::to_string(GetLastError()));
        process_=process.hProcess;pid_=process.dwProcessId;CloseHandle(process.hThread);
    } catch (...) {for(HANDLE handle:{input_,outRead_,errRead_})if(handle)CloseHandle(handle);input_=outRead_=errRead_=nullptr;if(initialized)DeleteProcThreadAttributeList(attributes);for(HANDLE handle:{in,out,err})if(handle&&handle!=INVALID_HANDLE_VALUE)CloseHandle(handle);throw;}
    if(initialized)DeleteProcThreadAttributeList(attributes);for(HANDLE handle:{in,out,err})CloseHandle(handle);
}
Child::~Child() {stop();for(HANDLE handle:{input_,outRead_,errRead_,process_})if(handle)CloseHandle(handle);}
bool Child::running() const {return process_&&WaitForSingleObject(process_,0)==WAIT_TIMEOUT;}
void Child::send(const std::string& value) {require(input_!=nullptr,"Child has no input channel");DWORD bytes=0;require(WriteFile(input_,value.data(),static_cast<DWORD>(value.size()),&bytes,nullptr)!=0&&bytes==value.size(),"Child request write failed");}
std::string Child::out() const {return read(out_);}
std::string Child::err() const {return read(err_);}
void Child::stop() {if(running()){TerminateProcess(process_,4);WaitForSingleObject(process_,5000);}}
Result Child::wait(int seconds) {DWORD status=WaitForSingleObject(process_,static_cast<DWORD>(seconds)*1000);if(status!=WAIT_OBJECT_0){stop();throw std::runtime_error("Child timeout: "+std::to_string(pid_)+"\n"+out()+"\n"+err());}DWORD code=0;require(GetExitCodeProcess(process_,&code)!=0,"Exit code unavailable");if(outputPipes_){write(out_,drain_pipe(outRead_));write(err_,drain_pipe(errRead_));}return {static_cast<int>(code),out(),err()};}
Result run(const std::vector<std::wstring>& args,const fs::path& root,int seconds,const Env& env) {Child child(args,root,env);return child.wait(seconds);}
void init_git(const fs::path& root) {for(const auto& args:std::vector<std::vector<std::wstring>>{{L"git",L"init",L"--quiet"},{L"git",L"add",L"."},{L"git",L"-c",L"user.name=Fixture",L"-c",L"user.email=fixture@example.invalid",L"commit",L"--quiet",L"-m",L"fixture"}}){auto result=run(args,root);require(result.code==0,result.err);}}
Bridge::Bridge(const fs::path& repo,const fs::path& root,bool legacy):root_(root),state_(root/L".build/native-test-bridge") {
    fs::create_directories(state_);fs::copy_file(repo/L"tests/build_tool_bridge.ps1",state_/L"bridge.ps1",fs::copy_options::overwrite_existing);
    for(int attempt=0;attempt<2;++attempt) {
        auto ready=state_/L"ready";fs::remove(ready);auto engine=powershell(legacy||attempt==1);
        child_=std::make_unique<Child>(std::vector<std::wstring>{engine.wstring(),L"-NoProfile",L"-NonInteractive",L"-File",(state_/L"bridge.ps1").wstring(),L"-Root",state_.wstring()},root,Env{{L"SNOWDESKTOP_ENTRY_POWERSHELL",engine.wstring()}},true);
        const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(6);
        while(child_->running()&&!fs::exists(ready)&&std::chrono::steady_clock::now()<end)std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if(fs::exists(ready)&&child_->running())return;
        child_->stop();write(state_/wide("startup-"+std::to_string(attempt)+".json"),Json::object({{"scriptStarted",false},{"runtime",utf8(engine.wstring())},{"stdout",child_->out()},{"stderr",child_->err()}}).dump());child_.reset();
    }
    throw std::runtime_error("Test bridge failed before target execution; startup evidence retained");
}
Bridge::~Bridge() {if(child_&&child_->running())try {child_->send("quit\n");child_->wait(5);}catch(...){child_->stop();}}
Result Bridge::invoke(const fs::path& script,const std::vector<std::string>& args,const Env& env,int seconds) {
    const std::string id=std::to_string(++sequence_);const auto response=state_/wide(id+".response");
    std::string request=id+'\t'+base64(utf8(root_.wstring()))+'\t'+base64(utf8(script.wstring()))+'\t';
    for(const auto& arg:args)request+=base64(arg)+",";
    request+='\t';for(const auto& [key,value]:env)request+=base64(utf8(key))+":"+base64(utf8(value))+",";
    child_->send(request+"\n");until([&]{require(child_->running(),"Bridge exited: "+child_->out()+"\n"+child_->err());return fs::exists(response);},seconds);
    std::istringstream stream(read(response));std::string code,out,err;std::getline(stream,code);std::getline(stream,out);std::getline(stream,err);
    return {std::stoi(code),unbase64(out),unbase64(err)};
}
Json Bridge::call(const fs::path& script,const std::vector<std::string>& args,int code,const Env& env) {auto result=invoke(script,args,env);require(result.code==code,"Unexpected target exit "+std::to_string(result.code)+" expected "+std::to_string(code)+"\n"+result.out+"\n"+result.err);return result.out.empty()?Json{}:Json::parse(result.out);}
Json owner(DWORD pid) {HANDLE handle=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,pid);require(handle!=nullptr,"Cannot inspect fixture owner");auto time=birth(handle);CloseHandle(handle);return Json::object({{"pid",static_cast<double>(pid)},{"startTicks",std::to_string(time+504911232000000000ULL)}});}
bool owner_alive(const Json& value) {if(!value.has("pid")||!value.has("startTicks"))return false;HANDLE handle=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,static_cast<DWORD>(value.at("pid").number));if(!handle)return false;bool alive=WaitForSingleObject(handle,0)==WAIT_TIMEOUT&&std::to_string(birth(handle)+504911232000000000ULL)==value.at("startTicks").str();CloseHandle(handle);return alive;}
void stop_owner(const Json& value) {if(!owner_alive(value))return;HANDLE handle=OpenProcess(PROCESS_TERMINATE|PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,static_cast<DWORD>(value.at("pid").number));if(!handle)return;if(std::to_string(birth(handle)+504911232000000000ULL)==value.at("startTicks").str()){TerminateProcess(handle,4);WaitForSingleObject(handle,5000);}CloseHandle(handle);}
int test_main(int argc,char** argv,const std::function<void(const fs::path&)>& test) {try {require(argc>=2,"Repository argument required");test(fs::path(wide(argv[1])));std::cout<<"PASSED"<<std::endl;return 0;}catch(const std::exception& error){std::cerr<<"FAILED: "<<error.what()<<std::endl;return 1;}}
Coordinator::Coordinator(const fs::path& repo,const std::string& label):root(temporary(label)),scripts(root/L"scripts"),stateRoot(root/L".build/collaboration") {
    copy(repo,root,{"scripts/build_entry.ps1","scripts/build_runtime.ps1","scripts/build_manager.ps1","scripts/build_inputs.ps1","scripts/build_job.cs","scripts/build_protocol.ps1","scripts/build_ownership.ps1","scripts/build_preflight.ps1","scripts/build_waiter.ps1","scripts/build_wait_tasks.py","tools/build-dashboard/server.py"});
    write(scripts/L"build_preflight.ps1",R"PS(param([Alias('ReloadShell')][switch]$ExecuteReloadShell,[Alias('CloseApplication')][switch]$ExecuteCloseApplication)
function Get-ReadOnlyPreflight([string]$Root) {
 $unknown=Test-Path (Join-Path $Root 'preflight-unknown');$blocked=Test-Path (Join-Path $Root 'preflight-blocked')
 [pscustomobject]@{status=$(if($unknown){'unknown'}elseif($blocked){'blocked'}else{'clear'});owners=@();unknownPids=$(if($unknown){@($PID)}else{@()});observedUtc=[DateTime]::UtcNow.ToString('o')}
}
if($MyInvocation.InvocationName -ne '.'){
 . (Join-Path $PSScriptRoot 'build_entry.ps1');$root=Split-Path $PSScriptRoot -Parent;Assert-EntryExecutionOwner $root
 if(Test-Path (Join-Path $root 'preflight-throw')){throw 'controlled owner preflight exception'}
 [IO.File]::WriteAllText((Join-Path $root 'preflight-arguments.txt'),$(if($ExecuteReloadShell){'--reload-shell'}elseif($ExecuteCloseApplication){'--close-application'}else{''}));exit 0
}
)PS",true);
    write(scripts/L"fake.ps1",R"PS(param([string]$Phase,[string]$BuildArgument='')
$ErrorActionPreference='Stop';$root=Split-Path $PSScriptRoot -Parent
[IO.File]::AppendAllText((Join-Path $root 'calls.txt'),$Phase+"`n")
if($Phase -eq 'build'){
 [IO.File]::WriteAllText((Join-Path $root 'child.json'),(ConvertTo-Json @{pid=$PID;startTicks=(Get-Process -Id $PID).StartTime.ToUniversalTime().Ticks.ToString()}))
 [IO.File]::WriteAllText((Join-Path $root 'build-arguments.txt'),$BuildArgument)
}
[IO.File]::WriteAllText((Join-Path $root ($Phase+'-started')),'started')
$timer=[Diagnostics.Stopwatch]::StartNew()
while(Test-Path (Join-Path $root ('hold-'+$Phase))){if($timer.Elapsed.TotalSeconds -gt 40){throw 'Fixture gate not released'};Start-Sleep -Milliseconds 40}
$failure=Join-Path $root ('fail-'+$Phase)
if(Test-Path $failure){Write-Output "controlled $Phase failure";exit ([int][IO.File]::ReadAllText($failure))}
Write-Output "controlled $Phase passed";exit 0
)PS",true);
    write(scripts/L"build.bat","@echo off\r\n\"%SNOWDESKTOP_ENTRY_POWERSHELL%\" -NoProfile -File \"%~dp0fake.ps1\" -Phase build -BuildArgument \"%~1\"\r\nexit /b %ERRORLEVEL%\r\n");
    write(scripts/L"build_batch_tests.ps1","param([string]$Batch)\n& (Join-Path $PSScriptRoot 'fake.ps1') -Phase test;exit $LASTEXITCODE\n",true);
    for(const auto& path:{"src/a.txt","src/b.txt","src/other.txt","src/fixture.cpp","src/winui/presenter.cpp","src/core/desktop.cpp"})write(root/path,"original\n");
    write(root/L".gitignore","/*\n!/scripts/\n!/src/\n!/.gitignore\n!/CMakePresets.json\n");init_git(root);
    bridge=std::make_unique<Bridge>(repo,root);
}
Coordinator::~Coordinator(){for(const auto& worker:workers)stop_owner(worker);}
Json Coordinator::call(std::vector<std::string> args,int code){return bridge->call(scripts/L"build_manager.ps1",args,code);}
std::unique_ptr<Child> Coordinator::start(std::vector<std::string> args){std::vector<std::wstring> command={powershell().wstring(),L"-NoProfile",L"-NonInteractive",L"-File",(scripts/L"build_manager.ps1").wstring()};for(const auto& arg:args)command.push_back(wide(arg));return std::make_unique<Child>(command,root);}
Json Coordinator::state() const{return Json::parse(read(stateRoot/L"state.json"));}
Json Coordinator::result(const std::string& batch) const{return Json::parse(read(stateRoot/wide(batch+".json")));}
Json Coordinator::begin(const std::string& task){return call({"begin",task});}
std::vector<std::string> Coordinator::bound(const std::string& action,const std::string& task,const std::string& batch,int revision)const{return {action,task,"-Batch",batch,"-Revision",std::to_string(revision)};}
Json Coordinator::plan(const std::string& task,const std::string& batch,int revision,const std::string& suite,const std::string& tests,const std::string& scope,const std::string& inputs){auto args=bound("plan",task,batch,revision);args.insert(args.end(),{"-Scope",scope,"-Suites",suite,"-Inputs",inputs,"-Reason","reviewed isolated fixture dependency mapping"});if(!tests.empty())args.insert(args.end(),{"-Tests",tests});return call(args);}
Json Coordinator::ready(const std::string& task,const std::string& batch,int revision,int code){auto value=call(bound("ready",task,batch,revision),code);if(value.has("waiter")&&value.at("waiter").has("pid"))workers.push_back(value.at("waiter"));return value;}
Json Coordinator::finish(const std::string& task,const std::string& batch,int revision,int code){return call(bound("finish",task,batch,revision),code);}
int Coordinator::calls() const{if(!fs::exists(root/L"calls.txt"))return 0;auto value=read(root/L"calls.txt");return static_cast<int>(std::count(value.begin(),value.end(),'\n'));}
void Coordinator::gate(const std::string& name,bool present){if(present)write(root/name,"hold");else fs::remove(root/name);}
Result Coordinator::git(const std::vector<std::wstring>& args){std::vector<std::wstring> command={L"git",L"-c",L"user.name=Fixture",L"-c",L"user.email=fixture@example.invalid"};command.insert(command.end(),args.begin(),args.end());auto value=run(command,root);require(value.code==0,value.err);return value;}
}
