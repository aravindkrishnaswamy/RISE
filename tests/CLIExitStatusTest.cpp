// DL-571: exercise the actual console process, not a test-only status adapter.
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#include <sys/wait.h>
#endif

namespace fs = std::filesystem;
static std::string Quote(const std::string& s) {
#ifdef _WIN32
    return "\""+s+"\"";
#else
    std::string q="'";
    for(char c:s) q += c=='\'' ? "'\\''" : std::string(1,c);
    return q+"'";
#endif
}
static void Write(const fs::path& p,const std::string& s) {
    std::ofstream f(p); f<<s;
    if(!f) throw std::runtime_error("cannot write test input");
}
int main() {
    const fs::path dir=fs::temp_directory_path()/ ("rise_cli_status_"+std::to_string(getpid()));
    fs::create_directories(dir);
    const fs::path scene=dir/"tiny.RISEscene", broken=dir/"broken.RISEscene", image=dir/"image";
    const std::string tiny="RISE ASCII SCENE 7\nfilm\n{\n width 4\n height 4\n}\n"
        "pinhole_camera\n{\n location 0 0 3\n lookat 0 0 0\n}\n"
        "standard_shader\n{\n name global\n shaderop DefaultPathTracing\n}\n"
        "pathtracing_pel_rasterizer\n{\n samples 1\n oidn_denoise false\n}\n"
        "file_rasterizeroutput\n{\n pattern "+image.generic_string()+"\n type PNG\n}\n";
    Write(scene,tiny); Write(broken,"RISE ASCII SCENE 7\nnot_a_chunk\n{\n}\n");
    int pass=0,fail=0,serial=0;
    auto check=[&](bool ok,const char* label) { std::cout<<(ok?"PASS ":"FAIL ")<<label<<"\n";ok?++pass:++fail; };
    const fs::path bin=fs::absolute("bin/rise");
    check(fs::is_regular_file(bin),"actual CLI exists");
    if(fail) return 1;
    auto run=[&](const fs::path& arg,const std::string& input) {
        const auto in=dir/("in"+std::to_string(serial)),out=dir/("out"+std::to_string(serial++));
        Write(in,input);
        const std::string cmd=Quote(bin.string())+(arg.empty()?"":" "+Quote(arg.string()))+
            " < "+Quote(in.string())+" > "+Quote(out.string())+" 2>&1";
        const int status=std::system(cmd.c_str());
#ifdef _WIN32
        return status;
#else
        return status!=-1 && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
    };
    auto success=[&](int rc,const char* label){check(rc==0,label);};
    auto failure=[&](int rc,const char* label){check(rc>0 && rc!=127,label);};
    success(run(scene,"render\nquit\n"),"clean render and quit returns zero");
    check(fs::is_regular_file(image.string()+".png") && fs::file_size(image.string()+".png")>0,"fresh render output exists");
    success(run(scene,"quit\n"),"load then quit returns zero");
    success(run(scene,"render\n"),"successful command then EOF returns zero");
    success(run({},""),"clean empty stdin returns zero");
    failure(run(broken,"quit\n"),"broken scene then quit remains failure");
    failure(run(dir/"missing.RISEscene","quit\n"),"missing scene remains failure");
    failure(run(scene,"not_a_command\nquit\n"),"unknown command then quit remains failure");
    failure(run(scene,"not_a_command"),"unterminated failing command at EOF remains failure");
    failure(run({},"render\nquit\n"),"render without camera/rasterizer remains failure");
    failure(run({},"load "+broken.generic_string()+"\nquit\n"),"interactive load failure remains failure");
    failure(run(scene,std::string(9000,'x')+"\nquit\n"),"overlong command input returns failure");
    fs::remove_all(dir);
    std::cout<<pass<<" passed, "<<fail<<" failed\n";
    return fail?1:0;
}
