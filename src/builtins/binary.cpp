// Binary buffer helpers
#include "erelang/runtime.hpp"
#include <vector>
#include <string>
#include <cstdint>
#include <sstream>
#include <iomanip>

namespace erelang {
struct BinBuf { std::vector<uint8_t> data; };
static std::unordered_map<int,BinBuf> g_binbufs; static int g_nextBin=1;

static std::string to_hex_buf(const std::vector<uint8_t>& d){ std::ostringstream ss; ss<<std::hex<<std::setfill('0'); for(uint8_t b: d) ss<<std::setw(2)<<(int)b; return ss.str(); }

static std::string binary_dispatch(const std::string& name, const std::vector<std::string>& argv){
    auto argS = [&](size_t i){ return i<argv.size()?argv[i]:std::string(); };
    if (name == "bin_new") { int id=g_nextBin++; g_binbufs[id]= {}; return std::string("bin:")+std::to_string(id); }
    if (name == "bin_from_hex") { int id=g_nextBin++; g_binbufs[id]={}; std::string h=argS(0); for(size_t i=0;i+1<h.size();i+=2){ unsigned int v=0; std::stringstream ss; ss<<std::hex<<h.substr(i,2); ss>>v; g_binbufs[id].data.push_back((uint8_t)v);} return std::string("bin:")+std::to_string(id);}    
    if (name == "bin_len") { auto h=argS(0); if(h.rfind("bin:",0)==0){int id=std::stoi(h.substr(4)); return std::to_string(g_binbufs[id].data.size()); } return "0"; }
    if (name == "bin_hex") { auto h=argS(0); if(h.rfind("bin:",0)==0){int id=std::stoi(h.substr(4)); return to_hex_buf(g_binbufs[id].data);} return {}; }
    if (name == "bin_push_u8") { auto h=argS(0); int v=0; try{ v=std::stoi(argS(1)); }catch(...){} if(h.rfind("bin:",0)==0){int id=std::stoi(h.substr(4)); g_binbufs[id].data.push_back((uint8_t)(v & 0xFF));} return {}; }
    if (name == "bin_get_u8") { auto h=argS(0); int idx=0; try{ idx=std::stoi(argS(1)); }catch(...){} if(h.rfind("bin:",0)==0){int id=std::stoi(h.substr(4)); if(idx>=0 && idx<(int)g_binbufs[id].data.size()) return std::to_string((int)g_binbufs[id].data[idx]); } return {};}    
    return {};
}

std::string __erelang_builtin_binary_dispatch(const std::string& name, const std::vector<std::string>& argv){ return binary_dispatch(name, argv);}    

} // namespace erelang
