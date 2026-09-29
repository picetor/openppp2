// Windows web control host. The VPN data plane and control commands stay in C++.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>
#include <bcrypt.h>
#include <ppp/core/CoreApi.h>
#include <json/json.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <mutex>
#include <atomic>
#include <thread>
#include <algorithm>
#include <stdexcept>
#include "resources.h"

namespace fs = std::filesystem;
using J = Json::Value;
static std::atomic<bool> exiting{false};
static std::mutex stateMutex;
static ppp_core_handle* core = nullptr;
static fs::path base, settingsPath, exe;
static J settings;
static std::string sessionToken, origin, lastError;
static int port=19999;

static std::wstring wide(const std::string& s) { int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.data(),(int)s.size(),nullptr,0); if(!n&&!s.empty())throw std::runtime_error("Invalid UTF-8");std::wstring w(n,0);MultiByteToWideChar(CP_UTF8,0,s.data(),(int)s.size(),w.data(),n);return w; }
static std::string utf8(const std::wstring& s) { int n=WideCharToMultiByte(CP_UTF8,0,s.data(),(int)s.size(),nullptr,0,nullptr,nullptr);std::string r(n,0);WideCharToMultiByte(CP_UTF8,0,s.data(),(int)s.size(),r.data(),n,nullptr,nullptr);return r; }
static std::string readFile(const fs::path& p) { std::ifstream f(p,std::ios::binary);if(!f)throw std::runtime_error("Cannot read: "+p.u8string());return {std::istreambuf_iterator<char>(f),{}}; }
static std::string embeddedAsset(const std::string& name) {
 int id = name=="index.html" ? IDR_WEB_INDEX : name=="app.js" ? IDR_WEB_APP : name=="style.css" ? IDR_WEB_STYLE : name=="favicon.ico" ? IDR_WEB_FAVICON : 0;
 HMODULE module=GetModuleHandleW(nullptr);
 HRSRC resource=id ? FindResourceW(module,MAKEINTRESOURCEW(id),RT_RCDATA) : nullptr;
 if(!resource)throw std::runtime_error("Missing embedded web resource: "+name);
 HGLOBAL data=LoadResource(module,resource);
 DWORD size=SizeofResource(module,resource);
 const char* bytes=data ? static_cast<const char*>(LockResource(data)) : nullptr;
 if(!bytes||!size)throw std::runtime_error("Cannot load embedded web resource: "+name);
 return std::string(bytes,size);
}
static std::string json(const J& v) { Json::StreamWriterBuilder b;b["indentation"]="";return Json::writeString(b,v).c_str(); }
static J parse(const std::string& s) { Json::CharReaderBuilder b;std::unique_ptr<Json::CharReader> r(b.newCharReader());J v;Json::String e;if(!r->parse(s.data(),s.data()+s.size(),&v,&e))throw std::runtime_error(std::string("Invalid JSON: ")+e.c_str());return v; }
static std::string str(const J& v,const char* key,const char* fallback="") { const J& x=v[key];return x.isString()?x.asCString():fallback; }
static bool flag(const J& v,const char* key,bool fallback=false) { return v[key].isBool()?v[key].asBool():fallback; }
static bool admin() { BOOL yes=FALSE;SID_IDENTIFIER_AUTHORITY a=SECURITY_NT_AUTHORITY;PSID sid=nullptr;if(AllocateAndInitializeSid(&a,2,SECURITY_BUILTIN_DOMAIN_RID,DOMAIN_ALIAS_RID_ADMINS,0,0,0,0,0,0,&sid)){CheckTokenMembership(nullptr,sid,&yes);FreeSid(sid);}return yes!=FALSE; }
static fs::path workDir() { auto p=fs::u8path(str(settings,"working_dir"));return fs::absolute(p.empty()?base:p.is_absolute()?p:base/p).lexically_normal(); }
static fs::path resolve(const std::string& s) { auto p=fs::u8path(s);return (p.is_absolute()?p:workDir()/p).lexically_normal(); }
static void saveSettings() { auto tmp=settingsPath;tmp+=L".tmp";{std::ofstream f(tmp,std::ios::binary|std::ios::trunc);if(!f)throw std::runtime_error("Cannot save settings");f<<json(settings);f.flush();if(!f)throw std::runtime_error("Writing settings failed");}if(!MoveFileExW(tmp.c_str(),settingsPath.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::runtime_error("Cannot replace settings file"); }
static void defaults() {
 settings=J(Json::objectValue);settings["web_port"]="19999";settings["working_dir"]=base.u8string().c_str();settings["mode"]="client";settings["config_path"]="./appsettings.json";settings["server_dir"]="./servers";settings["log_file"]="./ppp_win.log";settings["log_level"]="info";settings["tun_enabled"]=true;settings["tun_host"]=true;settings["tun_vnet"]=true;settings["system_proxy_enabled"]=false;settings["rt"]=true;settings["tcp_ip_cc"]="auto";settings["bypass_mode"]="no";settings["bypass_file"]="./ip.txt";settings["bypass6_file"]="./ipv6.txt";settings["dns_rules_file"]="./dns-rules.txt";settings["geo_rules_file"]="./geo-rules.yaml";settings["geosite_file"]="./geosite.dat";settings["geoip_file"]="./geoip.dat";settings["auto_restart"]="0";
 fs::path source=fs::exists(settingsPath)?settingsPath:base/L"ppp-tui.json";
 if(fs::exists(source)){J stored=parse(readFile(source));if(!stored.isObject())throw std::runtime_error("Settings must be an object");for(const auto& k:stored.getMemberNames())settings[k]=stored[k];}
 settings.removeMember("settings_file");settings.removeMember("rpc_address");
}
static const std::map<std::string,std::string> options={
 {"config_path","config"},{"server_dir","server-dir"},{"dns","dns"},{"tun_ip","tun-ip"},{"tun_gw","tun-gw"},{"tun_mask","tun-mask"},{"nic","nic"},{"ngw","ngw"},{"tun","tun"},{"tun_driver","tun-driver"},{"tun_mux","tun-mux"},{"tun_mux_acceleration","tun-mux-acceleration"},{"link_restart","link-restart"},{"auto_restart","auto-restart"},{"tun_lease_time","tun-lease-time-in-seconds"},{"proxy_http_port","proxy-http-port"},{"proxy_socks_port","proxy-socks-port"},{"bypass_ngw","bypass-ngw"},{"bypass_ngw6","bypass-ngw6"},{"log_file","log-file"},{"log_level","log-level"},{"rpc_listen","rpc-listen"},{"rpc_token","rpc-token"},{"firewall_rules","firewall-rules"}};
static const std::map<std::string,std::string> boolOptions={{"tun_host","tun-host"},{"tun_vnet","tun-vnet"},{"tun_static","tun-static"},{"tun_flash","tun-flash"},{"block_quic","block-quic"},{"rt","rt"},{"system_proxy_enabled","set-http-proxy"}};
static std::vector<std::string> arguments() {
 std::vector<std::string> a={"ppp-web","--headless"};std::string mode=str(settings,"mode","client");if(mode=="client"&&!flag(settings,"tun_enabled",true))mode="proxy";a.push_back("--mode="+mode);
 for(const auto& kv:options){std::string v=str(settings,kv.first.c_str());if(!v.empty())a.push_back("--"+kv.second+"="+v);}
 for(const auto& kv:boolOptions)a.push_back("--"+kv.second+"="+(flag(settings,kv.first.c_str())?"yes":"no"));
 auto tcp=str(settings,"tcp_ip_cc");if(tcp=="lwip"||tcp=="ctcp")a.push_back(std::string("--lwip=")+(tcp=="lwip"?"yes":"no"));
 if(mode=="client"){auto bypass=str(settings,"bypass_mode","no");a.push_back("--bypass-mode="+bypass);std::map<std::string,std::string> files=bypass=="geo"?std::map<std::string,std::string>{{"geo_rules_file","geo-rules"},{"geosite_file","geosite"},{"geoip_file","geoip"}}:bypass=="ip"?std::map<std::string,std::string>{{"bypass_file","bypass"},{"bypass6_file","bypass6"},{"dns_rules_file","dns-rules"}}:std::map<std::string,std::string>{};for(auto& kv:files){auto v=str(settings,kv.first.c_str());if(!v.empty())a.push_back("--"+kv.second+"="+v);}}
 return a;
}
static J callCore(const std::string& method,const J& params) {
 if(!core||!ppp_core_is_running(core))throw std::runtime_error("Core is not running");char err[4096]={};char* result=nullptr;int ok=ppp_core_command(core,method.c_str(),json(params).c_str(),&result,err,sizeof(err));std::string body=result?result:"{}";if(result)ppp_core_free_string(result);if(!ok)throw std::runtime_error(err);return parse(body);
}
static void stopCore() { if(!core)return;char err[4096]={};if(!ppp_core_stop(core,err,sizeof(err)))throw std::runtime_error(err);ppp_core_destroy(core);core=nullptr; }
static void startCore() {
 if(core&&ppp_core_is_running(core))throw std::runtime_error("Core is already running");if(core){ppp_core_destroy(core);core=nullptr;}
 if(str(settings,"mode","client")=="client"&&flag(settings,"tun_enabled",true)&&!admin())throw std::runtime_error("TUN needs administrator privileges. Click UAC first.");
 if(!fs::is_directory(workDir()))throw std::runtime_error("Working directory does not exist");if(!fs::is_regular_file(resolve(str(settings,"config_path"))))throw std::runtime_error("Configuration file does not exist");
 fs::current_path(workDir());auto a=arguments();std::vector<const char*> raw;for(auto& s:a)raw.push_back(s.c_str());char err[4096]={};core=ppp_core_start((int)raw.size(),raw.data(),nullptr,nullptr,err,sizeof(err));if(!core){lastError=err;throw std::runtime_error(lastError);}lastError.clear();
}
static J catalog() {
 J result(Json::arrayValue);std::set<fs::path> paths;auto dir=resolve(str(settings,"server_dir"));if(fs::is_directory(dir))for(auto& e:fs::directory_iterator(dir))if(e.is_regular_file()&&e.path().extension()==L".json")paths.insert(e.path());auto primary=resolve(str(settings,"config_path"));if(fs::is_regular_file(primary))paths.insert(primary);
 for(const auto& p:paths){J row;row["name"]=p.stem().u8string().c_str();row["path"]=p.u8string().c_str();try{auto c=parse(readFile(p));row["server"]=c["client"]["server"];row["valid"]=c.isObject();}catch(const std::exception& e){row["valid"]=false;row["error"]=e.what();}result.append(row);}return result;
}
static J status() {
 J r;r["admin"]=admin();r["running"]=core&&ppp_core_is_running(core);r["error"]=lastError.c_str();r["settings_path"]=settingsPath.u8string().c_str();r["web_url"]=origin.c_str();
 if(r["running"].asBool())r["snapshot"]=callCore("get_snapshot",J(Json::objectValue));return r;
}
static J dispatch(const std::string& m,const J& p) {
 std::lock_guard<std::mutex> lock(stateMutex);
 if(exiting)throw std::runtime_error("Host is exiting");
 if(m=="host.status")return status();
 if(m=="host.settings")return settings;
 if(m=="host.save"){if(p["settings"].isMember("web_port")){auto value=str(p["settings"],"web_port");if(value.empty()||value.size()>5||value.find_first_not_of("0123456789")!=std::string::npos||std::stoi(value)<1024||std::stoi(value)>65535)throw std::runtime_error("Web port must be between 1024 and 65535");}if(!p["settings"].isObject())throw std::runtime_error("settings object required");for(auto& k:p["settings"].getMemberNames()){auto v=p["settings"][k];if(!v.isString()&&!v.isBool())throw std::runtime_error("Settings must contain strings or booleans");settings[k]=v;}saveSettings();return settings;}
 if(m=="host.catalog")return catalog();
 if(m=="host.paths"){J r(Json::arrayValue);if(!p["paths"].isArray()||p["paths"].size()>32)throw std::runtime_error("Invalid paths");for(auto& value:p["paths"]){J row;auto path=resolve(value.asCString());row["path"]=path.u8string().c_str();row["exists"]=fs::is_regular_file(path);r.append(row);}return r;}
 if(m=="host.arguments"){J r(Json::arrayValue);for(auto a:arguments()){if(a.rfind("--rpc-token=",0)==0)a="--rpc-token=<redacted>";r.append(a.c_str());}return r;}
 if(m=="host.start"){startCore();return status();}
 if(m=="host.stop"){stopCore();return status();}
 if(m=="host.restart"){stopCore();startCore();return status();}
 if(m=="host.exit"){stopCore();exiting=true;return J(true);}
 if(m=="host.elevate"){
  if(admin())return J(true);if(core&&ppp_core_is_running(core))throw std::runtime_error("Stop the core before requesting UAC");saveSettings();std::wstring args=L"--port="+std::to_wstring(port)+L" --base=\""+base.wstring()+L"\"";SHELLEXECUTEINFOW info={sizeof(info)};info.fMask=SEE_MASK_NOCLOSEPROCESS;info.lpVerb=L"runas";info.lpFile=exe.c_str();info.lpParameters=args.c_str();info.lpDirectory=base.c_str();info.nShow=SW_HIDE;if(!ShellExecuteExW(&info))throw std::runtime_error("UAC was cancelled or elevation failed");if(info.hProcess)CloseHandle(info.hProcess);exiting=true;return J(true);
 }
 // Every core method passes through the same AI/API dispatcher used by TCP RPC.
 static const std::set<std::string> methods={"ping","describe_api","get_health","run_diagnostics","get_snapshot","get_log_level","get_logs","get_outbounds","get_settings","set_log_level","update_settings","configure_api","switch_server","switch_rank1","shutdown"};
 if(!methods.count(m))throw std::runtime_error("Unknown method");return callCore(m,p);
}
static std::string lower(std::string s){std::transform(s.begin(),s.end(),s.begin(),[](unsigned char c){return (char)std::tolower(c);});return s;}
static void sendAll(SOCKET s,const std::string& data){size_t sent=0;while(sent<data.size()){int n=send(s,data.data()+sent,(int)(data.size()-sent),0);if(n<=0)return;sent+=n;}}
static void respond(SOCKET s,int code,const std::string& body,const std::string& type="application/json; charset=utf-8"){
 sendAll(s,"HTTP/1.1 "+std::to_string(code)+(code==200?" OK":" Error")+"\r\nContent-Type: "+type+"\r\nContent-Length: "+std::to_string(body.size())+"\r\nConnection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nReferrer-Policy: no-referrer\r\nContent-Security-Policy: default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self' data:; connect-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'none'\r\n\r\n"+body);
}
static void serve(SOCKET s) {
 DWORD timeout=5000;setsockopt(s,SOL_SOCKET,SO_RCVTIMEO,(char*)&timeout,sizeof(timeout));setsockopt(s,SOL_SOCKET,SO_SNDTIMEO,(char*)&timeout,sizeof(timeout));
 try{
  std::string request;char buf[8192];size_t end;while((end=request.find("\r\n\r\n"))==std::string::npos){int n=recv(s,buf,sizeof(buf),0);if(n<=0)throw std::runtime_error("Incomplete request");request.append(buf,n);if(request.size()>16384)throw std::runtime_error("Headers too large");}
  std::istringstream stream(request.substr(0,end));std::string method,path,version,line;stream>>method>>path>>version;std::getline(stream,line);std::map<std::string,std::string> headers;
  while(std::getline(stream,line)){if(!line.empty()&&line.back()=='\r')line.pop_back();auto colon=line.find(':');if(colon==std::string::npos)continue;auto value=line.substr(colon+1);value.erase(0,value.find_first_not_of(" \t"));auto key=lower(line.substr(0,colon));if(headers.count(key))throw std::runtime_error("Duplicate header");headers[key]=value;}
  auto host="127.0.0.1:"+std::to_string(port);if(headers["host"]!=host){respond(s,403,"{\"error\":\"Invalid Host\"}");closesocket(s);return;}
  if((headers.count("origin")&&headers["origin"]!=origin)||headers["sec-fetch-site"]=="cross-site"){respond(s,403,"{\"error\":\"Cross-origin request denied\"}");closesocket(s);return;}
  if(method=="GET"&&(path=="/"||path=="/index.html"||path=="/app.js"||path=="/style.css"||path=="/favicon.ico"||path=="/session.js")){
   if(path=="/session.js")respond(s,200,"window.PPP_SESSION="+json(J(sessionToken.c_str()))+";","text/javascript; charset=utf-8");else{auto file=(path=="/"||path=="/index.html")?"index.html":path.substr(1);respond(s,200,embeddedAsset(file),file=="index.html"?"text/html; charset=utf-8":file=="app.js"?"text/javascript; charset=utf-8":file=="favicon.ico"?"image/x-icon":"text/css; charset=utf-8");}
  }else if(method=="POST"&&path=="/api/rpc"){
   if(headers["authorization"]!="Bearer "+sessionToken){respond(s,401,"{\"error\":\"Session expired; reload the page\"}");closesocket(s);return;}
   if(headers.count("transfer-encoding")||headers["content-type"].rfind("application/json",0)!=0)throw std::runtime_error("JSON content required");auto lengthText=headers["content-length"];if(lengthText.empty()||lengthText.find_first_not_of("0123456789")!=std::string::npos)throw std::runtime_error("Invalid Content-Length");size_t length=std::stoul(lengthText);if(length>1024*1024)throw std::runtime_error("Body too large");std::string body=request.substr(end+4);while(body.size()<length){int n=recv(s,buf,(int)std::min(sizeof(buf),length-body.size()),0);if(n<=0)throw std::runtime_error("Incomplete body");body.append(buf,n);}auto input=parse(body.substr(0,length));if(!input.isObject()||!input["method"].isString())throw std::runtime_error("method required");J r;r["id"]=input["id"];try{r["result"]=dispatch(input["method"].asCString(),input.get("params",J(Json::objectValue)));r["ok"]=true;}catch(const std::exception& e){r["ok"]=false;r["error"]=e.what();}respond(s,200,json(r));
  }else respond(s,404,"{\"error\":\"Not found\"}");
 }catch(const std::exception& e){J r;r["error"]=e.what();respond(s,400,json(r));}shutdown(s,SD_BOTH);closesocket(s);
}

// A hidden top-level window receives Explorer restart broadcasts and tray events.
// Core lifecycle work remains on workers so the tray can respond during startup.
static constexpr UINT WM_TRAY = WM_APP + 1;
static constexpr UINT MENU_OPEN = 1001, MENU_EXIT = 1002;
class TrayHost {
 HWND window=nullptr;
 NOTIFYICONDATAW icon{};
 UINT taskbarCreated=0;
 std::wstring label=L"PPP Web - 管理面板运行中；核心未启动";
 bool added=false;
 void add() {
  added=Shell_NotifyIconW(NIM_ADD,&icon)!=FALSE;
  if(added){icon.uVersion=NOTIFYICON_VERSION_4;Shell_NotifyIconW(NIM_SETVERSION,&icon);}
 }
 void openPanel() {
  if(reinterpret_cast<INT_PTR>(ShellExecuteW(window,L"open",wide(origin).c_str(),nullptr,nullptr,SW_SHOWNORMAL))<=32)
   MessageBoxW(window,L"无法打开浏览器，请使用托盘提示中的本机管理地址。",L"PPP Web",MB_OK|MB_ICONERROR);
 }
 void menu() {
  HMENU popup=CreatePopupMenu();
  if(!popup)return;
  AppendMenuW(popup,MF_STRING|MF_GRAYED,0,label.c_str());
  AppendMenuW(popup,MF_SEPARATOR,0,nullptr);
  AppendMenuW(popup,MF_STRING,MENU_OPEN,L"打开管理面板");
  AppendMenuW(popup,MF_STRING,MENU_EXIT,L"退出（停止核心）");
  SetMenuDefaultItem(popup,MENU_OPEN,FALSE);
  POINT point{};GetCursorPos(&point);SetForegroundWindow(window);
  UINT command=TrackPopupMenu(popup,TPM_RETURNCMD|TPM_NONOTIFY|TPM_RIGHTBUTTON,point.x,point.y,0,window,nullptr);
  DestroyMenu(popup);PostMessageW(window,WM_NULL,0,0);
  if(command==MENU_OPEN)openPanel();
  if(command==MENU_EXIT)exiting=true;
 }
 static LRESULT CALLBACK procedure(HWND hwnd,UINT message,WPARAM w,LPARAM l) {
  auto self=reinterpret_cast<TrayHost*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
  if(message==WM_NCCREATE){
   self=static_cast<TrayHost*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
   SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));
  }
  if(self){
   if(message==self->taskbarCreated&&self->taskbarCreated){self->add();return 0;}
   if(message==WM_TRAY){
    switch(LOWORD(l)){
     case NIN_SELECT: case NIN_KEYSELECT: self->openPanel();break;
     case WM_CONTEXTMENU: self->menu();break;
    }
    return 0;
   }
   if(message==WM_CLOSE){exiting=true;return 0;}
   if(message==WM_QUERYENDSESSION)return TRUE;
   if(message==WM_ENDSESSION&&w){exiting=true;return 0;}
  }
  return DefWindowProcW(hwnd,message,w,l);
 }
public:
 TrayHost() {
  taskbarCreated=RegisterWindowMessageW(L"TaskbarCreated");
  WNDCLASSW cls{};cls.lpfnWndProc=procedure;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=L"OpenPPP2WebTray";cls.hIcon=LoadIconW(cls.hInstance,MAKEINTRESOURCEW(IDI_PPP_WEB));
  if(!RegisterClassW(&cls)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)throw std::runtime_error("Cannot register tray window");
  window=CreateWindowExW(WS_EX_TOOLWINDOW,cls.lpszClassName,L"PPP Web",WS_OVERLAPPED,0,0,0,0,nullptr,nullptr,cls.hInstance,this);
  if(!window)throw std::runtime_error("Cannot create tray window");
  icon.cbSize=sizeof(icon);icon.hWnd=window;icon.uID=1;
  icon.uFlags=NIF_MESSAGE|NIF_ICON|NIF_TIP|NIF_SHOWTIP;icon.uCallbackMessage=WM_TRAY;
  icon.hIcon=LoadIconW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(IDI_PPP_WEB));
  lstrcpynW(icon.szTip,label.c_str(),static_cast<int>(sizeof(icon.szTip)/sizeof(wchar_t)));
  add();
  // Explorer may not be ready yet (or absent in a CI session); refresh retries.
 }
 ~TrayHost(){if(added)Shell_NotifyIconW(NIM_DELETE,&icon);if(window)DestroyWindow(window);}
 void refresh() {
  std::unique_lock<std::mutex> lock(stateMutex,std::try_to_lock);
  if(!lock.owns_lock())label=L"PPP Web - 管理面板运行中；核心操作中";
  else {
   label=L"PPP Web - 管理面板运行中；核心未启动";
   if(!lastError.empty())label=L"PPP Web - 核心异常，请打开面板";
   if(core&&ppp_core_is_running(core)){
    try{
     auto health=callCore("get_health",J(Json::objectValue));
     label=health["ready"].asBool()&&health["healthy"].asBool()
      ? L"PPP Web - 核心就绪" : L"PPP Web - 核心运行中（连接中或待检查）";
    }catch(...){label=L"PPP Web - 核心状态暂不可用";}
   }
  }
  auto tip=label+L"\n"+wide(origin);
  lstrcpynW(icon.szTip,tip.c_str(),static_cast<int>(sizeof(icon.szTip)/sizeof(wchar_t)));
  if(added&&!Shell_NotifyIconW(NIM_MODIFY,&icon))added=false;
  if(!added)add();
 }
 void run() {
  ULONGLONG next=0;
  while(!exiting){
   MSG message{};
   while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){
    if(message.message==WM_QUIT){exiting=true;break;}
    TranslateMessage(&message);DispatchMessageW(&message);
   }
   auto now=GetTickCount64();if(now>=next){refresh();next=now+1000;}
   MsgWaitForMultipleObjects(0,nullptr,FALSE,100,QS_ALLINPUT);
  }
 }
};

int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR,int) {
 try{
  wchar_t file[32768];GetModuleFileNameW(nullptr,file,32768);exe=fs::path(file);base=exe.parent_path();bool open=true;bool explicitPort=false;int argc=0;LPWSTR* argv=CommandLineToArgvW(GetCommandLineW(),&argc);for(int i=1;i<argc;++i){std::wstring a=argv[i];if(a.rfind(L"--base=",0)==0)base=fs::absolute(a.substr(7));else if(a.rfind(L"--port=",0)==0){port=std::stoi(a.substr(7));explicitPort=true;}else if(a==L"--no-browser")open=false;}LocalFree(argv);if(port<1024||port>65535)throw std::runtime_error("Invalid port");settingsPath=base/L"ppp-web.json";defaults();if(!explicitPort){auto saved=str(settings,"web_port","19999");if(saved.empty()||saved.find_first_not_of("0123456789")!=std::string::npos)throw std::runtime_error("Invalid saved web port");port=std::stoi(saved);if(port<1024||port>65535)throw std::runtime_error("Web port must be between 1024 and 65535");}origin="http://127.0.0.1:"+std::to_string(port);
  unsigned char random[32];if(BCryptGenRandom(nullptr,random,sizeof(random),BCRYPT_USE_SYSTEM_PREFERRED_RNG)!=0)throw std::runtime_error("Random generator failed");const char* hex="0123456789abcdef";for(auto c:random){sessionToken+=hex[c>>4];sessionToken+=hex[c&15];}
  WSADATA ws;if(WSAStartup(MAKEWORD(2,2),&ws))throw std::runtime_error("Winsock startup failed");
  SOCKET listener=INVALID_SOCKET;
  const int preferredPort=port;
  // Bind atomically; never open a URL served by the process occupying our port.
  // Wait briefly on the preferred port so a UAC predecessor can finish exiting.
  for(int attempt=0;attempt<131;++attempt){
   int candidate=attempt<30?preferredPort:attempt<130?preferredPort+attempt-29:0;
   if(candidate>65535)continue;
   listener=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
   if(listener==INVALID_SOCKET)throw std::runtime_error("Cannot create web listener");
   BOOL exclusive=TRUE;setsockopt(listener,SOL_SOCKET,SO_EXCLUSIVEADDRUSE,(char*)&exclusive,sizeof(exclusive));
   sockaddr_in addr={};addr.sin_family=AF_INET;addr.sin_port=htons((u_short)candidate);inet_pton(AF_INET,"127.0.0.1",&addr.sin_addr);
   if(bind(listener,(sockaddr*)&addr,sizeof(addr))==0&&listen(listener,16)==0){
    int size=sizeof(addr);if(getsockname(listener,(sockaddr*)&addr,&size)!=0)throw std::runtime_error("Cannot read bound web port");
    port=ntohs(addr.sin_port);break;
   }
   closesocket(listener);listener=INVALID_SOCKET;if(attempt<29)Sleep(100);
  }
  if(listener==INVALID_SOCKET)throw std::runtime_error("No available loopback port for web control");
  origin="http://127.0.0.1:"+std::to_string(port);
  // This convenience file contains no token and lets no-browser users find the URL.
  {std::ofstream endpoint(base/L"ppp-web-address.txt",std::ios::trunc);if(endpoint)endpoint<<origin<<"/\n";}
  for(const char* asset:{"index.html","style.css","app.js","favicon.ico"})embeddedAsset(asset);
  TrayHost tray;
  // Multiple accept workers must never block after another worker consumes a connection.
  u_long nonblocking=1;if(ioctlsocket(listener,FIONBIO,&nonblocking)!=0)throw std::runtime_error("Cannot configure web listener");
  if(open)ShellExecuteW(nullptr,L"open",wide(origin).c_str(),nullptr,nullptr,SW_SHOWNORMAL);
  // Bounded workers keep status responsive and slow clients cannot create unlimited threads.
  std::vector<std::thread> workers;for(int i=0;i<4;++i)workers.emplace_back([listener]{while(!exiting){fd_set set;FD_ZERO(&set);FD_SET(listener,&set);timeval wait={0,250000};if(select(0,&set,nullptr,nullptr,&wait)>0){SOCKET s=accept(listener,nullptr,nullptr);if(s!=INVALID_SOCKET){u_long blocking=0;if(ioctlsocket(s,FIONBIO,&blocking)==0)serve(s);else closesocket(s);}}}});
  workers.emplace_back([]{while(!exiting){Sleep(250);std::lock_guard<std::mutex> lock(stateMutex);if(exiting)break;if(core&&!ppp_core_is_running(core)){auto reason=ppp_core_get_exit_reason(core);ppp_core_destroy(core);core=nullptr;if(reason==PPP_CORE_EXIT_RESTART_REQUESTED){try{startCore();}catch(const std::exception& e){lastError=e.what();}}else if(reason==PPP_CORE_EXIT_FAILED)lastError="Core exited unexpectedly; check the core log";}}});
  tray.run();
  closesocket(listener);for(auto& thread:workers)thread.join();stopCore();WSACleanup();return 0;
 }catch(const std::exception& e){MessageBoxW(nullptr,wide(e.what()).c_str(),L"PPP Web",MB_OK|MB_ICONERROR);return 1;}
}
