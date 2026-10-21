#include "../build_tool_test_support.h"
#include <iostream>
#include <sstream>
#include <utility>
using namespace build_test;

namespace {
int probes(const std::string& output){
    std::istringstream lines(output);std::string line;int count=0;
    while(std::getline(lines,line))if(line.starts_with("-- Performing Test ")&&line.find(" - ")==std::string::npos)++count;
    return count;
}
void check_cache(const fs::path& repo,bool omitHeaderIdentity){
    const auto root=temporary(omitHeaderIdentity?"webp-probe-header-control":"webp-probe-cache");
    copy(repo,root,{"cmake/SnowDesktop.WebPProbeCache.cmake"});
    const auto vendor=root/L"vendor";
    for(const auto& entry:fs::recursive_directory_iterator(repo/L"third_party/libwebp/src/dsp"))if(entry.is_regular_file()){
        const auto path=vendor/L"src/dsp"/fs::relative(entry.path(),repo/L"third_party/libwebp/src/dsp");
        fs::create_directories(path.parent_path());fs::copy_file(entry.path(),path);
    }
    for(const auto& name:{"cmake/cpu.cmake","src/webp/types.h"}){
        const auto path=vendor/name;fs::create_directories(path.parent_path());fs::copy_file(repo/L"third_party/libwebp"/name,path);
    }
    if(omitHeaderIdentity){
        auto source=read(root/L"cmake/SnowDesktop.WebPProbeCache.cmake");
        replace(source,"file(SHA256 \"${path}\" digest)","set(digest \"controlled missing file identity\")");
        write(root/L"cmake/SnowDesktop.WebPProbeCache.cmake",source);
    }
    write(root/L"CMakeLists.txt",R"CMAKE(cmake_minimum_required(VERSION 3.24)
project(WebPProbeCache LANGUAGES C)
option(SNOWDESKTOP_CACHE_WEBP_PROBES "Probe cache" ON)
option(PROBE_IMPORTED "Unknown imported dependency" OFF)
include("${CMAKE_SOURCE_DIR}/cmake/SnowDesktop.WebPProbeCache.cmake")
if(PROBE_IMPORTED)
 add_library(ProbeDependency INTERFACE IMPORTED GLOBAL)
 set(CMAKE_REQUIRED_LIBRARIES ProbeDependency)
endif()
add_subdirectory(vendor)
)CMAKE");
    write(vendor/L"CMakeLists.txt",R"CMAKE(include(CheckCSourceCompiles)
set(WEBP_ENABLE_SIMD ON)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/cpu.cmake")
file(WRITE "${CMAKE_BINARY_DIR}/probe-state.json" "{\"sse2\":\"${WEBP_HAVE_SSE2}\",\"included\":\"${WEBP_SIMD_FILES_TO_INCLUDE}\"}")
)CMAKE");
    const auto header=vendor/L"src/dsp/cpu.h";
    const auto original=read(header)+"\n#ifdef SNOWDESKTOP_PROBE_DISABLE_SSE2\n#undef WEBP_USE_SSE2\n#endif\n";
    write(header,original);
    auto configure=[&](const std::vector<std::wstring>& options=std::vector<std::wstring>{}){
        std::vector<std::wstring> args={L"cmake",L"-S",root.wstring(),L"-B",(root/L"out").wstring(),L"-G",L"Visual Studio 18 2026",L"-A",L"x64"};
        args.insert(args.end(),options.begin(),options.end());
        const auto result=run(args,root,60);
        require(result.code==0,"Real WebP probe configuration failed: "+result.out+result.err);
        const auto state=Json::parse(read(root/L"out/probe-state.json"));
        std::cout<<"CASE WebP probe compile/link attempts="<<probes(result.out)<<" SSE2="<<state.at("sse2").str()<<'\n';
        return std::pair{result,state};
    };
    const auto first=configure();
    require(first.second.at("sse2").str()=="1"&&first.second.at("included").str().find("_sse2.c")!=std::string::npos&&probes(first.first.out)>0,
            "Native x64 baseline did not enable its actual SSE2 source set");
    const auto warm=configure();
    require(warm.second.dump()==first.second.dump()&&probes(warm.first.out)==0,"Unchanged WebP configuration repeated compiler probes or changed its source set");

    // Change the real probe's transitive header while restoring no flags: a
    // cache that ignores source identity would incorrectly keep SSE2 enabled.
    write(header,original+"\n#undef WEBP_USE_SSE2\n");
    const auto changed=configure();
    require(changed.second.at("sse2").str()=="0"&&changed.second.at("included").str().find("_sse2.c")==std::string::npos&&probes(changed.first.out)>0,
            "Changed probe header retained stale SSE2 capability/source selection");
    if(omitHeaderIdentity)return;

    write(header,original);
    const auto disabled=configure({L"-DCMAKE_C_FLAGS:STRING=/DWIN32 /D_WINDOWS /DSNOWDESKTOP_PROBE_DISABLE_SSE2"});
    require(disabled.second.at("sse2").str()=="0"&&probes(disabled.first.out)>0,"Changed compiler flags reused an incompatible SSE2 result");
    const auto restored=configure({L"-DCMAKE_C_FLAGS:STRING=/DWIN32 /D_WINDOWS"});
    require(restored.second.dump()==first.second.dump()&&probes(restored.first.out)>0,"Restored compiler inputs did not recover the SSE2 source set");
    const auto imported=configure({L"-DPROBE_IMPORTED=ON"});
    require(imported.second.dump()==first.second.dump()&&probes(imported.first.out)>0,"Imported dependency reused a context with unknown target state");
    const auto uncached=configure({L"-DPROBE_IMPORTED=OFF",L"-DSNOWDESKTOP_CACHE_WEBP_PROBES=OFF"});
    require(uncached.second.dump()==first.second.dump()&&probes(uncached.first.out)>0,"Uncached control changed SIMD selection or skipped the real checks");
}
}
void configuration_cache_tests(const fs::path& repo,bool omitHeaderIdentity){
    check_cache(repo,omitHeaderIdentity);
    if(omitHeaderIdentity)return;
    const auto root=temporary("webp-probe-negative-parent");
    const auto control=run({executable().wstring(),repo.wstring(),L"--configuration-header-control"},root,75);
    require(control.code==1&&control.err.find("Changed probe header retained stale SSE2 capability/source selection")!=std::string::npos,
            "Removing file identity did not reach the real stale-capability failure: "+control.out+control.err);
    std::cout<<"PASS real WebP probe warm reuse, header/flag invalidation, unknown-input fallback and stale-header negative control\n";
}
