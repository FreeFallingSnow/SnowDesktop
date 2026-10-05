#pragma once
#include <windows.h>
#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace build_test {
namespace fs = std::filesystem;
using Env = std::map<std::wstring, std::wstring>;
struct Json {
    enum class Kind { Null, Bool, Number, String, Array, Object } kind = Kind::Null;
    bool boolean = false;
    double number = 0;
    std::string text;
    std::vector<Json> items;
    std::map<std::string, Json> fields;
    Json() = default;
    Json(const char* value) : kind(Kind::String), text(value) {}
    Json(std::string value) : kind(Kind::String), text(std::move(value)) {}
    Json(bool value) : kind(Kind::Bool), boolean(value) {}
    Json(int value) : kind(Kind::Number), number(value) {}
    Json(double value) : kind(Kind::Number), number(value) {}
    Json& operator[](const std::string& key);
    const Json& at(const std::string& key) const;
    bool has(const std::string& key) const;
    std::string str() const;
    int integer() const;
    std::string dump() const;
    static Json parse(const std::string& value);
    static Json array(std::initializer_list<Json> value);
    static Json object(std::initializer_list<std::pair<const std::string, Json>> value);
};
void require(bool condition, const std::string& message);
std::string read(const fs::path& path);
void write(const fs::path& path, const std::string& text, bool bom = false);
void atomic(const fs::path& path, const std::string& text);
void copy(const fs::path& repo, const fs::path& root, const std::vector<std::string>& files);
std::string utf8(const std::wstring& value);
std::wstring wide(const std::string& value);
std::wstring environment(const wchar_t* name);
std::wstring quote(const std::wstring& argument);
std::string ps_literal(const fs::path& value);
std::string ps_literal(const std::string& value);
std::string base64(const std::string& value);
std::string unbase64(const std::string& value);
fs::path temporary(const std::string& label);
fs::path executable();
fs::path powershell(bool legacy = false);
fs::path production_python();
fs::path standin();
void replace(std::string& text, const std::string& before, const std::string& after);
void until(const std::function<bool()>& predicate, int seconds = 25);
struct Result { int code = -1; std::string out, err; };
class Child {
public:
    Child(const std::vector<std::wstring>& args, const fs::path& cwd, const Env& env = {}, bool input = false, bool outputPipes = false);
    ~Child();
    Child(const Child&) = delete;
    Child& operator=(const Child&) = delete;
    bool running() const;
    void send(const std::string& value);
    Result wait(int seconds = 25);
    void stop();
    DWORD pid() const { return pid_; }
    std::string out() const;
    std::string err() const;
private:
    HANDLE process_ = nullptr, input_ = nullptr, outRead_ = nullptr, errRead_ = nullptr;
    DWORD pid_ = 0;
    fs::path out_, err_;
    bool outputPipes_ = false;
};
Result run(const std::vector<std::wstring>& args, const fs::path& root, int seconds = 25, const Env& env = {});
void init_git(const fs::path& root);
class Bridge {
public:
    Bridge(const fs::path& repo, const fs::path& root, bool legacy = false);
    ~Bridge();
    Result invoke(const fs::path& script, const std::vector<std::string>& args = {}, const Env& env = {}, int seconds = 25);
    Json call(const fs::path& script, const std::vector<std::string>& args, int code = 0, const Env& env = {});
private:
    fs::path root_, state_;
    std::unique_ptr<Child> child_;
    int sequence_ = 0;
};
Json owner(DWORD pid);
bool owner_alive(const Json& value);
void stop_owner(const Json& value);
int test_main(int argc, char** argv, const std::function<void(const fs::path&)>& test);
void entry_tests(const fs::path& repo);
void shell_recovery_tests(const fs::path& repo);
void publication_tests(const fs::path& repo);
// A real coordinator with isolated, observable build/process boundaries.
class Coordinator {
public:
    fs::path root, scripts, stateRoot;
    std::unique_ptr<Bridge> bridge;
    std::vector<Json> workers;
    Coordinator(const fs::path& repo, const std::string& label);
    ~Coordinator();
    Json call(std::vector<std::string> args, int code = 0);
    std::unique_ptr<Child> start(std::vector<std::string> args);
    Json state() const;
    Json result(const std::string& batch) const;
    Json begin(const std::string& task);
    Json plan(const std::string& task, const std::string& batch, int revision = 0,
              const std::string& suite = "selected", const std::string& tests = "Alpha",
              const std::string& scope = "module", const std::string& inputs = "src/a.txt");
    Json ready(const std::string& task, const std::string& batch, int revision = 0, int code = 0);
    Json finish(const std::string& task, const std::string& batch, int revision = 0, int code = 0);
    std::vector<std::string> bound(const std::string& action, const std::string& task,
                                   const std::string& batch, int revision = 0) const;
    int calls() const;
    void gate(const std::string& name, bool present = true);
    Result git(const std::vector<std::wstring>& args);
};
}
