#include "../build_tool_test_support.h"
#include <bcrypt.h>
#include <psapi.h>
#include <array>
#include <iostream>

using namespace build_test;
namespace {
struct Handle {
    HANDLE value=nullptr;
    explicit Handle(HANDLE handle=nullptr):value(handle){}
    ~Handle(){if(value&&value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
    Handle(const Handle&)=delete;Handle& operator=(const Handle&)=delete;
};
std::wstring normalized(std::wstring path){
    if(path.starts_with(L"\\\\?\\UNC\\"))return L"\\\\"+path.substr(8);
    if(path.starts_with(L"\\\\?\\"))return path.substr(4);
    return path;
}
std::wstring image_path(HANDLE file,HANDLE process,void* base){
    std::wstring path(32768,L'\0');
    DWORD count=file?GetFinalPathNameByHandleW(file,path.data(),static_cast<DWORD>(path.size()),FILE_NAME_NORMALIZED):0;
    if(count&&count<path.size()){path.resize(count);return normalized(path);}
    count=K32GetMappedFileNameW(process,base,path.data(),static_cast<DWORD>(path.size()));
    if(!count||count>=path.size())return {};
    path.resize(count);
    for(wchar_t letter=L'A';letter<=L'Z';++letter){
        wchar_t drive[]{letter,L':',L'\0'};std::array<wchar_t,32768> device{};
        if(QueryDosDeviceW(drive,device.data(),static_cast<DWORD>(device.size()))){
            const std::wstring prefix=device.data();
            if(path.starts_with(prefix+L"\\"))return std::wstring(drive)+path.substr(prefix.size());
        }
    }
    return {};
}
std::string hash_file(HANDLE file){
    LARGE_INTEGER size{},zero{};
    require(GetFileSizeEx(file,&size)&&size.QuadPart>=0&&size.QuadPart<=1024LL*1024*1024,"Module file size unavailable or exceeds budget");
    require(SetFilePointerEx(file,zero,nullptr,FILE_BEGIN)!=0,"Cannot seek module file");
    BCRYPT_ALG_HANDLE algorithm=nullptr;BCRYPT_HASH_HANDLE hash=nullptr;
    require(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)>=0,"Cannot open module SHA256 provider");
    try {
        DWORD objectSize=0,written=0;
        require(BCryptGetProperty(algorithm,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&objectSize),sizeof(objectSize),&written,0)>=0,"Cannot query module SHA256 state");
        std::vector<unsigned char> object(objectSize),chunk(65536);std::array<unsigned char,32> digest{};
        require(BCryptCreateHash(algorithm,&hash,object.data(),objectSize,nullptr,0,0)>=0,"Cannot create module SHA256 state");
        for(;;){DWORD count=0;require(ReadFile(file,chunk.data(),static_cast<DWORD>(chunk.size()),&count,nullptr)!=0,"Cannot read loaded module file");if(!count)break;require(BCryptHashData(hash,chunk.data(),count,0)>=0,"Cannot hash loaded module file");}
        require(BCryptFinishHash(hash,digest.data(),static_cast<ULONG>(digest.size()),0)>=0,"Cannot finish module SHA256");
        BCryptDestroyHash(hash);hash=nullptr;BCryptCloseAlgorithmProvider(algorithm,0);algorithm=nullptr;
        const char* digits="0123456789abcdef";std::string result;for(auto byte:digest){result+=digits[byte>>4];result+=digits[byte&15];}return result;
    }catch(...){if(hash)BCryptDestroyHash(hash);if(algorithm)BCryptCloseAlgorithmProvider(algorithm,0);throw;}
}
std::vector<wchar_t> child_environment(){
    wchar_t* block=GetEnvironmentStringsW();require(block!=nullptr,"Cannot read observer environment");std::vector<wchar_t> result;
    for(const wchar_t* value=block;*value;value+=wcslen(value)+1){
        const std::wstring entry=value;
        constexpr wchar_t reportVariable[]=L"SNOWDESKTOP_TEST_RUNTIME_REPORT_ROOT=";
        if(_wcsnicmp(entry.c_str(),reportVariable,std::size(reportVariable)-1)==0)continue;
        result.insert(result.end(),entry.begin(),entry.end());result.push_back(L'\0');
    }
    FreeEnvironmentStringsW(block);result.push_back(L'\0');return result;
}
void save_report(const fs::path& path,const Json& report){
    fs::create_directories(path.parent_path());const auto pending=fs::path(path.wstring()+L"."+std::to_wstring(GetCurrentProcessId())+L".pending");
    const auto bytes=report.dump();
    {
        Handle output(CreateFileW(pending.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
        require(output.value!=INVALID_HANDLE_VALUE,"Cannot create unique module report staging file");DWORD count=0;
        require(bytes.size()<=8*1024*1024&&WriteFile(output.value,bytes.data(),static_cast<DWORD>(bytes.size()),&count,nullptr)&&count==bytes.size()&&FlushFileBuffers(output.value),"Cannot write module report");
    }
    if(!MoveFileExW(pending.c_str(),path.c_str(),MOVEFILE_WRITE_THROUGH)){DeleteFileW(pending.c_str());throw std::runtime_error("Cannot publish module report without replacing existing data");}
}
struct Process {HANDLE handle=nullptr;bool initialBreakpoint=false;std::map<void*,size_t> bases;};
struct EntryBreakpoint {
    void* address=nullptr;BYTE original=0;DWORD rva=0;
    template<class T> static T read(HANDLE process,const void* address){
        T value{};SIZE_T count=0;
        require(ReadProcessMemory(process,address,&value,sizeof(value),&count)&&count==sizeof(value),"Cannot read native image headers");return value;
    }
    static void patch(HANDLE process,void* address,BYTE value){
        DWORD protection=0;require(VirtualProtectEx(process,address,1,PAGE_EXECUTE_READWRITE,&protection)!=0,"Cannot protect owned image entry breakpoint");
        SIZE_T count=0;const bool written=WriteProcessMemory(process,address,&value,1,&count)&&count==1;
        DWORD previous=0;const bool restored=VirtualProtectEx(process,address,1,protection,&previous)!=0;
        require(written&&restored&&FlushInstructionCache(process,address,1),"Cannot write/restore owned image entry breakpoint");
    }
    void install(HANDLE process,void* base){
        const auto* bytes=static_cast<const BYTE*>(base);const auto dos=read<IMAGE_DOS_HEADER>(process,base);
        require(dos.e_magic==IMAGE_DOS_SIGNATURE&&dos.e_lfanew>=static_cast<LONG>(sizeof(IMAGE_DOS_HEADER))&&dos.e_lfanew<=1024*1024,"Unsupported image DOS header");
        const auto headers=read<IMAGE_NT_HEADERS64>(process,bytes+dos.e_lfanew);
        require(headers.Signature==IMAGE_NT_SIGNATURE&&headers.FileHeader.Machine==IMAGE_FILE_MACHINE_AMD64&&headers.FileHeader.SizeOfOptionalHeader==sizeof(IMAGE_OPTIONAL_HEADER64)&&headers.OptionalHeader.Magic==IMAGE_NT_OPTIONAL_HDR64_MAGIC,"Entry probe requires a native AMD64 PE64 image");
        const auto& optional=headers.OptionalHeader;
        require(optional.NumberOfRvaAndSizes<=IMAGE_NUMBEROF_DIRECTORY_ENTRIES&&optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR].VirtualAddress==0,"Managed image entry probes are unsupported");
        rva=optional.AddressOfEntryPoint;require(rva>0&&rva<optional.SizeOfImage,"Native image entry point is unavailable");
        address=static_cast<BYTE*>(base)+rva;original=read<BYTE>(process,address);require(original!=0xcc,"Native entry already contains a breakpoint");
        patch(process,address,0xcc);
    }
    void restore(HANDLE process)const{patch(process,address,original);}
};
int observe(int argc,wchar_t** argv){
    std::wstring name;fs::path report;ULONGLONG timeout=0;bool probe=false,entryProbe=false;int first=0;
    for(int i=1;i<argc;++i){const std::wstring option=argv[i];
        if(option==L"--"){first=i+1;break;}
        if(option==L"--probe"){probe=true;continue;}
        if(option==L"--entry-probe"){entryProbe=true;continue;}
        require(i+1<argc,"Missing observer option value");const std::wstring value=argv[++i];
        if(option==L"--test")name=value;
        else if(option==L"--report")report=value;
        else if(option==L"--timeout-ms"){require(!value.empty()&&value.find_first_not_of(L"0123456789")==std::wstring::npos,"Invalid observer timeout");timeout=std::stoull(value);require(timeout<=300000,"Observer timeout exceeds budget");}
        else throw std::runtime_error("Unknown runtime observer option");
    }
    require(first>0&&first<argc&&!name.empty()&&name.size()<=160,"Observer requires --test NAME -- PROGRAM [ARGUMENTS]");
    require(!(probe&&entryProbe),"Select only one runtime probe boundary");
    for(wchar_t value:name)require((value>=L'A'&&value<=L'Z')||(value>=L'a'&&value<=L'z')||(value>=L'0'&&value<=L'9')||value==L'_'||value==L'-',"Invalid runtime observation name");
    if(report.empty()){
        auto root=environment(L"SNOWDESKTOP_TEST_RUNTIME_REPORT_ROOT");if(root.empty())root=(fs::current_path()/L"Testing/runtime-modules").wstring();
        report=fs::path(root)/(name+L"."+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64())+L".json");
    }
    report=fs::absolute(report);require(!fs::exists(report),"Runtime report already exists");
    const auto executablePath=fs::absolute(argv[first]);require(fs::is_regular_file(executablePath),"Observed executable is absent");
    std::wstring command;Json arguments=Json::array({});for(int i=first;i<argc;++i){if(!command.empty())command+=L' ';command+=quote(argv[i]);arguments.items.emplace_back(utf8(argv[i]));}
    Handle job(CreateJobObjectW(nullptr,nullptr));require(job.value!=nullptr,"Cannot create observer child Job");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    require(SetInformationJobObject(job.value,JobObjectExtendedLimitInformation,&limits,sizeof(limits))!=0,"Cannot contain observed process lifetime");
    STARTUPINFOW startup{};startup.cb=sizeof(startup);startup.dwFlags=STARTF_USESTDHANDLES;startup.hStdInput=GetStdHandle(STD_INPUT_HANDLE);startup.hStdOutput=GetStdHandle(STD_OUTPUT_HANDLE);startup.hStdError=GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION created{};auto environmentBlock=child_environment();
    require(CreateProcessW(executablePath.c_str(),command.data(),nullptr,nullptr,TRUE,DEBUG_PROCESS|CREATE_SUSPENDED|CREATE_UNICODE_ENVIRONMENT|CREATE_NO_WINDOW,environmentBlock.data(),nullptr,&startup,&created)!=0,"Cannot start observed process: "+std::to_string(GetLastError()));
    Handle processHandle(created.hProcess),threadHandle(created.hThread);
    if(!AssignProcessToJobObject(job.value,created.hProcess)){TerminateProcess(created.hProcess,2);throw std::runtime_error("Cannot assign observed process to its private Job");}
    require(ResumeThread(created.hThread)!=static_cast<DWORD>(-1),"Cannot resume observed process");
    Json images=Json::array({}),issues=Json::array({});std::map<DWORD,Process> processes;
    const ULONGLONG started=GetTickCount64();ULONGLONG stopped=0;DWORD rootExit=2;bool rootSeen=false,rootExited=false,timedOut=false,probeStopped=false,entryReached=false;int processCount=0;EntryBreakpoint entry;
    auto image=[&](const DEBUG_EVENT& event,HANDLE file,void* base,const char* kind){
        Handle eventFile(file);auto& process=processes.at(event.dwProcessId);
        if(images.items.size()>=4096){issues.items.emplace_back("module-event-budget-exceeded");return;}
        try {
            const auto path=image_path(file,process.handle,base);require(!path.empty(),"Loaded image path is unavailable");
            Handle fallback(file?nullptr:CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));
            require(file||fallback.value!=INVALID_HANDLE_VALUE,"Loaded image file is unavailable");
            auto row=Json::object({{"path",utf8(path)},{"sha256",hash_file(file?file:fallback.value)},{"kind",kind},{"pid",static_cast<double>(event.dwProcessId)},
                {"startup",!process.initialBreakpoint},{"identitySource",file?"load-event-handle":"mapped-path"},{"unloaded",false}});
            if(!file)issues.items.emplace_back("load-event-file-handle-unavailable:"+utf8(path));
            process.bases[base]=images.items.size();images.items.push_back(std::move(row));
        }catch(const std::exception& error){issues.items.emplace_back(error.what());}
    };
    while(!rootExited||!processes.empty()){
        const ULONGLONG now=GetTickCount64();
        if(timeout&&!stopped&&now-started>=timeout){timedOut=true;stopped=now;issues.items.emplace_back("observer-timeout");TerminateJobObject(job.value,2);}
        require(!stopped||now-stopped<5000,"Observed process did not drain its debug events after termination");
        DEBUG_EVENT event{};
        if(!WaitForDebugEventEx(&event,50)){require(GetLastError()==ERROR_SEM_TIMEOUT,"Cannot wait for a debug event");continue;}
        DWORD continuation=DBG_CONTINUE;
        switch(event.dwDebugEventCode){
        case CREATE_PROCESS_DEBUG_EVENT:
            processes[event.dwProcessId].handle=event.u.CreateProcessInfo.hProcess;++processCount;if(event.dwProcessId==created.dwProcessId)rootSeen=true;
            image(event,event.u.CreateProcessInfo.hFile,event.u.CreateProcessInfo.lpBaseOfImage,"executable");
            if(entryProbe&&event.dwProcessId==created.dwProcessId)entry.install(event.u.CreateProcessInfo.hProcess,event.u.CreateProcessInfo.lpBaseOfImage);break;
        case LOAD_DLL_DEBUG_EVENT:image(event,event.u.LoadDll.hFile,event.u.LoadDll.lpBaseOfDll,"dll");break;
        case UNLOAD_DLL_DEBUG_EVENT:{auto& process=processes.at(event.dwProcessId);const auto found=process.bases.find(event.u.UnloadDll.lpBaseOfDll);if(found!=process.bases.end()){images.items[found->second]["unloaded"]=true;process.bases.erase(found);}break;}
        case EXCEPTION_DEBUG_EVENT:{auto& process=processes.at(event.dwProcessId);
            if(entryProbe&&event.dwProcessId==created.dwProcessId&&event.u.Exception.ExceptionRecord.ExceptionCode==EXCEPTION_BREAKPOINT&&event.u.Exception.ExceptionRecord.ExceptionAddress==entry.address){entry.restore(process.handle);entryReached=true;probeStopped=true;stopped=GetTickCount64();require(TerminateJobObject(job.value,0)!=0,"Cannot stop entry probe Job");}
            else if(event.u.Exception.ExceptionRecord.ExceptionCode==EXCEPTION_BREAKPOINT&&!process.initialBreakpoint){process.initialBreakpoint=true;if(probe&&event.dwProcessId==created.dwProcessId){probeStopped=true;stopped=GetTickCount64();TerminateJobObject(job.value,0);}}
            else continuation=DBG_EXCEPTION_NOT_HANDLED;break;}
        case EXIT_PROCESS_DEBUG_EVENT:if(event.dwProcessId==created.dwProcessId){rootExit=event.u.ExitProcess.dwExitCode;rootExited=true;}processes.erase(event.dwProcessId);break;
        default:break;
        }
        // Debug event process/thread handles are closed by Windows on their
        // corresponding exit continuation. LOAD/CREATE image files are ours.
        require(ContinueDebugEvent(event.dwProcessId,event.dwThreadId,continuation)!=0,"Cannot continue a debug event");
    }
    require(rootSeen&&rootExited,"Observed root process had no complete lifecycle");
    const auto result=Json::object({{"schemaVersion",1},{"test",utf8(name)},{"mode",entryProbe?"entry-probe":probe?"probe":"run"},{"status",issues.items.empty()?"observed":"incomplete"},
        {"rootPid",static_cast<double>(created.dwProcessId)},{"rootExitCode",static_cast<double>(rootExit)},{"processCount",processCount},{"timedOut",timedOut},{"probeStopped",probeStopped},
        {"argv",arguments},{"workingDirectory",utf8(fs::current_path().wstring())},{"images",images},{"issues",issues},{"elapsedSeconds",static_cast<double>(GetTickCount64()-started)/1000.0},
        {"phaseBoundary","initial-loader-breakpoint"},{"probeStopBoundary",entryProbe?"native-image-entry":probe?"initial-loader-breakpoint":"none"},
        {"imageEntryReached",entryReached},{"entryPointRva",static_cast<double>(entry.rva)},{"dllInitializationMayRun",entryProbe},
        {"runtimeClosure","unclosed"},{"reuseAllowed",false},{"timingComparableToUnobservedRun",false}});
    save_report(report,result);
    if(probe||entryProbe)return probeStopped&&!timedOut?0:2;
    return static_cast<int>(rootExit);
}
}
int wmain(int argc,wchar_t** argv){try{return observe(argc,argv);}catch(const std::exception& error){std::cerr<<"Runtime observer: "<<error.what()<<'\n';return 2;}}
