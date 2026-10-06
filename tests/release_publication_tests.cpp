#include "build_tool_test_support.h"
#include <algorithm>
#include <cctype>
namespace build_test {
void publication_tests(const fs::path& repo){
    auto root=temporary("publication");Bridge bridge(repo,root);auto script=root/L"publication.ps1";
    write(script,"$ast=[Management.Automation.Language.Parser]::ParseFile("+ps_literal(repo/L"scripts/release_manager.ps1")+R"PS(,[ref]$null,[ref]$null)
$definition=$ast.Find({param($node)$node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'New-GitHubReleasePublication'},$false)
. ([scriptblock]::Create($definition.Extent.Text))
$context=[pscustomobject]@{Version='1.2.3.0';Tag='v1.2.3.0';VersionDirectory=$PSScriptRoot}
New-GitHubReleasePublication -Context $context|ConvertTo-Json -Depth 8
)PS",true);auto portable="SnowDesktop-portable-x64-1.2.3.0.zip";write(root/portable,"abc");write(root/L"SHA256SUMS.txt","private Store checksum inventory\n");for(const auto& extension:{"msix","msixupload","appxsym"})write(root/("SnowDesktop-Store-x64-1.2.3.0."+std::string(extension)),"local Store fixture");
    for(bool signedValue:{false,true}){
        write(root/L"package-info.json",Json::object({{"msix",Json::object({{"signed",signedValue},{"path","SnowDesktop-Store-x64-1.2.3.0.msix"}})}}).dump());auto value=bridge.call(script,{});
        require(value.at("Title").str()=="1.2.3.0"&&value.at("Notes").str()==utf8((root/L"release-notes.md").wstring())&&value.at("Assets").items.size()==2,"Publication title/notes/assets incorrect");
        require(fs::equivalent(wide(value.at("Assets").items[0].str()),root/portable)&&fs::equivalent(wide(value.at("Assets").items[1].str()),root/L"github-release/SHA256SUMS.txt"),"Store metadata leaked into public assets");
        require(read(root/L"github-release/SHA256SUMS.txt")=="BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD  "+std::string(portable)+"\n"&&read(root/L"SHA256SUMS.txt")=="private Store checksum inventory\n","Public/private checksum inventory altered");
    }
    write(root/portable,"");bridge.call(script,{});require(read(root/L"github-release/SHA256SUMS.txt")=="E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855  "+std::string(portable)+"\n","Repeated publication preparation did not refresh digest");
    auto text=read(repo/L"scripts/release_manager.ps1");auto start=text.find("function Publish-GitHubRelease");require(start!=std::string::npos,"GitHub publisher missing");text=text.substr(start);auto end=text.find("\nfunction ",1);if(end!=std::string::npos)text.resize(end);text.erase(std::remove_if(text.begin(),text.end(),[](unsigned char c){return std::isspace(c)!=0||c=='`';}),text.end());for(const auto& marker:{"$publication=New-GitHubReleasePublication-Context$context","$assets=$publication.Assets","--title$publication.Title","name=$publication.Title"})require(text.find(marker)!=std::string::npos,"Publisher transport lost shared attachment/title plan");require(text.find("$assets+=")==std::string::npos,"Publisher adds unreviewed attachments");
}
}
